use std::{
    collections::HashMap,
    f32::consts::{FRAC_PI_2, PI, TAU},
    ffi::CString,
    time::{Duration, Instant},
};

use inventory_mod::items::AmmoKind;
use windows_sys::Win32::Foundation::COLORREF;

use crate::{
    client::Client,
    hud::{WHITE, rect},
    overlay::{Canvas, Font},
};

const GREEN: COLORREF = 0xff39_d68e;
const HOLD: Duration = Duration::from_millis(500);

#[derive(Clone, Eq, PartialEq)]
pub struct OrderKey {
    class: String,
    icon: String,
    occurrence: usize,
}

struct Entry {
    label: String,
    icon: String,
    detail: String,
    active: bool,
    color: COLORREF,
    weight: f32,
    lit: f32,
    command: EntryCommand,
}

#[derive(Clone, Eq, PartialEq)]
enum EntryCommand {
    Native(String),
    Stored(usize),
    Ammo(AmmoKind, i32),
}

pub struct Wheel {
    entries: Vec<Entry>,
    weight: f32,
    limit: f32,
    pointer: (f32, f32),
    pointer_angle: Option<f32>,
    dead: f32,
    opened: Instant,
    last_draw: Instant,
    right: Option<(Instant, AmmoKind, i32)>,
}

impl Wheel {
    pub fn new(client: &Client, order: &mut Vec<OrderKey>, height: i32) -> Option<Self> {
        let state = client.snapshot.as_ref()?;
        let now = Instant::now();
        let mut wheel = Self {
            entries: Vec::new(),
            weight: state.weight.unwrap_or_default(),
            limit: state.limits.max_weight,
            pointer: (0.0, 0.0),
            pointer_angle: None,
            dead: height as f32 * 0.11 * 0.3,
            opened: now,
            last_draw: now,
            right: None,
        };
        wheel.refresh(client, order);
        Some(wheel)
    }

    pub fn refresh(&mut self, client: &Client, order: &mut Vec<OrderKey>) {
        let Some(state) = &client.snapshot else {
            return;
        };
        self.weight = state.weight.unwrap_or_default();
        self.limit = state.limits.max_weight;
        let mut entries = Vec::new();
        let (items, weapons) = state.slots.split_at(state.limits.slots);
        for (index, weapon) in state.native.iter().enumerate() {
            if weapon.classname.is_empty()
                || (index >= 2
                    && items.iter().flatten().any(|item| {
                        item.held && client.catalog.item(item.item).classname == weapon.classname
                    }))
            {
                continue;
            }
            let known = client
                .catalog
                .find_item(&weapon.classname)
                .map(|id| client.catalog.item(id));
            let (label, icon) = if weapon.melee.is_empty() {
                (
                    known
                        .map_or(weapon.classname.as_str(), |def| def.name)
                        .to_owned(),
                    weapon.classname.clone(),
                )
            } else {
                (melee_title(&weapon.melee), weapon.melee.clone())
            };
            let detail = client
                .ammo
                .filter(|ammo| weapon.active && ammo.clip >= 0)
                .and_then(|ammo| Some(format!("{} / {}", ammo.clip, ammo.reserve?)))
                .unwrap_or_default();
            entries.push((
                false,
                weapon.classname.clone(),
                Entry {
                    label,
                    icon,
                    detail,
                    active: weapon.active,
                    color: WHITE,
                    weight: client.catalog.weapon_weight(&weapon.classname),
                    lit: 0.0,
                    command: EntryCommand::Native(weapon.classname.clone()),
                },
            ));
        }
        let ordered_slots = weapons
            .iter()
            .enumerate()
            .map(|(i, slot)| (state.limits.slots + i, slot))
            .chain(items.iter().enumerate());
        for (index, slot) in ordered_slots {
            let Some(stack) = slot else {
                continue;
            };
            let def = client.catalog.item(stack.item);
            let count = stack.count + i32::from(stack.held);
            let active = stack.held
                && state
                    .native
                    .iter()
                    .any(|weapon| weapon.active && weapon.classname == def.classname);
            let mut label = def.name.to_owned();
            let mut icon = def.classname.to_owned();
            let mut detail = String::new();
            if let Some(weapon) = &stack.weapon {
                if !weapon.melee.is_empty() {
                    label = melee_title(&weapon.melee);
                    icon.clone_from(&weapon.melee);
                } else if weapon.clip >= 0 {
                    detail = weapon.clip.to_string();
                    if let Some(reserve) = def
                        .reserve_kind()
                        .and_then(|kind| client.reserve.map(|ammo| ammo[kind.definition().index]))
                    {
                        detail = if weapon.ammo > 0 {
                            format!("{} / {reserve} +{}", weapon.clip, weapon.ammo)
                        } else {
                            format!("{} / {reserve}", weapon.clip)
                        };
                    }
                }
            } else if let Some(fuel) = stack.fuel.filter(|_| stack.count > 0) {
                let remaining = stack.count * fuel.full - fuel.spent;
                detail = if count > 1 {
                    format!("x{count}  fuel {remaining}")
                } else {
                    format!("fuel {remaining} / {}", fuel.full)
                };
            } else if count > 1 {
                detail = format!("x{count}");
            }
            entries.push((
                !def.is_weapon(),
                def.classname.to_owned(),
                Entry {
                    label,
                    icon,
                    detail,
                    active,
                    color: WHITE,
                    weight: def.weight * count as f32,
                    lit: 0.0,
                    command: EntryCommand::Stored(index),
                },
            ));
        }
        let mut seen = HashMap::new();
        let mut ranked = Vec::with_capacity(entries.len());
        for (item, class, entry) in entries {
            let occurrence = seen.entry((class.clone(), entry.icon.clone())).or_insert(0);
            let key = OrderKey {
                class,
                icon: entry.icon.clone(),
                occurrence: *occurrence,
            };
            *occurrence += 1;
            let rank = order.iter().position(|old| *old == key).unwrap_or_else(|| {
                let position = order.len();
                order.push(key);
                position
            });
            ranked.push(((item, rank), entry));
        }
        ranked.sort_by_key(|(rank, _)| *rank);
        let mut entries: Vec<_> = ranked.into_iter().map(|(_, entry)| entry).collect();
        if let Some(reserve) = client.reserve {
            for kind in AmmoKind::ALL {
                let def = kind.definition();
                let quantity = reserve[def.index];
                if quantity <= 0 {
                    continue;
                }
                let [_, red, green, blue] = def.color.to_be_bytes();
                let weight = client.catalog.round_weight(kind) * quantity as f32;
                entries.push(Entry {
                    label: def.title.to_owned(),
                    icon: String::new(),
                    detail: format!("{quantity} ({weight:.1})"),
                    active: false,
                    color: u32::from_le_bytes([red, green, blue, 230]),
                    weight,
                    lit: 0.0,
                    command: EntryCommand::Ammo(kind, quantity),
                });
            }
        }
        for entry in &mut entries {
            if let Some(old) = self.entries.iter().find(|old| old.icon == entry.icon && old.label == entry.label
                && matches!((&old.command, &entry.command), (EntryCommand::Ammo(a, _), EntryCommand::Ammo(b, _)) if a == b)
                    || old.icon == entry.icon && old.command == entry.command)
            {
                entry.lit = old.lit;
            }
        }
        self.entries = entries;
    }

    pub fn point(&mut self, x: i32, y: i32, width: i32, height: i32) -> bool {
        let previous = self.selected();
        self.dead = height as f32 * 0.11 * 0.3;
        self.pointer = (
            x as f32 - width as f32 * 0.5,
            y as f32 - height as f32 * 0.5,
        );
        previous != self.selected()
    }

    fn spans(&self) -> impl Iterator<Item = f32> + '_ {
        let n = self.entries.len().max(1) as f32;
        let total = self.entries.iter().map(|e| e.weight.max(0.0)).sum::<f32>();
        self.entries.iter().map(move |entry| {
            if total > 0.0 {
                TAU * (0.4 / n + 0.6 * entry.weight.max(0.0) / total)
            } else {
                TAU / n
            }
        })
    }

    fn selected(&self) -> Option<usize> {
        let (x, y) = self.pointer;
        if x.hypot(y) < self.dead {
            return None;
        }
        let angle = (y.atan2(x) + FRAC_PI_2).rem_euclid(TAU);
        let mut end = 0.0;
        self.spans().position(|span| {
            end += span;
            angle < end
        })
    }

    pub fn take(&self) -> Option<(CString, Option<usize>)> {
        match &self.entries.get(self.selected()?)?.command {
            EntryCommand::Native(classname) => {
                Some((CString::new(format!("inv_select {classname}")).ok()?, None))
            }
            EntryCommand::Stored(index) => {
                Some((CString::new(format!("inv_use {index}")).ok()?, Some(*index)))
            }
            EntryCommand::Ammo(..) => None,
        }
    }

    pub fn press_drop(&mut self) -> Option<CString> {
        match &self.entries.get(self.selected()?)?.command {
            EntryCommand::Native(classname) => {
                CString::new(format!("inv_dropweapon {classname}")).ok()
            }
            EntryCommand::Stored(index) => CString::new(format!("inv_drop {index} 1")).ok(),
            EntryCommand::Ammo(kind, count) => {
                self.right = Some((Instant::now(), *kind, *count));
                None
            }
        }
    }

    pub fn release_drop(&mut self) -> Option<CString> {
        let (at, kind, count) = self.right.take()?;
        let count = if at.elapsed() >= HOLD {
            0
        } else {
            kind.definition().magazine.min(count)
        };
        CString::new(format!("inv_dropammo {} {count}", kind.name())).ok()
    }

    pub fn tick(&mut self) -> Option<CString> {
        if self.right.is_some_and(|(at, _, _)| at.elapsed() >= HOLD) {
            self.release_drop()
        } else {
            None
        }
    }

    pub fn draw(&mut self, canvas: &mut impl Canvas, width: i32, height: i32) {
        let now = Instant::now();
        let dt = (now - self.last_draw).as_secs_f32().min(0.1);
        self.last_draw = now;
        let open = ((now - self.opened).as_secs_f32() / 0.14).clamp(0.0, 1.0);
        let alpha = 1.0 - (1.0 - open).powi(3);
        if alpha <= 0.001 {
            return;
        }
        let grow = 0.86 + 0.14 * alpha;
        let unit = height as f32 / 1080.0;
        let c = (width as f32 * 0.5, height as f32 * 0.5);
        let inner = height as f32 * 0.11 * grow;
        let outer = height as f32 * 0.27 * grow;
        let selected = self.selected();
        let spans: Vec<_> = self.spans().collect();
        for (index, entry) in self.entries.iter_mut().enumerate() {
            entry.lit += (f32::from(index == selected.unwrap_or(usize::MAX)) - entry.lit)
                * (1.0 - (-22.0 * dt).exp());
        }
        if selected.is_some() {
            let target = self.pointer.1.atan2(self.pointer.0);
            let previous = self.pointer_angle.unwrap_or(target);
            let delta = (target - previous + PI).rem_euclid(TAU) - PI;
            self.pointer_angle = Some(previous + delta * (1.0 - (-30.0 * dt).exp()));
        }
        disc(
            canvas,
            c,
            outer + height as f32 * 0.004,
            fade(0x5a00_0000, alpha),
        );
        let mut start = -FRAC_PI_2;
        for (entry, span) in self.entries.iter().zip(spans) {
            let gap = if self.entries.len() > 1 {
                0.012_f32.min(span * 0.4)
            } else {
                0.0
            };
            let a0 = start + gap;
            let a1 = start + span - gap;
            start += span;
            let mid = (a0 + a1) * 0.5;
            let sc = point(c, mid, 14.0 * unit * entry.lit);
            let r1 = outer + 10.0 * unit * entry.lit;
            ring(
                canvas,
                sc,
                inner,
                r1,
                a0,
                a1,
                fade(mix(0x9600_0000, 0xc81a_5c3c, entry.lit), alpha),
            );
            ring(
                canvas,
                sc,
                r1 - 4.0 * unit,
                r1,
                a0,
                a1,
                fade(GREEN, alpha * entry.lit),
            );
            if entry.active {
                ring(
                    canvas,
                    sc,
                    inner,
                    inner + 4.0 * unit,
                    a0,
                    a1,
                    fade(GREEN, alpha * 0.8),
                );
            }
            let rm = (inner + r1) * 0.5;
            let at = point(sc, mid, rm);
            let chord = ((r1 - inner) * 0.8).min(2.0 * rm * (span.min(PI) * 0.5).sin() * 0.8);
            let zoom = 1.0 + 0.15 * entry.lit;
            let half_w = chord * 0.5 * zoom;
            let half_h = chord.min((r1 - inner) * 0.5) * 0.45 * zoom;
            let ink = if entry.active {
                GREEN
            } else {
                mix(0xffd7_d7d7, WHITE, entry.lit)
            };
            if matches!(entry.command, EntryCommand::Ammo(..)) {
                let bw = half_w.min(half_h * 1.6);
                let bh = half_h.min(bw * 0.7);
                canvas.rect(
                    centered_rect(at, bw + 2.0 * unit + 1.0, bh + 2.0 * unit + 1.0),
                    fade(0xc800_0000, alpha),
                );
                canvas.rect(centered_rect(at, bw, bh), fade(entry.color, alpha));
            } else if !canvas.icon(
                &entry.icon,
                centered_rect(at, half_w, half_h),
                fade(ink, alpha),
            ) {
                line(canvas, Font::Text, at, &entry.label, fade(ink, alpha), true);
            }
            if !entry.detail.is_empty() {
                line(
                    canvas,
                    Font::Ammo,
                    (at.0, at.1 + half_h + 2.0 * unit),
                    &entry.detail,
                    fade(mix(0xdcc8_c8c8, WHITE, entry.lit), alpha),
                    false,
                );
            }
        }
        if self.limit > 0.0 {
            let inner = outer + height as f32 * 0.006;
            let outer = inner + height as f32 * 0.005;
            let fraction = (self.weight / self.limit).clamp(0.0, 1.0);
            ring(
                canvas,
                c,
                inner,
                outer,
                -FRAC_PI_2,
                TAU - FRAC_PI_2,
                fade(0xaa6e_6e6e, alpha),
            );
            let color = if fraction < 0.5 {
                mix(0xe63c_c846, 0xe628_d2e6, fraction * 2.0)
            } else {
                mix(0xe628_d2e6, 0xe623_28c8, fraction * 2.0 - 1.0)
            };
            ring(
                canvas,
                c,
                inner,
                outer,
                -FRAC_PI_2,
                TAU * fraction - FRAC_PI_2,
                fade(color, alpha),
            );
        }
        disc(canvas, c, inner * 0.95, fade(0xaa00_0000, alpha));
        if let Some(angle) = self.pointer_angle.filter(|_| selected.is_some()) {
            let base = inner * 0.95 - 9.0 * unit;
            canvas.polygon(
                &[
                    point(c, angle - 0.09, base),
                    point(c, angle, inner + unit),
                    point(c, angle + 0.09, base),
                ],
                fade(GREEN, alpha),
            );
        }
        if let Some((at, _, _)) = self.right {
            let progress = (at.elapsed().as_secs_f32() / HOLD.as_secs_f32()).clamp(0.0, 1.0);
            ring(
                canvas,
                c,
                inner * 0.95 - 5.0 * unit,
                inner * 0.95,
                -FRAC_PI_2,
                TAU * progress - FRAC_PI_2,
                0xe64b_5af0,
            );
        }
        self.center_labels(canvas, c, inner, unit, alpha, selected);
        crate::hud::pointer(
            canvas,
            (c.0 + self.pointer.0, c.1 + self.pointer.1),
            (height as f32 / 900.0).max(1.0),
        );
    }

    fn center_labels(
        &self,
        canvas: &mut impl Canvas,
        at: (f32, f32),
        inner: f32,
        unit: f32,
        alpha: f32,
        selected: Option<usize>,
    ) {
        let picked = selected.and_then(|i| self.entries.get(i));
        let mut lines = Vec::new();
        if let Some(item) = picked {
            let font = if canvas.measure(Font::Title, &item.label).0 as f32 > inner * 1.8 {
                Font::Text
            } else {
                Font::Title
            };
            lines.push((font, item.label.clone(), WHITE));
            if !item.detail.is_empty() {
                lines.push((Font::Ammo, item.detail.clone(), 0xffe6_e6e6));
            }
        } else {
            let title = if self.entries.is_empty() {
                "Nothing carried"
            } else {
                "Inventory"
            };
            lines.push((Font::Title, title.to_owned(), WHITE));
        }
        if self.limit > 0.0 {
            let weight = format!("{:.1} / {:.1}", self.weight, self.limit);
            lines.push((
                Font::Text,
                weight,
                if self.weight >= self.limit - 0.001 {
                    0xff4b_5af0
                } else {
                    0xffbe_bebe
                },
            ));
        }
        let hint = match picked.map(|entry| &entry.command) {
            Some(EntryCommand::Ammo(kind, count)) => format!(
                "right click: drop {}   hold: all",
                kind.definition().magazine.min(*count)
            ),
            Some(_) => "release: take   right click: drop".to_owned(),
            None if !self.entries.is_empty() => "point at an item".to_owned(),
            None => String::new(),
        };
        if !hint.is_empty() {
            lines.push((Font::Hint, hint, 0xff96_9696));
        }
        let spacing = 3.0 * unit;
        let total = lines
            .iter()
            .map(|(font, text, _)| canvas.measure(*font, text).1 as f32 + spacing)
            .sum::<f32>()
            - spacing;
        let mut y = at.1 - total * 0.5;
        for (font, text, color) in lines {
            line(canvas, font, (at.0, y), &text, fade(color, alpha), false);
            y += canvas.measure(font, &text).1 as f32 + spacing;
        }
    }
}

fn fade(color: COLORREF, opacity: f32) -> COLORREF {
    (color & 0x00ff_ffff) | ((((color >> 24) as f32 * opacity) as u32) << 24)
}

fn mix(a: COLORREF, b: COLORREF, amount: f32) -> COLORREF {
    let a = a.to_le_bytes();
    let b = b.to_le_bytes();
    u32::from_le_bytes(std::array::from_fn(|i| {
        (f32::from(a[i]) + (f32::from(b[i]) - f32::from(a[i])) * amount) as u8
    }))
}

fn point(center: (f32, f32), angle: f32, radius: f32) -> (f32, f32) {
    let (sin, cos) = angle.sin_cos();
    (center.0 + radius * cos, center.1 + radius * sin)
}

fn disc(canvas: &mut impl Canvas, center: (f32, f32), radius: f32, color: COLORREF) {
    ring(canvas, center, 0.0, radius, 0.0, TAU, color);
}

pub fn ring(
    canvas: &mut impl Canvas,
    center: (f32, f32),
    inner: f32,
    outer: f32,
    start: f32,
    end: f32,
    color: COLORREF,
) {
    if end <= start || color >> 24 == 0 {
        return;
    }
    let steps = ((end - start) * 20.0).ceil().clamp(2.0, 128.0) as i32;
    for step in 0..steps {
        let a = start + (end - start) * step as f32 / steps as f32;
        let b = start + (end - start) * (step + 1) as f32 / steps as f32;
        canvas.polygon(
            &[
                point(center, a, inner),
                point(center, a, outer),
                point(center, b, outer),
                point(center, b, inner),
            ],
            color,
        );
    }
}

fn line(
    canvas: &mut impl Canvas,
    font: Font,
    at: (f32, f32),
    text: &str,
    color: COLORREF,
    vertically_centered: bool,
) {
    if color >> 24 == 0 {
        return;
    }
    let (width, height) = canvas.measure(font, text);
    let y = if vertically_centered {
        at.1 as i32 - height / 2
    } else {
        at.1 as i32
    };
    canvas.styled_text(font, (at.0 as i32 - width / 2, y), text, color);
}

fn centered_rect(at: (f32, f32), half_w: f32, half_h: f32) -> windows_sys::Win32::Foundation::RECT {
    rect(
        (at.0 - half_w) as i32,
        (at.1 - half_h) as i32,
        (2.0 * half_w).max(1.0) as i32,
        (2.0 * half_h).max(1.0) as i32,
    )
}

fn melee_title(kind: &str) -> String {
    kind.split('_')
        .map(|part| {
            let mut chars = part.chars();
            let mut result = String::new();
            if let Some(first) = chars.next() {
                result.extend(first.to_uppercase());
            }
            result.extend(chars);
            result
        })
        .collect::<Vec<_>>()
        .join(" ")
}
