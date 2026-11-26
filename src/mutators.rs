use crate::rules::MutatorRules;

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum Mutator {
    Fog,
    Scarcity,
    HeavyLoad,
    Contagion,
    Brittle,
    HordeTide,
    Hunted,
    Crowd,
    Witch,
    Tank,
}

impl Mutator {
    pub const ALL: [Self; 10] = [
        Self::Fog,
        Self::Scarcity,
        Self::HeavyLoad,
        Self::Contagion,
        Self::Brittle,
        Self::HordeTide,
        Self::Hunted,
        Self::Crowd,
        Self::Witch,
        Self::Tank,
    ];

    pub fn from_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|mutator| mutator.id() == id)
    }

    pub fn id(self) -> &'static str {
        match self {
            Self::Fog => "fog",
            Self::Scarcity => "scarcity",
            Self::HeavyLoad => "heavy_load",
            Self::Contagion => "contagion",
            Self::Brittle => "brittle",
            Self::HordeTide => "horde_tide",
            Self::Hunted => "hunted",
            Self::Crowd => "crowd",
            Self::Witch => "witch",
            Self::Tank => "tank",
        }
    }

    pub fn title(self) -> &'static str {
        match self {
            Self::Fog => "Fog",
            Self::Scarcity => "Scarcity",
            Self::HeavyLoad => "Heavy load",
            Self::Contagion => "Contagion",
            Self::Brittle => "Brittle",
            Self::HordeTide => "Horde tide",
            Self::Hunted => "Hunted",
            Self::Crowd => "Crowd",
            Self::Witch => "Witch",
            Self::Tank => "Tank",
        }
    }
}

pub fn pick_mutators(rules: &MutatorRules, map: &str, seed: u32) -> Vec<Mutator> {
    if !rules.enabled {
        return Vec::new();
    }
    if let Some(pinned) = rules.maps.get(map) {
        return pinned.clone();
    }

    let mut state = if seed == 0 { 0x9e37_79b9 } else { seed };
    let mut next = || {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        state
    };
    if (next() % 100) as i32 >= rules.chance {
        return Vec::new();
    }

    let mut pool = Vec::new();
    for &mutator in &rules.pool {
        if !pool.contains(&mutator) {
            pool.push(mutator);
        }
    }
    let mut available: Vec<_> = (0..pool.len()).collect();
    for _ in 0..(rules.count.max(0) as usize).min(pool.len()) {
        available.remove(next() as usize % available.len());
    }
    pool.into_iter()
        .enumerate()
        .filter_map(|(index, mutator)| (!available.contains(&index)).then_some(mutator))
        .collect()
}
