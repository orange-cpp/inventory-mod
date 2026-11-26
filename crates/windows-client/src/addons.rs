use std::{
    ffi::{CStr, c_char, c_void},
    fs,
    os::windows::ffi::OsStrExt,
    path::{Path, PathBuf},
    ptr,
};

use windows_sys::Win32::{
    Globalization::{CP_ACP, WideCharToMultiByte},
    System::LibraryLoader::GetProcAddress,
};

use crate::native;

type Factory = unsafe extern "C" fn(*const c_char, *mut i32) -> *mut c_void;
type AddVpk = unsafe extern "thiscall" fn(*mut c_void, *const c_char, i32);
type Exists = unsafe extern "thiscall" fn(*mut c_void, *const c_char, *const c_char) -> bool;

fn interface(name: &CStr) -> Option<usize> {
    if let Some(module) = native::module("filesystem_stdio.dll") {
        let factory =
            unsafe { GetProcAddress(module.as_ptr(), c"CreateInterface".as_ptr().cast()) }?;
        let factory = unsafe {
            std::mem::transmute::<unsafe extern "system" fn() -> isize, Factory>(factory)
        };
        let object = unsafe { factory(name.as_ptr(), ptr::null_mut()) };
        (!object.is_null()).then_some(object.addr())
    } else {
        let module = native::module("dedicated.dll")?;
        let slot = if name == c"VFileSystem018" {
            0xde824
        } else {
            0xde828
        };
        native::word(module.as_ptr().addr() + slot)
            .map(|address| address as usize)
            .filter(|&address| address != 0)
    }
}

pub fn files(directory: &Path) -> Vec<PathBuf> {
    let Ok(entries) = fs::read_dir(directory.join("addons")) else {
        return Vec::new();
    };
    let mut paths = entries
        .filter_map(|entry| {
            let entry = entry.ok()?;
            let path = entry.path();
            (entry.file_type().ok()?.is_file()
                && path
                    .extension()
                    .is_some_and(|name| name.eq_ignore_ascii_case("vpk")))
            .then_some(path)
        })
        .collect::<Vec<_>>();
    paths.sort_unstable();
    paths
}

pub fn is_menu(path: &Path) -> bool {
    path.file_name()
        .is_some_and(|name| name.eq_ignore_ascii_case("inventory_mod_menu.vpk"))
}

fn ansi_path(path: &Path) -> Option<Vec<u8>> {
    let wide = path
        .as_os_str()
        .encode_wide()
        .chain([0])
        .collect::<Vec<_>>();
    let length = i32::try_from(wide.len()).ok()?;
    let count = unsafe {
        WideCharToMultiByte(
            CP_ACP,
            0,
            wide.as_ptr(),
            length,
            ptr::null_mut(),
            0,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    if count <= 0 {
        return None;
    }
    let mut output = vec![0; count as usize];
    let mut replaced = 0;
    let copied = unsafe {
        WideCharToMultiByte(
            CP_ACP,
            0,
            wide.as_ptr(),
            length,
            output.as_mut_ptr(),
            count,
            ptr::null(),
            &mut replaced,
        )
    };
    (copied == count && replaced == 0).then_some(output)
}

pub fn mount(path: &Path) -> bool {
    let Some(filesystem) = interface(c"VFileSystem018") else {
        return false;
    };
    let Some(at) = native::vslot(filesystem, 107)
        .filter(|&at| native::matches_pattern(at, "55 8B EC 81 EC 0C 01 00 00 A1"))
    else {
        return false;
    };
    let Some(path) = ansi_path(path) else {
        return false;
    };
    let mount = unsafe { std::mem::transmute::<usize, AddVpk>(at) };
    unsafe {
        mount(
            ptr::with_exposed_provenance_mut(filesystem),
            path.as_ptr().cast(),
            0,
        )
    };
    true
}

pub fn exists(path: &CStr) -> bool {
    let Some(filesystem) = interface(c"VBaseFileSystem012") else {
        return false;
    };
    let Some(at) = native::vslot(filesystem, 10)
        .filter(|&at| native::matches_pattern(at, "55 8B EC 81 EC 7C 01 00 00"))
    else {
        return false;
    };
    let exists = unsafe { std::mem::transmute::<usize, Exists>(at) };
    unsafe {
        exists(
            ptr::with_exposed_provenance_mut(filesystem),
            path.as_ptr(),
            c"GAME".as_ptr(),
        )
    }
}
