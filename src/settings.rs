use std::{fmt::Write, fs, io, path::Path};

use toml_edit::{ImDocument, Item};

use crate::inventory::Limits;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum View {
    #[default]
    Both,
    Slots,
    Wheel,
}

impl View {
    fn name(self) -> &'static str {
        match self {
            Self::Both => "both",
            Self::Slots => "slots",
            Self::Wheel => "wheel",
        }
    }

    pub fn shows_slots(self) -> bool {
        self != Self::Wheel
    }

    pub fn shows_wheel(self) -> bool {
        self != Self::Slots
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Settings {
    pub keys: Keys,
    pub hud: Hud,
    pub infection: InfectionEffects,
    pub stamina: StaminaEffects,
    pub limits: Limits,
    pub auto_store: bool,
    pub melee_ring: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Keys {
    pub toggle: i32,
    pub previous: i32,
    pub next: i32,
    pub take: i32,
    pub drop: i32,
    pub store: i32,
    pub pickup: i32,
    pub unload: i32,
    pub ping: i32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Hud {
    pub show: bool,
    pub replace_native_panel: bool,
    pub wheel_sounds: bool,
    pub scale: f32,
    pub view: View,
}

#[derive(Clone, Debug, PartialEq)]
pub struct InfectionEffects {
    pub effect: bool,
    pub cover_game_hud: bool,
    pub cover_mod_hud: bool,
    pub sound: bool,
    pub screamer: bool,
    pub screamer_chance: f32,
    pub screamer_image: String,
    pub screamer_sound: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StaminaEffects {
    pub bar: bool,
    pub vignette: bool,
    pub cover_game_hud: bool,
    pub cover_mod_hud: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            keys: Keys {
                toggle: i32::from(b'I'),
                previous: 0xdb,
                next: 0xdd,
                take: 0xdc,
                drop: i32::from(b'G'),
                store: 0xba,
                pickup: i32::from(b'H'),
                unload: i32::from(b'U'),
                ping: i32::from(b'V'),
            },
            hud: Hud {
                show: true,
                replace_native_panel: false,
                wheel_sounds: true,
                scale: 1.0,
                view: View::Both,
            },
            infection: InfectionEffects {
                effect: true,
                cover_game_hud: true,
                cover_mod_hud: true,
                sound: true,
                screamer: true,
                screamer_chance: 1.0,
                screamer_image: "inventory_mod_screamer.jpg".into(),
                screamer_sound: "npc/witch/voice/mad/female_ls_d_madscream01.wav".into(),
            },
            stamina: StaminaEffects {
                bar: true,
                vignette: true,
                cover_game_hud: true,
                cover_mod_hud: true,
            },
            limits: Limits {
                slots: 24,
                max_weight: 0.0,
                weapon_slots: 8,
            },
            auto_store: false,
            melee_ring: true,
        }
    }
}

impl Settings {
    pub const NAME: &str = "inventory_mod_settings.toml";
    const LEGACY_NAME: &str = "inventory_mod.ini";

    pub fn load(directory: &Path) -> Result<Self, SettingsError> {
        let path = directory.join(Self::NAME);
        match fs::read_to_string(&path) {
            Ok(text) => Self::parse(&text).map_err(SettingsError::Toml),
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                let old = directory.join(Self::LEGACY_NAME);
                let text = match fs::read_to_string(&old) {
                    Ok(text) => text,
                    Err(error) if error.kind() == io::ErrorKind::NotFound => {
                        return Ok(Self::default());
                    }
                    Err(error) => return Err(SettingsError::Io(error)),
                };
                let settings = Self::legacy(&text);
                settings.save(directory).map_err(SettingsError::Io)?;
                fs::rename(old, directory.join("inventory_mod.ini.old"))
                    .map_err(SettingsError::Io)?;
                Ok(settings)
            }
            Err(error) => Err(SettingsError::Io(error)),
        }
    }

    pub fn parse(text: &str) -> Result<Self, toml_edit::TomlError> {
        let document = ImDocument::parse(text.strip_prefix('\u{feff}').unwrap_or(text))?;
        let mut settings = Self::default();
        let field = |section: &str, key: &str| document.get(section)?.as_table_like()?.get(key);
        let integer = |section, key, default| {
            field(section, key)
                .and_then(|item| {
                    item.as_integer()
                        .or_else(|| item.as_bool().map(i64::from))
                        .or_else(|| {
                            item.as_float()
                                .filter(|n| n.fract() == 0.0 && n.abs() < i64::MAX as f64)
                                .map(|n| n as i64)
                        })
                })
                .unwrap_or(default)
        };
        let key = |name, default| {
            i32::try_from(integer("keys", name, i64::from(default))).unwrap_or(default)
        };
        let flag = |section, key, default| {
            field(section, key)
                .and_then(|item| item.as_bool().or_else(|| item.as_integer().map(|n| n != 0)))
                .unwrap_or(default)
        };
        let number = |section, key, default: f32| {
            field(section, key)
                .and_then(|item| {
                    item.as_float()
                        .or_else(|| item.as_integer().map(|n| n as f64))
                })
                .map(|n| n as f32)
                .filter(|n| n.is_finite())
                .unwrap_or(default)
        };
        let string = |section, key, out: &mut String| {
            if let Some(value) = field(section, key).and_then(Item::as_str) {
                value.clone_into(out);
            }
        };
        let k = &mut settings.keys;
        k.toggle = key("toggle", k.toggle);
        k.previous = key("previous", k.previous);
        k.next = key("next", k.next);
        k.take = key("use", k.take);
        k.drop = key("drop", k.drop);
        k.store = key("store", k.store);
        k.pickup = key("pickup", k.pickup);
        k.unload = key("unload", k.unload);
        k.ping = key("ping", k.ping);

        let hud = &mut settings.hud;
        hud.show = flag("hud", "show", hud.show);
        hud.replace_native_panel = flag("hud", "replace_native_panel", hud.replace_native_panel);
        hud.wheel_sounds = flag("hud", "wheel_sounds", hud.wheel_sounds);
        hud.scale = number("hud", "scale", hud.scale).clamp(0.5, 3.0);
        hud.view = match field("hud", "view").and_then(Item::as_str) {
            Some("slots") => View::Slots,
            Some("wheel") => View::Wheel,
            _ => View::Both,
        };

        settings.auto_store = flag("play", "auto_store", settings.auto_store);
        let effect = &mut settings.infection;
        effect.effect = flag("infection", "effect", effect.effect);
        effect.cover_game_hud = flag("infection", "cover_game_hud", effect.cover_game_hud);
        effect.cover_mod_hud = flag("infection", "cover_mod_hud", effect.cover_mod_hud);
        effect.sound = flag("infection", "sound", effect.sound);
        effect.screamer = flag("infection", "screamer", effect.screamer);
        effect.screamer_chance =
            number("infection", "screamer_chance", effect.screamer_chance).clamp(0.0, 1.0);
        string("infection", "screamer_image", &mut effect.screamer_image);
        string("infection", "screamer_sound", &mut effect.screamer_sound);

        let stamina = &mut settings.stamina;
        stamina.bar = flag("stamina", "bar", stamina.bar);
        stamina.vignette = flag("stamina", "vignette", stamina.vignette);
        stamina.cover_game_hud = flag("stamina", "cover_game_hud", stamina.cover_game_hud);
        stamina.cover_mod_hud = flag("stamina", "cover_mod_hud", stamina.cover_mod_hud);
        settings.melee_ring = flag("melee", "ring", settings.melee_ring);

        let limit = &mut settings.limits;
        limit.slots = integer("limits", "slots", limit.slots as i64).clamp(1, 24) as usize;
        limit.weapon_slots =
            integer("limits", "weapon_slots", limit.weapon_slots as i64).clamp(0, 8) as usize;
        limit.max_weight = number("limits", "max_weight", limit.max_weight).max(0.0);
        Ok(settings)
    }

    pub fn legacy(text: &str) -> Self {
        let mut settings = Self::default();
        let mut layout = 0;
        for (key, value) in text.lines().filter_map(|line| line.split_once('=')) {
            let integer: i32 = value.trim().parse().unwrap_or(0);
            let float = || {
                value
                    .trim()
                    .parse::<f32>()
                    .ok()
                    .filter(|n| n.is_finite())
                    .unwrap_or(0.0)
            };
            match key {
                "key_toggle" => settings.keys.toggle = integer,
                "key_prev" => settings.keys.previous = integer,
                "key_next" => settings.keys.next = integer,
                "key_use" => settings.keys.take = integer,
                "key_drop" => settings.keys.drop = integer,
                "key_store" => settings.keys.store = integer,
                "key_pickup" => settings.keys.pickup = integer,
                "slots" => settings.limits.slots = integer.clamp(1, 24) as usize,
                "weapon_slots" => settings.limits.weapon_slots = integer.clamp(0, 8) as usize,
                "max_weight" => settings.limits.max_weight = float().max(0.0),
                "replace_native" => settings.hud.replace_native_panel = integer != 0,
                "show_hotbar" => settings.hud.show = integer != 0,
                "auto_store" => settings.auto_store = integer != 0,
                "hud_scale" => settings.hud.scale = float().clamp(0.5, 3.0),
                "inventory_view" => {
                    settings.hud.view = match integer {
                        1 => View::Slots,
                        2 => View::Wheel,
                        _ => View::Both,
                    }
                }
                "layout" => layout = integer,
                _ => {}
            }
        }
        if layout < 2 {
            settings.keys.drop = i32::from(b'G');
            settings.keys.pickup = i32::from(b'H');
        }
        if layout < 3 {
            settings.hud.replace_native_panel = false;
        }
        if layout < 4 {
            settings.limits.slots = 24;
            settings.limits.weapon_slots = 8;
        }
        settings
    }

    pub fn save(&self, directory: &Path) -> io::Result<()> {
        let filename = directory.join(Self::NAME);
        let temp = filename.with_extension("toml.tmp");
        let result = fs::write(&temp, self.to_toml()).and_then(|()| fs::rename(&temp, filename));
        if result.is_err() {
            let _ = fs::remove_file(&temp);
        }
        result
    }

    pub fn to_toml(&self) -> String {
        let k = &self.keys;
        let hud = &self.hud;
        let infection = &self.infection;
        let stamina = &self.stamina;
        let limits = self.limits;
        let mut text = String::new();
        writeln!(text, "[keys]\ntoggle = {}\nprevious = {}\nnext = {}\nuse = {}\ndrop = {}\nstore = {}\npickup = {}\nunload = {}\nping = {}\n",
            k.toggle, k.previous, k.next, k.take, k.drop, k.store, k.pickup, k.unload, k.ping).unwrap();
        writeln!(text, "[hud]\nshow = {}\nreplace_native_panel = {}\nwheel_sounds = {}\nscale = {}\nview = \"{}\"\n",
            hud.show, hud.replace_native_panel, hud.wheel_sounds, hud.scale, hud.view.name()).unwrap();
        writeln!(text, "[play]\nauto_store = {}\n", self.auto_store).unwrap();
        writeln!(text, "[infection]\neffect = {}\ncover_game_hud = {}\ncover_mod_hud = {}\nsound = {}\nscreamer = {}\nscreamer_chance = {}\nscreamer_image = {}\nscreamer_sound = {}\n",
            infection.effect, infection.cover_game_hud, infection.cover_mod_hud, infection.sound,
            infection.screamer, infection.screamer_chance,
            quote(&infection.screamer_image), quote(&infection.screamer_sound)).unwrap();
        writeln!(
            text,
            "[stamina]\nbar = {}\nvignette = {}\ncover_game_hud = {}\ncover_mod_hud = {}\n",
            stamina.bar, stamina.vignette, stamina.cover_game_hud, stamina.cover_mod_hud
        )
        .unwrap();
        writeln!(text, "[melee]\nring = {}\n", self.melee_ring).unwrap();
        writeln!(
            text,
            "[limits]\nslots = {}\nweapon_slots = {}\nmax_weight = {}",
            limits.slots, limits.weapon_slots, limits.max_weight
        )
        .unwrap();
        text
    }
}

#[derive(Debug)]
pub enum SettingsError {
    Io(io::Error),
    Toml(toml_edit::TomlError),
}

impl std::fmt::Display for SettingsError {
    fn fmt(&self, out: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io(error) => error.fmt(out),
            Self::Toml(error) => error.fmt(out),
        }
    }
}

impl std::error::Error for SettingsError {}

fn quote(text: &str) -> String {
    let mut result = String::with_capacity(text.len() + 2);
    result.push('"');
    for c in text.chars() {
        match c {
            '"' => result.push_str("\\\""),
            '\\' => result.push_str("\\\\"),
            '\t' => result.push_str("\\t"),
            '\n' => result.push_str("\\n"),
            '\r' => result.push_str("\\r"),
            c if c.is_ascii_control() => write!(result, "\\u{:04x}", u32::from(c)).unwrap(),
            c => result.push(c),
        }
    }
    result.push('"');
    result
}
