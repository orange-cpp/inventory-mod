use std::{
    ffi::{CStr, c_char, c_void},
    io, ptr,
    sync::atomic::{AtomicUsize, Ordering},
};

use windows_sys::Win32::System::LibraryLoader::GetProcAddress;

use crate::{
    client,
    native::{self, Detour, PeImage},
};

type Factory = unsafe extern "C" fn(*const c_char, *mut i32) -> *mut c_void;
type Query = unsafe extern "thiscall" fn(*mut c_void) -> bool;
type Command = unsafe extern "thiscall" fn(*mut c_void, *const c_char);
type Matrix = unsafe extern "thiscall" fn(*mut c_void) -> *const f32;
type ProcessPrint = unsafe extern "thiscall" fn(*mut c_void, *mut c_void) -> bool;

const ENGINE_TIMESTAMPS: [u32; 2] = [0x6a44_0ed6, 0x6afc_b59c];
const PROCESS_PRINT: usize = 0x52100;
const PRINT_TEXT: usize = 0x54;

const IS_IN_GAME: &[Option<u8>] = &[
    Some(0xe8),
    None,
    None,
    None,
    None,
    Some(0x33),
    Some(0xc9),
    Some(0x83),
    Some(0x78),
    Some(0x68),
    Some(0x06),
    Some(0x0f),
    Some(0x94),
    Some(0xc0),
    Some(0xc3),
];
const CONNECTED: &[Option<u8>] = &[
    Some(0xe8),
    None,
    None,
    None,
    None,
    Some(0x33),
    Some(0xc9),
    Some(0x83),
    Some(0x78),
    Some(0x68),
    Some(0x02),
    Some(0x0f),
    Some(0x9d),
    Some(0xc0),
    Some(0xc3),
];
const CLIENT_COMMAND: &[Option<u8>] = &[
    Some(0x55),
    Some(0x8b),
    Some(0xec),
    Some(0xa1),
    None,
    None,
    None,
    None,
    Some(0x8b),
    Some(0x40),
    Some(0x30),
];
const PRINT: &[Option<u8>] = &[
    Some(0x55),
    Some(0x8b),
    Some(0xec),
    Some(0x8b),
    Some(0x45),
    Some(0x08),
    Some(0x8b),
    Some(0x48),
    Some(0x54),
    Some(0x51),
    Some(0x68),
    None,
    None,
    None,
    None,
    Some(0xff),
    Some(0x15),
];

static ORIGINAL_PRINT: AtomicUsize = AtomicUsize::new(0);

#[derive(Clone, Copy)]
pub struct Link {
    interface: *mut c_void,
    is_in_game: Query,
    is_connected: Query,
    command: Command,
    world_to_screen: Option<Matrix>,
    game_ui: Option<(usize, Query)>,
}

pub struct Engine {
    pub link: Link,
    print: Option<Detour>,
}

impl Engine {
    pub fn bind() -> io::Result<Self> {
        let unsupported = || io::Error::other("this engine.dll is not supported");
        let module = native::module("engine.dll").ok_or_else(unsupported)?;
        let image = PeImage::loaded(module)
            .filter(|image| ENGINE_TIMESTAMPS.contains(&image.timestamp))
            .ok_or_else(unsupported)?;
        let factory =
            unsafe { GetProcAddress(module.as_ptr(), c"CreateInterface".as_ptr().cast()) }
                .ok_or_else(unsupported)?;
        let factory = unsafe {
            std::mem::transmute::<unsafe extern "system" fn() -> isize, Factory>(factory)
        };
        let interface = unsafe { factory(c"VEngineClient013".as_ptr(), ptr::null_mut()) };
        if interface.is_null() {
            return Err(unsupported());
        }
        let resolve = |slot, signature: &[Option<u8>]| {
            native::vslot(interface.addr(), slot)
                .filter(|&address| {
                    image.includes(address, signature.len()) && native::matches(address, signature)
                })
                .ok_or_else(unsupported)
        };
        let print_address = image.base + PROCESS_PRINT;
        if !image.includes(print_address, PRINT.len()) || !native::matches(print_address, PRINT) {
            return Err(unsupported());
        }
        let in_game = resolve(26, IS_IN_GAME)?;
        let connected = resolve(27, CONNECTED)?;
        let command = resolve(107, CLIENT_COMMAND)?;
        let game_ui = unsafe { factory(c"VEngineVGui001".as_ptr(), ptr::null_mut()) }.addr();
        let game_ui = native::vslot(game_ui, 2)
            .filter(|&function| {
                image.includes(function, 8)
                    && native::matches_pattern(function, "83 B9 84 00 00 00 00 74")
            })
            .map(|function| {
                (game_ui, unsafe {
                    std::mem::transmute::<usize, Query>(function)
                })
            });
        let matrix = |slot, offset| {
            native::vslot(interface.addr(), slot)
                .filter(|&address| {
                    image.includes(address, 13)
                        && native::matches_pattern(
                            address,
                            &format!("8B 0D ?? ?? ?? ?? 8B 01 8B 50 {offset:02X} FF E2"),
                        )
                })
                .map(|address| unsafe { std::mem::transmute::<usize, Matrix>(address) })
        };
        let mut print = Detour::prepare(print_address, process_print as ProcessPrint as usize)?;
        ORIGINAL_PRINT.store(print.trampoline(), Ordering::Release);
        if let Err(error) = print.enable() {
            ORIGINAL_PRINT.store(0, Ordering::Release);
            return Err(error);
        }
        let link = Link {
            interface,
            is_in_game: unsafe { std::mem::transmute::<usize, Query>(in_game) },
            is_connected: unsafe { std::mem::transmute::<usize, Query>(connected) },
            command: unsafe { std::mem::transmute::<usize, Command>(command) },
            world_to_screen: matrix(37, 0x34),
            game_ui,
        };
        Ok(Self {
            link,
            print: Some(print),
        })
    }

    pub fn stop(&mut self) -> io::Result<()> {
        if let Some(print) = &mut self.print {
            print.disable()?;
        }
        ORIGINAL_PRINT.store(0, Ordering::Release);
        Ok(())
    }

    pub fn offline() -> Self {
        unsafe extern "thiscall" fn query(_: *mut c_void) -> bool {
            false
        }
        unsafe extern "thiscall" fn command(_: *mut c_void, _: *const c_char) {}
        Self {
            link: Link {
                interface: ptr::null_mut(),
                is_in_game: query,
                is_connected: query,
                command,
                world_to_screen: None,
                game_ui: None,
            },
            print: None,
        }
    }
}

impl Link {
    pub fn bound(self) -> bool {
        !self.interface.is_null()
    }

    pub fn menu_visible(self) -> bool {
        self.game_ui.is_some_and(|(interface, visible)| unsafe {
            visible(ptr::with_exposed_provenance_mut(interface))
        })
    }

    pub fn interface(self) -> usize {
        self.interface.addr()
    }

    pub fn in_game(self) -> bool {
        unsafe { (self.is_in_game)(self.interface) }
    }

    pub fn connected(self) -> bool {
        unsafe { (self.is_connected)(self.interface) }
    }

    pub fn send(self, command: &CStr) {
        // Calling IVEngineClient is restricted to the game's own window thread.
        unsafe { (self.command)(self.interface, command.as_ptr()) };
    }

    fn read_matrix(self, method: Matrix) -> Option<[[f32; 4]; 4]> {
        let address = unsafe { method(self.interface) }.addr();
        let mut data = [0_u8; 64];
        if !native::read(address, &mut data) {
            return None;
        }
        let words = data.as_chunks::<4>().0;
        Some(std::array::from_fn(|row| {
            std::array::from_fn(|col| f32::from_le_bytes(words[row * 4 + col]))
        }))
    }

    pub fn projection(self) -> Option<Projection> {
        let screen = self.read_matrix(self.world_to_screen?)?;
        Some(Projection {
            screen,
            eye: eye_position(&screen),
        })
    }
}

pub struct Projection {
    screen: [[f32; 4]; 4],
    eye: Option<[f32; 3]>,
}

impl Projection {
    pub fn point(
        &self,
        [x, y, z]: [f32; 3],
        width: i32,
        height: i32,
    ) -> Option<(f32, f32, bool, Option<i32>)> {
        let apply = |[a, b, c, d]: [f32; 4]| a * x + b * y + c * z + d;
        let w = apply(self.screen[3]);
        let front = w > 0.001;
        let divisor = if front { w } else { -w.abs().max(0.001) };
        let px = apply(self.screen[0]) / divisor;
        let py = apply(self.screen[1]) / divisor;
        if !w.is_finite() || !px.is_finite() || !py.is_finite() {
            return None;
        }
        let distance = self
            .eye
            .map(|[a, b, c]| ((x - a).hypot(y - b).hypot(z - c) * 0.01905).round() as i32);
        Some((
            (px + 1.0) * width as f32 * 0.5,
            (1.0 - py) * height as f32 * 0.5,
            front,
            distance,
        ))
    }
}

fn eye_position(matrix: &[[f32; 4]; 4]) -> Option<[f32; 3]> {
    let determinant = |[[a, b, c], [d, e, f], [g, h, i]]: [[f32; 3]; 3]| {
        a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    };
    let [a, b, c] = [matrix[0], matrix[1], matrix[3]];
    let rows = [a, b, c];
    let base = rows.map(|[x, y, z, _]| [x, y, z]);
    let divisor = determinant(base);
    if !divisor.is_finite() || divisor.abs() < 1e-12 {
        return None;
    }
    Some(std::array::from_fn(|column| {
        let mut copy = base;
        for (row, values) in rows.iter().enumerate() {
            copy[row][column] = -values[3];
        }
        determinant(copy) / divisor
    }))
}

unsafe extern "thiscall" fn process_print(state: *mut c_void, message: *mut c_void) -> bool {
    let captured = std::panic::catch_unwind(|| {
        let Some(addr) =
            native::word(message.addr().wrapping_add(PRINT_TEXT)).map(|addr| addr as usize)
        else {
            return false;
        };
        let mut marker = [0; 5];
        if !native::read(addr, &mut marker)
            || !matches!(
                &marker,
                b"@inv@" | b"@inf@" | b"@sta@" | b"@png@" | b"@mel@" | b"@mut@" | b"@mhi@"
            )
        {
            return false;
        }
        if let Some(text) = native::string(addr, 8192) {
            client::receive(&text);
            true
        } else {
            false
        }
    })
    .unwrap_or(false);
    if captured {
        return true;
    }
    let address = ORIGINAL_PRINT.load(Ordering::Acquire);
    if address == 0 {
        return false;
    }
    let function = unsafe { std::mem::transmute::<usize, ProcessPrint>(address) };
    unsafe { function(state, message) }
}
