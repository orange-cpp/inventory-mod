use std::{cell::Cell, ptr::NonNull};

use inventory_mod::{inventory::Inventory, items::ItemId, melee::BUTTON_RELOAD, rules::Rules};

use super::{Ent, Game, field, native::*, set_field, slot_index, vtable::VtablePatch};
use crate::source::{self, Entity, Source};

type Frame = unsafe extern "C" fn(*mut Entity);
const CHAINSAW_FRAME_RVA: usize = 0xc2608c;
const MAX_CLIP_SLOT: usize = 324;
const GAS_CAN: &str = "weapon_gascan";

thread_local! {
    static ORIGINAL_CHAINSAW_FRAME: Cell<Option<Frame>> = const { Cell::new(None) };
}

#[derive(Clone, Copy)]
enum Activity {
    Refuel { weapon: u32 },
    Revive { item: ItemId, index: Option<usize> },
}

#[derive(Clone, Copy)]
struct Progress {
    handle: u32,
    start: f32,
    duration: f32,
    activity: Activity,
}

#[derive(Default)]
pub struct Actions {
    reload_down: bool,
    progress: Option<Progress>,
}

pub struct ActionContext<'a> {
    pub source: &'a Source,
    pub rules: &'a Rules,
    pub inventory: &'a mut Inventory,
    pub actions: &'a mut Actions,
}

impl Game {
    pub fn install_actions(&mut self, rules: &Rules) -> bool {
        if !rules.chainsaw.enabled {
            return true;
        }
        let base = self.native.base;
        let original = self.native.chainsaw_frame;
        self.chainsaw_patch = unsafe {
            VtablePatch::install(
                base,
                base + CHAINSAW_FRAME_RVA,
                original as usize,
                chainsaw_frame as Frame as usize,
            )
        };
        if self.chainsaw_patch.is_none() {
            return false;
        }
        ORIGINAL_CHAINSAW_FRAME.set(Some(original));
        true
    }

    pub fn uninstall_actions(&mut self) {
        self.chainsaw_patch = None;
        ORIGINAL_CHAINSAW_FRAME.set(None);
    }

    fn max_fuel(saw: Ent) -> i32 {
        // SAFETY: Called only for a chainsaw in the checked build.
        let max_clip = unsafe {
            source::virtual_method::<unsafe extern "C" fn(*mut Entity) -> i32>(
                saw.cast(),
                MAX_CLIP_SLOT,
            )
        };
        let size = unsafe { max_clip(saw.as_ptr()) };
        if (1..=1000).contains(&size) { size } else { 30 }
    }

    fn remaining_fuel(inventory: &Inventory) -> i64 {
        let Some(can) = inventory.catalog().find_item(GAS_CAN) else {
            return 0;
        };
        inventory
            .slots()
            .iter()
            .filter(|stack| stack.item == Some(can))
            .map(|stack| stack.fuel(inventory.catalog()))
            .sum()
    }

    fn pour_fuel(&self, saw: Ent, inventory: &mut Inventory) -> String {
        let capacity = Self::max_fuel(saw);
        let before = unsafe { field::<i32>(saw, CLIP) }.max(0);
        if before >= capacity {
            return "The chainsaw is full".into();
        }
        let can = inventory.catalog().find_item(GAS_CAN);
        let mut needed = capacity - before;
        // Use the opened cans first. Inventory removes cans as they run dry.
        for open in [true, false] {
            for index in 0..inventory.limits().slots {
                let stack = &inventory.slots()[index];
                if needed > 0 && stack.item == can && (stack.spent > 0) == open {
                    needed -= inventory.take_fuel(index, needed);
                }
            }
        }
        let clip = capacity - needed;
        if clip <= before {
            return "No fuel: carry a gas can in your inventory".into();
        }
        unsafe {
            set_field(saw, CLIP, clip);
            (self.native.network_changed)(saw.as_ptr());
        }
        format!(
            "Chainsaw refuelled: {clip} / {capacity} ({} left in your gas cans)",
            Self::remaining_fuel(inventory)
        )
    }

    fn down(&self, player: Ent) -> bool {
        unsafe {
            (self.native.incapacitated)(player.as_ptr())
                || (self.native.hanging_from_ledge)(player.as_ptr())
        }
    }

    pub(super) fn active(&self, player: Ent) -> Option<Ent> {
        NonNull::new(unsafe { (self.native.active_weapon)(player.as_ptr()) })
    }

    fn set_progress(&self, player: Ent, action: Progress) {
        unsafe {
            set_field(player, PROGRESS_START, action.start);
            set_field(player, PROGRESS_DURATION, action.duration);
            (self.native.network_changed)(player.as_ptr());
        }
    }

    fn clear_progress(&self, player: Ent, action: Progress) {
        if action.handle != self.handle(player) || action.duration <= 0.0 {
            return;
        }
        unsafe {
            if field::<f32>(player, PROGRESS_START) == action.start
                && field::<f32>(player, PROGRESS_DURATION) == action.duration
            {
                set_field(player, PROGRESS_DURATION, 0.0f32);
                (self.native.network_changed)(player.as_ptr());
            }
        }
    }

    pub fn cancel_actions(&self, player: Option<Ent>, actions: &mut Actions) {
        if let Some(progress) = actions.progress.take()
            && let Some(player) = player
        {
            self.clear_progress(player, progress);
        }
        actions.reload_down = false;
    }

    pub fn action_frame(&mut self, player: Ent, ctx: &mut ActionContext<'_>) -> Option<String> {
        let now = ctx.source.current_time()?;
        let pressing_reload = unsafe { field::<i32>(player, BUTTONS) } & BUTTON_RELOAD != 0;
        let pressed =
            pressing_reload && !std::mem::replace(&mut ctx.actions.reload_down, pressing_reload);
        let pending = ctx.actions.progress;
        if let Some(progress) = pending {
            let valid = progress.handle == self.handle(player)
                && unsafe {
                    field::<u8>(player, LIFE_STATE) == 0
                        && !(self.native.dominated)(player.as_ptr())
                };
            let saw = match progress.activity {
                Activity::Refuel { weapon } if valid && !self.down(player) => self
                    .active(player)
                    .filter(|&saw| self.handle(saw) == weapon),
                _ => None,
            };
            let can_complete = match progress.activity {
                Activity::Refuel { .. } => saw.is_some(),
                Activity::Revive { .. } => valid && self.down(player),
            };
            if can_complete && now < progress.start + progress.duration {
                return None;
            }
            self.clear_progress(player, progress);
            ctx.actions.progress = None;
            if can_complete {
                return match progress.activity {
                    Activity::Refuel { .. } => saw.map(|saw| self.pour_fuel(saw, ctx.inventory)),
                    Activity::Revive { item, index } => {
                        Some(self.finish_revive(player, item, index, ctx))
                    }
                };
            }
        }
        if !pressed
            || self.chainsaw_patch.is_none()
            || !ctx.rules.chainsaw.enabled
            || !self.can_handle(player)
            || self.down(player)
            || unsafe { field::<u8>(player, LIFE_STATE) != 0 }
        {
            return None;
        }
        let saw = self
            .active(player)
            .filter(|&saw| self.classname(saw) == "weapon_chainsaw")?;
        if unsafe { field::<i32>(saw, CLIP) } >= Self::max_fuel(saw) {
            return Some("The chainsaw is full".into());
        }
        if Self::remaining_fuel(ctx.inventory) <= 0 {
            return Some("No fuel: carry a gas can in your inventory".into());
        }
        let duration = ctx.rules.chainsaw.seconds;
        if duration <= 0.0 {
            return Some(self.pour_fuel(saw, ctx.inventory));
        }
        let action = Progress {
            handle: self.handle(player),
            start: now,
            duration,
            activity: Activity::Refuel {
                weapon: self.handle(saw),
            },
        };
        self.set_progress(player, action);
        ctx.actions.progress = Some(action);
        Some("Refuelling the chainsaw".into())
    }

    fn revive_source(
        &self,
        player: Ent,
        item: ItemId,
        index: Option<usize>,
        inventory: &Inventory,
    ) -> Option<ReviveSource> {
        let def = inventory.catalog().item(item);
        let held = || {
            self.weapon_at(player, slot_index(def.game_slot))
                .filter(|&weapon| {
                    self.classname(weapon) == def.classname && !self.pending_removal(weapon)
                })
                .map(ReviveSource::Held)
        };
        if index.is_none()
            && let Some(held) = held()
        {
            return Some(held);
        }
        let available = |index| {
            inventory
                .slot(index)
                .is_some_and(|stack| stack.item == Some(item) && stack.count > 0)
        };
        if let Some(index) = index
            .filter(|&index| available(index))
            .or_else(|| (0..inventory.limits().slots).find(|&index| available(index)))
        {
            return Some(ReviveSource::Inventory(index));
        }
        index.and_then(|_| held())
    }

    fn finish_revive(
        &mut self,
        player: Ent,
        item: ItemId,
        index: Option<usize>,
        ctx: &mut ActionContext<'_>,
    ) -> String {
        let def = ctx.inventory.catalog().item(item);
        let title = def.name;
        if self
            .revive_source(player, item, index, ctx.inventory)
            .is_none()
        {
            return format!("You have no {title} left to get up with");
        }
        unsafe { (self.native.on_revived)(player.as_ptr()) };
        if self.down(player) {
            return "You could not get up".into();
        }
        match self.revive_source(player, item, index, ctx.inventory) {
            Some(ReviveSource::Held(weapon)) => {
                if self.release(player, weapon, def, self.native.quiet_drop) {
                    self.remove(weapon);
                    if let Some(home) = ctx.inventory.held_slot(item) {
                        ctx.inventory.clear_held(home);
                    }
                }
            }
            Some(ReviveSource::Inventory(index)) => {
                ctx.inventory.remove_from_slot(index, 1);
            }
            None => {}
        }
        if ctx.rules.self_revive.rush
            && ctx.inventory.catalog().item(item).classname == "weapon_adrenaline"
        {
            let seconds = ctx
                .source
                .find_cvar(c"adrenaline_duration")
                .map(|value| value.value())
                .filter(|&n| n > 0.0 && n < 600.0)
                .unwrap_or(15.0);
            unsafe { (self.native.on_adrenaline_used)(player.as_ptr(), seconds) };
        }
        unsafe { (self.native.network_changed)(player.as_ptr()) };
        format!("Got up with your {title}")
    }

    pub fn start_revive(
        &mut self,
        player: Ent,
        item: ItemId,
        index: Option<usize>,
        ctx: &mut ActionContext<'_>,
    ) -> Option<String> {
        let def = ctx.inventory.catalog().item(item);
        if !ctx.rules.revives(def.classname) || !self.down(player) {
            return None;
        }
        if !ctx.rules.self_revive.ledge
            && unsafe { (self.native.hanging_from_ledge)(player.as_ptr()) }
        {
            return Some("You cannot pull yourself up off a ledge".into());
        }
        if unsafe { (self.native.dominated)(player.as_ptr()) } {
            return Some("Not while you are pinned".into());
        }
        if ctx
            .actions
            .progress
            .is_some_and(|p| matches!(p.activity, Activity::Revive { .. }))
        {
            return Some(String::new());
        }
        let start = ctx.source.current_time()?;
        let title = def.name;
        if self
            .revive_source(player, item, index, ctx.inventory)
            .is_none()
        {
            return Some(format!("You have no {title} left to get up with"));
        }
        let duration = ctx.rules.self_revive.seconds;
        if duration <= 0.0 {
            return Some(self.finish_revive(player, item, index, ctx));
        }
        let action = Progress {
            handle: self.handle(player),
            start,
            duration,
            activity: Activity::Revive { item, index },
        };
        self.set_progress(player, action);
        ctx.actions.progress = Some(action);
        Some(format!("Getting up with your {title}"))
    }
}

enum ReviveSource {
    Held(Ent),
    Inventory(usize),
}

unsafe extern "C" fn chainsaw_frame(saw: *mut Entity) {
    if let Some(entity) = NonNull::new(saw)
        && unsafe { field::<i32>(entity, CLIP) } > 0
        && let Some(original) = ORIGINAL_CHAINSAW_FRAME.get()
    {
        unsafe { original(saw) };
    }
}
