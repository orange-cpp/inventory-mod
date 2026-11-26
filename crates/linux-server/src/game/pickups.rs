use std::{cell::Cell, collections::HashMap};

use inventory_mod::{
    inventory::{Inventory, WeaponState},
    items::{Catalog, ItemDef},
    remote_state::reconcile_native_inventory,
    rules::MapGunRules,
};

use super::{
    Ent, Game, Reserve, field, firearm, melee, native::*, set_field, slot_index,
    vtable::VtablePatch,
};
use crate::{
    plugin,
    source::{self, Entity, Source},
};

pub type Use = unsafe extern "C" fn(*mut Entity, *mut Entity, *mut Entity, i32, f32);
type Bump = unsafe extern "C" fn(*mut Entity, *mut Entity) -> bool;

const BUMP_RVA: usize = 0xd0b0f4;
pub(super) const USE_SLOT: usize = 107;

thread_local! {
    static BUMP: Cell<Option<Bump>> = const { Cell::new(None) };
    static WORLD_USE: Cell<Option<Use>> = const { Cell::new(None) };
    static SPAWNER_USE: Cell<Option<Use>> = const { Cell::new(None) };
}

#[derive(Default)]
pub struct Pickups {
    auto_store: bool,
    bump: Option<VtablePatch>,
    use_slots: HashMap<usize, VtablePatch>,
}

pub struct PickupContext<'a> {
    pub source: &'a Source,
    pub catalog: &'a Catalog,
    pub rules: &'a MapGunRules,
    pub inventory: Option<&'a mut Inventory>,
    pub notice: Option<String>,
}

#[derive(Clone, Copy)]
pub enum PickupCall {
    Bump,
    Use {
        caller: *mut Entity,
        mode: i32,
        value: f32,
        spawner: bool,
    },
}

impl Game {
    pub fn install_pickups(&mut self, auto_store: bool) -> bool {
        let base = self.native.base;
        self.pickups.bump = unsafe {
            VtablePatch::install(
                base,
                base + BUMP_RVA,
                self.native.bump_weapon as usize,
                bump as Bump as usize,
            )
        };
        if self.pickups.bump.is_none() {
            return false;
        }
        self.pickups.auto_store = auto_store;
        BUMP.set(Some(self.native.bump_weapon));
        WORLD_USE.set(Some(self.native.gun_use));
        SPAWNER_USE.set(Some(self.native.weapon_spawn_use));
        true
    }

    pub fn uninstall_pickups(&mut self) {
        self.pickups = Pickups::default();
        BUMP.set(None);
        WORLD_USE.set(None);
        SPAWNER_USE.set(None);
    }

    pub fn scan_pickups(&mut self, source: &Source) {
        if self.pickups.bump.is_none() {
            return;
        }
        let base = self.native.base;
        for entity in source.entities() {
            if !self.classname(entity).starts_with("weapon_") {
                continue;
            }
            let table = unsafe { field::<usize>(entity, 0) };
            let address = table.wrapping_add(USE_SLOT * size_of::<usize>());
            if self.pickups.use_slots.contains_key(&address) {
                continue;
            }
            let Some(slot) = VtablePatch::checked_slot(base, address) else {
                continue;
            };
            let original = unsafe { slot.read() };
            let replacement = if original == self.native.gun_use as usize {
                world_use as Use as usize
            } else if original == self.native.weapon_spawn_use as usize {
                spawner_use as Use as usize
            } else {
                continue;
            };
            if let Some(patch) =
                unsafe { VtablePatch::install(base, address, original, replacement) }
            {
                self.pickups.use_slots.insert(address, patch);
            }
        }
    }

    pub fn can_use_spawner(&self, entity: Ent) -> bool {
        let table = unsafe { field::<usize>(entity, 0) };
        let address = table.wrapping_add(USE_SLOT * size_of::<usize>());
        VtablePatch::checked_slot(self.native.base, address).is_some_and(|slot| {
            let method = unsafe { slot.read() };
            method == self.native.weapon_spawn_use as usize || method == spawner_use as Use as usize
        })
    }

    pub fn native_pickup(
        &mut self,
        player: Ent,
        item: Ent,
        call: PickupCall,
        ctx: &mut PickupContext<'_>,
    ) -> bool {
        let spawner = matches!(call, PickupCall::Use { spawner: true, .. });
        let Some((def, _)) = self.weapon_candidate(ctx.catalog, item) else {
            return self.original_pickup(player, item, call);
        };
        if ctx
            .source
            .as_edict(player)
            .and_then(|edict| ctx.source.player_info(edict))
            .is_none_or(|info| info.team != 2 || info.dead)
            || (!spawner && !unsafe { (self.native.owner)(item.as_ptr()) }.is_null())
        {
            return self.original_pickup(player, item, call);
        }
        if self.pending_removal(item) {
            return false;
        }
        let id = ctx.catalog.find_item(def.classname).unwrap();
        let game_slot = slot_index(def.game_slot);
        let previous = self.weapon_at(player, game_slot);
        let fresh = !spawner && self.shorten_map_gun(item, def, ctx.rules);
        let once = spawner && def.is_weapon() && ctx.rules.enabled && ctx.rules.spawners_once;
        if let Some(inventory) = &mut ctx.inventory
            && self.can_handle(player)
        {
            let native = self.native_weapons(player);
            let weight = ctx.catalog.ammo_weight(&self.reserve(player));
            reconcile_native_inventory(inventory, &native, weight);
            let wants_store = previous.is_some() || (!def.is_weapon() && self.pickups.auto_store);
            if wants_store && inventory.room_for(id) > 0 && inventory.has_box_for(id) {
                let candidate = if spawner {
                    self.spawn_pickup_copy(ctx.source, item, def, ctx.rules)
                } else {
                    Some(item)
                };
                if let Some(candidate) = candidate {
                    if self.store_world_item(player, candidate, id, inventory) {
                        if matches!(call, PickupCall::Bump) {
                            unsafe { set_field(item, WEAPON_OWNER, self.handle(player)) };
                        }
                        self.remove(candidate);
                        if spawner {
                            let count: i32 = unsafe { field(item, SPAWN_COUNT) };
                            if once || count == 1 {
                                self.remove(item);
                            } else if count > 1 {
                                unsafe { set_field(item, SPAWN_COUNT, count - 1) };
                            }
                        }
                        ctx.notice = Some(format!("Stored {}", def.name));
                        return true;
                    }
                    if spawner {
                        self.remove(candidate);
                    }
                }
            }

            let max = inventory.limits().max_weight;
            let displaced = previous.map_or(0.0, |weapon| {
                ctx.catalog.weapon_weight(self.classname(weapon))
            });
            if max > 0.0
                && (def.is_weapon() || previous.is_none())
                && inventory.weight() - displaced + def.weight > max + 1e-3
            {
                ctx.notice = Some(format!("Too heavy to pick up {}", def.name));
                return false;
            }
        }

        let mut snapshot = PickupSnapshot {
            previous,
            before: self.reserve(player),
            spawner,
            fresh,
            spare: if !spawner && firearm(def) {
                unsafe { field(item, EXTRA_PRIMARY_AMMO) }
            } else {
                0
            },
        };
        let equipped = self.original_pickup(player, item, call);
        let now_carried = self.weapon_at(player, game_slot);
        let given = if spawner {
            now_carried.filter(|&weapon| Some(weapon) != previous)
        } else if now_carried == Some(item) {
            Some(item)
        } else {
            None
        };
        if spawner {
            snapshot.fresh =
                given.is_some_and(|weapon| self.shorten_map_gun(weapon, def, ctx.rules));
        }
        if def.is_weapon() && (ctx.inventory.is_some() || ctx.rules.enabled) {
            self.finish_pickup(player, def, given, &snapshot, ctx);
        }
        if once && given.is_some() && !self.pending_removal(item) {
            self.remove(item);
        }
        equipped
    }

    fn spawn_pickup_copy(
        &mut self,
        source: &Source,
        spawner: Ent,
        def: &ItemDef,
        rules: &MapGunRules,
    ) -> Option<Ent> {
        let mut state = WeaponState::default();
        if melee(def) && self.classname(spawner) == "weapon_melee_spawn" {
            let script = unsafe { (self.native.melee_spawn_name)(spawner.as_ptr()) };
            state.melee = unsafe { source::text(script) }.unwrap_or("").to_owned();
        }
        let at = self.position(spawner)?.offset(0.0, 0.0, 16.0);
        let copy = self.spawn_at(source, def, &state, at)?;
        self.seen.remove(&self.handle(copy));
        self.shorten_map_gun(copy, def, rules);
        Some(copy)
    }

    fn original_pickup(&self, player: Ent, item: Ent, call: PickupCall) -> bool {
        unsafe {
            match call {
                PickupCall::Bump => (self.native.bump_weapon)(player.as_ptr(), item.as_ptr()),
                PickupCall::Use {
                    caller,
                    mode,
                    value,
                    spawner,
                } => {
                    let original = if spawner {
                        self.native.weapon_spawn_use
                    } else {
                        self.native.gun_use
                    };
                    original(item.as_ptr(), player.as_ptr(), caller, mode, value);
                    true
                }
            }
        }
    }

    fn finish_pickup(
        &mut self,
        player: Ent,
        def: &ItemDef,
        given: Option<Ent>,
        snapshot: &PickupSnapshot,
        ctx: &PickupContext<'_>,
    ) {
        self.normalize_ammo(player, ctx.catalog);
        let mut after = self.reserve(player);
        if let Some(kind) = def.ammo.filter(|_| firearm(def)) {
            let index = kind.definition().index;
            let before = snapshot.before[index];
            let capacity = (ctx.catalog.ammo_max(kind) - before).max(0);
            if snapshot.spawner && ctx.rules.enabled && given.is_none() {
                after[index] = before;
            } else if snapshot.fresh && ctx.rules.enabled {
                let min = ctx.rules.min_reserve;
                let span = ctx.rules.max_reserve - min + 1;
                let magazines = min + (self.round_seed() % span as u32) as i32;
                let bonus = capacity.min(magazines * kind.definition().magazine);
                after[index] = after[index].min(before + bonus);
            } else if given.is_some() && snapshot.spare > 0 {
                after[index] = after[index].max(before + capacity.min(snapshot.spare));
            }
        }
        for (actual, before) in after.iter_mut().zip(snapshot.before) {
            *actual = (*actual).max(before);
        }
        self.restore_reserve(player, &after);
        if let Some(previous) = snapshot.previous.filter(|&previous| {
            Some(previous) != given
                && !self.pending_removal(previous)
                && unsafe { (self.native.owner)(previous.as_ptr()) } != player.as_ptr()
        }) && ctx
            .catalog
            .find_item(self.classname(previous))
            .is_some_and(|id| firearm(ctx.catalog.item(id)))
        {
            unsafe {
                set_field(previous, EXTRA_PRIMARY_AMMO, 0i32);
                (self.native.network_changed)(previous.as_ptr());
            }
        }
    }
}

struct PickupSnapshot {
    previous: Option<Ent>,
    before: Reserve,
    spare: i32,
    spawner: bool,
    fresh: bool,
}

unsafe extern "C" fn bump(player: *mut Entity, item: *mut Entity) -> bool {
    plugin::pickup(player, item, PickupCall::Bump).unwrap_or_else(|| {
        BUMP.get()
            .is_some_and(|original| unsafe { original(player, item) })
    })
}

unsafe extern "C" fn world_use(
    item: *mut Entity,
    player: *mut Entity,
    caller: *mut Entity,
    mode: i32,
    value: f32,
) {
    let call = PickupCall::Use {
        caller,
        mode,
        value,
        spawner: false,
    };
    if plugin::pickup(player, item, call).is_none()
        && let Some(original) = WORLD_USE.get()
    {
        unsafe { original(item, player, caller, mode, value) };
    }
}

unsafe extern "C" fn spawner_use(
    item: *mut Entity,
    player: *mut Entity,
    caller: *mut Entity,
    mode: i32,
    value: f32,
) {
    let call = PickupCall::Use {
        caller,
        mode,
        value,
        spawner: true,
    };
    if plugin::pickup(player, item, call).is_none()
        && let Some(original) = SPAWNER_USE.get()
    {
        unsafe { original(item, player, caller, mode, value) };
    }
}
