use std::{
    ffi::{c_char, c_void},
    io, ptr,
    sync::atomic::{AtomicUsize, Ordering},
};

use windows_sys::Win32::System::LibraryLoader::GetProcAddress;

use crate::native::{self, PeImage, VtablePatch};

use super::{LIVE, Layer, run_at};

type Factory = unsafe extern "C" fn(*const c_char, *mut i32) -> *mut c_void;
type GetContext = unsafe extern "thiscall" fn(*mut c_void) -> *mut c_void;
type GetCallQueue = unsafe extern "thiscall" fn(*mut c_void) -> *mut c_void;
type Release = unsafe extern "thiscall" fn(*mut c_void) -> i32;
type QueueFunctor = unsafe extern "thiscall" fn(*mut c_void, *const Functor);
type PreHud = unsafe extern "thiscall" fn(*mut c_void, *const c_void);

static MATERIALS: AtomicUsize = AtomicUsize::new(0);
static PRE_HUD: AtomicUsize = AtomicUsize::new(0);
static QUEUED: AtomicUsize = AtomicUsize::new(0);

#[repr(C)]
struct Vtable {
    add_ref: unsafe extern "thiscall" fn(*const Functor) -> i32,
    release: unsafe extern "thiscall" fn(*const Functor) -> i32,
    destructor: unsafe extern "thiscall" fn(*const Functor, i32) -> *const Functor,
    invoke: unsafe extern "thiscall" fn(*const Functor),
}

#[repr(C)]
struct Functor {
    vtable: &'static Vtable,
    layer: Layer,
}

unsafe extern "thiscall" fn retain(_: *const Functor) -> i32 {
    1
}
unsafe extern "thiscall" fn no_destroy(this: *const Functor, _: i32) -> *const Functor {
    this
}
unsafe extern "thiscall" fn invoke(this: *const Functor) {
    let _ = std::panic::catch_unwind(|| run_at(unsafe { (*this).layer }));
    QUEUED.fetch_sub(1, Ordering::Release);
}

static TABLE: Vtable = Vtable {
    add_ref: retain,
    release: retain,
    destructor: no_destroy,
    invoke,
};
static BEFORE_GAME_HUD: Functor = Functor {
    vtable: &TABLE,
    layer: Layer::BeforeGameHud,
};
static BEFORE_INVENTORY: Functor = Functor {
    vtable: &TABLE,
    layer: Layer::BeforeInventory,
};
static AFTER_INVENTORY: Functor = Functor {
    vtable: &TABLE,
    layer: Layer::AfterInventory,
};

pub struct Queue(Option<VtablePatch>);

impl Queue {
    pub fn install() -> Self {
        if let Some(materials) = bind_materials() {
            MATERIALS.store(materials, Ordering::Release);
        }
        let patch = bind_pre_hud();
        if patch.is_some() {
            crate::client::log("bound the original screen-effects pass before the game's HUD");
        }
        Self(patch)
    }

    pub fn stop(&mut self) -> io::Result<()> {
        if let Some(patch) = &mut self.0 {
            patch.restore()?;
        }
        MATERIALS.store(0, Ordering::Release);
        Ok(())
    }
}

fn bind_materials() -> Option<usize> {
    let module = native::module("materialsystem.dll")?;
    let image = PeImage::loaded(module)?;
    let factory = unsafe { GetProcAddress(module.as_ptr(), c"CreateInterface".as_ptr().cast()) }?;
    let factory =
        unsafe { std::mem::transmute::<unsafe extern "system" fn() -> isize, Factory>(factory) };
    let material = unsafe { factory(c"VMaterialSystem080".as_ptr(), ptr::null_mut()) }.addr();
    let get_context = native::vslot(material, 96)?;
    if !image.includes(get_context, 5) || !native::matches_pattern(get_context, "56 57 8B F9 B9") {
        return None;
    }
    Some(material)
}

fn bind_pre_hud() -> Option<VtablePatch> {
    if MATERIALS.load(Ordering::Acquire) == 0 {
        return None;
    }
    let client = PeImage::loaded(native::module("client.dll")?)?;
    let vtable = if client.timestamp == 0x6afc_ccab {
        0x5c142c
    } else {
        0x5c1424
    };
    let slot = client.base + vtable + 37 * 4;
    let original = native::word(slot)? as usize;
    if !client.includes(original, 5) || !native::matches_pattern(original, "55 8B EC 5D E9") {
        return None;
    }
    PRE_HUD.store(original, Ordering::Release);
    VtablePatch::install(slot, original, pre_hud as PreHud as usize).ok()
}

unsafe extern "thiscall" fn pre_hud(this: *mut c_void, view: *const c_void) {
    let address = PRE_HUD.load(Ordering::Acquire);
    let original = unsafe { std::mem::transmute::<usize, PreHud>(address) };
    unsafe { original(this, view) };
    let _ = std::panic::catch_unwind(|| super::schedule(Layer::BeforeGameHud));
}

pub fn idle() -> bool {
    QUEUED.load(Ordering::Acquire) == 0
}

pub fn schedule(layer: Layer) {
    let materials = MATERIALS.load(Ordering::Acquire);
    if !LIVE.load(Ordering::Acquire) || materials == 0 {
        return;
    }
    let Some(address) = native::vslot(materials, 96) else {
        return;
    };
    let get_context = unsafe { std::mem::transmute::<usize, GetContext>(address) };
    let context = unsafe { get_context(ptr::with_exposed_provenance_mut(materials)) };
    if context.is_null() {
        return;
    }
    let Some(release) = native::vslot(context.addr(), 1) else {
        return;
    };
    let release = unsafe { std::mem::transmute::<usize, Release>(release) };
    if let Some(address) = native::vslot(context.addr(), 142) {
        if native::matches_pattern(address, "8D 81 A4 02 00 00 C3") {
            let get_queue = unsafe { std::mem::transmute::<usize, GetCallQueue>(address) };
            let queue = unsafe { get_queue(context) };
            let method = native::vslot(queue.addr(), 0).filter(|&address| {
                native::matches_pattern(address, "55 8B EC 56 8B 75 08 8B 06 8B 10")
            });
            if let Some(method) = method {
                let enqueue = unsafe { std::mem::transmute::<usize, QueueFunctor>(method) };
                let functor = match layer {
                    Layer::BeforeGameHud => &BEFORE_GAME_HUD,
                    Layer::BeforeInventory => &BEFORE_INVENTORY,
                    Layer::AfterInventory => &AFTER_INVENTORY,
                };
                QUEUED.fetch_add(1, Ordering::Release);
                unsafe { enqueue(queue, functor) };
            }
        } else if native::matches_pattern(address, "33 C0 C3") {
            run_at(layer);
        }
    }
    unsafe { release(context) };
}
