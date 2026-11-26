use std::{
    collections::HashMap,
    ffi::c_void,
    io,
    sync::atomic::{AtomicUsize, Ordering},
    time::{Duration, Instant},
};

use inventory_mod::{inventory::Inventory, items::ItemId, melee::BUTTON_RELOAD, rules::Rules};

use super::{CLIP, Game, Host, ItemDef, Session, address, ammo::handle, hooks::Hooks, slot_index};
use crate::native::{self, VtablePatch};

type Frame = unsafe extern "thiscall" fn(*mut c_void);
type Check = unsafe extern "thiscall" fn(*mut c_void) -> bool;
type Rush = unsafe extern "thiscall" fn(*mut c_void, f32);

static SAW_ORIGINAL: AtomicUsize = AtomicUsize::new(0);

const BUTTONS: usize = 0x1cb4;
const LEDGE: usize = 0x3a05;
const PROGRESS_START: usize = 0x2910;
const PROGRESS_DURATION: usize = 0x2914;

pub struct NativeActions {
    chainsaw: Option<VtablePatch>,
    revive: Option<Frame>,
    pinned: Option<Check>,
    rush: Option<Rush>,
}

#[derive(Default)]
pub struct Actions {
    progress: Option<Progress>,
    reload: bool,
}

#[derive(Default)]
pub(super) struct Cans {
    spent: HashMap<u32, i32>,
    last_prune: Option<Instant>,
}

impl Cans {
    pub fn spent(&self, game: &Game, can: usize) -> i32 {
        handle(game, can)
            .and_then(|handle| self.spent.get(&handle).copied())
            .unwrap_or_default()
    }

    pub fn set(&mut self, game: &Game, can: usize, spent: i32) {
        if let Some(handle) = handle(game, can) {
            if spent > 0 {
                self.spent.insert(handle, spent);
            } else {
                self.spent.remove(&handle);
            }
        }
    }

    pub fn clear(&mut self) {
        self.spent.clear();
    }

    pub fn prune(&mut self, game: &Game) {
        if self
            .last_prune
            .is_none_or(|last| last.elapsed() >= Duration::from_secs(5))
        {
            self.last_prune = Some(Instant::now());
            self.spent
                .retain(|&handle, _| game.resolve(handle).is_some());
        }
    }
}

#[derive(Clone, Copy)]
enum Action {
    Refuel { weapon: usize },
    Revive { item: ItemId, slot: Option<usize> },
}

#[derive(Clone, Copy)]
struct Progress {
    actor: usize,
    start: f32,
    duration: f32,
    action: Action,
}

struct Context<'a> {
    game: &'a Game,
    hooks: &'a Hooks,
    rules: &'a Rules,
}

impl NativeActions {
    pub fn install(game: &Game, rules: &Rules) -> Self {
        let base = game.image.base;
        let updated = game.image.timestamp == 0x6afc_cca0;
        let chainsaw = if rules.chainsaw.enabled {
            let (table, rva) = if updated {
                (0x6bc854, 0x3caa50)
            } else {
                (0x6bc7fc, 0x3ca970)
            };
            let function = base + rva;
            if native::matches_pattern(
                function,
                "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC A8 01 00 00",
            ) {
                VtablePatch::install(
                    base + table + 272 * 4,
                    function,
                    chainsaw_frame as Frame as usize,
                )
                .ok()
                .inspect(|_| SAW_ORIGINAL.store(function, Ordering::Release))
            } else {
                None
            }
        } else {
            None
        };
        let revive = native::word(game.player_table + 4 * 583)
            .map(|at| at as usize)
            .filter(|&at| {
                native::matches_pattern(
                    at,
                    "55 8B EC 83 EC 3C 53 56 8B F1 8B 06 8B 90 28 01 00 00 57 FF D2",
                )
            })
            .map(|at| unsafe { std::mem::transmute::<usize, Frame>(at) });
        let pinned = native::find_pattern(
            base + 0x2caf00,
            0xa00,
            "8B 15 ?? ?? ?? ?? 56 8B F1 8B 86 5C 3E 00 00",
        )
        .map(|at| unsafe { std::mem::transmute::<usize, Check>(at) });
        let rush = native::find_pattern(
            base + 0x318500,
            0x900,
            "55 8B EC 51 53 56 8B F1 8D 9E 80 32 00 00",
        )
        .map(|at| unsafe { std::mem::transmute::<usize, Rush>(at) });
        Self {
            chainsaw,
            revive,
            pinned,
            rush,
        }
    }

    pub fn stop(&mut self) -> io::Result<()> {
        if let Some(patch) = &mut self.chainsaw {
            patch.restore()?;
            SAW_ORIGINAL.store(0, Ordering::Release);
        }
        Ok(())
    }

    fn pinned(&self, actor: usize) -> bool {
        self.pinned
            .is_some_and(|check| unsafe { check(address(actor)) })
    }

    fn start_revive(
        &self,
        ctx: &Context<'_>,
        session: &mut Session,
        actor: usize,
        item: ItemId,
        slot: Option<usize>,
    ) -> Option<String> {
        let name = session.inventory.catalog().item(item);
        if self.revive.is_none()
            || !ctx.rules.revives(name.classname)
            || !ctx.game.living(actor)
            || !down(ctx.game, actor)
        {
            return None;
        }
        if self.pinned(actor) {
            return Some("Not while you are pinned".into());
        }
        if !ctx.rules.self_revive.ledge && hanging(actor) {
            return Some("You cannot pull yourself up off a ledge".into());
        }
        if session
            .actions
            .progress
            .is_some_and(|p| matches!(p.action, Action::Revive { .. }))
        {
            return Some(String::new());
        }
        if item_source(ctx.game, actor, &session.inventory, item, slot).is_none() {
            return Some(format!("You have no {} left to get up with", name.name));
        }
        let action = Action::Revive { item, slot };
        let start = ctx.hooks.time()?;
        let duration = ctx.rules.self_revive.seconds;
        if duration <= 0.0 {
            return Some(self.finish_revive(ctx, session, actor, item, slot));
        }
        let title = name.name;
        let progress = Progress {
            actor,
            start,
            duration,
            action,
        };
        progress.apply(ctx.hooks);
        session.actions.progress = Some(progress);
        Some(format!("Getting up with your {title}"))
    }

    fn finish_revive(
        &self,
        ctx: &Context<'_>,
        session: &mut Session,
        actor: usize,
        item: ItemId,
        slot: Option<usize>,
    ) -> String {
        let name = session.inventory.catalog().item(item).clone();
        let Some(source) = item_source(ctx.game, actor, &session.inventory, item, slot) else {
            return format!("You have no {} left to get up with", name.name);
        };
        let Some(revive) = self.revive else {
            return "You could not get up".into();
        };
        unsafe { revive(address(actor)) };
        if down(ctx.game, actor) {
            return "You could not get up".into();
        }
        match source {
            ItemSource::Stored(index) => {
                session.inventory.remove_from_slot(index, 1);
            }
            ItemSource::Held(handle) => {
                let live_item = || {
                    ctx.game
                        .resolve(handle)
                        .filter(|&entity| ctx.game.is_live_entity(entity))
                };
                if let Some(entity) = live_item() {
                    ctx.game
                        .release(actor, entity, &name, session.inventory.catalog(), true);
                }
                // The release can itself remove the item. Resolve the handle
                // again before the forced removal fallback.
                if let Some(entity) = live_item() {
                    ctx.game.remove(entity);
                }
                if live_item().is_some() {
                    return format!("Got up, but the game could not consume your {}", name.name);
                }
                if let Some(home) = session.inventory.held_slot(item) {
                    session.inventory.clear_held(home);
                }
            }
        }
        if ctx.rules.self_revive.rush
            && name.classname == "weapon_adrenaline"
            && let Some(rush) = self.rush
        {
            let duration = crate::cvars::ConVar::find(c"adrenaline_duration")
                .and_then(|var| var.scalar())
                .filter(|seconds| (0.0..600.0).contains(seconds) && *seconds > 0.0)
                .unwrap_or(15.0);
            unsafe { rush(address(actor), duration) };
        }
        ctx.hooks.changed(actor);
        format!("Got up with your {}", name.name)
    }

    fn frame(&self, ctx: &Context<'_>, actor: usize, session: &mut Session) -> Option<String> {
        let now = ctx.hooks.time()?;
        let buttons = native::word(actor + BUTTONS).unwrap_or_default() as i32;
        let holding_reload = buttons & BUTTON_RELOAD != 0;
        let was_down = std::mem::replace(&mut session.actions.reload, holding_reload);
        let pressed = holding_reload && !was_down;
        if let Some(progress) = session.actions.progress {
            let eligible = progress.actor == actor && ctx.game.living(actor) && !self.pinned(actor);
            let valid = eligible
                && match progress.action {
                    Action::Refuel { weapon } => {
                        ctx.game.active(actor) == Some(weapon) && !down(ctx.game, actor)
                    }
                    Action::Revive { .. } => down(ctx.game, actor),
                };
            if valid && now < progress.start + progress.duration {
                return None;
            }
            if progress.actor == actor {
                progress.clear(ctx.hooks);
            }
            session.actions.progress = None;
            if valid {
                return Some(match progress.action {
                    Action::Refuel { weapon } => pour(ctx.hooks, weapon, &mut session.inventory),
                    Action::Revive { item, slot } => {
                        self.finish_revive(ctx, session, actor, item, slot)
                    }
                });
            }
        }
        if !pressed
            || self.chainsaw.is_none()
            || !ctx.rules.chainsaw.enabled
            || !ctx.game.available(actor)
            || self.pinned(actor)
            || down(ctx.game, actor)
        {
            return None;
        }
        let weapon = ctx
            .game
            .active(actor)
            .filter(|&weapon| ctx.game.classname(weapon).as_deref() == Some("weapon_chainsaw"))?;
        if native::word(weapon + CLIP).unwrap_or(0) as i32 >= max_fuel(weapon) {
            return Some("The chainsaw is full".into());
        }
        if remaining_fuel(&session.inventory) <= 0 {
            return Some("No fuel: carry a gas can in your inventory".into());
        }
        if ctx.rules.chainsaw.seconds <= 0.0 {
            return Some(pour(ctx.hooks, weapon, &mut session.inventory));
        }
        let progress = Progress {
            actor,
            start: now,
            duration: ctx.rules.chainsaw.seconds,
            action: Action::Refuel { weapon },
        };
        progress.apply(ctx.hooks);
        session.actions.progress = Some(progress);
        Some("Refuelling the chainsaw".into())
    }
}

impl Progress {
    fn apply(self, hooks: &Hooks) {
        native::store_word(self.actor + PROGRESS_START, self.start.to_bits());
        native::store_word(self.actor + PROGRESS_DURATION, self.duration.to_bits());
        hooks.changed(self.actor);
    }

    fn clear(self, hooks: &Hooks) {
        if native::word(self.actor + PROGRESS_START) == Some(self.start.to_bits())
            && native::word(self.actor + PROGRESS_DURATION) == Some(self.duration.to_bits())
        {
            native::store_word(self.actor + PROGRESS_DURATION, 0);
            hooks.changed(self.actor);
        }
    }
}

enum ItemSource {
    Held(u32),
    Stored(usize),
}

fn item_source(
    game: &Game,
    actor: usize,
    inventory: &Inventory,
    item: ItemId,
    slot: Option<usize>,
) -> Option<ItemSource> {
    let def: &ItemDef = inventory.catalog().item(item);
    let held = || {
        game.weapons(actor, inventory.catalog())[slot_index(def.game_slot)]
            .filter(|&(_, current)| current == item)
            .and_then(|(entity, _)| handle(game, entity))
            .map(ItemSource::Held)
    };
    if slot.is_none()
        && let Some(held) = held()
    {
        return Some(held);
    }
    let has = |index| {
        inventory
            .slot(index)
            .is_some_and(|stack| stack.item == Some(item) && stack.count > 0)
    };
    slot.filter(|&index| has(index))
        .or_else(|| (0..inventory.limits().slots).find(|&index| has(index)))
        .map(ItemSource::Stored)
        .or_else(|| slot.and_then(|_| held()))
}

fn hanging(actor: usize) -> bool {
    let mut state = [0];
    native::read(actor + LEDGE, &mut state) && state[0] != 0
}

fn down(game: &Game, actor: usize) -> bool {
    (unsafe { (game.incapacitated)(address(actor)) }) || hanging(actor)
}

fn remaining_fuel(inventory: &Inventory) -> i64 {
    let Some(can) = inventory.catalog().find_item("weapon_gascan") else {
        return 0;
    };
    inventory
        .slots()
        .iter()
        .filter(|stack| stack.item == Some(can))
        .map(|stack| stack.fuel(inventory.catalog()))
        .sum()
}

fn max_fuel(saw: usize) -> i32 {
    let Some(slot) = native::vslot(saw, 323) else {
        return 30;
    };
    let get = unsafe {
        std::mem::transmute::<usize, unsafe extern "thiscall" fn(*mut c_void) -> i32>(slot)
    };
    let capacity = unsafe { get(address(saw)) };
    if (1..=1000).contains(&capacity) {
        capacity
    } else {
        30
    }
}

fn pour(hooks: &Hooks, weapon: usize, inventory: &mut Inventory) -> String {
    let capacity = max_fuel(weapon);
    let before = (native::word(weapon + CLIP).unwrap_or_default() as i32).max(0);
    let mut missing = capacity - before;
    if missing <= 0 {
        return "The chainsaw is full".into();
    }
    let Some(can) = inventory.catalog().find_item("weapon_gascan") else {
        return "No fuel: carry a gas can in your inventory".into();
    };
    for open in [true, false] {
        for index in 0..inventory.limits().slots {
            let stack = &inventory.slots()[index];
            if missing > 0 && stack.item == Some(can) && (stack.spent > 0) == open {
                missing -= inventory.take_fuel(index, missing);
            }
        }
    }
    let left = capacity - missing;
    if left == before {
        return "No fuel: carry a gas can in your inventory".into();
    }
    native::store_word(weapon + CLIP, left as u32);
    hooks.changed(weapon);
    format!(
        "Chainsaw refuelled: {left} / {capacity} ({} left in your gas cans)",
        remaining_fuel(inventory)
    )
}

unsafe extern "thiscall" fn chainsaw_frame(weapon: *mut c_void) {
    let original = SAW_ORIGINAL.load(Ordering::Acquire);
    if original != 0 && native::word(weapon.addr() + CLIP).unwrap_or_default() as i32 > 0 {
        let original = unsafe { std::mem::transmute::<usize, Frame>(original) };
        unsafe { original(weapon) };
    }
}

impl Host {
    pub fn actions_frame(&mut self) {
        let Some(hooks) = &self.hooks else {
            return;
        };
        if !self.rules.chainsaw.enabled && !self.rules.self_revive.enabled {
            return;
        }
        let ctx = Context {
            game: &self.game,
            hooks,
            rules: &self.rules,
        };
        if !self.dedicated
            && let Some(player) = self.game.host()
        {
            update(&self.actions, &ctx, player, &mut self.local);
        }
        for (&id, guest) in &mut self.guests {
            if hooks.user(guest.edict) == Some(id) {
                update(&self.actions, &ctx, guest.player, &mut guest.session);
            }
        }
    }

    pub fn clear_actions(&mut self) {
        let Some(hooks) = &self.hooks else {
            return;
        };
        if let Some(progress) = self.local.actions.progress.take() {
            progress.clear(hooks);
        }
        for guest in self.guests.values_mut() {
            if let Some(progress) = guest.session.actions.progress.take() {
                progress.clear(hooks);
            }
        }
    }
}

fn update(actions: &NativeActions, ctx: &Context<'_>, actor: usize, session: &mut Session) {
    if let Some(notice) = actions.frame(ctx, actor, session) {
        session.notice = notice;
        let _ = session.save();
    }
}

pub(super) fn revive_command(
    actions: &NativeActions,
    game: &Game,
    hooks: &Hooks,
    rules: &Rules,
    session: &mut Session,
    actor: usize,
    command: &str,
) -> Option<String> {
    let (action, target) = command.split_once(' ')?;
    let (item, index) = match action {
        "inv_select" => (session.inventory.catalog().find_item(target)?, None),
        "inv_use" => {
            let index = target.parse().ok()?;
            (session.inventory.slot(index)?.item?, Some(index))
        }
        _ => return None,
    };
    actions.start_revive(&Context { game, hooks, rules }, session, actor, item, index)
}
