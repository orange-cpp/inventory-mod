use std::{
    collections::HashMap,
    ffi::{CStr, CString, c_char, c_void},
    fs, io,
    path::{Path, PathBuf},
    ptr,
    sync::Arc,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use inventory_mod::{
    inventory::{Inventory, Limits, WeaponState, melee_script_name},
    items::{AmmoKind, Catalog, GameSlot, ItemDef, ItemId},
    mutators::{Mutator, pick_mutators},
    remote_state::{
        NATIVE_SLOTS, NativeWeapon, RemoteState, encode_remote_state, reconcile_native_inventory,
    },
    rules::{ChapterRules, MapGunRules, Rules},
    settings::Settings,
};
use windows_sys::Win32::System::LibraryLoader::GetProcAddress;

use crate::{
    native::{self, PeImage},
    reload::Reload,
};

type Factory = unsafe extern "C" fn(*const c_char, *mut i32) -> *mut c_void;
type GetKey =
    unsafe extern "thiscall" fn(*mut c_void, *mut c_void, *const c_char, *mut u8, i32) -> bool;
type SetKey =
    unsafe extern "thiscall" fn(*mut c_void, *mut c_void, *const c_char, *const c_char) -> bool;
type CreateEntity = unsafe extern "thiscall" fn(*mut c_void, *const c_char) -> *mut c_void;
type Spawn = unsafe extern "thiscall" fn(*mut c_void, *mut c_void);
type Remove = unsafe extern "C" fn(*mut c_void);
type Give = unsafe extern "thiscall" fn(*mut c_void, *const c_char, i32, i32, i32) -> *mut c_void;
type DropWeapon =
    unsafe extern "thiscall" fn(*mut c_void, *mut c_void, *const Vector, *const Vector);
type CanDrop = unsafe extern "thiscall" fn(*mut c_void) -> i32;
type Select = unsafe extern "thiscall" fn(*mut c_void, *const c_char, i32) -> *mut c_void;
type EyePosition = unsafe extern "thiscall" fn(*mut c_void, *mut Vector) -> *mut Vector;
type EyeAngles = unsafe extern "thiscall" fn(*mut c_void) -> *const Vector;
type Incapacitated = unsafe extern "thiscall" fn(*mut c_void) -> bool;
type Precache = unsafe extern "thiscall" fn(*mut c_void);
type Teleport =
    unsafe extern "thiscall" fn(*mut c_void, *const Vector, *const Vector, *const Vector);

const ENTITY_ORIGIN: usize = 0x388;
const PLAYER_TEAM: usize = 0x238;
const LIFE_STATE: usize = 0xf0;
const RESERVE_POOL: usize = 0x1874;
const WEAPONS: usize = 0x18f4;
const ACTIVE_WEAPON: usize = 0x19d4;
const CLIP: usize = 0x1414;
const WEAPON_OWNER: usize = 0x13e8;
const AMMO_TYPE: usize = 0x140c;
const EXTRA_AMMO: usize = 0x14f4;
const UPGRADES: usize = 0x17e0;
const SPECIAL_ROUNDS: usize = 0x1788;
// The engine saves the resolved name from +0x17dc. The adjacent datamap
// field holds the name requested when the weapon was created.
const MELEE_SCRIPT_NAME: usize = 0x17dc;
const MELEE_MAP_SCRIPT_NAME: usize = 0x17e0;

#[derive(Clone, Copy, Default)]
#[repr(C)]
struct Vector {
    x: f32,
    y: f32,
    z: f32,
}

impl Vector {
    fn read(addr: usize) -> Option<Self> {
        Some(Self {
            x: f32::from_bits(native::word(addr)?),
            y: f32::from_bits(native::word(addr + 4)?),
            z: f32::from_bits(native::word(addr + 8)?),
        })
    }

    fn ahead(self, dir: Self, distance: f32) -> Self {
        Self {
            x: self.x + dir.x * distance,
            y: self.y + dir.y * distance,
            z: self.z + dir.z * distance - 14.0,
        }
    }

    fn difference(self, before: Self) -> Self {
        Self {
            x: self.x - before.x,
            y: self.y - before.y,
            z: self.z - before.z,
        }
    }

    fn dot(self, other: Self) -> f32 {
        self.x * other.x + self.y * other.y + self.z * other.z
    }
}

fn address(value: usize) -> *mut c_void {
    ptr::with_exposed_provenance_mut(value)
}

struct Game {
    image: PeImage,
    tools: usize,
    entities: usize,
    player_table: usize,
    get_key: GetKey,
    set_key: SetKey,
    create: CreateEntity,
    spawn: Spawn,
    remove: Remove,
    give: Give,
    select: Select,
    drop: DropWeapon,
    quiet_drop: DropWeapon,
    cannot_drop: CanDrop,
    incapacitated: Incapacitated,
    eye_position: EyePosition,
    eye_angles: EyeAngles,
    wear: combat::Wear,
}

impl Game {
    fn bind() -> Option<Self> {
        let module = native::module("server.dll")?;
        let image = PeImage::loaded(module)?;
        let player_rva = match image.timestamp {
            0x6a44_0e53 => 0x6973b4,
            0x6afc_cca0 => 0x69740c,
            _ => return None,
        };
        let player_table = image.base + player_rva;
        let locator = native::word(player_table - 4)? as usize;
        let ty = native::word(locator + 12)? as usize;
        if native::word(locator + 4) != Some(0)
            || native::string(ty + 8, 64).as_deref() != Some(".?AVCTerrorPlayer@@")
        {
            return None;
        }
        let factory =
            unsafe { GetProcAddress(module.as_ptr(), c"CreateInterface".as_ptr().cast()) }?;
        let factory = unsafe {
            std::mem::transmute::<unsafe extern "system" fn() -> isize, Factory>(factory)
        };
        let tools = unsafe { factory(c"VSERVERTOOLS001".as_ptr(), ptr::null_mut()) }.addr();
        let slot = |index| native::vslot(tools, index).filter(|&entry| image.includes(entry, 1));
        let first = slot(7)?;
        let list = native::find_pattern(first, 24, "6A 00 B9 ?? ?? ?? ??")?;
        let entities = native::word(list + 3)? as usize;
        if !image.includes(entities, 16 * 4096 + 4) {
            return None;
        }
        let set_key = slot(13)?;
        if !native::matches_pattern(
            set_key,
            "55 8B EC 8B 55 10 8B 4D 08 8B 01 8B 80 88 00 00 00",
        ) {
            return None;
        }
        let call = native::find_pattern(slot(16)?, 0x60, "50 E8 ?? ?? ?? ?? 83 C4 04 B0 01")?;
        let remove = relative(call + 1)?;
        if !image.includes(remove, 1) || !native::matches_pattern(remove, "55") {
            return None;
        }

        let player = |index: usize| {
            native::word(player_table + 4 * index)
                .map(|target| target as usize)
                .filter(|&target| image.includes(target, 1))
        };
        let drop = player(283)?;
        let quiet = native::find_pattern(drop, 0x20, "50 51 57 8B CB E8 ?? ?? ?? ??")
            .and_then(|entry| relative(entry + 5))
            .filter(|&entry| image.includes(entry, 1) && native::matches_pattern(entry, "55"))
            .unwrap_or(drop);
        Some(Self {
            tools,
            entities,
            player_table,
            get_key: unsafe { std::mem::transmute::<usize, GetKey>(slot(10)?) },
            set_key: unsafe { std::mem::transmute::<usize, SetKey>(set_key) },
            create: unsafe { std::mem::transmute::<usize, CreateEntity>(slot(14)?) },
            spawn: unsafe { std::mem::transmute::<usize, Spawn>(slot(15)?) },
            remove: unsafe { std::mem::transmute::<usize, Remove>(remove) },
            give: unsafe { std::mem::transmute::<usize, Give>(player(431)?) },
            select: unsafe { std::mem::transmute::<usize, Select>(player(429)?) },
            drop: unsafe { std::mem::transmute::<usize, DropWeapon>(drop) },
            quiet_drop: unsafe { std::mem::transmute::<usize, DropWeapon>(quiet) },
            cannot_drop: unsafe { std::mem::transmute::<usize, CanDrop>(player(401)?) },
            incapacitated: unsafe { std::mem::transmute::<usize, Incapacitated>(player(505)?) },
            eye_position: unsafe { std::mem::transmute::<usize, EyePosition>(player(140)?) },
            eye_angles: unsafe { std::mem::transmute::<usize, EyeAngles>(player(141)?) },
            wear: combat::Wear::default(),
            image,
        })
    }

    fn entity(&self, index: usize) -> usize {
        native::word(self.entities + 4 + index * 16).unwrap_or_default() as usize
    }

    fn resolve(&self, handle: u32) -> Option<usize> {
        if handle == u32::MAX {
            return None;
        }
        let index = (handle & 0xfff) as usize;
        let serial = native::word(self.entities + 8 + index * 16)?;
        let entity = self.entity(index);
        (serial == handle >> 12 && entity != 0).then_some(entity)
    }

    fn host(&self) -> Option<usize> {
        let player = self.entity(1);
        (player != 0 && native::word(player).map(|table| table as usize) == Some(self.player_table))
            .then_some(player)
    }

    fn item(&self, entity: usize, catalog: &Catalog) -> Option<ItemId> {
        catalog.find_item(&self.classname(entity)?)
    }

    fn classname(&self, entity: usize) -> Option<String> {
        self.key(entity, c"classname")
    }

    fn key(&self, entity: usize, key: &CStr) -> Option<String> {
        let mut name = [0; 64];
        let ok = unsafe {
            (self.get_key)(
                address(self.tools),
                address(entity),
                key.as_ptr(),
                name.as_mut_ptr(),
                name.len() as i32,
            )
        };
        name[63] = 0;
        if !ok {
            return None;
        }
        Some(
            CStr::from_bytes_until_nul(&name)
                .ok()?
                .to_str()
                .ok()?
                .to_owned(),
        )
    }

    fn living(&self, player: usize) -> bool {
        let mut life = [0xff];
        native::word(player + PLAYER_TEAM) == Some(2)
            && native::read(player + LIFE_STATE, &mut life)
            && life[0] == 0
    }

    fn available(&self, player: usize) -> bool {
        self.living(player)
            && !unsafe { (self.incapacitated)(address(player)) }
            && unsafe { (self.cannot_drop)(address(player)) } == 0
    }

    fn ammo(&self, player: usize) -> [i32; 32] {
        let mut raw = [0; 128];
        if !native::read(player + RESERVE_POOL, &mut raw) {
            return [0; 32];
        }
        let data = raw.as_chunks::<4>().0;
        std::array::from_fn(|index| i32::from_le_bytes(data[index]))
    }

    fn restore(&self, player: usize, ammo: [i32; 32]) {
        for (index, (current, target)) in self.ammo(player).into_iter().zip(ammo).enumerate() {
            if current != target {
                native::store_word(player + RESERVE_POOL + 4 * index, target as u32);
            }
        }
    }

    fn retarget_ammo(&self, player: usize, catalog: &Catalog, hooks: Option<&Hooks>) {
        let original = self.ammo(player);
        let mut reserve = original;
        let auto = reserve[8];
        if auto > 0 {
            let cap = catalog.ammo_max(AmmoKind::Shotgun);
            reserve[7] = reserve[7].saturating_add(auto.min(cap.saturating_sub(reserve[7]).max(0)));
            reserve[8] = 0;
        }
        for kind in AmmoKind::ALL {
            if let Some(cap) = catalog.ammo_max_override(kind) {
                let index = kind.definition().index;
                reserve[index] = reserve[index].min(cap);
            }
        }
        if reserve != original {
            self.restore(player, reserve);
            if let Some(hooks) = hooks {
                hooks.changed(player);
            }
        }
        for (weapon, _) in self.weapons(player, catalog).into_iter().take(2).flatten() {
            let next = match native::word(weapon + AMMO_TYPE) {
                Some(1) => 5,
                Some(2) => 10,
                Some(8) => 7,
                _ => continue,
            };
            if native::store_word(weapon + AMMO_TYPE, next)
                && let Some(hooks) = hooks
            {
                hooks.changed(weapon);
            }
        }
    }

    fn weapons(&self, player: usize, catalog: &Catalog) -> [Option<(usize, ItemId)>; NATIVE_SLOTS] {
        let mut equipment = [None; NATIVE_SLOTS];
        for index in 0..56 {
            if let Some((entity, id)) = native::word(player + WEAPONS + index * 4)
                .and_then(|handle| self.resolve(handle))
                .and_then(|entity| self.item(entity, catalog).map(|id| (entity, id)))
            {
                equipment[slot_index(catalog.item(id).game_slot)].get_or_insert((entity, id));
            }
        }
        equipment
    }

    fn matches_melee(def: &ItemDef) -> bool {
        def.classname == "weapon_melee"
    }

    fn melee_script(weapon: usize) -> String {
        [MELEE_SCRIPT_NAME, MELEE_MAP_SCRIPT_NAME]
            .into_iter()
            .filter_map(|offset| native::word(weapon + offset))
            .filter_map(|name| native::string(name as usize, 64))
            .find_map(|name| melee_script_name(&name).map(str::to_owned))
            .unwrap_or_default()
    }

    fn firearm(def: &ItemDef) -> bool {
        def.is_weapon() && !matches!(def.classname, "weapon_chainsaw" | "weapon_melee")
    }

    fn state(&self, weapon: usize, def: &ItemDef, on_ground: bool) -> WeaponState {
        let mut state = WeaponState::default();
        if Self::matches_melee(def) {
            state.melee = Self::melee_script(weapon);
            state.wear = ammo::handle(self, weapon)
                .and_then(|handle| self.wear.numbers.borrow().get(&handle).copied())
                .unwrap_or_default();
        } else {
            state.clip = native::word(weapon + CLIP).map(|n| n as i32).unwrap_or(-1);
        }
        if Self::firearm(def) {
            state.upgrades = native::word(weapon + UPGRADES).unwrap_or(0) as i32 & 7;
            state.special = native::word(weapon + SPECIAL_ROUNDS).unwrap_or(0) as i32;
            if on_ground {
                state.ammo = native::word(weapon + EXTRA_AMMO)
                    .map(|n| n as i32)
                    .unwrap_or(-1);
            }
        }
        state
    }

    fn apply(&self, weapon: usize, def: &ItemDef, state: &WeaponState, on_ground: bool) {
        if Self::matches_melee(def)
            && let Some(handle) = ammo::handle(self, weapon)
        {
            if state.wear > 0 {
                self.wear.numbers.borrow_mut().insert(handle, state.wear);
            } else {
                self.wear.numbers.borrow_mut().remove(&handle);
            }
        }
        if state.clip >= 0 && !Self::matches_melee(def) {
            native::store_word(weapon + CLIP, state.clip as u32);
        }
        if Self::firearm(def) {
            native::store_word(weapon + UPGRADES, (state.upgrades & 7) as u32);
            native::store_word(weapon + SPECIAL_ROUNDS, state.special.max(0) as u32);
            if on_ground && state.ammo >= 0 {
                native::store_word(weapon + EXTRA_AMMO, state.ammo as u32);
            }
        }
    }

    fn active(&self, player: usize) -> Option<usize> {
        self.resolve(native::word(player + ACTIVE_WEAPON)?)
    }

    fn native(&self, player: usize, catalog: &Catalog) -> [NativeWeapon; NATIVE_SLOTS] {
        let active = self.active(player);
        self.weapons(player, catalog).map(|entry| {
            let Some((entity, id)) = entry else {
                return NativeWeapon::default();
            };
            let def = catalog.item(id);
            let state = self.state(entity, def, false);
            NativeWeapon {
                classname: def.classname.to_owned(),
                active: active == Some(entity),
                melee: state.melee,
                wear: Self::matches_melee(def).then_some(state.wear),
            }
        })
    }

    fn release(
        &self,
        player: usize,
        weapon: usize,
        def: &ItemDef,
        catalog: &Catalog,
        quiet: bool,
    ) -> bool {
        let reserve = self.ammo(player);
        let drop = if quiet { self.quiet_drop } else { self.drop };
        unsafe { drop(address(player), address(weapon), ptr::null(), ptr::null()) };
        self.restore(player, reserve);
        if self.weapons(player, catalog)[slot_index(def.game_slot)]
            .is_some_and(|(current, _)| current == weapon)
        {
            return false;
        }
        if Self::firearm(def) {
            native::store_word(weapon + EXTRA_AMMO, 0);
        }
        true
    }

    fn remove(&self, entity: usize) {
        unsafe { (self.remove)(address(entity)) };
    }

    fn select(&self, player: usize, name: &str) {
        if let Ok(name) = CString::new(name) {
            unsafe { (self.select)(address(player), name.as_ptr(), 0) };
        }
    }

    fn give(&self, player: usize, name: &str, def: &ItemDef, catalog: &Catalog) -> Option<usize> {
        let name = CString::new(name).ok()?;
        unsafe { (self.give)(address(player), name.as_ptr(), 0, 0, 0) };
        let (entity, id) = self.weapons(player, catalog)[slot_index(def.game_slot)]?;
        (catalog.item(id).classname == def.classname).then_some(entity)
    }

    fn eyes(&self, player: usize) -> Option<(Vector, Vector)> {
        let mut eye = Vector::default();
        unsafe { (self.eye_position)(address(player), &mut eye) };
        let angles = Vector::read(unsafe { (self.eye_angles)(address(player)) }.addr())?;
        let (sp, cp) = angles.x.to_radians().sin_cos();
        let (sy, cy) = angles.y.to_radians().sin_cos();
        Some((
            eye,
            Vector {
                x: cp * cy,
                y: cp * sy,
                z: -sp,
            },
        ))
    }

    fn look_for(
        &self,
        player: usize,
        catalog: &Catalog,
        facing: f32,
        wanted: impl Fn(&ItemDef) -> bool,
    ) -> Option<(usize, ItemId)> {
        self.look_ranked(player, facing, |entity| {
            let item = self.item(entity, catalog)?;
            (wanted(catalog.item(item)) && native::word(entity + WEAPON_OWNER) == Some(u32::MAX))
                .then_some(item)
        })
        .map(|(_, entity, id)| (entity, id))
    }

    fn look_ranked<T>(
        &self,
        player: usize,
        facing: f32,
        identify: impl Fn(usize) -> Option<T>,
    ) -> Option<(f32, usize, T)> {
        let (eye, forward) = self.eyes(player)?;
        let mut entries = vec![0_u8; 16 * 4096];
        let bulk = native::read(self.entities, &mut entries);
        (1..4096)
            .filter_map(|index| {
                let entity = if bulk {
                    let [word, ..] = entries[16 * index + 4..].as_chunks::<4>().0 else {
                        return None;
                    };
                    u32::from_le_bytes(*word) as usize
                } else {
                    self.entity(index)
                };
                if entity == 0 || entity == player {
                    return None;
                }
                let at = Vector::read(entity + ENTITY_ORIGIN)?;
                let delta = at.difference(eye);
                let distance = delta.dot(delta).sqrt();
                if !(1.0..=160.0).contains(&distance) {
                    return None;
                }
                let alignment = delta.dot(forward) / distance;
                if alignment < facing {
                    return None;
                }
                Some((distance * (2.0 - alignment), entity, identify(entity)?))
            })
            .min_by(|a, b| a.0.total_cmp(&b.0))
    }

    fn create(&self, def: &ItemDef, state: &WeaponState, position: Vector) -> Option<usize> {
        self.spawn_item(def, Some(state), position, Vector::default())
    }

    fn spawn_item(
        &self,
        def: &ItemDef,
        state: Option<&WeaponState>,
        position: Vector,
        angles: Vector,
    ) -> Option<usize> {
        let name = CString::new(def.classname).ok()?;
        let entity = unsafe { (self.create)(address(self.tools), name.as_ptr()) }.addr();
        if entity == 0 {
            return None;
        }
        let set_vector = |key: &CStr, value: Vector| {
            if let Ok(text) = CString::new(format!("{:.2} {:.2} {:.2}", value.x, value.y, value.z))
            {
                unsafe {
                    (self.set_key)(
                        address(self.tools),
                        address(entity),
                        key.as_ptr(),
                        text.as_ptr(),
                    )
                };
            }
        };
        set_vector(c"origin", position);
        set_vector(c"angles", angles);
        if let Some(state) = state.filter(|_| Self::matches_melee(def))
            && let Ok(name) = CString::new(state.give_name(def.classname))
        {
            unsafe {
                (self.set_key)(
                    address(self.tools),
                    address(entity),
                    c"melee_script_name".as_ptr(),
                    name.as_ptr(),
                )
            };
        }
        if let Some(precache) =
            native::vslot(entity, 26).filter(|&entry| self.image.includes(entry, 1))
        {
            let precache = unsafe { std::mem::transmute::<usize, Precache>(precache) };
            unsafe { precache(address(entity)) };
        }
        unsafe { (self.spawn)(address(self.tools), address(entity)) };
        if let Some(state) = state.filter(|_| def.is_weapon()) {
            self.apply(entity, def, state, true);
        }
        Some(entity)
    }
}

fn relative(at: usize) -> Option<usize> {
    let offset = native::word(at + 1)? as i32;
    Some((at + 5).wrapping_add_signed(offset as isize))
}

fn slot_index(slot: GameSlot) -> usize {
    match slot {
        GameSlot::Primary => 0,
        GameSlot::Secondary => 1,
        GameSlot::Throwable => 2,
        GameSlot::Medical => 3,
        GameSlot::Pills => 4,
        GameSlot::Carry => 5,
    }
}

mod actions;
mod ammo;
mod combat;
mod director;
mod hooks;
mod infection;
mod movement;
mod pickups;
mod pings;
use ammo::Ammo;
pub(crate) use hooks::factory;
use hooks::{Event, Hooks};

pub struct Host {
    game: Game,
    directory: PathBuf,
    rules: Rules,
    effective: ChapterRules,
    mutators: Vec<Mutator>,
    generation: u32,
    next_mutators: Option<Instant>,
    hud: Vec<String>,
    last_native_tick: Option<Instant>,
    last_reserve_tick: Option<Instant>,
    limits: Limits,
    local: Session,
    guests: HashMap<i32, Guest>,
    away: HashMap<PathBuf, Session>,
    campaign_since: Option<SystemTime>,
    cans: actions::Cans,
    hooks: Option<Hooks>,
    ammo: Option<Ammo>,
    movement: movement::Movement,
    infection: Option<infection::Infection>,
    pings: pings::Pings,
    combat: combat::Combat,
    pickups: pickups::Pickups,
    reload: Reload,
    director: director::Director,
    actions: actions::NativeActions,
    chapter: String,
    transition: bool,
    dedicated: bool,
}

pub(crate) struct Session {
    inventory: Inventory,
    file: Option<PathBuf>,
    revision: u32,
    chapter_start: String,
    carry_ammo: Option<[i32; 32]>,
    death: Option<Vector>,
    reserve: Option<(usize, [i32; 32])>,
    actions: actions::Actions,
    notice: String,
}

struct Guest {
    player: usize,
    edict: usize,
    session: Session,
}

impl Session {
    pub fn offline(directory: &Path, settings: &Settings) -> Self {
        let rules = read_rules(directory);
        Self::local(directory, &rules, settings)
    }

    fn local(directory: &Path, rules: &Rules, settings: &Settings) -> Self {
        Self::load(
            Some(directory.join("inventory_mod.inventory")),
            rules.limits.apply(settings.limits),
            Arc::clone(&rules.catalog),
        )
    }

    pub fn preview(&self) -> Option<RemoteState> {
        let line = encode_remote_state(&self.inventory, &[], "");
        RemoteState::decode(&line, self.inventory.catalog())
    }

    pub fn catalog(&self) -> &Catalog {
        self.inventory.catalog()
    }

    pub fn edit(&mut self, edit: impl FnOnce(&mut Inventory)) -> Option<RemoteState> {
        edit(&mut self.inventory);
        if let Err(error) = self.save() {
            crate::client::log(&format!("could not save the inventory ({error})"));
        }
        self.preview()
    }

    fn load(file: Option<PathBuf>, limits: Limits, catalog: Arc<Catalog>) -> Self {
        let mut inventory = Inventory::with_catalog(limits, catalog);
        if let Some(file) = &file
            && let Ok(saved) = fs::read_to_string(file)
        {
            inventory.load_text(&saved);
        }
        let revision = inventory.revision();
        let chapter_start = inventory.to_text();
        Self {
            inventory,
            file,
            revision,
            chapter_start,
            carry_ammo: None,
            death: None,
            reserve: None,
            actions: actions::Actions::default(),
            notice: String::new(),
        }
    }

    fn save(&mut self) -> io::Result<()> {
        if let Some(file) = self
            .file
            .as_ref()
            .filter(|_| self.revision != self.inventory.revision())
        {
            let temporary = file.with_extension("inventory.tmp");
            if let Err(error) = fs::write(&temporary, self.inventory.to_text())
                .and_then(|()| fs::rename(&temporary, file))
            {
                let _ = fs::remove_file(temporary);
                return Err(error);
            }
            self.revision = self.inventory.revision();
        }
        Ok(())
    }

    fn start_round(&mut self, same_map: bool, transition: bool) {
        if same_map {
            self.inventory.load_text(&self.chapter_start);
        } else if !transition {
            self.inventory.clear();
        }
        if !transition {
            self.carry_ammo = None;
        }
        self.death = None;
        self.reserve = None;
        self.actions = actions::Actions::default();
        self.chapter_start = self.inventory.to_text();
        let _ = self.save();
    }
}

fn read_rules(directory: &Path) -> Rules {
    let config = fs::read_to_string(directory.join("inventory_mod.toml")).unwrap_or_default();
    Rules::parse(&config)
        .map(|(rules, _)| rules)
        .unwrap_or_default()
}

impl Host {
    pub fn bind(directory: &Path, settings: &Settings, dedicated: bool) -> Option<Self> {
        let game = Game::bind()?;
        let rules = read_rules(directory);
        let limits = rules.limits.apply(settings.limits);
        let local = Session::local(directory, &rules, settings);
        let hooks = Hooks::install(&game.image, dedicated);
        if dedicated && hooks.is_none() {
            return None;
        }
        let ammo = Ammo::install(&game);
        let actions = actions::NativeActions::install(&game, &rules);
        let pickups = pickups::Pickups::install(&game, settings.auto_store);
        let combat = combat::Combat::install(&game, &rules);
        let reload = Reload::server(&game.image);
        let effective = rules.chapter(&[]);
        let seed = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|n| n.as_nanos() as u32)
            .unwrap_or_default();
        let infection = infection::Infection::install(&game, &rules, seed);
        let mut host = Self {
            game,
            directory: directory.to_owned(),
            rules,
            effective,
            mutators: Vec::new(),
            generation: SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map(|n| n.as_nanos() as u32)
                .unwrap_or_default(),
            next_mutators: None,
            hud: Vec::new(),
            last_native_tick: None,
            last_reserve_tick: None,
            limits,
            local,
            guests: HashMap::new(),
            away: HashMap::new(),
            campaign_since: None,
            cans: actions::Cans::default(),
            hooks,
            ammo,
            movement: movement::Movement::default(),
            infection,
            pings: pings::Pings::bind(),
            combat,
            pickups,
            reload,
            director: director::Director::default(),
            actions,
            chapter: String::new(),
            transition: false,
            dedicated,
        };
        host.apply_reserve_caps();
        Some(host)
    }

    pub fn stop(&mut self) -> io::Result<()> {
        self.reload.stop()?;
        self.combat.stop()?;
        self.pickups.stop()?;
        self.director_reset();
        self.pings.reset(&self.game, self.hooks.as_ref());
        self.clear_infection(true);
        if let Some(infection) = &mut self.infection {
            infection.stop()?;
        }
        self.clear_actions();
        self.actions.stop()?;
        self.reset_movement(true);
        if let Some(ammo) = &mut self.ammo {
            ammo.stop()?;
        }
        if let Some(hooks) = &mut self.hooks {
            hooks.stop()?;
        }
        Ok(())
    }

    pub fn active(&self) -> bool {
        self.game.host().is_some()
    }

    pub fn catalog(&self) -> &Catalog {
        &self.rules.catalog
    }

    pub fn debug_inventory(&mut self, edit: impl FnOnce(&mut Inventory)) -> Option<RemoteState> {
        if !self.active() {
            return self.local.edit(edit);
        }
        edit(&mut self.local.inventory);
        self.command(c"inv_sync")
    }

    pub fn local_preview(&self) -> Option<RemoteState> {
        self.local.preview()
    }

    pub fn is_local_player(&self, player: usize) -> bool {
        !self.dedicated && self.game.host() == Some(player)
    }

    pub fn set_auto_store(&mut self, enabled: bool) {
        self.pickups.auto_store = enabled;
    }

    pub fn block_reload(&mut self, weapon: usize) -> bool {
        let Some(actor) =
            native::word(weapon + WEAPON_OWNER).and_then(|handle| self.game.resolve(handle))
        else {
            return false;
        };
        self.reload.block(actor, weapon)
    }

    pub fn fire_empty(&self, weapon: usize) -> bool {
        if native::word(weapon + CLIP) != Some(0) {
            return false;
        }
        let rounds = native::word(weapon + AMMO_TYPE)
            .filter(|&kind| kind < 32)
            .and_then(|kind| {
                let owner = self.game.resolve(native::word(weapon + WEAPON_OWNER)?)?;
                native::word(owner + RESERVE_POOL + 4 * kind as usize)
            })
            .map(|rounds| rounds as i32)
            .unwrap_or_default();
        rounds > 0 && native::store_bytes(weapon + 0x144e, &[1])
    }

    pub fn dedicated_command(&mut self, edict: usize, args: usize) -> bool {
        let Some(command) = hooks::read_command(args) else {
            return false;
        };
        let Some(player) = self.hooks.as_ref().and_then(|hooks| hooks.player(edict)) else {
            return false;
        };
        self.guest(player, &command)
    }

    pub fn command(&mut self, request: &CStr) -> Option<RemoteState> {
        self.events();
        let player = self.game.host()?;
        finish_death(
            &self.game,
            &mut self.local,
            &mut self.cans,
            self.ammo.as_mut().zip(self.hooks.as_ref()),
            player,
        );
        let mut request = request.to_str().ok()?;
        if request == "inv_infect" {
            self.infect(player);
            request = "inv_sync";
        }
        if request == "inv_ping" {
            self.local.notice = self.ping(player);
            request = "inv_sync";
        }
        if let Some(hooks) = &self.hooks
            && let Some(notice) = actions::revive_command(
                &self.actions,
                &self.game,
                hooks,
                &self.rules,
                &mut self.local,
                player,
                request,
            )
        {
            self.local.notice = notice;
            request = "inv_sync";
        }
        let reply = run_session(
            &self.game,
            &mut self.local,
            &mut self.cans,
            self.ammo.as_mut().zip(self.hooks.as_ref()),
            (
                &mut self.pickups,
                &self.effective.map_guns,
                &mut self.reload,
            ),
            player,
            request,
        )?;
        RemoteState::decode(&reply, self.local.inventory.catalog())
    }

    pub fn guest(&mut self, player: usize, command: &str) -> bool {
        self.events();
        if (!self.dedicated && self.game.host() == Some(player))
            || native::word(player).map(|n| n as usize) != Some(self.game.player_table)
        {
            return false;
        }
        let ping = (command == "inv_ping").then(|| self.ping(player));
        let command = if command == "inv_infect" {
            self.infect(player);
            "inv_sync"
        } else if ping.is_some() {
            "inv_sync"
        } else {
            command
        };
        let Some(hooks) = &self.hooks else {
            return false;
        };
        let Some(edict) = native::word(player + 0x28)
            .map(|addr| addr as usize)
            .filter(|&addr| addr != 0)
        else {
            return false;
        };
        let Some(user) = hooks.user(edict) else {
            return false;
        };
        let identity = hooks.steam_id(edict);
        if identity.as_deref() == Some("STEAM_ID_PENDING") {
            return true;
        }
        if identity.as_deref() == Some("BOT") {
            return false;
        }
        let table = &mut self.guests;
        let guest = table.entry(user).or_insert_with(|| {
            let file = identity.and_then(|id| player_file(&self.directory, &id));
            let previous = file.as_ref().and_then(|file| self.away.remove(file));
            let stale = self
                .campaign_since
                .zip(file.as_ref())
                .is_some_and(|(since, path)| {
                    fs::metadata(path)
                        .and_then(|metadata| metadata.modified())
                        .is_ok_and(|modified| modified < since)
                });
            let session = if let Some(previous) = previous {
                previous
            } else if stale {
                let mut session = Session::load(None, self.limits, Arc::clone(&self.rules.catalog));
                session.file = file;
                session
            } else {
                Session::load(file, self.limits, Arc::clone(&self.rules.catalog))
            };
            Guest {
                player,
                edict,
                session,
            }
        });
        guest.player = player;
        guest.edict = edict;
        if let Some(notice) = ping {
            guest.session.notice = notice;
        }
        finish_death(
            &self.game,
            &mut guest.session,
            &mut self.cans,
            self.ammo.as_mut().map(|ammo| (ammo, hooks)),
            player,
        );
        let command = if let Some(notice) = actions::revive_command(
            &self.actions,
            &self.game,
            hooks,
            &self.rules,
            &mut guest.session,
            player,
            command,
        ) {
            guest.session.notice = notice;
            "inv_sync"
        } else {
            command
        };
        if let Some(response) = run_session(
            &self.game,
            &mut guest.session,
            &mut self.cans,
            self.ammo.as_mut().map(|ammo| (ammo, hooks)),
            (
                &mut self.pickups,
                &self.effective.map_guns,
                &mut self.reload,
            ),
            player,
            command,
        ) && let Ok(response) = CString::new(response)
        {
            hooks.print(edict, &response);
        }
        true
    }

    pub fn events(&mut self) {
        for event in hooks::take_events() {
            self.infection_event(event);
            let Some(hooks) = &self.hooks else {
                return;
            };
            match event {
                Event::Transition => {
                    self.transition = true;
                    if !self.dedicated {
                        self.local.carry_ammo =
                            self.game.host().map(|player| self.game.ammo(player));
                    }
                    for (&user, guest) in &mut self.guests {
                        if hooks.user(guest.edict) == Some(user) {
                            guest.session.carry_ammo = Some(self.game.ammo(guest.player));
                        }
                    }
                }
                Event::RoundStart => {
                    self.combat.reset();
                    self.game.wear.numbers.borrow_mut().clear();
                    self.pickups.reset();
                    self.reload.reset();
                    self.pings.reset(&self.game, Some(hooks));
                    let next = hooks.map().unwrap_or_default();
                    let first = self.chapter.is_empty();
                    let same = !self.transition && !first && self.chapter == next;
                    if !same {
                        self.generation = self.generation.wrapping_add(1);
                        let seed = SystemTime::now()
                            .duration_since(UNIX_EPOCH)
                            .map(|n| n.as_nanos() as u32)
                            .unwrap_or(self.generation);
                        self.mutators = pick_mutators(&self.rules.mutators, &next, seed);
                        self.effective = self.rules.chapter(&self.mutators);
                        if !self.transition {
                            self.campaign_since = Some(SystemTime::now());
                        }
                    }
                    self.director_chapter();
                    self.next_mutators = None;
                    self.local.start_round(same, self.transition);
                    self.local
                        .inventory
                        .set_max_weight(self.effective.max_weight(self.limits.max_weight));
                    if let Some(ammo) = &mut self.ammo {
                        ammo.reset(&self.effective.ammo_piles);
                    }
                    for session in self
                        .guests
                        .values_mut()
                        .map(|guest| &mut guest.session)
                        .chain(self.away.values_mut())
                    {
                        session.start_round(same, self.transition);
                        session
                            .inventory
                            .set_max_weight(self.effective.max_weight(self.limits.max_weight));
                    }
                    self.chapter = next;
                    self.transition = false;
                    self.cans.clear();
                }
                Event::Death(user) => {
                    let player = self.game.host().filter(|&player| {
                        if self.dedicated {
                            return false;
                        }
                        native::word(player + 0x28).and_then(|edict| hooks.user(edict as usize))
                            == Some(user)
                    });
                    if let Some(player) = player {
                        self.local.death = Vector::read(player + ENTITY_ORIGIN);
                    } else if let Some(guest) = self.guests.get_mut(&user)
                        && hooks.user(guest.edict) == Some(user)
                    {
                        guest.session.death = Vector::read(guest.player + ENTITY_ORIGIN);
                    }
                }
                Event::LeftStart => self.left_start(),
                _ => {}
            }
        }
    }
}

impl Host {
    pub fn native_tick(&mut self) {
        self.last_native_tick = Some(Instant::now());
        self.tick();
    }

    pub fn window_tick(&mut self) {
        if self
            .last_native_tick
            .is_none_or(|last| last.elapsed() >= Duration::from_millis(250))
        {
            self.tick();
        }
    }

    pub fn tick(&mut self) {
        self.events();
        self.cans.prune(&self.game);
        self.reserve_frame();
        self.infection_frame();
        self.ping_frame();
        self.director_frame();
        self.movement_frame();
        self.combat_frame();
        self.actions_frame();
        if !self.dedicated
            && let Some(player) = self.game.host()
        {
            finish_death(
                &self.game,
                &mut self.local,
                &mut self.cans,
                self.ammo.as_mut().zip(self.hooks.as_ref()),
                player,
            );
        }
        let Some(hooks) = &self.hooks else {
            return;
        };
        if let Some(ammo) = &mut self.ammo {
            ammo.place_due(&self.game, hooks, &self.effective.ammo_piles);
        }
        let mut disconnected = Vec::new();
        for (&user, guest) in &mut self.guests {
            if hooks.user(guest.edict) == Some(user) {
                finish_death(
                    &self.game,
                    &mut guest.session,
                    &mut self.cans,
                    self.ammo.as_mut().map(|ammo| (ammo, hooks)),
                    guest.player,
                );
            } else {
                disconnected.push(user);
            }
        }
        for user in disconnected {
            if let Some(mut guest) = self.guests.remove(&user) {
                let _ = guest.session.save();
                guest.session.actions = actions::Actions::default();
                guest.session.reserve = None;
                guest.session.death = None;
                if let Some(file) = &guest.session.file {
                    self.away.insert(file.clone(), guest.session);
                }
            }
        }
        if self.next_mutators.is_none_or(|next| Instant::now() >= next) {
            self.next_mutators = Some(Instant::now() + Duration::from_secs(2));
            let line = self.mutator_line();
            if !self.dedicated && self.game.host().is_some() {
                self.hud.push(line.clone());
            }
            if let Ok(line) = CString::new(line) {
                for guest in self.guests.values() {
                    hooks.print(guest.edict, &line);
                }
            }
        }
    }

    fn reserve_frame(&mut self) {
        if self
            .last_reserve_tick
            .is_some_and(|last| last.elapsed() < Duration::from_millis(100))
        {
            return;
        }
        self.last_reserve_tick = Some(Instant::now());
        let hooks = self.hooks.as_ref();
        let host = (!self.dedicated).then(|| self.game.host()).flatten();
        for index in 1..=64 {
            let player = self.game.entity(index);
            if player == 0
                || native::word(player).map(|table| table as usize) != Some(self.game.player_table)
                || !self.game.living(player)
            {
                continue;
            }
            self.game.retarget_ammo(player, &self.rules.catalog, hooks);
            let session = if Some(player) == host {
                Some(&mut self.local)
            } else {
                self.guests
                    .values_mut()
                    .find(|guest| guest.player == player)
                    .map(|guest| &mut guest.session)
            };
            if let Some(session) = session {
                reserve_capacity(&self.game, player, session, hooks);
            }
        }
    }

    pub fn level_shutdown(&mut self) {
        self.reset_movement(false);
        let _ = self.local.save();
        self.local.death = None;
        for guest in self.guests.values_mut() {
            let _ = guest.session.save();
            guest.session.death = None;
        }
        self.cans.clear();
        self.combat.reset();
        self.game.wear.numbers.borrow_mut().clear();
        self.pings.reset(&self.game, None);
    }

    pub fn take_hud(&mut self) -> Vec<String> {
        std::mem::take(&mut self.hud)
    }

    fn mutator_line(&self) -> String {
        use std::fmt::Write;

        let mut line = format!(
            "@mut@ {} {}|",
            self.generation,
            u8::from(self.rules.mutators.announce)
        );
        for (n, mutator) in self.mutators.iter().enumerate() {
            if n != 0 {
                line.push(';');
            }
            line.push_str(mutator.id());
            if *mutator == Mutator::Fog {
                let fog = &self.rules.mutators.fog;
                let density = (fog.density * 1000.0).round() as i32;
                let color = if fog.color.is_empty() {
                    "-"
                } else {
                    &fog.color
                };
                write!(line, " {density} {color}").unwrap();
            } else if *mutator == Mutator::Hunted {
                let kind = self
                    .rules
                    .mutators
                    .hunted
                    .class
                    .unwrap_or((self.generation % 6 + 1) as i32);
                write!(line, " {kind}").unwrap();
            }
        }
        line.push('\n');
        line
    }
}

fn player_file(directory: &Path, id: &str) -> Option<PathBuf> {
    if id.is_empty() || id.len() > 63 || matches!(id, "BOT" | "STEAM_ID_PENDING") {
        return None;
    }
    let safe: String = id
        .bytes()
        .map(|byte| {
            if byte.is_ascii_alphanumeric() {
                char::from(byte)
            } else {
                '_'
            }
        })
        .collect();
    Some(directory.join(format!("inventory_{safe}.inventory")))
}

fn finish_death(
    game: &Game,
    session: &mut Session,
    cans: &mut actions::Cans,
    ammo: Option<(&mut Ammo, &Hooks)>,
    player: usize,
) {
    if game.living(player) {
        return;
    }
    let Some(origin) = session.death.take() else {
        return;
    };
    let mut scattered = 0;
    for stack in session.inventory.slots() {
        let Some(id) = stack.item else {
            continue;
        };
        for number in 0..stack.count {
            let angle = scattered as f32 * 2.39996;
            let radius = (4.0 + scattered as f32 * 3.0).min(18.0);
            let position = Vector {
                x: origin.x + angle.cos() * radius,
                y: origin.y + angle.sin() * radius,
                z: origin.z + 16.0,
            };
            if let Some(entity) = game.create(
                session.inventory.catalog().item(id),
                &stack.weapon,
                position,
            ) && number == stack.count - 1
                && stack.spent > 0
            {
                cans.set(game, entity, stack.spent);
            }
            scattered += 1;
        }
    }
    if let Some((ammo, hooks)) = ammo {
        let mut reserve = game.ammo(player);
        let carried: Vec<_> = AmmoKind::ALL
            .into_iter()
            .filter(|kind| reserve[kind.definition().index] > 0)
            .collect();
        let step = std::f32::consts::TAU / carried.len().max(1) as f32;
        let radius = if carried.len() < 2 {
            28.0
        } else {
            (12.0 / (step / 2.0).sin()).max(28.0)
        };
        let mut changed = false;
        for (n, kind) in carried.into_iter().enumerate() {
            let angle = n as f32 * step;
            let position = Vector {
                x: origin.x + angle.cos() * radius,
                y: origin.y + angle.sin() * radius,
                z: origin.z + 16.0,
            };
            let index = kind.definition().index;
            if ammo.drop_at(game, hooks, position, kind, reserve[index]) {
                reserve[index] = 0;
                changed = true;
            }
        }
        if changed {
            game.restore(player, reserve);
            hooks.changed(player);
        }
    }
    session.inventory.clear();
}

fn reserve_capacity(game: &Game, player: usize, session: &mut Session, hooks: Option<&Hooks>) {
    let mut reserve = game.ammo(player);
    let old = session.reserve.replace((player, reserve));
    let catalog = session.inventory.catalog();
    let limit = session.inventory.limits().max_weight;
    let mut load = session.inventory.weight() - session.inventory.ammo_weight()
        + catalog.ammo_weight(&reserve);
    if let Some((_, previous)) = old.filter(|(actor, _)| *actor == player)
        && limit > 0.0
        && load > limit + 0.001
    {
        let mut changed = false;
        for kind in AmmoKind::ALL {
            let index = kind.definition().index;
            let raised = reserve[index].saturating_sub(previous[index]);
            let each = catalog.round_weight(kind);
            if raised <= 0 || each <= 0.0 {
                continue;
            }
            let trim = raised.min(((load - limit) / each - 0.0001).ceil() as i32);
            reserve[index] -= trim;
            load -= trim as f32 * each;
            changed |= trim > 0;
            session.notice = format!(
                "Too heavy: only {} {} fit",
                raised - trim,
                kind.definition().title
            );
            if load <= limit + 0.001 {
                break;
            }
        }
        if changed {
            game.restore(player, reserve);
            if let Some(hooks) = hooks {
                hooks.changed(player);
            }
            session.reserve = Some((player, reserve));
        }
    }
}

fn run_session(
    game: &Game,
    session: &mut Session,
    cans: &mut actions::Cans,
    ammo: Option<(&mut Ammo, &Hooks)>,
    pickups: (&mut pickups::Pickups, &MapGunRules, &mut Reload),
    player: usize,
    command: &str,
) -> Option<String> {
    if game.living(player)
        && let Some(previous) = session.carry_ammo.take()
    {
        let catalog = session.inventory.catalog();
        let mut carried = [false; 32];
        for (_, id) in game.weapons(player, catalog).into_iter().flatten() {
            if let Some(kind) = catalog.item(id).reserve_kind() {
                carried[kind.definition().index] = true;
            }
        }
        let mut merged = game.ammo(player);
        for kind in AmmoKind::ALL {
            let index = kind.definition().index;
            if !carried[index] {
                merged[index] = previous[index].min(catalog.ammo_max(kind));
            }
        }
        game.restore(player, merged);
    }
    let mut runner = Run {
        game,
        inventory: &mut session.inventory,
        cans,
        ammo,
        pickups,
    };
    let (native, mut notice) = runner.run(player, command)?;
    let pending = std::mem::take(&mut session.notice);
    if notice.is_empty() {
        notice = pending;
    }
    if let Err(error) = session.save() {
        notice = format!("Could not save the inventory ({error})");
    }
    Some(encode_remote_state(&session.inventory, &native, &notice))
}

struct Run<'a> {
    game: &'a Game,
    inventory: &'a mut Inventory,
    cans: &'a mut actions::Cans,
    ammo: Option<(&'a mut Ammo, &'a Hooks)>,
    pickups: (&'a mut pickups::Pickups, &'a MapGunRules, &'a mut Reload),
}

impl Run<'_> {
    fn run(
        &mut self,
        player: usize,
        request: &str,
    ) -> Option<([NativeWeapon; NATIVE_SLOTS], String)> {
        let mut fields = request.split_ascii_whitespace();
        let name = fields.next()?;
        if !name.starts_with("inv_") {
            return None;
        }
        let argument = fields.next();
        let numeric = argument.and_then(|slot| slot.parse::<usize>().ok());
        let amount = fields
            .next()
            .and_then(|slot| slot.parse::<i32>().ok())
            .unwrap_or(1);
        let mut native = self.refresh(player);
        let notice = if name == "inv_move" {
            if let (Some(from), Ok(to)) = (numeric, usize::try_from(amount)) {
                self.inventory.move_slot(from, to);
            }
            String::new()
        } else if name != "inv_sync" && self.game.available(player) {
            let message = match name {
                "inv_store" => self.store(player),
                "inv_pickup" => self.pickup(player),
                "inv_use" => numeric
                    .map(|index| self.take(player, index))
                    .unwrap_or_default(),
                "inv_drop" => numeric
                    .map(|index| self.drop_slot(player, index, amount))
                    .unwrap_or_default(),
                "inv_dropweapon" => self.drop_native(player, argument.unwrap_or_default()),
                "inv_select" => {
                    let classname = argument.unwrap_or_default();
                    self.put_away_carried(player, classname);
                    self.game.select(player, classname);
                    String::new()
                }
                "inv_unload" => self.unload(player),
                "inv_dropammo" => argument
                    .and_then(AmmoKind::from_name)
                    .map(|kind| self.drop_rounds(player, kind, amount))
                    .unwrap_or_default(),
                _ => String::new(),
            };
            native = self.refresh(player);
            message
        } else if name == "inv_sync" {
            String::new()
        } else {
            "You cannot do that right now".into()
        };
        Some((native, notice))
    }

    fn refresh(&mut self, player: usize) -> [NativeWeapon; NATIVE_SLOTS] {
        let native = self.game.native(player, self.inventory.catalog());
        if self.pickups.1.enabled {
            for (weapon, _) in self
                .game
                .weapons(player, self.inventory.catalog())
                .into_iter()
                .take(2)
                .flatten()
            {
                self.pickups.0.remember(self.game, weapon);
            }
        }
        self.game.retarget_ammo(
            player,
            self.inventory.catalog(),
            self.ammo.as_ref().map(|(_, hooks)| *hooks),
        );
        let rounds = self
            .inventory
            .catalog()
            .ammo_weight(&self.game.ammo(player));
        reconcile_native_inventory(self.inventory, &native, rounds);
        native
    }

    fn store(&mut self, player: usize) -> String {
        let catalog = self.inventory.catalog();
        let active = self
            .game
            .active(player)
            .and_then(|weapon| Some((weapon, self.game.item(weapon, catalog)?)));
        let Some((weapon, id)) = active.or_else(|| {
            self.game
                .weapons(player, catalog)
                .into_iter()
                .flatten()
                .find(|&(_, id)| !catalog.item(id).is_weapon())
        }) else {
            return "Nothing to store".into();
        };
        let def = self.inventory.catalog().item(id).clone();
        if self.store_weapon(player, weapon, id, &def) {
            format!("Stored {}", def.name)
        } else {
            format!("Could not store {}", def.name)
        }
    }

    fn store_weapon(&mut self, player: usize, weapon: usize, id: ItemId, def: &ItemDef) -> bool {
        let mut next = self.inventory.clone();
        if def.is_weapon() {
            next.set_weapons_weight((next.weapons_weight() - def.weight).max(0.0));
            if next
                .add_weapon(id, self.game.state(weapon, def, false), None)
                .is_none()
            {
                return false;
            }
        } else {
            let spent = self.cans.spent(self.game, weapon);
            if let Some(home) = next.held_slot(id) {
                next.release_held(home, spent);
            } else if next.add(id, 1, spent) == 0 {
                return false;
            }
        }
        if !self
            .game
            .release(player, weapon, def, self.inventory.catalog(), true)
        {
            return false;
        }
        if def.fuel > 0 {
            self.cans.set(self.game, weapon, 0);
        }
        self.game.remove(weapon);
        *self.inventory = next;
        true
    }

    fn pickup(&mut self, player: usize) -> String {
        let catalog = self.inventory.catalog();
        let Some((entity, id)) = self.game.look_for(player, catalog, 0.6, |_| true) else {
            return "Nothing to pick up in front of you".into();
        };
        let def = catalog.item(id).clone();
        self.pickups.0.cut(self.game, entity, &def, self.pickups.1);
        if def.is_weapon() {
            let mut state = self.game.state(entity, &def, true);
            let ammo = state.ammo;
            state.ammo = -1;
            if self.inventory.add_weapon(id, state, None).is_none() {
                return format!("Could not pick up {}", def.name);
            }
            self.add_reserve(player, def.ammo, ammo);
        } else if self
            .inventory
            .add(id, 1, self.cans.spent(self.game, entity))
            != 1
        {
            return format!("Could not pick up {}", def.name);
        }
        if def.fuel > 0 {
            self.cans.set(self.game, entity, 0);
        }
        self.game.remove(entity);
        format!("Picked up {}", def.name)
    }

    fn add_reserve(&self, player: usize, kind: Option<AmmoKind>, rounds: i32) {
        if let Some(kind) = kind.filter(|_| rounds > 0) {
            let index = kind.definition().index;
            let cap = self.inventory.catalog().ammo_max(kind);
            let previous = self.game.ammo(player)[index];
            let next = previous.saturating_add(rounds).min(cap);
            if next > previous {
                native::store_word(player + RESERVE_POOL + 4 * index, next as u32);
            }
        }
    }

    fn take(&mut self, player: usize, slot: usize) -> String {
        let Some(stack) = self
            .inventory
            .slot(slot)
            .filter(|stack| !stack.is_empty())
            .cloned()
        else {
            return String::new();
        };
        let id = stack.item.unwrap();
        let def = self.inventory.catalog().item(id).clone();
        self.put_away_carried(player, def.classname);
        let carried =
            self.game.weapons(player, self.inventory.catalog())[slot_index(def.game_slot)];
        if !def.is_weapon() {
            if let Some((weapon, current)) = carried {
                if current == id {
                    self.game.select(player, def.classname);
                    return String::new();
                }
                let other = self.inventory.catalog().item(current).clone();
                if !self.store_weapon(player, weapon, current, &other) {
                    return format!("Your hands are full: no room for {}", def.name);
                }
            }
            if stack.count <= 0 {
                return String::new();
            }
            let Some(given) = self
                .game
                .give(player, def.classname, &def, self.inventory.catalog())
            else {
                return format!("The game refused the {}", def.name);
            };
            let spent = self.inventory.next_spent(slot);
            if spent > 0 {
                self.cans.set(self.game, given, spent);
            }
            self.inventory.remove_from_slot(slot, 1);
            self.inventory.set_held(slot, id);
            self.game.select(player, def.classname);
            return String::new();
        }

        let previous = self.inventory.clone();
        self.inventory.remove_from_slot(slot, 1);
        // Snapshot the outgoing gun's state before releasing it from its game slot.
        let mut released = None;
        if let Some((weapon, old)) = carried {
            let old_def = self.inventory.catalog().item(old).clone();
            self.inventory
                .set_weapons_weight((self.inventory.weapons_weight() - old_def.weight).max(0.0));
            if self
                .inventory
                .add_weapon(old, self.game.state(weapon, &old_def, false), Some(slot))
                .is_none()
                || !self
                    .game
                    .release(player, weapon, &old_def, self.inventory.catalog(), true)
            {
                *self.inventory = previous;
                return format!("Could not put away the {}", old_def.name);
            }
            released = Some((weapon, old));
        }
        let before = self.game.ammo(player);
        let name = stack.weapon.give_name(def.classname);
        let given = self.game.give(player, name, &def, self.inventory.catalog());
        self.game.restore(player, before);
        let Some(given) = given else {
            if let Some((weapon, old)) = released {
                // Leave the original on the ground until its replacement equips.
                let original = self.inventory.catalog().item(old);
                let state = &self.inventory.slots()[slot].weapon;
                let name = state.give_name(original.classname);
                if let Some(equipped) =
                    self.game
                        .give(player, name, original, self.inventory.catalog())
                {
                    self.game.apply(equipped, original, state, false);
                    self.game.remove(weapon);
                }
                self.game.restore(player, before);
            }
            *self.inventory = previous;
            return format!("The game refused the {}", def.name);
        };
        if let Some((weapon, _)) = released {
            self.game.remove(weapon);
        }
        self.game.apply(given, &def, &stack.weapon, false);
        self.pickups.0.remember(self.game, given);
        self.add_reserve(player, def.ammo, stack.weapon.ammo);
        self.game.select(player, def.classname);
        String::new()
    }

    fn put_away_carried(&mut self, player: usize, selected: &str) {
        let Some(weapon) = self.game.active(player) else {
            return;
        };
        let Some(id) = self.game.item(weapon, self.inventory.catalog()) else {
            return;
        };
        let def = self.inventory.catalog().item(id);
        if def.game_slot == GameSlot::Carry && def.classname != selected {
            let def = def.clone();
            self.store_weapon(player, weapon, id, &def);
        }
    }

    fn drop_native(&mut self, player: usize, name: &str) -> String {
        let catalog = self.inventory.catalog();
        let Some(id) = catalog.find_item(name) else {
            return String::new();
        };
        let def = catalog.item(id);
        let Some((weapon, actual)) = self.game.weapons(player, catalog)[slot_index(def.game_slot)]
        else {
            return String::new();
        };
        if actual == id && self.game.release(player, weapon, def, catalog, false) {
            format!("Dropped {}", def.name)
        } else {
            "The game does not let you drop now".into()
        }
    }

    fn drop_slot(&mut self, player: usize, index: usize, count: i32) -> String {
        let Some(stack) = self
            .inventory
            .slot(index)
            .filter(|stack| !stack.is_empty())
            .cloned()
        else {
            return String::new();
        };
        let def = self.inventory.catalog().item(stack.item.unwrap()).clone();
        if stack.count == 0 {
            return self.drop_native(player, def.classname);
        }
        let Some((eye, forward)) = self.game.eyes(player) else {
            return String::new();
        };
        let mut removed = 0;
        for n in 0..stack.count.min(count.max(1)) {
            let position = eye.ahead(forward, 48.0 + 6.0 * n as f32);
            let Some(entity) = self.game.create(&def, &stack.weapon, position) else {
                break;
            };
            if def.is_weapon() && self.pickups.1.enabled {
                self.pickups.0.remember(self.game, entity);
            }
            let spent = self.inventory.next_spent(index);
            if spent > 0 {
                self.cans.set(self.game, entity, spent);
            }
            self.inventory.remove_from_slot(index, 1);
            removed += 1;
        }
        if removed == 0 {
            format!("Could not drop {}", def.name)
        } else {
            format!("Dropped {} ×{removed}", def.name)
        }
    }

    fn unload(&mut self, player: usize) -> String {
        let catalog = self.inventory.catalog();
        let unloadable = |def: &ItemDef| def.ammo.is_some_and(AmmoKind::is_unloadable);
        let ground = self.game.look_ranked(player, 0.9, |entity| {
            let id = self.game.item(entity, catalog)?;
            (unloadable(catalog.item(id)) && native::word(entity + WEAPON_OWNER) == Some(u32::MAX))
                .then_some(id)
        });
        let spawner = self.game.look_ranked(player, 0.9, |entity| {
            let id = self.pickups.0.spawner_item(self.game, entity, catalog)?;
            unloadable(catalog.item(id)).then_some(id)
        });
        let ground =
            match spawner.filter(|(score, _, _)| ground.is_none_or(|ground| *score < ground.0)) {
                Some((_, spawner, id)) => self.gun_from_spawner(spawner, id).map(|gun| (gun, id)),
                None => ground.map(|(_, gun, id)| (gun, id)),
            };
        let gun = ground.or_else(|| {
            self.game.active(player).and_then(|gun| {
                self.game
                    .item(gun, self.inventory.catalog())
                    .map(|id| (gun, id))
            })
        });
        let Some((gun, id)) = gun else {
            return "No gun to unload".into();
        };
        let catalog = self.inventory.catalog();
        let def = catalog.item(id);
        let title = def.name;
        let Some(kind) = def.ammo.filter(|kind| kind.is_unloadable()) else {
            return "No gun to unload".into();
        };
        let rounds = native::word(gun + CLIP).unwrap_or_default() as i32;
        let carried = self.game.active(player) == Some(gun);
        let mut reloading = [0];
        if carried && native::read(gun + 0x144d, &mut reloading) && reloading[0] != 0 {
            return "Wait for the reload to finish".into();
        }
        if rounds <= 0 {
            return format!("{title} is empty");
        }
        let limit = self.inventory.limits().max_weight;
        let room = (limit > 0.0).then(|| (limit - self.inventory.weight()).max(0.0));
        let index = kind.definition().index;
        let before = self.game.ammo(player)[index];
        let split = catalog.split_unload(Some(kind), rounds, before, room);
        if split.to_ground > 0 && !self.spawn_rounds(player, kind, split.to_ground) {
            return "The overflow could not be dropped".into();
        }
        if !native::store_word(gun + CLIP, 0) {
            return format!("Could not unload {title}");
        }
        native::store_word(
            gun + UPGRADES,
            native::word(gun + UPGRADES).unwrap_or_default() & !3,
        );
        native::store_word(gun + SPECIAL_ROUNDS, 0);
        if carried {
            self.pickups.2.mark(player, gun);
        } else {
            self.pickups.0.remember(self.game, gun);
        }
        self.add_reserve(player, Some(kind), split.to_reserve);
        if let Some((_, hooks)) = &self.ammo {
            hooks.changed(gun);
            hooks.changed(player);
        }
        let title = kind.definition().title;
        if split.to_ground > 0 {
            format!(
                "Unloaded {rounds} {title} ({} dropped: no room)",
                split.to_ground
            )
        } else {
            format!("Unloaded {rounds} {title}")
        }
    }

    fn gun_from_spawner(&mut self, spawner: usize, id: ItemId) -> Option<usize> {
        const SPAWN_COUNT: usize = 0x1444;
        let origin = Vector::read(spawner + ENTITY_ORIGIN)?;
        let angles = Vector::read(spawner + 0x394).unwrap_or_default();
        let count = native::word(spawner + SPAWN_COUNT)? as i32;
        let rules = self.pickups.1;
        let stays = count != 1 && !(rules.enabled && rules.spawners_once);
        let at = if stays {
            let (sin, cos) = (angles.y + 90.0).to_radians().sin_cos();
            let position = Vector {
                x: origin.x + 24.0 * cos,
                y: origin.y + 24.0 * sin,
                z: origin.z + 16.0,
            };
            self.ammo
                .as_ref()
                .map_or(position, |(ammo, _)| ammo.ground(self.game, position))
        } else {
            origin
        };
        let def = self.inventory.catalog().item(id);
        let gun = self.game.spawn_item(def, None, at, angles)?;
        self.pickups.0.cut(self.game, gun, def, rules);
        self.pickups.0.remember(self.game, gun);
        if !stays {
            self.game.remove(spawner);
        } else if count > 1 {
            native::store_word(spawner + SPAWN_COUNT, (count - 1) as u32);
        }
        Some(gun)
    }

    fn spawn_rounds(&mut self, player: usize, kind: AmmoKind, rounds: i32) -> bool {
        let Some((ammo, hooks)) = &mut self.ammo else {
            return false;
        };
        let Some((eye, forward)) = self.game.eyes(player) else {
            return false;
        };
        ammo.drop_at(self.game, hooks, eye.ahead(forward, 48.0), kind, rounds)
    }

    fn drop_rounds(&mut self, player: usize, kind: AmmoKind, requested: i32) -> String {
        let entry = kind.definition();
        let total = self.game.ammo(player)[entry.index];
        let rounds = if requested <= 0 {
            total
        } else {
            total.min(requested)
        };
        if rounds <= 0 {
            return format!("No {} to drop", entry.title);
        }
        if !self.spawn_rounds(player, kind, rounds) {
            return format!("Could not drop {}", entry.title);
        }
        native::store_word(
            player + RESERVE_POOL + 4 * entry.index,
            (total - rounds) as u32,
        );
        format!("Dropped {rounds} {}", entry.title)
    }
}
