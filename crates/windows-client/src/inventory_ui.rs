use std::time::{Duration, Instant};

use inventory_mod::{
    items::{AmmoKind, ItemId},
    remote_state::RemoteStack,
    settings::{Settings, View},
};
use windows_sys::Win32::{
    Foundation::{COLORREF, RECT},
    UI::Input::KeyboardAndMouse::GetDoubleClickTime,
    UI::WindowsAndMessaging::{WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEMOVE, WM_RBUTTONUP},
};

use crate::{
    client::Client,
    hud::{
        self, BG, BG_ALT, BG_FOCUSED, BORDER, BORDER_FOCUSED, TEXT, TEXT_ALT, WHITE, center,
        outline, rect,
    },
    overlay::Canvas,
};

mod settings;
use settings::{Setting, key_field, key_name};

const TITLE: i32 = 15;
const CONTENT: i32 = 33;
const SCROLLBAR: i32 = 15;
const SLOT: i32 = 56;
const COLUMNS: usize = 4;
const DIM_TEXT: COLORREF = 0xffa2_ada5;

#[derive(Clone, Copy, Default, Eq, PartialEq)]
enum Tab {
    #[default]
    Items,
    Debug,
    Settings,
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum Hit {
    Close,
    Tab(Tab),
    Title,
    Resize,
    Scroll(i32),
    ScrollThumb,
    Slot(usize),
    Store,
    Pickup,
    Unload,
    UseSelected,
    Equip(usize),
    Drop(usize, i32),
    Ammo(AmmoKind, i32),
    OpenChoices(Choices),
    Choose(Choices, usize),
    DebugCount(i32),
    Number(Number),
    DebugAdd,
    DebugAddEach,
    DebugClear,
    Infect,
    Screamer,
    PreviewInfection,
    PreviewStamina,
    PreviewExhausted,
    Fog,
    Slider(Slider),
    Bind(usize),
    Settings(Setting, i32),
}

enum Gesture {
    Slot(usize, (i32, i32)),
    Move((i32, i32)),
    Resize((i32, i32)),
    Scroll((i32, i32)),
    Slide(Slider, RECT),
    Control(Hit),
}

pub enum Action {
    Close,
    Send(String),
    Add(ItemId, i32),
    AddEach,
    Clear,
    Screamer,
    Fog(Option<f32>),
    SettingsChanged,
}

struct Popup {
    slot: usize,
    position: (i32, i32),
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum Choices {
    Item,
    View,
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum Slider {
    Infection,
    Stamina,
    Fog,
    Scale,
    Chance,
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum Number {
    Count,
    Limit(Setting),
}

struct NumberEdit {
    number: Number,
    text: String,
    select_all: bool,
}

pub struct Preview {
    active: bool,
    value: f32,
}

impl Preview {
    const fn new(value: f32) -> Self {
        Self {
            active: false,
            value,
        }
    }

    pub fn get(&self) -> Option<f32> {
        self.active.then_some(self.value)
    }

    fn toggle(&mut self) {
        self.active = !self.active;
    }
}

struct Dropdown {
    choices: Choices,
    anchor: RECT,
    first: usize,
    count: usize,
}

const VIEWS: &[(&str, View)] = &[
    ("Slots and wheel", View::Both),
    ("Slots only", View::Slots),
    ("Wheel only", View::Wheel),
];

pub struct InventoryUi {
    tab: Tab,
    size: (i32, i32),
    offset: (i32, i32),
    pointer: Option<(i32, i32)>,
    gesture: Option<Gesture>,
    last_click: Option<(usize, Instant)>,
    popup: Option<Popup>,
    dropdown: Option<Dropdown>,
    hits: Vec<(RECT, Hit)>,
    scroll: i32,
    max_scroll: i32,
    debug_item: usize,
    debug_count: i32,
    rebinding: Option<usize>,
    editing: Option<NumberEdit>,
    pub infection_preview: Preview,
    pub stamina_preview: Preview,
    pub exhausted_preview: bool,
    fog: Preview,
}

impl Default for InventoryUi {
    fn default() -> Self {
        Self {
            tab: Tab::Items,
            size: (300, 360),
            offset: (0, 0),
            pointer: None,
            gesture: None,
            last_click: None,
            popup: None,
            dropdown: None,
            hits: Vec::new(),
            scroll: 0,
            max_scroll: 0,
            debug_item: 0,
            debug_count: 1,
            rebinding: None,
            editing: None,
            infection_preview: Preview::new(0.5),
            stamina_preview: Preview::new(0.3),
            exhausted_preview: false,
            fog: Preview::new(0.5),
        }
    }
}

impl InventoryUi {
    pub fn pointer(&self) -> Option<(i32, i32)> {
        self.pointer
    }

    pub fn bounds(&self, width: i32, height: i32) -> RECT {
        let w = self.size.0.min(width.max(1));
        let h = self.size.1.min(height.max(1));
        let x = (width / 2 - w / 2 + self.offset.0).clamp(0, (width - w).max(0));
        let y = (height * 45 / 100 - h / 2 + self.offset.1).clamp(0, (height - h).max(0));
        rect(x, y, w, h)
    }

    pub fn reset_input(&mut self) {
        self.pointer = None;
        self.gesture = None;
        self.popup = None;
        self.dropdown = None;
        self.last_click = None;
        self.rebinding = None;
        self.editing = None;
    }

    pub fn scroll_by(&mut self, pixels: i32) {
        if let Some(dropdown) = &mut self.dropdown {
            dropdown.first = dropdown
                .first
                .saturating_add_signed(pixels.signum() as isize)
                .min(dropdown.count.saturating_sub(9));
            return;
        }
        self.scroll = self.scroll.saturating_add(pixels).clamp(0, self.max_scroll);
        self.popup = None;
    }

    pub fn rebind(&mut self, key: i32, settings: &mut Settings) -> bool {
        let Some(index) = self.rebinding.take() else {
            return false;
        };
        if key != 0x1b {
            *key_field(&mut settings.keys, index) = key;
        }
        true
    }

    pub fn number_key(&mut self, key: i32, settings: &mut Settings) -> Option<bool> {
        let edit = self.editing.as_mut()?;
        Some(match key {
            13 => self.finish_number(settings),
            27 => {
                self.editing = None;
                false
            }
            8 => {
                if edit.select_all {
                    edit.text.clear();
                } else {
                    edit.text.pop();
                }
                edit.select_all = false;
                false
            }
            _ => false,
        })
    }

    pub fn number_character(&mut self, value: usize) {
        let Some(edit) = &mut self.editing else {
            return;
        };
        let Ok(byte) = u8::try_from(value) else {
            return;
        };
        let decimal = edit.number == Number::Limit(Setting::Weight);
        if !byte.is_ascii_digit() && !(byte == b'.' && decimal) {
            return;
        }
        if edit.select_all {
            edit.text.clear();
            edit.select_all = false;
        }
        if edit.text.len() < 12 && (byte != b'.' || !edit.text.contains('.')) {
            edit.text.push(char::from(byte));
        }
    }

    fn finish_number(&mut self, cfg: &mut Settings) -> bool {
        let Some(edit) = self.editing.take() else {
            return false;
        };
        match edit.number {
            Number::Count => {
                if let Ok(count) = edit.text.parse::<i32>() {
                    self.debug_count = count.clamp(1, 99);
                }
                false
            }
            Number::Limit(setting) => setting.set_number(cfg, &edit.text),
        }
    }

    fn number_label(&self, number: Number, usual: String) -> String {
        self.editing
            .as_ref()
            .filter(|edit| edit.number == number)
            .map_or(usual, |edit| {
                if edit.select_all {
                    edit.text.clone()
                } else {
                    format!("{}│", edit.text)
                }
            })
    }

    pub fn paint(&mut self, canvas: &mut impl Canvas, bounds: RECT, client: &Client) {
        let width = bounds.right - bounds.left;
        let height = bounds.bottom - bounds.top;
        let mut hits = std::mem::take(&mut self.hits);
        hits.clear();
        if self.tab == Tab::Settings && client.has_settings_page() {
            self.tab = Tab::Items;
            self.scroll = 0;
        }
        let mut paint = Paint {
            canvas,
            origin: (bounds.left, bounds.top),
            pointer: self.pointer,
            hits: &mut hits,
            clip: rect(0, 0, width, height),
        };
        paint.fill(rect(0, 0, width, height), BG);
        paint.border(rect(0, 0, width, height), BORDER);
        paint.click(rect(1, 1, width - 17, TITLE - 1), Hit::Title);
        paint.label(rect(1, 1, width - 2, TITLE - 1), "Inventory", WHITE);
        paint.button(rect(width - 15, 1, 14, 14), "×", Hit::Close, false);
        let mut x = 1;
        for (tab, label, w) in [
            (Tab::Items, "Items", 36),
            (Tab::Debug, "Debug", 40),
            (Tab::Settings, "Settings", 55),
        ] {
            if tab != Tab::Settings || !client.has_settings_page() {
                paint.button(rect(x, TITLE, w, 16), label, Hit::Tab(tab), tab == self.tab);
                x += w;
            }
        }
        paint.fill(rect(1, TITLE + 16, width - 2, 1), BORDER);
        let viewport = rect(
            1,
            CONTENT,
            (width - SCROLLBAR - 1).max(0),
            (height - CONTENT - 1).max(0),
        );
        paint.clip = viewport;
        let mut y = CONTENT - self.scroll;
        match self.tab {
            Tab::Items => Self::items(&mut paint, client, &mut y, width),
            Tab::Debug => Self::debug(
                &mut paint,
                client,
                self.debug_item,
                self.debug_count,
                self,
                &mut y,
            ),
            Tab::Settings => Self::settings(&mut paint, client, self, &mut y),
        }
        let total = (y + self.scroll - CONTENT).max(0);
        self.max_scroll = (total - (height - CONTENT)).max(0);
        self.scroll = self.scroll.min(self.max_scroll);
        paint.clip = rect(0, 0, width, height);
        if self.max_scroll > 0 {
            let scroll_x = width - SCROLLBAR;
            let start = TITLE + 16;
            let rail = (height - start - 15).max(1);
            let thumb = ((height - CONTENT) * rail / total.max(1)).clamp(12.min(rail), rail);
            let thumb_y = start + (rail - thumb) * self.scroll / self.max_scroll;
            paint.fill(rect(scroll_x, TITLE, SCROLLBAR - 1, height - TITLE - 1), BG);
            paint.border(
                rect(scroll_x, TITLE, SCROLLBAR - 1, height - TITLE - 1),
                BORDER,
            );
            paint.button(
                rect(scroll_x, TITLE, SCROLLBAR - 1, 15),
                "▲",
                Hit::Scroll(-58),
                false,
            );
            paint.button(
                rect(scroll_x, height - 15, SCROLLBAR - 1, 14),
                "▼",
                Hit::Scroll(58),
                false,
            );
            paint.click(
                rect(scroll_x, start, SCROLLBAR - 1, thumb_y - start),
                Hit::Scroll(-(height - CONTENT)),
            );
            paint.click(
                rect(
                    scroll_x,
                    thumb_y + thumb,
                    SCROLLBAR - 1,
                    (start + rail - thumb_y - thumb).max(0),
                ),
                Hit::Scroll(height - CONTENT),
            );
            paint.fill(rect(scroll_x + 1, thumb_y, SCROLLBAR - 3, thumb), BORDER);
            paint.click(
                rect(scroll_x, thumb_y, SCROLLBAR - 1, thumb),
                Hit::ScrollThumb,
            );
            for row in -2..=2 {
                paint.fill(
                    rect(
                        scroll_x + 3,
                        thumb_y + thumb / 2 + row * 2,
                        SCROLLBAR - 7,
                        1,
                    ),
                    BG,
                );
            }
        }
        paint.click(rect(width - 8, height - 8, 7, 7), Hit::Resize);
        if let Some(dropdown) = &self.dropdown {
            paint.choices(dropdown, client, height);
        } else if let Some(popup) = &self.popup {
            let stack = client
                .snapshot
                .as_ref()
                .and_then(|state| state.slots.get(popup.slot)?.as_ref());
            if let Some(stack) = stack {
                paint.popup(popup, stack.count, width, height);
            }
        } else if let Some(point) = self.pointer {
            let source = match self.gesture {
                Some(Gesture::Slot(index, start)) if moved(start, point) => Some(index),
                _ => None,
            };
            let index = source.or_else(|| {
                paint.hits.iter().rev().find_map(|(bounds, hit)| {
                    if let Hit::Slot(slot) = hit
                        && contains(*bounds, point)
                    {
                        Some(*slot)
                    } else {
                        None
                    }
                })
            });
            if let Some(stack) =
                index.and_then(|index| client.snapshot.as_ref()?.slots.get(index)?.as_ref())
            {
                paint.tooltip(client, stack, width, height, source.is_some());
            }
        }
        self.hits = hits;
    }

    fn items(paint: &mut Paint<'_, impl Canvas>, client: &Client, y: &mut i32, width: i32) {
        let slots = client
            .snapshot
            .as_ref()
            .map_or(client.settings.limits.slots, |state| state.limits.slots);
        let weapons = client
            .snapshot
            .as_ref()
            .map_or(client.settings.limits.weapon_slots, |state| {
                state.limits.weapon_slots
            });
        for (base, len) in [(0, slots), (slots, weapons)] {
            if base > 0 && len > 0 {
                *y += 4;
            }
            for index in 0..len {
                let r = rect(
                    3 + (index % COLUMNS) as i32 * 60,
                    *y + (index / COLUMNS) as i32 * 58,
                    SLOT,
                    SLOT,
                );
                if paint.visible(r) {
                    let index = base + index;
                    let stack = client
                        .snapshot
                        .as_ref()
                        .and_then(|state| state.slots.get(index)?.as_ref());
                    paint.click(r, Hit::Slot(index));
                    let hovered = paint
                        .pointer
                        .is_some_and(|point| contains(r, point) && contains(paint.clip, point));
                    let translated = paint.translate(r);
                    let mut clipped = ClipCanvas {
                        inner: paint.canvas,
                        clip: translated_clip(paint.origin, paint.clip),
                    };
                    hud::slot(
                        &mut clipped,
                        translated,
                        &client.catalog,
                        stack,
                        index == client.selection,
                        hovered,
                        None,
                    );
                }
            }
            *y += len.div_ceil(COLUMNS) as i32 * 58;
        }
        *y += 4;
        paint.button(rect(3, *y, 120, 17), "Store held item", Hit::Store, false);
        paint.button(rect(127, *y, 110, 17), "Pick up nearby", Hit::Pickup, false);
        paint.button(rect(241, *y, 90, 17), "Unload gun", Hit::Unload, false);
        *y += 20;
        paint.button(
            rect(3, *y, 120, 17),
            "Use selected",
            Hit::UseSelected,
            false,
        );
        *y += 21;
        let inventory = client.snapshot.as_ref();
        let used = inventory.map_or(0, |state| {
            state
                .slots
                .iter()
                .take(slots)
                .filter(|slot| slot.is_some())
                .count()
        });
        let weight = inventory.and_then(|state| state.weight).unwrap_or_default();
        let limit = inventory.map_or(client.settings.limits.max_weight, |state| {
            state.limits.max_weight
        });
        let mut summary = format!("Slots {used}/{slots}    Weight {weight:.1}");
        if limit > 0.0 {
            summary.push_str(&format!("/{limit:.1}"));
        }
        paint.text(4, *y, &summary, TEXT);
        *y += 18;
        if let Some(reserve) = client.reserve {
            let rounds = AmmoKind::ALL.into_iter().filter_map(|kind| {
                let count = reserve[kind.definition().index];
                (count > 0).then_some((kind, count))
            });
            let mut shown = false;
            for (kind, count) in rounds {
                if !shown {
                    *y += 4;
                    paint.text(4, *y, "Rounds", WHITE);
                    *y += 16;
                    shown = true;
                }
                let def = kind.definition();
                let mag = count.min(def.magazine);
                paint.button(
                    rect(3, *y, 64, 17),
                    &format!("Drop {mag}"),
                    Hit::Ammo(kind, mag),
                    false,
                );
                paint.button(
                    rect(71, *y, 64, 17),
                    "Drop all",
                    Hit::Ammo(kind, count),
                    false,
                );
                let [_, r, g, b] = def.color.to_be_bytes();
                paint.fill(
                    rect(140, *y + 3, 10, 10),
                    u32::from_le_bytes([r, g, b, 255]),
                );
                paint.text(
                    154,
                    *y,
                    &format!(
                        "{} {count} ({:.1})",
                        def.title,
                        client.catalog.round_weight(kind) * count as f32
                    ),
                    TEXT,
                );
                *y += 20;
            }
        }
        paint.text(
            4,
            *y,
            if client.host.is_some() || (inventory.is_some() && client.link().in_game()) {
                "Game: connected"
            } else {
                "Game: not connected (list only)"
            },
            DIM_TEXT,
        );
        *y += 16;
        let hint = "Click selects, double click uses, right click for more, drag to move.";
        if width >= 450 {
            paint.text(4, *y, hint, DIM_TEXT);
            *y += 16;
        } else {
            paint.text(4, *y, "Click selects, double click uses,", DIM_TEXT);
            paint.text(4, *y + 14, "right click for more, drag to move.", DIM_TEXT);
            *y += 30;
        }
    }

    fn debug(
        paint: &mut Paint<'_, impl Canvas>,
        client: &Client,
        item: usize,
        count: i32,
        ui: &Self,
        y: &mut i32,
    ) {
        if client.can_edit_inventory() {
            paint.heading(y, "Add items");
            let name = client
                .catalog
                .items()
                .nth(item)
                .map_or("Item", |(_, def)| def.name);
            paint.text(4, *y + 2, "Item:", TEXT);
            paint.combo(72, *y, name, Choices::Item);
            *y += 21;
            paint.text(4, *y + 2, "Count:", TEXT);
            paint.stepper(
                72,
                *y,
                &ui.number_label(Number::Count, count.to_string()),
                Hit::DebugCount(-1),
                Hit::DebugCount(1),
                Hit::Number(Number::Count),
            );
            *y += 23;
            for (label, hit, x, w) in [
                ("Add", Hit::DebugAdd, 4, 40),
                ("Add one of each", Hit::DebugAddEach, 49, 110),
                ("Clear", Hit::DebugClear, 164, 52),
            ] {
                paint.button(rect(x, *y, w, 18), label, hit, false);
            }
            *y += 23;
        } else {
            paint.heading(y, "Only the host can edit inventory");
        }
        paint.button(rect(4, *y, 109, 18), "Infect me (test)", Hit::Infect, false);
        paint.button(
            rect(118, *y, 121, 18),
            "Screamer (test)",
            Hit::Screamer,
            false,
        );
        *y += 24;
        paint.checkbox(
            y,
            "Preview the infection effect",
            ui.infection_preview.active,
            Hit::PreviewInfection,
        );
        if let Some(amount) = ui.infection_preview.get() {
            paint.slider(16, *y, "Infection", amount, amount, Slider::Infection);
            *y += 21;
        }
        paint.checkbox(
            y,
            "Preview the stamina bar and vignette",
            ui.stamina_preview.active,
            Hit::PreviewStamina,
        );
        if let Some(amount) = ui.stamina_preview.get() {
            paint.slider(16, *y, "Stamina", amount, amount, Slider::Stamina);
            *y += 22;
            paint.checkbox(y, "Exhausted", ui.exhausted_preview, Hit::PreviewExhausted);
        }
        paint.checkbox(y, "Fog over the map", ui.fog.active, Hit::Fog);
        if let Some(amount) = ui.fog.get() {
            paint.slider(16, *y, "Fog", amount, amount, Slider::Fog);
            *y += 22;
        }
        let mutators = client
            .mutators
            .iter()
            .map(|active| active.mutator.title())
            .collect::<Vec<_>>()
            .join(", ");
        let mutators = if mutators.is_empty() {
            "none"
        } else {
            &mutators
        };
        paint.text(
            4,
            *y + 4,
            &format!("Chapter mutators: {mutators}"),
            DIM_TEXT,
        );
        *y += 24;
    }

    fn settings(paint: &mut Paint<'_, impl Canvas>, client: &Client, ui: &Self, y: &mut i32) {
        let mut cfg = client.settings.clone();
        let mut keys = cfg.keys;
        paint.heading(y, "Keys");
        for (index, name) in [
            "Open / close",
            "Previous item",
            "Next item",
            "Use item",
            "Drop item",
            "Store held item",
            "Pick up nearby",
            "Unload gun",
            "Ping",
        ]
        .into_iter()
        .enumerate()
        {
            paint.text(4, *y + 1, name, TEXT);
            let key = if ui.rebinding == Some(index) {
                "press a key…".into()
            } else {
                key_name(*key_field(&mut keys, index))
            };
            paint.button(
                rect(134, *y, 125, 17),
                &key,
                Hit::Bind(index),
                ui.rebinding == Some(index),
            );
            *y += 20;
        }
        for (heading, settings) in Setting::GROUPS {
            paint.heading(y, heading);
            for &setting in *settings {
                if !setting.visible(&cfg) {
                    continue;
                }
                if let Some(enabled) = setting.field(&mut cfg).copied() {
                    paint.checkbox(y, setting.name(), enabled, Hit::Settings(setting, 1));
                } else {
                    match setting {
                        Setting::Scale => paint.slider(
                            4,
                            *y,
                            setting.name(),
                            (cfg.hud.scale - 0.5) / 2.5,
                            cfg.hud.scale,
                            Slider::Scale,
                        ),
                        Setting::Chance => paint.slider(
                            4,
                            *y,
                            setting.name(),
                            cfg.infection.screamer_chance,
                            cfg.infection.screamer_chance,
                            Slider::Chance,
                        ),
                        Setting::View => {
                            paint.text(4, *y + 1, setting.name(), TEXT);
                            paint.combo(112, *y, &setting.value(&cfg), Choices::View);
                        }
                        _ => {
                            paint.text(4, *y + 1, setting.name(), TEXT);
                            paint.stepper(
                                134,
                                *y,
                                &ui.number_label(Number::Limit(setting), setting.value(&cfg)),
                                Hit::Settings(setting, -1),
                                Hit::Settings(setting, 1),
                                Hit::Number(Number::Limit(setting)),
                            );
                        }
                    }
                    *y += 21;
                }
            }
        }
    }

    pub fn mouse(
        &mut self,
        client: &mut Client,
        message: u32,
        point: (i32, i32),
        viewport: (i32, i32),
    ) -> Option<Action> {
        self.pointer = Some(point);
        let target = self
            .hits
            .iter()
            .rev()
            .find(|(r, _)| contains(*r, point))
            .copied();
        let hit = target.map(|(_, hit)| hit);
        let now = Instant::now();
        match message {
            WM_MOUSEMOVE => {
                match self.gesture {
                    Some(Gesture::Move(start)) => {
                        self.offset.0 += point.0 - start.0;
                        self.offset.1 += point.1 - start.1;
                        let bounds = self.bounds(viewport.0, viewport.1);
                        self.offset.0 =
                            bounds.left - viewport.0 / 2 + (bounds.right - bounds.left) / 2;
                        self.offset.1 =
                            bounds.top - viewport.1 * 45 / 100 + (bounds.bottom - bounds.top) / 2;
                    }
                    Some(Gesture::Resize(start)) => {
                        self.size = (
                            (point.0 + start.0).clamp(270, viewport.0.max(270)),
                            (point.1 + start.1).clamp(200, viewport.1.max(200)),
                        );
                    }
                    Some(Gesture::Scroll((start, amount))) => {
                        self.scroll = (amount
                            + (point.1 - start) * self.max_scroll
                                / (self.size.1 - CONTENT - 32).max(1))
                        .clamp(0, self.max_scroll);
                    }
                    Some(Gesture::Slide(slider, bounds)) => {
                        return self.slide(client, slider, fraction(bounds, point.0), false);
                    }
                    _ => {}
                }
                None
            }
            WM_RBUTTONUP => {
                self.dropdown = None;
                self.popup = hit.and_then(|hit| match hit {
                    Hit::Slot(slot)
                        if client
                            .snapshot
                            .as_ref()
                            .and_then(|state| state.slots.get(slot)?.as_ref())
                            .is_some() =>
                    {
                        Some(Popup {
                            slot,
                            position: point,
                        })
                    }
                    _ => None,
                });
                None
            }
            WM_LBUTTONDOWN => {
                if self.dropdown.is_some() && !matches!(hit, Some(Hit::Choose(..))) {
                    self.dropdown = None;
                    return None;
                }
                if self.popup.is_some() && !matches!(hit, Some(Hit::Equip(_) | Hit::Drop(_, _))) {
                    self.popup = None;
                    return None;
                }
                let commit = if !matches!(hit, Some(Hit::Number(number)) if self.editing.as_ref().is_some_and(|edit| edit.number == number))
                {
                    self.finish_number(&mut client.settings)
                } else {
                    false
                };
                self.gesture = match hit {
                    Some(Hit::Slot(slot)) => {
                        client.selection = slot;
                        client.native_selection = None;
                        Some(Gesture::Slot(slot, point))
                    }
                    Some(Hit::Title) => Some(Gesture::Move(point)),
                    Some(Hit::Resize) => Some(Gesture::Resize((
                        self.size.0 - point.0,
                        self.size.1 - point.1,
                    ))),
                    Some(Hit::ScrollThumb) => Some(Gesture::Scroll((point.1, self.scroll))),
                    Some(Hit::Slider(slider)) => {
                        let bounds = target?.0;
                        self.gesture = Some(Gesture::Slide(slider, bounds));
                        return self.slide(client, slider, fraction(bounds, point.0), false);
                    }
                    Some(hit) => Some(Gesture::Control(hit)),
                    None => None,
                };
                commit.then_some(Action::SettingsChanged)
            }
            WM_LBUTTONUP => {
                let gesture = self.gesture.take()?;
                match gesture {
                    Gesture::Slot(from, start) => {
                        let Some(Hit::Slot(to)) = hit else {
                            return None;
                        };
                        let filled = client
                            .snapshot
                            .as_ref()
                            .and_then(|state| state.slots.get(from)?.as_ref())
                            .is_some();
                        if from != to && moved(start, point) && filled {
                            self.last_click = None;
                            client.selection = to;
                            Some(Action::Send(format!("inv_move {from} {to}")))
                        } else if from == to && !moved(start, point) {
                            let double = self.last_click.is_some_and(|(slot, time)| {
                                slot == to
                                    && now - time
                                        < Duration::from_millis(u64::from(unsafe {
                                            GetDoubleClickTime()
                                        }))
                            });
                            self.last_click = (!double).then_some((to, now));
                            (double && filled).then(|| Action::Send(format!("inv_use {to}")))
                        } else {
                            self.last_click = None;
                            None
                        }
                    }
                    Gesture::Control(pressed) if hit == Some(pressed) => {
                        self.activate(client, pressed)
                    }
                    Gesture::Slide(slider, bounds) => {
                        self.slide(client, slider, fraction(bounds, point.0), true)
                    }
                    _ => None,
                }
            }
            _ => None,
        }
    }

    fn activate(&mut self, client: &mut Client, hit: Hit) -> Option<Action> {
        let command = match hit {
            Hit::Close => return Some(Action::Close),
            Hit::Tab(tab) => {
                self.tab = tab;
                self.scroll = 0;
                self.popup = None;
                return None;
            }
            Hit::Scroll(pixels) => {
                self.scroll_by(pixels);
                return None;
            }
            Hit::Store => "inv_store".to_owned(),
            Hit::Pickup => "inv_pickup".to_owned(),
            Hit::Unload => return client.unload().map(Action::Send),
            Hit::UseSelected => format!("inv_use {}", client.selection),
            Hit::Equip(slot) => format!("inv_use {slot}"),
            Hit::Drop(slot, n) => format!("inv_drop {slot} {n}"),
            Hit::Ammo(kind, n) => format!("inv_dropammo {} {n}", kind.name()),
            Hit::OpenChoices(choices) => {
                let anchor = self
                    .hits
                    .iter()
                    .find_map(|(r, hit)| (*hit == Hit::OpenChoices(choices)).then_some(*r))?;
                let count = match choices {
                    Choices::Item => client.catalog.items().count(),
                    Choices::View => VIEWS.len(),
                };
                let first = if choices == Choices::Item {
                    self.debug_item
                        .saturating_sub(4)
                        .min(count.saturating_sub(9))
                } else {
                    0
                };
                self.dropdown = Some(Dropdown {
                    choices,
                    anchor,
                    first,
                    count,
                });
                return None;
            }
            Hit::Choose(choices, index) => {
                self.dropdown = None;
                if choices == Choices::Item {
                    self.debug_item = index;
                } else if let Some((_, view)) = VIEWS.get(index) {
                    client.settings.hud.view = *view;
                    return Some(Action::SettingsChanged);
                }
                return None;
            }
            Hit::DebugCount(delta) => {
                self.debug_count = (self.debug_count + delta).clamp(1, 99);
                return None;
            }
            Hit::Number(number) => {
                let text = match number {
                    Number::Count => self.debug_count.to_string(),
                    Number::Limit(setting) => setting.value(&client.settings),
                };
                self.editing = Some(NumberEdit {
                    number,
                    text,
                    select_all: true,
                });
                self.rebinding = None;
                return None;
            }
            Hit::DebugAdd => {
                return client
                    .catalog
                    .items()
                    .nth(self.debug_item)
                    .map(|(id, _)| Action::Add(id, self.debug_count));
            }
            Hit::DebugAddEach => return Some(Action::AddEach),
            Hit::DebugClear => return Some(Action::Clear),
            Hit::Infect => "inv_infect".to_owned(),
            Hit::Screamer => return Some(Action::Screamer),
            Hit::PreviewInfection => {
                self.infection_preview.toggle();
                return None;
            }
            Hit::PreviewStamina => {
                self.stamina_preview.toggle();
                return None;
            }
            Hit::PreviewExhausted => {
                self.exhausted_preview = !self.exhausted_preview;
                return None;
            }
            Hit::Fog => {
                self.fog.toggle();
                return Some(Action::Fog(self.fog.get()));
            }
            Hit::Bind(key) => {
                self.rebinding = Some(key);
                self.editing = None;
                return None;
            }
            Hit::Settings(setting, direction) => {
                setting.apply(&mut client.settings, direction);
                return Some(Action::SettingsChanged);
            }
            _ => return None,
        };
        self.popup = None;
        Some(Action::Send(command))
    }

    fn slide(
        &mut self,
        client: &mut Client,
        slider: Slider,
        amount: f32,
        done: bool,
    ) -> Option<Action> {
        match slider {
            Slider::Infection => self.infection_preview.value = amount,
            Slider::Stamina => self.stamina_preview.value = amount,
            Slider::Fog => {
                self.fog.value = amount;
                return Some(Action::Fog(self.fog.get()));
            }
            Slider::Scale => client.settings.hud.scale = 0.5 + amount * 2.5,
            Slider::Chance => client.settings.infection.screamer_chance = amount,
        }
        (done && matches!(slider, Slider::Scale | Slider::Chance))
            .then_some(Action::SettingsChanged)
    }
}

fn fraction(bounds: RECT, x: i32) -> f32 {
    ((x - bounds.left) as f32 / (bounds.right - bounds.left).max(1) as f32).clamp(0.0, 1.0)
}

fn moved(start: (i32, i32), end: (i32, i32)) -> bool {
    (start.0 - end.0).abs().max((start.1 - end.1).abs()) >= 4
}

fn contains(r: RECT, (x, y): (i32, i32)) -> bool {
    (r.left..r.right).contains(&x) && (r.top..r.bottom).contains(&y)
}

fn intersection(a: RECT, b: RECT) -> Option<RECT> {
    let left = a.left.max(b.left);
    let right = a.right.min(b.right);
    let top = a.top.max(b.top);
    let bottom = a.bottom.min(b.bottom);
    (left < right && top < bottom).then(|| rect(left, top, right - left, bottom - top))
}

fn translated_clip(origin: (i32, i32), r: RECT) -> RECT {
    rect(
        origin.0 + r.left,
        origin.1 + r.top,
        r.right - r.left,
        r.bottom - r.top,
    )
}

struct ClipCanvas<'a, C> {
    inner: &'a mut C,
    clip: RECT,
}

impl<C: Canvas> Canvas for ClipCanvas<'_, C> {
    fn rect(&mut self, bounds: RECT, color: COLORREF) {
        if let Some(visible) = intersection(bounds, self.clip) {
            self.inner.rect(visible, color);
        }
    }

    fn text(&mut self, at: (i32, i32), text: &str, color: COLORREF) {
        let (width, height) = self.inner.text_size(text);
        if intersection(rect(at.0, at.1, width, height), self.clip).is_some()
            && at.1 >= self.clip.top
            && at.1 + height <= self.clip.bottom
        {
            self.inner.text(at, text, color);
        }
    }

    fn text_size(&self, text: &str) -> (i32, i32) {
        self.inner.text_size(text)
    }

    fn icon(&mut self, name: &str, bounds: RECT, color: COLORREF) -> bool {
        if bounds.top >= self.clip.top && bounds.bottom <= self.clip.bottom {
            self.inner.icon(name, bounds, color)
        } else {
            true
        }
    }
}

struct Paint<'a, C> {
    canvas: &'a mut C,
    origin: (i32, i32),
    pointer: Option<(i32, i32)>,
    hits: &'a mut Vec<(RECT, Hit)>,
    clip: RECT,
}

impl<C: Canvas> Paint<'_, C> {
    fn translate(&self, r: RECT) -> RECT {
        translated_clip(self.origin, r)
    }

    fn visible(&self, r: RECT) -> bool {
        intersection(self.clip, r).is_some()
    }

    fn fill(&mut self, r: RECT, color: COLORREF) {
        if let Some(r) = intersection(self.clip, r) {
            self.canvas.rect(self.translate(r), color);
        }
    }

    fn border(&mut self, r: RECT, color: COLORREF) {
        let r = self.translate(r);
        let clip = self.translate(self.clip);
        outline(
            &mut ClipCanvas {
                inner: self.canvas,
                clip,
            },
            r,
            color,
            1,
        );
    }

    fn label(&mut self, r: RECT, text: &str, color: COLORREF) {
        if self.visible(r) {
            let r = self.translate(r);
            let clip = self.translate(self.clip);
            center(
                &mut ClipCanvas {
                    inner: self.canvas,
                    clip,
                },
                r,
                text,
                color,
            );
        }
    }

    fn text(&mut self, x: i32, y: i32, text: &str, color: COLORREF) {
        let line = self.canvas.text_size(text).1;
        if y >= self.clip.top && y + line <= self.clip.bottom {
            self.canvas
                .text((self.origin.0 + x, self.origin.1 + y), text, color);
        }
    }

    fn click(&mut self, r: RECT, hit: Hit) {
        if let Some(r) = intersection(self.clip, r) {
            self.hits.push((r, hit));
        }
    }

    fn button(&mut self, r: RECT, caption: &str, hit: Hit, selected: bool) {
        let hover = self
            .pointer
            .is_some_and(|point| contains(r, point) && contains(self.clip, point));
        self.fill(
            r,
            if hover || selected {
                BG_FOCUSED
            } else {
                BG_ALT
            },
        );
        self.border(r, if hover { BORDER_FOCUSED } else { BORDER });
        self.label(r, caption, WHITE);
        self.click(r, hit);
    }

    fn checkbox(&mut self, y: &mut i32, caption: &str, enabled: bool, hit: Hit) {
        let checkbox = rect(4, *y + 2, 12, 12);
        self.fill(checkbox, BG_ALT);
        self.border(checkbox, BORDER);
        if enabled {
            self.fill(hud::inset(checkbox, 3, 3), BORDER_FOCUSED);
        }
        self.text(20, *y + 1, caption, TEXT);
        self.click(rect(4, *y, (self.clip.right - 8).max(0), 16), hit);
        *y += 19;
    }

    fn heading(&mut self, y: &mut i32, title: &str) {
        self.text(5, *y + 1, title, TEXT_ALT);
        *y += 17;
        self.fill(rect(4, *y, self.clip.right - 8, 1), BORDER);
        *y += 5;
    }

    fn stepper(&mut self, x: i32, y: i32, label: &str, minus: Hit, plus: Hit, edit: Hit) {
        let width = (self.clip.right - x - 5).min(170);
        self.button(rect(x, y, 18, 17), "−", minus, false);
        self.button(rect(x + 18, y, width - 36, 17), label, edit, false);
        self.button(rect(x + width - 18, y, 18, 17), "+", plus, false);
    }

    fn combo(&mut self, x: i32, y: i32, label: &str, choices: Choices) {
        let bounds = rect(x, y, (self.clip.right - x - 5).clamp(30, 175), 17);
        self.button(bounds, label, Hit::OpenChoices(choices), false);
        self.border(rect(bounds.right - 17, y, 17, 17), BORDER);
        self.label(rect(bounds.right - 17, y, 17, 17), "▼", TEXT_ALT);
    }

    fn slider(&mut self, x: i32, y: i32, label: &str, fill: f32, value: f32, kind: Slider) {
        let bounds = rect(x, y, (self.clip.right - x - 5).clamp(1, 264), 17);
        self.fill(bounds, BG_ALT);
        self.fill(
            rect(
                bounds.left + 1,
                bounds.top + 1,
                ((bounds.right - bounds.left - 2).max(0) as f32 * fill.clamp(0.0, 1.0)) as i32,
                15,
            ),
            BG_FOCUSED,
        );
        self.border(bounds, BORDER);
        self.label(bounds, &format!("{label}: {value:.2}"), WHITE);
        self.click(bounds, Hit::Slider(kind));
    }

    fn choices(&mut self, dropdown: &Dropdown, client: &Client, height: i32) {
        let x = dropdown.anchor.left;
        let width = dropdown.anchor.right - x;
        let count = dropdown.count.min(9);
        let size = count as i32 * 17 + 2;
        let y = dropdown.anchor.bottom.min(height - size - 2).max(CONTENT);
        self.fill(rect(x, y, width, size), BG);
        self.border(rect(x, y, width, size), BORDER);
        let options: Vec<_> = match dropdown.choices {
            Choices::Item => client.catalog.items().map(|(_, def)| def.name).collect(),
            Choices::View => VIEWS.iter().map(|(label, _)| *label).collect(),
        };
        for (index, label) in options
            .into_iter()
            .enumerate()
            .skip(dropdown.first)
            .take(count)
        {
            let row_y = y + 1 + (index - dropdown.first) as i32 * 17;
            self.button(
                rect(x + 1, row_y, width - 2, 17),
                label,
                Hit::Choose(dropdown.choices, index),
                false,
            );
        }
    }

    fn popup(&mut self, popup: &Popup, count: i32, width: i32, height: i32) {
        let n = if count > 1 { 3 } else { 2 };
        let x = popup.position.0.clamp(2, (width - 91).max(2));
        let y = popup
            .position
            .1
            .clamp(CONTENT, (height - 18 * n - 4).max(CONTENT));
        self.fill(rect(x, y, 88, 18 * n + 2), BG);
        self.border(rect(x, y, 88, 18 * n + 2), BORDER);
        self.button(
            rect(x + 1, y + 1, 86, 18),
            "Use",
            Hit::Equip(popup.slot),
            false,
        );
        self.button(
            rect(x + 1, y + 19, 86, 18),
            "Drop one",
            Hit::Drop(popup.slot, 1),
            false,
        );
        if count > 1 {
            self.button(
                rect(x + 1, y + 37, 86, 18),
                "Drop all",
                Hit::Drop(popup.slot, count),
                false,
            );
        }
    }

    fn tooltip(
        &mut self,
        client: &Client,
        stack: &RemoteStack,
        width: i32,
        height: i32,
        dragged: bool,
    ) {
        let def = client.catalog.item(stack.item);
        let mut lines = vec![if dragged {
            format!("{} ×{}", def.name, stack.count + i32::from(stack.held))
        } else {
            def.name.to_owned()
        }];
        if !dragged {
            let detail = if let Some(weapon) = &stack.weapon {
                if !weapon.melee.is_empty() {
                    weapon.melee.clone()
                } else {
                    format!("Magazine: {}", weapon.clip.max(0))
                }
            } else {
                def.description.to_owned()
            };
            if !detail.is_empty() {
                lines.push(detail);
            }
            if let Some(fuel) = stack.fuel {
                lines.push(format!("Fuel: {} / {}", fuel.full - fuel.spent, fuel.full));
            }
            lines.push(format!("Weight: {:.1}", def.weight));
        }
        let line_height = self.canvas.text_size("Ag").1.max(11);
        let w = (lines
            .iter()
            .map(|line| self.canvas.text_size(line).0)
            .max()
            .unwrap_or(0)
            + 12)
            .min(width - 4);
        let h = lines.len() as i32 * line_height + 8;
        let (px, py) = self.pointer.unwrap_or((0, 0));
        let x = (px + 14).min(width - w - 2).max(2);
        let y = (py + 16).min(height - h - 1).max(2);
        self.fill(rect(x, y, w, h), BG_ALT);
        self.border(rect(x, y, w, h), BORDER);
        for (i, label) in lines.iter().enumerate() {
            self.text(
                x + 6,
                y + 4 + i as i32 * line_height,
                label,
                if i == 0 { WHITE } else { TEXT },
            );
        }
    }
}
