use std::{
    collections::HashSet,
    ffi::{c_char, c_void},
    io,
    ptr::NonNull,
    sync::Arc,
    sync::atomic::{AtomicUsize, Ordering},
    time::{SystemTime, UNIX_EPOCH},
};

use inventory_mod::{
    inventory::{WeaponState, melee_script_name},
    items::{Catalog, ItemDef, ItemId},
    remote_state::reconcile_native_inventory,
    rules::MapGunRules,
};

use super::{
    AMMO_TYPE, CLIP, ENTITY_ORIGIN, EXTRA_AMMO, Game, Host, RESERVE_POOL, Vector, WEAPON_OWNER,
    address, ammo::handle, slot_index,
};
use crate::{
    client::with_client,
    dedicated,
    native::{self, Detour},
};

type Bump = unsafe extern "thiscall" fn(*mut c_void, *mut c_void) -> bool;
type Use = unsafe extern "thiscall" fn(*mut c_void, *mut c_void, *mut c_void, i32, f32);
type WeaponId = unsafe extern "thiscall" fn(*mut c_void) -> i32;
type IdName = unsafe extern "C" fn(i32) -> *const c_char;
type MeleeInfo = unsafe extern "thiscall" fn(*mut c_void, u32) -> *const c_void;

static BUMP: AtomicUsize = AtomicUsize::new(0);
static WORLD: AtomicUsize = AtomicUsize::new(0);
static SPAWN: AtomicUsize = AtomicUsize::new(0);

pub struct Pickups {
    patches: [Option<Detour>; 3],
    weapon_name: Option<IdName>,
    spawn_use: Option<usize>,
    known: HashSet<u32>,
    random: u32,
    pub auto_store: bool,
}

impl Pickups {
    pub fn install(game: &Game, auto_store: bool) -> Self {
        let base = game.image.base;
        let updated = game.image.timestamp == 0x6afc_cca0;
        let (guns, spawners) = if updated {
            (0x6d559c, 0x67973c)
        } else {
            (0x6d5544, 0x6796e4)
        };
        let target =
            |table: usize, slot: usize| native::word(table + 4 * slot).unwrap_or_default() as usize;
        let bump = install(
            target(game.player_table, 426),
            bump as Bump as usize,
            &BUMP,
            "55 8B EC 83 EC 08 53 57 8B 7D 08",
        );
        let world = install(
            target(base + guns, 106),
            world_use as Use as usize,
            &WORLD,
            "55 8B EC 83 EC 08 56 57 8B 7D 08 8B F1 85 FF",
        );
        let spawn_entry = target(base + spawners, 106);
        let spawn = install(
            spawn_entry,
            spawn_use as Use as usize,
            &SPAWN,
            "55 8B EC 83 EC 2C 53 57 8B 7D 08 8B D9",
        );
        let weapon_name = native::find_pattern(
            base + 0x24e900,
            0x700,
            "55 8B EC 8B 45 08 8D 48 FF 83 F9 36",
        )
        .map(|at| unsafe { std::mem::transmute::<usize, IdName>(at) });
        let random = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|stamp| stamp.as_nanos() as u32)
            .unwrap_or_default()
            .max(1);
        Self {
            spawn_use: spawn.as_ref().map(|_| spawn_entry),
            patches: [bump, world, spawn],
            weapon_name,
            known: HashSet::new(),
            random,
            auto_store,
        }
    }

    pub fn reset(&mut self) {
        self.known.clear();
    }

    pub fn stop(&mut self) -> io::Result<()> {
        for (patch, original) in self.patches.iter_mut().zip([&BUMP, &WORLD, &SPAWN]) {
            if let Some(patch) = patch {
                patch.disable()?;
                original.store(0, Ordering::Release);
            }
        }
        Ok(())
    }

    fn choose(&mut self, low: i32, high: i32) -> i32 {
        self.random ^= self.random << 13;
        self.random ^= self.random >> 17;
        self.random ^= self.random << 5;
        low + (self.random % (high - low + 1) as u32) as i32
    }

    pub fn remember(&mut self, game: &Game, weapon: usize) {
        if let Some(handle) = handle(game, weapon) {
            self.known.insert(handle);
        }
    }

    pub fn cut(&mut self, game: &Game, weapon: usize, def: &ItemDef, rules: &MapGunRules) -> bool {
        if !rules.enabled || !Game::firearm(def) || def.ammo.is_none() {
            return false;
        }
        if handle(game, weapon).is_some_and(|handle| !self.known.insert(handle)) {
            return false;
        }
        let percent = self.choose(rules.min_clip, rules.max_clip);
        let clip = native::word(weapon + CLIP)
            .map(|clip| clip as i32)
            .unwrap_or_default();
        if clip > 0 {
            native::store_word(
                weapon + CLIP,
                (f64::from(clip) * f64::from(percent) / 100.0).round() as u32,
            );
        }
        native::store_word(weapon + EXTRA_AMMO, 0);
        true
    }

    fn spawner(&self, entity: usize, catalog: &Catalog) -> Option<ItemId> {
        let method = native::vslot(entity, 240)?;
        let get_id = unsafe { std::mem::transmute::<usize, WeaponId>(method) };
        let get_name = self.weapon_name?;
        let id = unsafe { get_id(address(entity)) };
        if id <= 0 {
            return None;
        }
        let raw = unsafe { get_name(id) };
        let name = native::string(raw.addr(), 64)?;
        catalog.find_item(&name)
    }

    fn melee_spawn_state(&self, game: &Game, spawner: usize) -> Option<WeaponState> {
        let melee = spawner_melee_name(game, spawner)
            .and_then(|name| melee_script_name(&name).map(str::to_owned))
            .or_else(|| {
                let name = game.key(spawner, c"melee_weapon")?;
                melee_script_name(&name).map(str::to_owned)
            })?;
        Some(WeaponState {
            melee,
            ..WeaponState::default()
        })
    }

    pub(super) fn spawner_item(
        &self,
        game: &Game,
        entity: usize,
        catalog: &Catalog,
    ) -> Option<ItemId> {
        let classname = game.classname(entity)?;
        let spawned = classname.strip_suffix("_spawn")?;
        if let Some(weapon) = catalog.find_item(spawned) {
            return Some(weapon);
        }
        // Configurable spawners resolve their choice at runtime, so their
        // classname does not identify which gun they give.
        if self.spawn_use.is_some() && native::vslot(entity, 106) == self.spawn_use {
            self.spawner(entity, catalog)
        } else {
            None
        }
    }
}

fn spawner_melee_name(game: &Game, spawner: usize) -> Option<String> {
    // CWeaponMeleeSpawn::GiveWeapon resolves its selected token at +0x1460
    // in the melee database, then passes the record's name to GiveWeapon.
    let give = native::vslot(spawner, 244).filter(|&at| game.image.includes(at, 0x60))?;
    let call = native::find_pattern(
        give,
        0x60,
        "8B 86 60 14 00 00 50 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 74 ?? 05 50 0C 00 00",
    )?;
    let database = native::word(call + 8)? as usize;
    let lookup = super::relative(call + 12)?;
    if !game.image.includes(database, 4)
        || !game.image.includes(lookup, 12)
        || !native::matches_pattern(lookup, "55 8B EC 83 EC 08 8B 45 08 56 8B F1")
    {
        return None;
    }
    let lookup = unsafe { std::mem::transmute::<usize, MeleeInfo>(lookup) };
    let token = native::word(spawner + 0x1460)?;
    let record = NonNull::new(unsafe { lookup(address(database), token) }.cast_mut())?;
    native::string(record.as_ptr().addr() + 0xc50, 64)
}

#[derive(Clone, Copy)]
enum Kind {
    Weapon,
    World,
    Spawner,
}

enum Before {
    Pass,
    Stored,
    Refused,
    Track(Pending),
}

#[derive(Clone, Copy)]
struct Pending {
    player: usize,
    weapon: usize,
    item: ItemId,
    previous: Option<usize>,
    reserves: [i32; 32],
    fresh: bool,
    extra: i32,
    spawner: bool,
}

impl Host {
    fn before(&mut self, player: usize, weapon: usize, mode: Kind) -> Before {
        if !self.game.living(player) {
            return Before::Pass;
        }
        let spawner = matches!(mode, Kind::Spawner);
        let catalog = Arc::clone(&self.rules.catalog);
        let item = if spawner {
            self.pickups.spawner_item(&self.game, weapon, &catalog)
        } else {
            self.game.item(weapon, &catalog)
        };
        let Some(item) = item else {
            return Before::Pass;
        };
        let def = catalog.item(item);
        let existing = self.game.weapons(player, &catalog)[slot_index(def.game_slot)];
        let rules = &self.effective.map_guns;
        let fresh = !spawner && self.pickups.cut(&self.game, weapon, def, rules);
        let local = self.is_local_player(player);
        let session = if local {
            Some(&mut self.local)
        } else {
            native::word(player + 0x28)
                .and_then(|edict| self.hooks.as_ref()?.user(edict as usize))
                .and_then(|id| self.guests.get_mut(&id))
                .map(|guest| &mut guest.session)
        };
        if let Some(session) = session {
            let native = self.game.native(player, &catalog);
            reconcile_native_inventory(
                &mut session.inventory,
                &native,
                catalog.ammo_weight(&self.game.ammo(player)),
            );
            let wants = existing.is_some() || (!def.is_weapon() && self.pickups.auto_store);
            if wants && session.inventory.has_box_for(item) && session.inventory.room_for(item) >= 1
            {
                let candidate = if spawner {
                    let state = if Game::matches_melee(def) {
                        self.pickups.melee_spawn_state(&self.game, weapon)
                    } else {
                        Some(WeaponState::default())
                    };
                    state
                        .zip(Vector::read(weapon + ENTITY_ORIGIN))
                        .and_then(|(state, at)| {
                            let candidate = self.game.create(
                                def,
                                &state,
                                Vector {
                                    z: at.z + 12.0,
                                    ..at
                                },
                            )?;
                            if !state.melee.is_empty()
                                && Game::melee_script(candidate) != state.melee
                            {
                                self.game.remove(candidate);
                                return None;
                            }
                            Some(candidate)
                        })
                } else if native::word(weapon + WEAPON_OWNER) == Some(u32::MAX) {
                    Some(weapon)
                } else {
                    None
                };
                if let Some(candidate) = candidate {
                    if spawner {
                        self.pickups.cut(&self.game, candidate, def, rules);
                    }
                    let stored = if def.is_weapon() {
                        let mut state = self.game.state(candidate, def, true);
                        let spare = state.ammo;
                        state.ammo = -1;
                        if session.inventory.add_weapon(item, state, None).is_some() {
                            add_rounds(&self.game, player, def, &catalog, spare);
                            true
                        } else {
                            false
                        }
                    } else {
                        let spent = self.cans.spent(&self.game, candidate);
                        session.inventory.add(item, 1, spent) == 1
                    };
                    if stored {
                        if def.fuel > 0 {
                            self.cans.set(&self.game, candidate, 0);
                        }
                        if matches!(mode, Kind::Weapon)
                            && let Some(owner) = handle(&self.game, player)
                        {
                            native::store_word(candidate + WEAPON_OWNER, owner);
                        }
                        self.game.remove(candidate);
                        if spawner {
                            let count = native::word(weapon + 0x1444).unwrap_or_default();
                            if count == 1 || def.is_weapon() && rules.enabled && rules.spawners_once
                            {
                                self.game.remove(weapon);
                            } else if count > 1 {
                                native::store_word(weapon + 0x1444, count - 1);
                            }
                        }
                        session.notice = format!("Stored {}", def.name);
                        let _ = session.save();
                        return Before::Stored;
                    }
                    if spawner {
                        self.game.remove(candidate);
                    }
                }
            }
            let maximum = session.inventory.limits().max_weight;
            let displaced = existing.map_or(0.0, |(_, existing)| catalog.item(existing).weight);
            if maximum > 0.0 && session.inventory.weight() - displaced + def.weight > maximum + 1e-3
            {
                session.notice = format!("Too heavy to pick up {}", def.name);
                return Before::Refused;
            }
        }
        if def.is_weapon() {
            let extra = if !spawner && Game::firearm(def) {
                native::word(weapon + EXTRA_AMMO).unwrap_or_default() as i32
            } else {
                0
            };
            Before::Track(Pending {
                player,
                weapon,
                item,
                previous: existing.map(|(entity, _)| entity),
                reserves: self.game.ammo(player),
                fresh,
                extra,
                spawner,
            })
        } else {
            Before::Pass
        }
    }

    fn after(&mut self, pending: Pending) {
        let catalog = &self.rules.catalog;
        let item = catalog.item(pending.item);
        let incoming = self.game.weapons(pending.player, catalog)[slot_index(item.game_slot)]
            .map(|(weapon, _)| weapon)
            .filter(|&weapon| Some(weapon) != pending.previous);
        if let Some(weapon) = incoming {
            let rules = &self.effective.map_guns;
            let fresh = pending.fresh
                || (pending.spawner && self.pickups.cut(&self.game, weapon, item, rules));
            self.pickups.remember(&self.game, weapon);
            if let Some(ammo) = item.ammo.filter(|_| Game::firearm(item)) {
                let index = ammo.definition().index;
                let before = pending.reserves[index];
                let mut after = self.game.ammo(pending.player)[index];
                if fresh {
                    let magazines = self.pickups.choose(rules.min_reserve, rules.max_reserve);
                    after =
                        after.min(before.saturating_add(magazines * ammo.definition().magazine));
                } else if pending.extra > 0 {
                    after = after.max(
                        before
                            .saturating_add(pending.extra)
                            .min(catalog.ammo_max(ammo)),
                    );
                }
                native::store_word(
                    pending.player + RESERVE_POOL + 4 * index,
                    before.max(after) as u32,
                );
                if native::word(weapon + AMMO_TYPE) == Some(8) {
                    native::store_word(weapon + AMMO_TYPE, 7);
                }
            }
            if pending.spawner && rules.enabled && rules.spawners_once {
                self.game.remove(pending.weapon);
            }
        }
    }
}

fn add_rounds(game: &Game, player: usize, def: &ItemDef, catalog: &Catalog, amount: i32) {
    if let Some(kind) = def.ammo.filter(|_| amount > 0) {
        let index = kind.definition().index;
        let before = game.ammo(player)[index];
        let next = before.saturating_add(amount).min(catalog.ammo_max(kind));
        if next > before {
            native::store_word(player + RESERVE_POOL + 4 * index, next as u32);
        }
    }
}

fn on_host<T>(mut f: impl FnMut(&mut Host) -> T) -> Option<T> {
    with_client(|client| client.host.as_mut().map(&mut f))
        .flatten()
        .or_else(|| dedicated::with_host(f))
}

fn before(player: usize, weapon: usize, mode: Kind) -> Before {
    if player == 0 || weapon == 0 {
        return Before::Pass;
    }
    std::panic::catch_unwind(|| {
        on_host(|host| host.before(player, weapon, mode)).unwrap_or(Before::Pass)
    })
    .unwrap_or(Before::Pass)
}

fn after(prepared: Before) {
    if let Before::Track(pending) = prepared {
        let _ = std::panic::catch_unwind(|| on_host(|host| host.after(pending)));
    }
}

fn install(at: usize, hook: usize, original: &AtomicUsize, pattern: &str) -> Option<Detour> {
    if !native::matches_pattern(at, pattern) {
        return None;
    }
    let mut detour = Detour::prepare(at, hook).ok()?;
    original.store(detour.trampoline(), Ordering::Release);
    if detour.enable().is_err() {
        original.store(0, Ordering::Release);
        return None;
    }
    Some(detour)
}

unsafe extern "thiscall" fn bump(player: *mut c_void, weapon: *mut c_void) -> bool {
    let prepared = before(player.addr(), weapon.addr(), Kind::Weapon);
    match prepared {
        Before::Stored => return true,
        Before::Refused => return false,
        _ => {}
    }
    let original = BUMP.load(Ordering::Acquire);
    if original == 0 {
        return false;
    }
    let call = unsafe { std::mem::transmute::<usize, Bump>(original) };
    let result = unsafe { call(player, weapon) };
    after(prepared);
    result
}

unsafe extern "thiscall" fn world_use(
    weapon: *mut c_void,
    player: *mut c_void,
    caller: *mut c_void,
    mode: i32,
    value: f32,
) {
    unsafe { use_weapon(weapon, player, caller, mode, value, Kind::World, &WORLD) };
}

unsafe extern "thiscall" fn spawn_use(
    weapon: *mut c_void,
    player: *mut c_void,
    caller: *mut c_void,
    mode: i32,
    value: f32,
) {
    unsafe { use_weapon(weapon, player, caller, mode, value, Kind::Spawner, &SPAWN) };
}

unsafe fn use_weapon(
    weapon: *mut c_void,
    player: *mut c_void,
    caller: *mut c_void,
    mode: i32,
    value: f32,
    kind: Kind,
    original: &AtomicUsize,
) {
    let prepared = before(player.addr(), weapon.addr(), kind);
    if matches!(prepared, Before::Stored | Before::Refused) {
        return;
    }
    let original = original.load(Ordering::Acquire);
    if original != 0 {
        let call = unsafe { std::mem::transmute::<usize, Use>(original) };
        unsafe { call(weapon, player, caller, mode, value) };
    }
    after(prepared);
}
