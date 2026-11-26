use std::{
    ffi::{c_int, c_long, c_void},
    mem, ptr,
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

/// A changed slot in the validated server image. Restore it before the
/// plugin is unloaded so the engine never calls into an unmapped library.
pub struct VtablePatch {
    slot: *mut usize,
    original: usize,
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
        let old = unsafe { slot.read() };
        if old != expected || !unsafe { Self::write(slot, replacement) } {
            return None;
        }
        Some(Self {
            slot,
            original: old,
        })
    }

    unsafe fn write(slot: *mut usize, value: usize) -> bool {
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
            slot.write(value);
            if mprotect(page, page_size, READ) != 0 {
                eprintln!("inventory_mod: could not restore game vtable page permissions");
            }
        }
        true
    }
}

impl Drop for VtablePatch {
    fn drop(&mut self) {
        // SAFETY: All patches are dropped on the server thread before the game image unloads.
        if !unsafe { Self::write(self.slot, self.original) } {
            eprintln!("inventory_mod: could not restore a game vtable entry");
        }
    }
}
