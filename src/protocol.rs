use crate::{items::Catalog, mutators::Mutator, remote_state::RemoteState};

#[derive(Clone, Debug, PartialEq)]
pub enum ServerMessage {
    Inventory(Box<RemoteState>),
    Stamina {
        tenths: Option<u16>,
        exhausted: bool,
    },
    Infection {
        remaining_ms: u32,
        total_ms: u32,
        turned: bool,
    },
    Melee {
        hold_ms: u32,
        shove_hold_ms: u32,
        overhead: bool,
        throwing: bool,
    },
    MeleeHit {
        heavy: bool,
    },
    Ping(Ping),
    Mutators {
        generation: u32,
        announce: bool,
        active: Vec<ActiveMutator>,
    },
}

#[derive(Clone, Debug, PartialEq)]
pub struct Ping {
    pub owner: i32,
    pub mine: bool,
    pub kind: PingKind,
    pub lifetime_ms: u32,
    pub per_player: u32,
    pub position: [f32; 3],
    pub name: String,
    pub speech: String,
    pub server_tenths: Option<i32>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PingKind {
    Spot,
    Item,
    Enemy,
    Follow,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ActiveMutator {
    pub mutator: Mutator,
    pub fog_density: Option<f32>,
    pub fog_color: Option<[u8; 3]>,
    pub hunted_class: Option<u8>,
}

impl ServerMessage {
    pub fn decode(text: &str, catalog: &Catalog) -> Option<Self> {
        let line = text.trim_end_matches(['\n', '\r']);
        let (command, body) = line.split_once(' ')?;
        let mut fields = body.split_ascii_whitespace();
        match command {
            "@inv@" => RemoteState::decode(line, catalog)
                .map(Box::new)
                .map(Self::Inventory),
            "@sta@" => {
                let raw = fields.next()?.parse::<i32>().ok()?;
                let exhausted = fields.next()?.parse::<i32>().ok()? != 0;
                Some(Self::Stamina {
                    tenths: (raw >= 0).then(|| raw.min(1000) as u16),
                    exhausted,
                })
            }
            "@inf@" => {
                let remaining_ms = fields.next()?.parse().ok()?;
                let total_ms = fields.next()?.parse().ok()?;
                let turned = fields.next()?.parse::<i32>().ok()? != 0;
                Some(Self::Infection {
                    remaining_ms,
                    total_ms,
                    turned,
                })
            }
            "@mel@" => {
                let hold_ms = fields.next()?.parse().ok()?;
                let shove_hold_ms = fields.next()?.parse().ok()?;
                let flags = fields
                    .next()
                    .and_then(|text| text.parse::<u32>().ok())
                    .unwrap_or(0);
                Some(Self::Melee {
                    hold_ms,
                    shove_hold_ms,
                    overhead: flags & 1 != 0,
                    throwing: flags & 2 != 0,
                })
            }
            "@mhi@" => Some(Self::MeleeHit {
                heavy: fields.next()?.parse::<i32>().ok()? != 0,
            }),
            "@png@" => parse_ping(body).map(Self::Ping),
            "@mut@" => parse_mutators(body),
            _ => None,
        }
    }
}

fn parse_ping(text: &str) -> Option<Ping> {
    let mut parts = text.splitn(4, '|');
    let mut fields = parts.next()?.split_ascii_whitespace();
    let owner = fields.next()?.parse().ok()?;
    let mine = fields.next()?.parse::<i32>().ok()? != 0;
    let kind = match fields.next()? {
        "0" => PingKind::Spot,
        "1" => PingKind::Item,
        "2" => PingKind::Enemy,
        "3" => PingKind::Follow,
        _ => return None,
    };
    let lifetime_ms = fields.next()?.parse().ok()?;
    let per_player = fields.next()?.parse::<u32>().ok()?.clamp(1, 8);
    let mut axis = || fields.next()?.parse::<f32>().ok().filter(|n| n.is_finite());
    let position = [axis()?, axis()?, axis()?];
    Some(Ping {
        owner,
        mine,
        kind,
        lifetime_ms,
        per_player,
        position,
        name: parts.next().unwrap_or("").to_owned(),
        speech: parts.next().unwrap_or("").to_owned(),
        server_tenths: parts.next().and_then(|value| value.parse().ok()),
    })
}

fn parse_mutators(text: &str) -> Option<ServerMessage> {
    let (header, entries) = text.split_once('|')?;
    let mut fields = header.split_ascii_whitespace();
    let generation = fields.next()?.parse().ok()?;
    let announce = fields.next()?.parse::<i32>().ok()? != 0;
    let active = entries
        .split(';')
        .filter_map(|entry| {
            let mut fields = entry.split_ascii_whitespace();
            let mutator = Mutator::from_id(fields.next()?)?;
            let mut details = ActiveMutator {
                mutator,
                fog_density: None,
                fog_color: None,
                hunted_class: None,
            };
            if mutator == Mutator::Fog {
                details.fog_density = fields
                    .next()
                    .and_then(|value| value.parse::<u32>().ok())
                    .map(|value| value.min(1000) as f32 / 1000.0);
                let mut channel = || fields.next()?.parse::<u8>().ok();
                if let (Some(r), Some(g), Some(b)) = (channel(), channel(), channel()) {
                    details.fog_color = Some([r, g, b]);
                }
            } else if mutator == Mutator::Hunted {
                details.hunted_class = fields
                    .next()
                    .and_then(|value| value.parse::<u8>().ok())
                    .filter(|value| (1..=6).contains(value));
            }
            Some(details)
        })
        .collect();
    Some(ServerMessage::Mutators {
        generation,
        announce,
        active,
    })
}

/// Inventory Mod commands handled by both server implementations.
pub fn is_inventory_command(name: &str) -> bool {
    matches!(
        name,
        "inv_sync"
            | "inv_move"
            | "inv_select"
            | "inv_pickup"
            | "inv_store"
            | "inv_use"
            | "inv_drop"
            | "inv_dropweapon"
            | "inv_unload"
            | "inv_dropammo"
            | "inv_ping"
            | "inv_infect"
            | "inv_spawn"
    )
}
