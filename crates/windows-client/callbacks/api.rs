//! The only interface between the reloadable client and the resident callbacks.
use std::ffi::c_void;

#[repr(u32)]
#[derive(Clone, Copy, Debug)]
pub enum Method {
    Present,
    Reset,
    Unlock,
    GetCursor,
    SetCursor,
    BeforeHud,
}

#[repr(C)]
pub struct Api {
    pub connect: extern "system" fn(Method, usize, usize) -> usize,
    pub disconnect: extern "system" fn(Method, usize) -> bool,
    pub new_task: extern "system" fn(u32, extern "system" fn(u32)) -> *const c_void,
    pub retire_task: unsafe extern "system" fn(*const c_void) -> bool,
}
