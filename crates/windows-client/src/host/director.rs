use std::ffi::CString;

use inventory_mod::{items::AmmoKind, mutators::Mutator};

use crate::cvars::{ConVar, Restore};

use super::Host;

#[derive(Default)]
pub struct Director {
    capacity: Vec<Restore>,
    originals: Vec<Restore>,
    next_horde: Option<f32>,
}

impl Host {
    pub fn apply_reserve_caps(&mut self) {
        for kind in AmmoKind::ALL {
            let Some(amount) = self.rules.catalog.ammo_max_override(kind) else {
                continue;
            };
            let Ok(name) = CString::new(format!("ammo_{}_max", kind.name())) else {
                continue;
            };
            if let Some(before) =
                ConVar::find(&name).and_then(|var| var.change(&amount.to_string()))
            {
                self.director.capacity.push(before);
            }
        }
    }

    pub fn director_chapter(&mut self) {
        use std::ffi::CStr;
        for saved in self.director.originals.drain(..).rev() {
            saved.restore();
        }
        self.director.next_horde = None;
        let mut set = |name: &CStr, enabled: i32, whole: bool, absolute: bool| {
            let Some(var) = ConVar::find(name) else {
                return;
            };
            let Some(original) = var.scalar() else {
                return;
            };
            let desired = if absolute {
                enabled as f32
            } else {
                original * enabled as f32 / 100.0
            };
            let desired = if whole {
                desired.round().max(0.0)
            } else {
                desired
            };
            if let Some(prior) = var.change(&desired.to_string()) {
                self.director.originals.push(prior);
            }
        };
        let rules = &self.rules.mutators;
        for &kind in &self.mutators {
            match kind {
                Mutator::Hunted => {
                    let index = rules
                        .hunted
                        .class
                        .unwrap_or((self.generation % 6 + 1) as i32)
                        .max(1);
                    let names = [
                        c"z_smoker_limit",
                        c"z_boomer_limit",
                        c"z_hunter_limit",
                        c"z_spitter_limit",
                        c"z_jockey_limit",
                        c"z_charger_limit",
                    ];
                    if let Some(name) = names.get((index - 1) as usize) {
                        set(name, rules.hunted.limit, true, true);
                    }
                }
                Mutator::Crowd => {
                    set(c"z_common_limit", rules.crowd.commons, true, false);
                    for name in [
                        c"z_mob_spawn_min_size",
                        c"z_mob_spawn_max_size",
                        c"z_mega_mob_size",
                    ] {
                        set(name, rules.crowd.mobs, true, false);
                    }
                }
                Mutator::Witch => {
                    set(c"z_witch_health", rules.witch.health, true, false);
                    set(c"z_witch_anger_rate", rules.witch.anger, false, false);
                    set(c"z_witch_personal_space", rules.witch.space, false, false);
                    set(c"z_witch_damage", rules.witch.damage, false, false);
                }
                Mutator::Tank => {
                    set(c"z_tank_health", rules.tank.health, true, false);
                    set(c"z_tank_speed", rules.tank.speed, false, false);
                }
                _ => {}
            }
        }
    }

    pub fn director_reset(&mut self) {
        self.director.next_horde = None;
        for prior in self
            .director
            .originals
            .drain(..)
            .chain(self.director.capacity.drain(..))
            .rev()
        {
            prior.restore();
        }
    }

    pub fn left_start(&mut self) {
        if self.director.next_horde.is_none() && self.mutators.contains(&Mutator::HordeTide) {
            self.director.next_horde = self
                .hooks
                .as_ref()
                .and_then(|hooks| hooks.time())
                .map(|now| now + self.rules.mutators.horde_tide.first as f32);
        }
    }

    pub fn director_frame(&mut self) {
        let Some(hooks) = &self.hooks else {
            return;
        };
        if let Some(now) = hooks.time()
            && self.director.next_horde.is_some_and(|next| now >= next)
        {
            hooks.panic_event();
            self.director.next_horde = Some(now + self.rules.mutators.horde_tide.every as f32);
        }
    }
}
