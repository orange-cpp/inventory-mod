use std::{
    ffi::{OsString, c_void},
    io,
    os::windows::ffi::{OsStrExt, OsStringExt},
    path::PathBuf,
    ptr::{self, NonNull},
};

use iced_x86::{
    BlockEncoder, BlockEncoderOptions, Code, Decoder, DecoderOptions, Instruction, InstructionBlock,
};
use windows_sys::Win32::{
    Foundation::{FreeLibrary, HMODULE},
    System::{
        Diagnostics::Debug::{FlushInstructionCache, ReadProcessMemory, WriteProcessMemory},
        LibraryLoader::{
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, GetModuleFileNameW, GetModuleHandleExW,
            GetModuleHandleW,
        },
        Memory::{
            MEM_COMMIT, MEM_RELEASE, MEM_RESERVE, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE,
            PAGE_READWRITE, VirtualAlloc, VirtualFree, VirtualProtect,
        },
        Threading::GetCurrentProcess,
    },
};

pub fn wide(text: &str) -> Vec<u16> {
    std::ffi::OsStr::new(text)
        .encode_wide()
        .chain([0])
        .collect()
}

pub fn module(name: &str) -> Option<NonNull<c_void>> {
    NonNull::new(unsafe { GetModuleHandleW(wide(name).as_ptr()) })
}

pub fn module_path(module: HMODULE) -> Option<PathBuf> {
    let mut buffer = vec![0_u16; 32_768];
    let length =
        unsafe { GetModuleFileNameW(module, buffer.as_mut_ptr(), buffer.len() as u32) } as usize;
    if length == 0 || length >= buffer.len() {
        return None;
    }
    Some(PathBuf::from(OsString::from_wide(&buffer[..length])))
}

fn address(value: usize) -> *const c_void {
    ptr::with_exposed_provenance(value)
}

pub fn read(addr: usize, target: &mut [u8]) -> bool {
    let mut read = 0;
    // ReadProcessMemory returns an error for an invalid address in the current game.
    unsafe {
        ReadProcessMemory(
            GetCurrentProcess(),
            address(addr),
            target.as_mut_ptr().cast(),
            target.len(),
            &mut read,
        ) != 0
            && read == target.len()
    }
}

pub fn word(addr: usize) -> Option<u32> {
    let mut bytes = [0; 4];
    read(addr, &mut bytes).then(|| u32::from_le_bytes(bytes))
}

pub fn store_word(addr: usize, value: u32) -> bool {
    store_bytes(addr, &value.to_le_bytes())
}

pub fn store_bytes(addr: usize, bytes: &[u8]) -> bool {
    let mut written = 0;
    unsafe {
        WriteProcessMemory(
            GetCurrentProcess(),
            address(addr).cast_mut(),
            bytes.as_ptr().cast(),
            bytes.len(),
            &mut written,
        ) != 0
            && written == bytes.len()
    }
}

pub fn string(addr: usize, max: usize) -> Option<String> {
    let mut text = Vec::new();
    // Limit the largest read to the rest of its virtual memory page.
    while text.len() < max {
        let cursor = addr.checked_add(text.len())?;
        let count = ((4096 - cursor % 4096).min(max - text.len())).min(128);
        let mut buffer = [0; 128];
        let bytes = &mut buffer[..count];
        if !read(cursor, bytes) {
            return None;
        }
        if let Some(end) = bytes.iter().position(|&byte| byte == 0) {
            text.extend_from_slice(&bytes[..end]);
            return Some(String::from_utf8_lossy(&text).into_owned());
        }
        text.extend_from_slice(bytes);
    }
    None
}

pub fn vslot(object: usize, index: usize) -> Option<usize> {
    let table = word(object)? as usize;
    word(table.checked_add(index.checked_mul(size_of::<usize>())?)?).map(|word| word as usize)
}

pub fn matches(addr: usize, pattern: &[Option<u8>]) -> bool {
    let mut actual = vec![0; pattern.len()];
    read(addr, &mut actual)
        && actual
            .into_iter()
            .zip(pattern)
            .all(|(actual, want)| want.is_none_or(|want| want == actual))
}

pub fn matches_pattern(addr: usize, pattern: &str) -> bool {
    let bytes = pattern
        .split_ascii_whitespace()
        .map(|p| u8::from_str_radix(p, 16).ok())
        .collect::<Vec<_>>();
    matches(addr, &bytes)
}

pub fn find_pattern(addr: usize, size: usize, pattern: &str) -> Option<usize> {
    let expected = pattern
        .split_ascii_whitespace()
        .map(|byte| u8::from_str_radix(byte, 16).ok())
        .collect::<Vec<_>>();
    if expected.is_empty() || expected.len() > size {
        return None;
    }
    let mut bytes = vec![0; size];
    if !read(addr, &mut bytes) {
        return None;
    }
    bytes
        .windows(expected.len())
        .position(|window| {
            window
                .iter()
                .zip(&expected)
                .all(|(&actual, want)| want.is_none_or(|want| want == actual))
        })
        .and_then(|offset| addr.checked_add(offset))
}

pub struct PeImage {
    pub base: usize,
    pub size: usize,
    pub timestamp: u32,
    pub code: std::ops::Range<usize>,
}

impl PeImage {
    pub fn loaded(module: NonNull<c_void>) -> Option<Self> {
        let base = module.as_ptr().addr();
        let mut dos = [0; 64];
        if !read(base, &mut dos) || &dos[..2] != b"MZ" {
            return None;
        }
        let header = u32::from_le_bytes(dos[60..64].try_into().unwrap()) as usize;
        let mut pe = [0; 84];
        if !read(base.checked_add(header)?, &mut pe)
            || &pe[..4] != b"PE\0\0"
            || u16::from_le_bytes(pe[24..26].try_into().unwrap()) != 0x10b
        {
            return None;
        }
        Some(Self {
            base,
            timestamp: u32::from_le_bytes(pe[8..12].try_into().unwrap()),
            size: u32::from_le_bytes(pe[80..84].try_into().unwrap()) as usize,
            code: {
                let offset = u32::from_le_bytes(pe[44..48].try_into().unwrap()) as usize;
                let size = u32::from_le_bytes(pe[28..32].try_into().unwrap()) as usize;
                let start = base.checked_add(offset)?;
                start..start.checked_add(size)?
            },
        })
    }

    pub fn includes(&self, addr: usize, size: usize) -> bool {
        addr.checked_sub(self.base)
            .and_then(|offset| offset.checked_add(size))
            .is_some_and(|end| end <= self.size)
    }
}

pub struct Detour {
    entry: usize,
    allocation: NonNull<c_void>,
    original: Vec<u8>,
    replacement: Vec<u8>,
    enabled: bool,
    lease: ModuleLease,
}

pub struct VtablePatch {
    slot: usize,
    expected: u32,
    replacement: u32,
    enabled: bool,
    lease: ModuleLease,
}

pub mod resident;

/// The client worker can defer unload on an ownership conflict. Source's
/// dedicated-plugin Unload callback cannot. Secure a strong loader reference
/// before patching, so forced destruction can preserve a chained predecessor.
struct ModuleLease(Option<NonNull<c_void>>);

impl ModuleLease {
    fn acquire() -> io::Result<Self> {
        let mut module = ptr::null_mut();
        if unsafe {
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                Self::acquire as *const () as *const u16,
                &mut module,
            )
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok(Self(NonNull::new(module)))
    }

    fn keep(&mut self) {
        self.0.take();
    }
}

impl Drop for ModuleLease {
    fn drop(&mut self) {
        if let Some(module) = self.0 {
            unsafe { FreeLibrary(module.as_ptr()) };
        }
    }
}

impl VtablePatch {
    pub fn install(slot: usize, expected: usize, replacement: usize) -> io::Result<Self> {
        if !slot.is_multiple_of(align_of::<u32>()) || word(slot) != Some(expected as u32) {
            return Err(io::Error::other("unsupported game vtable"));
        }
        let lease = ModuleLease::acquire()?;
        Self::exchange(slot, expected as u32, replacement as u32)?;
        Ok(Self {
            slot,
            expected: expected as u32,
            replacement: replacement as u32,
            enabled: true,
            lease,
        })
    }

    fn exchange(slot: usize, expected: u32, value: u32) -> io::Result<()> {
        use std::sync::atomic::{AtomicU32, Ordering};

        let addr = address(slot);
        let mut previous = 0;
        if unsafe { VirtualProtect(addr, 4, PAGE_READWRITE, &mut previous) } == 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: The checked slot is an aligned x86 function pointer.
        let changed = unsafe { AtomicU32::from_ptr(addr.cast_mut().cast()) }.compare_exchange(
            expected,
            value,
            Ordering::AcqRel,
            Ordering::Acquire,
        );
        unsafe { VirtualProtect(addr, 4, previous, &mut previous) };
        changed.map(|_| ()).map_err(|current| {
            io::Error::new(
                io::ErrorKind::WouldBlock,
                format!(
                    "Source slot {slot:#x} now points at {current:#x} (expected {expected:#x})"
                ),
            )
        })
    }

    pub fn restore(&mut self) -> io::Result<()> {
        if self.enabled {
            Self::exchange(self.slot, self.replacement, self.expected)?;
        }
        self.enabled = false;
        Ok(())
    }

    fn retire_forwarder(&mut self) -> io::Result<()> {
        if let Err(error) = self.restore() {
            if error.kind() != io::ErrorKind::WouldBlock {
                return Err(error);
            }
            // A later owner's saved predecessor is a permanent pass-through.
            self.enabled = false;
        }
        Ok(())
    }
}

impl Drop for VtablePatch {
    fn drop(&mut self) {
        if self.restore().is_err() {
            self.lease.keep();
        }
    }
}

impl Detour {
    pub fn prepare(entry: usize, hook: usize) -> io::Result<Self> {
        let lease = ModuleLease::acquire()?;
        let rejected = || io::Error::other("could not decode the Source function");
        let mut source = [0; 32];
        if !read(entry, &mut source) {
            return Err(rejected());
        }
        let mut decoder = Decoder::with_ip(32, &source, entry as u64, DecoderOptions::NONE);
        let mut instructions = Vec::new();
        while decoder.position() < 5 {
            let instruction = decoder.decode();
            if instruction.is_invalid() {
                return Err(rejected());
            }
            instructions.push(instruction);
        }
        let stolen = decoder.position();
        let back = entry.wrapping_add(stolen);
        let jump =
            Instruction::with_branch(Code::Jmp_rel32_32, back as u64).map_err(io::Error::other)?;
        // Leave the synthetic return branch with IP zero. Targeting the original
        // handler's tail must not make the encoder redirect to the trampoline.
        instructions.push(jump);
        let allocation = NonNull::new(unsafe {
            VirtualAlloc(ptr::null(), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
        })
        .ok_or_else(io::Error::last_os_error)?;
        let mut result = Self {
            entry,
            allocation,
            original: source[..stolen].to_vec(),
            replacement: vec![0x90; stolen],
            enabled: false,
            lease,
        };
        let block = InstructionBlock::new(&instructions, result.trampoline() as u64);
        let bytes = BlockEncoder::encode(32, block, BlockEncoderOptions::NONE)
            .map_err(io::Error::other)?
            .code_buffer;
        if bytes.len() >= 4096 {
            return Err(rejected());
        }
        // SAFETY: The allocation is a page of writable memory owned by this detour.
        unsafe {
            ptr::copy_nonoverlapping(
                bytes.as_ptr(),
                allocation.as_ptr().cast::<u8>(),
                bytes.len(),
            )
        };
        let mut previous = 0;
        if unsafe { VirtualProtect(allocation.as_ptr(), 4096, PAGE_EXECUTE_READ, &mut previous) }
            == 0
        {
            return Err(io::Error::last_os_error());
        }
        unsafe { FlushInstructionCache(GetCurrentProcess(), allocation.as_ptr(), bytes.len()) };
        result.replacement[0] = 0xe9;
        let offset = hook.wrapping_sub(entry.wrapping_add(5));
        result.replacement[1..5].copy_from_slice(&offset.to_le_bytes());
        Ok(result)
    }

    pub fn trampoline(&self) -> usize {
        self.allocation.as_ptr().addr()
    }

    pub fn enable(&mut self) -> io::Result<()> {
        let mut observed = vec![0; self.original.len()];
        if !read(self.entry, &mut observed) || observed != self.original {
            return Err(io::Error::other(
                "the Source function changed before initialization",
            ));
        }
        write_code(self.entry, &self.replacement)?;
        self.enabled = true;
        Ok(())
    }

    pub fn disable(&mut self) -> io::Result<()> {
        if self.enabled {
            let mut current = vec![0; self.replacement.len()];
            if !read(self.entry, &mut current) || current != self.replacement {
                return Err(io::Error::new(
                    io::ErrorKind::WouldBlock,
                    format!("game hook at {:#x} changed after installation", self.entry),
                ));
            }
            write_code(self.entry, &self.original)?;
            self.enabled = false;
        }
        Ok(())
    }
}

impl Drop for Detour {
    fn drop(&mut self) {
        if self.disable().is_ok() {
            unsafe { VirtualFree(self.allocation.as_ptr(), 0, MEM_RELEASE) };
        } else {
            // The later owner can invoke our handler, which still calls its
            // original trampoline. Both allocations must stay valid.
            self.lease.keep();
        }
    }
}

fn write_code(addr: usize, bytes: &[u8]) -> io::Result<()> {
    let address = address(addr);
    let mut previous = 0;
    if unsafe { VirtualProtect(address, bytes.len(), PAGE_EXECUTE_READWRITE, &mut previous) } == 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: The caller runs from the main game window procedure while the
    // Source print handler is idle. No other threads call this print handler.
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), address.cast_mut().cast::<u8>(), bytes.len());
        FlushInstructionCache(GetCurrentProcess(), address, bytes.len());
        let mut ignored = 0;
        VirtualProtect(address, bytes.len(), previous, &mut ignored);
    }
    Ok(())
}
