use std::{
    cell::{Cell, RefCell},
    collections::HashMap,
    ffi::CString,
    ptr::{self, NonNull},
};

use inventory_mod::{
    inventory::{Inventory, WeaponState},
    items::{AmmoKind, Catalog, ItemDef},
    melee::{BUTTON_ATTACK, BUTTON_RELOAD},
    remote_state::reconcile_native_inventory,
    rules::{AmmoPileRules, MapGunRules},
};

use super::{Ent, Game, Vector, field, native::*, set_field, vtable::VtablePatch};
use crate::{
    plugin,
    source::{self, Entity, Source},
};

type Use = unsafe extern "C" fn(*mut Entity, *mut Entity, *mut Entity, i32, f32);
type Reload = unsafe extern "C" fn(*mut Entity) -> bool;
type FireEmpty = unsafe extern "C" fn(*mut Entity);

const PILE_USE_RVA: usize = 0xce8c14;
const RELOAD_SLOT: usize = 282;
const FIRE_EMPTY_SLOT: usize = 277;

thread_local! {
    static ORIGINAL_PILE: Cell<Option<Use>> = const { Cell::new(None) };
    static RELOAD_TABLES: RefCell<HashMap<usize, (Reload, FireEmpty)>> = RefCell::new(HashMap::new());
}

#[derive(Default)]
pub struct Ammo {
    piles: HashMap<u32, Pile>,
    unloaded: HashMap<u32, u32>,
    pile_patch: Option<VtablePatch>,
    reload_patches: HashMap<usize, [VtablePatch; 2]>,
}

#[derive(Clone, Copy)]
struct Pile {
    kind: AmmoKind,
    rounds: i32,
    map: bool,
    spawned: bool,
}

impl Game {
    pub fn install_ammo(&mut self) -> bool {
        let base = self.native.base;
        let original = self.native.pile_use;
        self.ammo.pile_patch = unsafe {
            VtablePatch::install(
                base,
                base + PILE_USE_RVA,
                original as usize,
                pile_use as Use as usize,
            )
        };
        if self.ammo.pile_patch.is_none() {
            return false;
        }
        ORIGINAL_PILE.set(Some(original));
        true
    }

    pub fn uninstall_ammo(&mut self, source: &Source) {
        self.clear_piles(source, true);
        self.ammo.reload_patches.clear();
        self.ammo.pile_patch = None;
        if !super::vtable::retained() {
            ORIGINAL_PILE.set(None);
            RELOAD_TABLES.with_borrow_mut(HashMap::clear);
        }
    }

    pub fn clear_piles(&mut self, source: &Source, remove_spawned: bool) {
        if remove_spawned && !self.ammo.piles.is_empty() {
            let spawned = source
                .entities()
                .filter(|&entity| {
                    self.classname(entity) == "weapon_ammo_spawn"
                        && self
                            .ammo
                            .piles
                            .get(&self.handle(entity))
                            .is_some_and(|pile| pile.spawned)
                        && !self.pending_removal(entity)
                })
                .collect::<Vec<_>>();
            for entity in spawned {
                self.remove(entity);
            }
        }
        self.ammo.piles.clear();
        self.ammo.unloaded.clear();
    }

    fn roll(&mut self, rules: &AmmoPileRules, spawned: bool) -> Option<Pile> {
        let size = rules.kinds.len();
        if size == 0 {
            return None;
        }
        let kind = rules.kinds[self.round_seed() as usize % size];
        let count = rules.min_magazines
            + (self.round_seed() % (rules.max_magazines - rules.min_magazines + 1) as u32) as i32;
        Some(Pile {
            kind,
            rounds: count * kind.definition().magazine,
            map: true,
            spawned,
        })
    }

    fn tint(source: &Source, entity: Ent, kind: AmmoKind) {
        let color = kind.definition().color;
        let tint = CString::new(format!(
            "{} {} {} 255",
            (color >> 16) & 255,
            (color >> 8) & 255,
            color & 255
        ))
        .unwrap();
        source.set_key(entity, c"rendercolor", &tint);
    }

    fn spawn_pile(&mut self, source: &Source, position: Vector, mut pile: Pile) -> bool {
        if self.ammo.pile_patch.is_none() || pile.rounds <= 0 {
            return false;
        }
        let mut nearby = Vec::new();
        for entity in source.entities() {
            if self.classname(entity) != "weapon_ammo_spawn" || self.pending_removal(entity) {
                continue;
            }
            let Some(other) = self.position(entity) else {
                continue;
            };
            let distance = (other.x - position.x).powi(2) + (other.y - position.y).powi(2);
            if (other.z - position.z).abs() > 72.0 || distance > 256.0 * 256.0 {
                continue;
            }
            let handle = self.handle(entity);
            if !pile.map
                && distance < 40.0 * 40.0
                && let Some(existing) = self.ammo.piles.get_mut(&handle)
                && !existing.map
                && existing.kind == pile.kind
                && let Some(combined) = existing.rounds.checked_add(pile.rounds)
            {
                existing.rounds = combined;
                return true;
            }
            nearby.push(other);
        }
        let at = (0..=8).find_map(|step| {
            let sign = if step % 2 == 0 { -1.0 } else { 1.0 };
            let at = position.offset(26.0 * ((step + 1) / 2) as f32 * sign, 0.0, 0.0);
            nearby
                .iter()
                .all(|other| (other.x - at.x).powi(2) + (other.y - at.y).powi(2) >= 24.0 * 24.0)
                .then_some(at)
        });
        let Some(mut at) = at else {
            return false;
        };

        // The pack's physics becomes static on spawn. Find its floor with a temporary entity.
        if let Some(probe) = source.create_entity(c"info_target") {
            unsafe { (self.native.set_origin)(probe.as_ptr(), &at) };
            if unsafe { (self.native.dispatch_spawn)(probe.as_ptr(), false) } >= 0 {
                if unsafe {
                    (self.native.drop_to_floor)(probe.as_ptr(), 0x200400b, ptr::null_mut())
                } == 1
                {
                    at = self.position(probe).unwrap_or(at);
                }
                self.remove(probe);
            }
        }

        let model = c"models/w_models/weapons/w_eq_incendiary_ammopack.mdl";
        if !source.precache_model(model) {
            return false;
        }
        let Some(entity) = source.create_entity(c"weapon_ammo_spawn") else {
            return false;
        };
        source.set_key(entity, c"model", model);
        Self::tint(source, entity, pile.kind);
        source.set_key(entity, c"solid", c"6");
        source.set_key(entity, c"spawnflags", c"2");
        source.set_key(entity, c"disableshadows", c"1");
        at.z += 0.3;
        unsafe { (self.native.set_origin)(entity.as_ptr(), &at) };
        if unsafe { (self.native.dispatch_spawn)(entity.as_ptr(), true) } < 0 {
            return false;
        }
        pile.spawned = true;
        self.ammo.piles.insert(self.handle(entity), pile);
        true
    }

    pub fn place_map_piles(&mut self, source: &Source, rules: &AmmoPileRules) {
        if self.ammo.pile_patch.is_none() || !rules.enabled {
            return;
        }
        let mut players = Vec::new();
        let mut item_spots = Vec::new();
        let mut ammo_spots = Vec::new();
        let mut placed = Vec::new();
        let mut registered = 0;
        for entity in source.entities() {
            let Some(at) = self.position(entity) else {
                continue;
            };
            match self.classname(entity) {
                "weapon_ammo_spawn" => {
                    ammo_spots.push(at);
                    let handle = self.handle(entity);
                    if rules.replace_map_piles
                        && !self.ammo.piles.contains_key(&handle)
                        && let Some(pile) = self.roll(rules, false)
                    {
                        Self::tint(source, entity, pile.kind);
                        self.ammo.piles.insert(handle, pile);
                        registered += 1;
                    }
                }
                "player" => {
                    if source
                        .as_edict(entity)
                        .and_then(|edict| source.player_info(edict))
                        .is_some_and(|info| info.team == 2 && !info.dead)
                    {
                        players.push(at);
                    }
                }
                classname if classname.starts_with("weapon_") && classname.ends_with("_spawn") => {
                    item_spots.push(at)
                }
                _ => {}
            }
        }
        for index in (1..item_spots.len()).rev() {
            item_spots.swap(index, self.round_seed() as usize % (index + 1));
        }
        for item in item_spots {
            if placed.len() >= rules.extra as usize {
                break;
            }
            let angle = self.round_seed() as f32 / u32::MAX as f32 * std::f32::consts::TAU;
            let at = item.offset(20.0 * angle.cos(), 20.0 * angle.sin(), 8.0);
            let near = |points: &[Vector], radius: f32| {
                points.iter().any(|point| {
                    let delta = point.offset(-at.x, -at.y, -at.z);
                    delta.dot(delta) < radius * radius
                })
            };
            if near(&players, 700.0) || near(&placed, 300.0) || near(&ammo_spots, 150.0) {
                continue;
            }
            if let Some(pile) = self.roll(rules, true)
                && self.spawn_pile(source, at, pile)
            {
                placed.push(at);
            }
        }
        eprintln!(
            "inventory_mod: {registered} map ammo piles made finite, {} extra packs placed",
            placed.len()
        );
    }

    pub fn use_pile(
        &mut self,
        source: &Source,
        pile: Ent,
        player: Ent,
        rules: &AmmoPileRules,
        catalog: &Catalog,
        inventory: Option<&mut Inventory>,
    ) -> bool {
        if self.pending_removal(pile) {
            return true;
        }
        let handle = self.handle(pile);
        if !self.ammo.piles.contains_key(&handle) && (!rules.enabled || rules.kinds.is_empty()) {
            return false;
        }
        let Some(info) = source
            .as_edict(player)
            .and_then(|edict| source.player_info(edict))
        else {
            return true;
        };
        if info.team != 2 || info.dead {
            return true;
        }
        if !self.ammo.piles.contains_key(&handle)
            && let Some(state) = self.roll(rules, false)
        {
            Self::tint(source, pile, state.kind);
            self.ammo.piles.insert(handle, state);
        }
        let Some(state) = self.ammo.piles.get(&handle).copied() else {
            return false;
        };
        self.normalize_ammo(player, catalog);
        let index = state.kind.definition().index;
        if info.fake
            && (!state.map
                || !(0..2).any(|slot| {
                    self.weapon_at(player, slot).is_some_and(|weapon| unsafe {
                        field::<i32>(weapon, PRIMARY_AMMO_TYPE) == index as i32
                    })
                }))
        {
            return true;
        }
        let reserve = self.reserve(player);
        let weight_room = inventory.and_then(|inventory| {
            reconcile_native_inventory(
                inventory,
                &self.native_weapons(player),
                catalog.ammo_weight(&reserve),
            );
            let limit = inventory.limits().max_weight;
            (limit > 0.0).then(|| (limit - inventory.weight()).max(0.0))
        });
        let before = reserve[index];
        let taken = catalog
            .split_unload(Some(state.kind), state.rounds, before, weight_room)
            .to_reserve;
        if taken > 0 {
            unsafe { (self.native.set_ammo_count)(player.as_ptr(), before + taken, index as i32) };
            if taken == state.rounds {
                self.ammo.piles.remove(&handle);
                self.remove(pile);
            } else if let Some(pile) = self.ammo.piles.get_mut(&handle) {
                pile.rounds -= taken;
            }
        }
        true
    }

    pub fn drop_ammo(
        &mut self,
        source: &Source,
        player: Ent,
        kind: AmmoKind,
        amount: i32,
    ) -> String {
        if !self.can_handle(player) {
            return String::new();
        }
        let def = kind.definition();
        let before = self.reserve(player)[def.index];
        let rounds = before.min(if amount > 0 { amount } else { before });
        if rounds <= 0 {
            return format!("No {} to drop", def.title);
        }
        let (eye, forward) = self.eyes(player);
        let pile = Pile {
            kind,
            rounds,
            map: false,
            spawned: true,
        };
        if !self.spawn_pile(source, eye.ahead(forward, 48.0), pile) {
            return format!("Could not drop {}", def.title);
        }
        unsafe { (self.native.set_ammo_count)(player.as_ptr(), before - rounds, def.index as i32) };
        format!("Dropped {rounds} {}", def.title)
    }

    pub fn drop_reserve(&mut self, source: &Source, player: Ent, origin: Vector, offset: i32) {
        let reserve = self.reserve(player);
        for (number, kind) in AmmoKind::ALL.into_iter().enumerate() {
            let def = kind.definition();
            let rounds = reserve[def.index];
            let angle = (offset + number as i32) as f32 * 2.39996;
            let position = origin.offset(38.0 * angle.cos(), 38.0 * angle.sin(), 16.0);
            let pile = Pile {
                kind,
                rounds,
                map: false,
                spawned: true,
            };
            if self.spawn_pile(source, position, pile) {
                unsafe { (self.native.set_ammo_count)(player.as_ptr(), 0, def.index as i32) };
            }
        }
    }

    fn install_reload(&mut self, gun: Ent) -> bool {
        if self.ammo.pile_patch.is_none() {
            return false;
        }
        let table: usize = unsafe { field(gun, 0) };
        if self.ammo.reload_patches.contains_key(&table) {
            return true;
        }
        let base = self.native.base;
        let reload_slot = table + RELOAD_SLOT * size_of::<usize>();
        let fire_slot = table + FIRE_EMPTY_SLOT * size_of::<usize>();
        let Some(reload_ptr) = VtablePatch::checked_slot(base, reload_slot) else {
            return false;
        };
        let original = unsafe { reload_ptr.read() };
        let reload = if original == self.native.gun_reload as usize {
            self.native.gun_reload
        } else if original == self.native.shotgun_reload as usize {
            self.native.shotgun_reload
        } else {
            return false;
        };
        let patches = unsafe {
            VtablePatch::install(
                base,
                reload_slot,
                reload as usize,
                reload_hook as Reload as usize,
            )
            .zip(VtablePatch::install(
                base,
                fire_slot,
                self.native.fire_empty as usize,
                fire_empty_hook as FireEmpty as usize,
            ))
        };
        let Some((reload_patch, fire_patch)) = patches else {
            return false;
        };
        self.ammo
            .reload_patches
            .insert(table, [reload_patch, fire_patch]);
        RELOAD_TABLES
            .with_borrow_mut(|tables| tables.insert(table, (reload, self.native.fire_empty)));
        true
    }

    pub(super) fn weapon_candidate<'a>(
        &self,
        catalog: &'a Catalog,
        entity: Ent,
    ) -> Option<(&'a ItemDef, bool)> {
        let classname = self.classname(entity);
        if let Some(id) = catalog.find_item(classname) {
            return Some((catalog.item(id), false));
        }
        if !classname.starts_with("weapon_") || !classname.ends_with("_spawn") {
            return None;
        }
        if !self.can_use_spawner(entity) {
            return None;
        }
        let alias = unsafe { source::text((self.native.spawned_weapon_name)(entity.as_ptr())) }?;
        let id = catalog
            .find_item(alias)
            .or_else(|| catalog.find_item(&format!("weapon_{alias}")))?;
        Some((catalog.item(id), true))
    }

    pub fn unload_gun(
        &mut self,
        source: &Source,
        player: Ent,
        inventory: &Inventory,
        rules: &MapGunRules,
    ) -> String {
        if !self.can_handle(player) {
            return "The game does not let you unload now".into();
        }
        let (eye, forward) = self.eyes(player);
        let catalog = inventory.catalog();
        let ground = source
            .entities()
            .filter_map(|gun| {
                let (def, spawner) = self.weapon_candidate(catalog, gun)?;
                let kind = def.ammo.filter(|&kind| kind.is_unloadable())?;
                if self.pending_removal(gun)
                    || (!spawner && !unsafe { (self.native.owner)(gun.as_ptr()) }.is_null())
                {
                    return None;
                }
                let pos = self.position(gun)?;
                let offset = pos.offset(-eye.x, -eye.y, -eye.z);
                let distance = offset.dot(offset).sqrt();
                if !(1.0..=160.0).contains(&distance) {
                    return None;
                }
                let facing = offset.dot(forward) / distance;
                (facing >= 0.9).then_some((distance * (2.0 - facing), gun, def, spawner, pos, kind))
            })
            .min_by(|a, b| a.0.total_cmp(&b.0));

        let (gun, def, kind, spawner) = if let Some((_, gun, def, spawner, pos, kind)) = ground {
            if spawner {
                let remaining = unsafe { field::<i32>(gun, SPAWN_COUNT) };
                let last = remaining == 1 || (rules.enabled && rules.spawners_once);
                let pos = if last {
                    pos
                } else {
                    pos.offset(-24.0 * forward.y, 24.0 * forward.x, 0.0)
                };
                let Some(spawned) = self.spawn_at(source, def, &WeaponState::default(), pos) else {
                    return format!("Cannot unload {}", def.name);
                };
                self.seen.remove(&self.handle(spawned));
                self.shorten_map_gun(spawned, def, rules);
                (spawned, def, kind, Some((gun, remaining, last)))
            } else {
                (gun, def, kind, None)
            }
        } else {
            let active = unsafe { (self.native.active_weapon)(player.as_ptr()) };
            let Some((gun, def, kind)) = NonNull::new(active).and_then(|gun| {
                let def = catalog.item(catalog.find_item(self.classname(gun))?);
                Some((gun, def, def.ammo.filter(|&kind| kind.is_unloadable())?))
            }) else {
                return "No gun to unload".into();
            };
            if unsafe { field::<u8>(gun, IN_RELOAD) } != 0 {
                return "Wait for the reload to finish".into();
            }
            (gun, def, kind, None)
        };
        let clip = unsafe { field::<i32>(gun, CLIP) };
        if clip <= 0 {
            if spawner.is_some() {
                self.remove(gun);
            }
            return format!("{} is empty", def.name);
        }
        let carried = ground.is_none();
        if carried && !self.install_reload(gun) {
            return format!("Cannot unload {}", def.name);
        }
        let index = kind.definition().index;
        let before = self.reserve(player)[index];
        let limit = inventory.limits().max_weight;
        let weight_room = (limit > 0.0).then(|| (limit - inventory.weight()).max(0.0));
        let split = catalog.split_unload(Some(kind), clip, before, weight_room);
        let pile = Pile {
            kind,
            rounds: split.to_ground,
            map: false,
            spawned: true,
        };
        if split.to_ground > 0 && !self.spawn_pile(source, eye.ahead(forward, 48.0), pile) {
            if spawner.is_some() {
                self.remove(gun);
            }
            return "The overflow could not be dropped".into();
        }

        if let Some((spawner, remaining, last)) = spawner {
            if last {
                self.remove(spawner);
            } else if remaining > 1 {
                unsafe { set_field(spawner, SPAWN_COUNT, remaining - 1) };
            }
        }
        if carried {
            self.ammo
                .unloaded
                .insert(self.handle(gun), self.handle(player));
        }
        unsafe {
            set_field(gun, CLIP, 0i32);
            set_field(gun, UPGRADES, field::<i32>(gun, UPGRADES) & !3);
            set_field(gun, SPECIAL_ROUNDS, 0u8);
            (self.native.network_changed)(gun.as_ptr());
            (self.native.set_ammo_count)(player.as_ptr(), before + split.to_reserve, index as i32);
        }
        self.seen.insert(self.handle(gun));
        let mut message = format!("Unloaded {clip} {}", kind.definition().title);
        if split.to_ground > 0 {
            message.push_str(&format!(" ({} dropped: no room)", split.to_ground));
        }
        message
    }

    pub fn block_reload(&mut self, gun: Ent) -> bool {
        let handle = self.handle(gun);
        let Some(&expected) = self.ammo.unloaded.get(&handle) else {
            return false;
        };
        let player = NonNull::new(unsafe { (self.native.owner)(gun.as_ptr()) });
        if player.is_some_and(|owner| {
            self.handle(owner) == expected
                && unsafe {
                    field::<i32>(gun, CLIP) == 0
                        && field::<i32>(owner, BUTTONS) & (BUTTON_ATTACK | BUTTON_RELOAD) == 0
                }
        }) {
            return true;
        }
        self.ammo.unloaded.remove(&handle);
        false
    }

    pub fn reload_on_empty(&mut self, gun: Ent) {
        let Some(&expected) = self.ammo.unloaded.get(&self.handle(gun)) else {
            return;
        };
        let Some(owner) = NonNull::new(unsafe { (self.native.owner)(gun.as_ptr()) }) else {
            return;
        };
        let index = unsafe { field::<i32>(gun, PRIMARY_AMMO_TYPE) } as usize;
        if self.handle(owner) == expected
            && self.reserve(owner).get(index).is_some_and(|&ammo| ammo > 0)
        {
            unsafe { set_field(gun, FIRE_ON_EMPTY, 1u8) };
        }
    }
}

unsafe extern "C" fn pile_use(
    pile: *mut Entity,
    player: *mut Entity,
    caller: *mut Entity,
    mode: i32,
    value: f32,
) {
    if plugin::ammo_use(pile, player) {
        return;
    }
    if let Some(original) = ORIGINAL_PILE.get() {
        unsafe { original(pile, player, caller, mode, value) };
    }
}

unsafe extern "C" fn reload_hook(gun: *mut Entity) -> bool {
    let Some(weapon) = NonNull::new(gun) else {
        return false;
    };
    if plugin::block_reload(weapon) {
        return false;
    }
    let table = unsafe { field::<usize>(weapon, 0) };
    let original = RELOAD_TABLES.with_borrow(|tables| tables.get(&table).map(|pair| pair.0));
    original.is_some_and(|function| unsafe { function(gun) })
}

unsafe extern "C" fn fire_empty_hook(gun: *mut Entity) {
    let Some(weapon) = NonNull::new(gun) else {
        return;
    };
    plugin::reload_on_empty(weapon);
    let table = unsafe { field::<usize>(weapon, 0) };
    let original = RELOAD_TABLES.with_borrow(|tables| tables.get(&table).map(|pair| pair.1));
    if let Some(function) = original {
        unsafe { function(gun) };
    }
}
