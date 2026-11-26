use std::{
    collections::{HashMap, HashSet},
    ffi::CString,
    ptr::{self, NonNull},
    time::{SystemTime, UNIX_EPOCH},
};

use inventory_mod::{
    inventory::{Inventory, WeaponState},
    items::{AmmoKind, Catalog, GameSlot, ItemDef, ItemId},
    remote_state::{NATIVE_SLOTS, NativeWeapon},
    rules::MapGunRules,
};

use crate::source::{self, Edict, Entity, Source};
use native::*;

mod actions;
mod ammo;
mod combat;
mod infection;
mod native;
mod pickups;
mod pings;
mod saved_frame;
mod stamina;
mod vtable;

pub use actions::{ActionContext, Actions};
pub use combat::AttackContext;
pub use infection::Exposure;
pub use pickups::{PickupCall, PickupContext};
pub use pings::{Marker, PingContext, Pings};
pub use stamina::DamageInput;

const ENTITY_HANDLE: usize = 0x3b4;

type Ent = NonNull<Entity>;
type DropWeapon = unsafe extern "C" fn(*mut Entity, *mut Entity, *const Vector, *const Vector);
pub type Reserve = [i32; 32];

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct Vector {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

impl Vector {
    fn ahead(self, forward: Self, distance: f32) -> Self {
        Self {
            x: self.x + distance * forward.x,
            y: self.y + distance * forward.y,
            z: self.z + distance * forward.z - 14.0,
        }
    }

    pub fn dot(self, other: Self) -> f32 {
        self.x * other.x + self.y * other.y + self.z * other.z
    }

    fn offset(self, x: f32, y: f32, z: f32) -> Self {
        Self {
            x: self.x + x,
            y: self.y + y,
            z: self.z + z,
        }
    }
}

#[derive(Default)]
struct Condition {
    spent: i32,
    wear: i32,
}

pub struct Game {
    native: ServerFunctions,
    ammo: ammo::Ammo,
    pickups: pickups::Pickups,
    stamina: stamina::Stamina,
    combat: combat::Combat,
    infection: infection::Infection,
    chainsaw_patch: Option<vtable::VtablePatch>,
    condition: HashMap<u32, Condition>,
    seen: HashSet<u32>,
    random: u32,
}

fn slot_index(slot: GameSlot) -> i32 {
    match slot {
        GameSlot::Primary => 0,
        GameSlot::Secondary => 1,
        GameSlot::Throwable => 2,
        GameSlot::Medical => 3,
        GameSlot::Pills => 4,
        GameSlot::Carry => 5,
    }
}

fn firearm(def: &ItemDef) -> bool {
    def.is_weapon() && !matches!(def.classname, "weapon_melee" | "weapon_chainsaw")
}

fn melee(def: &ItemDef) -> bool {
    def.classname == "weapon_melee"
}

/// x86 entity field offsets come from the matched server image.
/// The caller must ensure the entity's type has the field at this offset.
unsafe fn field<T: Copy>(entity: Ent, offset: usize) -> T {
    unsafe {
        entity
            .cast::<u8>()
            .as_ptr()
            .add(offset)
            .cast::<T>()
            .read_unaligned()
    }
}

/// x86 entity field offsets come from the matched server image.
unsafe fn set_field<T>(entity: Ent, offset: usize, value: T) {
    unsafe {
        entity
            .cast::<u8>()
            .as_ptr()
            .add(offset)
            .cast::<T>()
            .write_unaligned(value)
    }
}

impl Game {
    pub fn bind() -> Option<Self> {
        let time = SystemTime::now().duration_since(UNIX_EPOCH).ok()?;
        Some(Self {
            native: ServerFunctions::bind()?,
            ammo: ammo::Ammo::default(),
            pickups: pickups::Pickups::default(),
            stamina: stamina::Stamina::default(),
            combat: combat::Combat::default(),
            infection: infection::Infection::default(),
            chainsaw_patch: None,
            condition: HashMap::new(),
            seen: HashSet::new(),
            random: (time.as_nanos() as u32).max(1),
        })
    }

    pub fn round_seed(&mut self) -> u32 {
        self.random ^= self.random << 13;
        self.random ^= self.random >> 17;
        self.random ^= self.random << 5;
        self.random
    }

    pub fn reset(&mut self) {
        self.condition.clear();
        self.seen.clear();
    }

    fn classname(&self, entity: Ent) -> &str {
        unsafe { source::text((self.native.classname)(entity.as_ptr())) }.unwrap_or("")
    }

    fn melee_script(&self, entity: Ent) -> &str {
        unsafe { source::text((self.native.melee_script_name)(entity.as_ptr())) }.unwrap_or("")
    }

    fn weapon_at(&self, player: Ent, slot: i32) -> Option<Ent> {
        NonNull::new(unsafe { (self.native.weapon_in_slot)(player.as_ptr(), slot) })
    }

    fn position(&self, entity: Ent) -> Option<Vector> {
        let origin = unsafe { (self.native.origin)(entity.as_ptr()) };
        (!origin.is_null()).then(|| unsafe { origin.read_unaligned() })
    }

    pub fn death_position(&self, source: &Source, edict: NonNull<Edict>) -> Option<(u32, Vector)> {
        let actor = source.entity(edict)?;
        Some((self.handle(actor), self.position(actor)?))
    }

    fn eyes(&self, player: Ent) -> (Vector, Vector) {
        let mut eye = Vector::default();
        let mut forward = Vector::default();
        unsafe {
            (self.native.eye_vectors)(
                player.as_ptr(),
                &mut eye,
                &mut forward,
                ptr::null_mut(),
                ptr::null_mut(),
            )
        };
        (eye, forward)
    }

    pub fn handle(&self, entity: Ent) -> u32 {
        unsafe { field(entity, ENTITY_HANDLE) }
    }

    fn find_entity(&self, handle: u32) -> Option<Ent> {
        if handle == u32::MAX {
            return None;
        }
        // SAFETY: The matched build stores a pointer to the entity list here.
        // Its entries contain an entity and its serial in the handle.
        unsafe {
            let list =
                std::ptr::with_exposed_provenance::<*const u8>(self.native.base + ENTITY_LIST)
                    .read();
            if list.is_null() {
                return None;
            }
            let raw = list.add(4 + 16 * (handle & 0xfff) as usize);
            let serial = raw.add(4).cast::<u32>().read();
            if serial != handle >> 12 {
                return None;
            }
            let entity = NonNull::new(raw.cast::<*mut Entity>().read())?;
            (!self.pending_removal(entity)).then_some(entity)
        }
    }

    fn pending_removal(&self, entity: Ent) -> bool {
        unsafe {
            let property = (self.native.network_property)(entity.as_ptr());
            property.is_null() || (self.native.marked_for_deletion)(property)
        }
    }

    fn can_handle(&self, player: Ent) -> bool {
        unsafe {
            !(self.native.incapacitated)(player.as_ptr())
                && !(self.native.dominated)(player.as_ptr())
        }
    }

    fn remove(&mut self, entity: Ent) {
        self.condition.remove(&self.handle(entity));
        unsafe { (self.native.remove)(entity.as_ptr()) };
    }

    fn spent(&self, entity: Ent) -> i32 {
        self.condition
            .get(&self.handle(entity))
            .map_or(0, |state| state.spent)
    }

    fn wear(&self, entity: Ent) -> i32 {
        self.condition
            .get(&self.handle(entity))
            .map_or(0, |state| state.wear)
    }

    fn set_spent(&mut self, entity: Ent, spent: i32) {
        let handle = self.handle(entity);
        if spent > 0 || self.condition.contains_key(&handle) {
            self.condition.entry(handle).or_default().spent = spent;
        }
    }

    pub fn carried_condition(
        &self,
        player: Ent,
    ) -> Vec<inventory_mod::remote_state::CarriedCondition> {
        (0..NATIVE_SLOTS)
            .filter_map(|slot| {
                let weapon = self.weapon_at(player, slot as i32)?;
                let state = self.condition.get(&self.handle(weapon))?;
                if state.wear == 0 && state.spent == 0 {
                    return None;
                }
                let classname = self.classname(weapon);
                Some(inventory_mod::remote_state::CarriedCondition {
                    slot,
                    classname: classname.into(),
                    melee: if classname == "weapon_melee" {
                        self.melee_script(weapon).into()
                    } else {
                        String::new()
                    },
                    wear: state.wear,
                    spent: state.spent,
                })
            })
            .collect()
    }

    pub fn restore_condition(
        &mut self,
        player: Ent,
        saved: &[inventory_mod::remote_state::CarriedCondition],
    ) {
        for item in saved {
            let Some(weapon) = self.weapon_at(player, item.slot as i32) else {
                continue;
            };
            if self.classname(weapon) == item.classname
                && (item.classname != "weapon_melee" || self.melee_script(weapon) == item.melee)
            {
                let handle = self.handle(weapon);
                let state = self.condition.entry(handle).or_default();
                state.wear = item.wear;
                state.spent = item.spent;
            }
        }
    }

    pub fn native_weapons(&self, player: Ent) -> [NativeWeapon; NATIVE_SLOTS] {
        let active = unsafe { (self.native.active_weapon)(player.as_ptr()) };
        std::array::from_fn(|slot| {
            let Some(weapon) = self.weapon_at(player, slot as i32) else {
                return NativeWeapon::default();
            };
            let classname = self.classname(weapon);
            let melee = classname == "weapon_melee";
            NativeWeapon {
                classname: classname.to_owned(),
                active: weapon.as_ptr() == active,
                melee: if melee {
                    self.melee_script(weapon).to_owned()
                } else {
                    String::new()
                },
                wear: melee.then(|| self.wear(weapon)),
            }
        })
    }

    pub fn reserve(&self, player: Ent) -> Reserve {
        unsafe { field(player, PLAYER_AMMO) }
    }

    pub fn restore_reserve(&self, player: Ent, expected: &Reserve) {
        for (index, (actual, expected)) in
            self.reserve(player).into_iter().zip(expected).enumerate()
        {
            if actual != *expected {
                unsafe { (self.native.set_ammo_count)(player.as_ptr(), *expected, index as i32) };
            }
        }
    }

    pub fn normalize_ammo(&mut self, player: Ent, catalog: &Catalog) {
        let mut pool = self.reserve(player);
        let shotgun = AmmoKind::Shotgun.definition().index;
        if pool[8] > 0 {
            let incoming =
                pool[8].min((catalog.ammo_max(AmmoKind::Shotgun) - pool[shotgun]).max(0));
            pool[shotgun] += incoming;
            pool[8] = 0;
        }
        for kind in AmmoKind::ALL {
            let index = kind.definition().index;
            if let Some(cap) = catalog.ammo_max_override(kind) {
                pool[index] = pool[index].min(cap);
            }
        }
        self.restore_reserve(player, &pool);
        for index in [0, 1] {
            if let Some(weapon) = self.weapon_at(player, index) {
                self.seen.insert(self.handle(weapon));
                unsafe {
                    let original: i32 = field(weapon, PRIMARY_AMMO_TYPE);
                    let shared = match original {
                        1 => 5,
                        2 => 10,
                        8 => shotgun as i32,
                        _ => original,
                    };
                    if shared != original {
                        set_field(weapon, PRIMARY_AMMO_TYPE, shared);
                        (self.native.network_changed)(weapon.as_ptr());
                    }
                }
            }
        }
    }

    fn add_rounds(&self, player: Ent, weapon: Ent, def: &ItemDef, rounds: i32, catalog: &Catalog) {
        if rounds <= 0 {
            return;
        }
        let index = unsafe { field::<i32>(weapon, PRIMARY_AMMO_TYPE) };
        let index = if index == 8 { 7 } else { index };
        let Some(count) = self.reserve(player).get(index as usize).copied() else {
            return;
        };
        let cap = def
            .ammo
            .map(|kind| catalog.ammo_max(kind))
            .unwrap_or(i32::MAX);
        let added = rounds.min(cap.saturating_sub(count).max(0));
        if added > 0 {
            unsafe { (self.native.set_ammo_count)(player.as_ptr(), count + added, index) };
        }
    }

    fn weapon_state(&self, weapon: Ent, def: &ItemDef, carried: bool) -> WeaponState {
        let mut state = WeaponState {
            clip: unsafe { field(weapon, CLIP) },
            ..WeaponState::default()
        };
        if melee(def) {
            state.melee = self.melee_script(weapon).to_owned();
            state.wear = self.wear(weapon);
        }
        if firearm(def) {
            unsafe {
                state.upgrades = field::<i32>(weapon, UPGRADES) & 7;
                state.special = i32::from(field::<u8>(weapon, SPECIAL_ROUNDS));
                if !carried {
                    state.ammo = field(weapon, EXTRA_PRIMARY_AMMO);
                }
            }
        }
        state
    }

    fn apply_state(&mut self, weapon: Ent, def: &ItemDef, state: &WeaponState, on_ground: bool) {
        let handle = self.handle(weapon);
        if melee(def) {
            self.condition.entry(handle).or_default().wear = state.wear;
        }
        unsafe {
            if state.clip >= 0 && !melee(def) {
                set_field(weapon, CLIP, state.clip);
            }
            if firearm(def) {
                set_field(weapon, UPGRADES, state.upgrades & 7);
                set_field(weapon, SPECIAL_ROUNDS, state.special.clamp(0, 255) as u8);
                if on_ground && state.ammo >= 0 {
                    set_field(weapon, EXTRA_PRIMARY_AMMO, state.ammo);
                }
            }
            (self.native.network_changed)(weapon.as_ptr());
        }
        self.seen.insert(handle);
    }

    fn select_item(&self, player: Ent, classname: &str) {
        if let Ok(name) = CString::new(classname) {
            unsafe { (self.native.select_item)(player.as_ptr(), name.as_ptr(), 0) };
        }
    }

    fn put_away_carried(&mut self, player: Ent, selecting: &str, inventory: &mut Inventory) {
        if let Some(weapon) = self.weapon_at(player, slot_index(GameSlot::Carry))
            && let Some(id) = inventory.catalog().find_item(self.classname(weapon))
            && inventory.catalog().item(id).classname != selecting
        {
            self.store_item(player, weapon, id, inventory);
        }
    }

    pub fn select(&mut self, player: Ent, classname: &str, inventory: &mut Inventory) {
        if self.can_handle(player)
            && (0..NATIVE_SLOTS).any(|slot| {
                self.weapon_at(player, slot as i32)
                    .is_some_and(|weapon| self.classname(weapon) == classname)
            })
        {
            self.put_away_carried(player, classname, inventory);
            self.select_item(player, classname);
        }
    }

    fn release(&self, player: Ent, weapon: Ent, def: &ItemDef, drop_weapon: DropWeapon) -> bool {
        let pool = self.reserve(player);
        unsafe { drop_weapon(player.as_ptr(), weapon.as_ptr(), ptr::null(), ptr::null()) };
        self.restore_reserve(player, &pool);
        if self.weapon_at(player, slot_index(def.game_slot)) == Some(weapon) {
            return false;
        }
        if firearm(def) && !self.pending_removal(weapon) {
            unsafe {
                set_field(weapon, EXTRA_PRIMARY_AMMO, 0i32);
                (self.native.network_changed)(weapon.as_ptr());
            }
        }
        true
    }

    pub fn drop_weapon(&self, player: Ent, classname: &str, catalog: &Catalog) -> String {
        if !self.can_handle(player) {
            return "The game does not let you drop now".into();
        }
        let Some((def, weapon)) = catalog.find_item(classname).and_then(|id| {
            let def = catalog.item(id);
            self.weapon_at(player, slot_index(def.game_slot))
                .map(|weapon| (def, weapon))
        }) else {
            return String::new();
        };
        if self.classname(weapon) != classname {
            return String::new();
        }
        if self.release(player, weapon, def, self.native.weapon_drop) {
            format!("Dropped {}", def.name)
        } else {
            "The game does not let you drop now".into()
        }
    }

    fn store_item(
        &mut self,
        player: Ent,
        weapon: Ent,
        id: ItemId,
        inventory: &mut Inventory,
    ) -> bool {
        if self.pending_removal(weapon) {
            return false;
        }
        let def = inventory.catalog().item(id);
        let mut next = inventory.clone();
        if def.is_weapon() {
            next.set_weapons_weight((next.weapons_weight() - def.weight).max(0.0));
            if next
                .add_weapon(id, self.weapon_state(weapon, def, true), None)
                .is_none()
            {
                return false;
            }
        } else if let Some(home) = next.held_slot(id) {
            next.release_held(home, self.spent(weapon));
        } else if next.add(id, 1, self.spent(weapon)) != 1 {
            return false;
        }
        if !self.release(player, weapon, def, self.native.quiet_drop) {
            return false;
        }
        self.remove(weapon);
        *inventory = next;
        true
    }

    pub fn store(&mut self, player: Ent, inventory: &mut Inventory) -> String {
        if !self.can_handle(player) {
            return "The game does not let you put that away now".into();
        }
        let active = unsafe { (self.native.active_weapon)(player.as_ptr()) };
        let Some((weapon, id)) = NonNull::new(active).and_then(|weapon| {
            inventory
                .catalog()
                .find_item(self.classname(weapon))
                .map(|id| (weapon, id))
        }) else {
            return "Nothing to store".into();
        };
        let name = inventory.catalog().item(id).name;
        if self.store_item(player, weapon, id, inventory) {
            format!("Stored {name}")
        } else {
            format!("Could not store {name}")
        }
    }

    fn give(&self, player: Ent, def: &ItemDef, give_name: &str) -> Option<Ent> {
        let name = CString::new(give_name).ok()?;
        unsafe {
            (self.native.give_item)(player.as_ptr(), name.as_ptr(), 0, false, ptr::null_mut())
        };
        self.weapon_at(player, slot_index(def.game_slot))
            .filter(|&weapon| self.classname(weapon) == def.classname)
    }

    fn take_weapon(
        &mut self,
        player: Ent,
        inventory: &mut Inventory,
        index: usize,
        id: ItemId,
    ) -> String {
        let catalog = inventory.catalog();
        let def = catalog.item(id);
        let state = inventory.slots()[index].weapon.clone();
        let carried = self.weapon_at(player, slot_index(def.game_slot));
        let mut next = inventory.clone();
        next.remove_from_slot(index, 1);
        if let Some(carried) = carried {
            let Some(old_id) = catalog.find_item(self.classname(carried)) else {
                return "The game does not let you swap this weapon".into();
            };
            let old = catalog.item(old_id);
            next.set_weapons_weight((next.weapons_weight() - old.weight).max(0.0));
            if next
                .add_weapon(old_id, self.weapon_state(carried, old, true), Some(index))
                .is_none()
            {
                return format!("No room for your {}", old.name);
            }
            if !self.release(player, carried, old, self.native.quiet_drop) {
                return "The game does not let you put that away now".into();
            }
        }

        let pool = self.reserve(player);
        let give_name = state.give_name(def.classname);
        let given = self.give(player, def, give_name);
        self.restore_reserve(player, &pool);
        let Some(given) = given else {
            if let Some(carried) = carried.filter(|&weapon| !self.pending_removal(weapon)) {
                unsafe { (self.native.equip)(player.as_ptr(), carried.as_ptr()) };
                self.restore_reserve(player, &pool);
            }
            return format!("The game refused the {}", def.name);
        };
        if let Some(carried) = carried.filter(|&weapon| weapon != given) {
            self.remove(carried);
        }
        self.apply_state(given, def, &state, false);
        self.add_rounds(player, given, def, state.ammo, catalog);
        self.select_item(player, def.classname);
        *inventory = next;
        String::new()
    }

    pub fn take(&mut self, player: Ent, inventory: &mut Inventory, index: usize) -> String {
        if !self.can_handle(player) {
            return "The game does not let you take out items now".into();
        }
        let Some(id) = inventory.slot(index).and_then(|stack| stack.item) else {
            return String::new();
        };
        let def = inventory.catalog().item(id).clone();
        self.put_away_carried(player, def.classname, inventory);
        if inventory.slots()[index].item != Some(id) {
            return String::new();
        }
        if def.is_weapon() {
            return self.take_weapon(player, inventory, index, id);
        }
        if let Some(weapon) = self.weapon_at(player, slot_index(def.game_slot)) {
            if self.classname(weapon) == def.classname {
                self.select_item(player, def.classname);
                return String::new();
            }
            if !inventory
                .catalog()
                .find_item(self.classname(weapon))
                .is_some_and(|id| self.store_item(player, weapon, id, inventory))
            {
                return format!("Your hands are full: no room for {}", def.name);
            }
        }
        if inventory.slots()[index].count <= 0 {
            return String::new();
        }
        let spent = inventory.next_spent(index);
        let Some(given) = self.give(player, &def, def.classname) else {
            return format!("The game refused the {}", def.name);
        };
        self.set_spent(given, spent);
        inventory.remove_from_slot(index, 1);
        inventory.set_held(index, id);
        self.select_item(player, def.classname);
        String::new()
    }

    pub fn pickup(
        &mut self,
        source: &Source,
        player: Ent,
        inventory: &mut Inventory,
        map_guns: &MapGunRules,
    ) -> String {
        if !self.can_handle(player) {
            return "The game does not let you pick up items now".into();
        }
        let (eye, forward) = self.eyes(player);
        let catalog = inventory.catalog();
        let closest = source
            .entities()
            .filter_map(|entity| {
                let id = catalog.find_item(self.classname(entity))?;
                let has_owner = !unsafe { (self.native.owner)(entity.as_ptr()) }.is_null();
                if has_owner || self.pending_removal(entity) {
                    return None;
                }
                let target = self.position(entity)?;
                let relative = target.offset(-eye.x, -eye.y, -eye.z);
                let distance = relative.dot(relative).sqrt();
                if !(1.0..=160.0).contains(&distance) {
                    return None;
                }
                let facing = relative.dot(forward) / distance;
                if facing < 0.6 {
                    return None;
                }
                let hit = source.trace(eye, relative, source.as_edict(player), 0x4600_4003)?;
                (!hit.start_solid && (hit.fraction >= 1.0 || hit.entity == Some(entity)))
                    .then_some((distance * (2.0 - facing), entity, id))
            })
            .min_by(|a, b| a.0.total_cmp(&b.0));
        let Some((_, entity, id)) = closest else {
            return "Nothing to pick up in front of you".into();
        };
        let def = catalog.item(id).clone();
        self.shorten_map_gun(entity, &def, map_guns);
        if !self.store_world_item(player, entity, id, inventory) {
            if inventory.has_box_for(id) {
                return "Too heavy".into();
            }
            return if def.is_weapon() {
                "No free weapon box"
            } else {
                "The inventory is full"
            }
            .into();
        }
        self.remove(entity);
        format!("Picked up {}", def.name)
    }

    fn store_world_item(
        &self,
        player: Ent,
        entity: Ent,
        id: ItemId,
        inventory: &mut Inventory,
    ) -> bool {
        let def = inventory.catalog().item(id).clone();
        if !def.is_weapon() {
            return inventory.add(id, 1, self.spent(entity)) == 1;
        }
        let mut state = self.weapon_state(entity, &def, false);
        let spare = state.ammo;
        state.ammo = -1;
        if inventory.add_weapon(id, state, None).is_none() {
            return false;
        }
        if let Some(kind) = def.ammo.filter(|_| spare > 0) {
            let room = (inventory.limits().max_weight > 0.0)
                .then(|| (inventory.limits().max_weight - inventory.weight()).max(0.0));
            let before = self.reserve(player)[kind.definition().index];
            let split = inventory
                .catalog()
                .split_unload(Some(kind), spare, before, room);
            self.add_rounds(player, entity, &def, split.to_reserve, inventory.catalog());
        }
        true
    }

    fn shorten_map_gun(&mut self, weapon: Ent, def: &ItemDef, rules: &MapGunRules) -> bool {
        if !rules.enabled
            || !firearm(def)
            || def.ammo.is_none()
            || !self.seen.insert(self.handle(weapon))
        {
            return false;
        }
        let clip: i32 = unsafe { field(weapon, CLIP) };
        let percent = rules.min_clip
            + (self.round_seed() % (rules.max_clip - rules.min_clip + 1) as u32) as i32;
        unsafe {
            if clip > 0 {
                let clipped = (f64::from(clip) * f64::from(percent) / 100.0).round() as i32;
                set_field(weapon, CLIP, clipped);
            }
            set_field(weapon, EXTRA_PRIMARY_AMMO, 0i32);
            (self.native.network_changed)(weapon.as_ptr());
        }
        true
    }

    fn spawn_at(
        &mut self,
        source: &Source,
        def: &ItemDef,
        state: &WeaponState,
        position: Vector,
    ) -> Option<Ent> {
        let classname = CString::new(def.classname).ok()?;
        let entity = source.create_entity(&classname)?;
        unsafe { (self.native.set_origin)(entity.as_ptr(), &position) };
        if melee(def)
            && let Ok(melee_name) = CString::new(state.give_name(def.classname))
        {
            source.set_key(entity, c"melee_script_name", &melee_name);
        }
        if unsafe { (self.native.dispatch_spawn)(entity.as_ptr(), true) } < 0 {
            return None;
        }
        if def.is_weapon() {
            self.apply_state(entity, def, state, true);
        }
        Some(entity)
    }

    pub fn spawn_item(
        &mut self,
        source: &Source,
        player: Ent,
        catalog: &Catalog,
        classname: &str,
    ) -> String {
        let Some(def) = catalog.find_item(classname).map(|id| catalog.item(id)) else {
            return String::new();
        };
        if !self.can_handle(player) {
            return String::new();
        }
        let (eye, forward) = self.eyes(player);
        let verb = if self
            .spawn_at(
                source,
                def,
                &WeaponState::default(),
                eye.ahead(forward, 56.0),
            )
            .is_some()
        {
            "Spawned"
        } else {
            "Could not spawn"
        };
        format!("{verb} {}", def.name)
    }

    pub fn drop_items(
        &mut self,
        source: &Source,
        player: Ent,
        inventory: &mut Inventory,
        index: usize,
        count: i32,
    ) -> String {
        if !self.can_handle(player) {
            return "The game does not let you drop items now".into();
        }
        let Some(stack) = inventory.slot(index).filter(|stack| !stack.is_empty()) else {
            return String::new();
        };
        let def = inventory.catalog().item(stack.item.unwrap()).clone();
        if stack.count == 0 {
            return self.drop_weapon(player, def.classname, inventory.catalog());
        }
        let state = stack.weapon.clone();
        let (eye, forward) = self.eyes(player);
        let mut dropped = 0;
        for i in 0..stack.count.min(count.max(1)) {
            let Some(entity) = self.spawn_at(
                source,
                &def,
                &state,
                eye.ahead(forward, 48.0 + 6.0 * i as f32),
            ) else {
                break;
            };
            self.set_spent(entity, inventory.next_spent(index));
            inventory.remove_from_slot(index, 1);
            dropped += 1;
        }
        if dropped == 0 {
            format!("Could not drop {}", def.name)
        } else {
            format!("Dropped {} x{dropped}", def.name)
        }
    }

    pub fn drop_on_death(
        &mut self,
        source: &Source,
        position: Vector,
        inventory: &mut Inventory,
    ) -> i32 {
        let mut count = 0;
        for index in 0..inventory.slots().len() {
            let stack = inventory.slots()[index].clone();
            let Some(id) = stack.item else {
                continue;
            };
            for _ in 0..stack.count {
                let angle = count as f32 * 2.39996;
                let radius = (4.0 + count as f32 * 3.0).min(18.0);
                let Some(entity) = self.spawn_at(
                    source,
                    inventory.catalog().item(id),
                    &stack.weapon,
                    position.offset(angle.cos() * radius, angle.sin() * radius, 16.0),
                ) else {
                    break;
                };
                self.set_spent(entity, inventory.next_spent(index));
                inventory.remove_from_slot(index, 1);
                count += 1;
            }
            inventory.clear_held(index);
        }
        count
    }

    pub fn trigger_panic(&self) -> bool {
        let ptr =
            std::ptr::with_exposed_provenance::<*mut std::ffi::c_void>(self.native.base + DIRECTOR);
        // SAFETY: The guarded build contains the director pointer at this address.
        let director = unsafe { ptr.read() };
        if director.is_null() {
            return false;
        }
        unsafe { (self.native.panic_event)(director, ptr::null_mut(), 0) };
        true
    }
}
