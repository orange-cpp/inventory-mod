use std::{
    io,
    sync::atomic::{AtomicU64, AtomicUsize, Ordering},
};

use windows_sys::Win32::{Foundation::POINT, System::LibraryLoader::GetProcAddress};

use crate::native::{
    self, PeImage,
    resident::{CallbackSlots, Method},
};

type Set = unsafe extern "system" fn(i32, i32) -> i32;
type Get = unsafe extern "system" fn(*mut POINT) -> i32;

static SET: AtomicUsize = AtomicUsize::new(0);
static GET: AtomicUsize = AtomicUsize::new(0);
static CENTER: AtomicU64 = AtomicU64::new(0);
const FREE: u64 = u64::MAX;

pub struct Cursor {
    get: CallbackSlots,
    set: CallbackSlots,
}

impl Cursor {
    pub fn install() -> Option<Self> {
        release();
        let user = native::module("user32.dll")?;
        let get = unsafe { GetProcAddress(user.as_ptr(), c"GetCursorPos".as_ptr().cast()) }?;
        let set = unsafe { GetProcAddress(user.as_ptr(), c"SetCursorPos".as_ptr().cast()) }?;
        let (get, set) = (get as usize, set as usize);
        GET.store(get, Ordering::Release);
        SET.store(set, Ordering::Release);
        // Only Source's input imports need interception. Other threads and
        // overlays keep calling the original system functions.
        let input_slot = |module, stamp, offset| {
            let image = PeImage::loaded(native::module(module)?)?;
            (image.timestamp == stamp && image.includes(image.base + offset, 4))
                .then_some(image.base + offset)
        };
        let input = input_slot("inputsystem.dll", 0x6a440e51, 0x1411c)?;
        let vgui = input_slot("vgui2.dll", 0x6a440da5, 0x43300)?;
        let tier0 = input_slot("tier0.dll", 0x6a440d8a, 0x3021c)?;
        let get = CallbackSlots::install(
            Method::GetCursor,
            &[tier0],
            get,
            get_position as Get as usize,
        )
        .ok()?;
        let set = CallbackSlots::install(
            Method::SetCursor,
            &[input, vgui],
            set,
            set_position as Set as usize,
        )
        .ok()?;
        Some(Self { get, set })
    }

    pub fn stop(&mut self) -> io::Result<()> {
        release();
        self.set.stop()?;
        self.get.stop()
    }
}

pub fn capture(center: POINT) {
    let raw = u64::from(center.x as u32) | (u64::from(center.y as u32) << 32);
    CENTER.store(raw, Ordering::Release);
}

pub fn release() {
    CENTER.store(FREE, Ordering::Release);
}

unsafe extern "system" fn set_position(x: i32, y: i32) -> i32 {
    if CENTER.load(Ordering::Acquire) != FREE {
        return 1;
    }
    let target = SET.load(Ordering::Acquire);
    let original = unsafe { std::mem::transmute::<usize, Set>(target) };
    unsafe { original(x, y) }
}

unsafe extern "system" fn get_position(point: *mut POINT) -> i32 {
    let target = GET.load(Ordering::Acquire);
    let original = unsafe { std::mem::transmute::<usize, Get>(target) };
    let result = unsafe { original(point) };
    let center = CENTER.load(Ordering::Acquire);
    if result != 0 && !point.is_null() && center != FREE {
        unsafe {
            point.write(POINT {
                x: center as u32 as i32,
                y: (center >> 32) as u32 as i32,
            })
        };
    }
    result
}
