use std::{
    io,
    sync::atomic::{AtomicU64, AtomicUsize, Ordering},
};

use windows_sys::Win32::{Foundation::POINT, System::LibraryLoader::GetProcAddress};

use crate::native::{self, Detour};

type Set = unsafe extern "system" fn(i32, i32) -> i32;
type Get = unsafe extern "system" fn(*mut POINT) -> i32;

static SET: AtomicUsize = AtomicUsize::new(0);
static GET: AtomicUsize = AtomicUsize::new(0);
static CENTER: AtomicU64 = AtomicU64::new(0);
const FREE: u64 = u64::MAX;

pub struct Cursor {
    get: Detour,
    set: Detour,
}

impl Cursor {
    pub fn install() -> Option<Self> {
        release();
        let user = native::module("user32.dll")?;
        let get = unsafe { GetProcAddress(user.as_ptr(), c"GetCursorPos".as_ptr().cast()) }?;
        let set = unsafe { GetProcAddress(user.as_ptr(), c"SetCursorPos".as_ptr().cast()) }?;
        let mut get = Detour::prepare(get as usize, get_position as Get as usize).ok()?;
        let mut set = Detour::prepare(set as usize, set_position as Set as usize).ok()?;
        GET.store(get.trampoline(), Ordering::Release);
        SET.store(set.trampoline(), Ordering::Release);
        if get.enable().and_then(|()| set.enable()).is_err() {
            let _ = get.disable();
            return None;
        }
        Some(Self { get, set })
    }

    pub fn stop(&mut self) -> io::Result<()> {
        release();
        self.set.disable()?;
        self.get.disable()
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
