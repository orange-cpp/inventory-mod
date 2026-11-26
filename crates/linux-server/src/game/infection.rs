use std::{
    cell::Cell,
    collections::{HashMap, HashSet},
    ffi::{CString, c_void},
    ptr::{self, NonNull},
    time::{Duration, Instant},
};

use inventory_mod::rules::{InfectionRules, Rules};

use super::{Ent, Game, Vector, field, native::*, saved_frame::SavedFrame};
use crate::{
    plugin,
    source::{Clients, Edict, Entity, Source},
};

use inventory_mod::infection_sounds as voices;

type RescueCall = unsafe extern "C" fn(*mut Entity, *mut Entity);

thread_local! {
    static ORIGINAL: Cell<Option<RescueCall>> = const { Cell::new(None) };
}

#[derive(Default)]
pub struct Infection {
    active: HashMap<i32, Case>,
    turning: HashMap<i32, Turn>,
    no_rescue: HashSet<i32>,
    corpses: Vec<Corpse>,
    hook: Option<SavedFrame>,
}

#[derive(Clone, Copy)]
struct Patient {
    edict: NonNull<Edict>,
    handle: u32,
}

#[derive(Clone, Copy)]
struct Case {
    who: Patient,
    until: Instant,
    total_ms: u32,
    cough_at: Instant,
    insane_at: Option<Instant>,
    told: bool,
}

struct Turn {
    who: Patient,
    position: Vector,
    model: i32,
    deadline: Instant,
}

struct Corpse {
    position: Vector,
    model: i32,
    deadline: Instant,
}

#[derive(Clone, Copy)]
pub enum Exposure {
    Grab(i32),
    KnockedDown {
        user: i32,
        attacker: i32,
        entity_index: i32,
    },
    Pills(i32),
}

impl Game {
    pub fn install_infection(&mut self, rules: &Rules) -> bool {
        if !rules.infection.enabled && !rules.mutators.enabled {
            return true;
        }
        self.infection.hook = unsafe {
            SavedFrame::install(
                self.native.rescue_call as usize,
                rescue_call as RescueCall as usize,
            )
        };
        if let Some(hook) = &self.infection.hook {
            // SAFETY: The trampoline preserves the original callback's exact ABI.
            ORIGINAL.set(Some(unsafe {
                std::mem::transmute::<usize, RescueCall>(hook.original())
            }));
            true
        } else {
            false
        }
    }

    pub fn uninstall_infection(&mut self, source: &Source, receives: impl Fn(i32) -> bool) {
        self.clear_infection(source, true, receives);
        self.infection.hook = None;
        ORIGINAL.set(None);
    }

    fn patient(&self, source: &Source, user: i32, patient: Patient) -> Option<Ent> {
        if source.user_id(patient.edict) != Some(user) {
            return None;
        }
        source
            .entity(patient.edict)
            .filter(|&player| self.handle(player) == patient.handle)
    }

    fn tell(
        &self,
        source: &Source,
        user: i32,
        state: Case,
        now: Instant,
        turned: bool,
        clear: bool,
    ) -> bool {
        if self.patient(source, user, state.who).is_none() {
            return false;
        }
        let (left, total) = if clear {
            (0, 0)
        } else {
            (
                state.until.saturating_duration_since(now).as_millis(),
                state.total_ms,
            )
        };
        let message = CString::new(format!("@inf@ {left} {total} {}\n", u8::from(turned))).unwrap();
        source.client_print(state.who.edict, &message);
        true
    }

    pub fn infect(
        &mut self,
        source: &Source,
        edict: NonNull<Edict>,
        rules: &InfectionRules,
        receives: bool,
    ) {
        if !rules.enabled
            || self.infection.hook.is_none()
            || !source.player_info(edict).is_some_and(|info| {
                info.connected && !info.dead && info.team == 2 && (rules.bots || !info.fake)
            })
        {
            return;
        }
        let (Some(user), Some(player)) = (source.user_id(edict), source.entity(edict)) else {
            return;
        };
        if self.infection.turning.contains_key(&user) {
            return;
        }
        let who = Patient {
            edict,
            handle: self.handle(player),
        };
        let now = Instant::now();
        let mut state = if let Some(mut state) = self
            .infection
            .active
            .remove(&user)
            .filter(|case| case.who.handle == who.handle)
        {
            state.until = state
                .until
                .checked_sub(Duration::from_secs(rules.hit_seconds as u64))
                .unwrap_or(now)
                .max(now + Duration::from_secs(1));
            state
        } else {
            Case {
                who,
                until: now + Duration::from_secs(rules.seconds as u64),
                total_ms: rules.seconds as u32 * 1000,
                cough_at: now + self.voice_delay(26.0, 2.5),
                insane_at: None,
                told: false,
            }
        };
        state.told = receives && self.tell(source, user, state, now, false, false);
        self.infection.active.insert(user, state);
    }

    pub fn forget_infection(&mut self, source: &Source, user: i32, receives: bool) {
        if let Some(state) = self.infection.active.remove(&user)
            && receives
        {
            self.tell(source, user, state, Instant::now(), false, true);
        }
    }

    pub fn clear_infection(
        &mut self,
        source: &Source,
        round_end: bool,
        receives: impl Fn(i32) -> bool,
    ) {
        let now = Instant::now();
        for (user, state) in std::mem::take(&mut self.infection.active) {
            if receives(user) {
                self.tell(source, user, state, now, false, true);
            }
        }
        if round_end {
            self.infection.turning.clear();
            self.infection.no_rescue.clear();
            self.infection.corpses.clear();
        }
    }

    pub fn exposure(
        &mut self,
        source: &Source,
        clients: Option<&Clients>,
        exposure: Exposure,
        rules: &InfectionRules,
        receives: impl Fn(i32) -> bool,
    ) {
        if !rules.enabled || self.infection.hook.is_none() {
            return;
        }
        let user_edict = |user: i32| {
            clients.and_then(|list| {
                list.live()
                    .find(|&edict| source.user_id(edict) == Some(user))
            })
        };
        let user = match exposure {
            Exposure::Grab(user) => user,
            Exposure::Pills(user) => {
                if !rules.no_cure {
                    self.forget_infection(source, user, receives(user));
                } else if let Some(mut state) = self.infection.active.remove(&user) {
                    let now = Instant::now();
                    state.until = now + Duration::from_millis(u64::from(state.total_ms));
                    state.insane_at = None;
                    state.told =
                        receives(user) && self.tell(source, user, state, now, false, false);
                    self.infection.active.insert(user, state);
                }
                return;
            }
            Exposure::KnockedDown {
                user,
                attacker,
                entity_index,
            } => {
                let hostile = if let Some(edict) = user_edict(attacker) {
                    source.player_info(edict).is_some_and(|info| info.team == 3)
                } else if (1..4096).contains(&entity_index) {
                    source
                        .entities()
                        .find(|&entity| self.handle(entity) & 0xfff == entity_index as u32)
                        .is_some_and(|entity| {
                            matches!(self.classname(entity), "infected" | "witch")
                        })
                } else {
                    false
                };
                if !hostile {
                    return;
                }
                user
            }
        };
        if let Some(edict) = user_edict(user) {
            self.infect(source, edict, rules, receives(user));
        }
    }

    fn voice_delay(&mut self, seconds: f32, minimum: f32) -> Duration {
        let spread = 0.6 + 0.8 * self.round_seed() as f32 / u32::MAX as f32;
        Duration::from_millis((1000.0 * minimum.max(seconds * spread)) as u64)
    }

    fn infection_voice(&mut self, model: &str, player: Ent, cough: bool, stage: f32) {
        let sound = if cough {
            voices::COUGHS.iter().find_map(|&(part, prefix, count)| {
                model
                    .contains(part)
                    .then(|| format!("{prefix}_Cough0{}", 1 + self.round_seed() % count))
            })
        } else {
            voices::VOICES.iter().find_map(|voice| {
                if !model.contains(voice.model) {
                    return None;
                }
                let chance = self.round_seed() as f32 / u32::MAX as f32;
                let sounds = if chance < 0.3 + 0.4 * stage {
                    voice.scream
                } else {
                    voice.laugh
                };
                Some(sounds[self.round_seed() as usize % sounds.len()].to_owned())
            })
        };
        if let Some(sound) = sound.and_then(|sound| CString::new(sound).ok()) {
            unsafe {
                (self.native.precache_sound)(player.as_ptr(), sound.as_ptr());
                (self.native.emit_sound)(player.as_ptr(), sound.as_ptr(), 0.0, ptr::null_mut());
            }
        }
    }

    fn turn(&mut self, source: &Source, user: i32, case: Case, now: Instant, receives: bool) {
        let Some(player) = self.patient(source, user, case.who) else {
            return;
        };
        let Some(position) = self.position(player) else {
            return;
        };
        let model = unsafe { field(player, SURVIVOR_CHARACTER) };
        self.infection.turning.insert(
            user,
            Turn {
                who: case.who,
                position,
                model,
                deadline: now + Duration::from_secs(1),
            },
        );
        self.infection.no_rescue.insert(user);
        if receives {
            self.tell(source, user, case, now, true, true);
        }
        unsafe { (self.native.commit_suicide)(player.as_ptr(), false, true) };
    }

    fn finish_turns(&mut self, source: &Source, rules: &InfectionRules, now: Instant) {
        for (user, turn) in std::mem::take(&mut self.infection.turning) {
            let player = self.patient(source, user, turn.who);
            if player.is_some_and(|player| unsafe { field::<u8>(player, LIFE_STATE) != 0 }) {
                self.infection.corpses.push(Corpse {
                    position: turn.position,
                    model: turn.model,
                    deadline: now + Duration::from_secs(3),
                });
                let manager =
                    ptr::with_exposed_provenance::<*mut c_void>(self.native.base + ZOMBIE_MANAGER);
                let manager = unsafe { manager.read() };
                if !manager.is_null() {
                    let kind = if rules.specials.is_empty() {
                        3
                    } else {
                        rules.specials[self.round_seed() as usize % rules.specials.len()]
                    };
                    let at = turn.position.offset(0.0, 0.0, 4.0);
                    let facing = Vector::default();
                    if !unsafe { (self.native.spawn_special)(manager, kind, &at, &facing) }
                        .is_null()
                    {
                        eprintln!(
                            "inventory_mod: player {user} turned into special infected {kind}"
                        );
                    }
                }
            } else if player.is_none() || now >= turn.deadline {
                self.infection.no_rescue.remove(&user);
            } else {
                self.infection.turning.insert(user, turn);
            }
        }
        for corpse in std::mem::take(&mut self.infection.corpses) {
            let closest = source
                .entities()
                .filter_map(|entity| {
                    if self.classname(entity) != "survivor_death_model"
                        || self.pending_removal(entity)
                        || unsafe { field::<i32>(entity, CORPSE_CHARACTER) } != corpse.model
                    {
                        return None;
                    }
                    let distance = self.position(entity)?.offset(
                        -corpse.position.x,
                        -corpse.position.y,
                        -corpse.position.z,
                    );
                    let distance = distance.dot(distance);
                    (distance < 250.0 * 250.0).then_some((distance, entity))
                })
                .min_by(|a, b| a.0.total_cmp(&b.0));
            if let Some((_, entity)) = closest {
                self.remove(entity);
            } else if now < corpse.deadline {
                self.infection.corpses.push(corpse);
            }
        }
    }

    pub fn infection_frame(
        &mut self,
        source: &Source,
        rules: &InfectionRules,
        receives: impl Fn(i32) -> bool,
    ) {
        let now = Instant::now();
        let active: Vec<_> = self.infection.active.keys().copied().collect();
        for user in active {
            let Some(mut state) = self.infection.active.remove(&user) else {
                continue;
            };
            let receiving = receives(user);
            let player = self
                .patient(source, user, state.who)
                .filter(|&player| unsafe { field::<u8>(player, LIFE_STATE) == 0 });
            let info = source.player_info(state.who.edict);
            let Some(player) =
                player.filter(|_| info.is_some_and(|info| info.connected && info.team == 2))
            else {
                if receiving {
                    self.tell(source, user, state, now, false, true);
                }
                continue;
            };
            if now >= state.until {
                self.turn(source, user, state, now, receiving);
                continue;
            }
            if !state.told && receiving {
                state.told = self.tell(source, user, state, now, false, false);
            }
            let fraction = 1.0
                - state.until.duration_since(now).as_secs_f32() * 1000.0 / state.total_ms as f32;
            let severity = fraction.clamp(0.0, 1.0);
            if rules.cough && now >= state.cough_at {
                if let Some(model) = source.model_name(state.who.edict) {
                    self.infection_voice(model, player, true, severity);
                }
                state.cough_at = now + self.voice_delay(26.0 - 22.0 * severity, 2.5);
                state.insane_at = state
                    .insane_at
                    .map(|at| at.max(now + Duration::from_millis(2500)));
            }
            if !rules.insane || severity * 100.0 < rules.insane_at as f32 {
                state.insane_at = None;
            } else if let Some(at) = state.insane_at {
                if now >= at {
                    let threshold = rules.insane_at as f32;
                    let stage = if threshold >= 100.0 {
                        1.0
                    } else {
                        (severity * 100.0 - threshold) / (100.0 - threshold)
                    };
                    if let Some(model) = source.model_name(state.who.edict) {
                        self.infection_voice(model, player, false, stage);
                    }
                    state.insane_at = Some(now + self.voice_delay(18.0 - 13.0 * stage, 3.0));
                    state.cough_at = state.cough_at.max(now + Duration::from_secs(3));
                }
            } else {
                state.insane_at =
                    Some(now + Duration::from_millis(2000 + u64::from(self.round_seed() % 4000)));
            }
            self.infection.active.insert(user, state);
        }
        self.finish_turns(source, rules, now);
    }

    pub fn can_rescue(&self, source: &Source, player: Ent) -> bool {
        self.infection.no_rescue.is_empty()
            || source
                .as_edict(player)
                .and_then(|edict| source.user_id(edict))
                .is_none_or(|id| !self.infection.no_rescue.contains(&id))
    }
}

unsafe extern "C" fn rescue_call(rescue: *mut Entity, survivor: *mut Entity) {
    let allowed = NonNull::new(survivor).is_none_or(plugin::can_rescue);
    if let Some(original) = ORIGINAL.get() {
        unsafe { original(rescue, if allowed { survivor } else { ptr::null_mut() }) };
    }
}
