use std::{
    cell::RefCell,
    collections::{HashMap, HashSet},
    ffi::c_void,
    io, ptr,
    sync::atomic::{AtomicUsize, Ordering},
};

use inventory_mod::{
    melee::{
        BUTTON_HEAVY, BUTTON_HEAVY_SHOVE, BUTTON_RELOAD, BUTTON_THROW, BUTTON_WINDING, WEAR_BROKEN,
        cleave_scale, melee_overhead_swing, melee_profile, melee_wear_after,
    },
    rules::{MeleeRules, Rules},
};

use super::{
    ENTITY_ORIGIN, Game, Host, LIFE_STATE, PLAYER_TEAM, Teleport, Vector, WEAPON_OWNER, address,
    ammo::handle, hooks::Hooks, relative,
};
use crate::{
    client::with_client,
    dedicated,
    native::{self, Detour},
};

const BUTTONS: usize = 0x1cb4;
const NEXT_PRIMARY: usize = 0x13f0;
const NEXT_SECONDARY: usize = 0x13f4;
const HEALTH: usize = 0xec;
const MAX_HEALTH: usize = 0xe8;
const HIT_GROUP: usize = 0x1838;
const ANIMATION: usize = 0x17f8;
const LAST_SWING: usize = 0x1828;
const INFO_SIZE: usize = 0x60;
const INFO_INF: usize = 0x30;
const INFO_ATTACKER: usize = 0x34;
const INFO_WEAPON: usize = 0x38;
const INFO_DAMAGE: usize = 0x3c;

type Attack = unsafe extern "thiscall" fn(*mut c_void);
type Damage = unsafe extern "thiscall" fn(*mut c_void, *const c_void) -> i32;
type Sound = unsafe extern "thiscall" fn(*mut c_void, i32, f32);
type Anim = unsafe extern "thiscall" fn(*mut c_void, i32, i32);
type Punch = unsafe extern "thiscall" fn(*mut c_void, *const Vector);
type Stagger = unsafe extern "thiscall" fn(*mut c_void, *mut c_void, *const Vector);
type Infected = unsafe extern "thiscall" fn(*mut c_void) -> *mut c_void;
type Shove = unsafe extern "thiscall" fn(*mut c_void, *mut c_void);
type Info = unsafe extern "thiscall" fn(*mut c_void, u32) -> *mut c_void;

static PRIMARY: AtomicUsize = AtomicUsize::new(0);
static SECONDARY: AtomicUsize = AtomicUsize::new(0);
static DAMAGE: AtomicUsize = AtomicUsize::new(0);
static SOUND: AtomicUsize = AtomicUsize::new(0);

#[derive(Default)]
pub struct Wear {
    pub numbers: RefCell<HashMap<u32, i32>>,
}

pub struct Combat {
    hooks: [Option<Detour>; 4],
    animation: Option<Anim>,
    punch: Option<Punch>,
    stagger: Option<Stagger>,
    info: Option<(usize, Info)>,
    players: HashMap<usize, Actor>,
    broken: HashSet<u32>,
    thrown: Vec<Thrown>,
    template: Option<[u8; INFO_SIZE]>,
}

struct Actor {
    wound: Option<(f32, f32)>,
    winding: Option<(f32, f32)>,
    block: Option<f32>,
    swung: Option<Swing>,
    thrown_at: f32,
}

impl Default for Actor {
    fn default() -> Self {
        Self {
            wound: None,
            winding: None,
            block: None,
            swung: None,
            thrown_at: -1000.0,
        }
    }
}

struct Swing {
    weapon: usize,
    at: f32,
    heavy: bool,
    victim: Vec<u32>,
    bounced: bool,
}

struct Thrown {
    weapon: u32,
    player: u32,
    at: f32,
    last: Vector,
    targets: Vec<u32>,
}

#[derive(Clone, Copy)]
struct Prepared {
    weapon: usize,
    player: usize,
    before: f32,
    now: f32,
    heavy: bool,
    shove: bool,
    heavy_shove: bool,
    saved: Option<[u32; 2]>,
}

impl Combat {
    pub fn install(game: &Game, rules: &Rules) -> Self {
        let base = game.image.base;
        let enabled = rules.melee.enabled;
        let table = base
            + if game.image.timestamp == 0x6afc_cca0 {
                0x6d6174
            } else {
                0x6d611c
            };
        let method = |slot: usize| native::word(table + 4 * slot).unwrap_or_default() as usize;
        let damage = if enabled || rules.stamina.enabled || rules.mutators.enabled {
            find(base, 0x55000, 0x1400, "55 8B EC 83 EC 6C 57 8B F9 8B 0D")
                .and_then(|at| attach(at, damage as Damage as usize, &DAMAGE))
        } else {
            None
        };
        let primary = if enabled && damage.is_some() {
            validated(
                method(282),
                "56 57 8B F1 E8 ?? ?? ?? ?? 8B F8 85 FF 0F 84 ?? ?? ?? ?? 8B 87 D8 33 00 00",
            )
            .and_then(|at| attach(at, primary as Attack as usize, &PRIMARY))
        } else {
            None
        };
        let secondary = if primary.is_some() {
            validated(
                method(283),
                "56 57 8B F1 E8 ?? ?? ?? ?? 8B F8 85 FF 0F 84 ?? ?? ?? ?? 8B 06 8B 90 7C",
            )
            .and_then(|at| attach(at, secondary as Attack as usize, &SECONDARY))
        } else {
            None
        };
        let sound = if primary.is_some() {
            validated(
                method(301),
                "55 8B EC 81 EC CC 00 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 53 8B 5D 08 56 8B F1",
            )
            .and_then(|at| attach(at, sound as Sound as usize, &SOUND))
        } else {
            None
        };

        let info = find(
            base,
            0x3eb800,
            0x1800,
            "55 8B EC 53 56 8B F1 8B 86 DC 17 00 00 50 B9 ?? ?? ?? ?? E8",
        )
        .and_then(|start| {
            let db = native::word(start + 15)? as usize;
            let lookup = relative(start + 19)?;
            if !game.image.includes(db, 4) || !game.image.includes(lookup, 1) {
                return None;
            }
            Some((db, unsafe { std::mem::transmute::<usize, Info>(lookup) }))
        });
        let animation = native::word(game.player_table + 4 * 510)
            .map(|p| p as usize)
            .and_then(|at| validated(at, "55 8B EC 56 8B 75 08 57 8B F9 83 FE 63 74 29"))
            .map(|at| unsafe { std::mem::transmute::<usize, Anim>(at) });
        let punch = find(
            base,
            0x7cc00,
            0x900,
            "55 8B EC A1 ?? ?? ?? ?? 83 EC 0C 83 78 30 00 56 8B F1 0F 85",
        )
        .map(|at| unsafe { std::mem::transmute::<usize, Punch>(at) });
        let stagger = find(base, 0x32cc00, 0x900, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 83 EC 48 56 57 8B F1 E8 ?? ?? ?? ?? 84 C0")
            .map(|at| unsafe { std::mem::transmute::<usize, Stagger>(at) });
        Self {
            hooks: [primary, secondary, damage, sound],
            animation,
            punch,
            stagger,
            info,
            players: HashMap::new(),
            broken: HashSet::new(),
            thrown: Vec::new(),
            template: None,
        }
    }

    pub fn available(&self) -> bool {
        self.hooks[0].is_some()
    }

    pub fn has_shove(&self) -> bool {
        self.hooks[1].is_some()
    }

    pub fn features(&self, rules: &MeleeRules) -> u8 {
        u8::from(self.info.is_some() && rules.overhead)
            | (u8::from(rules.throwing && self.hooks[2].is_some()) << 1)
    }

    pub fn reset(&mut self) {
        self.players.clear();
        self.broken.clear();
        self.thrown.clear();
    }

    pub fn stop(&mut self) -> io::Result<()> {
        for (hook, original) in self
            .hooks
            .iter_mut()
            .zip([&PRIMARY, &SECONDARY, &DAMAGE, &SOUND])
        {
            if let Some(hook) = hook {
                hook.disable()?;
                original.store(0, Ordering::Release);
            }
        }
        Ok(())
    }

    fn attack_info(&self, weapon: usize) -> Option<(usize, usize)> {
        let (db, lookup) = self.info?;
        let script = native::word(weapon + 0x17dc)?;
        let info = unsafe { lookup(address(db), script) }.addr();
        let data = native::word(info + 0xc20)? as usize;
        let count = native::word(info + 0xc2c)?.min(8) as usize;
        (data != 0 && count > 0).then_some((data, count))
    }

    fn gesture(&self, player: usize, weapon: usize) {
        if let (Some(anim), Some((table, count))) = (self.animation, self.attack_info(weapon)) {
            let index = word(weapon + ANIMATION).clamp(0, count as i32 - 1) as usize;
            let activity = word(table + 0x30 * index + 8);
            if activity > 0 {
                unsafe { anim(address(player), 66, activity) };
            }
        }
    }

    fn stagger(&self, game: &Game, victim: usize, source: usize) {
        match game.classname(victim).as_deref() {
            Some("player") => {
                if let (Some(stagger), Some(at)) =
                    (self.stagger, Vector::read(source + ENTITY_ORIGIN))
                {
                    unsafe { stagger(address(victim), address(source), &at) };
                }
            }
            Some("infected") => {
                let Some(method) =
                    native::vslot(victim, 82).filter(|&at| game.image.includes(at, 1))
                else {
                    return;
                };
                let get = unsafe { std::mem::transmute::<usize, Infected>(method) };
                let infected = unsafe { get(address(victim)) }.addr();
                if infected == 0 {
                    return;
                }
                let next_bot = infected + 0x1a40;
                let Some(at) = native::vslot(next_bot, 28).filter(|&at| {
                    validated(at, "55 8B EC 56 57 8B F9 8B 07 8B 50 04 FF D2").is_some()
                }) else {
                    return;
                };
                let shove = unsafe { std::mem::transmute::<usize, Shove>(at) };
                let attr = infected + 0x1d94;
                if let Some(before) = native::word(attr) {
                    native::store_word(attr, (before & !255) | 1);
                    unsafe { shove(address(next_bot), address(source)) };
                    native::store_word(attr, before);
                }
            }
            _ => {}
        }
    }
}

fn find(base: usize, search: usize, count: usize, pattern: &str) -> Option<usize> {
    native::find_pattern(base + search, count, pattern)
}

fn validated(at: usize, signature: &str) -> Option<usize> {
    native::matches_pattern(at, signature).then_some(at)
}

fn attach(at: usize, hook: usize, original: &AtomicUsize) -> Option<Detour> {
    let mut detour = Detour::prepare(at, hook).ok()?;
    original.store(detour.trampoline(), Ordering::Release);
    if detour.enable().is_err() {
        original.store(0, Ordering::Release);
        return None;
    }
    Some(detour)
}

fn word(addr: usize) -> i32 {
    native::word(addr).unwrap_or_default() as i32
}

fn float(addr: usize) -> f32 {
    native::word(addr)
        .map(f32::from_bits)
        .filter(|n| n.is_finite())
        .unwrap_or_default()
}

fn alive(actor: usize) -> bool {
    let mut state = [1];
    native::read(actor + LIFE_STATE, &mut state) && state[0] == 0
}

fn owner(game: &Game, weapon: usize) -> Option<usize> {
    game.resolve(native::word(weapon + WEAPON_OWNER)?)
}

fn active_melee(game: &Game, actor: usize) -> Option<usize> {
    game.active(actor)
        .filter(|&weapon| game.classname(weapon).as_deref() == Some("weapon_melee"))
}

fn script(weapon: usize) -> String {
    Game::melee_script(weapon)
}

fn front(game: &Game, actor: usize, target: usize) -> bool {
    let Some((eye, forward)) = game.eyes(actor) else {
        return false;
    };
    let Some(at) = Vector::read(target + ENTITY_ORIGIN) else {
        return false;
    };
    let x = at.x - eye.x;
    let y = at.y - eye.y;
    let distance = x.hypot(y);
    distance < 1.0 || (x * forward.x + y * forward.y) > 0.5 * distance * forward.x.hypot(forward.y)
}

fn full(game: &Game, victim: usize, default: f32) -> f32 {
    if game.classname(victim).as_deref() == Some("infected") {
        let maximum = word(victim + MAX_HEALTH);
        if maximum > 0 { maximum as f32 } else { 50.0 }
    } else {
        default
    }
}

fn bytes_word(bytes: &[u8; INFO_SIZE], index: usize) -> u32 {
    u32::from_le_bytes(bytes[index..index + 4].try_into().unwrap())
}

fn set_word(bytes: &mut [u8; INFO_SIZE], index: usize, word: u32) {
    bytes[index..index + 4].copy_from_slice(&word.to_le_bytes());
}

impl Host {
    fn prepare_attack(&self, weapon: usize, shove: bool) -> Option<Prepared> {
        let rules = &self.effective.melee;
        if !rules.enabled || !self.combat.available() {
            return None;
        }
        let player = owner(&self.game, weapon)?;
        if !self.game.living(player) {
            return None;
        }
        let now = self.hooks.as_ref().and_then(Hooks::time)?;
        let charge = self.combat.players.get(&player).map_or(0.0, |state| {
            if let Some((start, last)) = state.winding {
                last - start
            } else {
                state
                    .wound
                    .filter(|&(end, _)| now - end <= 1.0)
                    .map_or(0.0, |(_, held)| held)
            }
        });
        let profile = melee_profile(&script(weapon), rules);
        let buttons = word(player + BUTTONS);
        let can_heavy = !self.movement.exhausted(player);
        let heavy = !shove
            && rules.heavy
            && can_heavy
            && buttons & BUTTON_HEAVY != 0
            && charge + 0.15 >= profile.heavy_hold;
        let heavy_shove =
            shove && rules.heavy_shove && can_heavy && buttons & BUTTON_HEAVY_SHOVE != 0;
        let saved = if heavy && rules.overhead {
            self.combat.attack_info(weapon).and_then(|(table, n)| {
                let directions = (0..n)
                    .map(|index| word(table + index * 0x30 + 0x0c))
                    .collect::<Vec<_>>();
                let move_to = melee_overhead_swing(&directions)?;
                let saved = [
                    native::word(weapon + ANIMATION)?,
                    native::word(weapon + LAST_SWING)?,
                ];
                native::store_word(weapon + ANIMATION, move_to as u32);
                native::store_word(weapon + LAST_SWING, now.to_bits());
                Some(saved)
            })
        } else {
            None
        };
        let before = float(weapon + if shove { NEXT_SECONDARY } else { NEXT_PRIMARY });
        Some(Prepared {
            weapon,
            player,
            before,
            now,
            heavy,
            shove,
            heavy_shove,
            saved,
        })
    }

    fn complete_attack(&mut self, swing: Prepared) {
        let Prepared {
            player,
            weapon,
            now,
            shove,
            heavy,
            heavy_shove,
            before,
            saved,
        } = swing;
        let offset = if shove { NEXT_SECONDARY } else { NEXT_PRIMARY };
        let after = float(weapon + offset);
        if after <= before + 0.05 || after <= now {
            if let Some([animation, last]) = saved {
                native::store_word(weapon + ANIMATION, animation);
                native::store_word(weapon + LAST_SWING, last);
            }
            return;
        }
        let rules = &self.effective.melee;
        let profile = melee_profile(&script(weapon), rules);
        let stamina = &self.effective.stamina;
        let stretch = |field: usize, scale: f32| {
            let time = float(weapon + field);
            if time > now {
                native::store_word(weapon + field, (now + (time - now) * scale).to_bits());
            }
        };
        if shove {
            if heavy_shove {
                stretch(NEXT_SECONDARY, 1.5);
                self.movement.spend(
                    player,
                    rules.heavy_shove_stamina * profile.stamina / 100.0 - stamina.shove,
                    now,
                );
                if let Some(at) = Vector::read(player + ENTITY_ORIGIN) {
                    for entity in self.enemies() {
                        if Vector::read(entity + ENTITY_ORIGIN)
                            .is_some_and(|point| (at.x - point.x).hypot(at.y - point.y) < 120.0)
                            && front(&self.game, player, entity)
                        {
                            self.combat.stagger(&self.game, entity, player);
                        }
                    }
                }
            }
        } else {
            self.combat.players.entry(player).or_default().swung = Some(Swing {
                weapon,
                at: now,
                heavy,
                victim: Vec::new(),
                bounced: false,
            });
            if heavy {
                stretch(NEXT_PRIMARY, rules.heavy_recovery / 100.0);
                stretch(NEXT_SECONDARY, rules.heavy_recovery / 100.0);
            }
            let cost = if heavy {
                rules.heavy_stamina
            } else {
                stamina.swing
            };
            self.movement
                .spend(player, cost * profile.stamina / 100.0 - stamina.swing, now);
        }
        if let Some(hooks) = &self.hooks {
            hooks.changed(weapon);
        }
    }

    fn wear_down(&mut self, weapon: usize, hits: f32) {
        let rules = &self.effective.melee;
        if !rules.breaking {
            return;
        }
        let profile = melee_profile(&script(weapon), rules);
        if profile.durability <= 0 {
            return;
        }
        let Some(handle) = handle(&self.game, weapon) else {
            return;
        };
        let wear = self
            .game
            .wear
            .numbers
            .borrow()
            .get(&handle)
            .copied()
            .unwrap_or_default();
        let next = melee_wear_after(wear, hits, profile.durability);
        self.game.wear.numbers.borrow_mut().insert(handle, next);
        if next >= WEAR_BROKEN {
            self.combat.broken.insert(handle);
        } else if wear < 8000
            && next >= 8000
            && let Some(player) = owner(&self.game, weapon)
        {
            self.notice(player, "Your weapon is about to break".into());
        }
    }

    fn notice(&mut self, player: usize, text: String) {
        if self.is_local_player(player) {
            self.local.notice = text;
        } else if let Some(guest) = self
            .guests
            .values_mut()
            .find(|guest| guest.player == player)
        {
            guest.session.notice = text;
        }
    }

    fn feedback(&mut self, player: usize, heavy: bool) {
        let rules = &self.effective.melee;
        if rules.view_kick
            && rules.view_punch > 0.0
            && let Some(punch) = self.combat.punch
        {
            let amount = rules.view_punch * if heavy { 1.0 } else { 0.33 };
            let offset = Vector {
                x: amount,
                y: amount * 0.35,
                z: 0.0,
            };
            unsafe { punch(address(player), &offset) };
        }
        let line = if heavy { c"@mhi@ 1\n" } else { c"@mhi@ 0\n" };
        if self.is_local_player(player) {
            self.hud.push(line.to_string_lossy().into_owned());
        } else if let Some(hooks) = &self.hooks
            && let Some(guest) = self.guests.values().find(|guest| guest.player == player)
        {
            hooks.print(guest.edict, line);
        }
    }

    fn damage(&mut self, victim: usize, info: &[u8; INFO_SIZE], amount: f32) -> f32 {
        let attacker_handle = bytes_word(info, INFO_ATTACKER);
        let Some(attacker) = self
            .game
            .resolve(attacker_handle)
            .filter(|&actor| actor != victim && amount > 0.0)
        else {
            return amount;
        };
        let rules = &self.effective.melee;
        let now = self
            .hooks
            .as_ref()
            .and_then(Hooks::time)
            .unwrap_or_default();
        if rules.enabled
            && rules.blocking
            && word(victim + PLAYER_TEAM) == 2
            && self.game.classname(attacker).as_deref() == Some("infected")
            && !self.movement.exhausted(victim)
            && front(&self.game, victim, attacker)
            && let Some(block_since) = self
                .combat
                .players
                .get(&victim)
                .and_then(|actor| actor.block)
            && let Some(weapon) = active_melee(&self.game, victim)
        {
            let profile = melee_profile(&script(weapon), rules);
            let deflected = rules.deflecting && now - block_since <= rules.deflect;
            self.movement.spend(
                victim,
                rules.block_stamina * profile.stamina * if deflected { 0.005 } else { 0.01 },
                now,
            );
            if deflected {
                self.combat.stagger(&self.game, attacker, victim);
                return 0.0;
            }
            return amount * rules.block / 100.0;
        }
        if word(attacker + PLAYER_TEAM) != 2
            || self.game.classname(attacker).as_deref() != Some("player")
        {
            return amount;
        }
        let Some(weapon) = active_melee(&self.game, attacker) else {
            return amount;
        };
        if [INFO_INF, INFO_WEAPON]
            .into_iter()
            .all(|at| self.game.resolve(bytes_word(info, at)) != Some(weapon))
        {
            return amount;
        }
        self.combat.template.get_or_insert(*info);
        let scale = if self.movement.exhausted(attacker) {
            self.effective.stamina.damage / 100.0
        } else {
            1.0
        };
        let Some(swing) = self
            .combat
            .players
            .get_mut(&attacker)
            .and_then(|actor| actor.swung.as_mut())
            .filter(|swing| {
                rules.enabled
                    && swing.weapon == weapon
                    && now - swing.at <= 1.5
                    && word(victim + PLAYER_TEAM) != 2
            })
        else {
            return amount * scale;
        };
        let victim_id = handle(&self.game, victim).unwrap_or_default();
        let earlier = swing.victim.iter().position(|&value| value == victim_id);
        let count = earlier.unwrap_or(swing.victim.len()) as i32;
        let heavy = swing.heavy;
        if earlier.is_none() {
            swing.victim.push(victim_id);
        }
        let profile = melee_profile(&script(weapon), rules);
        let mut result = full(&self.game, victim, amount)
            * if heavy {
                profile.heavy_damage
            } else {
                profile.light_damage
            };
        result *= cleave_scale(count, profile.cleave, profile.cleave_min) * scale / 100.0;
        let class = self.game.classname(victim);
        let head = matches!(class.as_deref(), Some("player" | "infected" | "witch"))
            && word(victim + PLAYER_TEAM) == 3
            && word(victim + HIT_GROUP) == 1;
        if head {
            result *= rules.head_damage / 100.0;
        }
        let health = word(victim + HEALTH);
        if class.as_deref() == Some("infected") && rules.head_only && !head && health > 0 {
            result = result.min((health - 1) as f32);
        }
        if earlier.is_none() {
            self.movement
                .spend(attacker, rules.hit_stamina * profile.stamina / 100.0, now);
            if heavy
                && rules.hit_stagger
                && word(victim + PLAYER_TEAM) == 3
                && result < health as f32
            {
                self.combat.stagger(&self.game, victim, attacker);
            }
            self.wear_down(weapon, if heavy { 1.5 } else { 1.0 });
            if count == 0 {
                self.feedback(attacker, heavy);
            }
        }
        result
    }

    fn wall_hit(&mut self, weapon: usize) {
        let rules = &self.effective.melee;
        if !rules.enabled || !rules.wall_bounce {
            return;
        }
        let Some(actor) = owner(&self.game, weapon) else {
            return;
        };
        let now = self
            .hooks
            .as_ref()
            .and_then(Hooks::time)
            .unwrap_or_default();
        let Some(swing) = self
            .combat
            .players
            .get_mut(&actor)
            .and_then(|actor| actor.swung.as_mut())
            .filter(|swing| {
                swing.weapon == weapon
                    && now - swing.at <= 1.5
                    && !swing.bounced
                    && swing.victim.is_empty()
            })
        else {
            return;
        };
        if self.pings.wall_ahead(&self.game, actor) {
            swing.bounced = true;
            self.wear_down(weapon, 3.0);
            self.feedback(actor, true);
        }
    }

    fn enemies(&self) -> impl Iterator<Item = usize> + '_ {
        (1..2048)
            .map(|index| self.game.entity(index))
            .filter(|&entity| {
                if entity == 0 || !alive(entity) {
                    return false;
                }
                match self.game.classname(entity).as_deref() {
                    Some("infected" | "witch") => true,
                    Some("player") => word(entity + PLAYER_TEAM) == 3,
                    _ => false,
                }
            })
    }

    fn throw_melee(&mut self, player: usize, weapon: usize, now: f32) -> bool {
        let Some((eye, forward)) = self.game.eyes(player) else {
            return false;
        };
        let speed = self.effective.melee.throw_speed;
        let velocity = Vector {
            x: speed * forward.x,
            y: speed * forward.y,
            z: speed * forward.z + 120.0,
        };
        let target = Vector {
            x: eye.x + 200.0 * forward.x,
            y: eye.y + 200.0 * forward.y,
            z: eye.z + 200.0 * forward.z,
        };
        unsafe { (self.game.quiet_drop)(address(player), address(weapon), &target, &velocity) };
        if owner(&self.game, weapon).is_some() {
            return false;
        }
        let start = Vector {
            x: eye.x + 24.0 * forward.x,
            y: eye.y + 24.0 * forward.y,
            z: eye.z + 24.0 * forward.z - 4.0,
        };
        if let Some(method) =
            native::vslot(weapon, 118).filter(|&at| self.game.image.includes(at, 1))
        {
            let teleport = unsafe { std::mem::transmute::<usize, Teleport>(method) };
            let angles = Vector {
                x: -forward.z.asin().to_degrees(),
                y: forward.y.atan2(forward.x).to_degrees(),
                z: 0.0,
            };
            unsafe { teleport(address(weapon), &start, &angles, &velocity) };
        }
        let targets = self
            .enemies()
            .filter_map(|entity| {
                let at = Vector::read(entity + ENTITY_ORIGIN)?;
                ((at.x - eye.x).hypot(at.y - eye.y) < 1500.0)
                    .then(|| handle(&self.game, entity))
                    .flatten()
            })
            .collect::<Vec<_>>();
        if let (Some(weapon), Some(player)) =
            (handle(&self.game, weapon), handle(&self.game, player))
        {
            self.combat.thrown.push(Thrown {
                weapon,
                player,
                at: now,
                last: start,
                targets,
            });
            true
        } else {
            false
        }
    }

    fn thrown_frame(&mut self, now: f32) {
        let original = DAMAGE.load(Ordering::Acquire);
        if original == 0 {
            return;
        }
        let deal = unsafe { std::mem::transmute::<usize, Damage>(original) };
        for mut flight in std::mem::take(&mut self.combat.thrown) {
            let Some(weapon) = self
                .game
                .resolve(flight.weapon)
                .filter(|&weapon| now - flight.at <= 1.6 && owner(&self.game, weapon).is_none())
            else {
                continue;
            };
            let Some(at) = Vector::read(weapon + ENTITY_ORIGIN) else {
                continue;
            };
            let last = std::mem::replace(&mut flight.last, at);
            let delta = at.difference(last);
            let target = flight.targets.iter().find_map(|&id| {
                let actor = self.game.resolve(id)?;
                if !alive(actor) {
                    return None;
                }
                let position = Vector::read(actor + ENTITY_ORIGIN)?;
                let center = Vector {
                    z: position.z + 40.0,
                    ..position
                };
                let offset = center.difference(last);
                let length = delta.dot(delta);
                let ratio = if length > 0.0 {
                    (offset.dot(delta) / length).clamp(0.0, 1.0)
                } else {
                    0.0
                };
                let difference = Vector {
                    x: offset.x - ratio * delta.x,
                    y: offset.y - ratio * delta.y,
                    z: offset.z - ratio * delta.z,
                };
                (difference.dot(difference) <= 36.0 * 36.0).then_some((actor, center))
            });
            let Some((target, at)) = target else {
                self.combat.thrown.push(flight);
                continue;
            };
            let player = self.game.resolve(flight.player);
            let amount =
                full(&self.game, target, 100.0) * self.effective.melee.throw_damage / 100.0;
            let mut buffer = Aligned(self.combat.template.unwrap_or([0; INFO_SIZE]));
            if self.combat.template.is_none() {
                set_word(&mut buffer.0, 0x48, 0x80);
            }
            for (offset, value) in [
                (0, (delta.x * 300.0).to_bits()),
                (4, (delta.y * 300.0).to_bits()),
                (8, 0),
                (12, at.x.to_bits()),
                (16, at.y.to_bits()),
                (20, at.z.to_bits()),
                (INFO_INF, flight.weapon),
                (INFO_ATTACKER, flight.player),
                (INFO_WEAPON, flight.weapon),
                (INFO_DAMAGE, amount.to_bits()),
            ] {
                set_word(&mut buffer.0, offset, value);
            }
            unsafe { deal(address(target), buffer.0.as_ptr().cast()) };
            if let Some(player) = player {
                if alive(target) {
                    self.combat.stagger(&self.game, target, player);
                }
                self.feedback(player, true);
            }
            self.wear_down(weapon, 2.0);
        }
    }

    pub fn combat_frame(&mut self) {
        if !self.combat.available() || !self.effective.melee.enabled {
            return;
        }
        let Some(now) = self.hooks.as_ref().and_then(Hooks::time) else {
            return;
        };
        let mut states = std::mem::take(&mut self.combat.players);
        for index in 1..65 {
            let player = self.game.entity(index);
            if player == 0 {
                continue;
            }
            if !self.game.living(player) || self.game.classname(player).as_deref() != Some("player")
            {
                continue;
            }
            let mut state = states.remove(&player).unwrap_or_default();
            let held = active_melee(&self.game, player);
            let buttons = word(player + BUTTONS);
            if let Some(weapon) = held.filter(|_| buttons & BUTTON_WINDING != 0) {
                let (start, last) = state.winding.get_or_insert_with(|| {
                    if self.effective.melee.gesture {
                        self.combat.gesture(player, weapon);
                    }
                    (now, now)
                });
                *last = now.max(*start);
            } else if let Some((start, last)) = state.winding.take() {
                state.wound = Some((now, last - start));
            }
            let blocking = held.is_some()
                && state.winding.is_none()
                && buttons & BUTTON_RELOAD != 0
                && self.effective.melee.blocking
                && !self.movement.exhausted(player);
            if blocking {
                state.block.get_or_insert(now);
            } else {
                state.block = None;
            }
            if let Some(weapon) = held.filter(|_| {
                self.effective.melee.throwing
                    && buttons & BUTTON_THROW != 0
                    && now - state.thrown_at > 0.5
            }) && self.throw_melee(player, weapon, now)
            {
                state.thrown_at = now;
            }
            self.combat.players.insert(player, state);
        }
        self.thrown_frame(now);
        for broken in std::mem::take(&mut self.combat.broken) {
            if let Some(weapon) = self.game.resolve(broken) {
                if let Some(player) = owner(&self.game, weapon) {
                    self.notice(player, format!("Your {} broke", script(weapon)));
                    unsafe {
                        (self.game.quiet_drop)(
                            address(player),
                            address(weapon),
                            ptr::null(),
                            ptr::null(),
                        )
                    };
                }
                self.game.remove(weapon);
                self.game.wear.numbers.borrow_mut().remove(&broken);
            }
        }
    }
}

#[repr(align(16))]
struct Aligned([u8; INFO_SIZE]);

fn with_host<T>(mut f: impl FnMut(&mut Host) -> T) -> Option<T> {
    with_client(|client| client.host.as_mut().map(&mut f))
        .flatten()
        .or_else(|| dedicated::with_host(f))
}

unsafe extern "thiscall" fn primary(weapon: *mut c_void) {
    unsafe { invoke_attack(weapon, false, &PRIMARY) };
}

unsafe extern "thiscall" fn secondary(weapon: *mut c_void) {
    unsafe { invoke_attack(weapon, true, &SECONDARY) };
}

unsafe fn invoke_attack(weapon: *mut c_void, shove: bool, pointer: &AtomicUsize) {
    let original = pointer.load(Ordering::Acquire);
    if original == 0 {
        return;
    }
    let prepared =
        std::panic::catch_unwind(|| with_host(|host| host.prepare_attack(weapon.addr(), shove)))
            .ok()
            .flatten()
            .flatten();
    let original = unsafe { std::mem::transmute::<usize, Attack>(original) };
    unsafe { original(weapon) };
    if let Some(prepared) = prepared {
        let _ = std::panic::catch_unwind(|| {
            with_host(|host| host.complete_attack(prepared));
        });
    }
}

unsafe extern "thiscall" fn sound(weapon: *mut c_void, sound: i32, at: f32) {
    let original = SOUND.load(Ordering::Acquire);
    if original == 0 {
        return;
    }
    let original = unsafe { std::mem::transmute::<usize, Sound>(original) };
    unsafe { original(weapon, sound, at) };
    if sound == 10 {
        let _ = std::panic::catch_unwind(|| with_host(|host| host.wall_hit(weapon.addr())));
    }
}

unsafe extern "thiscall" fn damage(victim: *mut c_void, info: *const c_void) -> i32 {
    let original = DAMAGE.load(Ordering::Acquire);
    if original == 0 {
        return 0;
    }
    let original = unsafe { std::mem::transmute::<usize, Damage>(original) };
    let mut buffer = Aligned([0; INFO_SIZE]);
    if !native::read(info.addr(), &mut buffer.0) {
        return unsafe { original(victim, info) };
    }
    let amount = f32::from_bits(bytes_word(&buffer.0, INFO_DAMAGE));
    let chosen = std::panic::catch_unwind(|| {
        with_host(|host| host.damage(victim.addr(), &buffer.0, amount))
    })
    .ok()
    .flatten()
    .unwrap_or(amount);
    if chosen != amount && chosen.is_finite() {
        set_word(&mut buffer.0, INFO_DAMAGE, chosen.to_bits());
        unsafe { original(victim, buffer.0.as_ptr().cast()) }
    } else {
        unsafe { original(victim, info) }
    }
}
