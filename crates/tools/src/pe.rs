use std::{
    collections::{BTreeMap, BTreeSet},
    ffi::OsString,
    io,
};

use iced_x86::{Decoder, DecoderOptions, Formatter, Instruction, IntelFormatter, Mnemonic, OpKind};

use crate::binary::{Bytes, invalid};

struct Pe {
    base: u32,
    data: Vec<u8>,
    sections: Vec<Section>,
    references: BTreeMap<u32, Vec<u32>>,
}

struct Section {
    name: String,
    addr: u32,
    size: u32,
    executable: bool,
}

impl Pe {
    fn parse(file: &[u8]) -> io::Result<Self> {
        let input = Bytes(file);
        if input.at(0, 2)? != b"MZ" {
            return Err(invalid("not a PE image"));
        }
        let header = input.u32(60)? as usize;
        if input.at(header, 4)? != b"PE\0\0" || input.u16(header + 4)? != 0x14c {
            return Err(invalid("expected a 32-bit x86 PE image"));
        }
        let section_count = input.u16(header + 6)? as usize;
        let opt_size = input.u16(header + 20)? as usize;
        let opt = Bytes(input.at(header + 24, opt_size)?);
        if opt.u16(0)? != 0x10b {
            return Err(invalid("expected a PE32 optional header"));
        }
        let base = opt.u32(28)?;
        let size = opt.u32(56)? as usize;
        let headers = opt.u32(60)? as usize;
        if size < headers || u64::from(base) + size as u64 > u64::from(u32::MAX) + 1 {
            return Err(invalid("invalid mapped PE image size"));
        }
        let mut data = Vec::new();
        data.try_reserve_exact(size).map_err(io::Error::other)?;
        data.resize(size, 0);
        data[..headers].copy_from_slice(input.at(0, headers)?);
        let mut sections = Vec::with_capacity(section_count);
        for index in 0..section_count {
            let offset = header + 24 + opt_size + 40 * index;
            let section = Bytes(input.at(offset, 40)?);
            let name = section
                .at(0, 8)?
                .split(|&byte| byte == 0)
                .next()
                .unwrap_or_default();
            let rva = section.u32(12)?;
            let section_size = section.u32(8)?;
            let virtual_offset = rva as usize;
            let raw_size = section.u32(16)? as usize;
            let raw_offset = section.u32(20)? as usize;
            let executable = section.u32(36)? & 0x2000_0000 != 0;
            let addr = base
                .checked_add(rva)
                .ok_or_else(|| invalid("invalid section address"))?;
            let width = raw_size.min(size.saturating_sub(virtual_offset));
            if width > 0 {
                data[virtual_offset..virtual_offset + width]
                    .copy_from_slice(input.at(raw_offset, width)?);
            }
            sections.push(Section {
                name: String::from_utf8_lossy(name).into_owned(),
                addr,
                size: section_size,
                executable,
            });
        }

        let mut image = Self {
            base,
            data,
            sections,
            references: BTreeMap::new(),
        };
        if opt.u32(92)? > 5 {
            image.index_relocations(opt.u32(136)? as usize, opt.u32(140)? as usize)?;
        }
        Ok(image)
    }

    fn index_relocations(&mut self, rva: usize, size: usize) -> io::Result<()> {
        let image = Bytes(&self.data);
        let table = image.at(rva, size)?;
        let mut blocks = table;
        while blocks.len() >= 8 {
            let block = Bytes(blocks);
            let page = block.u32(0)?;
            let len = block.u32(4)? as usize;
            if len == 0 {
                break;
            }
            if len < 8 || len > blocks.len() || !len.is_multiple_of(2) {
                return Err(invalid("invalid PE relocation block"));
            }
            for bytes in blocks[8..len].as_chunks::<2>().0 {
                let entry = u16::from_le_bytes(*bytes);
                if entry >> 12 != 3 {
                    continue;
                }
                let fixup = page
                    .checked_add(u32::from(entry & 0xfff))
                    .ok_or_else(|| invalid("invalid relocation address"))?;
                let stored = image.u32(fixup as usize)?;
                let addr = self
                    .base
                    .checked_add(fixup)
                    .ok_or_else(|| invalid("invalid relocation address"))?;
                self.references.entry(stored).or_default().push(addr);
            }
            blocks = &blocks[len..];
        }
        for entries in self.references.values_mut() {
            entries.sort_unstable();
        }
        Ok(())
    }

    fn read(&self, addr: u32, size: usize) -> &[u8] {
        addr.checked_sub(self.base)
            .and_then(|rva| self.data.get(rva as usize..))
            .map_or(&[], |bytes| &bytes[..size.min(bytes.len())])
    }

    fn word(&self, addr: u32) -> Option<u32> {
        let bytes = self.read(addr, 4);
        Some(u32::from_le_bytes(bytes.try_into().ok()?))
    }

    fn section(&self, address: u32) -> Option<&Section> {
        self.sections
            .iter()
            .find(|section| address >= section.addr && address - section.addr < section.size)
    }

    fn code(&self, addr: u32) -> bool {
        self.section(addr).is_some_and(|section| section.executable)
    }

    fn xrefs(&self, addr: u32) -> &[u32] {
        self.references.get(&addr).map_or(&[], Vec::as_slice)
    }

    fn strings(&self, needle: &str) -> Vec<(u32, &str)> {
        if needle.is_empty() {
            return Vec::new();
        }
        let matches = self
            .data
            .windows(needle.len())
            .enumerate()
            .filter(|(_, bytes)| *bytes == needle.as_bytes());
        let ascii = |c: u8| (32..127).contains(&c);
        let mut visited = BTreeSet::new();
        matches
            .filter_map(|(found, _)| {
                let begin = self.data[..found]
                    .iter()
                    .rposition(|&byte| !ascii(byte))
                    .map_or(0, |i| i + 1);
                if !visited.insert(begin) {
                    return None;
                }
                let end = self.data[found..]
                    .iter()
                    .position(|&byte| !ascii(byte))
                    .map_or(self.data.len(), |i| found + i);
                let text = std::str::from_utf8(&self.data[begin..end]).ok()?;
                Some((self.base.checked_add(begin as u32)?, text))
            })
            .collect()
    }

    fn text_at(&self, addr: u32) -> Option<&str> {
        self.section(addr)?;
        let bytes = self.read(addr, 80);
        let length = bytes
            .iter()
            .position(|&byte| !(32..127).contains(&byte))
            .unwrap_or(bytes.len());
        if length < 3 || bytes.get(length).is_some_and(|&byte| byte != 0) {
            return None;
        }
        std::str::from_utf8(&bytes[..length]).ok()
    }

    fn address(&self, arg: &str) -> io::Result<u32> {
        if let Some(name) = arg.strip_prefix("string:") {
            let strings = self.strings(name);
            return strings
                .iter()
                .find(|(_, text)| *text == name)
                .or_else(|| strings.first())
                .map(|(addr, _)| *addr)
                .ok_or_else(|| invalid("string not found"));
        }
        u32::from_str_radix(arg.trim_start_matches("0x"), 16).map_err(io::Error::other)
    }

    fn function_start(&self, addr: u32) -> u32 {
        let Some(section) = self.section(addr) else {
            return addr;
        };
        let mut aligned = addr & !15;
        while aligned >= section.addr {
            let before = aligned
                .checked_sub(1)
                .map(|va| self.read(va, 1))
                .unwrap_or_default();
            if (matches!(before, [0xcc] | [0xc3]) || self.read(aligned, 3) == [0x55, 0x8b, 0xec])
                && self.read(aligned, 1) != [0xcc]
            {
                return aligned;
            }
            let Some(next) = aligned.checked_sub(16) else {
                break;
            };
            aligned = next;
        }
        addr
    }

    fn disassemble(&self, addr: u32, count: usize, whole: bool) {
        let code = self.read(addr, 0x2000);
        let mut decoder = Decoder::with_ip(32, code, u64::from(addr), DecoderOptions::NONE);
        let mut formatter = IntelFormatter::new();
        formatter.options_mut().set_hex_prefix("0x");
        formatter.options_mut().set_hex_suffix("");
        formatter.options_mut().set_uppercase_hex(false);
        let mut text = String::new();
        for instruction in (&mut decoder).into_iter().take(count) {
            if instruction.is_invalid() {
                break;
            }
            text.clear();
            formatter.format(&instruction, &mut text);
            let note = self.note(&instruction);
            println!("{:#010x}  {text}{note}", instruction.ip32());
            if whole
                && instruction.mnemonic() == Mnemonic::Ret
                && matches!(self.read(instruction.next_ip32(), 1), [0xcc] | [0])
            {
                break;
            }
        }
    }

    fn note(&self, instruction: &Instruction) -> String {
        for operand in 0..instruction.op_count() {
            let target = match instruction.op_kind(operand) {
                OpKind::NearBranch16 | OpKind::NearBranch32 => {
                    instruction.near_branch_target() as u32
                }
                OpKind::Memory => instruction.memory_displacement32(),
                OpKind::Immediate16 | OpKind::Immediate32 => instruction.immediate(operand) as u32,
                _ => continue,
            };
            if self.code(target) {
                return format!("   ; -> {target:#x}");
            }
            if let Some(text) = self.text_at(target) {
                return format!("   ; \"{text}\"");
            }
        }
        String::new()
    }

    fn rtti(&self, prefix: &str) {
        let needle = if prefix.starts_with(".?") {
            prefix.to_owned()
        } else {
            format!(".?AV{prefix}")
        };
        for (text_at, text) in self
            .strings(&needle)
            .into_iter()
            .filter(|(_, name)| name.starts_with(".?A"))
        {
            let Some(typedesc) = text_at.checked_sub(8) else {
                continue;
            };
            let tables = self
                .xrefs(typedesc)
                .iter()
                .filter_map(|&xref| xref.checked_sub(12))
                .filter(|&col| self.word(col) == Some(0))
                .flat_map(|col| {
                    self.xrefs(col)
                        .iter()
                        .filter_map(|&xref| xref.checked_add(4))
                })
                .collect::<BTreeSet<_>>();
            let list = tables
                .iter()
                .map(|x| format!("{x:#x}"))
                .collect::<Vec<_>>()
                .join(", ");
            println!("{text}  typedesc={typedesc:#x}  vtables=[{list}]");
        }
    }
}

pub fn inspect(file: &[u8], args: &[OsString]) -> io::Result<()> {
    let bad = || {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "expected strings, xrefs, rtti, vtable, disasm or func",
        )
    };
    let image = Pe::parse(file)?;
    let command = args
        .first()
        .and_then(|text| text.to_str())
        .ok_or_else(bad)?;
    let name = args.get(1).and_then(|text| text.to_str());
    let n = args
        .get(2)
        .and_then(|text| text.to_str())
        .map(|text| text.parse::<usize>())
        .transpose()
        .map_err(io::Error::other)?;
    match command {
        "strings" => {
            let needle = name.ok_or_else(bad)?;
            for (addr, text) in image.strings(needle) {
                let section = image
                    .section(addr)
                    .map_or("", |section| section.name.as_str());
                println!("{addr:#010x} {section}  {text}");
            }
        }
        "xrefs" => {
            for &slot in image.xrefs(image.address(name.ok_or_else(bad)?)?) {
                let section = image
                    .section(slot)
                    .map_or("", |section| section.name.as_str());
                if image.code(slot) {
                    println!(
                        "{slot:#010x} [{section}] in function {:#x}",
                        image.function_start(slot)
                    );
                } else {
                    println!("{slot:#010x} [{section}]");
                }
            }
        }
        "rtti" => image.rtti(name.unwrap_or("")),
        "vtable" => {
            let start = image.address(name.ok_or_else(bad)?)?;
            for (index, addr) in (start..)
                .step_by(4)
                .take(n.unwrap_or(usize::MAX))
                .enumerate()
            {
                let Some(target) = image.word(addr).filter(|&target| image.code(target)) else {
                    break;
                };
                println!("[{index:3}] {target:#010x}");
            }
        }
        "disasm" => image.disassemble(
            image.address(name.ok_or_else(bad)?)?,
            n.unwrap_or(60),
            false,
        ),
        "func" => image.disassemble(
            image.address(name.ok_or_else(bad)?)?,
            n.unwrap_or(400),
            true,
        ),
        _ => return Err(bad()),
    }
    Ok(())
}
