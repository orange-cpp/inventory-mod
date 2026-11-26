use std::{fmt::Write, sync::Arc};

use crate::items::{Catalog, ItemId};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct WeaponState {
    pub clip: i32,
    pub ammo: i32,
    pub melee: String,
    pub upgrades: i32,
    pub special: i32,
    /// Wear ranges from zero for a new weapon to 10,000 for a broken one.
    pub wear: i32,
}

impl Default for WeaponState {
    fn default() -> Self {
        Self {
            clip: -1,
            ammo: -1,
            melee: String::new(),
            upgrades: 0,
            special: 0,
            wear: 0,
        }
    }
}

impl WeaponState {
    /// The engine uses a concrete script name to give a melee weapon.
    pub fn give_name<'a>(&'a self, classname: &'a str) -> &'a str {
        if classname == "weapon_melee" {
            melee_script_name(&self.melee).unwrap_or("fireaxe")
        } else {
            classname
        }
    }

    pub(crate) fn write_fields(&self, out: &mut String) {
        let melee = if self.melee.is_empty() {
            "-"
        } else {
            &self.melee
        };
        write!(
            out,
            " {} {} {} {} {} {}",
            self.clip, self.ammo, melee, self.upgrades, self.special, self.wear
        )
        .unwrap();
    }

    pub(crate) fn read_fields<'a>(fields: &mut impl Iterator<Item = &'a str>) -> Self {
        let clip = fields.next().and_then(parse_int).unwrap_or(-1);
        let ammo = fields.next().and_then(parse_int).unwrap_or(-1);
        let melee = fields.next().filter(|&name| name != "-").unwrap_or("");
        let mut nonnegative = || fields.next().and_then(parse_int).unwrap_or(0).max(0);
        Self {
            clip,
            ammo,
            melee: melee.to_owned(),
            upgrades: nonnegative(),
            special: nonnegative(),
            wear: nonnegative(),
        }
    }
}

/// Ignore spawn selectors and missing names from legacy melee entries.
pub fn melee_script_name(name: &str) -> Option<&str> {
    let name = name.trim();
    (!name.is_empty()
        && name != "-"
        && !["any", "random", "weapon_melee"]
            .iter()
            .any(|alias| name.eq_ignore_ascii_case(alias)))
    .then_some(name)
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct Stack {
    pub item: Option<ItemId>,
    pub count: i32,
    /// A held item reserves its slot and counts towards carried weight.
    pub held: bool,
    pub weapon: WeaponState,
    /// Fuel missing from the open can. Other cans in the stack are full.
    pub spent: i32,
}

impl Stack {
    fn new(item: ItemId, count: i32) -> Self {
        Self {
            item: Some(item),
            count,
            ..Self::default()
        }
    }

    pub fn is_empty(&self) -> bool {
        self.item.is_none() || (self.count == 0 && !self.held)
    }

    pub fn fuel(&self, catalog: &Catalog) -> i64 {
        self.item.map_or(0, |id| {
            i64::from(self.count) * i64::from(catalog.item(id).fuel) - i64::from(self.spent)
        })
    }

    fn add_spent(&mut self, amount: i64, full: i32) {
        if full <= 0 {
            self.spent = 0;
            return;
        }
        let spent = i64::from(self.spent) + amount;
        let full = i64::from(full);
        let emptied = (spent / full).min(i64::from(self.count)) as i32;
        self.count -= emptied;
        self.spent = if self.count > 0 {
            (spent % full) as i32
        } else {
            0
        };
        if self.is_empty() {
            *self = Self::default();
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Limits {
    pub slots: usize,
    pub max_weight: f32,
    pub weapon_slots: usize,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            slots: 8,
            max_weight: 0.0,
            weapon_slots: 2,
        }
    }
}

#[derive(Clone, Debug)]
pub struct Inventory {
    catalog: Arc<Catalog>,
    limits: Limits,
    slots: Vec<Stack>,
    selected: usize,
    revision: u32,
    weapons_weight: f32,
    ammo_weight: f32,
}

impl Default for Inventory {
    fn default() -> Self {
        Self::new(Limits::default())
    }
}

impl Inventory {
    pub fn new(limits: Limits) -> Self {
        Self::with_catalog(limits, Catalog::shared_default())
    }

    pub fn with_catalog(mut limits: Limits, catalog: Arc<Catalog>) -> Self {
        limits.slots = limits.slots.max(1);
        limits.weapon_slots = limits.weapon_slots.min(16);
        Self {
            catalog,
            slots: vec![Stack::default(); limits.slots + limits.weapon_slots],
            limits,
            selected: 0,
            revision: 0,
            weapons_weight: 0.0,
            ammo_weight: 0.0,
        }
    }

    pub fn catalog(&self) -> &Catalog {
        &self.catalog
    }

    pub fn limits(&self) -> Limits {
        self.limits
    }

    pub fn set_max_weight(&mut self, weight: f32) {
        self.limits.max_weight = weight.max(0.0);
    }

    pub fn slot(&self, index: usize) -> Option<&Stack> {
        self.slots.get(index)
    }

    pub fn slots(&self) -> &[Stack] {
        &self.slots
    }

    fn item_boxes(&self) -> &[Stack] {
        &self.slots[..self.limits.slots]
    }

    fn boxes_for(&self, id: ItemId) -> std::ops::Range<usize> {
        if self.catalog.item(id).is_weapon() {
            self.limits.slots..self.slots.len()
        } else {
            0..self.limits.slots
        }
    }

    pub fn is_weapon_slot(&self, index: usize) -> bool {
        (self.limits.slots..self.slots.len()).contains(&index)
    }

    pub fn room_for(&self, id: ItemId) -> i32 {
        let unit = self.catalog.item(id).weight;
        if self.limits.max_weight <= 0.0 || unit <= 0.0 {
            return 1 << 20;
        }
        let left = self.limits.max_weight - self.weight();
        if left <= 0.0 {
            return 0;
        }
        (left / unit + 1e-4).floor() as i32
    }

    pub fn add(&mut self, id: ItemId, count: i32, spent: i32) -> i32 {
        let def = self.catalog.item(id);
        if def.is_weapon() {
            return (0..count)
                .take_while(|_| self.add_weapon(id, WeaponState::default(), None).is_some())
                .count() as i32;
        }
        let count = count.min(self.room_for(id));
        let mut added = 0;
        let mut first = None;
        let slots = &mut self.slots[..self.limits.slots];
        // Fill existing stacks before starting new ones, including the held item's home.
        for fill_empty in [false, true] {
            for (index, stack) in slots.iter_mut().enumerate() {
                if added >= count {
                    break;
                }
                if fill_empty {
                    if !stack.is_empty() {
                        continue;
                    }
                    *stack = Stack::new(id, 0);
                } else if stack.item != Some(id) || stack.count >= def.max_stack {
                    continue;
                }
                let take = (def.max_stack - stack.count).min(count - added);
                stack.count += take;
                added += take;
                first.get_or_insert(index);
            }
        }
        if let Some(index) = first {
            self.slots[index].add_spent(i64::from(spent.max(0)), def.fuel);
            self.changed();
        }
        added
    }

    pub fn add_to_slot(&mut self, index: usize, id: ItemId, count: i32, spent: i32) -> i32 {
        if count <= 0 || !self.boxes_for(id).contains(&index) {
            return 0;
        }
        let def = self.catalog.item(id);
        if def.is_weapon() {
            return i32::from(
                self.add_weapon(id, WeaponState::default(), Some(index))
                    .is_some(),
            );
        }
        let room = self.room_for(id);
        let stack = &mut self.slots[index];
        if !stack.is_empty() && stack.item != Some(id) {
            return 0;
        }
        let take = count.min(room).min(def.max_stack - stack.count);
        if take <= 0 {
            return 0;
        }
        stack.item = Some(id);
        stack.count += take;
        stack.add_spent(i64::from(spent.max(0)), def.fuel);
        self.changed();
        take
    }

    pub fn add_weapon(
        &mut self,
        id: ItemId,
        state: WeaponState,
        index: Option<usize>,
    ) -> Option<usize> {
        if !self.catalog.item(id).is_weapon() || self.room_for(id) < 1 {
            return None;
        }
        let index = index.or_else(|| self.boxes_for(id).find(|&i| self.slots[i].is_empty()))?;
        if !self.is_weapon_slot(index) || !self.slots[index].is_empty() {
            return None;
        }
        self.slots[index] = Stack {
            weapon: state,
            ..Stack::new(id, 1)
        };
        self.changed();
        Some(index)
    }

    pub fn has_box_for(&self, id: ItemId) -> bool {
        let def = self.catalog.item(id);
        self.slots[self.boxes_for(id)].iter().any(|stack| {
            stack.is_empty()
                || (!def.is_weapon() && stack.item == Some(id) && stack.count < def.max_stack)
        })
    }

    pub fn remove_from_slot(&mut self, index: usize, count: i32) -> i32 {
        let Some(stack) = self.slots.get_mut(index) else {
            return 0;
        };
        let removed = count.min(stack.count);
        if removed <= 0 {
            return 0;
        }
        stack.count -= removed;
        if stack.count == 0 {
            stack.spent = 0;
        }
        if stack.is_empty() {
            *stack = Stack::default();
        }
        self.changed();
        removed
    }

    pub fn remove_item(&mut self, id: ItemId, count: i32) -> i32 {
        let mut removed = 0;
        for index in self.boxes_for(id).rev() {
            if removed >= count {
                break;
            }
            if self.slots[index].item == Some(id) {
                removed += self.remove_from_slot(index, count - removed);
            }
        }
        removed
    }

    pub fn move_slot(&mut self, from: usize, to: usize) -> bool {
        if self.is_weapon_slot(from) != self.is_weapon_slot(to) {
            return false;
        }
        let Ok([source, target]) = self.slots.get_disjoint_mut([from, to]) else {
            return false;
        };
        let Some(id) = source.item.filter(|_| !source.is_empty()) else {
            return false;
        };
        let def = self.catalog.item(id);
        let moved = (def.max_stack - target.count).min(source.count);
        if target.item != Some(id) || moved <= 0 {
            std::mem::swap(source, target);
        } else {
            target.count += moved;
            source.count -= moved;
            if source.count == 0 {
                target.add_spent(i64::from(std::mem::take(&mut source.spent)), def.fuel);
                if source.is_empty() {
                    *source = Stack::default();
                }
            }
        }
        self.changed();
        true
    }

    pub fn held_slot(&self, id: ItemId) -> Option<usize> {
        self.item_boxes()
            .iter()
            .position(|stack| stack.held && stack.item == Some(id))
    }

    pub fn home_for(&self, id: ItemId) -> Option<usize> {
        let boxes = self.item_boxes();
        boxes
            .iter()
            .position(|stack| stack.item == Some(id) && !stack.held)
            .or_else(|| boxes.iter().position(Stack::is_empty))
    }

    pub fn set_held(&mut self, index: usize, id: ItemId) -> bool {
        if self.catalog.item(id).is_weapon() || index >= self.limits.slots {
            return false;
        }
        let stack = &mut self.slots[index];
        if !stack.is_empty() && (stack.item != Some(id) || stack.held) {
            return false;
        }
        stack.item = Some(id);
        stack.held = true;
        self.changed();
        true
    }

    fn take_held(&mut self, index: usize) -> Option<&mut Stack> {
        let stack = self.slots.get_mut(index)?;
        if !stack.held {
            return None;
        }
        stack.held = false;
        Some(stack)
    }

    pub fn clear_held(&mut self, index: usize) {
        if let Some(stack) = self.take_held(index) {
            if stack.is_empty() {
                *stack = Stack::default();
            }
            self.changed();
        }
    }

    pub fn release_held(&mut self, index: usize, spent: i32) {
        let Some(id) = self.slot(index).and_then(|stack| stack.item) else {
            return;
        };
        let full = self.catalog.item(id).fuel;
        if let Some(stack) = self.take_held(index) {
            stack.count = stack.count.saturating_add(1);
            stack.add_spent(i64::from(spent.max(0)), full);
            self.changed();
        }
    }

    pub fn take_fuel(&mut self, index: usize, amount: i32) -> i32 {
        let taken = self
            .slot(index)
            .map_or(0, |stack| stack.fuel(&self.catalog))
            .min(i64::from(amount));
        if taken <= 0 {
            return 0;
        }
        self.add_spent(index, taken as i32);
        taken as i32
    }

    pub fn add_spent(&mut self, index: usize, spent: i32) {
        let Some(stack) = self
            .slots
            .get_mut(index)
            .filter(|stack| !stack.is_empty() && spent > 0)
        else {
            return;
        };
        let full = stack.item.map_or(0, |id| self.catalog.item(id).fuel);
        stack.add_spent(i64::from(spent), full);
        self.changed();
    }

    pub fn next_spent(&self, index: usize) -> i32 {
        self.slot(index)
            .filter(|stack| stack.count == 1)
            .map_or(0, |stack| stack.spent)
    }

    pub fn count_of(&self, id: ItemId) -> i64 {
        self.slots[self.boxes_for(id)]
            .iter()
            .filter(|stack| stack.item == Some(id))
            .map(|stack| i64::from(stack.count))
            .sum()
    }

    pub fn used_slots(&self) -> usize {
        self.slots.iter().filter(|stack| !stack.is_empty()).count()
    }

    pub fn weight(&self) -> f32 {
        self.slots
            .iter()
            .fold(self.weapons_weight + self.ammo_weight, |weight, stack| {
                weight
                    + stack.item.map_or(0.0, |id| {
                        self.catalog.item(id).weight * (stack.count as f32 + f32::from(stack.held))
                    })
            })
    }

    pub fn set_weapons_weight(&mut self, weight: f32) {
        self.weapons_weight = weight;
    }

    pub fn weapons_weight(&self) -> f32 {
        self.weapons_weight
    }

    pub fn set_ammo_weight(&mut self, weight: f32) {
        self.ammo_weight = weight;
    }

    pub fn ammo_weight(&self) -> f32 {
        self.ammo_weight
    }

    pub fn selected(&self) -> usize {
        self.selected
    }

    pub fn select(&mut self, index: usize) {
        if index < self.limits.slots {
            self.selected = index;
        }
    }

    pub fn select_step(&mut self, direction: i32) {
        let len = self.limits.slots;
        let next = |offset| {
            if direction < 0 {
                (self.selected + len - offset) % len
            } else {
                (self.selected + offset) % len
            }
        };
        self.selected = (1..=len)
            .map(next)
            .find(|&index| !self.slots[index].is_empty())
            .unwrap_or_else(|| next(1));
    }

    pub fn clear(&mut self) {
        self.slots.fill(Stack::default());
        self.changed();
    }

    pub fn revision(&self) -> u32 {
        self.revision
    }

    fn changed(&mut self) {
        self.revision = self.revision.wrapping_add(1);
    }

    pub fn to_text(&self) -> String {
        let mut out = String::from("inventory 1\n");
        for (index, stack) in self.slots.iter().enumerate() {
            let Some(id) = stack.item.filter(|_| stack.count > 0) else {
                continue;
            };
            let def = self.catalog.item(id);
            write!(out, "{index} {} {}", def.classname, stack.count).unwrap();
            if def.is_weapon() {
                stack.weapon.write_fields(&mut out);
            } else if stack.spent > 0 {
                write!(out, " {}", stack.spent).unwrap();
            }
            out.push('\n');
        }
        out
    }

    /// Replace the contents only after recognizing the save format.
    /// Older saves may put weapons in item boxes or refer to slots that no longer exist.
    pub fn load_text(&mut self, text: &str) -> bool {
        let mut lines = text.split('\n').map(|line| {
            line.split('\r')
                .next()
                .unwrap_or("")
                .split([' ', '\t'])
                .filter(|s| !s.is_empty())
        });
        let Some(mut header) = lines.find(|line| line.clone().next().is_some()) else {
            return false;
        };
        if header.next() != Some("inventory") || header.next() != Some("1") {
            return false;
        }

        let mut loaded = vec![Stack::default(); self.slots.len()];
        let mut misplaced = Vec::new();
        for mut fields in lines {
            let (Some(index), Some(id), Some(count)) = (
                fields.next().and_then(parse_int),
                fields.next().and_then(|name| self.catalog.find_item(name)),
                fields.next().and_then(parse_int).filter(|&count| count > 0),
            ) else {
                continue;
            };
            let def = self.catalog.item(id);
            let mut stack = Stack::new(id, count.min(def.max_stack));
            if def.is_weapon() {
                stack.weapon = WeaponState::read_fields(&mut fields);
            } else if let Some(spent) = fields.next().and_then(parse_int).filter(|&spent| spent > 0)
            {
                stack.add_spent(i64::from(spent), def.fuel);
                if stack.is_empty() {
                    continue;
                }
            }
            let boxes = self.boxes_for(id);
            if let Ok(index) = usize::try_from(index)
                && boxes.contains(&index)
                && loaded[index].is_empty()
            {
                loaded[index] = stack;
            } else {
                misplaced.push((boxes, stack));
            }
        }
        for (mut boxes, stack) in misplaced {
            if let Some(index) = boxes.find(|&index| loaded[index].is_empty()) {
                loaded[index] = stack;
            }
        }
        self.slots = loaded;
        self.changed();
        true
    }
}

fn parse_int(text: &str) -> Option<i32> {
    // The legacy decoder used from_chars, which accepts a leading minus only.
    if text.starts_with('+') {
        None
    } else {
        text.parse().ok()
    }
}
