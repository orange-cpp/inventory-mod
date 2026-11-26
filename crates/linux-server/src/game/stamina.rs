use std::{
    cell::RefCell,
    collections::{HashMap, HashSet},
    ffi::{CString, c_void},
    mem::MaybeUninit,
    ptr::{self, NonNull},
};

use inventory_mod::{melee::BUTTON_ATTACK2, rules::StaminaRules};

use super::{Ent, Game, field, native::*, set_field, vtable::VtablePatch};
use crate::{
    plugin,
    source::{Clients, Edict, Entity, Source},
};

pub(super) const DAMAGE_SLOT: usize = 73;
pub(super) const DAMAGE_SIZE: usize = 0x60;
pub(super) const DAMAGE_INFLICTOR: usize = 0x30;
pub(super) const DAMAGE_ATTACKER: usize = 0x34;
pub(super) const DAMAGE_WEAPON: usize = 0x38;
pub(super) const DAMAGE_VALUE: usize = 0x3c;
const INVALID_HANDLE: u32 = u32::MAX;

type Damage = unsafe extern "C" fn(*mut Entity, *const c_void) -> i32;

thread_local! {
    static ORIGINALS: RefCell<HashMap<usize, Damage>> = RefCell::new(HashMap::new());
}

#[derive(Default)]
pub struct Stamina {
    players: HashMap<i32, PlayerStamina>,
    last_tick: Option<f32>,
    next_damage_scan: Option<f32>,
    patches: HashMap<usize, VtablePatch>,
}

struct PlayerStamina {
    edict: NonNull<Edict>,
    handle: u32,
    weapon: u32,
    value: f32,
    used_at: f32,
    speed: f32,
    base_speed: f32,
    applied: f32,
    next_primary: f32,
    next_secondary: f32,
    sent_at: f32,
    sent_value: i32,
    exhausted: bool,
    sent_exhausted: bool,
    on_ground: bool,
    melee: bool,
}

impl PlayerStamina {
    fn new(edict: NonNull<Edict>, handle: u32, on_ground: bool, movement: f32) -> Self {
        Self {
            edict,
            handle,
            on_ground,
            weapon: INVALID_HANDLE,
            value: 100.0,
            used_at: -1000.0,
            speed: 1.0,
            base_speed: movement,
            applied: movement,
            next_primary: 0.0,
            next_secondary: 0.0,
            sent_at: -1000.0,
            sent_value: -2,
            exhausted: false,
            sent_exhausted: false,
            melee: false,
        }
    }

    fn send(&mut self, source: &Source, now: f32, gone: bool) {
        let value = if gone {
            -1
        } else {
            (self.value * 10.0).round() as i32
        };
        let exhausted = !gone && self.exhausted;
        let important = gone
            || self.sent_value < 0
            || self.sent_exhausted != exhausted
            || value == 0
            || value == 1000;
        if (value == self.sent_value && exhausted == self.sent_exhausted)
            || (!important && now - self.sent_at < 0.1)
        {
            return;
        }
        let message = CString::new(format!("@sta@ {value} {}\n", u8::from(exhausted))).unwrap();
        source.client_print(self.edict, &message);
        self.sent_value = value;
        self.sent_exhausted = exhausted;
        self.sent_at = now;
    }

    fn spend(&mut self, amount: f32, now: f32) {
        if amount > 0.0 {
            self.value = (self.value - amount).max(0.0);
            self.used_at = now;
            self.exhausted |= self.value == 0.0;
        }
    }
}

impl Game {
    pub fn stamina_frame(
        &mut self,
        source: &Source,
        clients: Option<&Clients>,
        rules: &StaminaRules,
        receives: impl Fn(i32) -> bool,
    ) {
        let (Some(clients), Some(now)) = (clients.filter(|_| rules.enabled), source.current_time())
        else {
            self.clear_stamina(source, true, receives);
            return;
        };
        let elapsed = now - self.stamina.last_tick.unwrap_or(now);
        if self.stamina.last_tick == Some(now) {
            return;
        }
        if elapsed < 0.0 {
            self.clear_stamina(source, true, &receives);
        }
        self.stamina.last_tick = Some(now);
        let dt = if (0.0..=0.25).contains(&elapsed) {
            elapsed
        } else {
            0.0
        };
        if rules.damage < 100.0 && self.stamina.next_damage_scan.is_none_or(|next| now >= next) {
            self.scan_damage(source);
            self.stamina.next_damage_scan = Some(now + 0.25);
        }

        let mut seen = HashSet::new();
        for edict in clients.live() {
            if !source.player_info(edict).is_some_and(|info| {
                info.connected && info.team == 2 && !info.dead && (rules.bots || !info.fake)
            }) {
                continue;
            }
            let (Some(user), Some(player)) = (source.user_id(edict), source.entity(edict)) else {
                continue;
            };
            let handle = self.handle(player);
            let mut state = self
                .stamina
                .players
                .remove(&user)
                .filter(|state| state.edict == edict && state.handle == handle)
                .unwrap_or_else(|| {
                    PlayerStamina::new(
                        edict,
                        handle,
                        unsafe { field::<i32>(player, FLAGS) & 1 != 0 },
                        unsafe { field(player, LAGGED_MOVEMENT) },
                    )
                });
            self.tick_stamina(player, &mut state, rules, now, dt);
            if receives(user) {
                state.send(source, now, false);
            }
            self.stamina.players.insert(user, state);
            seen.insert(user);
        }
        let unseen = self
            .stamina
            .players
            .keys()
            .copied()
            .filter(|user| !seen.contains(user))
            .collect::<Vec<_>>();
        for user in unseen {
            if let Some(state) = self.stamina.players.remove(&user) {
                self.release_stamina(source, user, state, receives(user));
            }
        }
    }

    fn tick_stamina(
        &self,
        player: Ent,
        state: &mut PlayerStamina,
        rules: &StaminaRules,
        now: f32,
        dt: f32,
    ) {
        let on_ground = unsafe { field::<i32>(player, FLAGS) & 1 != 0 };
        let velocity: [f32; 3] = unsafe { field(player, LOCAL_VELOCITY) };
        let running = on_ground && velocity[0].hypot(velocity[1]) > 130.0;
        let adrenaline = unsafe { (self.native.adrenaline_active)(player.as_ptr()) };
        if adrenaline {
            state.value = 100.0;
            state.exhausted = false;
        }
        let was_exhausted = state.exhausted;
        if running && !state.exhausted && !adrenaline {
            state.spend(rules.run * dt, now);
        }
        if state.on_ground && !on_ground {
            if velocity[2] > 150.0 && !adrenaline {
                state.spend(rules.jump, now);
            }
            if state.speed < 1.0 {
                unsafe {
                    for offset in [LOCAL_VELOCITY, ABSOLUTE_VELOCITY] {
                        let [x, y, z] = field::<[f32; 3]>(player, offset);
                        set_field(player, offset, [x * state.speed, y * state.speed, z]);
                    }
                    (self.native.network_changed)(player.as_ptr());
                }
            }
        }
        state.on_ground = on_ground;
        let penalty = unsafe { field::<i32>(player, SHOVE_PENALTY) };
        if penalty != 0 {
            unsafe { (self.native.set_shove_penalty)(player.as_ptr(), 0) };
        }

        let weapon = NonNull::new(unsafe { (self.native.active_weapon)(player.as_ptr()) });
        if let Some(weapon) = weapon {
            let mut primary: f32 = unsafe { field(weapon, NEXT_PRIMARY) };
            let mut secondary: f32 = unsafe { field(weapon, NEXT_SECONDARY) };
            let handle = self.handle(weapon);
            if handle != state.weapon {
                state.melee = self.classname(weapon) == "weapon_melee";
            } else {
                let primary_moved = primary > state.next_primary + 0.05 && primary > now;
                let secondary_moved =
                    secondary > state.next_secondary + 0.05 && secondary > now + 0.4;
                let buttons: i32 = unsafe { field(player, BUTTONS) };
                let shove = secondary_moved
                    && (penalty != 0
                        || buttons & BUTTON_ATTACK2 != 0
                        || !state.melee
                        || !primary_moved);
                let swing = !shove && state.melee && primary_moved;
                if shove || swing {
                    if was_exhausted && rules.swing_speed < 100.0 {
                        let stretch = 100.0 / rules.swing_speed;
                        unsafe {
                            if primary > now {
                                primary = now + (primary - now) * stretch;
                                set_field(weapon, NEXT_PRIMARY, primary);
                            }
                            if shove && secondary > now {
                                secondary = now + (secondary - now) * stretch;
                                set_field(weapon, NEXT_SECONDARY, secondary);
                            }
                            (self.native.network_changed)(weapon.as_ptr());
                        }
                    }
                    if !adrenaline {
                        state.spend(if shove { rules.shove } else { rules.swing }, now);
                    }
                }
            }
            state.next_primary = primary;
            state.next_secondary = secondary;
            state.weapon = handle;
        } else {
            state.melee = false;
            state.weapon = INVALID_HANDLE;
        }

        if now - state.used_at >= rules.regen_delay {
            let rate = if running { 0.5 } else { 1.0 };
            state.value = (state.value + rules.regen * dt * rate).min(100.0);
        }
        if state.exhausted && state.value >= rules.recover.max(0.1) {
            state.exhausted = false;
        }
        let target = if state.exhausted {
            rules.slow / 100.0
        } else {
            1.0
        };
        let step = 1.5 * dt;
        state.speed += (target - state.speed).clamp(-step, step);
        let speed: f32 = unsafe { field(player, LAGGED_MOVEMENT) };
        if speed != state.applied {
            state.base_speed = speed;
        }
        state.applied = state.base_speed
            * if on_ground {
                state.speed * self.melee_scale(player)
            } else {
                1.0
            };
        if speed != state.applied {
            unsafe {
                set_field(player, LAGGED_MOVEMENT, state.applied);
                (self.native.network_changed)(player.as_ptr());
            }
        }
    }

    fn release_stamina(&self, source: &Source, user: i32, mut state: PlayerStamina, sends: bool) {
        if source.user_id(state.edict) != Some(user) {
            return;
        }
        let Some(player) = source
            .entity(state.edict)
            .filter(|&player| self.handle(player) == state.handle)
        else {
            return;
        };
        let movement: f32 = unsafe { field(player, LAGGED_MOVEMENT) };
        if movement == state.applied && movement != state.base_speed {
            unsafe {
                set_field(player, LAGGED_MOVEMENT, state.base_speed);
                (self.native.network_changed)(player.as_ptr());
            }
        }
        if sends {
            state.send(source, self.stamina.last_tick.unwrap_or(0.0), true);
        }
    }

    pub fn clear_stamina(
        &mut self,
        source: &Source,
        restore: bool,
        receives: impl Fn(i32) -> bool,
    ) {
        let players = std::mem::take(&mut self.stamina.players);
        if restore {
            for (user, state) in players {
                self.release_stamina(source, user, state, receives(user));
            }
        }
        self.stamina.last_tick = None;
        self.stamina.next_damage_scan = None;
    }

    pub fn uninstall_stamina(&mut self, source: &Source, receives: impl Fn(i32) -> bool) {
        self.clear_stamina(source, true, receives);
        self.stamina.patches.clear();
        if !super::vtable::retained() {
            ORIGINALS.with_borrow_mut(HashMap::clear);
        }
    }

    pub(super) fn scan_damage(&mut self, source: &Source) {
        let base = self.native.base;
        for entity in source.entities() {
            let address = unsafe { damage_slot(entity) };
            if self.stamina.patches.contains_key(&address) {
                continue;
            }
            let Some(slot) = VtablePatch::checked_slot(base, address) else {
                continue;
            };
            let target = unsafe { slot.read() };
            let rva = target.wrapping_sub(base);
            if !(0x3656f0..0xbe9724).contains(&rva) {
                continue;
            }
            if let Some(patch) = unsafe {
                VtablePatch::install(base, address, target, take_damage as Damage as usize)
            } {
                let original = unsafe { std::mem::transmute::<usize, Damage>(target) };
                self.stamina.patches.insert(address, patch);
                ORIGINALS.with_borrow_mut(|table| table.insert(address, original));
            }
        }
    }

    pub(super) fn has_damage_hook(&self, entity: Ent) -> bool {
        self.stamina
            .patches
            .contains_key(&unsafe { damage_slot(entity) })
    }

    pub fn stamina_damage(&self, victim: Ent, info: DamageInput, rules: &StaminaRules) -> f32 {
        let damage = info.damage();
        if !rules.enabled
            || damage <= 0.0
            || !self.stamina.players.values().any(|state| state.exhausted)
        {
            return damage;
        }
        let actor = info.handle(DAMAGE_ATTACKER);
        if !self
            .stamina
            .players
            .values()
            .any(|state| state.handle == actor && state.exhausted)
        {
            return damage;
        }
        let Some(attacker) = self.find_entity(actor) else {
            return damage;
        };
        if victim == attacker {
            return damage;
        }
        let active = NonNull::new(unsafe { (self.native.active_weapon)(attacker.as_ptr()) });
        if let Some(weapon) = active.filter(|&weapon| self.classname(weapon) == "weapon_melee") {
            let handle = self.handle(weapon);
            if [DAMAGE_WEAPON, DAMAGE_INFLICTOR]
                .into_iter()
                .any(|offset| info.handle(offset) == handle)
            {
                return damage * rules.damage / 100.0;
            }
        }
        damage
    }

    pub(super) fn exhausted(&self, player: Ent) -> bool {
        let handle = self.handle(player);
        self.stamina
            .players
            .values()
            .any(|state| state.handle == handle && state.exhausted)
    }

    pub(super) fn spend_stamina(&mut self, player: Ent, amount: f32, now: f32) {
        let handle = self.handle(player);
        if unsafe { (self.native.adrenaline_active)(player.as_ptr()) } {
            return;
        }
        if let Some(state) = self
            .stamina
            .players
            .values_mut()
            .find(|state| state.handle == handle)
        {
            if amount > 0.0 {
                state.spend(amount, now);
            } else {
                state.value = (state.value - amount).min(100.0);
            }
        }
    }
}

unsafe fn damage_slot(entity: Ent) -> usize {
    unsafe { field::<usize>(entity, 0) }.wrapping_add(DAMAGE_SLOT * size_of::<usize>())
}

#[derive(Clone, Copy)]
pub struct DamageInput(NonNull<u8>);

impl DamageInput {
    pub fn handle(self, offset: usize) -> u32 {
        // SAFETY: The checked build initializes these fields in CTakeDamageInfo.
        unsafe { self.0.as_ptr().add(offset).cast::<u32>().read_unaligned() }
    }

    pub fn damage(self) -> f32 {
        f32::from_bits(self.handle(DAMAGE_VALUE))
    }
}

#[repr(align(16))]
struct DamageInfo(MaybeUninit<[u8; DAMAGE_SIZE]>);

unsafe extern "C" fn take_damage(victim: *mut Entity, info: *const c_void) -> i32 {
    let Some(entity) = NonNull::new(victim) else {
        return 0;
    };
    let slot = unsafe { damage_slot(entity) };
    let original = ORIGINALS.with_borrow(|table| table.get(&slot).copied());
    let Some(original) = original else {
        return 0;
    };
    let Some(damage_info) = NonNull::new(info.cast::<u8>().cast_mut()).map(DamageInput) else {
        return unsafe { original(victim, info) };
    };
    let damage = damage_info.damage();
    let next = plugin::combat_damage(entity, damage_info, damage);
    if damage == next {
        return unsafe { original(victim, info) };
    }
    let mut adjusted = DamageInfo(MaybeUninit::uninit());
    let bytes = adjusted.0.as_mut_ptr().cast::<u8>();
    // Copy the C++ representation, including padding, without constructing a Rust slice of it.
    unsafe {
        ptr::copy_nonoverlapping(damage_info.0.as_ptr(), bytes, DAMAGE_SIZE);
        bytes.add(DAMAGE_VALUE).cast::<f32>().write_unaligned(next);
        original(victim, bytes.cast())
    }
}
