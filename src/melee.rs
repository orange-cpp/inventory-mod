use crate::rules::MeleeRules;

pub const BUTTON_ATTACK: i32 = 1;
pub const BUTTON_ATTACK2: i32 = 1 << 11;
pub const BUTTON_RELOAD: i32 = 1 << 13;
pub const BUTTON_THROW: i32 = 1 << 27;
pub const BUTTON_WINDING: i32 = 1 << 28;
pub const BUTTON_HEAVY: i32 = 1 << 29;
pub const BUTTON_HEAVY_SHOVE: i32 = 1 << 30;

#[derive(Clone, Copy, Debug)]
pub struct MeleeFrame {
    pub buttons: i32,
    pub now: f64,
    pub melee: bool,
    pub can_heavy: bool,
    pub next_attack: f32,
    pub next_shove: f32,
    pub game_time: f32,
    pub hold: f32,
    pub shove_hold: f32,
    pub throwing: bool,
}

impl Default for MeleeFrame {
    fn default() -> Self {
        Self {
            buttons: 0,
            now: 0.0,
            melee: false,
            can_heavy: true,
            next_attack: 0.0,
            next_shove: 0.0,
            game_time: 0.0,
            hold: 0.0,
            shove_hold: 0.0,
            throwing: true,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub enum Swing {
    #[default]
    Idle,
    Winding {
        since: f64,
        can_heavy: bool,
    },
    Cancelled,
    Releasing(Release),
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub enum Shove {
    #[default]
    Idle,
    Holding {
        since: f64,
    },
    Passing,
    Releasing(Release),
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Release {
    pub heavy: bool,
    at: f64,
    next: f32,
    pressed_again_at: Option<f64>,
}

impl Release {
    fn new(heavy: bool, at: f64, next: f32) -> Self {
        Self {
            heavy,
            at,
            next,
            pressed_again_at: None,
        }
    }

    fn finished(&mut self, now: f64, clock: f32, next: f32) -> bool {
        // The server can correct the recovery time before a predicted attack fires.
        if self.next != next && self.next > clock + 0.3 {
            self.next = next;
        }
        self.next != next || now - self.at > 0.1
    }
}

#[derive(Clone, Debug, Default)]
pub struct MeleeInput {
    swing: Swing,
    shove: Shove,
    thrown_at: Option<f64>,
    throw_until: Option<f64>,
    last_buttons: i32,
}

impl MeleeInput {
    pub fn swing(&self) -> Swing {
        self.swing
    }

    pub fn shove(&self) -> Shove {
        self.shove
    }

    pub fn thrown_at(&self) -> Option<f64> {
        self.thrown_at
    }

    pub fn command(&mut self, frame: MeleeFrame) -> i32 {
        let buttons = frame.buttons;
        let attack = buttons & BUTTON_ATTACK != 0;
        let shove = buttons & BUTTON_ATTACK2 != 0;
        let pressed = buttons & !std::mem::replace(&mut self.last_buttons, buttons);
        let pressed_attack = pressed & BUTTON_ATTACK != 0;
        let pressed_shove = pressed & BUTTON_ATTACK2 != 0;
        let pressed_reload = pressed & BUTTON_RELOAD != 0;

        if frame.hold <= 0.0 || !frame.melee {
            self.swing = Swing::Idle;
            self.shove = Shove::Idle;
            self.throw_until = None;
            return buttons;
        }

        if let Swing::Releasing(release) = &mut self.swing {
            if pressed_attack {
                release.pressed_again_at = Some(frame.now);
            }
            if release.finished(frame.now, frame.game_time, frame.next_attack) {
                self.swing = match release.pressed_again_at.filter(|_| attack) {
                    Some(since) => Swing::Winding {
                        since,
                        can_heavy: frame.can_heavy,
                    },
                    None => Swing::Idle,
                };
            }
        }
        if matches!(self.swing, Swing::Winding { .. }) {
            if pressed_reload && frame.throwing {
                self.swing = Swing::Cancelled;
                self.thrown_at = Some(frame.now);
                self.throw_until = Some(frame.now + 0.15);
            } else if pressed_shove {
                self.swing = Swing::Cancelled;
                self.shove = Shove::Passing;
            }
        }
        if self.swing == Swing::Cancelled && !attack {
            self.swing = Swing::Idle;
        }
        if let Swing::Winding { since, can_heavy } = self.swing {
            let exhausted = can_heavy && !frame.can_heavy;
            if !attack || exhausted {
                let heavy =
                    can_heavy && frame.can_heavy && frame.now - since >= f64::from(frame.hold);
                self.swing = Swing::Releasing(Release::new(heavy, frame.now, frame.next_attack));
            }
        }
        if self.swing == Swing::Idle && attack {
            self.swing = Swing::Winding {
                since: frame.now,
                can_heavy: frame.can_heavy,
            };
        }

        if let Shove::Releasing(release) = &mut self.shove
            && release.finished(frame.now, frame.game_time, frame.next_shove)
        {
            self.shove = Shove::Idle;
        }
        if self.shove == Shove::Passing && !shove {
            self.shove = Shove::Idle;
        }
        if let Shove::Holding { since } = self.shove
            && !shove
        {
            let heavy = frame.shove_hold > 0.0
                && frame.can_heavy
                && frame.now - since >= f64::from(frame.shove_hold);
            self.shove = Shove::Releasing(Release::new(heavy, frame.now, frame.next_shove));
        }
        if self.shove == Shove::Idle && pressed_shove {
            self.shove = if frame.shove_hold > 0.0 && !matches!(self.swing, Swing::Winding { .. }) {
                Shove::Holding { since: frame.now }
            } else {
                Shove::Passing
            };
        }
        if self.throw_until.is_some_and(|until| frame.now >= until) {
            self.throw_until = None;
        }
        self.peek(buttons)
    }

    pub fn peek(&self, mut buttons: i32) -> i32 {
        match self.swing {
            Swing::Idle => {}
            Swing::Winding { .. } => buttons = (buttons & !BUTTON_ATTACK) | BUTTON_WINDING,
            Swing::Cancelled => buttons &= !BUTTON_ATTACK,
            Swing::Releasing(release) => {
                buttons |= BUTTON_ATTACK;
                if release.heavy {
                    buttons |= BUTTON_HEAVY;
                }
            }
        }
        match self.shove {
            Shove::Idle | Shove::Passing => {}
            Shove::Holding { .. } => buttons &= !BUTTON_ATTACK2,
            Shove::Releasing(release) => {
                buttons |= BUTTON_ATTACK2;
                if release.heavy {
                    buttons |= BUTTON_HEAVY_SHOVE;
                }
            }
        }
        if self.throw_until.is_some() {
            buttons = (buttons | BUTTON_THROW) & !BUTTON_RELOAD;
        }
        buttons
    }
}

pub fn cleave_scale(earlier_targets: i32, cleave: f32, minimum: f32) -> f32 {
    if earlier_targets <= 0 {
        return 1.0;
    }
    (cleave.clamp(0.0, 100.0) / 100.0)
        .powf(earlier_targets as f32)
        .max(minimum.clamp(0.0, 100.0) / 100.0)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MeleeClass {
    Blade1h,
    Blunt1h,
    Blade2h,
    Blunt2h,
}

impl MeleeClass {
    pub fn name(self) -> &'static str {
        match self {
            Self::Blade1h => "blade_1h",
            Self::Blunt1h => "blunt_1h",
            Self::Blade2h => "blade_2h",
            Self::Blunt2h => "blunt_2h",
        }
    }

    pub fn from_name(name: &str) -> Option<Self> {
        [Self::Blade1h, Self::Blunt1h, Self::Blade2h, Self::Blunt2h]
            .into_iter()
            .find(|class| class.name() == name)
    }

    pub fn from_weapon(weapon: &str) -> Self {
        match weapon {
            "machete" | "knife" => Self::Blade1h,
            "fireaxe" | "katana" | "pitchfork" => Self::Blade2h,
            "baseball_bat" | "cricket_bat" | "electric_guitar" | "golfclub" | "shovel" => {
                Self::Blunt2h
            }
            _ => Self::Blunt1h,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MeleeProfile {
    pub class: MeleeClass,
    pub heavy_hold: f32,
    pub light_damage: f32,
    pub heavy_damage: f32,
    pub cleave: f32,
    pub cleave_min: f32,
    pub stamina: f32,
    pub durability: i32,
}

pub fn melee_profile(weapon: &str, rules: &MeleeRules) -> MeleeProfile {
    let overrides = rules.weapons.get(weapon);
    let class = overrides
        .and_then(|rule| rule.class)
        .unwrap_or_else(|| MeleeClass::from_weapon(weapon));
    let (hold, light, heavy, stamina, cleave, cleave_min, durability) = match class {
        MeleeClass::Blade1h => (0.85, 100.0, 90.0, 80.0, 65.0, 30.0, 150.0),
        MeleeClass::Blunt1h => (0.9, 95.0, 100.0, 90.0, 35.0, 15.0, 250.0),
        MeleeClass::Blade2h => (1.15, 115.0, 115.0, 125.0, 85.0, 40.0, 200.0),
        MeleeClass::Blunt2h => (1.25, 110.0, 125.0, 135.0, 55.0, 25.0, 300.0),
    };
    MeleeProfile {
        class,
        heavy_hold: overrides
            .and_then(|rule| rule.heavy_hold)
            .unwrap_or(rules.heavy_hold * hold),
        light_damage: overrides
            .and_then(|rule| rule.light_damage)
            .unwrap_or(rules.light_damage * light / 100.0),
        heavy_damage: overrides
            .and_then(|rule| rule.heavy_damage)
            .unwrap_or(rules.heavy_damage * heavy / 100.0),
        cleave: overrides
            .and_then(|rule| rule.cleave)
            .unwrap_or((cleave * rules.cleave / 70.0).clamp(0.0, 100.0)),
        cleave_min: (cleave_min * rules.cleave_min / 30.0).clamp(0.0, 100.0),
        stamina: overrides.and_then(|rule| rule.stamina).unwrap_or(stamina),
        durability: overrides
            .and_then(|rule| rule.durability)
            .unwrap_or_else(|| (durability * rules.durability / 100.0).round() as i32),
    }
}

pub fn melee_overhead_swing(directions: &[i32]) -> Option<usize> {
    directions
        .iter()
        .position(|&dir| dir == 0)
        .or_else(|| directions.iter().position(|&dir| matches!(dir, 4 | 7)))
}

pub const WEAR_BROKEN: i32 = 10_000;

pub fn melee_wear_after(wear: i32, hits: f32, durability: i32) -> i32 {
    let wear = if durability > 0 && hits > 0.0 {
        wear.saturating_add((WEAR_BROKEN as f32 / durability as f32 * hits).round() as i32)
    } else {
        wear
    };
    wear.clamp(0, WEAR_BROKEN)
}
