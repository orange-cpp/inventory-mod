use super::vtable::ModuleLease;
use std::{
    ffi::{c_int, c_void},
    ptr::NonNull,
};

const SAVED: [u8; 6] = [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53];
const TRAMPOLINE_SIZE: usize = SAVED.len() + 5;
const SYSCONF_PAGE_SIZE: c_int = 30;
const READ: c_int = 1;
const WRITE: c_int = 2;
const EXEC: c_int = 4;
const PRIVATE: c_int = 2;
const ANONYMOUS: c_int = 0x20;

unsafe extern "C" {
    fn sysconf(name: c_int) -> std::ffi::c_long;
    fn mprotect(start: *mut c_void, len: usize, protection: c_int) -> c_int;
    fn mmap(
        addr: *mut c_void,
        len: usize,
        protection: c_int,
        flags: c_int,
        fd: c_int,
        offset: i32,
    ) -> *mut c_void;
    fn munmap(addr: *mut c_void, len: usize) -> c_int;
}

pub struct SavedFrame {
    target: NonNull<u8>,
    trampoline: NonNull<u8>,
    replacement: [u8; 6],
    lease: ModuleLease,
}

impl SavedFrame {
    /// The caller validates the game build and supplies functions with a matching
    /// ABI. The six saved bytes contain complete, position-independent instructions.
    pub unsafe fn install(target: usize, replacement: usize) -> Option<Self> {
        let target = NonNull::new(std::ptr::with_exposed_provenance_mut::<u8>(target))?;
        let lease = ModuleLease::acquire()?;
        if unsafe { target.as_ptr().cast::<[u8; SAVED.len()]>().read_unaligned() } != SAVED {
            return None;
        }
        let map = unsafe {
            mmap(
                std::ptr::null_mut(),
                TRAMPOLINE_SIZE,
                READ | WRITE,
                PRIVATE | ANONYMOUS,
                -1,
                0,
            )
        };
        if map as isize == -1 {
            return None;
        }
        let trampoline = NonNull::new(map.cast::<u8>())?;
        let tail = trampoline.as_ptr().wrapping_add(SAVED.len());
        let jump_back = jump(tail.addr(), target.as_ptr().addr() + SAVED.len());
        unsafe {
            trampoline
                .as_ptr()
                .cast::<[u8; SAVED.len()]>()
                .write_unaligned(SAVED);
            tail.cast::<[u8; 5]>().write_unaligned(jump_back);
        }
        let branch = jump(target.as_ptr().addr(), replacement);
        let code = [branch[0], branch[1], branch[2], branch[3], branch[4], 0x90];
        if unsafe { mprotect(map, TRAMPOLINE_SIZE, READ | EXEC) } != 0
            || !unsafe { write_code(target, code) }
        {
            unsafe { munmap(map, TRAMPOLINE_SIZE) };
            return None;
        }
        Some(Self {
            target,
            trampoline,
            replacement: code,
            lease,
        })
    }

    pub fn original(&self) -> usize {
        self.trampoline.as_ptr().addr()
    }
}

impl Drop for SavedFrame {
    fn drop(&mut self) {
        let current = unsafe { self.target.cast::<[u8; 6]>().read_unaligned() };
        if current == self.replacement && unsafe { write_code(self.target, SAVED) } {
            unsafe { munmap(self.trampoline.as_ptr().cast(), TRAMPOLINE_SIZE) };
        } else {
            self.lease.keep();
            eprintln!(
                "inventory_mod: retaining the chained rescue closet callback and its original trampoline"
            );
        }
    }
}

fn jump(from: usize, to: usize) -> [u8; 5] {
    let displacement = to.wrapping_sub(from.wrapping_add(5)).to_le_bytes();
    [
        0xe9,
        displacement[0],
        displacement[1],
        displacement[2],
        displacement[3],
    ]
}

unsafe fn write_code(target: NonNull<u8>, bytes: [u8; 6]) -> bool {
    let page_size = unsafe { sysconf(SYSCONF_PAGE_SIZE) };
    if page_size <= 0 {
        return false;
    }
    let page_size = page_size as usize;
    let address = target.as_ptr().addr();
    let offset = address % page_size;
    if offset + bytes.len() > page_size {
        return false;
    }
    let page = std::ptr::with_exposed_provenance_mut(address - offset);
    // Installation and removal both occur on Source's server thread.
    unsafe {
        if mprotect(page, page_size, READ | WRITE) != 0 {
            return false;
        }
        target.as_ptr().cast::<[u8; 6]>().write_unaligned(bytes);
        if mprotect(page, page_size, READ | EXEC) != 0 {
            eprintln!("inventory_mod: could not restore game code page permissions");
        }
    }
    true
}
