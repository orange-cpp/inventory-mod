use std::{
    collections::HashMap,
    ffi::c_void,
    ptr::NonNull,
    time::{Duration, Instant},
};

use inventory_mod::{items::Catalog, rules::PingRules};

use super::{Ent, Game, Vector, field, native::LIFE_STATE, set_field};
use crate::source::{Edict, Source};

const MASK_SHOT: u32 = 0x46004003;
const GLOW_FIELD: usize = 0x1f0;
const HIGHLIGHT: [i32; 3] = [3, 0, 0];
const HIGHLIGHT_COLOR: u32 = 235 | (70 << 8) | (60 << 16);

#[derive(Default)]
pub struct Pings {
    follows: HashMap<i32, Follow>,
    glow: HashMap<u32, ActiveGlow>,
    next_update: Option<Instant>,
}

struct Follow {
    handle: u32,
    team: i32,
    end: Instant,
    glow_end: Option<Instant>,
}

struct ActiveGlow {
    settings: Glow,
    users: usize,
}

#[derive(Clone, Copy)]
struct Glow {
    values: [i32; 3],
    color: u32,
    flashing: u8,
}

struct Target {
    position: Vector,
    kind: i32,
    name: &'static str,
    follow: Option<u32>,
}

pub struct Marker {
    owner: i32,
    team: i32,
    target: Target,
    lifetime_ms: u32,
    per_player: i32,
    speech: &'static str,
}

impl Marker {
    pub fn team(&self) -> i32 {
        self.team
    }

    pub fn encode(&self, recipient: i32, now: f32) -> String {
        let p = self.target.position;
        let mine = recipient == self.owner;
        let speech = if mine { self.speech } else { "" };
        let tenths = (now * 10.0) as i32;
        format!(
            "@png@ {} {} {} {} {} {:.1} {:.1} {:.1}|{}|{}|{tenths}\n",
            self.owner,
            u8::from(mine),
            self.target.kind,
            self.lifetime_ms,
            self.per_player,
            p.x,
            p.y,
            p.z,
            self.target.name,
            speech
        )
    }
}

pub struct PingContext<'a> {
    pub source: &'a Source,
    pub catalog: &'a Catalog,
    pub rules: &'a PingRules,
    pub edict: NonNull<Edict>,
    pub owner: i32,
    pub team: i32,
    pub now: Instant,
}

impl Pings {
    pub fn mark(&mut self, game: &Game, ctx: PingContext<'_>) -> Option<Marker> {
        let target = game.ping_target(
            ctx.source,
            ctx.catalog,
            ctx.edict,
            ctx.team,
            ctx.rules.range,
        )?;
        let lifetime = Duration::from_secs_f32(ctx.rules.lifetime);
        let speech = match (ctx.team, target.kind, target.name) {
            (2, 1 | 2, "Ammo") => "PlayerSpotAmmo",
            (2, 1 | 2, _) => "smartlook",
            _ => "",
        };
        self.remove(game, ctx.owner);
        if let Some(handle) = target.follow {
            let glow_end = self
                .light(game, handle)
                .then(|| ctx.now + lifetime.min(Duration::from_secs(5)));
            self.follows.insert(
                ctx.owner,
                Follow {
                    handle,
                    team: ctx.team,
                    end: ctx.now + lifetime,
                    glow_end,
                },
            );
        }
        Some(Marker {
            owner: ctx.owner,
            team: ctx.team,
            target,
            lifetime_ms: lifetime.as_millis() as u32,
            per_player: ctx.rules.per_player,
            speech,
        })
    }

    pub fn advance(&mut self, game: &Game, now: Instant) -> Vec<Marker> {
        if self.follows.is_empty() || self.next_update.is_some_and(|at| now < at) {
            return Vec::new();
        }
        self.next_update = Some(now + Duration::from_millis(100));
        let mut updates = Vec::new();
        for (owner, mut follow) in std::mem::take(&mut self.follows) {
            let position = game
                .follow_target(follow.handle)
                .filter(|_| now < follow.end);
            if follow
                .glow_end
                .is_some_and(|at| position.is_none() || now >= at)
            {
                self.unlight(game, follow.handle);
                follow.glow_end = None;
            }
            if let Some(position) = position {
                let team = follow.team;
                self.follows.insert(owner, follow);
                updates.push(Marker {
                    owner,
                    team,
                    target: Target {
                        position,
                        name: "",
                        kind: 3,
                        follow: None,
                    },
                    lifetime_ms: 1000,
                    per_player: 1,
                    speech: "",
                });
            }
        }
        updates
    }

    pub fn remove(&mut self, game: &Game, owner: i32) {
        if let Some(follow) = self.follows.remove(&owner)
            && follow.glow_end.is_some()
        {
            self.unlight(game, follow.handle);
        }
    }

    pub fn clear(&mut self, game: &Game, restore: bool) {
        if restore {
            for (_, follow) in std::mem::take(&mut self.follows) {
                if follow.glow_end.is_some() {
                    self.unlight(game, follow.handle);
                }
            }
        }
        *self = Self::default();
    }

    fn light(&mut self, game: &Game, handle: u32) -> bool {
        if let Some(active) = self.glow.get_mut(&handle) {
            active.users += 1;
            return true;
        }
        let Some(target) = game.find_entity(handle) else {
            return false;
        };
        let original = game.glow(target);
        game.set_glow(
            target,
            Glow {
                values: HIGHLIGHT,
                color: HIGHLIGHT_COLOR,
                flashing: 0,
            },
        );
        self.glow.insert(
            handle,
            ActiveGlow {
                settings: original,
                users: 1,
            },
        );
        true
    }

    fn unlight(&mut self, game: &Game, handle: u32) {
        let Some(active) = self.glow.get_mut(&handle) else {
            return;
        };
        active.users -= 1;
        if active.users != 0 {
            return;
        }
        let original = active.settings;
        self.glow.remove(&handle);
        if let Some(target) = game.find_entity(handle) {
            let current = game.glow(target);
            if current.values[0] == HIGHLIGHT[0] && current.color == HIGHLIGHT_COLOR {
                game.set_glow(target, original);
            }
        }
    }
}

impl Game {
    fn ping_target(
        &self,
        source: &Source,
        catalog: &Catalog,
        edict: NonNull<Edict>,
        team: i32,
        range: f32,
    ) -> Option<Target> {
        let pinger = source.entity(edict)?;
        let (eye, forward) = self.eyes(pinger);
        let delta = Vector {
            x: forward.x * range,
            y: forward.y * range,
            z: forward.z * range,
        };
        let hit = source.trace(eye, delta, Some(edict), MASK_SHOT)?;
        if hit.start_solid {
            return None;
        }
        if let Some(entity) = hit.entity.filter(|&entity| entity != pinger)
            && let Some(mark) = self.mark_entity(source, catalog, entity, team)
        {
            return Some(mark);
        }
        let reach = range * hit.fraction.clamp(0.0, 1.0) + 8.0;
        let near = source
            .entities()
            .filter(|&entity| entity != pinger)
            .filter_map(|entity| {
                let mark = self.mark_entity(source, catalog, entity, team)?;
                let rel = mark.position.offset(-eye.x, -eye.y, -eye.z);
                let along = rel.dot(forward);
                if !(1.0..=reach).contains(&along) {
                    return None;
                }
                let slack = if mark.kind == 2 { 30.0 } else { 18.0 } + (along * 0.025).min(26.0);
                let sideways = rel.dot(rel) - along * along;
                (sideways <= slack * slack).then_some((mark, sideways + along * 0.1))
            })
            .max_by(|(a, a_score), (b, b_score)| {
                a.kind.cmp(&b.kind).then_with(|| b_score.total_cmp(a_score))
            });
        if let Some((target, _)) = near {
            return Some(target);
        }
        if hit.fraction >= 1.0 || hit.sky {
            return None;
        }
        Some(Target {
            position: hit
                .end
                .offset(6.0 * hit.normal.x, 6.0 * hit.normal.y, 6.0 * hit.normal.z),
            kind: 0,
            name: "",
            follow: None,
        })
    }

    fn mark_entity(
        &self,
        source: &Source,
        catalog: &Catalog,
        target: Ent,
        team: i32,
    ) -> Option<Target> {
        let classname = self.classname(target);
        let item = match classname {
            "weapon_ammo_spawn" => Some("Ammo"),
            name if name.starts_with("weapon_") => self
                .weapon_candidate(catalog, target)
                .filter(|(_, spawner)| {
                    *spawner || unsafe { (self.native.owner)(target.as_ptr()) }.is_null()
                })
                .map(|(def, _)| def.name),
            _ => None,
        };
        if let Some(name) = item {
            return Some(Target {
                position: self.position(target)?.offset(0.0, 0.0, 10.0),
                kind: 1,
                name,
                follow: None,
            });
        }

        let name = match classname {
            "infected" if team == 2 => "Common",
            "witch" if team == 2 => "Witch",
            "player" => {
                let other = source
                    .as_edict(target)
                    .and_then(|edict| source.player_info(edict))?;
                if other.dead || other.team != if team == 2 { 3 } else { 2 } {
                    return None;
                }
                if team == 3 {
                    "Survivor"
                } else {
                    let kinds = [
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
                    let kind = unsafe { (self.native.zombie_class)(target.as_ptr()) };
                    kinds[kind.clamp(0, 8) as usize]
                }
            }
            _ => return None,
        };
        let follow = self.handle(target);
        (unsafe { field::<u8>(target, LIFE_STATE) } == 0).then_some(Target {
            position: self.position(target)?.offset(0.0, 0.0, 40.0),
            kind: 2,
            name,
            follow: Some(follow),
        })
    }

    fn follow_target(&self, handle: u32) -> Option<Vector> {
        let entity = self.find_entity(handle)?;
        if unsafe { field::<u8>(entity, LIFE_STATE) } != 0 {
            return None;
        }
        Some(self.position(entity)?.offset(0.0, 0.0, 40.0))
    }

    fn glow(&self, entity: Ent) -> Glow {
        unsafe {
            Glow {
                values: [
                    field(entity, GLOW_FIELD + 4),
                    field(entity, GLOW_FIELD + 8),
                    field(entity, GLOW_FIELD + 12),
                ],
                color: field(entity, GLOW_FIELD + 16),
                flashing: field(entity, GLOW_FIELD + 20),
            }
        }
    }

    fn set_glow(&self, entity: Ent, glow: Glow) {
        unsafe {
            let property = entity
                .cast::<u8>()
                .as_ptr()
                .add(GLOW_FIELD)
                .cast::<c_void>();
            (self.native.set_glow_state)(property, glow.values[0]);
            (self.native.set_glow_range)(property, glow.values[1]);
            (self.native.set_glow_minimum)(property, glow.values[2]);
            (self.native.set_glow_color)(property, glow.color);
            set_field(entity, GLOW_FIELD + 20, glow.flashing);
            (self.native.network_changed)(entity.as_ptr());
        }
    }
}
