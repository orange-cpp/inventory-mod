use std::{
    collections::HashMap,
    ffi::{CString, c_void},
    ptr,
    time::{Duration, Instant},
};

use inventory_mod::{items::Catalog, rules::PingRules};

use super::{
    ENTITY_ORIGIN, Game, Host, PLAYER_TEAM, Vector, address,
    ammo::handle,
    hooks::{self, Hooks},
};
use crate::native;

const GLOW: usize = 0x1dc;
const GLOW_COLOR: u32 = 235 | (70 << 8) | (60 << 16);
const MASK_SHOT: u32 = 0x4600_4003;

type TraceRay = unsafe extern "thiscall" fn(*mut c_void, *const Ray, u32, *mut Filter, *mut RawHit);

pub(super) struct Trace {
    object: usize,
    trace: TraceRay,
}

#[repr(C, align(16))]
struct Ray {
    start: [f32; 4],
    delta: [f32; 4],
    offset: [f32; 4],
    extents: [f32; 4],
    axis: *const c_void,
    ray: bool,
    swept: bool,
}

#[repr(C, align(4))]
struct RawHit([u8; 84]);

#[repr(C)]
struct Filter {
    methods: &'static Methods,
    ignore: usize,
    world: bool,
}

#[repr(C)]
struct Methods {
    hit: unsafe extern "thiscall" fn(*const Filter, *mut c_void, i32) -> bool,
    kind: extern "thiscall" fn(*const Filter) -> i32,
}

static FILTER: Methods = Methods {
    hit: should_hit,
    kind: filter_kind,
};

unsafe extern "thiscall" fn should_hit(filter: *const Filter, entity: *mut c_void, _: i32) -> bool {
    unsafe { !(*filter).world && (*filter).ignore != entity.addr() }
}

extern "thiscall" fn filter_kind(filter: *const Filter) -> i32 {
    i32::from(unsafe { (*filter).world })
}

struct Hit {
    end: Vector,
    normal: Vector,
    fraction: f32,
    entity: usize,
    start_solid: bool,
    sky: bool,
}

impl Trace {
    pub(super) fn bind() -> Option<Self> {
        let factory = hooks::factory("engine.dll")?;
        let object = unsafe { factory(c"EngineTraceServer003".as_ptr(), ptr::null_mut()) }.addr();
        let slot = native::vslot(object, 5)?;
        let engine = native::PeImage::loaded(native::module("engine.dll")?)?;
        if !engine.includes(slot, 5) {
            return None;
        }
        Some(Self {
            object,
            trace: unsafe { std::mem::transmute::<usize, TraceRay>(slot) },
        })
    }

    pub(super) fn visible(&self, eye: Vector, delta: Vector, player: usize, target: usize) -> bool {
        let hit = self.ray(eye, delta, 1.0, player, false);
        !hit.start_solid && (hit.fraction >= 1.0 || hit.entity == target)
    }
    fn ray(&self, origin: Vector, forward: Vector, reach: f32, player: usize, world: bool) -> Hit {
        let ray = Ray {
            start: [origin.x, origin.y, origin.z, 0.0],
            delta: [forward.x * reach, forward.y * reach, forward.z * reach, 0.0],
            offset: [0.0; 4],
            extents: [0.0; 4],
            axis: ptr::null(),
            ray: true,
            swept: true,
        };
        let edict = native::word(player + 0x28).unwrap_or_default() as usize;
        let ignore = if native::word(edict).unwrap_or_default() & (1 << 2) != 0 {
            native::word(edict + 12).unwrap_or_default() as usize
        } else {
            0
        };
        let mut filter = Filter {
            methods: &FILTER,
            ignore,
            world,
        };
        let mut result = RawHit([0; 84]);
        let mask = if world { 0x0200_400b } else { MASK_SHOT };
        unsafe { (self.trace)(address(self.object), &ray, mask, &mut filter, &mut result) };
        let word = |at| u32::from_ne_bytes(result.0[at..at + 4].try_into().unwrap());
        let vector = |at| Vector {
            x: f32::from_bits(word(at)),
            y: f32::from_bits(word(at + 4)),
            z: f32::from_bits(word(at + 8)),
        };
        Hit {
            end: vector(12),
            normal: vector(24),
            fraction: f32::from_bits(word(44)),
            entity: word(76) as usize,
            start_solid: result.0[55] != 0,
            sky: result.0[66] & 4 != 0,
        }
    }
}

pub struct Pings {
    trace: Option<Trace>,
    cooldown: HashMap<i32, Instant>,
    follows: HashMap<i32, Follow>,
    highlights: HashMap<u32, Highlight>,
    next: Option<Instant>,
}

struct Follow {
    handle: u32,
    team: i32,
    until: Instant,
    highlight: Option<Instant>,
}

struct Highlight {
    before: [u32; 4],
    flashing: u8,
    count: usize,
}

struct Target {
    at: Vector,
    name: &'static str,
    kind: i32,
    follow: Option<u32>,
}

struct Mark {
    owner: i32,
    team: i32,
    target: Target,
    life: u32,
    keep: i32,
    speech: &'static str,
}

impl Mark {
    fn encode(&self, for_user: i32, time: f32) -> String {
        let mine = for_user == self.owner;
        let p = self.target.at;
        let speech = if mine { self.speech } else { "" };
        format!(
            "@png@ {} {} {} {} {} {:.1} {:.1} {:.1}|{}|{}|{}\n",
            self.owner,
            u8::from(mine),
            self.target.kind,
            self.life,
            self.keep,
            p.x,
            p.y,
            p.z,
            self.target.name,
            speech,
            (time * 10.0) as i32
        )
    }
}

impl Pings {
    pub fn wall_ahead(&self, game: &Game, player: usize) -> bool {
        let Some(trace) = &self.trace else {
            return false;
        };
        let Some((eye, forward)) = game.eyes(player) else {
            return false;
        };
        let pitch = forward.z.clamp(-0.34202014, 0.34202014);
        let flat = forward.x.hypot(forward.y);
        if flat < 1e-3 {
            return false;
        }
        let horizontal = (1.0 - pitch * pitch).sqrt() / flat;
        let direction = Vector {
            x: forward.x * horizontal,
            y: forward.y * horizontal,
            z: pitch,
        };
        let hit = trace.ray(eye, direction, 72.0, player, true);
        hit.fraction < 1.0 && !hit.start_solid && hit.normal.z.abs() < 0.6
    }
    pub fn bind() -> Self {
        Self {
            trace: Trace::bind(),
            cooldown: HashMap::new(),
            follows: HashMap::new(),
            highlights: HashMap::new(),
            next: None,
        }
    }

    pub fn reset(&mut self, game: &Game, hooks: Option<&Hooks>) {
        if let Some(hooks) = hooks {
            for (_, follow) in std::mem::take(&mut self.follows) {
                if follow.highlight.is_some() {
                    self.unlight(game, hooks, follow.handle);
                }
            }
        }
        self.highlights.clear();
        self.follows.clear();
        self.cooldown.clear();
        self.next = None;
    }

    fn light(&mut self, game: &Game, hooks: &Hooks, handle: u32) -> bool {
        if let Some(existing) = self.highlights.get_mut(&handle) {
            existing.count += 1;
            return true;
        }
        let Some(entity) = game.resolve(handle) else {
            return false;
        };
        let before = std::array::from_fn(|index| {
            native::word(entity + GLOW + 4 * (index + 1)).unwrap_or_default()
        });
        let mut flash = [0];
        native::read(entity + GLOW + 20, &mut flash);
        self.highlights.insert(
            handle,
            Highlight {
                before,
                flashing: flash[0],
                count: 1,
            },
        );
        for (index, value) in [3, 0, 0, GLOW_COLOR].into_iter().enumerate() {
            native::store_word(entity + GLOW + 4 * (index + 1), value);
        }
        set_flash(entity, 0);
        hooks.changed(entity);
        true
    }

    fn unlight(&mut self, game: &Game, hooks: &Hooks, handle: u32) {
        let Some(active) = self.highlights.get_mut(&handle) else {
            return;
        };
        active.count -= 1;
        if active.count != 0 {
            return;
        }
        let Some(active) = self.highlights.remove(&handle) else {
            return;
        };
        if let Some(entity) = game.resolve(handle)
            && native::word(entity + GLOW + 4) == Some(3)
            && native::word(entity + GLOW + 16) == Some(GLOW_COLOR)
        {
            for (index, value) in active.before.into_iter().enumerate() {
                native::store_word(entity + GLOW + 4 * (index + 1), value);
            }
            set_flash(entity, active.flashing);
            hooks.changed(entity);
        }
    }

    fn mark(
        &mut self,
        game: &Game,
        hooks: &Hooks,
        rules: &PingRules,
        catalog: &Catalog,
        player: usize,
    ) -> Option<Mark> {
        let team = native::word(player + PLAYER_TEAM)? as i32;
        if !(2..=3).contains(&team) || !alive(player) {
            return None;
        }
        let edict = native::word(player + 0x28)? as usize;
        let owner = hooks.user(edict)?;
        let now = Instant::now();
        if self
            .cooldown
            .get(&owner)
            .is_some_and(|&then| now - then < Duration::from_secs_f32(rules.cooldown))
        {
            return None;
        }
        let (eye, forward) = game.eyes(player)?;
        let hit = self
            .trace
            .as_ref()?
            .ray(eye, forward, rules.range, player, false);
        if hit.start_solid {
            return None;
        }
        let nearest = || {
            let reach = rules.range * hit.fraction.clamp(0.0, 1.0) + 8.0;
            (1..4096)
                .map(|index| game.entity(index))
                .filter(|&entity| entity != 0 && entity != player)
                .filter_map(|entity| {
                    let at = Vector::read(entity + ENTITY_ORIGIN)?;
                    let rel = at.difference(eye);
                    let along = rel.dot(forward);
                    if !(1.0..=reach).contains(&along) {
                        return None;
                    }
                    let lateral = rel.dot(rel) - along * along;
                    let slack = 30.0 + (along * 0.025).min(26.0);
                    if lateral > slack * slack {
                        return None;
                    }
                    let candidate = target(game, catalog, entity, team)?;
                    let fit = if candidate.kind == 2 {
                        slack
                    } else {
                        slack - 12.0
                    };
                    (lateral <= fit * fit).then_some((candidate, lateral + along * 0.1))
                })
                .max_by(|(a, a_distance), (b, b_distance)| {
                    a.kind
                        .cmp(&b.kind)
                        .then_with(|| b_distance.total_cmp(a_distance))
                })
                .map(|(candidate, _)| candidate)
        };
        let direct = (hit.entity != 0 && hit.entity != player)
            .then(|| target(game, catalog, hit.entity, team))
            .flatten();
        let target = direct.or_else(nearest).or_else(|| {
            (hit.fraction < 1.0 && !hit.sky).then_some(Target {
                at: Vector {
                    x: hit.end.x + hit.normal.x * 6.0,
                    y: hit.end.y + hit.normal.y * 6.0,
                    z: hit.end.z + hit.normal.z * 6.0,
                },
                name: "",
                kind: 0,
                follow: None,
            })
        })?;
        self.cooldown.insert(owner, now);
        if let Some(old) = self.follows.remove(&owner)
            && old.highlight.is_some()
        {
            self.unlight(game, hooks, old.handle);
        }
        let lifetime = Duration::from_secs_f32(rules.lifetime);
        if let Some(handle) = target.follow {
            let highlight = self
                .light(game, hooks, handle)
                .then(|| now + lifetime.min(Duration::from_secs(5)));
            self.follows.insert(
                owner,
                Follow {
                    handle,
                    team,
                    until: now + lifetime,
                    highlight,
                },
            );
        }
        let speech = if team == 2 && target.kind != 0 {
            if target.name == "Ammo" {
                "PlayerSpotAmmo"
            } else {
                "smartlook"
            }
        } else {
            ""
        };
        Some(Mark {
            owner,
            team,
            target,
            life: lifetime.as_millis() as u32,
            keep: rules.per_player,
            speech,
        })
    }

    fn advance(&mut self, game: &Game, hooks: &Hooks) -> Vec<Mark> {
        let now = Instant::now();
        if self.follows.is_empty() || self.next.is_some_and(|next| now < next) {
            return Vec::new();
        }
        self.next = Some(now + Duration::from_millis(100));
        let mut marks = Vec::new();
        for (owner, mut follow) in std::mem::take(&mut self.follows) {
            let position = game
                .resolve(follow.handle)
                .filter(|&actor| now < follow.until && alive(actor))
                .and_then(|actor| Vector::read(actor + ENTITY_ORIGIN));
            if follow
                .highlight
                .is_some_and(|at| now >= at || position.is_none())
            {
                self.unlight(game, hooks, follow.handle);
                follow.highlight = None;
            }
            if let Some(at) = position {
                let target = Target {
                    at: Vector {
                        z: at.z + 40.0,
                        ..at
                    },
                    name: "",
                    kind: 3,
                    follow: None,
                };
                marks.push(Mark {
                    owner,
                    team: follow.team,
                    target,
                    life: 1000,
                    keep: 1,
                    speech: "",
                });
                self.follows.insert(owner, follow);
            }
        }
        marks
    }
}

fn alive(player: usize) -> bool {
    let mut state = [1];
    native::read(player + super::LIFE_STATE, &mut state) && state[0] == 0
}

fn set_flash(entity: usize, value: u8) {
    let addr = entity + GLOW + 20;
    if let Some(previous) = native::word(addr) {
        native::store_word(addr, (previous & !255) | u32::from(value));
    }
}

fn target(game: &Game, catalog: &Catalog, entity: usize, team: i32) -> Option<Target> {
    let name = game.classname(entity)?;
    let at = Vector::read(entity + ENTITY_ORIGIN)?;
    let item = if name == "weapon_ammo_spawn" {
        Some("Ammo")
    } else if name.starts_with("weapon_") {
        let candidate = name.strip_suffix("_spawn").unwrap_or(&name);
        let eligible = name.ends_with("_spawn")
            || native::word(entity + super::WEAPON_OWNER) == Some(u32::MAX);
        if eligible {
            catalog.find_item(candidate).map(|id| catalog.item(id).name)
        } else {
            None
        }
    } else {
        None
    };
    if let Some(name) = item {
        return Some(Target {
            at: Vector {
                z: at.z + 10.0,
                ..at
            },
            name,
            kind: 1,
            follow: None,
        });
    }
    let name = match name.as_str() {
        "infected" if team == 2 => "Common",
        "witch" if team == 2 => "Witch",
        "player" if native::word(entity + PLAYER_TEAM) == Some(if team == 2 { 3 } else { 2 }) => {
            if team == 3 {
                "Survivor"
            } else {
                let classes = [
                    "Special infected",
                    "Smoker",
                    "Boomer",
                    "Hunter",
                    "Spitter",
                    "Jockey",
                    "Charger",
                    "Witch",
                    "Tank",
                ];
                classes[native::word(entity + 0x2bb8).unwrap_or_default().min(8) as usize]
            }
        }
        _ => return None,
    };
    alive(entity).then(|| Target {
        at: Vector {
            z: at.z + 40.0,
            ..at
        },
        name,
        kind: 2,
        follow: handle(game, entity),
    })
}

impl Host {
    pub fn ping(&mut self, player: usize) -> String {
        if !self.rules.ping.enabled {
            return "Pings are off on this server".into();
        }
        let Some(hooks) = &self.hooks else {
            return String::new();
        };
        if let Some(mark) = self.pings.mark(
            &self.game,
            hooks,
            &self.rules.ping,
            &self.rules.catalog,
            player,
        ) {
            self.broadcast_ping(mark);
        }
        String::new()
    }

    pub fn ping_frame(&mut self) {
        if let Some(hooks) = &self.hooks {
            for mark in self.pings.advance(&self.game, hooks) {
                self.broadcast_ping(mark);
            }
        }
    }

    fn broadcast_ping(&mut self, mark: Mark) {
        let Some(hooks) = &self.hooks else {
            return;
        };
        let now = hooks.time().unwrap_or_default();
        if !self.dedicated
            && let Some(local) = self.game.host()
            && native::word(local + PLAYER_TEAM) == Some(mark.team as u32)
            && let Some(user) =
                native::word(local + 0x28).and_then(|edict| hooks.user(edict as usize))
        {
            self.hud.push(mark.encode(user, now));
        }
        for (&user, guest) in &self.guests {
            if hooks.user(guest.edict) == Some(user)
                && native::word(guest.player + PLAYER_TEAM) == Some(mark.team as u32)
                && let Ok(line) = CString::new(mark.encode(user, now))
            {
                hooks.print(guest.edict, &line);
            }
        }
    }
}
