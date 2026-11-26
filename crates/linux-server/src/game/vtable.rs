use std::{
    ffi::{c_char, c_int, c_long, c_void},
    mem, ptr,
    sync::atomic::{AtomicBool, AtomicUsize, Ordering},
};

const READ: c_int = 1;
const WRITE: c_int = 2;
const SYSCONF_PAGE_SIZE: c_int = 30;
const VTABLE_START: usize = 0xbe9740;
const VTABLE_END: usize = 0xd486f8;

unsafe extern "C" {
    fn sysconf(name: c_int) -> c_long;
    fn mprotect(start: *mut c_void, length: usize, permissions: c_int) -> c_int;
}

#[repr(C)]
#[derive(Default)]
struct DlInfo {
    filename: *const c_char,
    base: *mut c_void,
    symbol: *const c_char,
    symbol_addr: *mut c_void,
}

#[link(name = "dl")]
unsafe extern "C" {
    fn dladdr(address: *const c_void, info: *mut DlInfo) -> c_int;
    fn dlopen(filename: *const c_char, flags: c_int) -> *mut c_void;
    fn dlclose(handle: *mut c_void) -> c_int;
}

static RETAINED: AtomicBool = AtomicBool::new(false);

/// Source's Unload callback cannot veto a dlclose. Own a loader reference
/// before publishing a hook; keep it only if another plugin still chains to us.
pub(super) struct ModuleLease(Option<ptr::NonNull<c_void>>);

impl ModuleLease {
    pub(super) fn acquire() -> Option<Self> {
        let mut info = DlInfo::default();
        if unsafe { dladdr(Self::acquire as *const () as *const c_void, &mut info) } == 0
            || info.filename.is_null()
        {
            return None;
        }
        const NOW: c_int = 2;
        const NOLOAD: c_int = 4;
        ptr::NonNull::new(unsafe { dlopen(info.filename, NOW | NOLOAD) }).map(|h| Self(Some(h)))
    }

    pub(super) fn keep(&mut self) {
        // The cached hook and its pure pass-through must outlive the outer owner.
        self.0.take();
        RETAINED.store(true, Ordering::Release);
    }
}

pub(super) fn retained() -> bool {
    RETAINED.load(Ordering::Acquire)
}

impl Drop for ModuleLease {
    fn drop(&mut self) {
        if let Some(handle) = self.0 {
            unsafe { dlclose(handle.as_ptr()) };
        }
    }
}

/// A changed slot in the validated server image. Restore it before the
/// plugin is unloaded so the engine never calls into an unmapped library.
pub struct VtablePatch {
    slot: *mut usize,
    original: usize,
    replacement: usize,
    lease: ModuleLease,
}

impl VtablePatch {
    pub fn checked_slot(base: usize, address: usize) -> Option<*mut usize> {
        let rva = address.checked_sub(base)?;
        let align = mem::align_of::<usize>();
        (address.is_multiple_of(align)
            && rva >= VTABLE_START
            && rva <= VTABLE_END - mem::size_of::<usize>())
        .then(|| ptr::with_exposed_provenance_mut(address))
    }

    pub unsafe fn install(
        base: usize,
        address: usize,
        expected: usize,
        replacement: usize,
    ) -> Option<Self> {
        let slot = Self::checked_slot(base, address)?;
        let lease = ModuleLease::acquire()?;
        if !unsafe { Self::exchange(slot, expected, replacement) } {
            return None;
        }
        Some(Self {
            slot,
            original: expected,
            replacement,
            lease,
        })
    }

    unsafe fn exchange(slot: *mut usize, expected: usize, value: usize) -> bool {
        let page_size = unsafe { sysconf(SYSCONF_PAGE_SIZE) };
        if page_size <= 0 {
            return false;
        }
        let page_size = page_size as usize;
        let addr = slot.addr();
        let page = ptr::with_exposed_provenance_mut::<c_void>(addr - addr % page_size);
        // SAFETY: Only a checked vtable page in server_srv.so is temporarily writable.
        unsafe {
            if mprotect(page, page_size, READ | WRITE) != 0 {
                return false;
            }
            let changed = AtomicUsize::from_ptr(slot)
                .compare_exchange(expected, value, Ordering::AcqRel, Ordering::Acquire)
                .is_ok();
            if mprotect(page, page_size, READ) != 0 {
                eprintln!("inventory_mod: could not restore game vtable page permissions");
            }
            changed
        }
    }
}

impl Drop for VtablePatch {
    fn drop(&mut self) {
        // SAFETY: All patches are dropped on the server thread before the game image unloads.
        if !unsafe { Self::exchange(self.slot, self.replacement, self.original) } {
            self.lease.keep();
            eprintln!(
                "inventory_mod: retained callback for a game vtable entry owned by another plugin or unavailable for restoration"
            );
        }
    }
}
