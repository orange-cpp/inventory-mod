use std::sync::Arc;

/// An item ID stays stable across catalogs that use different server rules.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ItemId(usize);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GameSlot {
    Primary,
    Secondary,
    Throwable,
    Medical,
    Pills,
    Carry,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ItemDef {
    pub classname: &'static str,
    pub name: &'static str,
    pub tag: &'static str,
    pub description: &'static str,
    pub max_stack: i32,
    pub weight: f32,
    pub color: u32,
    pub game_slot: GameSlot,
    pub fuel: i32,
    pub ammo: Option<AmmoKind>,
}

impl ItemDef {
    pub fn is_weapon(&self) -> bool {
        matches!(self.game_slot, GameSlot::Primary | GameSlot::Secondary)
    }

    /// The engine's reserve after Inventory Mod retargets the sidearms.
    /// Unloading uses the weapon's own ammo definition instead.
    pub fn reserve_kind(&self) -> Option<AmmoKind> {
        match self.classname {
            "weapon_pistol" => Some(AmmoKind::Smg),
            "weapon_pistol_magnum" => Some(AmmoKind::SniperRifle),
            _ => self.ammo,
        }
    }
}

const fn rgb(r: u32, g: u32, b: u32) -> u32 {
    r | (g << 8) | (b << 16) | 0xff00_0000
}

const GUN_COLOR: u32 = rgb(150, 160, 175);
const SIDEARM_COLOR: u32 = rgb(170, 150, 120);

const ITEMS: &[ItemDef] = &[
    ItemDef {
        classname: "weapon_first_aid_kit",
        name: "First Aid Kit",
        tag: "FA",
        description: "Heals most of your missing health.",
        max_stack: 2,
        weight: 3.0,
        color: rgb(210, 60, 60),
        game_slot: GameSlot::Medical,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_defibrillator",
        name: "Defibrillator",
        tag: "DF",
        description: "Revives a dead survivor.",
        max_stack: 1,
        weight: 3.0,
        color: rgb(230, 200, 60),
        game_slot: GameSlot::Medical,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_pain_pills",
        name: "Pain Pills",
        tag: "PP",
        description: "Temporary health.",
        max_stack: 4,
        weight: 1.0,
        color: rgb(235, 235, 235),
        game_slot: GameSlot::Pills,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_adrenaline",
        name: "Adrenaline",
        tag: "AD",
        description: "Run faster and shrug off damage for a while.",
        max_stack: 4,
        weight: 1.0,
        color: rgb(120, 190, 240),
        game_slot: GameSlot::Pills,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_pipe_bomb",
        name: "Pipe Bomb",
        tag: "PB",
        description: "Attracts the horde, then explodes.",
        max_stack: 3,
        weight: 2.0,
        color: rgb(150, 150, 150),
        game_slot: GameSlot::Throwable,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_molotov",
        name: "Molotov",
        tag: "MO",
        description: "Sets the ground on fire.",
        max_stack: 3,
        weight: 2.0,
        color: rgb(240, 130, 40),
        game_slot: GameSlot::Throwable,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_vomitjar",
        name: "Bile Jar",
        tag: "BJ",
        description: "Draws infected to the target.",
        max_stack: 3,
        weight: 2.0,
        color: rgb(150, 190, 70),
        game_slot: GameSlot::Throwable,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_upgradepack_incendiary",
        name: "Incendiary Ammo",
        tag: "IA",
        description: "Incendiary ammo upgrade pack.",
        max_stack: 1,
        weight: 3.0,
        color: rgb(240, 90, 40),
        game_slot: GameSlot::Medical,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_upgradepack_explosive",
        name: "Explosive Ammo",
        tag: "EA",
        description: "Explosive ammo upgrade pack.",
        max_stack: 1,
        weight: 3.0,
        color: rgb(250, 170, 40),
        game_slot: GameSlot::Medical,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_gascan",
        name: "Gas Can",
        tag: "GC",
        description: "Fuel for a generator, or the chainsaw.",
        max_stack: 1,
        weight: 6.0,
        color: rgb(200, 60, 40),
        game_slot: GameSlot::Carry,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_propanetank",
        name: "Propane Tank",
        tag: "PT",
        description: "Explodes when shot.",
        max_stack: 1,
        weight: 5.0,
        color: rgb(220, 220, 230),
        game_slot: GameSlot::Carry,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_oxygentank",
        name: "Oxygen Tank",
        tag: "OT",
        description: "Explodes when shot.",
        max_stack: 1,
        weight: 5.0,
        color: rgb(110, 170, 220),
        game_slot: GameSlot::Carry,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_fireworkcrate",
        name: "Fireworks",
        tag: "FW",
        description: "Loud and attractive.",
        max_stack: 1,
        weight: 4.0,
        color: rgb(200, 80, 160),
        game_slot: GameSlot::Carry,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_gnome",
        name: "Gnome",
        tag: "GN",
        description: "Gnome Chompski.",
        max_stack: 1,
        weight: 2.0,
        color: rgb(90, 160, 90),
        game_slot: GameSlot::Carry,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_cola_bottles",
        name: "Cola Bottles",
        tag: "CB",
        description: "Delivery item.",
        max_stack: 1,
        weight: 4.0,
        color: rgb(180, 40, 40),
        game_slot: GameSlot::Carry,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_pistol",
        name: "Pistol",
        tag: "P",
        description: "Sidearm.",
        max_stack: 1,
        weight: 1.0,
        color: SIDEARM_COLOR,
        game_slot: GameSlot::Secondary,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_pistol_magnum",
        name: "Magnum",
        tag: "MG",
        description: "Heavy sidearm.",
        max_stack: 1,
        weight: 1.5,
        color: SIDEARM_COLOR,
        game_slot: GameSlot::Secondary,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_melee",
        name: "Melee Weapon",
        tag: "ML",
        description: "Close combat.",
        max_stack: 1,
        weight: 2.0,
        color: SIDEARM_COLOR,
        game_slot: GameSlot::Secondary,
        fuel: 0,
        ammo: None,
    },
    ItemDef {
        classname: "weapon_chainsaw",
        name: "Chainsaw",
        tag: "CS",
        description: "Loud and fuel hungry.",
        max_stack: 1,
        weight: 6.0,
        color: SIDEARM_COLOR,
        game_slot: GameSlot::Secondary,
        fuel: 0,
        ammo: Some(AmmoKind::Chainsaw),
    },
    ItemDef {
        classname: "weapon_smg",
        name: "SMG",
        tag: "SM",
        description: "Submachine gun.",
        max_stack: 1,
        weight: 3.0,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::Smg),
    },
    ItemDef {
        classname: "weapon_smg_silenced",
        name: "Silenced SMG",
        tag: "SS",
        description: "Quiet submachine gun.",
        max_stack: 1,
        weight: 3.0,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::Smg),
    },
    ItemDef {
        classname: "weapon_smg_mp5",
        name: "MP5",
        tag: "M5",
        description: "Submachine gun.",
        max_stack: 1,
        weight: 3.0,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::Smg),
    },
    ItemDef {
        classname: "weapon_pumpshotgun",
        name: "Pump Shotgun",
        tag: "PS",
        description: "Pump-action shotgun.",
        max_stack: 1,
        weight: 4.0,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::Shotgun),
    },
    ItemDef {
        classname: "weapon_shotgun_chrome",
        name: "Chrome Shotgun",
        tag: "CH",
        description: "Pump-action shotgun.",
        max_stack: 1,
        weight: 4.0,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::Shotgun),
    },
    ItemDef {
        classname: "weapon_autoshotgun",
        name: "Auto Shotgun",
        tag: "AS",
        description: "Automatic shotgun.",
        max_stack: 1,
        weight: 5.0,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::Shotgun),
    },
    ItemDef {
        classname: "weapon_shotgun_spas",
        name: "Combat Shotgun",
        tag: "SP",
        description: "Automatic shotgun.",
        max_stack: 1,
        weight: 5.0,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::Shotgun),
    },
    ItemDef {
        classname: "weapon_rifle",
        name: "M16",
        tag: "M1",
        description: "Assault rifle.",
        max_stack: 1,
        weight: 4.0,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::AssaultRifle),
    },
    ItemDef {
        classname: "weapon_rifle_ak47",
        name: "AK-47",
        tag: "AK",
        description: "Assault rifle.",
        max_stack: 1,
        weight: 4.5,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::AssaultRifle),
    },
    ItemDef {
        classname: "weapon_rifle_desert",
        name: "Combat Rifle",
        tag: "SC",
        description: "Burst rifle.",
        max_stack: 1,
        weight: 4.5,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::AssaultRifle),
    },
    ItemDef {
        classname: "weapon_rifle_sg552",
        name: "SG552",
        tag: "SG",
        description: "Assault rifle with a scope.",
        max_stack: 1,
        weight: 4.5,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::AssaultRifle),
    },
    ItemDef {
        classname: "weapon_hunting_rifle",
        name: "Hunting Rifle",
        tag: "HR",
        description: "Sniper rifle.",
        max_stack: 1,
        weight: 5.0,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::HuntingRifle),
    },
    ItemDef {
        classname: "weapon_sniper_military",
        name: "Military Sniper",
        tag: "MS",
        description: "Sniper rifle.",
        max_stack: 1,
        weight: 5.5,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::SniperRifle),
    },
    ItemDef {
        classname: "weapon_sniper_awp",
        name: "AWP",
        tag: "AW",
        description: "Sniper rifle.",
        max_stack: 1,
        weight: 5.5,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::SniperRifle),
    },
    ItemDef {
        classname: "weapon_sniper_scout",
        name: "Scout",
        tag: "SO",
        description: "Light sniper rifle.",
        max_stack: 1,
        weight: 4.0,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::SniperRifle),
    },
    ItemDef {
        classname: "weapon_rifle_m60",
        name: "M60",
        tag: "60",
        description: "Machine gun.",
        max_stack: 1,
        weight: 8.0,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::M60),
    },
    ItemDef {
        classname: "weapon_grenade_launcher",
        name: "Grenade Launcher",
        tag: "GL",
        description: "Explosive rounds.",
        max_stack: 1,
        weight: 6.0,
        color: GUN_COLOR,
        game_slot: GameSlot::Primary,
        fuel: 0,
        ammo: Some(AmmoKind::GrenadeLauncher),
    },
];

/// Reserve types supported by the mod, in their inventory display order.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum AmmoKind {
    AssaultRifle,
    Smg,
    Shotgun,
    HuntingRifle,
    SniperRifle,
    GrenadeLauncher,
    M60,
    Chainsaw,
}

impl AmmoKind {
    pub const ALL: [Self; 8] = [
        Self::AssaultRifle,
        Self::Smg,
        Self::Shotgun,
        Self::HuntingRifle,
        Self::SniperRifle,
        Self::GrenadeLauncher,
        Self::M60,
        Self::Chainsaw,
    ];

    pub fn from_name(name: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|kind| kind.name() == name)
    }

    pub fn from_index(index: usize) -> Option<Self> {
        Self::ALL
            .into_iter()
            .find(|kind| kind.definition().index == index)
    }

    pub fn name(self) -> &'static str {
        self.definition().name
    }

    pub fn definition(self) -> &'static AmmoDef {
        &AMMO[self as usize]
    }

    pub fn is_unloadable(self) -> bool {
        !matches!(self, Self::M60 | Self::Chainsaw)
    }
}

#[derive(Debug)]
pub struct AmmoDef {
    pub name: &'static str,
    pub index: usize,
    pub default_max: i32,
    pub title: &'static str,
    pub default_weight: f32,
    pub magazine: i32,
    pub color: u32,
}

const AMMO: [AmmoDef; AmmoKind::ALL.len()] = [
    AmmoDef {
        name: "assaultrifle",
        index: 3,
        default_max: 360,
        title: "Rifle rounds",
        default_weight: 0.01,
        magazine: 50,
        color: 0xF0C828,
    },
    AmmoDef {
        name: "smg",
        index: 5,
        default_max: 650,
        title: "SMG rounds",
        default_weight: 0.006,
        magazine: 50,
        color: 0x3C8CFF,
    },
    AmmoDef {
        name: "shotgun",
        index: 7,
        default_max: 72,
        title: "Shotgun shells",
        default_weight: 0.04,
        magazine: 8,
        color: 0xE63228,
    },
    AmmoDef {
        name: "huntingrifle",
        index: 9,
        default_max: 150,
        title: "Hunting rifle rounds",
        default_weight: 0.02,
        magazine: 15,
        color: 0x46D246,
    },
    AmmoDef {
        name: "sniperrifle",
        index: 10,
        default_max: 180,
        title: "Sniper rounds",
        default_weight: 0.02,
        magazine: 20,
        color: 0x28D2C8,
    },
    AmmoDef {
        name: "grenadelauncher",
        index: 17,
        default_max: 30,
        title: "Grenades",
        default_weight: 0.15,
        magazine: 1,
        color: 0xFF8C1E,
    },
    AmmoDef {
        name: "m60",
        index: 6,
        default_max: 0,
        title: "M60 rounds",
        default_weight: 0.01,
        magazine: 150,
        color: 0xE6E6E6,
    },
    AmmoDef {
        name: "chainsaw",
        index: 19,
        default_max: 20,
        title: "Chainsaw fuel",
        default_weight: 0.05,
        magazine: 10,
        color: 0x8C6440,
    },
];

#[derive(Clone, Debug)]
pub struct Catalog {
    items: Vec<ItemDef>,
    ammo_max: [Option<i32>; AmmoKind::ALL.len()],
    round_weight: [Option<f32>; AmmoKind::ALL.len()],
}

impl Default for Catalog {
    fn default() -> Self {
        Self {
            items: ITEMS.to_vec(),
            ammo_max: Default::default(),
            round_weight: Default::default(),
        }
    }
}

impl Catalog {
    pub fn shared_default() -> Arc<Self> {
        use std::sync::OnceLock;

        static DEFAULT: OnceLock<Arc<Catalog>> = OnceLock::new();
        Arc::clone(DEFAULT.get_or_init(|| Arc::new(Self::default())))
    }

    pub fn items(&self) -> impl Iterator<Item = (ItemId, &ItemDef)> {
        self.items
            .iter()
            .enumerate()
            .map(|(index, def)| (ItemId(index), def))
    }

    pub fn find_item(&self, classname: &str) -> Option<ItemId> {
        self.items()
            .find_map(|(id, def)| (def.classname == classname).then_some(id))
    }

    pub fn item(&self, id: ItemId) -> &ItemDef {
        &self.items[id.0]
    }

    pub fn weapon_weight(&self, classname: &str) -> f32 {
        self.find_item(classname)
            .map(|id| self.item(id))
            .filter(|def| def.is_weapon())
            .map_or(0.0, |def| def.weight)
    }

    pub fn set_item_weight(&mut self, id: ItemId, weight: f32) -> bool {
        if !weight.is_finite() || weight < 0.0 {
            return false;
        }
        self.items[id.0].weight = weight;
        true
    }

    pub fn set_item_stack(&mut self, id: ItemId, max_stack: i32) -> bool {
        if max_stack < 1 || self.item(id).is_weapon() {
            return false;
        }
        self.items[id.0].max_stack = max_stack;
        true
    }

    pub fn set_item_fuel(&mut self, id: ItemId, fuel: i32) -> bool {
        if fuel < 0 || self.item(id).is_weapon() {
            return false;
        }
        self.items[id.0].fuel = fuel;
        true
    }

    pub fn ammo_max(&self, kind: AmmoKind) -> i32 {
        self.ammo_max_override(kind)
            .unwrap_or(kind.definition().default_max)
    }

    pub fn ammo_max_override(&self, kind: AmmoKind) -> Option<i32> {
        self.ammo_max[kind as usize]
    }

    pub fn set_ammo_max(&mut self, kind: AmmoKind, rounds: Option<i32>) -> bool {
        if rounds.is_some_and(|value| value < 0) {
            return false;
        }
        self.ammo_max[kind as usize] = rounds;
        true
    }

    pub fn round_weight(&self, kind: AmmoKind) -> f32 {
        self.round_weight[kind as usize].unwrap_or(kind.definition().default_weight)
    }

    pub fn set_round_weight(&mut self, kind: AmmoKind, weight: f32) -> bool {
        if !weight.is_finite() || weight < 0.0 {
            return false;
        }
        self.round_weight[kind as usize] = Some(weight);
        true
    }

    pub(crate) fn reset_round_weights(&mut self) {
        self.round_weight.fill(None);
    }

    pub fn ammo_weight(&self, pool: &[i32; 32]) -> f32 {
        AmmoKind::ALL
            .iter()
            .map(|&kind| pool[kind.definition().index].max(0) as f32 * self.round_weight(kind))
            .sum()
    }

    pub fn split_unload(
        &self,
        kind: Option<AmmoKind>,
        rounds: i32,
        reserve: i32,
        weight_room: Option<f32>,
    ) -> UnloadSplit {
        let rounds = rounds.max(0);
        let mut fit = rounds;
        if let Some(kind) = kind {
            fit = fit.min((self.ammo_max(kind) - reserve.max(0)).max(0));
            let unit = self.round_weight(kind);
            if let Some(room) = weight_room.filter(|room| *room >= 0.0 && unit > 0.0) {
                fit = fit.min((room / unit + 1e-4).floor() as i32);
            }
        }
        UnloadSplit {
            to_reserve: fit,
            to_ground: rounds - fit,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct UnloadSplit {
    pub to_reserve: i32,
    pub to_ground: i32,
}
