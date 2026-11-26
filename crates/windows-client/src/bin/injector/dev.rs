use std::{
    ffi::{OsStr, OsString},
    fs, io,
    path::{Path, PathBuf},
    process::Command,
    ptr, thread,
    time::{Duration, Instant},
};

use windows_sys::Win32::{
    Foundation::{HWND, LPARAM, RECT},
    Graphics::Gdi::{
        BI_RGB, BITMAPINFO, BITMAPINFOHEADER, CreateCompatibleDC, CreateDIBSection, DIB_RGB_COLORS,
        DeleteDC, DeleteObject, GetDC, ReleaseDC, SelectObject,
    },
    Storage::Xps::PrintWindow,
    System::Threading::{OpenProcess, PROCESS_QUERY_INFORMATION},
    UI::WindowsAndMessaging::{
        EnumWindows, GetClassNameW, GetWindowRect, GetWindowThreadProcessId, PostMessageW, WM_CHAR,
        WM_KEYDOWN, WM_KEYUP, WM_LBUTTONDOWN, WM_LBUTTONUP,
    },
};

use super::{Game, Handle, attach, default_dll, find_game, inventory_module, validate_game};

const USAGE: &str = "Commands: start-client [map], start-server [map] [port], unload [game PID], reload [game PID], key <virtual-key> [game PID], click <x> <y> [game PID], console <text> [game PID], shot <image.png> [game PID]";

pub fn dispatch(verb: &OsStr, args: &[OsString]) -> Option<io::Result<()>> {
    match verb.to_str()? {
        "help" | "--help" => {
            println!("{USAGE}\nInjector: injector [inventory DLL] [left4dead2.exe|game PID]");
            Some(Ok(()))
        }
        "start-client" => Some(start_client(args)),
        "start-server" => Some(start_server(args)),
        "unload" | "reload" => Some(unload(verb == "reload", args)),
        "key" | "click" | "console" | "shot" => Some(input(verb.to_str()?, args)),
        _ => None,
    }
}

fn args_error() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, USAGE)
}

fn folder(variable: &str, default: &str) -> PathBuf {
    std::env::var_os(variable).map_or_else(
        || Path::new(r"C:\Program Files (x86)\Steam\steamapps\common").join(default),
        PathBuf::from,
    )
}

fn map(arg: Option<&OsString>) -> io::Result<&str> {
    let name = arg.and_then(|s| s.to_str()).unwrap_or("c1m1_hotel");
    if name.is_empty() || !name.bytes().all(|n| n.is_ascii_alphanumeric() || n == b'_') {
        return Err(args_error());
    }
    Ok(name)
}

fn start_client(args: &[OsString]) -> io::Result<()> {
    if args.len() > 1 {
        return Err(args_error());
    }
    let dir = folder("L4D2_GAME_DIR", "Left 4 Dead 2");
    let mut launch = Command::new(dir.join("left4dead2.exe"));
    launch.current_dir(&dir).args([
        "-insecure",
        "-novid",
        "-windowed",
        "-w",
        "1280",
        "-h",
        "720",
    ]);
    if !args.is_empty() {
        launch.args(["+map", map(args.first())?]);
    }
    let mut game = launch.spawn()?;
    let until = Instant::now() + Duration::from_secs(60);
    while Instant::now() < until {
        if window(game.id()).is_some() {
            return attach(game.id(), &default_dll()?);
        }
        if game.try_wait()?.is_some() {
            return Err(io::Error::other(
                "Left 4 Dead 2 stopped before opening its window",
            ));
        }
        thread::sleep(Duration::from_millis(150));
    }
    Err(io::Error::new(
        io::ErrorKind::TimedOut,
        "Left 4 Dead 2 did not open its window",
    ))
}

fn start_server(args: &[OsString]) -> io::Result<()> {
    if args.len() > 2 {
        return Err(args_error());
    }
    let map = map(args.first())?;
    let port = args
        .get(1)
        .and_then(|arg| arg.to_str())
        .unwrap_or("27020")
        .parse::<u16>()
        .map_err(io::Error::other)?;
    let dir = folder("L4D2_SERVER_DIR", "Left 4 Dead 2 Dedicated Server");
    let plugin_dir = dir.join("left4dead2/addons/inventory_mod");
    fs::create_dir_all(&plugin_dir)?;
    let dll = default_dll()?;
    fs::copy(&dll, plugin_dir.join("inventory_mod.dll"))?;
    // Keep the server's own settings and rules.
    let addons = dll.parent().unwrap_or(Path::new(".")).join("addons");
    if let Ok(files) = fs::read_dir(addons) {
        let target = plugin_dir.join("addons");
        fs::create_dir_all(&target)?;
        for file in files {
            let file = file?;
            if file.file_type()?.is_file()
                && file
                    .path()
                    .extension()
                    .is_some_and(|ext| ext.eq_ignore_ascii_case("vpk"))
            {
                fs::copy(file.path(), target.join(file.file_name()))?;
            }
        }
    }
    let server = Command::new(dir.join("srcds.exe"))
        .current_dir(&dir)
        .args([
            "-console",
            "-game",
            "left4dead2",
            "-insecure",
            "-port",
            &port.to_string(),
            "+sv_lan",
            "1",
            "+plugin_load",
            "addons/inventory_mod/inventory_mod",
            "+map",
            map,
        ])
        .spawn()?;
    println!(
        "Started the LAN server (PID {}) with rules in {}",
        server.id(),
        plugin_dir.display()
    );
    Ok(())
}

fn target(args: &[OsString]) -> io::Result<u32> {
    match args {
        [] => find_game(Game::Client),
        [pid] => pid
            .to_str()
            .ok_or_else(args_error)?
            .parse()
            .map_err(io::Error::other),
        _ => Err(args_error()),
    }
}

fn check(pid: u32) -> io::Result<HWND> {
    let process = Handle::new(unsafe { OpenProcess(PROCESS_QUERY_INFORMATION, 0, pid) })?;
    validate_game(&process)?;
    window(pid).ok_or_else(|| io::Error::other("the game's window is unavailable"))
}

struct Search {
    pid: u32,
    hwnd: HWND,
}

fn window(pid: u32) -> Option<HWND> {
    let mut search = Search {
        pid,
        hwnd: ptr::null_mut(),
    };
    unsafe { EnumWindows(Some(find), (&mut search as *mut Search).addr() as LPARAM) };
    (!search.hwnd.is_null()).then_some(search.hwnd)
}

unsafe extern "system" fn find(hwnd: HWND, input: LPARAM) -> i32 {
    let search = unsafe { &mut *std::ptr::with_exposed_provenance_mut::<Search>(input as usize) };
    let mut pid = 0;
    unsafe { GetWindowThreadProcessId(hwnd, &mut pid) };
    if pid != search.pid {
        return 1;
    }
    let mut title = [0; 64];
    let n = unsafe { GetClassNameW(hwnd, title.as_mut_ptr(), title.len() as i32) } as usize;
    if title
        .get(..n)
        .is_some_and(|name| name.iter().copied().eq("Valve001".encode_utf16()))
    {
        search.hwnd = hwnd;
        0
    } else {
        1
    }
}

fn message(hwnd: HWND, code: u32, data: usize, extra: isize) -> io::Result<()> {
    if unsafe { PostMessageW(hwnd, code, data, extra) } == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

fn unload(reload: bool, args: &[OsString]) -> io::Result<()> {
    let pid = target(args)?;
    let hwnd = check(pid)?;
    let dll = default_dll()?;
    let (mut module, mut reply) = (inventory_module(pid, &dll)?, 0usize);
    use windows_sys::Win32::UI::WindowsAndMessaging::{SMTO_ABORTIFHUNG, SendMessageTimeoutW};
    if unsafe { SendMessageTimeoutW(hwnd, 0x804b, 0, 0, SMTO_ABORTIFHUNG, 3000, &mut reply) } != 0
        && reply != 0
    {
        module = Some(reply);
    }
    let module = module.ok_or_else(|| io::Error::other("Inventory Mod is not loaded"))?;
    message(hwnd, 0x804a, 0, 0)?;
    let deadline = Instant::now() + Duration::from_secs(30);
    while super::module_present(pid, module)? {
        if Instant::now() >= deadline {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "Inventory Mod did not finish unloading",
            ));
        }
        thread::sleep(Duration::from_millis(100));
    }
    if reload {
        attach(pid, &dll)?;
    }
    Ok(())
}

fn key(hwnd: HWND, code: usize) -> io::Result<()> {
    message(hwnd, WM_KEYDOWN, code, 1)?;
    message(hwnd, WM_KEYUP, code, 0xc000_0001_u32 as i32 as isize)
}

fn input(verb: &str, args: &[OsString]) -> io::Result<()> {
    let values = if verb == "click" { 2 } else { 1 };
    let (params, pid) = args.split_at_checked(values).ok_or_else(args_error)?;
    let hwnd = check(target(pid)?)?;
    let integer = |index: usize| {
        params[index]
            .to_str()
            .ok_or_else(args_error)?
            .parse::<i16>()
            .map_err(io::Error::other)
    };
    match verb {
        "key" => key(hwnd, integer(0)? as usize),
        "click" => {
            let x = integer(0)? as u16 as u32;
            let y = integer(1)? as u16 as u32;
            let point = ((y << 16) | x) as isize;
            message(hwnd, WM_LBUTTONDOWN, 1, point)?;
            message(hwnd, WM_LBUTTONUP, 0, point)
        }
        "console" => {
            key(hwnd, 0xc0)?;
            thread::sleep(Duration::from_millis(180));
            for char in params[0].to_string_lossy().encode_utf16() {
                message(hwnd, WM_CHAR, char as usize, 1)?;
            }
            key(hwnd, 13)?;
            thread::sleep(Duration::from_millis(180));
            key(hwnd, 0xc0)
        }
        "shot" => screenshot(hwnd, Path::new(&params[0])),
        _ => Err(args_error()),
    }
}

fn screenshot(window: HWND, path: &Path) -> io::Result<()> {
    let mut bounds = RECT::default();
    if unsafe { GetWindowRect(window, &mut bounds) } == 0 {
        return Err(io::Error::last_os_error());
    }
    let width = bounds.right - bounds.left;
    let height = bounds.bottom - bounds.top;
    if !(1..=8192).contains(&width) || !(1..=8192).contains(&height) {
        return Err(io::Error::other("unexpected game window dimensions"));
    }
    let source = unsafe { GetDC(window) };
    let target = unsafe { CreateCompatibleDC(source) };
    if target.is_null() {
        unsafe { ReleaseDC(window, source) };
        return Err(io::Error::last_os_error());
    }
    let info = BITMAPINFO {
        bmiHeader: BITMAPINFOHEADER {
            biSize: size_of::<BITMAPINFOHEADER>() as u32,
            biWidth: width,
            biHeight: -height,
            biPlanes: 1,
            biBitCount: 32,
            biCompression: BI_RGB,
            ..Default::default()
        },
        ..Default::default()
    };
    let mut bits = ptr::null_mut();
    let bitmap =
        unsafe { CreateDIBSection(target, &info, DIB_RGB_COLORS, &mut bits, ptr::null_mut(), 0) };
    let result = if !bitmap.is_null() && !bits.is_null() {
        let saved = unsafe { SelectObject(target, bitmap) };
        let output = if unsafe { PrintWindow(window, target, 2) } != 0 {
            let length = width as usize * height as usize * 4;
            let mut rgba =
                unsafe { std::slice::from_raw_parts(bits.cast::<u8>(), length) }.to_vec();
            for pixel in rgba.as_chunks_mut::<4>().0 {
                pixel.swap(0, 2);
                pixel[3] = 255;
            }
            image::save_buffer(
                path,
                &rgba,
                width as u32,
                height as u32,
                image::ColorType::Rgba8,
            )
            .map_err(io::Error::other)
        } else {
            Err(io::Error::last_os_error())
        };
        unsafe { SelectObject(target, saved) };
        output
    } else {
        Err(io::Error::last_os_error())
    };
    if !bitmap.is_null() {
        unsafe { DeleteObject(bitmap) };
    }
    unsafe {
        DeleteDC(target);
        ReleaseDC(window, source);
    }
    result
}
