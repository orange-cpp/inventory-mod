use std::{
    ffi::{CStr, c_char, c_void},
    io, ptr,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU8, AtomicUsize, Ordering},
    },
};

use windows::{
    Win32::{
        Foundation::{HWND, RECT},
        Graphics::{Direct3D::ID3DBlob, Direct3D9::*, Gdi::RGNDATA},
    },
    core::Interface,
};
use windows_sys::Win32::{
    Foundation::FreeLibrary,
    System::{
        LibraryLoader::{GetProcAddress, LoadLibraryW},
        SystemInformation::GetTickCount64,
    },
};

use crate::native::{
    self,
    resident::{CallbackSlots, Method},
};

mod fallback;
mod queue;
mod shaders;

pub use fallback::{DrawList, Drawing};

type Present =
    unsafe extern "system" fn(*mut c_void, *const RECT, *const RECT, HWND, *const RGNDATA) -> i32;
type Reset = unsafe extern "system" fn(*mut c_void, *mut D3DPRESENT_PARAMETERS) -> i32;
type Compile = unsafe extern "system" fn(
    *const c_void,
    usize,
    *const c_char,
    *const c_void,
    *mut c_void,
    *const c_char,
    *const c_char,
    u32,
    u32,
    *mut Option<ID3DBlob>,
    *mut Option<ID3DBlob>,
) -> i32;

static PRESENT: AtomicUsize = AtomicUsize::new(0);
static RESET: AtomicUsize = AtomicUsize::new(0);
static LIVE: AtomicBool = AtomicBool::new(false);
static RETIRING: AtomicBool = AtomicBool::new(false);
static RETIRED: AtomicBool = AtomicBool::new(false);
static READY: AtomicU8 = AtomicU8::new(0);
static DEVICE: AtomicBool = AtomicBool::new(false);
static DONE: AtomicU8 = AtomicU8::new(0);
static CURRENT: Mutex<Frame> = Mutex::new(Frame::EMPTY);
static FALLBACK: Mutex<Option<Arc<Drawing>>> = Mutex::new(None);
static RENDER: Mutex<Option<Graphics>> = Mutex::new(None);

const INFECTION: u8 = 1;
const STAMINA: u8 = 2;

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum Layer {
    BeforeGameHud = 0,
    BeforeInventory = 1,
    AfterInventory = 2,
}

impl Layer {
    pub fn new(game_hud: bool, inventory: bool) -> Self {
        if !game_hud {
            Self::BeforeGameHud
        } else if !inventory {
            Self::BeforeInventory
        } else {
            Self::AfterInventory
        }
    }

    fn bit(self) -> u8 {
        1 << self as u8
    }
}

#[derive(Clone, Copy)]
pub struct Effect {
    pub layer: Layer,
    pub params: [f32; 3],
}

#[derive(Clone, Copy, Default)]
pub struct Frame {
    pub infection: Option<Effect>,
    pub stamina: Option<Effect>,
}

impl Frame {
    const EMPTY: Self = Self {
        infection: None,
        stamina: None,
    };

    fn for_layer(self, layer: Layer) -> Self {
        Self {
            infection: self.infection.filter(|effect| effect.layer == layer),
            stamina: self.stamina.filter(|effect| effect.layer == layer),
        }
    }

    fn has_effect(self) -> bool {
        self.infection.is_some() || self.stamina.is_some()
    }
}

pub fn shader_time() -> f32 {
    (unsafe { GetTickCount64() } % 1_000_000) as f32 / 1000.0
}

pub fn publish(frame: Frame) {
    if let Ok(mut current) = CURRENT.lock() {
        *current = frame;
    }
}

pub fn publish_drawing(drawing: Option<Drawing>) {
    if let Ok(mut next) = FALLBACK.lock() {
        *next = drawing.map(Arc::new);
    }
}

pub fn drawable() -> bool {
    LIVE.load(Ordering::Acquire) && DEVICE.load(Ordering::Acquire)
}

fn current() -> Frame {
    CURRENT.lock().map_or(Frame::EMPTY, |frame| *frame)
}

pub fn infection_ready() -> bool {
    READY.load(Ordering::Acquire) & INFECTION != 0
}

pub fn stamina_ready() -> bool {
    READY.load(Ordering::Acquire) & STAMINA != 0
}

pub fn schedule(layer: Layer) {
    if LIVE.load(Ordering::Acquire)
        && !RETIRING.load(Ordering::Acquire)
        && current().for_layer(layer).has_effect()
    {
        queue::schedule(layer);
    }
}

pub struct Renderer {
    present: CallbackSlots,
    reset: CallbackSlots,
    queue: queue::Queue,
}

impl Renderer {
    pub fn install() -> io::Result<Self> {
        let fail = || io::Error::other("could not bind Direct3D 9 for the original screen effects");
        let table = game_device_table().ok_or_else(fail)?;
        let present_slot = table + 17 * size_of::<usize>();
        let reset_slot = table + 16 * size_of::<usize>();
        let present = native::word(present_slot).ok_or_else(fail)? as usize;
        let reset = native::word(reset_slot).ok_or_else(fail)? as usize;
        PRESENT.store(present, Ordering::Release);
        RESET.store(reset, Ordering::Release);
        let present = CallbackSlots::install(
            Method::Present,
            &[present_slot],
            present,
            hooked_present as Present as usize,
        )?;
        let reset = CallbackSlots::install(
            Method::Reset,
            &[reset_slot],
            reset,
            hooked_reset as Reset as usize,
        )?;
        RETIRING.store(false, Ordering::Release);
        RETIRED.store(false, Ordering::Release);
        LIVE.store(true, Ordering::Release);
        Ok(Self {
            present,
            reset,
            queue: queue::Queue::install(),
        })
    }

    pub fn stop(&mut self) -> io::Result<()> {
        LIVE.store(false, Ordering::Release);
        self.queue.stop()?;
        RETIRING.store(true, Ordering::Release);
        if !RETIRED.load(Ordering::Acquire) && RENDER.lock().is_ok_and(|render| render.is_some()) {
            return Err(io::Error::new(
                io::ErrorKind::WouldBlock,
                "waiting for Direct3D to retire its resources on Present",
            ));
        }
        self.reset.stop()?;
        self.present.stop()?;
        READY.store(0, Ordering::Release);
        DEVICE.store(false, Ordering::Release);
        DONE.store(0, Ordering::Release);
        publish(Frame::EMPTY);
        publish_drawing(None);
        Ok(())
    }
}

/// The checked shader device's two Present branches read this same global
/// immediately before the IDirect3DDevice9::Present call. An independently
/// created dummy device can have a different (driver/overlay-owned) vtable.
fn game_device_table() -> Option<usize> {
    let image = native::PeImage::loaded(native::module("shaderapidx9.dll")?)?;
    let at = image.base.checked_add(0x2c40c)?;
    if image.timestamp != 0x6a440e0a
        || !image.includes(at, 21)
        || !native::matches_pattern(
            at,
            "A1 ?? ?? ?? ?? 8B 08 8B 51 44 6A 00 6A 00 6A 00 6A 00 50 FF D2",
        )
    {
        return None;
    }
    let variable = native::word(at + 1)? as usize;
    if !image.includes(variable, 4) {
        return None;
    }
    let device = native::word(variable)? as usize;
    native::word(device)
        .filter(|table| *table != 0)
        .map(|table| table as usize)
}

struct Graphics {
    device: IDirect3DDevice9,
    programs: Option<Programs>,
    frame_copy: Option<(u32, u32, D3DFORMAT, IDirect3DTexture9)>,
    saved: Option<IDirect3DStateBlock9>,
    warned: bool,
    warned_fallback: bool,
    painter: fallback::Painter,
}

// The mutex owns the renderer between callbacks. Direct3D calls and the
// release of live resources happen on the device's Present or render queue.
unsafe impl Send for Graphics {}

struct Programs {
    vertex: IDirect3DVertexShader9,
    declaration: IDirect3DVertexDeclaration9,
    infection: Option<IDirect3DPixelShader9>,
    stamina: Option<IDirect3DPixelShader9>,
}

impl Graphics {
    fn new(device: IDirect3DDevice9) -> Self {
        let programs = Programs::load(&device);
        let usable = programs.as_ref().map_or(0, |programs| {
            (u8::from(programs.infection.is_some()) * INFECTION)
                | (u8::from(programs.stamina.is_some()) * STAMINA)
        });
        READY.store(usable, Ordering::Release);
        if usable == INFECTION | STAMINA {
            crate::client::log("compiled the original Direct3D infection and stamina shaders");
        } else if usable == 0 {
            crate::client::log(
                "the Direct3D effects could not initialize; using the original tint fallback",
            );
        }
        Self {
            device,
            programs,
            frame_copy: None,
            saved: None,
            warned: false,
            warned_fallback: false,
            painter: fallback::Painter::default(),
        }
    }

    fn games_frame(&mut self, frame: Frame, outside_scene: bool) -> bool {
        let requested = (u8::from(frame.infection.is_some()) * INFECTION)
            | (u8::from(frame.stamina.is_some()) * STAMINA);
        if !frame.has_effect()
            || self.programs.is_none()
            || READY.load(Ordering::Acquire) & requested == 0
        {
            return false;
        }
        let frame = Frame {
            infection: frame.infection.filter(|_| infection_ready()),
            stamina: frame.stamina.filter(|_| stamina_ready()),
        };
        let result = (|| -> windows::core::Result<()> {
            let device = &self.device;
            let target = unsafe {
                if outside_scene {
                    device.GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO)
                } else {
                    device.GetRenderTarget(0)
                }
            }?;
            let mut desc = D3DSURFACE_DESC::default();
            unsafe { target.GetDesc(&mut desc) }?;
            let (w, h, format) = (desc.Width, desc.Height, desc.Format);
            if self
                .frame_copy
                .as_ref()
                .is_none_or(|(x, y, f, _)| (*x, *y, *f) != (w, h, format))
            {
                self.frame_copy = None;
                let mut texture = None;
                unsafe {
                    device.CreateTexture(
                        w,
                        h,
                        1,
                        D3DUSAGE_RENDERTARGET as u32,
                        format,
                        D3DPOOL_DEFAULT,
                        &mut texture,
                        ptr::null_mut(),
                    )
                }?;
                self.frame_copy = texture.map(|texture| (w, h, format, texture));
            }
            let Some((_, _, _, texture)) = self.frame_copy.as_ref() else {
                return Err(windows::core::Error::empty());
            };
            if self.saved.is_none() {
                self.saved = Some(unsafe { device.CreateStateBlock(D3DSBT_ALL) }?);
            }
            let state = self.saved.as_ref().unwrap();
            let programs = self.programs.as_ref().unwrap();
            let copy = unsafe { texture.GetSurfaceLevel(0) }?;
            unsafe { state.Capture() }?;
            let started = outside_scene && unsafe { device.BeginScene() }.is_ok();
            if outside_scene && !started {
                return Err(windows::core::Error::empty());
            }
            let work = (|| -> windows::core::Result<()> {
                unsafe { configure_draw(device, w, h) }?;
                if let Some((effect, shader)) = frame.stamina.zip(programs.stamina.as_ref()) {
                    unsafe {
                        pass(
                            device,
                            programs,
                            shader,
                            (&target, &copy),
                            texture,
                            (w, h),
                            effect.params,
                        )
                    }?;
                }
                if let Some((effect, shader)) = frame.infection.zip(programs.infection.as_ref()) {
                    unsafe {
                        pass(
                            device,
                            programs,
                            shader,
                            (&target, &copy),
                            texture,
                            (w, h),
                            effect.params,
                        )
                    }?;
                }
                Ok(())
            })();
            let _ = unsafe { state.Apply() };
            if started {
                let _ = unsafe { device.EndScene() };
            }
            work
        })();
        if let Err(error) = result {
            // A lost device or unsupported render target may reject a frame copy.
            // Invalidate the state on Reset and let the game continue presenting.
            self.saved = None;
            READY.fetch_and(!requested, Ordering::AcqRel);
            if !self.warned {
                self.warned = true;
                crate::client::log(&format!("Direct3D screen-effects pass failed ({error})"));
            }
            return false;
        }
        true
    }
}

impl Programs {
    fn load(device: &IDirect3DDevice9) -> Option<Self> {
        let module = unsafe { LoadLibraryW(native::wide("d3dcompiler_47.dll").as_ptr()) };
        if module.is_null() {
            return None;
        }
        let result = (|| {
            let address = unsafe { GetProcAddress(module, c"D3DCompile".as_ptr().cast()) }?;
            let compile = unsafe {
                std::mem::transmute::<unsafe extern "system" fn() -> isize, Compile>(address)
            };
            let vs = shader(compile, shaders::VERTEX, c"vs_3_0")?;
            let vertex = unsafe { device.CreateVertexShader(vs.GetBufferPointer().cast()) }.ok()?;
            let pixel = |source| {
                let bytes = shader(compile, source, c"ps_3_0")?;
                unsafe { device.CreatePixelShader(bytes.GetBufferPointer().cast()) }.ok()
            };
            let infection = pixel(shaders::PIXEL);
            let stamina = pixel(shaders::STAMINA);
            let input = [
                element(0, D3DDECLTYPE_FLOAT4.0, D3DDECLUSAGE_POSITION.0),
                element(16, D3DDECLTYPE_FLOAT2.0, D3DDECLUSAGE_TEXCOORD.0),
                D3DVERTEXELEMENT9 {
                    Stream: 0xff,
                    Type: D3DDECLTYPE_UNUSED.0 as u8,
                    ..Default::default()
                },
            ];
            let declaration = unsafe { device.CreateVertexDeclaration(input.as_ptr()) }.ok()?;
            Some(Self {
                vertex,
                declaration,
                infection,
                stamina,
            })
        })();
        unsafe { FreeLibrary(module) };
        result
    }
}

fn shader(compile: Compile, source: &str, target: &CStr) -> Option<ID3DBlob> {
    let mut result = None;
    let mut errors = None;
    let status = unsafe {
        compile(
            source.as_ptr().cast(),
            source.len(),
            c"inventory-mod".as_ptr(),
            ptr::null(),
            ptr::null_mut(),
            c"main".as_ptr(),
            target.as_ptr(),
            1 << 15,
            0,
            &mut result,
            &mut errors,
        )
    };
    if status < 0 {
        if let Some(error) = errors {
            let slice = unsafe {
                std::slice::from_raw_parts(error.GetBufferPointer().cast(), error.GetBufferSize())
            };
            crate::client::log(&format!(
                "{} shader compilation: {}",
                target.to_string_lossy(),
                String::from_utf8_lossy(slice)
            ));
        }
        return None;
    }
    result
}

fn element(offset: u16, kind: i32, usage: i32) -> D3DVERTEXELEMENT9 {
    D3DVERTEXELEMENT9 {
        Offset: offset,
        Type: kind as u8,
        Usage: usage as u8,
        ..Default::default()
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
struct Vertex {
    x: f32,
    y: f32,
    z: f32,
    w: f32,
    u: f32,
    v: f32,
}

const fn vertex(x: f32, y: f32, u: f32, v: f32) -> Vertex {
    Vertex {
        x,
        y,
        z: 0.5,
        w: 1.0,
        u,
        v,
    }
}

unsafe fn configure_draw(
    device: &IDirect3DDevice9,
    width: u32,
    height: u32,
) -> windows::core::Result<()> {
    let viewport = D3DVIEWPORT9 {
        X: 0,
        Y: 0,
        Width: width,
        Height: height,
        MinZ: 0.0,
        MaxZ: 1.0,
    };
    unsafe { device.SetViewport(&viewport) }?;
    for (state, value) in [
        (D3DRS_ZENABLE, 0),
        (D3DRS_ZWRITEENABLE, 0),
        (D3DRS_ALPHABLENDENABLE, 0),
        (D3DRS_ALPHATESTENABLE, 0),
        (D3DRS_STENCILENABLE, 0),
        (D3DRS_SCISSORTESTENABLE, 0),
        (D3DRS_FOGENABLE, 0),
        (D3DRS_CULLMODE, D3DCULL_NONE.0 as u32),
        (D3DRS_FILLMODE, D3DFILL_SOLID.0 as u32),
        (D3DRS_SRGBWRITEENABLE, 0),
        (D3DRS_COLORWRITEENABLE, 0xf),
    ] {
        unsafe { device.SetRenderState(state, value) }?;
    }
    for (state, value) in [
        (D3DSAMP_MINFILTER, D3DTEXF_LINEAR.0 as u32),
        (D3DSAMP_MAGFILTER, D3DTEXF_LINEAR.0 as u32),
        (D3DSAMP_MIPFILTER, D3DTEXF_NONE.0 as u32),
        (D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP.0 as u32),
        (D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP.0 as u32),
        (D3DSAMP_SRGBTEXTURE, 0),
    ] {
        unsafe { device.SetSamplerState(0, state, value) }?;
    }
    Ok(())
}

unsafe fn pass(
    device: &IDirect3DDevice9,
    programs: &Programs,
    shader: &IDirect3DPixelShader9,
    (target, copy): (&IDirect3DSurface9, &IDirect3DSurface9),
    texture: &IDirect3DTexture9,
    (width, height): (u32, u32),
    [a, b, c]: [f32; 3],
) -> windows::core::Result<()> {
    let w = width as f32;
    let h = height as f32;
    let vertex_consts = [-1.0 / w, 1.0 / h, 0.0, 0.0];
    let pixel_consts = [shader_time(), a, b, c, 1.0 / w, 1.0 / h, w / h, 0.0];
    let quad = [
        vertex(-1.0, 1.0, 0.0, 0.0),
        vertex(1.0, 1.0, 1.0, 0.0),
        vertex(-1.0, -1.0, 0.0, 1.0),
        vertex(1.0, -1.0, 1.0, 1.0),
    ];
    unsafe {
        device.StretchRect(target, ptr::null(), copy, ptr::null(), D3DTEXF_NONE)?;
        device.SetTexture(0, texture)?;
        device.SetVertexDeclaration(&programs.declaration)?;
        device.SetVertexShader(&programs.vertex)?;
        device.SetPixelShader(shader)?;
        device.SetVertexShaderConstantF(0, vertex_consts.as_ptr(), 1)?;
        device.SetPixelShaderConstantF(0, pixel_consts.as_ptr(), 2)?;
        device.DrawPrimitiveUP(
            D3DPT_TRIANGLESTRIP,
            2,
            quad.as_ptr().cast(),
            size_of::<Vertex>() as u32,
        )?;
        device.SetTexture(0, None)?;
        Ok(())
    }
}

fn run_at(layer: Layer) {
    if !LIVE.load(Ordering::Acquire) {
        return;
    }
    let frame = current().for_layer(layer);
    if let Ok(mut render) = RENDER.lock()
        && let Some(graphics) = render.as_mut().filter(|_| frame.has_effect())
        && graphics.games_frame(frame, false)
    {
        DONE.fetch_or(layer.bit(), Ordering::Release);
    }
}

unsafe extern "system" fn hooked_present(
    device: *mut c_void,
    source: *const RECT,
    dest: *const RECT,
    window: HWND,
    dirty: *const RGNDATA,
) -> i32 {
    let original = PRESENT.load(Ordering::Acquire);
    let _ = std::panic::catch_unwind(|| {
        if let Ok(mut render) = RENDER.lock() {
            if RETIRING.load(Ordering::Acquire) {
                *render = None;
                RETIRED.store(true, Ordering::Release);
                return;
            }
            if !LIVE.load(Ordering::Acquire) {
                return;
            }
            let Some(device) = (unsafe { IDirect3DDevice9::from_raw_borrowed(&device) }) else {
                return;
            };
            if render
                .as_ref()
                .is_some_and(|render| render.device.as_raw() != device.as_raw())
            {
                return;
            }
            let render = render.get_or_insert_with(|| Graphics::new(device.clone()));
            DEVICE.store(true, Ordering::Release);
            let existing = DONE.swap(0, Ordering::AcqRel);
            let frame = current();
            let fallback = FALLBACK.lock().ok().and_then(|frame| frame.clone());
            for layer in [
                Layer::BeforeGameHud,
                Layer::BeforeInventory,
                Layer::AfterInventory,
            ] {
                if let Some(drawing) = &fallback {
                    let list = match layer {
                        Layer::BeforeGameHud => None,
                        Layer::BeforeInventory => Some(&drawing.hud),
                        Layer::AfterInventory => Some(&drawing.inventory),
                    };
                    if let Some(list) = list
                        && let Err(error) = render.painter.draw(&render.device, list)
                        && !render.warned_fallback
                    {
                        render.warned_fallback = true;
                        crate::client::log(&format!("Direct3D HUD fallback failed ({error})"));
                    }
                }
                if existing & layer.bit() == 0 {
                    render.games_frame(frame.for_layer(layer), true);
                }
            }
        }
    });
    let call = unsafe { std::mem::transmute::<usize, Present>(original) };
    unsafe { call(device, source, dest, window, dirty) }
}

unsafe extern "system" fn hooked_reset(
    device: *mut c_void,
    parameters: *mut D3DPRESENT_PARAMETERS,
) -> i32 {
    if let Ok(mut render) = RENDER.lock()
        && render
            .as_ref()
            .is_some_and(|render| render.device.as_raw() == device)
    {
        *render = None;
        READY.store(0, Ordering::Release);
        DEVICE.store(false, Ordering::Release);
    }
    let address = RESET.load(Ordering::Acquire);
    let original = unsafe { std::mem::transmute::<usize, Reset>(address) };
    unsafe { original(device, parameters) }
}
