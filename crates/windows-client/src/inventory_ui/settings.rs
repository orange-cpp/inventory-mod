use inventory_mod::settings::{Keys, Settings, View};

pub(super) fn key_field(keys: &mut Keys, index: usize) -> &mut i32 {
    match index {
        0 => &mut keys.toggle,
        1 => &mut keys.previous,
        2 => &mut keys.next,
        3 => &mut keys.take,
        4 => &mut keys.drop,
        5 => &mut keys.store,
        6 => &mut keys.pickup,
        7 => &mut keys.unload,
        _ => &mut keys.ping,
    }
}

pub(super) fn key_name(key: i32) -> String {
    match key {
        0 => "None".into(),
        0x08 => "Backspace".into(),
        0x09 => "Tab".into(),
        0x0d => "Enter".into(),
        0x20 => "Space".into(),
        0x30..=0x39 | 0x41..=0x5a => char::from(key as u8).to_string(),
        0xba => ";".into(),
        0xbb => "=".into(),
        0xbc => ",".into(),
        0xbd => "-".into(),
        0xbe => ".".into(),
        0xbf => "/".into(),
        0xdb => "[".into(),
        0xdc => "\\".into(),
        0xdd => "]".into(),
        0xde => "'".into(),
        0x70..=0x87 => format!("F{}", key - 0x6f),
        _ => format!("Key {key}"),
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub(super) enum Setting {
    Hotbar,
    View,
    WheelSounds,
    AutoStore,
    ReplacePanel,
    Scale,
    Effect,
    InfectionGameHud,
    InfectionModHud,
    Sound,
    Screamer,
    Chance,
    StaminaBar,
    Vignette,
    StaminaGameHud,
    StaminaModHud,
    Melee,
    Slots,
    Weapons,
    Weight,
}

impl Setting {
    pub(super) fn visible(self, cfg: &Settings) -> bool {
        match self {
            Self::InfectionGameHud => cfg.infection.effect,
            Self::InfectionModHud => cfg.infection.effect && cfg.infection.cover_game_hud,
            Self::Chance => cfg.infection.screamer,
            Self::StaminaGameHud => cfg.stamina.vignette,
            Self::StaminaModHud => cfg.stamina.vignette && cfg.stamina.cover_game_hud,
            _ => true,
        }
    }

    pub(super) const GROUPS: &[(&str, &[Self])] = &[
        (
            "HUD",
            &[
                Self::Hotbar,
                Self::View,
                Self::WheelSounds,
                Self::AutoStore,
                Self::ReplacePanel,
                Self::Scale,
            ],
        ),
        (
            "Infection",
            &[
                Self::Effect,
                Self::InfectionGameHud,
                Self::InfectionModHud,
                Self::Sound,
                Self::Screamer,
                Self::Chance,
            ],
        ),
        (
            "Stamina",
            &[
                Self::StaminaBar,
                Self::Vignette,
                Self::StaminaGameHud,
                Self::StaminaModHud,
            ],
        ),
        ("Melee", &[Self::Melee]),
        ("Limits", &[Self::Slots, Self::Weapons, Self::Weight]),
    ];

    pub(super) fn name(self) -> &'static str {
        match self {
            Self::Hotbar => "Show hotbar",
            Self::View => "Inventory view",
            Self::WheelSounds => "Wheel sounds",
            Self::AutoStore => "Store items on use",
            Self::ReplacePanel => "Replace the game's weapon panel",
            Self::Scale => "Scale",
            Self::Effect => "Screen effect",
            Self::InfectionGameHud => "Effect covers the game's HUD",
            Self::InfectionModHud => "Effect covers the mod's window",
            Self::Sound => "Sound distortion and heartbeat",
            Self::Screamer => "Screamer when the clock runs out",
            Self::Chance => "Screamer chance",
            Self::StaminaBar => "Bar under the crosshair",
            Self::Vignette => "Vignette as it runs low",
            Self::StaminaGameHud => "Vignette covers the game's HUD",
            Self::StaminaModHud => "Vignette covers the mod's window",
            Self::Melee => "Wind-up ring at the crosshair",
            Self::Slots => "Max item boxes",
            Self::Weapons => "Max weapon boxes",
            Self::Weight => "Max weight",
        }
    }

    pub(super) fn field(self, cfg: &mut Settings) -> Option<&mut bool> {
        Some(match self {
            Self::Hotbar => &mut cfg.hud.show,
            Self::WheelSounds => &mut cfg.hud.wheel_sounds,
            Self::AutoStore => &mut cfg.auto_store,
            Self::ReplacePanel => &mut cfg.hud.replace_native_panel,
            Self::Effect => &mut cfg.infection.effect,
            Self::InfectionGameHud => &mut cfg.infection.cover_game_hud,
            Self::InfectionModHud => &mut cfg.infection.cover_mod_hud,
            Self::Sound => &mut cfg.infection.sound,
            Self::Screamer => &mut cfg.infection.screamer,
            Self::StaminaBar => &mut cfg.stamina.bar,
            Self::Vignette => &mut cfg.stamina.vignette,
            Self::StaminaGameHud => &mut cfg.stamina.cover_game_hud,
            Self::StaminaModHud => &mut cfg.stamina.cover_mod_hud,
            Self::Melee => &mut cfg.melee_ring,
            _ => return None,
        })
    }

    pub(super) fn value(self, cfg: &Settings) -> String {
        match self {
            Self::View => match cfg.hud.view {
                View::Both => "Slots and wheel",
                View::Slots => "Slots only",
                View::Wheel => "Wheel only",
            }
            .to_owned(),
            Self::Scale => format!("{:.2}", cfg.hud.scale),
            Self::Chance => format!("{:.2}", cfg.infection.screamer_chance),
            Self::Slots => cfg.limits.slots.to_string(),
            Self::Weapons => cfg.limits.weapon_slots.to_string(),
            Self::Weight => format!("{:.1}", cfg.limits.max_weight),
            _ => String::new(),
        }
    }

    pub(super) fn apply(self, cfg: &mut Settings, step: i32) {
        if let Some(flag) = self.field(cfg) {
            *flag = !*flag;
            return;
        }
        match self {
            Self::View => {
                cfg.hud.view = [View::Both, View::Slots, View::Wheel]
                    [(cfg.hud.view as i32 + step).rem_euclid(3) as usize]
            }
            Self::Scale => cfg.hud.scale = (cfg.hud.scale + step as f32 * 0.05).clamp(0.5, 3.0),
            Self::Chance => {
                cfg.infection.screamer_chance =
                    (cfg.infection.screamer_chance + step as f32 * 0.05).clamp(0.0, 1.0)
            }
            Self::Slots => {
                cfg.limits.slots = (cfg.limits.slots as i32 + step).clamp(1, 24) as usize
            }
            Self::Weapons => {
                cfg.limits.weapon_slots =
                    (cfg.limits.weapon_slots as i32 + step).clamp(0, 8) as usize
            }
            Self::Weight => cfg.limits.max_weight = (cfg.limits.max_weight + step as f32).max(0.0),
            _ => {}
        }
    }

    pub(super) fn set_number(self, cfg: &mut Settings, text: &str) -> bool {
        match self {
            Self::Slots => {
                let Ok(value) = text.parse::<i32>() else {
                    return false;
                };
                cfg.limits.slots = value.clamp(1, 24) as usize;
            }
            Self::Weapons => {
                let Ok(value) = text.parse::<i32>() else {
                    return false;
                };
                cfg.limits.weapon_slots = value.clamp(0, 8) as usize;
            }
            Self::Weight => {
                let Ok(value) = text.parse::<f32>() else {
                    return false;
                };
                if !value.is_finite() {
                    return false;
                }
                cfg.limits.max_weight = value.max(0.0);
            }
            _ => return false,
        }
        true
    }
}
