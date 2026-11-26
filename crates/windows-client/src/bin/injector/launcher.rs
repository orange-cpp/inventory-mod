use std::{
    ffi::{OsStr, OsString, c_void},
    io,
    os::windows::ffi::{OsStrExt, OsStringExt},
    path::{Path, PathBuf},
    ptr::{self, NonNull},
    slice,
};

use windows_sys::Win32::{
    Foundation::{
        CloseHandle, ERROR_ALREADY_EXISTS, GetLastError, HANDLE, INVALID_HANDLE_VALUE, LocalFree,
        WAIT_OBJECT_0, WAIT_TIMEOUT,
    },
    System::{
        Console::GetConsoleProcessList,
        Diagnostics::{
            Debug::WriteProcessMemory,
            ToolHelp::{
                CreateToolhelp32Snapshot, MODULEENTRY32W, Module32FirstW, Module32NextW,
                PROCESSENTRY32W, Process32FirstW, Process32NextW, TH32CS_SNAPMODULE,
                TH32CS_SNAPPROCESS,
            },
        },
        LibraryLoader::{
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            GetModuleFileNameW, GetModuleHandleExW, GetModuleHandleW, GetProcAddress,
        },
        Memory::{
            MEM_COMMIT, MEM_RELEASE, MEM_RESERVE, PAGE_READWRITE, VirtualAllocEx, VirtualFreeEx,
        },
        Threading::{
            CreateMutexW, CreateRemoteThread, GetExitCodeThread, IsWow64Process2,
            LPTHREAD_START_ROUTINE, OpenProcess, PROCESS_CREATE_THREAD, PROCESS_QUERY_INFORMATION,
            PROCESS_VM_OPERATION, PROCESS_VM_READ, PROCESS_VM_WRITE, QueryFullProcessImageNameW,
            WaitForSingleObject,
        },
    },
    UI::Shell::CommandLineToArgvW,
};

#[path = "dev.rs"]
mod dev;

const I386: u16 = 0x014c;

struct Handle(NonNull<c_void>);

impl Handle {
    fn new(handle: HANDLE) -> io::Result<Self> {
        NonNull::new(handle)
            .filter(|handle| handle.as_ptr() != INVALID_HANDLE_VALUE)
            .map(Self)
            .ok_or_else(io::Error::last_os_error)
    }

    fn raw(&self) -> HANDLE {
        self.0.as_ptr()
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        unsafe { CloseHandle(self.raw()) };
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum Game {
    Client,
    Server,
}

impl Game {
    fn from_name(name: &OsStr) -> Option<Self> {
        let name = name.to_string_lossy();
        if name.eq_ignore_ascii_case("left4dead2.exe") {
            Some(Self::Client)
        } else if name.eq_ignore_ascii_case("srcds.exe") {
            Some(Self::Server)
        } else {
            None
        }
    }
}

fn wide(text: &OsStr) -> Vec<u16> {
    text.encode_wide().chain([0]).collect()
}

fn wide_string(buffer: &[u16]) -> OsString {
    let end = buffer
        .iter()
        .position(|&word| word == 0)
        .unwrap_or(buffer.len());
    OsString::from_wide(&buffer[..end])
}

fn find_game(game: Game) -> io::Result<u32> {
    let processes = Handle::new(unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) })?;
    let mut entry = PROCESSENTRY32W {
        dwSize: size_of::<PROCESSENTRY32W>() as u32,
        ..Default::default()
    };
    let mut result = None;
    let mut status = unsafe { Process32FirstW(processes.raw(), &mut entry) };
    while status != 0 {
        if Game::from_name(&wide_string(&entry.szExeFile)) == Some(game) {
            if result.is_some() {
                return Err(io::Error::other(
                    "multiple games are running. Specify the game process ID",
                ));
            }
            result = Some(entry.th32ProcessID);
        }
        status = unsafe { Process32NextW(processes.raw(), &mut entry) };
    }
    result.ok_or_else(|| io::Error::other("the requested Left 4 Dead 2 process is not running"))
}

fn process_path(process: &Handle) -> io::Result<PathBuf> {
    let mut executable = vec![0u16; 32_768];
    let mut count = executable.len() as u32;
    if unsafe { QueryFullProcessImageNameW(process.raw(), 0, executable.as_mut_ptr(), &mut count) }
        == 0
    {
        return Err(io::Error::last_os_error());
    }
    Ok(PathBuf::from(OsString::from_wide(
        &executable[..count as usize],
    )))
}

#[repr(C)]
struct UnicodeString {
    length: u16,
    capacity: u16,
    data: *const u16,
}

#[link(name = "ntdll")]
unsafe extern "system" {
    fn NtQueryInformationProcess(
        process: HANDLE,
        class: u32,
        buffer: *mut c_void,
        length: u32,
        returned: *mut u32,
    ) -> i32;
}

fn process_arguments(process: &Handle) -> io::Result<Vec<OsString>> {
    const COMMAND_LINE: u32 = 60; // ProcessCommandLineInformation
    let mut length = 0;
    unsafe {
        NtQueryInformationProcess(process.raw(), COMMAND_LINE, ptr::null_mut(), 0, &mut length)
    };
    if length < size_of::<UnicodeString>() as u32 || length > 131_072 {
        return Err(io::Error::other("could not read the game's command line"));
    }
    let mut buffer = vec![0usize; (length as usize).div_ceil(size_of::<usize>())];
    let result = unsafe {
        NtQueryInformationProcess(
            process.raw(),
            COMMAND_LINE,
            buffer.as_mut_ptr().cast(),
            length,
            &mut length,
        )
    };
    if result < 0 {
        return Err(io::Error::other("could not read the game's command line"));
    }
    let command = unsafe { buffer.as_ptr().cast::<UnicodeString>().read() };
    let start = buffer.as_ptr().addr();
    let end = start + buffer.len() * size_of::<usize>();
    let address = command.data.addr();
    let count = usize::from(command.length);
    if !count.is_multiple_of(2)
        || address < start
        || !address.is_multiple_of(align_of::<u16>())
        || address
            .checked_add(count)
            .is_none_or(|end_addr| end_addr > end)
    {
        return Err(io::Error::other("invalid game command line"));
    }
    // NtQueryInformationProcess returns the string in our aligned buffer.
    let terminated: Vec<_> = unsafe { slice::from_raw_parts(command.data, count / 2) }
        .iter()
        .copied()
        .chain([0])
        .collect();
    let mut argc = 0;
    let argv = NonNull::new(unsafe { CommandLineToArgvW(terminated.as_ptr(), &mut argc) })
        .ok_or_else(io::Error::last_os_error)?;
    let args = unsafe { slice::from_raw_parts(argv.as_ptr(), argc.max(0) as usize) }
        .iter()
        .map(|&arg| {
            let mut n = 0;
            unsafe {
                while *arg.add(n) != 0 {
                    n += 1;
                }
                OsString::from_wide(slice::from_raw_parts(arg, n))
            }
        })
        .collect();
    unsafe { LocalFree(argv.as_ptr().cast()) };
    Ok(args)
}

fn validate_game(process: &Handle) -> io::Result<()> {
    let executable = process_path(process)?;
    let game = executable
        .file_name()
        .and_then(Game::from_name)
        .ok_or_else(|| {
            io::Error::other("the launcher only loads Inventory Mod into a Left 4 Dead 2 process")
        })?;
    if game == Game::Server {
        return Err(io::Error::other(
            "use start-server or Source plugin_load to initialize the dedicated server plugin",
        ));
    }
    let args = process_arguments(process)?;
    let is = |arg: &OsStr, name: &str| arg.to_string_lossy().eq_ignore_ascii_case(name);
    if !args.iter().any(|arg| is(arg, "-insecure")) {
        return Err(io::Error::other(
            "start Left 4 Dead 2 with -insecure before loading Inventory Mod",
        ));
    }
    let mut machine = 0;
    let mut native = 0;
    if unsafe { IsWow64Process2(process.raw(), &mut machine, &mut native) } == 0 {
        return Err(io::Error::last_os_error());
    }
    if if machine == 0 { native } else { machine } != I386 {
        return Err(io::Error::other("the running game must be an x86 process"));
    }
    Ok(())
}

/// Locate the same Windows system module in the game's x86 module list.
fn loader(pid: u32) -> io::Result<LPTHREAD_START_ROUTINE> {
    let kernel = unsafe { GetModuleHandleW(wide(OsStr::new("kernel32.dll")).as_ptr()) };
    let load = unsafe { GetProcAddress(kernel, c"LoadLibraryW".as_ptr().cast()) }
        .ok_or_else(io::Error::last_os_error)?;
    let address = load as usize;
    let mut base = ptr::null_mut();
    if unsafe {
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            ptr::with_exposed_provenance(address),
            &mut base,
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    let offset = address - base.addr();
    let mut path = [0_u16; 1024];
    let len = unsafe { GetModuleFileNameW(base, path.as_mut_ptr(), path.len() as u32) };
    if len == 0 || len as usize >= path.len() {
        return Err(io::Error::last_os_error());
    }
    let path = PathBuf::from(OsString::from_wide(&path[..len as usize]));
    let filename = path
        .file_name()
        .ok_or_else(|| io::Error::other("could not name the Windows loader module"))?;
    let modules = Handle::new(unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid) })?;
    let mut module = MODULEENTRY32W {
        dwSize: size_of::<MODULEENTRY32W>() as u32,
        ..Default::default()
    };
    let mut status = unsafe { Module32FirstW(modules.raw(), &mut module) };
    while status != 0 {
        let name = wide_string(&module.szModule);
        if name
            .to_string_lossy()
            .eq_ignore_ascii_case(&filename.to_string_lossy())
            && offset < module.modBaseSize as usize
        {
            let remote = module.modBaseAddr.addr() + offset;
            return Ok(Some(unsafe {
                std::mem::transmute::<usize, unsafe extern "system" fn(*mut c_void) -> u32>(remote)
            }));
        }
        status = unsafe { Module32NextW(modules.raw(), &mut module) };
    }
    Err(io::Error::other(
        "could not find the Windows loader in the game",
    ))
}

struct Argument<'a> {
    process: &'a Handle,
    address: *mut c_void,
    release: bool,
}

impl Drop for Argument<'_> {
    fn drop(&mut self) {
        if self.release {
            unsafe { VirtualFreeEx(self.process.raw(), self.address, 0, MEM_RELEASE) };
        }
    }
}

/// Find an existing copy, including either of the standard Cargo/release names.
fn inventory_module(pid: u32, dll: &Path) -> io::Result<Option<usize>> {
    find_module(pid, |module| {
        let name = wide_string(&module.szModule);
        ["inventory_mod.dll", "inventory_mod_client.dll"]
            .iter()
            .any(|candidate| name.eq_ignore_ascii_case(candidate))
            || dll
                .file_name()
                .is_some_and(|candidate| name.eq_ignore_ascii_case(candidate))
    })
}

fn module_present(pid: u32, address: usize) -> io::Result<bool> {
    Ok(find_module(pid, |module| module.modBaseAddr.addr() == address)?.is_some())
}

fn find_module(pid: u32, matches: impl Fn(&MODULEENTRY32W) -> bool) -> io::Result<Option<usize>> {
    let list = Handle::new(unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid) })?;
    let mut module = MODULEENTRY32W {
        dwSize: size_of::<MODULEENTRY32W>() as u32,
        ..Default::default()
    };
    if unsafe { Module32FirstW(list.raw(), &mut module) } == 0 {
        return Err(io::Error::last_os_error());
    }
    loop {
        if matches(&module) {
            return Ok(Some(module.modBaseAddr.addr()));
        }
        if unsafe { Module32NextW(list.raw(), &mut module) } == 0 {
            let error = io::Error::last_os_error();
            if error.raw_os_error() == Some(18) {
                return Ok(None);
            } // ERROR_NO_MORE_FILES
            return Err(error);
        }
    }
}

fn attach(pid: u32, dll: &Path) -> io::Result<()> {
    let process = Handle::new(unsafe {
        OpenProcess(
            PROCESS_CREATE_THREAD
                | PROCESS_QUERY_INFORMATION
                | PROCESS_VM_OPERATION
                | PROCESS_VM_WRITE
                | PROCESS_VM_READ,
            0,
            pid,
        )
    })?;
    validate_game(&process)?;
    let name = wide(OsStr::new(&format!("Local\\InventoryMod.Attach.{pid}")));
    let lock = unsafe { CreateMutexW(ptr::null(), 0, name.as_ptr()) };
    let already_exists = unsafe { GetLastError() } == ERROR_ALREADY_EXISTS;
    let _lock = Handle::new(lock)?;
    if already_exists || inventory_module(pid, dll)?.is_some() {
        return Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            "Inventory Mod is already loaded or is being loaded",
        ));
    }
    let load = loader(pid)?;
    let path = wide(dll.as_os_str());
    let size = path.len() * size_of::<u16>();
    let address = NonNull::new(unsafe {
        VirtualAllocEx(
            process.raw(),
            ptr::null(),
            size,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE,
        )
    })
    .ok_or_else(io::Error::last_os_error)?;
    let mut argument = Argument {
        process: &process,
        address: address.as_ptr(),
        release: true,
    };
    let mut written = 0;
    if unsafe {
        WriteProcessMemory(
            process.raw(),
            argument.address,
            path.as_ptr().cast(),
            size,
            &mut written,
        )
    } == 0
        || written != size
    {
        return Err(io::Error::last_os_error());
    }
    let thread = Handle::new(unsafe {
        CreateRemoteThread(
            process.raw(),
            ptr::null(),
            0,
            load,
            argument.address,
            0,
            ptr::null_mut(),
        )
    })?;
    match unsafe { WaitForSingleObject(thread.raw(), 10_000) } {
        WAIT_OBJECT_0 => {}
        WAIT_TIMEOUT => {
            // The game thread may still read the argument; its process will free this page on exit.
            argument.release = false;
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "the game did not finish loading Inventory Mod",
            ));
        }
        _ => {
            argument.release = false;
            return Err(io::Error::last_os_error());
        }
    }
    let mut result = 0;
    if unsafe { GetExitCodeThread(thread.raw(), &mut result) } == 0 {
        return Err(io::Error::last_os_error());
    }
    if result == 0 {
        return Err(io::Error::other(
            "the game could not load the DLL. Check that it is an x86 build",
        ));
    }
    Ok(())
}

fn default_dll() -> io::Result<PathBuf> {
    let mut path = std::env::current_exe()?;
    path.set_file_name("inventory_mod.dll");
    if !path.is_file() {
        path.set_file_name("inventory_mod_client.dll");
    }
    Ok(path)
}

pub fn run() -> io::Result<()> {
    let all = std::env::args_os().skip(1).collect::<Vec<_>>();
    if let Some((verb, rest)) = all.split_first()
        && let Some(result) = dev::dispatch(verb, rest)
    {
        return result;
    }
    let mut args = all.into_iter();
    let path = args
        .next()
        .map_or_else(default_dll, |arg| Ok(PathBuf::from(arg)))?;
    let target = args.next().unwrap_or_else(|| "left4dead2.exe".into());
    if args.next().is_some() {
        return Err(io::Error::other(
            "Usage: injector [inventory DLL] [left4dead2.exe|game PID]",
        ));
    }
    // Keep the ordinary Win32 path for older game loaders and validate it before allocating remotely.
    let dll = if path.is_absolute() {
        path
    } else {
        std::env::current_dir()?.join(path)
    };
    if !dll.is_file()
        || !dll
            .extension()
            .is_some_and(|extension| extension.eq_ignore_ascii_case("dll"))
    {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("DLL not found at {}", dll.display()),
        ));
    }
    let pid = if let Some(pid) = target
        .to_str()
        .and_then(|target| target.parse::<u32>().ok())
        .filter(|&pid| pid != 0)
    {
        pid
    } else {
        let game = Game::from_name(&target).ok_or_else(|| {
            io::Error::other("specify left4dead2.exe, srcds.exe or their process ID")
        })?;
        find_game(game)?
    };
    attach(pid, &dll)?;
    println!("Loaded {} into the game (PID {pid})", dll.display());
    Ok(())
}

pub fn wait_if_launched_from_explorer() {
    if std::env::args_os().nth(1).is_some() {
        return;
    }
    let mut processes = [0; 2];
    if unsafe { GetConsoleProcessList(processes.as_mut_ptr(), processes.len() as u32) } == 1 {
        use std::io::Write;
        print!("Press Enter to close.");
        let _ = io::stdout().flush();
        let _ = io::stdin().read_line(&mut String::new());
    }
}
