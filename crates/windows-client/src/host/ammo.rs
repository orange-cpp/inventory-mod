use std::{
    collections::HashMap,
    ffi::{CStr, CString, c_void},
    io, ptr,
    sync::atomic::{AtomicUsize, Ordering},
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use inventory_mod::{
    inventory::Inventory,
    items::{AmmoKind, Catalog},
    remote_state::reconcile_native_inventory,
    rules::AmmoPileRules,
};

use super::{AMMO_TYPE, ENTITY_ORIGIN, Game, Host, RESERVE_POOL, Vector, address, hooks::Hooks};
use crate::{
    addons,
    client::with_client,
    dedicated,
    native::{self, VtablePatch},
};

type Use = unsafe extern "thiscall" fn(*mut c_void, *mut c_void, *mut c_void, i32, f32);
type Floor = unsafe extern "C" fn(*mut c_void, u32, *mut c_void) -> i32;

static ORIGINAL: AtomicUsize = AtomicUsize::new(0);

pub struct Ammo {
    slot: VtablePatch,
    piles: HashMap<u32, Pile>,
    random: u32,
    place: Option<Instant>,
    floor: Option<Floor>,
}

#[derive(Clone, Copy)]
struct Pile {
    kind: AmmoKind,
    rounds: i32,
    position: Vector,
    map: bool,
}

impl Ammo {
    pub fn install(game: &Game) -> Option<Self> {
        let (table, use_rva) = if game.image.timestamp == 0x6afc_cca0 {
            (0x682d4c, 0x29f6e0)
        } else {
            (0x682cf4, 0x29f5f0)
        };
        let base = game.image.base;
        let function = base + use_rva;
        if !native::matches_pattern(function, "55 8B EC 51 53 8B 5D 08 85 DB 0F 84") {
            return None;
        }
        let slot = VtablePatch::install(base + table + 4 * 106, function, use_pile as Use as usize)
            .ok()?;
        ORIGINAL.store(function, Ordering::Release);
        let random = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|n| n.as_nanos() as u32)
            .unwrap_or(1)
            .max(1);
        let floor = native::find_pattern(
            base + 0x57500,
            0x500,
            "6A 00 68 0B 40 00 02 57 E8 ?? ?? ?? ?? 83 C4 0C",
        )
        .and_then(|call| super::relative(call + 8))
        .filter(|&at| game.image.includes(at, 1) && native::matches_pattern(at, "55"))
        .map(|at| unsafe { std::mem::transmute::<usize, Floor>(at) });
        Some(Self {
            slot,
            piles: HashMap::new(),
            random,
            place: None,
            floor,
        })
    }

    pub fn stop(&mut self) -> io::Result<()> {
        self.slot.restore()?;
        ORIGINAL.store(0, Ordering::Release);
        Ok(())
    }

    pub fn reset(&mut self, rules: &AmmoPileRules) {
        self.piles.clear();
        self.place = rules
            .enabled
            .then(|| Instant::now() + Duration::from_millis(1500));
    }

    fn random(&mut self) -> u32 {
        self.random ^= self.random << 13;
        self.random ^= self.random >> 17;
        self.random ^= self.random << 5;
        self.random
    }

    fn roll(&mut self, rules: &AmmoPileRules, at: Vector) -> Option<Pile> {
        if rules.kinds.is_empty() {
            return None;
        }
        let kind = rules.kinds[self.random() as usize % rules.kinds.len()];
        let span = (rules.max_magazines - rules.min_magazines + 1) as u32;
        let packs = rules.min_magazines + (self.random() % span) as i32;
        Some(Pile {
            kind,
            rounds: packs * kind.definition().magazine,
            position: at,
            map: true,
        })
    }

    pub(super) fn ground(&self, game: &Game, position: Vector) -> Vector {
        let Some(floor) = self.floor else {
            return position;
        };
        let probe = unsafe { (game.create)(address(game.tools), c"info_target".as_ptr()) }.addr();
        if probe == 0 {
            return position;
        }
        key(game, probe, c"origin", &position_string(position));
        if !game.dispatch_spawn(probe) {
            return position;
        }
        let landed = unsafe { floor(address(probe), 0x0200_400b, ptr::null_mut()) };
        let at = if landed == 1 {
            Vector::read(probe + ENTITY_ORIGIN).unwrap_or(position)
        } else {
            position
        };
        game.remove(probe);
        at
    }

    fn spawn(&mut self, game: &Game, hooks: &Hooks, mut pile: Pile) -> bool {
        if pile.rounds <= 0 {
            return false;
        }
        self.piles
            .retain(|&handle, _| game.resolve(handle).is_some());
        let position = pile.position;
        let close = |a: Vector, b: Vector, distance: f32| {
            let delta = a.difference(b);
            delta.z.abs() < 72.0 && delta.x * delta.x + delta.y * delta.y < distance * distance
        };
        self.piles
            .retain(|&handle, _| game.resolve(handle).is_some());
        if !pile.map
            && let Some(close_pack) = self.piles.values_mut().find(|existing| {
                !existing.map
                    && existing.kind == pile.kind
                    && close(existing.position, position, 40.0)
                    && existing.rounds.checked_add(pile.rounds).is_some()
            })
        {
            close_pack.rounds += pile.rounds;
            return true;
        }
        let Some(at) = (0..=8).find_map(|step| {
            let sign = if step % 2 == 0 { -1.0 } else { 1.0 };
            let position = Vector {
                x: position.x + sign * 26.0 * ((step + 1) / 2) as f32,
                ..position
            };
            self.piles
                .values()
                .all(|other| !close(other.position, position, 24.0))
                .then_some(position)
        }) else {
            return false;
        };
        let at = self.ground(game, at);
        let custom =
            CString::new(format!("models/inv_mod/rounds_{}.mdl", pile.kind.name())).unwrap();
        let fallback = c"models/w_models/weapons/w_eq_incendiary_ammopack.mdl";
        let own_pack = addons::exists(&custom) && hooks.precache(&custom);
        let model = if own_pack {
            custom.as_c_str()
        } else if hooks.precache(fallback) {
            fallback
        } else {
            let stock = c"models/props/terror/ammo_stack.mdl";
            if !hooks.precache(stock) {
                return false;
            }
            stock
        };
        let probe =
            unsafe { (game.create)(address(game.tools), c"weapon_ammo_spawn".as_ptr()) }.addr();
        if probe == 0 {
            return false;
        }
        key(game, probe, c"model", model);
        tint(
            game,
            probe,
            if own_pack {
                0xff_ffff
            } else {
                pile.kind.definition().color
            },
        );
        key(game, probe, c"solid", c"6");
        key(game, probe, c"spawnflags", c"2");
        key(game, probe, c"disableshadows", c"1");
        // Ammo piles are static physics entities. Set their origin before spawning.
        key(
            game,
            probe,
            c"origin",
            &position_string(Vector {
                z: at.z + 0.3,
                ..at
            }),
        );
        if !game.dispatch_spawn(probe) {
            return false;
        }
        let Some(handle) = handle(game, probe) else {
            return false;
        };
        pile.position = at;
        self.piles.insert(handle, pile);
        true
    }

    pub fn drop_at(
        &mut self,
        game: &Game,
        hooks: &Hooks,
        position: Vector,
        kind: AmmoKind,
        rounds: i32,
    ) -> bool {
        self.spawn(
            game,
            hooks,
            Pile {
                kind,
                rounds,
                position,
                map: false,
            },
        )
    }

    pub fn place_due(&mut self, game: &Game, hooks: &Hooks, rules: &AmmoPileRules) {
        if self.place.is_none_or(|due| Instant::now() < due) {
            return;
        }
        self.place = None;
        let mut candidates = Vec::new();
        let mut starts = Vec::new();
        let mut ammo_spots = Vec::new();
        for entity in (1..4096)
            .map(|index| game.entity(index))
            .filter(|&entity| entity != 0)
        {
            let Some(at) = Vector::read(entity + ENTITY_ORIGIN) else {
                continue;
            };
            let Some(name) = game.classname(entity) else {
                continue;
            };
            match name.as_str() {
                "weapon_ammo_spawn" => {
                    ammo_spots.push(at);
                    if rules.replace_map_piles
                        && let (Some(stock_handle), Some(pile)) =
                            (handle(game, entity), self.roll(rules, at))
                        && !self.piles.contains_key(&stock_handle)
                    {
                        // Keep the stock pile if its replacement cannot be made.
                        if self.spawn(game, hooks, pile) {
                            game.remove(entity);
                        } else {
                            self.piles.insert(stock_handle, pile);
                            tint(game, entity, pile.kind.definition().color);
                        }
                    }
                }
                "player" if game.living(entity) => starts.push(at),
                name if name.starts_with("weapon_") && name.ends_with("_spawn") => {
                    candidates.push(at)
                }
                _ => {}
            }
        }
        for i in (1..candidates.len()).rev() {
            candidates.swap(i, self.random() as usize % (i + 1));
        }
        let mut extra = Vec::new();
        for at in candidates {
            if extra.len() >= rules.extra.max(0) as usize {
                break;
            }
            let angle = self.random() as f32 / u32::MAX as f32 * std::f32::consts::TAU;
            let position = Vector {
                x: at.x + 20.0 * angle.cos(),
                y: at.y + 20.0 * angle.sin(),
                z: at.z + 8.0,
            };
            let near = |points: &[Vector], threshold: f32| {
                points.iter().any(|&point| {
                    let delta = point.difference(position);
                    delta.dot(delta) < threshold * threshold
                })
            };
            if near(&starts, 700.0) || near(&ammo_spots, 150.0) || near(&extra, 300.0) {
                continue;
            }
            if let Some(pile) = self.roll(rules, position)
                && self.spawn(game, hooks, pile)
            {
                extra.push(position);
            }
        }
    }

    fn take(&mut self, game: &Game, rules: &AmmoPileRules, context: Take<'_>) -> bool {
        let Take {
            pile,
            player,
            catalog,
            inventory,
            bot,
        } = context;
        let Some(handle) = handle(game, pile) else {
            return false;
        };
        if !self.piles.contains_key(&handle) {
            if !rules.enabled {
                return false;
            }
            let Some(at) = Vector::read(pile + ENTITY_ORIGIN) else {
                return false;
            };
            if let Some(new) = self.roll(rules, at) {
                tint(game, pile, new.kind.definition().color);
                self.piles.insert(handle, new);
            }
        }
        let Some(stock) = self.piles.get(&handle).copied() else {
            return false;
        };
        if !game.living(player) {
            return true;
        }
        let index = stock.kind.definition().index;
        if bot
            && (!stock.map
                || !game
                    .weapons(player, catalog)
                    .iter()
                    .take(2)
                    .flatten()
                    .any(|(weapon, _)| native::word(weapon + AMMO_TYPE) == Some(index as u32)))
        {
            return true;
        }

        let ammo = game.ammo(player);
        let room = inventory.and_then(|inventory| {
            reconcile_native_inventory(
                inventory,
                &game.native(player, catalog),
                catalog.ammo_weight(&ammo),
            );
            let limit = inventory.limits().max_weight;
            (limit > 0.0).then(|| (limit - inventory.weight()).max(0.0))
        });
        let before = ammo[index];
        let n = catalog
            .split_unload(Some(stock.kind), stock.rounds, before, room)
            .to_reserve;
        if n > 0 && native::store_word(player + RESERVE_POOL + 4 * index, (before + n) as u32) {
            if stock.rounds == n {
                self.piles.remove(&handle);
                game.remove(pile);
            } else if let Some(remaining) = self.piles.get_mut(&handle) {
                remaining.rounds -= n;
            }
        }
        true
    }
}

struct Take<'a> {
    pile: usize,
    player: usize,
    catalog: &'a Catalog,
    inventory: Option<&'a mut Inventory>,
    bot: bool,
}

impl Host {
    pub fn ammo_pickup(&mut self, pile: usize, player: usize) -> bool {
        let Some(ammo) = &mut self.ammo else {
            return false;
        };
        let Some(hooks) = &self.hooks else {
            return false;
        };
        let edict = native::word(player + 0x28).unwrap_or_default() as usize;
        let bot = hooks.steam_id(edict).as_deref() == Some("BOT");
        let inventory = if !self.dedicated && self.game.host() == Some(player) {
            Some(&mut self.local.inventory)
        } else {
            hooks
                .user(edict)
                .and_then(|id| self.guests.get_mut(&id))
                .map(|guest| &mut guest.session.inventory)
        };
        ammo.take(
            &self.game,
            &self.effective.ammo_piles,
            Take {
                pile,
                player,
                bot,
                inventory,
                catalog: &self.rules.catalog,
            },
        )
    }
}

pub(super) fn handle(game: &Game, entity: usize) -> Option<u32> {
    let mut data = vec![0; 4096 * 16];
    if !native::read(game.entities + 4, &mut data) {
        return None;
    }
    data.as_chunks::<16>()
        .0
        .iter()
        .enumerate()
        .find_map(|(index, entry)| {
            let address = u32::from_le_bytes(entry[..4].try_into().unwrap()) as usize;
            let serial = u32::from_le_bytes(entry[4..8].try_into().unwrap());
            (address == entity).then_some(index as u32 | (serial << 12))
        })
}

fn key(game: &Game, entity: usize, field: &CStr, value: &CStr) {
    unsafe {
        (game.set_key)(
            address(game.tools),
            address(entity),
            field.as_ptr(),
            value.as_ptr(),
        )
    };
}

fn tint(game: &Game, entity: usize, color: u32) {
    let value = CString::new(format!(
        "{} {} {} 255",
        (color >> 16) & 255,
        (color >> 8) & 255,
        color & 255
    ))
    .unwrap();
    key(game, entity, c"rendercolor", &value);
}

fn position_string(at: Vector) -> CString {
    CString::new(format!("{:.2} {:.2} {:.2}", at.x, at.y, at.z)).unwrap()
}

unsafe extern "thiscall" fn use_pile(
    pile: *mut c_void,
    activator: *mut c_void,
    caller: *mut c_void,
    mode: i32,
    value: f32,
) {
    if !pile.is_null() && !activator.is_null() {
        let handled = std::panic::catch_unwind(|| {
            with_client(|client| {
                client
                    .host
                    .as_mut()
                    .is_some_and(|host| host.ammo_pickup(pile.addr(), activator.addr()))
            })
            .unwrap_or(false)
                || dedicated::with_host(|host| host.ammo_pickup(pile.addr(), activator.addr()))
                    .unwrap_or(false)
        })
        .unwrap_or(false);
        if handled {
            return;
        }
    }
    let original = ORIGINAL.load(Ordering::Acquire);
    if original != 0 {
        let function = unsafe { std::mem::transmute::<usize, Use>(original) };
        unsafe { function(pile, activator, caller, mode, value) };
    }
}
