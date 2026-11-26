use std::collections::{HashMap, HashSet};

use inventory_mod::{
    melee::{BUTTON_ATTACK2, BUTTON_RELOAD, BUTTON_WINDING, melee_profile},
    rules::{ChapterRules, StaminaRules},
};

use super::{Game, Host, combat::Combat, hooks::Hooks};
use crate::native;

const PLAYER_FLAGS: usize = 0x13c;
const PLAYER_BUTTONS: usize = 0x1cb4;
const IN_AIR_VELOCITY: usize = 0x2d8;
const REAL_VELOCITY: usize = 0x260;
const MOVEMENT_SCALE: usize = 0x20d0;
const SHOVE_PENALTY: usize = 0x2cf0;
const ADRENALINE: usize = 0x328c;
const NEXT_ATTACK: usize = 0x13f0;
const NEXT_SHOVE: usize = 0x13f4;

#[derive(Default)]
pub struct Movement {
    players: HashMap<usize, Player>,
    last: Option<f32>,
}

struct Player {
    amount: f32,
    exhausted: bool,
    spent_at: f32,
    grounded: bool,
    eased_speed: f32,
    original_speed: f32,
    applied_speed: f32,
    last_weapon: Option<usize>,
    last_primary: f32,
    last_secondary: f32,
    sent: Option<(i32, bool, f32)>,
    winding: Option<f32>,
    melee_sent: Option<(i32, i32, f32)>,
}

impl Player {
    fn new(player: usize) -> Self {
        let speed = field_float(player + MOVEMENT_SCALE).unwrap_or(1.0);
        let grounded = field_int(player + PLAYER_FLAGS) & 1 != 0;
        Self {
            amount: 100.0,
            exhausted: false,
            spent_at: -1000.0,
            grounded,
            eased_speed: 1.0,
            original_speed: speed,
            applied_speed: speed,
            last_weapon: None,
            last_primary: 0.0,
            last_secondary: 0.0,
            sent: None,
            winding: None,
            melee_sent: None,
        }
    }

    fn spend(&mut self, cost: f32, now: f32) {
        if cost > 0.0 {
            self.amount = (self.amount - cost).max(0.0);
            self.spent_at = now;
            self.exhausted |= self.amount <= 0.0;
        }
    }

    fn update(
        &mut self,
        game: &Game,
        hooks: &Hooks,
        actor: usize,
        rules: &ChapterRules,
        now: f32,
        dt: f32,
    ) {
        let stamina = &rules.stamina;
        let melee_rules = &rules.melee;
        let grounded = field_int(actor + PLAYER_FLAGS) & 1 != 0;
        let vx = field_float(actor + IN_AIR_VELOCITY).unwrap_or_default();
        let vy = field_float(actor + IN_AIR_VELOCITY + 4).unwrap_or_default();
        let vz = field_float(actor + IN_AIR_VELOCITY + 8).unwrap_or_default();
        let running = grounded && vx.hypot(vy) > 130.0;
        let mut adrenaline = [0];
        native::read(actor + ADRENALINE, &mut adrenaline);
        let adrenal = adrenaline[0] != 0;
        if adrenal {
            self.amount = 100.0;
            self.exhausted = false;
        }
        let before = self.exhausted;
        if stamina.enabled && !adrenal {
            if running && !self.exhausted {
                self.spend(stamina.run * dt, now);
            }
            if self.grounded && !grounded {
                if vz > 150.0 {
                    self.spend(stamina.jump, now);
                }
                if self.eased_speed < 1.0 {
                    for offset in [REAL_VELOCITY, IN_AIR_VELOCITY] {
                        for axis in [0, 4] {
                            let field = actor + offset + axis;
                            if let Some(speed) = field_float(field) {
                                native::store_word(field, (speed * self.eased_speed).to_bits());
                            }
                        }
                    }
                    hooks.changed(actor);
                }
            }
        }
        self.grounded = grounded;

        let penalty = field_int(actor + SHOVE_PENALTY);
        if stamina.enabled && penalty != 0 {
            native::store_word(actor + SHOVE_PENALTY, 0);
        }
        let buttons = field_int(actor + PLAYER_BUTTONS);
        let weapon = game.active(actor);
        let is_melee =
            weapon.and_then(|weapon| game.classname(weapon)).as_deref() == Some("weapon_melee");
        if let Some(weapon) = weapon {
            let mut primary = field_float(weapon + NEXT_ATTACK).unwrap_or_default();
            let mut secondary = field_float(weapon + NEXT_SHOVE).unwrap_or_default();
            if stamina.enabled && self.last_weapon == Some(weapon) {
                let primary_moved = primary > self.last_primary + 0.05 && primary > now;
                let secondary_moved =
                    secondary > self.last_secondary + 0.05 && secondary > now + 0.4;
                let shove = secondary_moved
                    && (penalty != 0
                        || buttons & BUTTON_ATTACK2 != 0
                        || !is_melee
                        || !primary_moved);
                let swing = !shove && is_melee && primary_moved;
                if shove || swing {
                    if before && stamina.swing_speed < 100.0 {
                        let stretch = 100.0 / stamina.swing_speed;
                        if primary > now {
                            primary = now + (primary - now) * stretch;
                            native::store_word(weapon + NEXT_ATTACK, primary.to_bits());
                        }
                        if shove && secondary > now {
                            secondary = now + (secondary - now) * stretch;
                            native::store_word(weapon + NEXT_SHOVE, secondary.to_bits());
                        }
                        hooks.changed(weapon);
                    }
                    if !adrenal {
                        self.spend(if shove { stamina.shove } else { stamina.swing }, now);
                    }
                }
            }
            self.last_primary = primary;
            self.last_secondary = secondary;
        }
        self.last_weapon = weapon;

        let target = if stamina.enabled && self.exhausted {
            stamina.slow / 100.0
        } else {
            1.0
        };
        let step = 1.5 * dt;
        self.eased_speed += (target - self.eased_speed).clamp(-step, step);
        if stamina.enabled {
            if now - self.spent_at >= stamina.regen_delay {
                let multiplier = if running { 0.5 } else { 1.0 };
                self.amount = (self.amount + stamina.regen * dt * multiplier).min(100.0);
            }
            if self.exhausted && self.amount >= stamina.recover.max(0.1) {
                self.exhausted = false;
            }
        }

        let mut battle_scale = 1.0;
        if melee_rules.enabled && is_melee {
            let script = weapon.map(Game::melee_script).unwrap_or_default();
            let profile = melee_profile(&script, melee_rules);
            if buttons & BUTTON_WINDING != 0 {
                let since = *self.winding.get_or_insert(now);
                if melee_rules.wind_slowdown {
                    let progress = ((now - since) / profile.heavy_hold.max(0.1)).clamp(0.0, 1.0);
                    battle_scale = 1.0 + progress * (melee_rules.wind_speed / 100.0 - 1.0);
                }
                if stamina.enabled && !adrenal {
                    self.spend(melee_rules.wind_stamina * profile.stamina * dt / 100.0, now);
                }
            } else {
                self.winding = None;
                if melee_rules.blocking && buttons & BUTTON_RELOAD != 0 && !self.exhausted {
                    battle_scale = melee_rules.block_speed / 100.0;
                }
            }
        } else {
            self.winding = None;
        }

        let current = field_float(actor + MOVEMENT_SCALE).unwrap_or(1.0);
        if current != self.applied_speed {
            self.original_speed = current;
        }
        let scale = if grounded {
            self.eased_speed * battle_scale
        } else {
            1.0
        };
        self.applied_speed = self.original_speed * scale;
        if current != self.applied_speed {
            native::store_word(actor + MOVEMENT_SCALE, self.applied_speed.to_bits());
            hooks.changed(actor);
        }
    }

    fn stamina_update(&mut self, rules: &StaminaRules, now: f32) -> Option<String> {
        if !rules.enabled {
            return self.sent.take().map(|_| "@sta@ -1 0\n".to_owned());
        }
        let amount = (self.amount * 10.0).round() as i32;
        if let Some((saved, exhausted, at)) = self.sent
            && ((saved, exhausted) == (amount, self.exhausted)
                || (exhausted == self.exhausted && amount != 0 && amount != 1000 && now - at < 0.1))
        {
            return None;
        }
        self.sent = Some((amount, self.exhausted, now));
        Some(format!("@sta@ {amount} {}\n", u8::from(self.exhausted)))
    }

    fn melee_update(
        &mut self,
        game: &Game,
        actor: usize,
        rules: &ChapterRules,
        combat: &Combat,
        now: f32,
    ) -> Option<String> {
        let rules = &rules.melee;
        if !rules.enabled || !combat.available() {
            return self.melee_sent.take().map(|_| "@mel@ 0 0 0\n".to_owned());
        }
        let script = game
            .active(actor)
            .filter(|&weapon| game.classname(weapon).as_deref() == Some("weapon_melee"))
            .map(Game::melee_script)
            .unwrap_or_default();
        let profile = melee_profile(&script, rules);
        let hold = if rules.heavy {
            (profile.heavy_hold * 1000.0).round() as i32
        } else {
            0
        };
        let shove = if rules.heavy_shove && combat.has_shove() {
            (rules.heavy_shove_hold * 1000.0).round() as i32
        } else {
            0
        };
        if self.melee_sent.is_some_and(|(old_hold, old_shove, sent)| {
            hold == old_hold && shove == old_shove && now - sent < 5.0
        }) {
            return None;
        }
        self.melee_sent = Some((hold, shove, now));
        let features = combat.features(rules);
        Some(format!("@mel@ {hold} {shove} {features}\n"))
    }
}

fn field_float(addr: usize) -> Option<f32> {
    native::word(addr)
        .map(f32::from_bits)
        .filter(|n| n.is_finite())
}

fn field_int(addr: usize) -> i32 {
    native::word(addr).unwrap_or_default() as i32
}

impl Movement {
    pub(super) fn exhausted(&self, player: usize) -> bool {
        self.players
            .get(&player)
            .is_some_and(|state| state.exhausted)
    }

    pub(super) fn spend(&mut self, player: usize, amount: f32, now: f32) {
        if let Some(state) = self.players.get_mut(&player) {
            let mut adrenaline = [0];
            native::read(player + ADRENALINE, &mut adrenaline);
            if adrenaline[0] == 0 {
                if amount < 0.0 {
                    state.amount = (state.amount - amount).min(100.0);
                } else {
                    state.spend(amount, now);
                }
            }
        }
    }
    fn reset(&mut self, hooks: &Hooks) {
        for (player, state) in std::mem::take(&mut self.players) {
            restore(player, state, hooks);
        }
        self.last = None;
    }
}

fn restore(player: usize, state: Player, hooks: &Hooks) {
    if state.original_speed != state.applied_speed
        && field_float(player + MOVEMENT_SCALE) == Some(state.applied_speed)
    {
        native::store_word(player + MOVEMENT_SCALE, state.original_speed.to_bits());
        hooks.changed(player);
    }
}

impl Host {
    pub fn movement_frame(&mut self) {
        let Some(hooks) = &self.hooks else {
            return;
        };
        let Some(now) = hooks.time() else {
            return;
        };
        let elapsed = now - self.movement.last.unwrap_or(now);
        if elapsed < 0.0 {
            self.movement.reset(hooks);
        }
        if self.movement.last == Some(now) {
            return;
        }
        self.movement.last = Some(now);
        let dt = if (0.0..=0.25).contains(&elapsed) {
            elapsed
        } else {
            0.0
        };
        let rules = &self.effective;
        if !rules.stamina.enabled && !rules.melee.enabled {
            self.movement.reset(hooks);
            return;
        }
        let local = if self.dedicated {
            None
        } else {
            self.game.host()
        };
        let mut seen = HashSet::new();
        for actor in (1..65)
            .map(|index| self.game.entity(index))
            .filter(|&player| player != 0)
        {
            if self.game.classname(actor).as_deref() != Some("player") || !self.game.living(actor) {
                continue;
            }
            let edict = native::word(actor + 0x28).unwrap_or_default() as usize;
            let bot = hooks.steam_id(edict).as_deref() == Some("BOT");
            if bot && !rules.stamina.bots && !rules.melee.enabled {
                continue;
            }
            let state = self
                .movement
                .players
                .entry(actor)
                .or_insert_with(|| Player::new(actor));
            state.update(&self.game, hooks, actor, rules, now, dt);
            seen.insert(actor);
            let guest = hooks
                .user(edict)
                .is_some_and(|id| self.guests.contains_key(&id));
            if bot || !guest && local != Some(actor) {
                continue;
            }
            for line in [
                state.stamina_update(&rules.stamina, now),
                state.melee_update(&self.game, actor, rules, &self.combat, now),
            ]
            .into_iter()
            .flatten()
            {
                if local == Some(actor) {
                    self.hud.push(line);
                } else if let Ok(line) = std::ffi::CString::new(line) {
                    hooks.print(edict, &line);
                }
            }
        }
        let missing = self
            .movement
            .players
            .keys()
            .copied()
            .filter(|player| !seen.contains(player))
            .collect::<Vec<_>>();
        for missing in missing {
            if let Some(state) = self.movement.players.remove(&missing) {
                restore(missing, state, hooks);
            }
        }
    }

    pub fn reset_movement(&mut self, restore_velocity: bool) {
        if restore_velocity && let Some(hooks) = &self.hooks {
            self.movement.reset(hooks);
        } else {
            self.movement = Movement::default();
        }
    }
}
