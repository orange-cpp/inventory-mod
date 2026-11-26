use std::{sync::Arc, time::Instant};
use windows_sys::Win32::{
    Foundation::{COLORREF, HWND, LPARAM, RECT},
    UI::WindowsAndMessaging::GetClientRect,
};

use crate::{
    client::{Client, send, with_client},
    hud::rect,
    postfx::{self, DrawList, Drawing, Effect, Frame, Layer},
};

pub fn update(client: &mut Client, game: HWND) {
    let native = client.vgui.as_ref().is_some_and(|vgui| vgui.active());
    let visible = !native && postfx::drawable() && (client.link().in_game() || client.open);
    let mut bounds = RECT::default();
    let frame = if visible && unsafe { GetClientRect(game, &mut bounds) != 0 } {
        Some(capture(client, (bounds.right, bounds.bottom)))
    } else {
        None
    };
    postfx::publish_drawing(frame);
}

fn capture(client: &mut Client, (width, height): (i32, i32)) -> Drawing {
    let mut hud = DrawList::new((width, height), Arc::clone(&client.icons));
    let mut inventory = DrawList::new((width, height), Arc::clone(&client.icons));
    update_effects(client);
    draw_effects(&mut hud, client, width, height, false);
    draw_indicators(&mut hud, client, width, height);
    crate::hud::paint(&mut hud, client, width, height);
    if let Some(wheel) = &mut client.wheel {
        wheel.draw(&mut hud, width, height);
    }
    if show_panel(client) {
        let bounds = panel_bounds(client, width, height);
        draw_panel(&mut inventory, client, bounds);
        let at = client
            .ui
            .pointer()
            .map_or((width / 2, height / 2), |(x, y)| {
                (bounds.left + x, bounds.top + y)
            });
        crate::hud::pointer(
            &mut inventory,
            (at.0 as f32, at.1 as f32),
            (height as f32 / 900.0).max(1.0),
        );
    }
    draw_effects(&mut inventory, client, width, height, true);
    if let Some(effect) = &client.screamer {
        inventory.screamer(effect);
    }
    Drawing { hud, inventory }
}

const fn rgb(r: u32, g: u32, b: u32) -> COLORREF {
    r | (g << 8) | (b << 16)
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum Font {
    Title,
    Hint,
    Ammo,
    Text,
}

pub trait Canvas {
    fn rect(&mut self, bounds: RECT, color: COLORREF);
    fn text(&mut self, at: (i32, i32), label: &str, color: COLORREF);
    fn text_size(&self, label: &str) -> (i32, i32) {
        (label.chars().count() as i32 * 7, 14)
    }
    fn measure(&self, _font: Font, label: &str) -> (i32, i32) {
        self.text_size(label)
    }
    fn styled_text(&mut self, _font: Font, at: (i32, i32), label: &str, color: COLORREF) {
        self.text(at, label, color);
    }
    fn icon(&mut self, _name: &str, _bounds: RECT, _color: COLORREF) -> bool {
        false
    }
    fn polygon(&mut self, _points: &[(f32, f32)], _color: COLORREF) {}
}

pub fn fit(bounds: RECT, aspect: f32) -> RECT {
    let width = (bounds.right - bounds.left).max(1) as f32;
    let height = (bounds.bottom - bounds.top).max(1) as f32;
    let margin_x = ((width - (height * aspect).min(width)) * 0.5).round() as i32;
    let margin_y = ((height - (width / aspect).min(height)) * 0.5).round() as i32;
    RECT {
        left: bounds.left + margin_x,
        right: bounds.right - margin_x,
        top: bounds.top + margin_y,
        bottom: bounds.bottom - margin_y,
    }
}

pub fn show_panel(client: &Client) -> bool {
    client.open && client.wheel.is_none()
}

pub fn panel_bounds(client: &Client, screen_width: i32, screen_height: i32) -> RECT {
    client.ui.bounds(screen_width, screen_height)
}

pub fn draw_panel(canvas: &mut impl Canvas, client: &mut Client, bounds: RECT) {
    let mut ui = std::mem::take(&mut client.ui);
    ui.paint(canvas, bounds, client);
    client.ui = ui;
}
#[derive(Default)]
pub struct Visuals {
    last: Option<Instant>,
    beats: f64,
    stamina: StaminaVisual,
    frame: Frame,
}

struct StaminaVisual {
    value: f32,
    alpha: f32,
    exhausted: f32,
    exhausted_now: bool,
    breath: f32,
    full_since: Option<Instant>,
}

impl Default for StaminaVisual {
    fn default() -> Self {
        Self {
            value: 1.0,
            alpha: 0.0,
            exhausted: 0.0,
            exhausted_now: false,
            breath: 0.0,
            full_since: None,
        }
    }
}

impl Visuals {
    pub fn clear(&mut self) {
        *self = Self::default();
    }

    pub fn beats(&self) -> f64 {
        self.beats
    }
}

pub fn update_effects(client: &mut Client) {
    let now = Instant::now();
    let visual = &mut client.visuals;
    let delta = visual
        .last
        .replace(now)
        .map(|previous| (now - previous).as_secs_f32())
        .filter(|&delta| delta < 0.25)
        .unwrap_or_default();
    let preview = client
        .ui
        .infection_preview
        .get()
        .map(|value| ((1.0 - value) * 120.0, value));
    let timing = preview.or_else(|| {
        let (until, total) = client.infection?;
        let remaining = until.checked_duration_since(now)?.as_secs_f32();
        Some((
            remaining,
            (1.0 - remaining * 1000.0 / total as f32).clamp(0.0, 1.0),
        ))
    });
    let infection = timing.and_then(|(remaining, severity)| {
        visual.beats += f64::from(delta) * f64::from(50.0 + 100.0 * severity) / 60.0;
        let phase = visual.beats.fract() as f32;
        let beat = (-phase * 14.0).exp() + 0.6 * (-(phase - 0.25).abs() * 18.0).exp();
        let last = (1.0 - remaining / 10.0).clamp(0.0, 1.0);
        let cfg = &client.settings.infection;
        cfg.effect.then_some(Effect {
            layer: Layer::new(cfg.cover_game_hud, cfg.cover_mod_hud),
            params: [severity, beat.min(1.0), last],
        })
    });
    let stamina_amount = client
        .ui
        .stamina_preview
        .get()
        .map(|value| (value, client.ui.exhausted_preview))
        .or_else(|| {
            client
                .stamina
                .map(|value| (f32::from(value) / 1000.0, client.exhausted))
        });
    let stamina = if let Some((value, exhausted)) = stamina_amount {
        let view = &mut visual.stamina;
        view.value += (value - view.value) * (12.0 * delta).min(1.0);
        view.exhausted =
            (view.exhausted + if exhausted { delta / 0.4 } else { -delta / 0.8 }).clamp(0.0, 1.0);
        view.exhausted_now = exhausted;
        if value < 0.999 {
            view.full_since = None;
        } else if view.full_since.is_none() {
            view.full_since = Some(now);
        }
        let visible = view
            .full_since
            .is_none_or(|at| (now - at).as_secs_f32() < 1.2);
        view.alpha =
            (view.alpha + if visible { delta * 6.0 } else { -delta * 2.0 }).clamp(0.0, 1.0);
        view.breath = 0.5 - 0.5 * (postfx::shader_time() * std::f32::consts::TAU * 0.85).cos();
        let tired = ((0.5 - view.value) / 0.5).clamp(0.0, 1.0);
        let cfg = &client.settings.stamina;
        (cfg.vignette && (tired > 0.0 || view.exhausted > 0.0)).then_some(Effect {
            layer: Layer::new(cfg.cover_game_hud, cfg.cover_mod_hud),
            params: [tired, view.exhausted, view.breath],
        })
    } else {
        visual.stamina = StaminaVisual::default();
        None
    };
    let infection = client
        .screamer
        .as_ref()
        .filter(|_| client.settings.infection.effect && postfx::infection_ready())
        .map(|effect| Effect {
            layer: Layer::AfterInventory,
            params: [
                0.92,
                0.5 + 0.5 * (effect.started.elapsed().as_secs_f32() * 40.0).sin(),
                0.7,
            ],
        })
        .or(infection);
    visual.frame = Frame { infection, stamina };
    postfx::publish(visual.frame);
}

pub fn draw_effects(
    canvas: &mut impl Canvas,
    client: &Client,
    width: i32,
    height: i32,
    foreground: bool,
) {
    let size = width.min(height) as f32;
    let frame = client.visuals.frame;
    let on_layer = |effect: &Effect| (effect.layer == Layer::AfterInventory) == foreground;
    if let Some(effect) = frame
        .stamina
        .filter(on_layer)
        .filter(|_| !postfx::stamina_ready())
    {
        let [tired, exhausted, breath] = effect.params;
        let depth = (0.12 + 0.16 * tired + 0.06 * exhausted * breath) * size;
        let alpha = (120.0 * tired + 70.0 * exhausted * (0.6 + 0.4 * breath)).clamp(0.0, 220.0);
        vignette(canvas, width, height, depth, (alpha as u32) << 24);
    }
    if let Some(effect) = frame
        .infection
        .filter(on_layer)
        .filter(|_| !postfx::infection_ready())
    {
        let [severity, pulse, last] = effect.params;
        let alpha = (18.0 + 70.0 * severity + 25.0 * severity * pulse).clamp(0.0, 140.0) as u32;
        let full = rect(0, 0, width, height);
        canvas.rect(full, rgb(90, 125, 25) | (alpha << 24));
        let depth = (0.10 + 0.30 * severity + 0.05 * severity * pulse) * size;
        let edge = (90.0 + 150.0 * severity + 30.0 * pulse).clamp(0.0, 245.0) as u32;
        vignette(canvas, width, height, depth, rgb(10, 30, 5) | (edge << 24));
        let flash = (60.0 * pulse * last) as u32;
        if flash > 0 {
            canvas.rect(full, rgb(150, 20, 10) | (flash << 24));
        }
    }
}

fn vignette(canvas: &mut impl Canvas, width: i32, height: i32, depth: f32, color: COLORREF) {
    let edge = (color >> 24) as f32 / 255.0;
    if depth < 1.0 || edge <= 0.0 {
        return;
    }
    let single = (255.0 * (1.0 - (1.0 - edge.min(0.995)).powf(1.0 / 12.0))) as u32;
    if single == 0 {
        return;
    }
    let color = (color & 0x00ff_ffff) | (single << 24);
    for i in 1..=12 {
        let d = (depth * i as f32 / 12.0).round() as i32;
        canvas.rect(rect(0, 0, width, d), color);
        canvas.rect(rect(0, height - d, width, d), color);
        canvas.rect(rect(0, 0, d, height), color);
        canvas.rect(rect(width - d, 0, d, height), color);
    }
}

fn capsule(canvas: &mut impl Canvas, bounds: RECT, color: COLORREF) {
    use std::f32::consts::{FRAC_PI_2, PI};
    if color >> 24 == 0 || bounds.left >= bounds.right {
        return;
    }
    let radius =
        ((bounds.bottom - bounds.top) as f32 * 0.5).min((bounds.right - bounds.left) as f32 * 0.5);
    let y = (bounds.top + bounds.bottom) as f32 * 0.5;
    let mut polygon = [(0.0, 0.0); 18];
    for (i, point) in polygon.iter_mut().enumerate() {
        let right = i < 9;
        let x = if right {
            bounds.right as f32 - radius
        } else {
            bounds.left as f32 + radius
        };
        let angle = (if right { -FRAC_PI_2 } else { FRAC_PI_2 }) + (i % 9) as f32 * PI / 8.0;
        let (sin, cos) = angle.sin_cos();
        *point = (x + radius * cos, y + radius * sin);
    }
    canvas.polygon(&polygon, color);
}

fn draw_stamina(canvas: &mut impl Canvas, client: &Client, width: i32, height: i32) {
    let view = &client.visuals.stamina;
    if !client.settings.stamina.bar || view.alpha <= 0.01 {
        return;
    }
    let scale = client.settings.hud.scale * (height as f32 / 900.0).max(1.0);
    let w = (150.0 * scale).round() as i32;
    let h = (5.0 * scale).round().max(3.0) as i32;
    let x = (width - w) / 2;
    let y = (height as f32 * 0.5 + 70.0 * scale).round() as i32;
    let alpha = view.alpha;
    capsule(
        canvas,
        rect(x - 1, y - 1, w + 2, h + 2),
        ((120.0 * alpha) as u32) << 24,
    );
    let (color, strength) = if view.exhausted_now {
        (rgb(225, 45, 30), 255.0 * (0.55 + 0.45 * view.breath))
    } else if view.value < 0.3 {
        (rgb(240, 150, 40), 230.0)
    } else {
        (rgb(235, 235, 225), 210.0)
    };
    capsule(
        canvas,
        rect(
            x,
            y,
            (view.value.clamp(0.0, 1.0) * w as f32).round() as i32,
            h,
        ),
        color | (((strength * alpha) as u32) << 24),
    );
}

fn draw_melee(canvas: &mut impl Canvas, client: &Client, width: i32, height: i32) {
    use inventory_mod::melee::WEAR_BROKEN;
    use std::f32::consts::{FRAC_PI_2, TAU};
    if !client.settings.melee_ring || !client.playing() {
        return;
    }
    let Some(melee) = client.melee.as_ref().filter(|melee| melee.has_ring()) else {
        return;
    };
    let scale = client.settings.hud.scale * (height as f32 / 900.0).max(1.0);
    let center = ((width as f32 * 0.5).round(), (height as f32 * 0.5).round());
    let radius = 26.0 * scale;
    let thick = (3.0 * scale).max(2.0);
    let arc = |canvas: &mut _, radius, start, end, thickness, color| {
        crate::wheel::ring(
            canvas,
            center,
            radius - thickness * 0.5,
            radius + thickness * 0.5,
            start,
            end,
            color,
        );
    };
    if let Some((held, charge)) = melee.charge() {
        let alpha = ((held - 0.12) / 0.1).clamp(0.0, 1.0);
        if alpha > 0.0 {
            arc(
                canvas,
                radius,
                0.0,
                TAU,
                thick + 2.0,
                ((110.0 * alpha) as u32) << 24,
            );
            let full = charge >= 1.0 && !client.exhausted;
            let pulse = if full {
                0.5 + 0.5 * (postfx::shader_time() * 12.0).sin()
            } else {
                0.0
            };
            let (ink, amount) = if client.exhausted {
                (rgb(150, 150, 150), 150.0)
            } else if full {
                (rgb(240, 150, 40), 200.0 + 55.0 * pulse)
            } else {
                (rgb(235, 235, 225), 220.0)
            };
            arc(
                canvas,
                radius,
                -FRAC_PI_2,
                TAU * charge - FRAC_PI_2,
                thick + pulse,
                ink | (((amount * alpha) as u32) << 24),
            );
        }
    }
    if let Some(wear) = client
        .snapshot
        .as_ref()
        .and_then(|state| {
            state
                .native
                .iter()
                .find(|weapon| weapon.active && !weapon.melee.is_empty())
        })
        .and_then(|weapon| weapon.wear)
        .filter(|&wear| wear >= WEAR_BROKEN / 2)
    {
        let left = (1.0 - wear as f32 / WEAR_BROKEN as f32).clamp(0.0, 1.0);
        let width = (40.0 * scale).round() as i32;
        let height = (3.0 * scale).max(2.0).round() as i32;
        let x = center.0 as i32 - width / 2;
        let y = (center.1 + radius + 8.0 * scale).round() as i32;
        canvas.rect(rect(x, y, width, height), 0x7800_0000);
        canvas.rect(
            rect(x, y, (width as f32 * left).round() as i32, height),
            if left > 0.2 { 0xdc3c_c8f0 } else { 0xe632_3ce6 },
        );
    }
    if let Some(progress) = melee.heavy_flash() {
        arc(
            canvas,
            radius * (1.0 + 0.6 * progress),
            0.0,
            TAU,
            thick * (1.0 - 0.5 * progress),
            rgb(240, 150, 40) | (((230.0 * (1.0 - progress)) as u32) << 24),
        );
    }
}

pub fn draw_indicators(canvas: &mut impl Canvas, client: &Client, width: i32, height: i32) {
    draw_stamina(canvas, client, width, height);
    draw_melee(canvas, client, width, height);
    if !client.playing() {
        return;
    }
    let now = Instant::now();
    let hud = &client.settings.hud;
    if !(hud.replace_native_panel && hud.show && hud.view.shows_slots())
        && let Some((line, _)) = client
            .panel_notice
            .as_ref()
            .filter(|(_, until)| *until > now)
    {
        let (w, _) = canvas.text_size(line);
        canvas.text(
            ((width - w) / 2, (height as f32 * 0.72) as i32),
            line,
            crate::hud::TEXT_SELECTED,
        );
    }

    draw_pings(canvas, client, width, height, now);
}

fn draw_pings(canvas: &mut impl Canvas, client: &Client, width: i32, height: i32, now: Instant) {
    use inventory_mod::protocol::PingKind;
    if client.pings.is_empty() {
        return;
    }
    let Some(projection) = client.link().projection() else {
        return;
    };
    let scale = client.settings.hud.scale * (height as f32 / 900.0).max(1.0);
    let center = (width as f32 * 0.5, height as f32 * 0.5);
    let margin = (40.0 * scale).min(center.0.min(center.1) - 1.0).max(0.0);
    for mark in client.pings.iter().filter(|mark| now < mark.until) {
        let Some((mut x, mut y, in_front, distance)) =
            projection.point(mark.ping.position, width, height)
        else {
            continue;
        };
        let age = now.saturating_duration_since(mark.born).as_secs_f32();
        let left = (mark.until - now).as_secs_f32();
        let alpha = (left / 0.8).clamp(0.0, 1.0) * (age / 0.06).clamp(0.2, 1.0);
        let pop = 1.0 + 0.7 * (1.0 - age / 0.25).max(0.0);
        let edge = !in_front
            || x < margin
            || x > width as f32 - margin
            || y < margin
            || y > height as f32 - margin;
        let direction = if edge {
            let dx = x - center.0;
            let mut dy = y - center.1;
            if dx.abs() < 1.0 && dy.abs() < 1.0 {
                dy = 1.0;
            }
            let length = dx.hypot(dy);
            let (nx, ny) = (dx / length, dy / length);
            let reach = ((center.0 - margin) / nx.abs().max(1e-6))
                .min((center.1 - margin) / ny.abs().max(1e-6));
            x = center.0 + nx * reach;
            y = center.1 + ny * reach;
            Some((nx, ny))
        } else {
            None
        };
        let ink = match mark.ping.kind {
            PingKind::Enemy => 0xff3c_46eb,
            PingKind::Item => 0xff46_c8fa,
            _ => 0xfff0_cd55,
        };
        let fade = |color: COLORREF| {
            (color & 0x00ff_ffff) | ((((color >> 24) as f32 * alpha) as u32) << 24)
        };
        let radius = if edge { 9.0 } else { 11.0 } * scale * pop;
        for (r, color) in [
            (radius + 2.0 * scale, 0xc800_0000),
            (radius, ink),
            (radius * 0.42, 0xdc00_0000),
        ] {
            canvas.polygon(
                &[(x, y - r), (x + r, y), (x, y + r), (x - r, y)],
                fade(color),
            );
        }
        if let Some((dx, dy)) = direction {
            let tip = (
                x + dx * (radius + 11.0 * scale),
                y + dy * (radius + 11.0 * scale),
            );
            let base = (
                x + dx * (radius + 3.0 * scale),
                y + dy * (radius + 3.0 * scale),
            );
            let side = (-dy * 5.0 * scale, dx * 5.0 * scale);
            canvas.polygon(
                &[
                    tip,
                    (base.0 + side.0, base.1 + side.1),
                    (base.0 - side.0, base.1 - side.1),
                ],
                fade(ink),
            );
        }
        let mut y = (y + radius + 4.0 * scale).round() as i32;
        if let Some(distance) = distance {
            let distance = format!("{distance} m");
            let (w, h) = canvas.measure(Font::Hint, &distance);
            canvas.styled_text(
                Font::Hint,
                (x.round() as i32 - w / 2, y),
                &distance,
                fade(0xffff_ffff),
            );
            y += h;
        }
        if !mark.ping.name.is_empty() {
            let w = canvas.measure(Font::Text, &mark.ping.name).0;
            canvas.styled_text(
                Font::Text,
                (x.round() as i32 - w / 2, y),
                &mark.ping.name,
                fade(ink),
            );
        }
    }
}

pub fn mouse(message: u32, lparam: LPARAM, window: HWND) -> bool {
    let result = with_client(|client| {
        if !client.open {
            return None;
        }
        let viewport = if let Some(vgui) = client.vgui.as_ref().filter(|vgui| vgui.active()) {
            vgui.size()
        } else if postfx::drawable() {
            let mut rect = RECT::default();
            if unsafe { GetClientRect(window, &mut rect) } == 0 {
                return None;
            }
            (rect.right, rect.bottom)
        } else {
            return None;
        };
        let panel = client.ui.bounds(viewport.0, viewport.1);
        let point = (
            i32::from(lparam as i16) - panel.left,
            i32::from((lparam >> 16) as i16) - panel.top,
        );
        let mut ui = std::mem::take(&mut client.ui);
        let action = ui.mouse(client, message, point, viewport);
        client.ui = ui;
        let command = action.and_then(|action| client.inventory_action(action));
        Some((client.link(), command))
    })
    .flatten();
    let Some((link, command)) = result else {
        return false;
    };
    if let Some(command) = command {
        send(command, link);
    }
    true
}
