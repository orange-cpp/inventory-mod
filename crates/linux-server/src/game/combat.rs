use std::{
    cell::Cell,
    collections::{HashMap, HashSet},
    ffi::CString,
    ptr::{self, NonNull},
};

use inventory_mod::{
    melee::{
        BUTTON_HEAVY, BUTTON_HEAVY_SHOVE, BUTTON_RELOAD, BUTTON_THROW, BUTTON_WINDING, WEAR_BROKEN,
        cleave_scale, melee_overhead_swing, melee_profile, melee_wear_after,
    },
    rules::{ChapterRules, MeleeRules},
};

use super::{
    Ent, Game, Vector, field,
    native::*,
    set_field,
    stamina::{DAMAGE_ATTACKER, DAMAGE_INFLICTOR, DAMAGE_SIZE, DAMAGE_WEAPON, DamageInput},
    vtable::VtablePatch,
};
use crate::{
    plugin,
    source::{Clients, Entity, Source},
};

type Attack = unsafe extern "C" fn(*mut Entity);
type Sound = unsafe extern "C" fn(*mut Entity, i32, f32);

const PRIMARY_RVA: usize = 0xc41b54;
const SECONDARY_RVA: usize = 0xc41b58;
const SOUND_RVA: usize = 0xc41ba0;
const MAX_HIT_AGE: f32 = 1.5;

thread_local! {
    static ORIGINAL_ATTACKS: Cell<Option<[Attack; 2]>> = const { Cell::new(None) };
    static ORIGINAL_SOUND: Cell<Option<Sound>> = const { Cell::new(None) };
}

#[derive(Default)]
pub struct Combat {
    attacks: Option<[VtablePatch; 2]>,
    sound: Option<VtablePatch>,
    players: HashMap<u32, Survivor>,
    broken: HashSet<u32>,
    thrown: Vec<Thrown>,
    last_frame: Option<f32>,
    scan_damage_at: f32,
    prune_wear_at: f32,
    pub notices: Vec<(i32, String)>,
}

struct Survivor {
    charge_start: Option<f32>,
    charge_last_tick: f32,
    charged_seconds: f32,
    charge_end: f32,
    block_since: Option<f32>,
    thrown_at: f32,
    swing: Option<Swing>,
    move_scale: f32,
    applied: f32,
    base_speed: f32,
    slowing: bool,
    sent: Option<(i32, i32, f32)>,
}

impl Default for Survivor {
    fn default() -> Self {
        Self {
            charge_start: None,
            charge_last_tick: 0.0,
            charged_seconds: 0.0,
            charge_end: -1000.0,
            block_since: None,
            thrown_at: -1000.0,
            swing: None,
            move_scale: 1.0,
            applied: 1.0,
            base_speed: 1.0,
            slowing: false,
            sent: None,
        }
    }
}

struct Swing {
    at: f32,
    weapon: u32,
    heavy: bool,
    bounced: bool,
    victims: Vec<u32>,
}

struct Thrown {
    weapon: u32,
    player: u32,
    since: f32,
    last: Vector,
    targets: Vec<u32>,
}

#[derive(Clone, Copy)]
pub struct AttackContext {
    weapon: Ent,
    player: Ent,
    offset: usize,
    before: f32,
    start: f32,
    heavy: bool,
    shove: bool,
    heavy_shove: bool,
    saved: Option<(i32, f32)>,
}

impl Game {
    pub fn install_melee(&mut self, rules: &MeleeRules) -> bool {
        if !rules.enabled {
            return true;
        }
        let base = self.native.base;
        let [light, shove] = [self.native.melee_primary, self.native.melee_secondary];
        let (primary, secondary) = unsafe {
            (
                VtablePatch::install(
                    base,
                    base + PRIMARY_RVA,
                    light as usize,
                    primary as Attack as usize,
                ),
                VtablePatch::install(
                    base,
                    base + SECONDARY_RVA,
                    shove as usize,
                    secondary as Attack as usize,
                ),
            )
        };
        let (Some(primary), Some(secondary)) = (primary, secondary) else {
            return false;
        };
        self.combat.attacks = Some([primary, secondary]);
        ORIGINAL_ATTACKS.set(Some([light, shove]));
        let original = self.native.weapon_sound;
        self.combat.sound = unsafe {
            VtablePatch::install(
                base,
                base + SOUND_RVA,
                original as usize,
                sound as Sound as usize,
            )
        };
        if self.combat.sound.is_some() {
            ORIGINAL_SOUND.set(Some(original));
        }
        true
    }

    pub fn clear_melee(&mut self, restore: bool) {
        let players = std::mem::take(&mut self.combat.players);
        if restore {
            for (id, state) in players {
                self.restore_melee_speed(id, &state);
            }
        }
        self.combat.broken.clear();
        self.combat.thrown.clear();
        self.combat.notices.clear();
        self.combat.last_frame = None;
        self.combat.prune_wear_at = 0.0;
        self.combat.scan_damage_at = 0.0;
    }

    pub fn uninstall_melee(&mut self) {
        self.clear_melee(true);
        self.combat.attacks = None;
        self.combat.sound = None;
        ORIGINAL_ATTACKS.set(None);
        ORIGINAL_SOUND.set(None);
    }

    fn restore_melee_speed(&self, handle: u32, state: &Survivor) {
        if state.slowing
            && let Some(player) = self.find_entity(handle)
            && unsafe { field::<f32>(player, LAGGED_MOVEMENT) } == state.applied
        {
            unsafe {
                set_field(player, LAGGED_MOVEMENT, state.base_speed);
                (self.native.network_changed)(player.as_ptr());
            }
        }
    }

    pub fn melee_scale(&self, player: Ent) -> f32 {
        self.combat
            .players
            .get(&self.handle(player))
            .map_or(1.0, |state| state.move_scale)
    }

    fn is_melee(&self, player: Ent) -> bool {
        self.classname(player) == "weapon_melee"
    }

    fn wears(&mut self, source: &Source, weapon: Ent, hits: f32, rules: &MeleeRules) {
        if !rules.breaking {
            return;
        }
        let profile = melee_profile(self.melee_script(weapon), rules);
        if profile.durability <= 0 {
            return;
        }
        let wear = self.wear(weapon);
        let next = melee_wear_after(wear, hits, profile.durability);
        let handle = self.handle(weapon);
        self.condition.entry(handle).or_default().wear = next;
        if next == WEAR_BROKEN {
            self.combat.broken.insert(handle);
        } else if wear < 8000
            && next >= 8000
            && let Some(user) = self.owner_id(source, weapon)
        {
            self.combat
                .notices
                .push((user, "Your weapon is about to break".into()));
        }
    }

    fn owner_id(&self, source: &Source, weapon: Ent) -> Option<i32> {
        let owner = self.owner(weapon)?;
        let edict = source.as_edict(owner)?;
        source
            .player_info(edict)
            .filter(|info| info.team == 2 && !info.dead)?;
        source.user_id(edict)
    }

    fn owner(&self, weapon: Ent) -> Option<Ent> {
        NonNull::new(unsafe { (self.native.owner)(weapon.as_ptr()) })
    }

    fn facing(&self, player: Ent, target: Ent) -> bool {
        let (eye, forward) = self.eyes(player);
        let Some(position) = self.position(target) else {
            return false;
        };
        let x = position.x - eye.x;
        let y = position.y - eye.y;
        let distance = x.hypot(y);
        distance < 1.0
            || (x * forward.x + y * forward.y) > 0.5 * distance * forward.x.hypot(forward.y)
    }

    fn stagger(&self, victim: Ent, attacker: Ent) {
        match self.classname(victim) {
            "infected" => unsafe {
                let flag = field::<u8>(victim, INFECTED_PUSH_ONLY);
                set_field(victim, INFECTED_PUSH_ONLY, 1_u8);
                let responder = victim.cast::<u8>().as_ptr().add(INFECTED_NEXTBOT).cast();
                (self.native.shove_responder)(responder, attacker.as_ptr());
                set_field(victim, INFECTED_PUSH_ONLY, flag);
            },
            "player" => {
                if let Some(at) = self.position(attacker) {
                    unsafe {
                        (self.native.stagger_player)(victim.as_ptr(), attacker.as_ptr(), &at)
                    };
                }
            }
            _ => {}
        }
    }

    fn hit(
        &self,
        source: &Source,
        player: Ent,
        heavy: bool,
        rules: &MeleeRules,
        receives: impl Fn(i32) -> bool,
    ) {
        if rules.view_kick && rules.view_punch > 0.0 {
            let amount = rules.view_punch * if heavy { 1.0 } else { 0.33 };
            let punch = Vector {
                x: amount,
                y: 0.35 * amount,
                z: 0.0,
            };
            unsafe { (self.native.view_punch)(player.as_ptr(), &punch) };
        }
        if let Some(edict) = source.as_edict(player)
            && source.user_id(edict).is_some_and(receives)
        {
            source.client_print(edict, if heavy { c"@mhi@ 1\n" } else { c"@mhi@ 0\n" });
        }
    }

    fn animation_entry(&self, weapon: Ent, offset: usize, index: usize) -> Option<i32> {
        unsafe {
            let info = NonNull::new((self.native.melee_info)(weapon.as_ptr()).cast_mut())?;
            let entries =
                NonNull::new(info.as_ptr().add(0xc20).cast::<*mut u8>().read_unaligned())?;
            let count = info
                .as_ptr()
                .add(0xc2c)
                .cast::<i32>()
                .read_unaligned()
                .clamp(0, 8) as usize;
            (index < count).then(|| {
                entries
                    .as_ptr()
                    .add(index * 0x30 + offset)
                    .cast::<i32>()
                    .read_unaligned()
            })
        }
    }

    fn raise(&self, player: Ent, weapon: Ent) {
        let index = unsafe { field::<i32>(weapon, MELEE_NEXT_SEQUENCE) } as usize;
        let activity = self
            .animation_entry(weapon, 0x08, index)
            .or_else(|| self.animation_entry(weapon, 0x08, 0));
        if let Some(activity) = activity.filter(|&activity| activity > 0) {
            unsafe { (self.native.animation_event)(player.as_ptr(), 66, activity) };
        }
    }

    pub fn prepare_attack(
        &self,
        source: &Source,
        weapon: Ent,
        shove: bool,
        rules: &MeleeRules,
    ) -> Option<AttackContext> {
        if !rules.enabled || self.combat.attacks.is_none() {
            return None;
        }
        let player = self.owner(weapon)?;
        let edict = source.as_edict(player)?;
        let info = source.player_info(edict)?;
        if info.team != 2 || info.dead {
            return None;
        }
        let now = source.current_time()?;
        let profile = melee_profile(self.melee_script(weapon), rules);
        let charge = self
            .combat
            .players
            .get(&self.handle(player))
            .map_or(0.0, |held| {
                if let Some(start) = held.charge_start {
                    held.charge_last_tick - start
                } else if now - held.charge_end < 1.0 {
                    held.charged_seconds
                } else {
                    0.0
                }
            });
        let buttons = unsafe { field::<i32>(player, BUTTONS) };
        let heavy = !shove
            && rules.heavy
            && buttons & BUTTON_HEAVY != 0
            && !self.exhausted(player)
            && charge + 0.15 >= profile.heavy_hold;
        let saved = if heavy && rules.overhead {
            let directions = (0..8)
                .map_while(|index| self.animation_entry(weapon, 0x0c, index))
                .collect::<Vec<_>>();
            melee_overhead_swing(&directions).map(|index| unsafe {
                let saved = (
                    field(weapon, MELEE_NEXT_SEQUENCE),
                    field(weapon, MELEE_SEQUENCE_TIME),
                );
                set_field(weapon, MELEE_NEXT_SEQUENCE, index as i32);
                set_field(weapon, MELEE_SEQUENCE_TIME, now);
                saved
            })
        } else {
            None
        };
        let offset = if shove { NEXT_SECONDARY } else { NEXT_PRIMARY };
        Some(AttackContext {
            weapon,
            player,
            offset,
            before: unsafe { field(weapon, offset) },
            start: now,
            heavy,
            shove,
            heavy_shove: rules.heavy_shove
                && buttons & BUTTON_HEAVY_SHOVE != 0
                && !self.exhausted(player),
            saved,
        })
    }

    pub fn finish_attack(
        &mut self,
        source: &Source,
        attack: AttackContext,
        chapter: &ChapterRules,
    ) {
        let AttackContext {
            weapon,
            player,
            offset,
            start: now,
            heavy,
            shove,
            heavy_shove,
            saved,
            before,
        } = attack;
        let current = unsafe { field::<f32>(weapon, offset) };
        if current <= before + 0.05 || current <= now {
            if let Some((index, time)) = saved {
                unsafe {
                    set_field(weapon, MELEE_NEXT_SEQUENCE, index);
                    set_field(weapon, MELEE_SEQUENCE_TIME, time);
                }
            }
            return;
        }
        let rules = &chapter.melee;
        let profile = melee_profile(self.melee_script(weapon), rules);
        if shove {
            if !heavy_shove {
                return;
            }
            self.stretch(weapon, NEXT_SECONDARY, now, 1.5);
            self.spend_stamina(
                player,
                rules.heavy_shove_stamina * profile.stamina / 100.0 - chapter.stamina.shove,
                now,
            );
            let Some(origin) = self.position(player) else {
                return;
            };
            for target in source.entities().filter(|&target| unsafe {
                field::<i32>(target, TEAM) == 3 && field::<u8>(target, LIFE_STATE) == 0
            }) {
                if self
                    .position(target)
                    .is_some_and(|at| (at.x - origin.x).hypot(at.y - origin.y) <= 120.0)
                    && self.facing(player, target)
                {
                    self.stagger(target, player);
                }
            }
            return;
        }
        let weapon_handle = self.handle(weapon);
        let player_handle = self.handle(player);
        self.combat.players.entry(player_handle).or_default().swing = Some(Swing {
            at: now,
            weapon: weapon_handle,
            heavy,
            bounced: false,
            victims: Vec::new(),
        });
        if heavy {
            self.stretch(weapon, NEXT_PRIMARY, now, rules.heavy_recovery / 100.0);
            self.stretch(weapon, NEXT_SECONDARY, now, rules.heavy_recovery / 100.0);
        }
        let cost = if heavy {
            rules.heavy_stamina
        } else {
            chapter.stamina.swing
        };
        self.spend_stamina(
            player,
            cost * profile.stamina / 100.0 - chapter.stamina.swing,
            now,
        );
    }

    fn stretch(&self, weapon: Ent, offset: usize, now: f32, scale: f32) {
        let timer = unsafe { field::<f32>(weapon, offset) };
        if timer > now {
            unsafe {
                set_field(weapon, offset, now + (timer - now) * scale);
                (self.native.network_changed)(weapon.as_ptr());
            }
        }
    }

    pub fn wall_hit(
        &mut self,
        source: &Source,
        weapon: Ent,
        rules: &MeleeRules,
        receives: impl Fn(i32) -> bool,
    ) {
        if !rules.enabled || !rules.wall_bounce {
            return;
        }
        let (Some(player), Some(now)) = (self.owner(weapon), source.current_time()) else {
            return;
        };
        let handle = self.handle(player);
        let weapon_handle = self.handle(weapon);
        let Some(swing) = self
            .combat
            .players
            .get(&handle)
            .and_then(|state| state.swing.as_ref())
        else {
            return;
        };
        if swing.weapon != weapon_handle
            || now - swing.at > MAX_HIT_AGE
            || swing.bounced
            || !swing.victims.is_empty()
        {
            return;
        }
        let (eye, forward) = self.eyes(player);
        let pitch = forward.z.clamp(-0.34202014, 0.34202014);
        let horizontal = forward.x.hypot(forward.y);
        if horizontal < 1e-3 {
            return;
        }
        let length = 72.0 * (1.0 - pitch * pitch).sqrt() / horizontal;
        let delta = Vector {
            x: forward.x * length,
            y: forward.y * length,
            z: pitch * 72.0,
        };
        const MASK_SOLID: u32 = 0x200400b;
        if source
            .trace_world(eye, delta, MASK_SOLID)
            .is_some_and(|hit| hit.fraction < 1.0 && !hit.start_solid && hit.normal.z.abs() < 0.6)
        {
            if let Some(swing) = self
                .combat
                .players
                .get_mut(&handle)
                .and_then(|state| state.swing.as_mut())
            {
                swing.bounced = true;
            }
            self.wears(source, weapon, 3.0, rules);
            self.hit(source, player, true, rules, receives);
        }
    }

    pub fn combat_damage(
        &mut self,
        source: &Source,
        victim: Ent,
        info: DamageInput,
        chapter: &ChapterRules,
        receives: impl Fn(i32) -> bool,
    ) -> f32 {
        let damage = info.damage();
        if self.combat.attacks.is_none() || !chapter.melee.enabled {
            return self.stamina_damage(victim, info, &chapter.stamina);
        }
        let attacker_handle = info.handle(DAMAGE_ATTACKER);
        let Some(attacker) = self
            .find_entity(attacker_handle)
            .filter(|&attacker| attacker != victim && damage > 0.0)
        else {
            return damage;
        };
        let rules = &chapter.melee;
        let now = source.current_time().unwrap_or(0.0);
        let attacker_team = unsafe { field::<i32>(attacker, TEAM) };
        let victim_team = unsafe { field::<i32>(victim, TEAM) };
        if rules.blocking
            && attacker_team == 3
            && victim_team == 2
            && self.classname(attacker) == "infected"
            && !self.exhausted(victim)
            && self.facing(victim, attacker)
            && let Some(since) = self
                .combat
                .players
                .get(&self.handle(victim))
                .and_then(|state| state.block_since)
            && let Some(weapon) = self.active(victim).filter(|&weapon| self.is_melee(weapon))
        {
            let profile = melee_profile(self.melee_script(weapon), rules);
            let deflected = rules.deflecting && now - since <= rules.deflect;
            let scale = if deflected { 0.005 } else { 0.01 };
            self.spend_stamina(victim, rules.block_stamina * profile.stamina * scale, now);
            return if deflected {
                self.stagger(attacker, victim);
                0.0
            } else {
                damage * rules.block / 100.0
            };
        }
        if attacker_team != 2 || self.classname(attacker) != "player" {
            return damage;
        }
        let Some(weapon) = self
            .active(attacker)
            .filter(|&weapon| self.is_melee(weapon))
        else {
            return damage;
        };
        let weapon_handle = self.handle(weapon);
        if [DAMAGE_WEAPON, DAMAGE_INFLICTOR]
            .into_iter()
            .all(|offset| info.handle(offset) != weapon_handle)
        {
            return damage;
        }
        let penalty = if self.exhausted(attacker) {
            chapter.stamina.damage / 100.0
        } else {
            1.0
        };
        let profile = melee_profile(self.melee_script(weapon), rules);
        let victim_handle = self.handle(victim);
        let Some(swing) = self
            .combat
            .players
            .get_mut(&attacker_handle)
            .and_then(|state| state.swing.as_mut())
            .filter(|swing| {
                swing.weapon == weapon_handle && now - swing.at <= MAX_HIT_AGE && victim_team != 2
            })
        else {
            return damage * penalty;
        };

        let existing = swing
            .victims
            .iter()
            .position(|&handle| handle == victim_handle);
        let index = existing.unwrap_or(swing.victims.len());
        let heavy = swing.heavy;
        if existing.is_none() {
            swing.victims.push(victim_handle);
        }
        let class = self.classname(victim);
        let common = class == "infected";
        let head = victim_team == 3
            && matches!(class, "infected" | "player" | "witch")
            && unsafe { field::<i32>(victim, HIT_GROUP) } == 1;
        let health = unsafe { field::<i32>(victim, HEALTH) };
        let full = if common {
            unsafe { field::<i32>(victim, MAX_HEALTH) }.max(1) as f32
        } else {
            damage
        };
        let scale = if heavy {
            profile.heavy_damage
        } else {
            profile.light_damage
        };
        let mut result =
            full * scale * cleave_scale(index as i32, profile.cleave, profile.cleave_min) * penalty
                / 100.0;
        if head {
            result *= rules.head_damage / 100.0;
        }
        if common && rules.head_only && !head && health > 0 {
            result = result.min((health - 1) as f32);
        }
        if existing.is_none() {
            self.wears(source, weapon, if heavy { 1.5 } else { 1.0 }, rules);
            self.spend_stamina(attacker, rules.hit_stamina * profile.stamina / 100.0, now);
            if index == 0 {
                self.hit(source, attacker, heavy, rules, receives);
            }
            if heavy && rules.hit_stagger && victim_team == 3 && result < health as f32 {
                self.stagger(victim, attacker);
            }
        }
        result
    }

    fn throw_weapon(
        &mut self,
        source: &Source,
        player: Ent,
        weapon: Ent,
        now: f32,
        rules: &MeleeRules,
    ) -> bool {
        if self.pending_removal(weapon) {
            return false;
        }
        let (eye, forward) = self.eyes(player);
        let speed = rules.throw_speed;
        let velocity = Vector {
            x: forward.x * speed,
            y: forward.y * speed,
            z: forward.z * speed + 120.0,
        };
        let target = eye.offset(200.0 * forward.x, 200.0 * forward.y, 200.0 * forward.z);
        unsafe { (self.native.quiet_drop)(player.as_ptr(), weapon.as_ptr(), &target, &velocity) };
        if self.owner(weapon).is_some() || self.pending_removal(weapon) {
            return false;
        }
        let axis = Vector {
            x: -libm::asinf(forward.z.clamp(-1.0, 1.0)).to_degrees(),
            y: libm::atan2f(forward.y, forward.x).to_degrees(),
            z: 0.0,
        };
        let start = eye.offset(24.0 * forward.x, 24.0 * forward.y, 24.0 * forward.z - 4.0);
        unsafe { (self.native.teleport)(weapon.as_ptr(), &start, &axis, &velocity) };
        let targets = source
            .entities()
            .filter_map(|target| {
                let class = self.classname(target);
                let enemy = matches!(class, "infected" | "witch")
                    || (class == "player" && unsafe { field::<i32>(target, TEAM) } == 3);
                if enemy
                    && self
                        .position(target)
                        .is_some_and(|at| (at.x - eye.x).hypot(at.y - eye.y) < 1500.0)
                {
                    Some(self.handle(target))
                } else {
                    None
                }
            })
            .collect();
        self.combat.thrown.push(Thrown {
            weapon: self.handle(weapon),
            player: self.handle(player),
            since: now,
            last: self.position(weapon).unwrap_or(start),
            targets,
        });
        true
    }

    fn thrown_frame(
        &mut self,
        source: &Source,
        rules: &MeleeRules,
        now: f32,
        receives: &impl Fn(i32) -> bool,
    ) {
        for mut thrown in std::mem::take(&mut self.combat.thrown) {
            let Some(weapon) = self
                .find_entity(thrown.weapon)
                .filter(|&weapon| now - thrown.since <= 1.6 && self.owner(weapon).is_none())
            else {
                continue;
            };
            let Some(at) = self.position(weapon) else {
                continue;
            };
            let last = std::mem::replace(&mut thrown.last, at);
            let delta = at.offset(-last.x, -last.y, -last.z);
            let target = thrown.targets.iter().find_map(|&handle| {
                let victim = self.find_entity(handle)?;
                if unsafe { field::<u8>(victim, LIFE_STATE) } != 0 {
                    return None;
                }
                let p = self.position(victim)?.offset(0.0, 0.0, 40.0);
                let offset = p.offset(-last.x, -last.y, -last.z);
                let length = delta.dot(delta);
                let fraction = if length > 0.0 {
                    (offset.dot(delta) / length).clamp(0.0, 1.0)
                } else {
                    0.0
                };
                let difference = offset.offset(
                    -fraction * delta.x,
                    -fraction * delta.y,
                    -fraction * delta.z,
                );
                (difference.dot(difference) <= 36.0 * 36.0).then_some((handle, victim, p))
            });
            let Some((handle, victim, point)) = target else {
                self.combat.thrown.push(thrown);
                continue;
            };
            let player = self.find_entity(thrown.player);
            let owner = player.unwrap_or(weapon);
            let full = if self.classname(victim) == "infected" {
                unsafe { field::<i32>(victim, MAX_HEALTH) }.max(1) as f32
            } else {
                100.0
            };
            #[repr(align(16))]
            struct DamageBuffer([u8; DAMAGE_SIZE]);
            let mut buf = DamageBuffer([0; DAMAGE_SIZE]);
            let raw = buf.0.as_mut_ptr();
            unsafe {
                (self.native.damage_info)(
                    raw,
                    weapon.as_ptr(),
                    owner.as_ptr(),
                    weapon.as_ptr(),
                    full * rules.throw_damage / 100.0,
                    0x80,
                    0,
                );
                raw.cast::<Vector>().write_unaligned(Vector {
                    x: delta.x * 300.0,
                    y: delta.y * 300.0,
                    z: 0.0,
                });
                raw.add(12).cast::<Vector>().write_unaligned(point);
                (self.native.take_damage)(victim.as_ptr(), raw);
            }
            if let Some(alive) = self
                .find_entity(handle)
                .filter(|&entity| unsafe { field::<u8>(entity, LIFE_STATE) } == 0)
            {
                self.stagger(alive, owner);
            }
            self.wears(source, weapon, 2.0, rules);
            if let Some(player) = player {
                self.hit(source, player, true, rules, receives);
            }
        }
    }

    pub fn melee_frame(
        &mut self,
        source: &Source,
        clients: Option<&Clients>,
        chapter: &ChapterRules,
        receives: impl Fn(i32) -> bool,
    ) {
        let rules = &chapter.melee;
        let (Some(clients), Some(now)) = (
            clients.filter(|_| rules.enabled && self.combat.attacks.is_some()),
            source.current_time(),
        ) else {
            return;
        };
        let dt = (now - self.combat.last_frame.unwrap_or(now)).clamp(0.0, 0.25);
        self.combat.last_frame = Some(now);
        if now >= self.combat.scan_damage_at {
            self.scan_damage(source);
            self.combat.scan_damage_at = now + 0.25;
        }

        let mut previous = std::mem::take(&mut self.combat.players);
        for edict in clients.live() {
            let Some(info) = source
                .player_info(edict)
                .filter(|info| info.connected && info.team == 2 && !info.dead)
            else {
                continue;
            };
            let Some(player) = source.entity(edict) else {
                continue;
            };
            let handle = self.handle(player);
            let mut state = previous.remove(&handle).unwrap_or_default();
            let weapon = self.active(player).filter(|&weapon| self.is_melee(weapon));
            let profile =
                melee_profile(weapon.map_or("", |weapon| self.melee_script(weapon)), rules);
            let buttons = unsafe { field::<i32>(player, BUTTONS) };
            let winding = weapon.is_some() && buttons & BUTTON_WINDING != 0;
            if let Some(weapon) = weapon.filter(|_| winding) {
                if state.charge_start.is_none() {
                    state.charge_start = Some(now);
                    if rules.gesture {
                        self.raise(player, weapon);
                    }
                }
                state.charge_last_tick = now;
                self.spend_stamina(
                    player,
                    rules.wind_stamina * profile.stamina * dt / 100.0,
                    now,
                );
            } else if let Some(start) = state.charge_start.take() {
                state.charged_seconds = state.charge_last_tick - start;
                state.charge_end = now;
            }
            let blocking = weapon.is_some()
                && rules.blocking
                && !winding
                && buttons & BUTTON_RELOAD != 0
                && !self.exhausted(player);
            if blocking {
                state.block_since.get_or_insert(now);
            } else {
                state.block_since = None;
            }
            if let Some(weapon) = weapon.filter(|_| {
                rules.throwing && buttons & BUTTON_THROW != 0 && now - state.thrown_at > 0.5
            }) && self.throw_weapon(source, player, weapon, now, rules)
            {
                state.thrown_at = now;
            }
            state.move_scale = 1.0;
            if let Some(start) = state.charge_start.filter(|_| rules.wind_slowdown) {
                let progress = ((now - start) / profile.heavy_hold.max(0.1)).clamp(0.0, 1.0);
                state.move_scale = 1.0 + progress * (rules.wind_speed / 100.0 - 1.0);
            }
            if blocking {
                state.move_scale = state.move_scale.min(rules.block_speed / 100.0);
            }
            if !chapter.stamina.enabled {
                let on_ground = unsafe { field::<i32>(player, FLAGS) } & 1 != 0;
                let scale = if on_ground { state.move_scale } else { 1.0 };
                let movement = unsafe { field::<f32>(player, LAGGED_MOVEMENT) };
                if !state.slowing || movement != state.applied {
                    state.base_speed = movement;
                }
                state.applied = state.base_speed * scale;
                state.slowing = scale != 1.0;
                if movement != state.applied {
                    unsafe {
                        set_field(player, LAGGED_MOVEMENT, state.applied);
                        (self.native.network_changed)(player.as_ptr());
                    }
                }
            }
            let hold = if rules.heavy {
                (profile.heavy_hold * 1000.0).round() as i32
            } else {
                0
            };
            let shove = if rules.heavy_shove {
                (rules.heavy_shove_hold * 1000.0).round() as i32
            } else {
                0
            };
            if !info.fake
                && source.user_id(edict).is_some_and(&receives)
                && state.sent.is_none_or(|(old_hold, old_shove, at)| {
                    old_hold != hold || old_shove != shove || now - at >= 5.0
                })
            {
                let features = u8::from(rules.overhead) | (u8::from(rules.throwing) << 1);
                let line = CString::new(format!("@mel@ {hold} {shove} {features}\n")).unwrap();
                source.client_print(edict, &line);
                state.sent = Some((hold, shove, now));
            }
            self.combat.players.insert(handle, state);
        }
        for (handle, state) in previous {
            self.restore_melee_speed(handle, &state);
        }

        self.thrown_frame(source, rules, now, &receives);
        for handle in std::mem::take(&mut self.combat.broken) {
            if let Some(weapon) = self.find_entity(handle) {
                if let Some(player) = self.owner(weapon) {
                    if let Some(user) = self.owner_id(source, weapon) {
                        let script = self.melee_script(weapon);
                        self.combat
                            .notices
                            .push((user, format!("Your {script} broke")));
                    }
                    unsafe {
                        (self.native.quiet_drop)(
                            player.as_ptr(),
                            weapon.as_ptr(),
                            ptr::null(),
                            ptr::null(),
                        )
                    };
                }
                self.remove(weapon);
            }
        }
        if now >= self.combat.prune_wear_at {
            self.combat.prune_wear_at = now + 10.0;
            let gone = self
                .condition
                .keys()
                .copied()
                .filter(|&id| self.find_entity(id).is_none())
                .collect::<Vec<_>>();
            for id in gone {
                self.condition.remove(&id);
            }
        }
    }

    pub fn melee_notices(&mut self) -> Vec<(i32, String)> {
        std::mem::take(&mut self.combat.notices)
    }
}

unsafe extern "C" fn primary(weapon: *mut Entity) {
    unsafe { run_attack(weapon, false) };
}

unsafe extern "C" fn secondary(weapon: *mut Entity) {
    unsafe { run_attack(weapon, true) };
}

unsafe fn run_attack(weapon: *mut Entity, shove: bool) {
    let Some(weapon) = NonNull::new(weapon) else {
        return;
    };
    let prepared = plugin::prepare_attack(weapon, shove);
    if let Some(functions) = ORIGINAL_ATTACKS.get() {
        unsafe { functions[usize::from(shove)](weapon.as_ptr()) };
    }
    if let Some(attack) = prepared {
        plugin::finish_attack(attack);
    }
}

unsafe extern "C" fn sound(weapon: *mut Entity, kind: i32, time: f32) {
    if let Some(original) = ORIGINAL_SOUND.get() {
        unsafe { original(weapon, kind, time) };
    }
    if kind == 10
        && let Some(weapon) = NonNull::new(weapon)
    {
        plugin::wall_hit(weapon);
    }
}
