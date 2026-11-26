use inventory_mod::{
    items::{Catalog, ItemDef},
    melee::WEAR_BROKEN,
    remote_state::{Fuel, NativeWeapon, RemoteStack, RemoteState},
};
use windows_sys::Win32::Foundation::{COLORREF, RECT};

use crate::{
    client::Client,
    overlay::{Canvas, Font},
};

pub const BG: COLORREF = 0xff42_3323;
pub const BG_ALT: COLORREF = 0xff4c_3b29;
pub const BG_FOCUSED: COLORREF = 0xff77_5836;
pub const BORDER: COLORREF = 0xff96_960f;
pub const BORDER_FOCUSED: COLORREF = 0xff8f_e138;
pub const BORDER_SELECTED: COLORREF = 0xff0f_43c6;
pub const TEXT: COLORREF = 0xffe0_d2c3;
pub const TEXT_ALT: COLORREF = 0xffb4_e1ff;
pub const TEXT_SELECTED: COLORREF = 0xff00_7fff;
pub const WHITE: COLORREF = 0xffff_ffff;
const HELD: COLORREF = 0xff59_d973;
const IN_HANDS: COLORREF = 0xff39_d68e;

pub fn rect(x: i32, y: i32, width: i32, height: i32) -> RECT {
    RECT {
        left: x,
        top: y,
        right: x + width,
        bottom: y + height,
    }
}

pub fn inset(bounds: RECT, x: i32, y: i32) -> RECT {
    RECT {
        left: bounds.left + x,
        top: bounds.top + y,
        right: bounds.right - x,
        bottom: bounds.bottom - y,
    }
}

pub fn center(canvas: &mut impl Canvas, bounds: RECT, text: &str, color: COLORREF) {
    let (wide, tall) = canvas.text_size(text);
    canvas.text(
        (
            (bounds.left + bounds.right - wide) / 2,
            (bounds.top + bounds.bottom - tall) / 2,
        ),
        text,
        color,
    );
}

pub fn pointer(canvas: &mut impl Canvas, at: (f32, f32), scale: f32) {
    let shape = |canvas: &mut _, (dx, dy), color| {
        let p = |x, y| (at.0 + x * scale + dx, at.1 + y * scale + dy);
        Canvas::polygon(canvas, &[p(0.0, 0.0), p(0.0, 16.0), p(4.0, 12.0)], color);
        Canvas::polygon(canvas, &[p(0.0, 0.0), p(4.0, 12.0), p(11.0, 11.0)], color);
        Canvas::polygon(
            canvas,
            &[p(4.0, 11.0), p(6.5, 11.0), p(9.5, 17.0), p(7.0, 18.0)],
            color,
        );
    };
    for offset in [(-1.0, 0.0), (1.0, 0.0), (0.0, -1.0), (0.0, 1.0)] {
        shape(canvas, offset, 0xe600_0000);
    }
    shape(canvas, (0.0, 0.0), WHITE);
}

pub fn outline(canvas: &mut impl Canvas, r: RECT, color: COLORREF, thick: i32) {
    let w = r.right - r.left;
    let h = r.bottom - r.top;
    canvas.rect(rect(r.left, r.top, w, thick), color);
    canvas.rect(rect(r.left, r.bottom - thick, w, thick), color);
    canvas.rect(rect(r.left, r.top + thick, thick, h - 2 * thick), color);
    canvas.rect(
        rect(r.right - thick, r.top + thick, thick, h - 2 * thick),
        color,
    );
}

pub fn item_icon(
    canvas: &mut impl Canvas,
    bounds: RECT,
    stack: &RemoteStack,
    def: &ItemDef,
    color: COLORREF,
) {
    let name = stack
        .weapon
        .as_ref()
        .map_or("", |weapon| weapon.melee.as_str());
    let icon = if name.is_empty() { def.classname } else { name };
    if !canvas.icon(icon, bounds, color) {
        center(canvas, bounds, def.tag, color);
    }
}

fn bar(canvas: &mut impl Canvas, r: RECT, fraction: f32, color: COLORREF, down: i32) {
    let left = r.left + 3;
    let width = (r.right - left - 3).max(0);
    let top = r.bottom - down - 5;
    canvas.rect(rect(left, top, width, 3), 0x9900_0000);
    canvas.rect(
        rect(left, top, (width as f32 * fraction).floor() as i32, 3),
        color,
    );
}

fn wear(canvas: &mut impl Canvas, r: RECT, wear: i32) {
    if wear <= 0 {
        return;
    }
    let left = (1.0 - wear as f32 / WEAR_BROKEN as f32).clamp(0.0, 1.0);
    let color = if left > 0.5 {
        HELD
    } else if left > 0.2 {
        0xff40_ccf2
    } else {
        0xff33_40e6
    };
    bar(canvas, r, left, color, 0);
}

fn fuel(canvas: &mut impl Canvas, bounds: RECT, stack: &RemoteStack) {
    if let Some(Fuel { spent, full }) = stack.fuel.filter(|fuel| fuel.spent > 0)
        && stack.count > 0
    {
        bar(
            canvas,
            bounds,
            1.0 - spent as f32 / full as f32,
            0xff28_aaf0,
            if stack.held { 4 } else { 0 },
        );
    }
}

fn count(canvas: &mut impl Canvas, r: RECT, stack: &RemoteStack, color: COLORREF) {
    let count = stack.count + i32::from(stack.held);
    if count > 1 {
        let count = count.to_string();
        let (w, h) = canvas.text_size(&count);
        canvas.text((r.right - w - 3, r.bottom - h), &count, color);
    }
}

pub fn slot(
    canvas: &mut impl Canvas,
    r: RECT,
    catalog: &Catalog,
    stack: Option<&RemoteStack>,
    selected: bool,
    hovered: bool,
    key: Option<&str>,
) {
    canvas.rect(r, if hovered { BG_FOCUSED } else { BG_ALT });
    if let Some(stack) = stack {
        let def = catalog.item(stack.item);
        let pad = (r.bottom - r.top) / 6;
        let [red, green, blue, _] = def.color.to_le_bytes();
        let shade = |x| (f32::from(x) * 0.4) as u8;
        let field = inset(r, pad, pad);
        canvas.rect(
            field,
            u32::from_le_bytes([shade(red), shade(green), shade(blue), 255]),
        );
        outline(canvas, field, def.color, 1);
        item_icon(canvas, inset(r, 2, 2), stack, def, WHITE);
        if let Some(state) = stack
            .weapon
            .as_ref()
            .filter(|state| !state.melee.is_empty())
        {
            wear(canvas, r, state.wear);
        }
        fuel(canvas, r, stack);
        if stack.held {
            canvas.rect(
                rect(r.left + 3, r.bottom - 4, r.right - r.left - 6, 2),
                HELD,
            );
        }
        count(canvas, r, stack, TEXT);
    }
    if let Some(key) = key {
        canvas.styled_text(Font::Hint, (r.left + 3, r.top + 1), key, TEXT_ALT);
    }
    outline(
        canvas,
        r,
        if selected { BORDER_SELECTED } else { BORDER },
        1,
    );
}

pub fn paint(canvas: &mut impl Canvas, client: &Client, width: i32, height: i32) {
    if !client.playing() || !client.settings.hud.show || !client.settings.hud.view.shows_slots() {
        return;
    }
    let Some(state) = client.snapshot.as_ref() else {
        return;
    };
    if client.settings.hud.replace_native_panel {
        hotbar(canvas, client, state, width, height);
    } else {
        column(canvas, client, state, width, height);
    }
}

fn shown(slots: &[Option<RemoteStack>], spare: usize) -> usize {
    let filled = slots.iter().rposition(Option::is_some).map_or(0, |i| i + 1);
    (filled + spare).min(slots.len())
}

fn class_icon(canvas: &mut impl Canvas, weapon: &NativeWeapon, bounds: RECT, catalog: &Catalog) {
    let name = if weapon.melee.is_empty() {
        &weapon.classname
    } else {
        &weapon.melee
    };
    if !name.is_empty() && !canvas.icon(name, bounds, WHITE) {
        let caption = catalog
            .find_item(&weapon.classname)
            .map_or(name.as_str(), |id| catalog.item(id).tag);
        center(canvas, bounds, caption, WHITE);
    }
}

fn hotbar(canvas: &mut impl Canvas, client: &Client, state: &RemoteState, width: i32, height: i32) {
    let scale = client.settings.hud.scale * (height as f32 / 900.0).max(1.0);
    let size = (40.0 * scale) as i32;
    let gap = (4.0 * scale) as i32;
    let native = &state.native[..2];
    let items = &state.slots[..state.limits.slots];
    let equipped = client
        .native_selection
        .filter(|&i| i < native.len() && !native[i].classname.is_empty());
    let n = shown(items, 1);
    let total = (4 + n as i32) * size + (1 + n as i32) * gap;
    let x = (width as f32 * 0.62) as i32 - total / 2;
    let y = height - size - (10.0 * scale) as i32;
    let mut at = x;
    for (i, weapon) in native.iter().enumerate() {
        let r = rect(at, y, size * 2, size);
        canvas.rect(r, BG_ALT);
        if !weapon.classname.is_empty() {
            let field = inset(r, size / 6, size / 6);
            canvas.rect(field, 0xff47_3d38);
            outline(canvas, field, 0xffb3_a399, 1);
            class_icon(canvas, weapon, inset(r, 2, 2), &client.catalog);
            wear(canvas, r, weapon.wear.unwrap_or(0));
        }
        canvas.styled_text(
            Font::Hint,
            (r.left + 3, r.top + 1),
            &(i + 1).to_string(),
            TEXT_ALT,
        );
        outline(
            canvas,
            r,
            if equipped == Some(i) {
                BORDER_SELECTED
            } else {
                BORDER
            },
            1,
        );
        at += 2 * size + gap;
    }
    for (i, stack) in items.iter().take(n).enumerate() {
        let key = (i < 8).then(|| ((i + 3) % 10).to_string());
        slot(
            canvas,
            rect(at, y, size, size),
            &client.catalog,
            stack.as_ref(),
            equipped.is_none() && client.selection == i,
            false,
            key.as_deref(),
        );
        at += size + gap;
    }
    if let Some(ammo) = client
        .ammo
        .filter(|ammo| ammo.clip >= 0 || ammo.reserve.is_some_and(|n| n > 0))
        && state.native.iter().any(|weapon| {
            weapon.active
                && client
                    .catalog
                    .find_item(&weapon.classname)
                    .is_some_and(|id| client.catalog.item(id).is_weapon())
        })
    {
        let caption = match ammo.reserve.filter(|&n| n > 0) {
            Some(n) if ammo.clip >= 0 => format!("{} / {n}", ammo.clip),
            Some(n) => n.to_string(),
            None => ammo.clip.to_string(),
        };
        let (_, line) = canvas.measure(Font::Ammo, &caption);
        canvas.styled_text(
            Font::Ammo,
            (at + 2 * gap, y + (size - line) / 2),
            &caption,
            if ammo.clip == 0 { 0xff4d_59f2 } else { WHITE },
        );
    }
    let mut caption_y = y - 20;
    if let Some((message, _)) = client
        .panel_notice
        .as_ref()
        .filter(|(_, until)| std::time::Instant::now() < *until)
    {
        center(
            canvas,
            rect(x, caption_y, total, 18),
            message,
            TEXT_SELECTED,
        );
        caption_y -= 20;
    }
    let name = equipped
        .and_then(|i| client.catalog.find_item(&native[i].classname))
        .or_else(|| {
            state
                .slots
                .get(client.selection)?
                .as_ref()
                .map(|stack| stack.item)
        });
    if let Some(name) = name {
        center(
            canvas,
            rect(x, caption_y, total, 18),
            client.catalog.item(name).name,
            WHITE,
        );
    }
}

fn column(canvas: &mut impl Canvas, client: &Client, state: &RemoteState, width: i32, height: i32) {
    let (items, weapons) = state.slots.split_at(state.limits.slots);
    let weapon_rows = shown(weapons, 0);
    let items_shown = shown(items, 1);
    let unit = height as f32 / 480.0;
    let size = (23.3 * unit).floor() as i32;
    let pitch = 26.7 * unit;
    let right = width as f32 - 8.3 * unit;
    let base = height as f32 * 0.5 - 22.7 * unit;
    let wide = (53.3 * unit).floor() as i32;
    let pad = (unit * 2.0).floor().max(1.0) as i32;
    let line = (unit * 0.5).max(1.0) as i32;
    for weapon in state.native.iter().take(2) {
        if let Some(damage) = weapon.wear.filter(|_| !weapon.melee.is_empty()) {
            wear(
                canvas,
                rect(right as i32 - wide, 0, wide, base as i32),
                damage,
            );
        } else if matches!(
            weapon.classname.as_str(),
            "weapon_pistol" | "weapon_pistol_magnum"
        ) && let Some(reserve) = client
            .catalog
            .find_item(&weapon.classname)
            .and_then(|id| client.catalog.item(id).reserve_kind())
            .and_then(|kind| client.reserve.map(|ammo| ammo[kind.definition().index]))
        {
            let hint = reserve.to_string();
            let (_, h) = canvas.measure(Font::Hint, &hint);
            canvas.styled_text(
                Font::Hint,
                (
                    right as i32 - wide + pad * 2,
                    (base - 6.7 * unit) as i32 - h - pad,
                ),
                &hint,
                0xffa8_a8a8,
            );
        }
    }
    for (i, stack) in weapons.iter().take(weapon_rows).enumerate() {
        let y = base + i as f32 * pitch;
        let r = rect(
            right as i32 - wide,
            y as i32,
            wide,
            ((y + 20.0 * unit).floor() - y.floor()) as i32,
        );
        panel_cell(canvas, r, stack.is_some(), false, false, line);
        if let Some(stack) = stack {
            let def = client.catalog.item(stack.item);
            let mut image = inset(r, pad, pad);
            if let Some(weapon) = &stack.weapon {
                if weapon.clip >= 0 {
                    image.left = r.left + (wide as f32 * 0.32) as i32;
                    canvas.styled_text(
                        Font::Ammo,
                        (r.left + 2 * pad, r.top + pad),
                        &weapon.clip.to_string(),
                        WHITE,
                    );
                    if let Some(kind) = def.reserve_kind()
                        && let Some(reserve) = client.reserve
                    {
                        let text = reserve[kind.definition().index].to_string();
                        let (_, h) = canvas.measure(Font::Hint, &text);
                        canvas.text((r.left + 2 * pad, r.bottom - h - pad), &text, 0xffa8_a8a8);
                    }
                } else {
                    wear(canvas, r, weapon.wear);
                }
            }
            item_icon(canvas, image, stack, def, WHITE);
        }
    }
    let first_bottom = height as f32 * 0.5 + 54.0 * unit - 2.0 * pitch + weapon_rows as f32 * pitch;
    for (i, stack) in items.iter().take(items_shown).enumerate() {
        let x = (right - (2 - i % 3) as f32 * pitch).floor() as i32;
        let y = (first_bottom + (i / 3) as f32 * pitch).floor() as i32;
        let r = rect(x - size, y - size, size, size);
        let active = stack.as_ref().is_some_and(|stack| {
            stack.held
                && state.native.iter().any(|weapon| {
                    weapon.active && weapon.classname == client.catalog.item(stack.item).classname
                })
        });
        panel_cell(
            canvas,
            r,
            stack.is_some(),
            active,
            client.selection == i,
            line,
        );
        if let Some(stack) = stack {
            let color = if active { IN_HANDS } else { WHITE };
            item_icon(
                canvas,
                inset(r, size / 10, size / 10),
                stack,
                client.catalog.item(stack.item),
                color,
            );
            count(canvas, r, stack, color);
            fuel(canvas, r, stack);
        }
        if i < 8 {
            canvas.styled_text(
                Font::Hint,
                (r.left + 3, r.top + 1),
                &((i + 3) % 10).to_string(),
                0x8cff_ffff,
            );
        }
    }
    if state.limits.max_weight > 0.0 && items_shown > 0 {
        let limit = state.limits.max_weight;
        let weight = state.weight.unwrap_or(0.0);
        let fraction = (weight / limit).clamp(0.0, 1.0);
        let full = weight >= limit - 0.001;
        let weight_label = format!("{weight:.1} / {limit:.1}");
        let left = (right - 2.0 * pitch - 23.3 * unit).floor() as i32;
        let top = (first_bottom + ((items_shown - 1) / 3) as f32 * pitch + pitch - 23.3 * unit)
            .floor() as i32;
        let (_, h) = canvas.text_size(&weight_label);
        let width = right as i32 - left;
        let r = rect(left, top, width, h + 4);
        canvas.rect(r, 0x7800_0000);
        canvas.rect(
            rect(left, top, (width as f32 * fraction).floor() as i32, h + 4),
            if full { 0xaa28_32c8 } else { 0x3cff_ffff },
        );
        outline(
            canvas,
            r,
            if full { 0xe63c_46e6 } else { 0x78ff_ffff },
            line,
        );
        center(canvas, r, &weight_label, 0xe6ff_ffff);
    }
}

fn panel_cell(
    canvas: &mut impl Canvas,
    bounds: RECT,
    filled: bool,
    active: bool,
    selected: bool,
    line: i32,
) {
    canvas.rect(bounds, if filled { 0x7800_0000 } else { 0x3c00_0000 });
    let color = match (active, selected, filled) {
        (true, _, _) => IN_HANDS,
        (_, true, true) => 0xe6ff_ffff,
        (_, true, false) => 0xaaff_ffff,
        (_, _, true) => 0x78ff_ffff,
        _ => 0x32ff_ffff,
    };
    outline(
        canvas,
        bounds,
        color,
        if active || (selected && filled) {
            line * 2
        } else {
            line
        },
    );
}
