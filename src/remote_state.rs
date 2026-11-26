use std::fmt::Write;

use crate::{
    inventory::{Inventory, Limits, WeaponState},
    items::{Catalog, ItemId},
};

pub const NATIVE_SLOTS: usize = 6;
pub const STATE_MARKER: &str = "@inv@";

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct NativeWeapon {
    pub classname: String,
    pub active: bool,
    pub melee: String,
    pub wear: Option<i32>,
}

/// Condition carried by a native weapon across a level change, when entity handles change.
#[derive(Clone, Debug)]
pub struct CarriedCondition {
    pub slot: usize,
    pub classname: String,
    pub melee: String,
    pub wear: i32,
    pub spent: i32,
}

pub fn reconcile_native_inventory(
    inventory: &mut Inventory,
    native: &[NativeWeapon],
    rounds_weight: f32,
) {
    for index in 0..inventory.limits().slots {
        let stack = &inventory.slots()[index];
        if let Some(id) = stack.item.filter(|_| stack.held) {
            let name = inventory.catalog().item(id).classname;
            let carried = native.iter().any(|weapon| weapon.classname == name);
            if !carried || inventory.held_slot(id) != Some(index) {
                inventory.clear_held(index);
            }
        }
    }

    let mut weight = 0.0;
    for weapon in native {
        let catalog = inventory.catalog();
        if let Some(id) = catalog.find_item(&weapon.classname) {
            let def = catalog.item(id);
            if def.is_weapon() {
                weight += def.weight;
            } else if inventory.held_slot(id).is_none() {
                if let Some(index) = inventory.home_for(id) {
                    inventory.set_held(index, id);
                } else {
                    weight += def.weight;
                }
            }
        }
    }
    inventory.set_weapons_weight(weight);
    inventory.set_ammo_weight(rounds_weight);
}

/// The update sent through the Source server's ClientPrintf.
pub fn encode_remote_state(
    inventory: &Inventory,
    native: &[NativeWeapon],
    message: &str,
) -> String {
    let limits = inventory.limits();
    let mut line = format!(
        "{STATE_MARKER} {} {:.2} {:.2} {}|",
        limits.slots,
        limits.max_weight,
        inventory.weight(),
        limits.weapon_slots
    );

    for (index, stack) in inventory.slots().iter().enumerate() {
        let Some(id) = stack.item.filter(|_| !stack.is_empty()) else {
            continue;
        };
        let def = inventory.catalog().item(id);
        write!(
            line,
            "{index} {} {} {}",
            def.classname,
            stack.count,
            u8::from(stack.held)
        )
        .unwrap();
        if def.is_weapon() {
            stack.weapon.write_fields(&mut line);
        } else if def.fuel > 0 {
            write!(line, " {} {}", stack.spent, def.fuel).unwrap();
        }
        line.push(',');
    }

    line.push('|');
    for (index, weapon) in native.iter().enumerate() {
        if index > 0 {
            line.push(',');
        }
        if weapon.classname.is_empty() {
            line.push('-');
        } else {
            if weapon.active {
                line.push('*');
            }
            line.push_str(&weapon.classname);
            if !weapon.melee.is_empty() {
                write!(line, ":{}", weapon.melee).unwrap();
                if let Some(wear) = weapon.wear.filter(|&wear| wear >= 0) {
                    write!(line, ":{wear}").unwrap();
                }
            }
        }
    }
    writeln!(line, "|{message}").unwrap();
    line
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RemoteStack {
    pub item: ItemId,
    pub count: i32,
    pub held: bool,
    pub weapon: Option<WeaponState>,
    pub fuel: Option<Fuel>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Fuel {
    pub spent: i32,
    pub full: i32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct RemoteState {
    pub limits: Limits,
    pub weight: Option<f32>,
    pub slots: Vec<Option<RemoteStack>>,
    pub native: [NativeWeapon; NATIVE_SLOTS],
    pub message: String,
}

impl RemoteState {
    /// Read the host's snapshot with the host's counts and fuel. Local rule
    /// overrides cannot alter a remote player's inventory.
    pub fn decode(line: &str, catalog: &Catalog) -> Option<Self> {
        let mut parts = line
            .strip_prefix(STATE_MARKER)?
            .trim_end_matches(['\n', '\r'])
            .splitn(4, '|');
        let mut header = parts.next()?.split_ascii_whitespace();
        let slots: usize = header.next()?.parse().ok()?;
        if !(1..=64).contains(&slots) {
            return None;
        }

        let parse_weight = |field: Option<&str>| {
            field
                .and_then(|text| text.parse::<f32>().ok())
                .filter(|n| n.is_finite() && *n >= 0.0)
        };
        let max_weight = parse_weight(header.next()).unwrap_or(0.0);
        let weight = parse_weight(header.next());
        let weapon_slots = header
            .next()
            .and_then(|s| s.parse::<usize>().ok())
            .unwrap_or(0)
            .min(16);
        let mut stacks = vec![None; slots + weapon_slots];
        for entry in parts.next()?.split(',') {
            if let Some((index, stack)) = decode_stack(entry, catalog)
                && let Some(slot) = stacks.get_mut(index)
                && (index >= slots) == stack.weapon.is_some()
                && slot.is_none()
            {
                *slot = Some(stack);
            }
        }

        let mut native = std::array::from_fn(|_| NativeWeapon::default());
        for (out, entry) in native.iter_mut().zip(parts.next()?.split(',')) {
            let active = entry.starts_with('*');
            let mut fields = entry.trim_start_matches('*').splitn(3, ':');
            let classname = fields.next().unwrap_or("");
            if !classname.is_empty() && classname != "-" {
                *out = NativeWeapon {
                    classname: classname.to_owned(),
                    active,
                    melee: fields.next().unwrap_or("").to_owned(),
                    wear: fields
                        .next()
                        .and_then(|field| field.parse::<i32>().ok())
                        .filter(|&wear| wear >= 0),
                };
            }
        }

        Some(Self {
            limits: Limits {
                slots,
                max_weight,
                weapon_slots,
            },
            weight,
            slots: stacks,
            native,
            message: parts.next()?.to_owned(),
        })
    }
}

fn decode_stack(entry: &str, catalog: &Catalog) -> Option<(usize, RemoteStack)> {
    let mut fields = entry.split_ascii_whitespace();
    let index: usize = fields.next()?.parse().ok()?;
    let item = catalog.find_item(fields.next()?)?;
    let count: i32 = fields.next()?.parse().ok()?;
    let held = fields.next()?.parse::<i32>().ok()? != 0;
    let (weapon, fuel) = if catalog.item(item).is_weapon() {
        (Some(WeaponState::read_fields(&mut fields)), None)
    } else {
        if count < 0 || (count == 0 && !held) {
            return None;
        }
        let mut number = || fields.next().and_then(|s| s.parse::<i32>().ok());
        let fuel = number().zip(number()).and_then(|(spent, full)| {
            (full > 0).then(|| Fuel {
                spent: spent.clamp(0, full - 1),
                full,
            })
        });
        (None, fuel)
    };
    Some((
        index,
        RemoteStack {
            item,
            count: if weapon.is_some() { 1 } else { count },
            held: weapon.is_none() && held,
            weapon,
            fuel,
        },
    ))
}
