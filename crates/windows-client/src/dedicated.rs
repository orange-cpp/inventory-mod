use std::{
    cell::RefCell,
    ffi::{CStr, c_char, c_void},
    ptr,
    sync::atomic::{AtomicBool, AtomicUsize, Ordering},
};

use inventory_mod::settings::Settings;
use windows_sys::Win32::{Foundation::HINSTANCE, System::LibraryLoader::GetModuleFileNameW};

use crate::{host::Host, native};

type Factory = unsafe extern "C" fn(*const c_char, *mut i32) -> *mut c_void;
type Void = extern "thiscall" fn(*mut c_void);

static MODULE: AtomicUsize = AtomicUsize::new(0);
static PAUSED: AtomicBool = AtomicBool::new(false);

thread_local! {
    static SERVER: RefCell<Option<Host>> = const { RefCell::new(None) };
}

pub fn on_attach(module: HINSTANCE) -> bool {
    let mut path = [0; 1024];
    let size = unsafe { GetModuleFileNameW(ptr::null_mut(), path.as_mut_ptr(), path.len() as u32) };
    if size == 0 || size as usize >= path.len() {
        return false;
    }
    let name = &path[..size as usize];
    let start = name
        .iter()
        .rposition(|&ch| ch == u16::from(b'\\'))
        .map_or(0, |offset| offset + 1);
    if name.len() - start == 9
        && name[start..]
            .iter()
            .copied()
            .zip(b"srcds.exe")
            .all(|(actual, &wanted)| {
                u8::try_from(actual).is_ok_and(|actual| actual.eq_ignore_ascii_case(&wanted))
            })
    {
        MODULE.store(module.addr(), Ordering::Release);
        true
    } else {
        false
    }
}

pub fn with_host<T>(f: impl FnOnce(&mut Host) -> T) -> Option<T> {
    if PAUSED.load(Ordering::Acquire) {
        return None;
    }
    SERVER.with(|slot| {
        let mut slot = slot.try_borrow_mut().ok()?;
        slot.as_mut().map(f)
    })
}

// Layout of IServerPluginCallbacks002 in the Windows x86 Source SDK.
#[repr(C)]
struct Methods {
    load: extern "thiscall" fn(*mut c_void, Option<Factory>, Option<Factory>) -> bool,
    unload: Void,
    pause: Void,
    unpause: Void,
    description: extern "thiscall" fn(*mut c_void) -> *const c_char,
    level_init: extern "thiscall" fn(*mut c_void, *const c_char),
    activate: extern "thiscall" fn(*mut c_void, *mut c_void, i32, i32),
    frame: extern "thiscall" fn(*mut c_void, bool),
    level_shutdown: Void,
    client_active: extern "thiscall" fn(*mut c_void, *mut c_void),
    disconnect: extern "thiscall" fn(*mut c_void, *mut c_void),
    put_in_server: extern "thiscall" fn(*mut c_void, *mut c_void, *const c_char),
    set_command_client: extern "thiscall" fn(*mut c_void, i32),
    settings_changed: extern "thiscall" fn(*mut c_void, *mut c_void),
    connect: extern "thiscall" fn(
        *mut c_void,
        *mut bool,
        *mut c_void,
        *const c_char,
        *const c_char,
        *mut c_char,
        i32,
    ) -> i32,
    command: extern "thiscall" fn(*mut c_void, *mut c_void, *const c_void) -> i32,
    network_validated: extern "thiscall" fn(*mut c_void, *const c_char, *const c_char) -> i32,
    query_done:
        extern "thiscall" fn(*mut c_void, i32, *mut c_void, i32, *const c_char, *const c_char),
}

#[repr(C)]
struct Table {
    locator: usize,
    methods: Methods,
}

#[repr(C)]
struct Object {
    methods: &'static Methods,
}

static TABLE: Table = Table {
    locator: 0,
    methods: Methods {
        load,
        unload,
        pause,
        unpause,
        description,
        level_init,
        activate,
        frame,
        level_shutdown,
        client_active: ignore_edict,
        disconnect: ignore_edict,
        put_in_server,
        set_command_client,
        settings_changed: ignore_edict,
        connect,
        command,
        network_validated,
        query_done,
    },
};

static OBJECT: Object = Object {
    methods: &TABLE.methods,
};

#[unsafe(no_mangle)]
pub unsafe extern "C" fn CreateInterface(name: *const c_char, result: *mut i32) -> *mut c_void {
    let valid = MODULE.load(Ordering::Acquire) != 0
        && !name.is_null()
        && unsafe { CStr::from_ptr(name) } == c"ISERVERPLUGINCALLBACKS002";
    if !result.is_null() {
        unsafe { result.write(i32::from(!valid)) };
    }
    if valid {
        ptr::from_ref(&OBJECT).cast_mut().cast()
    } else {
        ptr::null_mut()
    }
}

extern "thiscall" fn load(_: *mut c_void, engine: Option<Factory>, game: Option<Factory>) -> bool {
    std::panic::catch_unwind(|| {
        let insecure =
            std::env::args_os().any(|arg| arg.to_string_lossy().eq_ignore_ascii_case("-insecure"));
        if !insecure || engine.is_none() || game.is_none() {
            return false;
        }
        let module = ptr::with_exposed_provenance_mut(MODULE.load(Ordering::Acquire));
        let Some(path) = native::module_path(module) else {
            return false;
        };
        let Some(directory) = path.parent() else {
            return false;
        };
        SERVER.with(|slot| {
            let Ok(mut slot) = slot.try_borrow_mut() else {
                return false;
            };
            if slot.is_some() {
                return false;
            }
            let settings = Settings::load(directory).unwrap_or_default();
            for file in crate::addons::files(directory) {
                if !crate::addons::is_menu(&file) {
                    crate::addons::mount(&file);
                }
            }
            *slot = Host::bind(directory, &settings, true);
            PAUSED.store(false, Ordering::Release);
            slot.is_some()
        })
    })
    .unwrap_or(false)
}

extern "thiscall" fn unload(_: *mut c_void) {
    let _ = std::panic::catch_unwind(|| {
        SERVER.with(|slot| {
            if let Ok(mut slot) = slot.try_borrow_mut()
                && let Some(mut host) = slot.take()
            {
                if let Err(error) = host.stop() {
                    eprintln!("inventory_mod: could not restore a game hook on unload ({error})");
                }
                host.level_shutdown();
            }
        })
    });
}

extern "thiscall" fn pause(_: *mut c_void) {
    PAUSED.store(true, Ordering::Release);
}
extern "thiscall" fn unpause(_: *mut c_void) {
    PAUSED.store(false, Ordering::Release);
}
extern "thiscall" fn description(_: *mut c_void) -> *const c_char {
    c"Inventory Mod (Rust Windows dedicated server)".as_ptr()
}
extern "thiscall" fn level_init(_: *mut c_void, _: *const c_char) {}
extern "thiscall" fn activate(_: *mut c_void, _: *mut c_void, _: i32, _: i32) {}
extern "thiscall" fn frame(_: *mut c_void, simulating: bool) {
    if simulating {
        let _ = std::panic::catch_unwind(|| with_host(Host::tick));
    }
}
extern "thiscall" fn level_shutdown(_: *mut c_void) {
    let _ = std::panic::catch_unwind(|| with_host(Host::level_shutdown));
}
extern "thiscall" fn ignore_edict(_: *mut c_void, _: *mut c_void) {}
extern "thiscall" fn put_in_server(_: *mut c_void, _: *mut c_void, _: *const c_char) {}
extern "thiscall" fn set_command_client(_: *mut c_void, _: i32) {}
extern "thiscall" fn connect(
    _: *mut c_void,
    _: *mut bool,
    _: *mut c_void,
    _: *const c_char,
    _: *const c_char,
    _: *mut c_char,
    _: i32,
) -> i32 {
    0
}
extern "thiscall" fn command(_: *mut c_void, edict: *mut c_void, args: *const c_void) -> i32 {
    let used = std::panic::catch_unwind(|| {
        with_host(|host| host.dedicated_command(edict.addr(), args.addr())).unwrap_or(false)
    })
    .unwrap_or(false);
    if used { 2 } else { 0 }
}
extern "thiscall" fn network_validated(_: *mut c_void, _: *const c_char, _: *const c_char) -> i32 {
    0
}
extern "thiscall" fn query_done(
    _: *mut c_void,
    _: i32,
    _: *mut c_void,
    _: i32,
    _: *const c_char,
    _: *const c_char,
) {
}
