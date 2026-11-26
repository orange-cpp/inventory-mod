use std::{
    borrow::Cow,
    ops::{Range, RangeInclusive},
    sync::Arc,
};

use toml_edit::{ImDocument, Item, TableLike};

use super::*;

const SPECIALS: [&str; 6] = ["smoker", "boomer", "hunter", "spitter", "jockey", "charger"];

#[derive(Clone, Debug, Default)]
pub struct RuleReport {
    pub items_changed: usize,
    pub ammo_changed: usize,
    pub diagnostics: Vec<String>,
}

impl Rules {
    pub fn parse(text: &str) -> Result<(Self, RuleReport), String> {
        let mut rules = Self::default();
        let report = rules.reload(text)?;
        Ok((rules, report))
    }

    /// A syntax error leaves the current rules intact. Each valid entry in a
    /// well-formed file still applies when another entry fails validation.
    /// Item weights, stack sizes and reserve caps remain until overridden.
    pub fn reload(&mut self, text: &str) -> Result<RuleReport, String> {
        let text = text.strip_prefix('\u{feff}').unwrap_or(text);
        let document = ImDocument::parse(text)
            .map_err(|error| diagnostic(text, error.span(), error.message()))?;
        let mut catalog = (*self.catalog).clone();
        catalog.reset_round_weights();
        let mut rules = Self::default();
        let mut report = RuleReport::default();

        for (name, item) in document.iter() {
            let diagnostics = &mut report.diagnostics;
            let Some(table) = item.as_table_like() else {
                diagnostics.push(diagnostic(
                    text,
                    item.span(),
                    format!("{name} must be a table"),
                ));
                continue;
            };
            match name {
                "limits" => read_limits(text, table, &mut rules.limits, diagnostics),
                "weight" => read_fields(text, name, table, diagnostics, |key, entry| {
                    if let Some(weight) = entry.number(0.0..=f64::from(f32::MAX)) {
                        if let Some(id) = catalog.find_item(&classname(key)) {
                            catalog.set_item_weight(id, weight);
                            report.items_changed += 1;
                        } else {
                            entry.error(format!("weight: unknown item {key}"));
                        }
                    }
                }),
                "stack" => read_fields(text, name, table, diagnostics, |key, entry| {
                    if let Some(count) = entry.integer(1..=99, true) {
                        if catalog
                            .find_item(&classname(key))
                            .is_some_and(|id| catalog.set_item_stack(id, count))
                        {
                            report.items_changed += 1;
                        } else {
                            entry.error(format!(
                                "stack: unknown item, or a weapon (always one per box): {key}"
                            ));
                        }
                    }
                }),
                "ammo" => read_fields(text, name, table, diagnostics, |key, entry| {
                    if let Some(rounds) = entry.integer(0..=10_000, true) {
                        if let Some(kind) = ammo_kind(key) {
                            catalog.set_ammo_max(kind, Some(rounds));
                            report.ammo_changed += 1;
                        } else {
                            entry.error(format!("ammo: unknown ammo type {key}"));
                        }
                    }
                }),
                "round_weight" => read_fields(text, name, table, diagnostics, |key, entry| {
                    if let Some(weight) = entry.number(0.0..=10.0) {
                        if let Some(kind) = ammo_kind(key) {
                            catalog.set_round_weight(kind, weight);
                        } else {
                            entry.error(format!("round_weight: unknown ammo type {key}"));
                        }
                    }
                }),
                "infection" => read_infection(text, table, &mut rules.infection, diagnostics),
                "stamina" => read_stamina(text, table, &mut rules.stamina, diagnostics),
                "melee" => read_melee(text, table, &mut rules.melee, diagnostics),
                "ammo_piles" => read_ammo_piles(text, table, &mut rules.ammo_piles, diagnostics),
                "ping" => read_ping(text, table, &mut rules.ping, diagnostics),
                "map_guns" => read_map_guns(text, table, &mut rules.map_guns, diagnostics),
                "self_revive" => read_self_revive(text, table, &mut rules.self_revive, diagnostics),
                "chainsaw" => read_chainsaw(text, table, &mut rules.chainsaw, diagnostics),
                "mutators" => read_mutators(text, table, &mut rules.mutators, diagnostics),
                _ => diagnostics.push(diagnostic(
                    text,
                    item.span(),
                    format!("unknown table {name}"),
                )),
            }
        }

        let fuel = if rules.chainsaw.enabled {
            rules.chainsaw.can_fuel
        } else {
            0
        };
        if let Some(id) = catalog.find_item("weapon_gascan") {
            catalog.set_item_fuel(id, fuel);
        }
        if rules.mutators.enabled
            && !rules.melee.enabled
            && (rules.mutators.pool.contains(&Mutator::Brittle)
                || rules
                    .mutators
                    .maps
                    .values()
                    .any(|ids| ids.contains(&Mutator::Brittle)))
        {
            report
                .diagnostics
                .push("mutators: brittle needs [melee] (the weapons' wear)".to_owned());
        }
        rules.catalog = Arc::new(catalog);
        *self = rules;
        Ok(report)
    }
}

fn classname(key: &str) -> Cow<'_, str> {
    if key.starts_with("weapon_") {
        Cow::Borrowed(key)
    } else {
        Cow::Owned(format!("weapon_{key}"))
    }
}

fn ammo_kind(name: &str) -> Option<AmmoKind> {
    AmmoKind::from_name(&name.replace('_', ""))
}

fn special_class(name: &str) -> Option<i32> {
    SPECIALS
        .iter()
        .position(|&special| special == name)
        .map(|index| index as i32 + 1)
}

fn diagnostic(text: &str, span: Option<Range<usize>>, message: impl std::fmt::Display) -> String {
    let end = span.map_or(0, |span| span.start).min(text.len());
    let line = text.as_bytes()[..end]
        .iter()
        .filter(|&&byte| byte == b'\n')
        .count()
        + 1;
    format!("line {line}: {message}")
}

struct Setting<'a> {
    text: &'a str,
    name: &'a str,
    item: &'a Item,
    diagnostics: &'a mut Vec<String>,
}

impl Setting<'_> {
    fn error(&mut self, message: impl std::fmt::Display) {
        self.diagnostics
            .push(diagnostic(self.text, self.item.span(), message));
    }

    fn unknown(&mut self) {
        self.error(format!("unknown setting {}", self.name));
    }

    fn set_bool(&mut self, field: &mut bool) {
        if let Some(value) = self.item.as_bool() {
            *field = value;
        } else {
            self.error(format!("{} must be true or false", self.name));
        }
    }

    fn integer(&mut self, range: RangeInclusive<i32>, coerce: bool) -> Option<i32> {
        let value = self
            .item
            .as_integer()
            .and_then(|n| i32::try_from(n).ok())
            .or_else(|| {
                if !coerce {
                    return None;
                }
                self.item.as_bool().map(i32::from).or_else(|| {
                    self.item
                        .as_float()
                        .filter(|n| {
                            n.fract() == 0.0
                                && *n >= f64::from(*range.start())
                                && *n <= f64::from(*range.end())
                        })
                        .map(|n| n as i32)
                })
            });
        let value = value.filter(|n| range.contains(n));
        if value.is_none() {
            self.error(format!(
                "{} must be a whole number from {} to {}",
                self.name,
                range.start(),
                range.end()
            ));
        }
        value
    }

    fn set_integer(&mut self, field: &mut i32, range: RangeInclusive<i32>) {
        if let Some(value) = self.integer(range, false) {
            *field = value;
        }
    }

    fn number(&mut self, range: RangeInclusive<f64>) -> Option<f32> {
        let value = self
            .item
            .as_float()
            .or_else(|| self.item.as_integer().map(|n| n as f64))
            .filter(|f| range.contains(f));
        if value.is_none() {
            self.error(format!(
                "{} must be a number from {} to {}",
                self.name,
                range.start(),
                range.end()
            ));
        }
        value.map(|f| f as f32)
    }

    fn set_number(&mut self, field: &mut f32, range: RangeInclusive<f64>) {
        if let Some(value) = self.number(range) {
            *field = value;
        }
    }

    fn list<T>(
        &mut self,
        example: &str,
        strict: bool,
        parse: impl Fn(&str) -> Option<T>,
    ) -> Option<Vec<T>> {
        let Some(list) = self.item.as_array() else {
            self.error(format!("{} must be a list, like {example}", self.name));
            return None;
        };
        let mut parsed = Vec::new();
        for entry in list {
            if let Some(value) = entry.as_str().and_then(&parse) {
                parsed.push(value);
            } else {
                self.diagnostics.push(diagnostic(
                    self.text,
                    entry.span(),
                    format!(
                        "{}: invalid entry {:?}",
                        self.name,
                        entry.as_str().unwrap_or("")
                    ),
                ));
                if strict {
                    return None;
                }
            }
        }
        Some(parsed)
    }
}

fn read_fields(
    text: &str,
    prefix: &str,
    table: &dyn TableLike,
    diagnostics: &mut Vec<String>,
    mut read: impl FnMut(&str, &mut Setting<'_>),
) {
    for (key, item) in table.iter() {
        let name = format!("{prefix}.{key}");
        read(
            key,
            &mut Setting {
                text,
                item,
                name: &name,
                diagnostics,
            },
        );
    }
}

fn read_limits(
    text: &str,
    table: &dyn TableLike,
    limits: &mut LimitOverrides,
    diagnostics: &mut Vec<String>,
) {
    read_fields(text, "limits", table, diagnostics, |key, entry| match key {
        "slots" => limits.slots = entry.integer(1..=24, true).map(|n| n as usize),
        "weapon_slots" => limits.weapon_slots = entry.integer(0..=8, true).map(|n| n as usize),
        "max_weight" => limits.max_weight = entry.number(0.0..=f64::from(f32::MAX)),
        _ => entry.unknown(),
    });
}

fn read_infection(
    text: &str,
    table: &dyn TableLike,
    infection: &mut InfectionRules,
    diagnostics: &mut Vec<String>,
) {
    infection.enabled = true;
    read_fields(
        text,
        "infection",
        table,
        diagnostics,
        |key, entry| match key {
            "enabled" => {
                if let Some(value) = entry.item.as_integer() {
                    infection.enabled = value != 0;
                } else {
                    entry.set_bool(&mut infection.enabled);
                }
            }
            "seconds" => {
                if let Some(seconds) = entry.integer(0..=3600, true) {
                    infection.seconds = seconds.max(1);
                }
            }
            "hit_seconds" => {
                if let Some(seconds) = entry.integer(0..=3600, true) {
                    infection.hit_seconds = seconds;
                }
            }
            "insane_at" => entry.set_integer(&mut infection.insane_at, 0..=100),
            "bots" => entry.set_bool(&mut infection.bots),
            "no_cure" => entry.set_bool(&mut infection.no_cure),
            "cough" => entry.set_bool(&mut infection.cough),
            "insane" => entry.set_bool(&mut infection.insane),
            "specials" => {
                if let Some(list) = entry
                    .list("[\"hunter\", \"jockey\"]", false, special_class)
                    .filter(|list| !list.is_empty())
                {
                    infection.specials = list;
                }
            }
            _ => entry.unknown(),
        },
    );
}

fn read_stamina(
    text: &str,
    table: &dyn TableLike,
    stamina: &mut StaminaRules,
    diagnostics: &mut Vec<String>,
) {
    stamina.enabled = true;
    read_fields(
        text,
        "stamina",
        table,
        diagnostics,
        |key, entry| match key {
            "enabled" => entry.set_bool(&mut stamina.enabled),
            "bots" => entry.set_bool(&mut stamina.bots),
            "run" => entry.set_number(&mut stamina.run, 0.0..=100.0),
            "jump" => entry.set_number(&mut stamina.jump, 0.0..=100.0),
            "swing" => entry.set_number(&mut stamina.swing, 0.0..=100.0),
            "shove" => entry.set_number(&mut stamina.shove, 0.0..=100.0),
            "regen" => entry.set_number(&mut stamina.regen, 0.0..=1000.0),
            "regen_delay" => entry.set_number(&mut stamina.regen_delay, 0.0..=60.0),
            "recover" => entry.set_number(&mut stamina.recover, 0.0..=100.0),
            "slow" => entry.set_number(&mut stamina.slow, 10.0..=100.0),
            "swing_speed" => entry.set_number(&mut stamina.swing_speed, 10.0..=100.0),
            "damage" => entry.set_number(&mut stamina.damage, 0.0..=100.0),
            _ => entry.unknown(),
        },
    );
}

fn read_melee(
    text: &str,
    table: &dyn TableLike,
    melee: &mut MeleeRules,
    diagnostics: &mut Vec<String>,
) {
    melee.enabled = true;
    read_fields(text, "melee", table, diagnostics, |key, entry| match key {
        "enabled" => entry.set_bool(&mut melee.enabled),
        "head_only" => entry.set_bool(&mut melee.head_only),
        "wall_bounce" => entry.set_bool(&mut melee.wall_bounce),
        "overhead" => entry.set_bool(&mut melee.overhead),
        "gesture" => entry.set_bool(&mut melee.gesture),
        "heavy" => entry.set_bool(&mut melee.heavy),
        "wind_slowdown" => entry.set_bool(&mut melee.wind_slowdown),
        "blocking" => entry.set_bool(&mut melee.blocking),
        "deflecting" => entry.set_bool(&mut melee.deflecting),
        "heavy_shove" => entry.set_bool(&mut melee.heavy_shove),
        "throwing" => entry.set_bool(&mut melee.throwing),
        "breaking" => entry.set_bool(&mut melee.breaking),
        "hit_stagger" => entry.set_bool(&mut melee.hit_stagger),
        "view_kick" => entry.set_bool(&mut melee.view_kick),
        "heavy_hold" => entry.set_number(&mut melee.heavy_hold, 0.1..=3.0),
        "heavy_damage" => entry.set_number(&mut melee.heavy_damage, 0.0..=1000.0),
        "heavy_recovery" => entry.set_number(&mut melee.heavy_recovery, 50.0..=400.0),
        "heavy_stamina" => entry.set_number(&mut melee.heavy_stamina, 0.0..=100.0),
        "light_damage" => entry.set_number(&mut melee.light_damage, 0.0..=1000.0),
        "head_damage" => entry.set_number(&mut melee.head_damage, 0.0..=1000.0),
        "cleave" => entry.set_number(&mut melee.cleave, 0.0..=100.0),
        "cleave_min" => entry.set_number(&mut melee.cleave_min, 0.0..=100.0),
        "wind_speed" => entry.set_number(&mut melee.wind_speed, 10.0..=100.0),
        "wind_stamina" => entry.set_number(&mut melee.wind_stamina, 0.0..=100.0),
        "hit_stamina" => entry.set_number(&mut melee.hit_stamina, 0.0..=100.0),
        "durability" => entry.set_number(&mut melee.durability, 0.0..=1000.0),
        "block" => entry.set_number(&mut melee.block, 0.0..=100.0),
        "block_stamina" => entry.set_number(&mut melee.block_stamina, 0.0..=100.0),
        "block_speed" => entry.set_number(&mut melee.block_speed, 10.0..=100.0),
        "deflect" => entry.set_number(&mut melee.deflect, 0.0..=2.0),
        "heavy_shove_hold" => entry.set_number(&mut melee.heavy_shove_hold, 0.0..=3.0),
        "heavy_shove_stamina" => entry.set_number(&mut melee.heavy_shove_stamina, 0.0..=100.0),
        "throw_damage" => entry.set_number(&mut melee.throw_damage, 0.0..=1000.0),
        "throw_speed" => entry.set_number(&mut melee.throw_speed, 100.0..=3000.0),
        "view_punch" => entry.set_number(&mut melee.view_punch, 0.0..=30.0),
        "weapons" => {
            if let Some(weapons) = entry.item.as_table_like() {
                read_fields(
                    text,
                    "melee.weapons",
                    weapons,
                    entry.diagnostics,
                    |name, entry| {
                        if let Some(table) = entry.item.as_table_like() {
                            read_melee_weapon(
                                text,
                                entry.name,
                                table,
                                melee.weapons.entry(name.to_owned()).or_default(),
                                entry.diagnostics,
                            );
                        } else {
                            entry.error(format!(
                                "{} must be a table, like {{ class = \"blade_2h\" }}",
                                entry.name
                            ));
                        }
                    },
                );
            } else {
                entry.error("melee.weapons must be a table of weapons");
            }
        }
        _ => entry.unknown(),
    });
}

fn read_melee_weapon(
    text: &str,
    prefix: &str,
    table: &dyn TableLike,
    weapon: &mut MeleeWeaponRules,
    diagnostics: &mut Vec<String>,
) {
    read_fields(text, prefix, table, diagnostics, |key, entry| match key {
        "class" => {
            weapon.class = entry.item.as_str().and_then(MeleeClass::from_name);
            if weapon.class.is_none() {
                entry.error(format!(
                    "{} must be blade_1h, blunt_1h, blade_2h or blunt_2h",
                    entry.name
                ));
            }
        }
        "durability" => weapon.durability = entry.integer(0..=100_000, false),
        "heavy_hold" => weapon.heavy_hold = entry.number(0.1..=3.0),
        "light_damage" => weapon.light_damage = entry.number(0.0..=1000.0),
        "heavy_damage" => weapon.heavy_damage = entry.number(0.0..=1000.0),
        "cleave" => weapon.cleave = entry.number(0.0..=100.0),
        "stamina" => weapon.stamina = entry.number(0.0..=1000.0),
        _ => entry.unknown(),
    });
}

fn ordered_pair(prefix: &str, key: &str, low: i32, high: &mut i32, diagnostics: &mut Vec<String>) {
    if *high < low {
        diagnostics.push(format!(
            "{prefix}: max_{key} is less than min_{key} (taken as the same)"
        ));
        *high = low;
    }
}

fn read_ammo_piles(
    text: &str,
    table: &dyn TableLike,
    piles: &mut AmmoPileRules,
    diagnostics: &mut Vec<String>,
) {
    piles.enabled = true;
    read_fields(
        text,
        "ammo_piles",
        table,
        diagnostics,
        |key, entry| match key {
            "enabled" => entry.set_bool(&mut piles.enabled),
            "replace_map_piles" => entry.set_bool(&mut piles.replace_map_piles),
            "extra" => entry.set_integer(&mut piles.extra, 0..=50),
            "min_magazines" => entry.set_integer(&mut piles.min_magazines, 1..=50),
            "max_magazines" => entry.set_integer(&mut piles.max_magazines, 1..=50),
            "kinds" => {
                if let Some(list) = entry
                    .list("[\"smg\", \"shotgun\"]", false, ammo_kind)
                    .filter(|list| !list.is_empty())
                {
                    piles.kinds = list;
                }
            }
            _ => entry.unknown(),
        },
    );
    ordered_pair(
        "ammo_piles",
        "magazines",
        piles.min_magazines,
        &mut piles.max_magazines,
        diagnostics,
    );
}

fn read_ping(
    text: &str,
    table: &dyn TableLike,
    ping: &mut PingRules,
    diagnostics: &mut Vec<String>,
) {
    ping.enabled = true;
    read_fields(text, "ping", table, diagnostics, |key, entry| match key {
        "enabled" => entry.set_bool(&mut ping.enabled),
        "per_player" => entry.set_integer(&mut ping.per_player, 1..=8),
        "lifetime" => entry.set_number(&mut ping.lifetime, 1.0..=60.0),
        "cooldown" => entry.set_number(&mut ping.cooldown, 0.0..=10.0),
        "range" => entry.set_number(&mut ping.range, 100.0..=20_000.0),
        _ => entry.unknown(),
    });
}

fn read_map_guns(
    text: &str,
    table: &dyn TableLike,
    guns: &mut MapGunRules,
    diagnostics: &mut Vec<String>,
) {
    guns.enabled = true;
    read_fields(
        text,
        "map_guns",
        table,
        diagnostics,
        |key, entry| match key {
            "enabled" => entry.set_bool(&mut guns.enabled),
            "spawners_once" => entry.set_bool(&mut guns.spawners_once),
            "min_clip" => entry.set_integer(&mut guns.min_clip, 0..=100),
            "max_clip" => entry.set_integer(&mut guns.max_clip, 0..=100),
            "min_reserve" => entry.set_integer(&mut guns.min_reserve, 0..=20),
            "max_reserve" => entry.set_integer(&mut guns.max_reserve, 0..=20),
            _ => entry.unknown(),
        },
    );
    ordered_pair(
        "map_guns",
        "reserve",
        guns.min_reserve,
        &mut guns.max_reserve,
        diagnostics,
    );
    ordered_pair(
        "map_guns",
        "clip",
        guns.min_clip,
        &mut guns.max_clip,
        diagnostics,
    );
}

fn deduplicate<T: PartialEq>(list: Vec<T>) -> Vec<T> {
    let mut unique = Vec::with_capacity(list.len());
    for value in list {
        if !unique.contains(&value) {
            unique.push(value);
        }
    }
    unique
}

fn read_self_revive(
    text: &str,
    table: &dyn TableLike,
    revive: &mut SelfReviveRules,
    diagnostics: &mut Vec<String>,
) {
    revive.enabled = true;
    read_fields(
        text,
        "self_revive",
        table,
        diagnostics,
        |key, entry| match key {
            "enabled" => entry.set_bool(&mut revive.enabled),
            "ledge" => entry.set_bool(&mut revive.ledge),
            "rush" => entry.set_bool(&mut revive.rush),
            "seconds" => entry.set_number(&mut revive.seconds, 0.0..=30.0),
            "items" => {
                let items = entry
                    .list("[\"adrenaline\"]", false, |name| {
                        let item = classname(name);
                        matches!(
                            item.as_ref(),
                            "weapon_adrenaline" | "weapon_pain_pills" | "weapon_first_aid_kit"
                        )
                        .then(|| item.into_owned())
                    })
                    .unwrap_or_default();
                revive.items = deduplicate(items);
            }
            _ => entry.unknown(),
        },
    );
}

fn read_chainsaw(
    text: &str,
    table: &dyn TableLike,
    saw: &mut ChainsawRules,
    diagnostics: &mut Vec<String>,
) {
    saw.enabled = true;
    read_fields(
        text,
        "chainsaw",
        table,
        diagnostics,
        |key, entry| match key {
            "enabled" => entry.set_bool(&mut saw.enabled),
            "can_fuel" => entry.set_integer(&mut saw.can_fuel, 1..=1000),
            "seconds" => entry.set_number(&mut saw.seconds, 0.0..=30.0),
            _ => entry.unknown(),
        },
    );
}

fn read_mutators(
    text: &str,
    table: &dyn TableLike,
    m: &mut MutatorRules,
    diagnostics: &mut Vec<String>,
) {
    m.enabled = true;
    read_fields(
        text,
        "mutators",
        table,
        diagnostics,
        |key, entry| match key {
            "enabled" => entry.set_bool(&mut m.enabled),
            "announce" => entry.set_bool(&mut m.announce),
            "chance" => entry.set_integer(&mut m.chance, 0..=100),
            "count" => entry.set_integer(&mut m.count, 0..=8),
            "pool" => {
                if let Some(ids) = entry.list("[\"fog\"]", true, Mutator::from_id) {
                    m.pool = deduplicate(ids);
                }
            }
            "maps" => {
                if let Some(maps) = entry.item.as_table_like() {
                    read_fields(
                        text,
                        "mutators.maps",
                        maps,
                        entry.diagnostics,
                        |map, entry| {
                            if let Some(ids) = entry.list("[\"fog\"]", true, Mutator::from_id) {
                                m.maps.insert(map.to_owned(), deduplicate(ids));
                            }
                        },
                    );
                } else {
                    entry.error("mutators.maps must be a table");
                }
            }
            key => {
                if let Some(mutator) = Mutator::from_id(key)
                    && let Some(settings) = entry.item.as_table_like()
                {
                    read_fields(
                        text,
                        entry.name,
                        settings,
                        entry.diagnostics,
                        |key, entry| {
                            read_mutator_setting(m, mutator, key, entry);
                        },
                    );
                } else {
                    entry.unknown();
                }
            }
        },
    );
}

fn read_mutator_setting(
    m: &mut MutatorRules,
    mutator: Mutator,
    key: &str,
    entry: &mut Setting<'_>,
) {
    use Mutator::*;
    match (mutator, key) {
        (Fog, "density") => entry.set_number(&mut m.fog.density, 0.0..=1.0),
        (Fog, "color") => {
            if let Some(color) = entry.item.as_str().filter(|s| {
                s.is_empty() || {
                    let rgb: Vec<_> = s.split_ascii_whitespace().collect();
                    rgb.len() == 3 && rgb.into_iter().all(|v| v.parse::<u8>().is_ok())
                }
            }) {
                color.clone_into(&mut m.fog.color);
            } else {
                entry.error("mutators.fog.color must be \"r g b\" (0 to 255 each), or \"\" for the map's own");
            }
        }
        (Scarcity, "ammo") => entry.set_integer(&mut m.scarcity.ammo, 1..=100),
        (Scarcity, "clip") => entry.set_integer(&mut m.scarcity.clip, 0..=100),
        (Scarcity, "extra") => entry.set_integer(&mut m.scarcity.extra, 0..=50),
        (HeavyLoad, "weight") => entry.set_integer(&mut m.heavy_load.weight, 10..=100),
        (HeavyLoad, "limit") => entry.set_integer(&mut m.heavy_load.limit, 1..=1000),
        (HeavyLoad, "stamina") => entry.set_integer(&mut m.heavy_load.stamina, 100..=500),
        (Contagion, "clock") => entry.set_integer(&mut m.contagion.clock, 10..=100),
        (Brittle, "wear") => entry.set_integer(&mut m.brittle.wear, 100..=1000),
        (HordeTide, "first") => entry.set_integer(&mut m.horde_tide.first, 0..=3600),
        (HordeTide, "every") => entry.set_integer(&mut m.horde_tide.every, 30..=3600),
        (Hunted, "class") => match entry.item.as_str() {
            Some("random") => m.hunted.class = None,
            Some(class) if special_class(class).is_some() => m.hunted.class = special_class(class),
            _ => entry.error("mutators.hunted.class must be random, smoker, boomer, hunter, spitter, jockey or charger"),
        },
        (Hunted, "limit") => entry.set_integer(&mut m.hunted.limit, 0..=10),
        (Crowd, "commons") => entry.set_integer(&mut m.crowd.commons, 10..=1000),
        (Crowd, "mobs") => entry.set_integer(&mut m.crowd.mobs, 10..=1000),
        (Witch, "health") => entry.set_integer(&mut m.witch.health, 10..=1000),
        (Witch, "anger") => entry.set_integer(&mut m.witch.anger, 10..=1000),
        (Witch, "space") => entry.set_integer(&mut m.witch.space, 10..=1000),
        (Witch, "damage") => entry.set_integer(&mut m.witch.damage, 1..=1000),
        (Tank, "health") => entry.set_integer(&mut m.tank.health, 10..=1000),
        (Tank, "speed") => entry.set_integer(&mut m.tank.speed, 10..=300),
        _ => entry.unknown(),
    }
}
