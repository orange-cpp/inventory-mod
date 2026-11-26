use std::{
    collections::HashMap,
    path::{Path, PathBuf},
    ptr,
    sync::Arc,
    time::Instant,
};

use image::RgbaImage;
use inventory_source_assets::icons::Icons;
use windows::{
    Win32::{Foundation::HANDLE, Graphics::Direct3D9::*},
    core::Result,
};
use windows_sys::Win32::{
    Foundation::{COLORREF, RECT, SIZE},
    Graphics::Gdi::{
        ANTIALIASED_QUALITY, BI_RGB, BITMAPINFO, BITMAPINFOHEADER, CreateCompatibleDC,
        CreateDIBSection, CreateFontW, DEFAULT_CHARSET, DIB_RGB_COLORS, DeleteDC, DeleteObject,
        GetTextExtentPoint32W, SelectObject, SetBkColor, SetTextColor, TextOutW,
    },
};

use crate::{
    client::Surprise,
    native,
    overlay::{Canvas, Font, fit},
};

pub struct Drawing {
    pub hud: DrawList,
    pub inventory: DrawList,
}

pub struct DrawList {
    commands: Vec<Draw>,
    icons: Arc<Icons>,
    size: (i32, i32),
}

enum Draw {
    Rect(RECT, COLORREF),
    Text(Font, (i32, i32), String, COLORREF),
    Polygon(Vec<(f32, f32)>, COLORREF),
    Icon(String, RECT, COLORREF),
    Screamer(PathBuf, Instant),
}

impl DrawList {
    pub fn new(size: (i32, i32), icons: Arc<Icons>) -> Self {
        Self {
            commands: Vec::new(),
            icons,
            size,
        }
    }

    pub fn screamer(&mut self, effect: &Surprise) {
        self.commands
            .push(Draw::Screamer(effect.path.clone(), effect.started));
    }
}

impl Canvas for DrawList {
    fn rect(&mut self, bounds: RECT, color: COLORREF) {
        if bounds.right > bounds.left && bounds.bottom > bounds.top {
            self.commands.push(Draw::Rect(bounds, color));
        }
    }

    fn polygon(&mut self, points: &[(f32, f32)], color: COLORREF) {
        if points.len() >= 3 {
            self.commands.push(Draw::Polygon(points.to_owned(), color));
        }
    }

    fn text(&mut self, at: (i32, i32), text: &str, color: COLORREF) {
        self.styled_text(Font::Text, at, text, color);
    }

    fn text_size(&self, text: &str) -> (i32, i32) {
        self.measure(Font::Text, text)
    }

    fn measure(&self, font: Font, text: &str) -> (i32, i32) {
        let size = font_pixels(font, self.size.1);
        with_font(font, size, |dc| {
            text_extent(dc, text).map(|size| (size.cx, size.cy))
        })
        .unwrap_or((text.chars().count() as i32 * size / 2, size))
    }

    fn styled_text(&mut self, font: Font, at: (i32, i32), text: &str, color: COLORREF) {
        if !text.is_empty() {
            self.commands
                .push(Draw::Text(font, at, text.to_owned(), color));
        }
    }

    fn icon(&mut self, name: &str, bounds: RECT, color: COLORREF) -> bool {
        let Some(sprite) = self.icons.get(name) else {
            return false;
        };
        self.commands.push(Draw::Icon(
            name.to_owned(),
            fit(bounds, sprite.aspect()),
            color,
        ));
        true
    }
}

#[derive(Default)]
pub struct Painter {
    white: Option<IDirect3DTexture9>,
    sheets: HashMap<String, IDirect3DTexture9>,
    texts: HashMap<(Font, i32, String), GlyphRun>,
    picture: Option<(PathBuf, Option<GlyphRun>)>,
}

struct GlyphRun {
    texture: IDirect3DTexture9,
    raster: (u32, u32),
}

impl Painter {
    pub fn draw(&mut self, device: &IDirect3DDevice9, list: &DrawList) -> Result<()> {
        if list.commands.is_empty() {
            return Ok(());
        }
        if self.white.is_none() {
            self.white = Some(texture(
                device,
                &RgbaImage::from_pixel(1, 1, image::Rgba([255; 4])),
            )?);
        }
        if self.texts.len() > 256 {
            self.texts.clear();
        }
        let state = unsafe { device.CreateStateBlock(D3DSBT_ALL) }?;
        unsafe {
            state.Capture()?;
            device.BeginScene()?;
        }
        let result = self.paint(device, list);
        let _ = unsafe { state.Apply() };
        let _ = unsafe { device.EndScene() };
        result
    }

    fn paint(&mut self, device: &IDirect3DDevice9, list: &DrawList) -> Result<()> {
        unsafe {
            super::configure_draw(device, list.size.0.max(1) as u32, list.size.1.max(1) as u32)
        }?;
        unsafe {
            device.SetVertexShader(None)?;
            device.SetPixelShader(None)?;
            device.SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)?;
            device.SetRenderState(D3DRS_ALPHABLENDENABLE, 1)?;
            device.SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA.0 as u32)?;
            device.SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA.0 as u32)?;
            device.SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, 0)?;
            for (slot, setting) in [
                (D3DTSS_COLOROP, D3DTOP_MODULATE.0 as u32),
                (D3DTSS_COLORARG1, D3DTA_TEXTURE),
                (D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                (D3DTSS_ALPHAOP, D3DTOP_MODULATE.0 as u32),
                (D3DTSS_ALPHAARG1, D3DTA_TEXTURE),
                (D3DTSS_ALPHAARG2, D3DTA_DIFFUSE),
                (D3DTSS_TEXCOORDINDEX, 0),
            ] {
                device.SetTextureStageState(0, slot, setting)?;
            }
            device.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE.0 as u32)?;
        }
        let white = self.white.as_ref().unwrap();
        for command in &list.commands {
            match command {
                Draw::Rect(bounds, color) => {
                    quad(device, white, *bounds, [0.0, 0.0, 1.0, 1.0], *color)?
                }
                Draw::Polygon(points, color) => {
                    let vertices: Vec<_> = points
                        .iter()
                        .map(|&(x, y)| vertex(x, y, 0.5, 0.5, *color))
                        .collect();
                    unsafe {
                        device.SetTexture(0, white)?;
                        device.DrawPrimitiveUP(
                            D3DPT_TRIANGLEFAN,
                            vertices.len().saturating_sub(2) as u32,
                            vertices.as_ptr().cast(),
                            size_of::<Vertex>() as u32,
                        )?;
                    }
                }
                Draw::Icon(name, bounds, ink) => {
                    let Some(sprite) = list.icons.get(name) else {
                        continue;
                    };
                    let atlas = &sprite.material;
                    if !self.sheets.contains_key(atlas)
                        && let Some(image) = list.icons.sheet(atlas)
                    {
                        self.sheets.insert(atlas.clone(), texture(device, image)?);
                    }
                    if let Some(sheet) = self.sheets.get(atlas) {
                        quad(device, sheet, *bounds, sprite.uv(), *ink)?;
                    }
                }
                Draw::Text(font, (x, y), text, ink) => {
                    let size = font_pixels(*font, list.size.1);
                    let key = (*font, size, text.clone());
                    if let std::collections::hash_map::Entry::Vacant(entry) =
                        self.texts.entry(key.clone())
                        && let Some(image) = rasterize(*font, size, text)
                    {
                        entry.insert(GlyphRun {
                            raster: image.dimensions(),
                            texture: texture(device, &image)?,
                        });
                    }
                    if let Some(glyphs) = self.texts.get(&key) {
                        let bounds = crate::hud::rect(
                            *x,
                            *y,
                            glyphs.raster.0 as i32,
                            glyphs.raster.1 as i32,
                        );
                        quad(device, &glyphs.texture, bounds, [0.0, 0.0, 1.0, 1.0], *ink)?;
                    }
                }
                Draw::Screamer(path, started) => {
                    if self.picture.as_ref().is_none_or(|(old, _)| old != path) {
                        let image = load_picture(path).and_then(|image| {
                            let raster = image.dimensions();
                            Some(GlyphRun {
                                texture: texture(device, &image).ok()?,
                                raster,
                            })
                        });
                        self.picture = Some((path.clone(), image));
                    }
                    let picture = self
                        .picture
                        .as_ref()
                        .and_then(|(_, picture)| picture.as_ref());
                    draw_screamer(
                        device,
                        white,
                        picture,
                        list.size,
                        started.elapsed().as_secs_f32(),
                    )?;
                }
            }
        }
        unsafe { device.SetTexture(0, None) }
    }
}

fn load_picture(path: &Path) -> Option<RgbaImage> {
    let image = image::ImageReader::open(path).ok()?.decode().ok()?;
    Some(if image.width().max(image.height()) > 2048 {
        image
            .resize(2048, 2048, image::imageops::FilterType::Triangle)
            .into_rgba8()
    } else {
        image.into_rgba8()
    })
}

fn draw_screamer(
    device: &IDirect3DDevice9,
    white: &IDirect3DTexture9,
    picture: Option<&GlyphRun>,
    (width, height): (i32, i32),
    time: f32,
) -> Result<()> {
    use crate::hud::rect;
    if time >= 1.7 {
        return Ok(());
    }
    let fade = ((1.7 - time) / 0.45).clamp(0.0, 1.0);
    let pulse = (time * 40.0).sin();
    let full = rect(0, 0, width, height);
    const UV: [f32; 4] = [0.0, 0.0, 1.0, 1.0];
    let alpha = ((255.0 * fade) as u32).max(1) << 24;
    quad(device, white, full, UV, alpha)?;
    if let Some(picture) = picture {
        let aspect = picture.raster.0 as f32 / picture.raster.1 as f32;
        let zoom = 1.04 + 0.30 * (1.0 - (-9.0 * time).exp()) + 0.06 * time;
        let h = (width as f32 / aspect).max(height as f32) * zoom;
        let w = h * aspect;
        let random = ((super::shader_time() * 1000.0) as u32).wrapping_mul(2654435761);
        let shake = 34.0 * (1.0 - time / 1.2).max(0.0) * height as f32 / 1080.0;
        let x = (width as f32 - w) * 0.5 + shake * ((random & 255) as f32 / 127.5 - 1.0);
        let y = (height as f32 - h) * 0.5 + shake * (((random >> 8) & 255) as f32 / 127.5 - 1.0);
        let shade = if time > 0.2 && (random >> 16) & 7 == 0 {
            110
        } else {
            255
        };
        quad(
            device,
            &picture.texture,
            rect(x as i32, y as i32, w as i32, h as i32),
            UV,
            alpha | (shade * 0x010101),
        )?;
    }
    let red = (if picture.is_some() {
        70.0 * (0.25 + 0.2 * pulse)
    } else {
        200.0 * (0.6 + 0.4 * pulse)
    }) * fade;
    if red >= 1.0 {
        quad(device, white, full, UV, ((red as u32) << 24) | 0x96)?;
    }
    let flash = 220.0 * (1.0 - time / 0.08).max(0.0);
    if flash >= 1.0 {
        quad(
            device,
            white,
            full,
            UV,
            ((flash as u32) << 24) | 0x00ff_ffff,
        )?;
    }
    Ok(())
}

#[repr(C)]
struct Vertex {
    x: f32,
    y: f32,
    z: f32,
    rhw: f32,
    argb: u32,
    u: f32,
    v: f32,
}

fn vertex(x: f32, y: f32, u: f32, v: f32, color: COLORREF) -> Vertex {
    let [r, g, b, a] = color.to_le_bytes();
    Vertex {
        x: x - 0.5,
        y: y - 0.5,
        z: 0.0,
        rhw: 1.0,
        argb: u32::from_le_bytes([b, g, r, if a == 0 { 255 } else { a }]),
        u,
        v,
    }
}

fn quad(
    device: &IDirect3DDevice9,
    texture: &IDirect3DTexture9,
    bounds: RECT,
    [u0, v0, u1, v1]: [f32; 4],
    ink: COLORREF,
) -> Result<()> {
    let (x0, y0, x1, y1) = (
        bounds.left as f32,
        bounds.top as f32,
        bounds.right as f32,
        bounds.bottom as f32,
    );
    let vertices = [
        vertex(x0, y0, u0, v0, ink),
        vertex(x1, y0, u1, v0, ink),
        vertex(x0, y1, u0, v1, ink),
        vertex(x1, y1, u1, v1, ink),
    ];
    unsafe {
        device.SetTexture(0, texture)?;
        device.DrawPrimitiveUP(
            D3DPT_TRIANGLESTRIP,
            2,
            vertices.as_ptr().cast(),
            size_of::<Vertex>() as u32,
        )
    }
}

fn texture(device: &IDirect3DDevice9, image: &RgbaImage) -> Result<IDirect3DTexture9> {
    let mut texture = None;
    unsafe {
        device.CreateTexture(
            image.width(),
            image.height(),
            1,
            0,
            D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED,
            &mut texture,
            ptr::null_mut::<HANDLE>(),
        )
    }?;
    let texture = texture.ok_or_else(windows::core::Error::empty)?;
    let mut locked = D3DLOCKED_RECT::default();
    unsafe { texture.LockRect(0, &mut locked, ptr::null(), 0) }?;
    if locked.pBits.is_null() || locked.Pitch < image.width() as i32 * 4 {
        let _ = unsafe { texture.UnlockRect(0) };
        return Err(windows::core::Error::empty());
    }
    for y in 0..image.height() as usize {
        let out = unsafe {
            std::slice::from_raw_parts_mut(
                locked
                    .pBits
                    .cast::<u8>()
                    .offset(y as isize * locked.Pitch as isize)
                    .cast::<[u8; 4]>(),
                image.width() as usize,
            )
        };
        for (x, pixel) in out.iter_mut().enumerate() {
            let [r, g, b, a] = image.get_pixel(x as u32, y as u32).0;
            *pixel = [b, g, r, a];
        }
    }
    unsafe { texture.UnlockRect(0) }?;
    Ok(texture)
}

fn font_pixels(font: Font, height: i32) -> i32 {
    let base = match font {
        Font::Title => 16,
        Font::Hint => 12,
        Font::Ammo => 13,
        Font::Text => 13,
    };
    let scale = (height as f32 / 900.0).max(1.0);
    (base as f32 * scale).round() as i32
}

fn with_font<T>(
    font: Font,
    pixels: i32,
    get: impl FnOnce(windows_sys::Win32::Graphics::Gdi::HDC) -> Option<T>,
) -> Option<T> {
    let dc = unsafe { CreateCompatibleDC(ptr::null_mut()) };
    if dc.is_null() {
        return None;
    }
    let weight = if matches!(font, Font::Title | Font::Ammo) {
        650
    } else {
        400
    };
    let font = unsafe {
        CreateFontW(
            -pixels,
            0,
            0,
            0,
            weight,
            0,
            0,
            0,
            u32::from(DEFAULT_CHARSET),
            0,
            0,
            u32::from(ANTIALIASED_QUALITY),
            0,
            native::wide("Tahoma").as_ptr(),
        )
    };
    let previous = if font.is_null() {
        ptr::null_mut()
    } else {
        unsafe { SelectObject(dc, font) }
    };
    let result = get(dc);
    unsafe {
        if !previous.is_null() {
            SelectObject(dc, previous);
        }
        if !font.is_null() {
            DeleteObject(font);
        }
        DeleteDC(dc);
    }
    result
}

fn text_extent(dc: windows_sys::Win32::Graphics::Gdi::HDC, text: &str) -> Option<SIZE> {
    let text = native::wide(text);
    let mut size = SIZE::default();
    (unsafe { GetTextExtentPoint32W(dc, text.as_ptr(), text.len() as i32 - 1, &mut size) } != 0)
        .then_some(size)
}

fn rasterize(font: Font, pixels: i32, text: &str) -> Option<RgbaImage> {
    with_font(font, pixels, |dc| {
        let SIZE { cx, cy } = text_extent(dc, text)?;
        if cx <= 0 || cy <= 0 || cx > 4096 || cy > 256 {
            return None;
        }
        let mut bits = ptr::null_mut();
        let info = BITMAPINFO {
            bmiHeader: BITMAPINFOHEADER {
                biSize: size_of::<BITMAPINFOHEADER>() as u32,
                biWidth: cx,
                biHeight: -cy,
                biPlanes: 1,
                biBitCount: 32,
                biCompression: BI_RGB,
                ..Default::default()
            },
            ..Default::default()
        };
        let bitmap =
            unsafe { CreateDIBSection(dc, &info, DIB_RGB_COLORS, &mut bits, ptr::null_mut(), 0) };
        if bitmap.is_null() || bits.is_null() {
            if !bitmap.is_null() {
                unsafe { DeleteObject(bitmap) };
            }
            return None;
        }
        let text = native::wide(text);
        let result = unsafe {
            let original = SelectObject(dc, bitmap);
            SetBkColor(dc, 0);
            SetTextColor(dc, 0xffffff);
            let ok = TextOutW(dc, 0, 0, text.as_ptr(), text.len() as i32 - 1);
            let raster =
                std::slice::from_raw_parts(bits.cast::<[u8; 4]>(), cx as usize * cy as usize);
            let result = (ok != 0).then(|| {
                raster
                    .iter()
                    .flat_map(|pixel| [255, 255, 255, pixel[1]])
                    .collect::<Vec<_>>()
            });
            SelectObject(dc, original);
            DeleteObject(bitmap);
            result
        }?;
        RgbaImage::from_raw(cx as u32, cy as u32, result)
    })
}
