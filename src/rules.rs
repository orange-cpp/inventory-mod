use std::{collections::BTreeMap, sync::Arc};

use crate::{
    inventory::Limits,
    items::{AmmoKind, Catalog},
    melee::MeleeClass,
    mutators::Mutator,
};

mod parser;

pub use parser::RuleReport;

#[derive(Clone, Debug, Default)]
pub struct Rules {
    pub debug_spawn: bool,
    pub limits: LimitOverrides,
    pub catalog: Arc<Catalog>,
    pub infection: InfectionRules,
    pub stamina: StaminaRules,
    pub melee: MeleeRules,
    pub ammo_piles: AmmoPileRules,
    pub ping: PingRules,
    pub map_guns: MapGunRules,
    pub self_revive: SelfReviveRules,
    pub chainsaw: ChainsawRules,
    pub mutators: MutatorRules,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct LimitOverrides {
    pub slots: Option<usize>,
    pub max_weight: Option<f32>,
    pub weapon_slots: Option<usize>,
}

impl LimitOverrides {
    pub fn apply(self, defaults: Limits) -> Limits {
        Limits {
            slots: self.slots.unwrap_or(defaults.slots),
            max_weight: self.max_weight.unwrap_or(defaults.max_weight),
            weapon_slots: self.weapon_slots.unwrap_or(defaults.weapon_slots),
        }
    }
}

#[derive(Clone, Debug)]
pub struct ChapterRules {
    pub infection: InfectionRules,
    pub stamina: StaminaRules,
    pub melee: MeleeRules,
    pub ammo_piles: AmmoPileRules,
    pub map_guns: MapGunRules,
    heavy_load: Option<HeavyLoadMutatorRules>,
}

impl ChapterRules {
    pub fn max_weight(&self, limit: f32) -> f32 {
        match self.heavy_load.as_ref() {
            Some(rule) if limit > 0.0 => limit * rule.weight as f32 / 100.0,
            Some(rule) => rule.limit as f32,
            None => limit,
        }
    }
}

fn percentage(value: i32, percent: i32) -> i32 {
    (f64::from(value) * f64::from(percent) / 100.0).round() as i32
}

impl Rules {
    pub fn chapter(&self, mutators: &[Mutator]) -> ChapterRules {
        let mut rules = ChapterRules {
            infection: self.infection.clone(),
            stamina: self.stamina.clone(),
            melee: self.melee.clone(),
            ammo_piles: self.ammo_piles.clone(),
            map_guns: self.map_guns.clone(),
            heavy_load: None,
        };
        for &mutator in mutators {
            match mutator {
                Mutator::Scarcity => {
                    let m = &self.mutators.scarcity;
                    let piles = &mut rules.ammo_piles;
                    piles.enabled = true;
                    piles.min_magazines = percentage(piles.min_magazines, m.ammo).max(1);
                    piles.max_magazines =
                        percentage(piles.max_magazines, m.ammo).max(piles.min_magazines);
                    piles.extra = m.extra;

                    let guns = &mut rules.map_guns;
                    guns.enabled = true;
                    guns.min_clip = percentage(guns.min_clip, m.clip);
                    guns.max_clip = percentage(guns.max_clip, m.clip).max(guns.min_clip);
                    guns.min_reserve = guns.min_reserve * m.ammo / 100;
                    guns.max_reserve = (guns.max_reserve * m.ammo / 100).max(guns.min_reserve);
                }
                Mutator::HeavyLoad => {
                    let m = &self.mutators.heavy_load;
                    let scale = m.stamina as f32 / 100.0;
                    let stamina = &mut rules.stamina;
                    stamina.enabled = true;
                    stamina.run *= scale;
                    stamina.jump *= scale;
                    stamina.swing *= scale;
                    stamina.shove *= scale;
                    rules.heavy_load = Some(m.clone());
                }
                Mutator::Contagion => {
                    rules.infection.enabled = true;
                    rules.infection.seconds =
                        percentage(rules.infection.seconds, self.mutators.contagion.clock).max(10);
                }
                Mutator::Brittle if rules.melee.enabled => {
                    let wear = self.mutators.brittle.wear.max(1);
                    rules.melee.durability = rules.melee.durability * 100.0 / wear as f32;
                    for weapon in rules.melee.weapons.values_mut() {
                        if let Some(hits) = &mut weapon.durability {
                            *hits = (f64::from(*hits) * 100.0 / f64::from(wear))
                                .round()
                                .max(1.0) as i32;
                        }
                    }
                }
                _ => {}
            }
        }
        rules
    }

    pub fn revives(&self, classname: &str) -> bool {
        self.self_revive.enabled && self.self_revive.items.iter().any(|item| item == classname)
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct InfectionRules {
    pub enabled: bool,
    pub seconds: i32,
    pub hit_seconds: i32,
    pub specials: Vec<i32>,
    pub bots: bool,
    pub no_cure: bool,
    pub cough: bool,
    pub insane: bool,
    pub insane_at: i32,
}

impl Default for InfectionRules {
    fn default() -> Self {
        Self {
            enabled: false,
            seconds: 120,
            hit_seconds: 20,
            specials: vec![1, 2, 3, 4, 5, 6],
            bots: false,
            no_cure: true,
            cough: true,
            insane: true,
            insane_at: 70,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct StaminaRules {
    pub enabled: bool,
    pub run: f32,
    pub jump: f32,
    pub swing: f32,
    pub shove: f32,
    pub regen: f32,
    pub regen_delay: f32,
    pub recover: f32,
    pub slow: f32,
    pub swing_speed: f32,
    pub damage: f32,
    pub bots: bool,
}

impl Default for StaminaRules {
    fn default() -> Self {
        Self {
            enabled: false,
            run: 8.0,
            jump: 12.0,
            swing: 10.0,
            shove: 8.0,
            regen: 20.0,
            regen_delay: 1.0,
            recover: 30.0,
            slow: 60.0,
            swing_speed: 60.0,
            damage: 50.0,
            bots: false,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Default)]
pub struct MeleeWeaponRules {
    pub class: Option<MeleeClass>,
    pub heavy_hold: Option<f32>,
    pub light_damage: Option<f32>,
    pub heavy_damage: Option<f32>,
    pub cleave: Option<f32>,
    pub stamina: Option<f32>,
    pub durability: Option<i32>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct MeleeRules {
    pub enabled: bool,
    pub heavy_hold: f32,
    pub heavy_damage: f32,
    pub heavy_recovery: f32,
    pub heavy_stamina: f32,
    pub light_damage: f32,
    pub head_damage: f32,
    pub head_only: bool,
    pub cleave: f32,
    pub cleave_min: f32,
    pub wind_speed: f32,
    pub wind_stamina: f32,
    pub hit_stamina: f32,
    pub durability: f32,
    pub block: f32,
    pub block_stamina: f32,
    pub block_speed: f32,
    pub deflect: f32,
    pub heavy_shove_hold: f32,
    pub heavy_shove_stamina: f32,
    pub throw_damage: f32,
    pub throw_speed: f32,
    pub heavy: bool,
    pub wind_slowdown: bool,
    pub blocking: bool,
    pub deflecting: bool,
    pub heavy_shove: bool,
    pub throwing: bool,
    pub breaking: bool,
    pub hit_stagger: bool,
    pub view_kick: bool,
    pub wall_bounce: bool,
    pub view_punch: f32,
    pub overhead: bool,
    pub gesture: bool,
    pub weapons: BTreeMap<String, MeleeWeaponRules>,
}

impl Default for MeleeRules {
    fn default() -> Self {
        Self {
            enabled: false,
            heavy_hold: 0.6,
            heavy_damage: 200.0,
            heavy_recovery: 150.0,
            heavy_stamina: 25.0,
            light_damage: 40.0,
            head_damage: 200.0,
            head_only: true,
            cleave: 70.0,
            cleave_min: 30.0,
            wind_speed: 65.0,
            wind_stamina: 4.0,
            hit_stamina: 3.0,
            durability: 100.0,
            block: 25.0,
            block_stamina: 6.0,
            block_speed: 60.0,
            deflect: 0.25,
            heavy_shove_hold: 0.5,
            heavy_shove_stamina: 20.0,
            throw_damage: 250.0,
            throw_speed: 900.0,
            heavy: true,
            wind_slowdown: true,
            blocking: true,
            deflecting: true,
            heavy_shove: true,
            throwing: true,
            breaking: true,
            hit_stagger: true,
            view_kick: true,
            wall_bounce: true,
            view_punch: 3.0,
            overhead: true,
            gesture: true,
            weapons: Default::default(),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct AmmoPileRules {
    pub enabled: bool,
    pub min_magazines: i32,
    pub max_magazines: i32,
    pub kinds: Vec<AmmoKind>,
    pub replace_map_piles: bool,
    pub extra: i32,
}

impl Default for AmmoPileRules {
    fn default() -> Self {
        Self {
            enabled: false,
            min_magazines: 1,
            max_magazines: 3,
            kinds: vec![
                AmmoKind::AssaultRifle,
                AmmoKind::Smg,
                AmmoKind::Shotgun,
                AmmoKind::HuntingRifle,
                AmmoKind::SniperRifle,
                AmmoKind::GrenadeLauncher,
            ],
            replace_map_piles: true,
            extra: 4,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct PingRules {
    pub enabled: bool,
    pub lifetime: f32,
    pub cooldown: f32,
    pub per_player: i32,
    pub range: f32,
}

impl Default for PingRules {
    fn default() -> Self {
        Self {
            enabled: false,
            lifetime: 8.0,
            cooldown: 0.4,
            per_player: 1,
            range: 6000.0,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct MapGunRules {
    pub enabled: bool,
    pub min_clip: i32,
    pub max_clip: i32,
    pub spawners_once: bool,
    pub min_reserve: i32,
    pub max_reserve: i32,
}

impl Default for MapGunRules {
    fn default() -> Self {
        Self {
            enabled: false,
            min_clip: 10,
            max_clip: 60,
            spawners_once: true,
            min_reserve: 0,
            max_reserve: 1,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct SelfReviveRules {
    pub enabled: bool,
    pub items: Vec<String>,
    pub seconds: f32,
    pub ledge: bool,
    pub rush: bool,
}

impl Default for SelfReviveRules {
    fn default() -> Self {
        Self {
            enabled: false,
            items: vec!["weapon_adrenaline".to_owned()],
            seconds: 3.0,
            ledge: true,
            rush: true,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct ChainsawRules {
    pub enabled: bool,
    pub can_fuel: i32,
    pub seconds: f32,
}

impl Default for ChainsawRules {
    fn default() -> Self {
        Self {
            enabled: false,
            can_fuel: 90,
            seconds: 2.0,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct FogMutatorRules {
    pub density: f32,
    pub color: String,
}

impl Default for FogMutatorRules {
    fn default() -> Self {
        Self {
            density: 0.6,
            color: Default::default(),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct ScarcityMutatorRules {
    pub ammo: i32,
    pub clip: i32,
    pub extra: i32,
}

impl Default for ScarcityMutatorRules {
    fn default() -> Self {
        Self {
            ammo: 50,
            clip: 50,
            extra: 0,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct HeavyLoadMutatorRules {
    pub weight: i32,
    pub limit: i32,
    pub stamina: i32,
}

impl Default for HeavyLoadMutatorRules {
    fn default() -> Self {
        Self {
            weight: 60,
            limit: 15,
            stamina: 150,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct ContagionMutatorRules {
    pub clock: i32,
}

impl Default for ContagionMutatorRules {
    fn default() -> Self {
        Self { clock: 60 }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct BrittleMutatorRules {
    pub wear: i32,
}

impl Default for BrittleMutatorRules {
    fn default() -> Self {
        Self { wear: 300 }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct HordeTideMutatorRules {
    pub first: i32,
    pub every: i32,
}

impl Default for HordeTideMutatorRules {
    fn default() -> Self {
        Self {
            first: 60,
            every: 180,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct HuntedMutatorRules {
    pub class: Option<i32>,
    pub limit: i32,
}

impl Default for HuntedMutatorRules {
    fn default() -> Self {
        Self {
            class: Default::default(),
            limit: 3,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct CrowdMutatorRules {
    pub commons: i32,
    pub mobs: i32,
}

impl Default for CrowdMutatorRules {
    fn default() -> Self {
        Self {
            commons: 200,
            mobs: 150,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct WitchMutatorRules {
    pub health: i32,
    pub anger: i32,
    pub space: i32,
    pub damage: i32,
}

impl Default for WitchMutatorRules {
    fn default() -> Self {
        Self {
            health: 150,
            anger: 300,
            space: 300,
            damage: 45,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct TankMutatorRules {
    pub health: i32,
    pub speed: i32,
}

impl Default for TankMutatorRules {
    fn default() -> Self {
        Self {
            health: 150,
            speed: 110,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct MutatorRules {
    pub enabled: bool,
    pub chance: i32,
    pub count: i32,
    pub pool: Vec<Mutator>,
    pub announce: bool,
    pub maps: BTreeMap<String, Vec<Mutator>>,
    pub fog: FogMutatorRules,
    pub scarcity: ScarcityMutatorRules,
    pub heavy_load: HeavyLoadMutatorRules,
    pub contagion: ContagionMutatorRules,
    pub brittle: BrittleMutatorRules,
    pub horde_tide: HordeTideMutatorRules,
    pub hunted: HuntedMutatorRules,
    pub crowd: CrowdMutatorRules,
    pub witch: WitchMutatorRules,
    pub tank: TankMutatorRules,
}

impl Default for MutatorRules {
    fn default() -> Self {
        Self {
            enabled: false,
            chance: 100,
            count: 1,
            pool: vec![Mutator::Fog],
            announce: true,
            maps: Default::default(),
            fog: Default::default(),
            scarcity: Default::default(),
            heavy_load: Default::default(),
            contagion: Default::default(),
            brittle: Default::default(),
            horde_tide: Default::default(),
            hunted: Default::default(),
            crowd: Default::default(),
            witch: Default::default(),
            tank: Default::default(),
        }
    }
}
