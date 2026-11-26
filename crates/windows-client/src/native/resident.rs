//! Callback code fetched by render/audio threads must remain executable even if
//! the game pauses a thread before its first instruction. The tiny forwarding
//! library stays resident. The mod's handlers and resources detach on unload.
#[path = "../../callbacks/api.rs"]
mod abi;

use std::{
    ffi::OsStr,
    fs::{self, File, OpenOptions},
    io::{self, Read, Write},
    os::windows::{ffi::OsStrExt, fs::OpenOptionsExt},
    path::{Path, PathBuf},
    ptr,
    sync::OnceLock,
};
use windows_sys::Win32::Foundation::FreeLibrary;
use windows_sys::Win32::System::LibraryLoader::{
    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, GET_MODULE_HANDLE_EX_FLAG_PIN, GetModuleHandleExW,
    GetProcAddress, LOAD_LIBRARY_SEARCH_SYSTEM32, LoadLibraryExW,
};

use super::VtablePatch;
pub use abi::{Api, Method};

const IMAGE: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/inventory_callbacks.dll"));
static API: OnceLock<&'static Api> = OnceLock::new();

fn image_file(folder: &Path, file: &Path) -> io::Result<File> {
    fs::create_dir_all(folder)?;
    // Deny write/delete sharing until LoadLibrary has mapped these exact bytes.
    let open = || OpenOptions::new().read(true).share_mode(1).open(file);
    let mut image = match open() {
        Ok(file) => file,
        Err(e) if e.kind() == io::ErrorKind::NotFound => {
            let temp = folder.join(format!(
                "{}-{}-{}.tmp",
                std::process::id(),
                env!("INVENTORY_CALLBACK_IMAGE"),
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .map_err(io::Error::other)?
                    .as_nanos()
            ));
            let mut output = OpenOptions::new()
                .create_new(true)
                .write(true)
                .share_mode(0)
                .open(&temp)?;
            output.write_all(IMAGE)?;
            drop(output);
            match fs::rename(&temp, file) {
                Ok(()) => open()?,
                Err(error) => {
                    let _ = fs::remove_file(temp);
                    // Another game process may already have mapped the identical image.
                    open().map_err(|_| error)?
                }
            }
        }
        Err(error) => return Err(error),
    };
    let mut actual = Vec::with_capacity(IMAGE.len());
    (&mut image)
        .take(IMAGE.len() as u64 + 1)
        .read_to_end(&mut actual)?;
    if actual != IMAGE {
        return Err(io::Error::other("invalid Inventory Mod callback image"));
    }
    Ok(image)
}

pub fn api() -> io::Result<&'static Api> {
    if let Some(api) = API.get() {
        return Ok(api);
    }
    static INITIALIZING: std::sync::Mutex<()> = std::sync::Mutex::new(());
    let _guard = INITIALIZING.lock().unwrap_or_else(|err| err.into_inner());
    if let Some(api) = API.get() {
        return Ok(api);
    }
    let folder = std::env::var_os("LOCALAPPDATA")
        .map_or_else(std::env::temp_dir, PathBuf::from)
        .join("Inventory Mod")
        .join("callbacks");
    let path = folder.join(concat!(
        "inventory_callbacks_v1_",
        env!("INVENTORY_CALLBACK_IMAGE"),
        ".dll"
    ));
    let _verified = image_file(&folder, &path)?;
    let wide: Vec<u16> = OsStr::new(&path).encode_wide().chain([0]).collect();
    // Dependencies are system libraries. Do not search the game or cache folder.
    let dll =
        unsafe { LoadLibraryExW(wide.as_ptr(), ptr::null_mut(), LOAD_LIBRARY_SEARCH_SYSTEM32) };
    if dll.is_null() {
        return Err(io::Error::last_os_error());
    }
    let result = (|| {
        let address = unsafe { GetProcAddress(dll, c"inventory_callbacks_v1".as_ptr().cast()) }
            .ok_or_else(io::Error::last_os_error)?;
        let mut pinned = ptr::null_mut();
        if unsafe {
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                address as *const u16,
                &mut pinned,
            )
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        let entry = unsafe {
            std::mem::transmute::<
                unsafe extern "system" fn() -> isize,
                extern "system" fn() -> &'static Api,
            >(address)
        };
        Ok(*API.get_or_init(|| entry()))
    })();
    unsafe { FreeLibrary(dll) };
    result
}

/// Own a set of import or virtual slots with the same ABI and original method.
pub struct CallbackSlots {
    patches: Vec<VtablePatch>,
    api: &'static Api,
    method: Method,
    handler: usize,
    connected: bool,
}

impl CallbackSlots {
    pub fn install(
        method: Method,
        slots: &[usize],
        original: usize,
        handler: usize,
    ) -> io::Result<Self> {
        let api = api()?;
        let entry = (api.connect)(method, original, handler);
        if entry == 0 {
            return Err(io::Error::other("the Source callback is already in use"));
        }
        let mut result = Self {
            patches: Vec::new(),
            api,
            method,
            handler,
            connected: true,
        };
        for &slot in slots {
            result
                .patches
                .push(VtablePatch::install(slot, original, entry)?);
        }
        Ok(result)
    }

    pub fn stop(&mut self) -> io::Result<()> {
        for patch in &mut self.patches {
            patch.retire_forwarder()?;
        }
        if self.connected {
            if !(self.api.disconnect)(self.method, self.handler) {
                return Err(io::Error::new(
                    io::ErrorKind::WouldBlock,
                    format!("waiting for the {:?} callback to return", self.method),
                ));
            }
            self.connected = false;
        }
        Ok(())
    }
}

impl Drop for CallbackSlots {
    fn drop(&mut self) {
        // Covers installation rollback. Even a later owner that remembers the
        // forwarding address can only reach the original after disconnection.
        for patch in &mut self.patches {
            let _ = patch.retire_forwarder();
        }
        while self.connected && !(self.api.disconnect)(self.method, self.handler) {
            std::thread::yield_now();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        ffi::c_void,
        sync::{
            Barrier,
            atomic::{AtomicU32, Ordering},
        },
    };

    static ENTERED: OnceLock<Barrier> = OnceLock::new();
    static LEAVE: OnceLock<Barrier> = OnceLock::new();
    extern "system" fn cursor_original(_: *mut c_void) -> i32 {
        3
    }
    extern "system" fn cursor_handler(_: *mut c_void) -> i32 {
        ENTERED.get().unwrap().wait();
        LEAVE.get().unwrap().wait();
        7
    }

    #[test]
    fn cached_cursor_call_survives_an_in_flight_retirement_and_new_slot_owner() {
        let entered = ENTERED.get_or_init(|| Barrier::new(2));
        let leave = LEAVE.get_or_init(|| Barrier::new(2));
        let original = cursor_original as *const () as usize;
        let slot = AtomicU32::new(original as u32);
        let mut owner = CallbackSlots::install(
            Method::GetCursor,
            &[slot.as_ptr().addr()],
            original,
            cursor_handler as *const () as usize,
        )
        .unwrap();
        let fetched = slot.load(Ordering::Acquire) as usize;
        let call = unsafe {
            std::mem::transmute::<usize, extern "system" fn(*mut c_void) -> i32>(fetched)
        };
        let worker = std::thread::spawn(move || call(ptr::null_mut()));
        entered.wait();
        // A third party has saved our entry and occupied this slot.
        slot.store(0x1234, Ordering::Release);
        assert_eq!(owner.stop().unwrap_err().kind(), io::ErrorKind::WouldBlock);
        leave.wait();
        assert_eq!(worker.join().unwrap(), 7);
        owner.stop().unwrap();
        drop(owner);
        assert_eq!(slot.load(Ordering::Acquire), 0x1234);
        assert_eq!(call(ptr::null_mut()), 3);
    }

    #[repr(C)]
    struct TestMethods {
        retain: unsafe extern "thiscall" fn(*const c_void) -> i32,
        release: unsafe extern "thiscall" fn(*const c_void) -> i32,
        _destructor: usize,
        invoke: unsafe extern "thiscall" fn(*const c_void),
    }

    #[test]
    fn material_queue_can_cancel_or_release_a_task_after_client_retirement() {
        static LAYERS: AtomicU32 = AtomicU32::new(0);
        extern "system" fn draw(layer: u32) {
            LAYERS.fetch_add(layer, Ordering::Relaxed);
        }
        let api = api().unwrap();
        for execute in [true, false] {
            let task = (api.new_task)(2, draw);
            let methods = unsafe { &**task.cast::<*const TestMethods>() };
            unsafe {
                (api.retain_for_queue)(task);
                assert_eq!((methods.retain)(task), 3);
                assert_eq!((methods.release)(task), 2);
                if execute {
                    (methods.invoke)(task);
                }
                assert!((api.retire_task)(task));
                (methods.invoke)(task);
                assert_eq!((methods.release)(task), 0);
            }
        }
        assert_eq!(LAYERS.load(Ordering::Relaxed), 2);
    }
}
