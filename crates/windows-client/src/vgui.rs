use std::{
    cell::Cell,
    collections::BTreeMap,
    ffi::{CStr, CString, c_char, c_void},
    io,
    path::PathBuf,
    ptr,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
    time::{Duration, Instant},
};

use inventory_source_assets::icons::Icons;
use windows_sys::Win32::{
    Foundation::{COLORREF, RECT},
    System::LibraryLoader::GetProcAddress,
};

use crate::{
    client::{Client, with_client},
    native::{self, Detour, matches_pattern},
    overlay::{self, Canvas, Font as HudFont},
    postfx::{self, Layer},
};

type Factory = unsafe extern "C" fn(*const c_char, *mut i32) -> *mut c_void;
type Paint = unsafe extern "thiscall" fn(*mut c_void, u32, bool, bool);
type Name = unsafe extern "thiscall" fn(*mut c_void, u32) -> *const c_char;
type Focus = unsafe extern "thiscall" fn(*mut c_void) -> u32;
type PanelRect = unsafe extern "thiscall" fn(*mut c_void, u32, *mut i32, *mut i32);
type SurfaceAlpha = unsafe extern "thiscall" fn(*mut c_void) -> f32;
type SchemeTag = unsafe extern "thiscall" fn(*mut c_void, *const c_char) -> u32;
type SchemeAt = unsafe extern "thiscall" fn(*mut c_void, u32) -> *mut c_void;
type FontFor = unsafe extern "thiscall" fn(*mut c_void, *const c_char, bool) -> u32;
type Color = unsafe extern "thiscall" fn(*mut c_void, i32, i32, i32, i32);
type Rect = unsafe extern "thiscall" fn(*mut c_void, i32, i32, i32, i32);
type Font = unsafe extern "thiscall" fn(*mut c_void, u32);
type Position = unsafe extern "thiscall" fn(*mut c_void, i32, i32);
type Text = unsafe extern "thiscall" fn(*mut c_void, *const u16, i32, i32);
type TextSize = unsafe extern "thiscall" fn(*mut c_void, u32, *const u16, *mut i32, *mut i32);
type Size = unsafe extern "thiscall" fn(*mut c_void, *mut i32, *mut i32);
type SetTexture = unsafe extern "thiscall" fn(*mut c_void, i32);
type CreateTexture = unsafe extern "thiscall" fn(*mut c_void, bool) -> i32;
type TextureFile = unsafe extern "thiscall" fn(*mut c_void, i32, *const c_char, i32, bool);
type Polygon = unsafe extern "thiscall" fn(*mut c_void, i32, *mut Vertex, bool);
type Play = unsafe extern "thiscall" fn(*mut c_void, *const c_char);
type Upload = unsafe extern "thiscall" fn(*mut c_void, i32, *const u8, i32, i32);
type TexturedRect =
    unsafe extern "thiscall" fn(*mut c_void, i32, i32, i32, i32, f32, f32, f32, f32);

static ORIGINAL_PAINT: AtomicUsize = AtomicUsize::new(0);
thread_local! { static PAINT_DEPTH: Cell<u32> = const { Cell::new(0) }; }

struct Scoreboard {
    size: PanelRect,
    position: PanelRect,
    alpha: Option<SurfaceAlpha>,
    title: Option<(i32, i32, f32)>,
}

pub struct Vgui {
    detour: Detour,
    panels: usize,
    panel_name: Name,
    focus_panel: Option<u32>,
    input: Option<(usize, Focus)>,
    last_draw: Option<Instant>,
    schemes: usize,
    scheme_handle: SchemeTag,
    scheme_at: SchemeAt,
    expected_scheme: usize,
    fonts: Option<[u32; 4]>,
    font_size: (i32, i32),
    white: Option<i32>,
    screamer: ScreamerTexture,
    next_font_lookup: Option<Instant>,
    surface: Surface,
    sheets: BTreeMap<String, i32>,
    scoreboard: Option<Scoreboard>,
}

struct Surface {
    instance: usize,
    set_color: Color,
    filled_rect: Rect,
    set_font: Font,
    set_text_color: Color,
    set_text_position: Position,
    print_text: Text,
    text_size: TextSize,
    layout: Size,
    set_texture: SetTexture,
    create_texture: CreateTexture,
    texture_file: TextureFile,
    textured_polygon: Polygon,
    play: Play,
    upload: Upload,
    textured_rect: TexturedRect,
}

#[derive(Default)]
struct ScreamerTexture {
    path: Option<PathBuf>,
    texture: Option<i32>,
    aspect: Option<f32>,
}

#[repr(C)]
struct Vertex {
    x: f32,
    y: f32,
    u: f32,
    v: f32,
}

fn module_factory(name: &str) -> Option<(usize, Factory)> {
    let module = native::module(name)?;
    let export = unsafe { GetProcAddress(module.as_ptr(), c"CreateInterface".as_ptr().cast()) }?;
    let factory =
        unsafe { std::mem::transmute::<unsafe extern "system" fn() -> isize, Factory>(export) };
    Some((module.as_ptr().addr(), factory))
}

fn interface(factory: Factory, version: &CStr, table: usize) -> Option<usize> {
    let object = unsafe { factory(version.as_ptr(), ptr::null_mut()) }.addr();
    if object != 0 && native::word(object).map(|value| value as usize) == Some(table) {
        Some(object)
    } else {
        None
    }
}

fn slot(object: usize, index: usize, pattern: &str) -> Option<usize> {
    native::vslot(object, index).filter(|&function| matches_pattern(function, pattern))
}

macro_rules! method {
    ($type:ty, $object:expr, $index:literal, $pattern:literal) => {{
        let address = slot($object, $index, $pattern)?;
        // SAFETY: Each wrapper names a verified game vtable slot and its x86 ABI.
        unsafe { std::mem::transmute::<usize, $type>(address) }
    }};
}

impl Vgui {
    pub fn install() -> Option<Self> {
        let (vgui, valve) = module_factory("vgui2.dll")?;
        let (surface, material) = module_factory("vguimatsurface.dll")?;
        let panels = interface(valve, c"VGUI_Panel009", vgui + 0x480cc)?;
        let schemes = interface(valve, c"VGUI_Scheme010", vgui + 0x46fcc)?;
        let instance = interface(material, c"VGUI_Surface031", surface + 0xbef2c)?;
        let input = unsafe { valve(c"VGUI_Input005".as_ptr(), ptr::null_mut()) }.addr();
        let image = native::module("vgui2.dll").and_then(native::PeImage::loaded)?;
        let input = native::vslot(input, 4)
            .filter(|&address| image.includes(address, 1))
            .map(|address| {
                (input, unsafe {
                    std::mem::transmute::<usize, Focus>(address)
                })
            });
        let name = slot(
            panels,
            36,
            "55 8B EC 8B 4D 08 8B 01 8B 90 8C 00 00 00 FF D2",
        )?;
        let paint = slot(
            panels,
            41,
            "55 8B EC 8B 01 8B 55 08 8B 80 04 01 00 00 52 FF",
        )?;
        if name != vgui + 0x1eac0 || paint != vgui + 0x1ebe0 {
            return None;
        }
        let panel_name = unsafe { std::mem::transmute::<usize, Name>(name) };
        let scoreboard = (|| {
            let size = method!(
                PanelRect,
                panels,
                5,
                "55 8B EC 8B 55 10 8B 4D 08 8B 01 8B 40 34 52 8B"
            );
            let position = method!(
                PanelRect,
                panels,
                10,
                "55 8B EC 8B 55 10 8B 4D 08 8B 01 8B 40 48 52 8B"
            );
            let alpha = slot(instance, 106, "D9 41 10 C3")
                .map(|address| unsafe { std::mem::transmute::<usize, SurfaceAlpha>(address) });
            Some(Scoreboard {
                size,
                position,
                alpha,
                title: None,
            })
        })();
        let scheme_handle = method!(
            SchemeTag,
            schemes,
            5,
            "55 8B EC 53 56 57 8B F9 BE 01 00 00 00 39 77 10"
        );
        let scheme_at = method!(
            SchemeAt,
            schemes,
            8,
            "55 8B EC 8B 45 08 3B 41 10 72 06 33 C0"
        );
        let surface = Surface {
            instance,
            set_color: method!(Color, instance, 11, "55 8B EC 8A 45 08 8A 55 0C"),
            filled_rect: method!(
                Rect,
                instance,
                12,
                "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00"
            ),
            set_font: method!(Font, instance, 17, "55 8B EC 8B 45 08 89 81 94 02 00 00"),
            set_text_color: method!(Color, instance, 19, "55 8B EC 53 56 8B F1 0F B6 46 28"),
            set_text_position: method!(
                Position,
                instance,
                20,
                "55 8B EC 8B 45 08 8B 55 0C 89 41 2C 89 51 30"
            ),
            print_text: method!(
                Text,
                instance,
                22,
                "55 8B EC 8B 45 10 8B 55 0C 50 8B 45 08 6A 00 52"
            ),
            text_size: method!(
                TextSize,
                instance,
                72,
                "55 8B EC 8B 45 14 8B 4D 10 8B 55 0C 50 8B 45 08"
            ),
            layout: method!(
                Size,
                instance,
                35,
                "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00"
            ),
            set_texture: method!(
                SetTexture,
                instance,
                29,
                "55 8B EC 56 57 8B 7D 08 8B F1 3B BE A0 02 00 00"
            ),
            create_texture: method!(
                CreateTexture,
                instance,
                34,
                "55 8B EC E8 ?? ?? ?? ?? 8B 10 8B C8 8B 12 5D FF"
            ),
            texture_file: method!(
                TextureFile,
                instance,
                27,
                "55 8B EC 56 57 8B F1 E8 ?? ?? ?? ?? 8B 4D 0C"
            ),
            textured_polygon: method!(Polygon, instance, 99, "55 8B EC 51 8B 45 08 56 8B F1 85 C0"),
            play: method!(Play, instance, 75, "55 8B EC 8B 81 D0 02 01 00 85 C0"),
            upload: method!(
                Upload,
                instance,
                28,
                "55 8B EC E8 ?? ?? ?? ?? 8B 4D 14 8B 10"
            ),
            textured_rect: method!(
                TexturedRect,
                instance,
                98,
                "55 8B EC 83 EC 50 56 8B F1 80 7E 27 00"
            ),
        };

        let mut detour = Detour::prepare(paint, paint_traverse as Paint as usize).ok()?;
        ORIGINAL_PAINT.store(detour.trampoline(), Ordering::Release);
        if detour.enable().is_err() {
            ORIGINAL_PAINT.store(0, Ordering::Release);
            return None;
        }
        Some(Self {
            detour,
            panels,
            panel_name,
            focus_panel: None,
            input,
            last_draw: None,
            schemes,
            scheme_handle,
            scheme_at,
            expected_scheme: vgui + 0x47010,
            fonts: None,
            font_size: (0, 0),
            white: None,
            screamer: ScreamerTexture::default(),
            next_font_lookup: None,
            surface,
            sheets: BTreeMap::new(),
            scoreboard,
        })
    }

    pub fn stop(&mut self) -> io::Result<()> {
        self.detour.disable()?;
        ORIGINAL_PAINT.store(0, Ordering::Release);
        Ok(())
    }

    pub fn active(&self) -> bool {
        self.last_draw
            .is_some_and(|draw| draw.elapsed() < Duration::from_millis(500))
    }

    pub fn typing(&self) -> bool {
        self.input.is_some_and(|(instance, focus)| unsafe {
            focus(ptr::with_exposed_provenance_mut(instance)) != 0
        })
    }

    pub fn size(&self) -> (i32, i32) {
        self.surface.size()
    }

    pub fn tick_sound(&self) {
        self.play_sound("ui/menu_focus.wav");
    }

    pub fn play_sound(&self, path: &str) {
        if let Ok(path) = CString::new(path) {
            unsafe { (self.surface.play)(self.surface.object(), path.as_ptr()) };
        }
    }

    fn is_focus(&mut self, panel: u32) -> bool {
        if let Some(focus) = self.focus_panel {
            return focus == panel;
        }
        let name =
            unsafe { (self.panel_name)(ptr::with_exposed_provenance_mut(self.panels), panel) };
        let focus = native::string(name.addr(), 64).as_deref() == Some("FocusOverlayPanel");
        if focus {
            self.focus_panel = Some(panel);
        }
        focus
    }

    fn scoreboard(&mut self, panel: u32, outer: bool, label: Option<&str>, icons: &Icons) {
        let Some(mut scoreboard) = self.scoreboard.take() else {
            return;
        };
        if outer {
            if let Some(((x, y, alpha), label)) = scoreboard.title.take().zip(label)
                && let Some(fonts) = self.fonts
            {
                let color = ((alpha.clamp(0.0, 1.0) * 255.0).round() as u32) << 24;
                if color != 0 {
                    let mut canvas = NativeCanvas {
                        surface: &self.surface,
                        fonts,
                        white: self.white,
                        icons,
                        sheets: &mut self.sheets,
                    };
                    canvas.text((x + 1, y + 1), label, color);
                    canvas.text((x, y), label, color | 0x00e6_e6e6);
                }
            }
        } else if label.is_some() {
            let panels = ptr::with_exposed_provenance_mut(self.panels);
            let name = unsafe { (self.panel_name)(panels, panel) };
            if native::string(name.addr(), 48).as_deref() == Some("MissionTitle") {
                let (mut x, mut y, mut w, mut h) = (0, 0, 0, 0);
                let alpha = unsafe {
                    (scoreboard.position)(panels, panel, &mut x, &mut y);
                    (scoreboard.size)(panels, panel, &mut w, &mut h);
                    scoreboard
                        .alpha
                        .map_or(1.0, |read| read(self.surface.object()))
                };
                scoreboard.title = Some((x, y + h, alpha));
            }
        }
        self.scoreboard = Some(scoreboard);
    }

    fn update_font(&mut self, size: (i32, i32)) {
        let now = Instant::now();
        if self.font_size == size
            && (self.fonts.is_some() || self.next_font_lookup.is_some_and(|next| now < next))
        {
            return;
        }
        self.font_size = size;
        self.next_font_lookup = Some(now + Duration::from_secs(2));
        self.fonts = self.resolve_fonts();
    }

    fn resolve_fonts(&self) -> Option<[u32; 4]> {
        let manager = ptr::with_exposed_provenance_mut(self.schemes);
        let id = unsafe { (self.scheme_handle)(manager, c"ClientScheme".as_ptr()) };
        if id == 0 {
            return None;
        }
        let scheme = unsafe { (self.scheme_at)(manager, id) };
        if scheme.is_null()
            || native::word(scheme.addr()).map(|addr| addr as usize) != Some(self.expected_scheme)
        {
            return None;
        }
        let font = method!(FontFor, scheme.addr(), 3, "55 8B EC 56 8B F1 8D 4E 18 E8");
        let get = |name: &CStr| {
            [true, false].into_iter().find_map(|sized| {
                let id = unsafe { font(scheme, name.as_ptr(), sized) };
                (id != 0).then_some(id)
            })
        };
        let body = get(c"MessageText").or_else(|| get(c"Default"))?;
        Some([
            get(c"PlayerDisplayName").unwrap_or(body),
            get(c"HudHintText").unwrap_or(body),
            get(c"HudAmmoSmall").unwrap_or(body),
            body,
        ])
    }

    fn draw(&mut self, client: &mut Client) {
        if !client.link().in_game() && !client.open {
            return;
        }
        let size = self.surface.size();
        if size.0 < 1 || size.1 < 1 {
            return;
        }
        self.update_font(size);
        let Some(fonts) = self.fonts else {
            return;
        };
        self.last_draw = Some(Instant::now());
        postfx::publish_drawing(None);
        if self.white.is_none() {
            let id = unsafe { (self.surface.create_texture)(self.surface.object(), false) };
            if id > 0 {
                unsafe {
                    (self.surface.texture_file)(
                        self.surface.object(),
                        id,
                        c"vgui/white".as_ptr(),
                        0,
                        false,
                    )
                };
                self.white = Some(id);
            }
        }
        let icons = Arc::clone(&client.icons);
        let mut canvas = NativeCanvas {
            surface: &self.surface,
            fonts,
            white: self.white,
            icons: &icons,
            sheets: &mut self.sheets,
        };
        overlay::update_effects(client);
        overlay::draw_effects(&mut canvas, client, size.0, size.1, false);
        overlay::draw_indicators(&mut canvas, client, size.0, size.1);
        crate::hud::paint(&mut canvas, client, size.0, size.1);
        if let Some(wheel) = &mut client.wheel {
            wheel.draw(&mut canvas, size.0, size.1);
        }
        postfx::schedule(Layer::BeforeInventory);
        if overlay::show_panel(client) {
            let bounds = overlay::panel_bounds(client, size.0, size.1);
            overlay::draw_panel(&mut canvas, client, bounds);
            let at = client
                .ui
                .pointer()
                .map_or((size.0 / 2, size.1 / 2), |(x, y)| {
                    (bounds.left + x, bounds.top + y)
                });
            crate::hud::pointer(
                &mut canvas,
                (at.0 as f32, at.1 as f32),
                (size.1 as f32 / 900.0).max(1.0),
            );
        }
        overlay::draw_effects(&mut canvas, client, size.0, size.1, true);
        if let Some(effect) = &client.screamer {
            self.screamer.draw(&mut canvas, effect, size);
        }
        postfx::schedule(Layer::AfterInventory);
    }
}

impl ScreamerTexture {
    fn draw(
        &mut self,
        canvas: &mut NativeCanvas<'_>,
        effect: &crate::client::Surprise,
        (width, height): (i32, i32),
    ) {
        let time = effect.started.elapsed().as_secs_f32();
        if time >= 1.7 {
            return;
        }
        if self.path.as_ref() != Some(&effect.path) {
            self.path = Some(effect.path.clone());
            self.aspect = None;
            let picture = image::ImageReader::open(&effect.path)
                .ok()
                .and_then(|image| image.decode().ok());
            if let Some(picture) = picture {
                let pixels = picture.into_rgba8();
                let (w, h) = pixels.dimensions();
                if (1..=4096).contains(&w) && (1..=4096).contains(&h) {
                    let surface = canvas.surface.object();
                    let texture = self.texture.unwrap_or_else(|| unsafe {
                        (canvas.surface.create_texture)(surface, true)
                    });
                    if texture > 0 {
                        unsafe {
                            (canvas.surface.upload)(
                                surface,
                                texture,
                                pixels.as_ptr(),
                                w as i32,
                                h as i32,
                            )
                        };
                        self.texture = Some(texture);
                        self.aspect = Some(w as f32 / h as f32);
                    }
                }
            }
        }
        let fade = ((1.7 - time) / 0.45).clamp(0.0, 1.0);
        let bounds = RECT {
            left: 0,
            top: 0,
            right: width,
            bottom: height,
        };
        canvas.rect(bounds, ((fade * 255.0) as u32).max(1) << 24);
        let roll = ((postfx::shader_time() * 1000.0) as u32).wrapping_mul(2654435761);
        if let Some((texture, aspect)) = self.texture.zip(self.aspect) {
            let zoom = 1.04 + 0.30 * (1.0 - (-9.0 * time).exp()) + 0.06 * time;
            let h = (width as f32 / aspect).max(height as f32) * zoom;
            let w = h * aspect;
            let shake = 34.0 * (1.0 - time / 1.2).max(0.0) * (height as f32 / 1080.0);
            let x = (width as f32 - w) * 0.5 + shake * ((roll & 255) as f32 / 127.5 - 1.0);
            let y = (height as f32 - h) * 0.5 + shake * (((roll >> 8) & 255) as f32 / 127.5 - 1.0);
            let shade = if time > 0.2 && (roll >> 16) & 7 == 0 {
                110
            } else {
                255
            };
            let surface = canvas.surface.object();
            unsafe {
                (canvas.surface.set_texture)(surface, texture);
                (canvas.surface.set_color)(surface, shade, shade, shade, (255.0 * fade) as i32);
                (canvas.surface.textured_rect)(
                    surface,
                    x as i32,
                    y as i32,
                    (x + w) as i32,
                    (y + h) as i32,
                    0.0,
                    0.0,
                    1.0,
                    1.0,
                );
            }
        }
        let pulse = (time * 40.0).sin();
        let red = if self.texture.zip(self.aspect).is_some() {
            70.0 * (0.25 + 0.2 * pulse) * fade
        } else {
            200.0 * fade * (0.6 + 0.4 * pulse)
        };
        if red >= 1.0 {
            canvas.rect(bounds, 0x0000_0096 | ((red as u32) << 24));
        }
        let flash = if time < 0.08 {
            220.0 * (1.0 - time / 0.08)
        } else {
            0.0
        };
        if flash >= 1.0 {
            canvas.rect(bounds, 0x00ff_ffff | ((flash as u32) << 24));
        }
    }
}

impl Surface {
    fn object(&self) -> *mut c_void {
        ptr::with_exposed_provenance_mut(self.instance)
    }

    fn size(&self) -> (i32, i32) {
        let (mut width, mut height) = (0, 0);
        unsafe { (self.layout)(self.object(), &mut width, &mut height) };
        (width, height)
    }
}

struct NativeCanvas<'a> {
    surface: &'a Surface,
    fonts: [u32; 4],
    white: Option<i32>,
    icons: &'a Icons,
    sheets: &'a mut BTreeMap<String, i32>,
}

fn channels(color: COLORREF) -> [i32; 3] {
    [
        (color & 255) as i32,
        ((color >> 8) & 255) as i32,
        ((color >> 16) & 255) as i32,
    ]
}

impl Canvas for NativeCanvas<'_> {
    fn rect(&mut self, bounds: RECT, color: COLORREF) {
        let [r, g, b] = channels(color);
        let alpha = (color >> 24) as i32;
        let surface = self.surface.object();
        unsafe {
            (self.surface.set_color)(surface, r, g, b, if alpha == 0 { 235 } else { alpha });
            (self.surface.filled_rect)(
                surface,
                bounds.left,
                bounds.top,
                bounds.right,
                bounds.bottom,
            );
        }
    }

    fn text(&mut self, at: (i32, i32), label: &str, color: COLORREF) {
        self.styled_text(HudFont::Text, at, label, color);
    }

    fn styled_text(&mut self, font: HudFont, at: (i32, i32), label: &str, color: COLORREF) {
        let text = label.encode_utf16().collect::<Vec<_>>();
        let [r, g, b] = channels(color);
        let surface = self.surface.object();
        unsafe {
            (self.surface.set_font)(surface, self.fonts[font as usize]);
            (self.surface.set_text_color)(surface, r, g, b, alpha(color));
            (self.surface.set_text_position)(surface, at.0, at.1);
            (self.surface.print_text)(surface, text.as_ptr(), text.len() as i32, 0);
        }
    }

    fn text_size(&self, label: &str) -> (i32, i32) {
        self.measure(HudFont::Text, label)
    }

    fn measure(&self, font: HudFont, label: &str) -> (i32, i32) {
        let text = native::wide(label);
        let (mut width, mut height) = (0, 0);
        unsafe {
            (self.surface.text_size)(
                self.surface.object(),
                self.fonts[font as usize],
                text.as_ptr(),
                &mut width,
                &mut height,
            )
        };
        (width, height)
    }

    fn icon(&mut self, name: &str, bounds: RECT, color: COLORREF) -> bool {
        let Some(sprite) = self.icons.get(name) else {
            return false;
        };
        let surface = self.surface.object();
        let texture = if let Some(&texture) = self.sheets.get(&sprite.material) {
            texture
        } else {
            let Ok(path) = CString::new(sprite.material.as_str()) else {
                return false;
            };
            let texture = unsafe { (self.surface.create_texture)(surface, false) };
            if texture > 0 {
                unsafe { (self.surface.texture_file)(surface, texture, path.as_ptr(), 1, false) };
            }
            self.sheets.insert(sprite.material.clone(), texture);
            texture
        };
        if texture <= 0 {
            return false;
        }
        let bounds = overlay::fit(bounds, sprite.aspect());
        let [u0, v0, u1, v1] = sprite.uv();
        let [r, g, b] = channels(color);
        let alpha = alpha(color);
        unsafe {
            (self.surface.set_texture)(surface, texture);
            (self.surface.set_color)(surface, r, g, b, alpha);
            (self.surface.textured_rect)(
                surface,
                bounds.left,
                bounds.top,
                bounds.right,
                bounds.bottom,
                u0,
                v0,
                u1,
                v1,
            );
        }
        true
    }

    fn polygon(&mut self, points: &[(f32, f32)], color: COLORREF) {
        let Some(texture) = self.white else {
            return;
        };
        let mut points = points
            .iter()
            .map(|&(x, y)| Vertex {
                x,
                y,
                u: 0.5,
                v: 0.5,
            })
            .collect::<Vec<_>>();
        let [r, g, b] = channels(color);
        let alpha = (color >> 24) as i32;
        let surface = self.surface.object();
        unsafe {
            (self.surface.set_texture)(surface, texture);
            (self.surface.set_color)(surface, r, g, b, alpha);
            (self.surface.textured_polygon)(
                surface,
                points.len() as i32,
                points.as_mut_ptr(),
                true,
            );
        }
    }
}

fn alpha(color: COLORREF) -> i32 {
    let alpha = (color >> 24) as i32;
    if alpha == 0 { 255 } else { alpha }
}

unsafe extern "thiscall" fn paint_traverse(
    panels: *mut c_void,
    panel: u32,
    force: bool,
    allow: bool,
) {
    let original = ORIGINAL_PAINT.load(Ordering::Acquire);
    if original == 0 {
        return;
    }
    let original = unsafe { std::mem::transmute::<usize, Paint>(original) };
    let outer = PAINT_DEPTH.with(|depth| depth.replace(depth.get() + 1) == 0);
    unsafe { original(panels, panel, force, allow) };
    PAINT_DEPTH.with(|depth| depth.set(depth.get() - 1));
    let _ = std::panic::catch_unwind(|| {
        with_client(|client| {
            if let Some(label) = &client.mutator_caption
                && let Some(vgui) = &mut client.vgui
            {
                vgui.scoreboard(panel, outer, Some(label), &client.icons);
            }
            if client
                .vgui
                .as_mut()
                .is_some_and(|vgui| vgui.is_focus(panel))
                && let Some(mut vgui) = client.vgui.take()
            {
                // Keep ownership even if a Rust rendering operation fails.
                let _ =
                    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| vgui.draw(client)));
                client.vgui = Some(vgui);
            }
        });
    });
}
