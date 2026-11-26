use std::{
    collections::{HashMap, HashSet},
    ffi::{CString, c_char, c_void},
    io, ptr,
    sync::atomic::{AtomicUsize, Ordering},
    time::{Duration, Instant},
};

use inventory_mod::{
    infection_sounds::{COUGHS, VOICES},
    rules::Rules,
};

use super::{
    ENTITY_ORIGIN, Game, Host, Vector, address,
    hooks::{Event, Hooks},
};
use crate::{
    client::with_client,
    dedicated,
    native::{self, Detour},
};

type Suicide = unsafe extern "thiscall" fn(*mut c_void, bool, bool);
type Rescue = unsafe extern "thiscall" fn(*mut c_void, *mut c_void);
type Script = unsafe extern "C" fn(*const Command);

// CSurvivorDeathModel::SetPlayer copies the survivor-character field into the
// death model's networked m_nCharacterType on the supported Windows builds.
const SURVIVOR_CHARACTER: usize = 0x2bb4;
const CORPSE_CHARACTER: usize = 0x17d8;

static RESCUE_ORIGINAL: AtomicUsize = AtomicUsize::new(0);

#[repr(C)]
struct Command {
    argc: i32,
    first_size: i32,
    command: [u8; 512],
    first: [u8; 512],
    argv: [*const c_char; 64],
}

pub struct Infection {
    rescue: Detour,
    suicide: Suicide,
    script: Script,
    cases: HashMap<i32, InfectionCase>,
    survivors: HashSet<i32>,
    bodies: Vec<Body>,
    random: u32,
}

#[derive(Clone, Copy)]
struct InfectionCase {
    end: Instant,
    duration_ms: u32,
    cough: Instant,
    insane: Option<Instant>,
    told: bool,
}

struct Body {
    character: u32,
    existing: HashSet<u32>,
    at: Vector,
    retry: Instant,
    end: Instant,
}

impl Infection {
    pub fn install(game: &Game, rules: &Rules, seed: u32) -> Option<Self> {
        if !rules.infection.enabled
            && !rules
                .mutators
                .pool
                .contains(&inventory_mod::mutators::Mutator::Contagion)
            && !rules
                .mutators
                .maps
                .values()
                .any(|ids| ids.contains(&inventory_mod::mutators::Mutator::Contagion))
        {
            return None;
        }
        let (script, rescue) = if game.image.timestamp == 0x6afc_cca0 {
            (0x21cd00, 0x2cebc0)
        } else {
            (0x21cb90, 0x2ceaf0)
        };
        let script = game.image.base + script;
        let rescue = game.image.base + rescue;
        let suicide = native::word(game.player_table + 4 * 476)? as usize;
        for (at, pattern) in [
            (script, "55 8B EC 56 8B 75 08 83 3E 01 7E 0B"),
            (rescue, "55 8B EC 51 8B 15 ?? ?? ?? ?? 53 56 8B F1"),
            (suicide, "55 8B EC 83 EC 64"),
        ] {
            if !native::matches_pattern(at, pattern) {
                return None;
            }
        }
        let mut detour = Detour::prepare(rescue, block_rescue as Rescue as usize).ok()?;
        RESCUE_ORIGINAL.store(detour.trampoline(), Ordering::Release);
        if detour.enable().is_err() {
            RESCUE_ORIGINAL.store(0, Ordering::Release);
            return None;
        }
        Some(Self {
            rescue: detour,
            suicide: unsafe { std::mem::transmute::<usize, Suicide>(suicide) },
            script: unsafe { std::mem::transmute::<usize, Script>(script) },
            cases: HashMap::new(),
            survivors: HashSet::new(),
            bodies: Vec::new(),
            random: seed.max(1),
        })
    }

    pub fn stop(&mut self) -> io::Result<()> {
        self.rescue.disable()?;
        RESCUE_ORIGINAL.store(0, Ordering::Release);
        Ok(())
    }

    fn random(&mut self) -> u32 {
        self.random ^= self.random << 13;
        self.random ^= self.random >> 17;
        self.random ^= self.random << 5;
        self.random
    }

    fn voice_delay(&mut self, expected: f32, minimum: f32) -> Duration {
        let factor = 0.6 + 0.8 * self.random() as f32 / u32::MAX as f32;
        Duration::from_secs_f32((expected * factor).max(minimum))
    }

    fn game_script(&self, code: &str) {
        call_script(self.script, code);
    }
}

pub(super) fn panic_event(image: &native::PeImage) {
    let at = image.base
        + if image.timestamp == 0x6afc_cca0 {
            0x21cd00
        } else {
            0x21cb90
        };
    if native::matches_pattern(at, "55 8B EC 56 8B 75 08 83 3E 01 7E 0B") {
        let script = unsafe { std::mem::transmute::<usize, Script>(at) };
        call_script(script, "Director.ForcePanicEvent()");
    }
}

fn call_script(script: Script, code: &str) {
    // Only fixed game calls with bounded numbers and known sound names are supplied here.
    if code.len() > 480 || code.as_bytes().contains(&0) {
        return;
    }
    let mut args = Command {
        argc: 2,
        first_size: 6,
        command: [0; 512],
        first: [0; 512],
        argv: [ptr::null(); 64],
    };
    args.command[..7].copy_from_slice(b"script ");
    args.command[7..7 + code.len()].copy_from_slice(code.as_bytes());
    args.first[..6].copy_from_slice(b"script");
    args.argv[0] = args.first.as_ptr().cast();
    args.argv[1] = args.command[7..].as_ptr().cast();
    unsafe { script(&args) };
}

impl Infection {
    fn sound(&mut self, actor: usize, user: i32, cough: bool, stage: f32) {
        let Some(model) =
            native::word(actor + 0x24c).and_then(|addr| native::string(addr as usize, 120))
        else {
            return;
        };
        let name = if cough {
            COUGHS.iter().find_map(|&(part, prefix, count)| {
                model
                    .contains(part)
                    .then(|| format!("{prefix}_Cough0{}", 1 + self.random() % count))
            })
        } else {
            VOICES.iter().find_map(|voice| {
                if !model.contains(voice.model) {
                    return None;
                }
                let probability = self.random() as f32 / u32::MAX as f32;
                let sounds = if probability < 0.3 + 0.4 * stage {
                    voice.scream
                } else {
                    voice.laugh
                };
                Some(sounds[self.random() as usize % sounds.len()].to_owned())
            })
        };
        if let Some(name) = name {
            self.game_script(&format!(
                "GetPlayerFromUserID({user}).PrecacheScriptSound(\"{name}\")"
            ));
            self.game_script(&format!(
                "EmitSoundOn(\"{name}\", GetPlayerFromUserID({user}))"
            ));
        }
    }

    fn remove_bodies(&mut self, game: &Game, now: Instant) {
        self.bodies.retain_mut(|body| {
            if now < body.retry {
                return true;
            }
            let closest = (1..4096)
                .map(|n| game.entity(n))
                .filter(|&entity| entity != 0)
                .filter_map(|entity| {
                    let position = Vector::read(entity + ENTITY_ORIGIN)?;
                    let delta = position.difference(body.at);
                    let distance = delta.dot(delta);
                    if distance < 250.0 * 250.0
                        && game.classname(entity).as_deref() == Some("survivor_death_model")
                        && native::word(entity + CORPSE_CHARACTER) == Some(body.character)
                        && super::ammo::handle(game, entity)
                            .is_some_and(|h| !body.existing.contains(&h))
                    {
                        Some((entity, distance))
                    } else {
                        None
                    }
                })
                .min_by(|a, b| a.1.total_cmp(&b.1));
            if let Some((entity, _)) = closest {
                game.remove(entity);
                false
            } else {
                body.retry = now + Duration::from_millis(200);
                now < body.end
            }
        });
    }
}

impl Host {
    pub fn infect(&mut self, actor: usize) {
        let Some(hooks) = &self.hooks else {
            return;
        };
        let Some(edict) = native::word(actor + 0x28)
            .map(|addr| addr as usize)
            .filter(|&edict| edict != 0)
        else {
            return;
        };
        let Some(id) = hooks.user(edict) else {
            return;
        };
        let rules = &self.effective.infection;
        if !rules.enabled
            || !self.game.living(actor)
            || (!rules.bots && hooks.steam_id(edict).as_deref() == Some("BOT"))
        {
            return;
        }
        let Some(infection) = &mut self.infection else {
            return;
        };
        let now = Instant::now();
        let duration_ms = rules.seconds.max(1) as u32 * 1000;
        let mut case = if let Some(mut active) = infection.cases.remove(&id) {
            active.end = active
                .end
                .checked_sub(Duration::from_secs(rules.hit_seconds as u64))
                .unwrap_or(now)
                .max(now + Duration::from_secs(1));
            active
        } else {
            InfectionCase {
                end: now + Duration::from_millis(u64::from(duration_ms)),
                duration_ms,
                cough: now + infection.voice_delay(26.0, 2.5),
                insane: None,
                told: false,
            }
        };
        case.told = self.infection_message(id, actor, Some(case), false);
        if let Some(infection) = &mut self.infection {
            infection.cases.insert(id, case);
        }
    }

    fn infection_message(
        &mut self,
        user: i32,
        actor: usize,
        infection: Option<InfectionCase>,
        turned: bool,
    ) -> bool {
        let (left, total) = infection.map_or((0, 0), |case| {
            (
                case.end
                    .saturating_duration_since(Instant::now())
                    .as_millis(),
                case.duration_ms,
            )
        });
        let line = format!("@inf@ {left} {total} {}\n", u8::from(turned));
        if self.is_local_player(actor) {
            self.hud.push(line);
            true
        } else if let (Some(hooks), Some(guest)) = (&self.hooks, self.guests.get(&user))
            && hooks.user(guest.edict) == Some(user)
        {
            if let Ok(line) = CString::new(line) {
                hooks.print(guest.edict, &line);
            }
            true
        } else {
            false
        }
    }

    fn infection_actor(&self, hooks: &Hooks, user: i32) -> Option<usize> {
        if user <= 0 {
            return None;
        }
        (1..65)
            .map(|index| self.game.entity(index))
            .filter(|&actor| actor != 0)
            .find(|&actor| {
                self.game.classname(actor).as_deref() == Some("player")
                    && native::word(actor + 0x28).and_then(|edict| hooks.user(edict as usize))
                        == Some(user)
            })
    }

    pub fn infection_event(&mut self, event: Event) {
        let Some(hooks) = &self.hooks else {
            return;
        };
        if self.infection.is_none() {
            return;
        }
        match event {
            Event::Exposure(user) => {
                if let Some(actor) = self.infection_actor(hooks, user) {
                    self.infect(actor);
                }
            }
            Event::Down {
                user,
                attacker,
                entity,
            } => {
                let infected = if let Some(attacker) = self.infection_actor(hooks, attacker) {
                    native::word(attacker + super::PLAYER_TEAM) == Some(3)
                } else if (1..4096).contains(&entity) {
                    let attacker = self.game.entity(entity as usize);
                    attacker != 0
                        && matches!(
                            self.game.classname(attacker).as_deref(),
                            Some("infected" | "witch")
                        )
                } else {
                    false
                };
                if infected && let Some(actor) = self.infection_actor(hooks, user) {
                    self.infect(actor);
                }
            }
            Event::Pills(user) => {
                let actor = self.infection_actor(hooks, user);
                if let Some(actor) = actor
                    && let Some(mut case) = self
                        .infection
                        .as_mut()
                        .and_then(|infection| infection.cases.remove(&user))
                {
                    if self.effective.infection.no_cure {
                        case.end =
                            Instant::now() + Duration::from_millis(u64::from(case.duration_ms));
                        case.insane = None;
                        case.told = self.infection_message(user, actor, Some(case), false);
                        if let Some(infection) = &mut self.infection {
                            infection.cases.insert(user, case);
                        }
                    } else {
                        self.infection_message(user, actor, None, false);
                    }
                }
            }
            Event::Death(user) => {
                if let Some(infection) = &mut self.infection
                    && infection.cases.remove(&user).is_some()
                    && let Some(actor) = self.infection_actor(hooks, user)
                {
                    self.infection_message(user, actor, None, false);
                }
            }
            Event::RoundStart | Event::Transition | Event::RoundEnd => {
                self.clear_infection(!matches!(event, Event::RoundEnd))
            }
            Event::LeftStart | Event::PlayerTransitioned(_) => {}
        }
    }

    pub fn clear_infection(&mut self, all: bool) {
        let Some(infection) = &mut self.infection else {
            return;
        };
        let cases = std::mem::take(&mut infection.cases);
        if all {
            infection.survivors.clear();
            infection.bodies.clear();
        }
        for user in cases.keys().copied() {
            if let Some(actor) = self
                .hooks
                .as_ref()
                .and_then(|hooks| self.infection_actor(hooks, user))
            {
                self.infection_message(user, actor, None, false);
            }
        }
    }

    pub fn infection_frame(&mut self) {
        let Some(mut infection) = self.infection.take() else {
            return;
        };
        let now = Instant::now();
        for (id, mut case) in std::mem::take(&mut infection.cases) {
            let actor = self
                .hooks
                .as_ref()
                .and_then(|hooks| self.infection_actor(hooks, id));
            let Some(actor) = actor.filter(|&actor| self.game.living(actor)) else {
                continue;
            };
            if now >= case.end {
                let Some(at) = Vector::read(actor + ENTITY_ORIGIN) else {
                    continue;
                };
                let character = native::word(actor + SURVIVOR_CHARACTER);
                let existing = (1..4096)
                    .filter_map(|i| {
                        let entity = self.game.entity(i);
                        (entity != 0
                            && self.game.classname(entity).as_deref()
                                == Some("survivor_death_model"))
                        .then(|| super::ammo::handle(&self.game, entity))
                        .flatten()
                    })
                    .collect();
                unsafe { (infection.suicide)(address(actor), false, true) };
                if self.game.living(actor) {
                    self.infection_message(id, actor, None, false);
                    crate::client::log("infection: the game refused CommitSuicide");
                    continue;
                }
                self.infection_message(id, actor, None, true);
                infection.survivors.insert(id);
                let specials = &self.effective.infection.specials;
                let kind = if specials.is_empty() {
                    3
                } else {
                    specials[infection.random() as usize % specials.len()]
                };
                infection.game_script(&format!(
                    "ZSpawn({{type={kind},pos=Vector({:.1},{:.1},{:.1})}})",
                    at.x,
                    at.y,
                    at.z + 4.0
                ));
                if let Some(character) = character {
                    infection.bodies.push(Body {
                        character,
                        existing,
                        at,
                        retry: now + Duration::from_millis(200),
                        end: now + Duration::from_secs(3),
                    });
                }
                continue;
            }
            if !case.told {
                case.told = self.infection_message(id, actor, Some(case), false);
            }
            let rules = &self.effective.infection;
            let severity = (1.0
                - case.end.duration_since(now).as_secs_f32() * 1000.0 / case.duration_ms as f32)
                .clamp(0.0, 1.0);
            if rules.cough && now >= case.cough {
                infection.sound(actor, id, true, severity);
                case.cough = now + infection.voice_delay(26.0 - 22.0 * severity, 2.5);
                case.insane = case
                    .insane
                    .map(|value| value.max(now + Duration::from_millis(2500)));
            }
            if !rules.insane || severity * 100.0 < rules.insane_at as f32 {
                case.insane = None;
            } else if let Some(at) = case.insane {
                if now >= at {
                    let at = rules.insane_at as f32;
                    let stage = if at >= 100.0 {
                        1.0
                    } else {
                        (severity * 100.0 - at) / (100.0 - at)
                    };
                    infection.sound(actor, id, false, stage);
                    case.insane = Some(now + infection.voice_delay(18.0 - 13.0 * stage, 3.0));
                    case.cough = case.cough.max(now + Duration::from_secs(3));
                }
            } else {
                case.insane =
                    Some(now + Duration::from_millis(2000 + u64::from(infection.random() % 4000)));
            }
            infection.cases.insert(id, case);
        }
        infection.remove_bodies(&self.game, now);
        self.infection = Some(infection);
    }

    fn rescue_allowed(&self, actor: usize) -> bool {
        if let Some(infection) = &self.infection
            && !infection.survivors.is_empty()
            && let Some(user) = native::word(actor + 0x28)
                .and_then(|edict| self.hooks.as_ref()?.user(edict as usize))
        {
            !infection.survivors.contains(&user)
        } else {
            true
        }
    }
}

unsafe extern "thiscall" fn block_rescue(rescue: *mut c_void, survivor: *mut c_void) {
    let allowed = survivor.is_null()
        || std::panic::catch_unwind(|| {
            with_client(|client| {
                client
                    .host
                    .as_ref()
                    .map(|host| host.rescue_allowed(survivor.addr()))
            })
            .flatten()
            .or_else(|| dedicated::with_host(|host| host.rescue_allowed(survivor.addr())))
            .unwrap_or(true)
        })
        .unwrap_or(true);
    let original = RESCUE_ORIGINAL.load(Ordering::Acquire);
    if original != 0 {
        let original = unsafe { std::mem::transmute::<usize, Rescue>(original) };
        unsafe { original(rescue, if allowed { survivor } else { ptr::null_mut() }) };
    }
}
