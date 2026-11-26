use std::{collections::BTreeMap, io};

use crate::binary::{Bytes, invalid};

struct Module {
    base: u64,
    size: u32,
    name: String,
}

impl Module {
    fn filename(&self) -> &str {
        self.name.rsplit(['\\', '/']).next().unwrap_or("")
    }
}

pub fn summarize(data: &[u8]) -> io::Result<()> {
    let data = Bytes(data);
    if data.at(0, 4)? != b"MDMP" {
        return Err(invalid("not a minidump"));
    }
    let count = data.u32(8)? as usize;
    let start = data.u32(12)? as usize;
    let directory = data.at(
        start,
        count
            .checked_mul(12)
            .ok_or_else(|| invalid("invalid stream count"))?,
    )?;
    let streams = directory
        .as_chunks::<12>()
        .0
        .iter()
        .map(|entry| {
            let entry = Bytes(entry);
            Ok((
                entry.u32(0)?,
                (entry.u32(4)? as usize, entry.u32(8)? as usize),
            ))
        })
        .collect::<io::Result<BTreeMap<_, _>>>()?;
    let mut modules = Vec::new();
    if let Some(&(size, rva)) = streams.get(&4) {
        let stream = Bytes(data.at(rva, size)?);
        let count = stream.u32(0)? as usize;
        if count > size.saturating_sub(4) / 108 {
            return Err(invalid("invalid minidump module count"));
        }
        for index in 0..count {
            let entry = Bytes(stream.at(4 + 108 * index, 108)?);
            let name_at = entry.u32(20)? as usize;
            let name = data.at(name_at + 4, data.u32(name_at)? as usize)?;
            let (wide, left) = name.as_chunks::<2>();
            if !left.is_empty() {
                return Err(invalid("invalid UTF-16 string in module list"));
            }
            let wide = wide
                .iter()
                .map(|&pair| u16::from_le_bytes(pair))
                .collect::<Vec<_>>();
            modules.push(Module {
                base: entry.u64(0)?,
                size: entry.u32(8)?,
                name: String::from_utf16_lossy(&wide),
            });
        }
    }
    let location = |address| {
        modules
            .iter()
            .find(|module| address >= module.base && address - module.base < u64::from(module.size))
            .map(|module| format!("{}+0x{:X}", module.filename(), address - module.base))
            .unwrap_or_else(|| "unknown".into())
    };
    if let Some(&(size, rva)) = streams.get(&6) {
        let stream = Bytes(data.at(rva, size)?);
        let thread = stream.u32(0)?;
        let exception = stream.u32(8)?;
        let address = stream.u64(24)?;
        let params = stream.u32(32)?.min(15) as usize;
        println!(
            "thread {thread} exception 0x{exception:08X} at 0x{address:X} = {}",
            location(address)
        );
        let param = (0..params)
            .map(|index| {
                stream
                    .u64(40 + 8 * index)
                    .map(|value| format!("{value:#x}"))
            })
            .collect::<io::Result<Vec<_>>>()?;
        println!("params: {}", param.join(", "));
        let ctx_size = stream.u32(160)? as usize;
        let ctx_at = stream.u32(164)? as usize;
        println!("context size {ctx_size}");
        if ctx_size == 716 {
            let context = Bytes(data.at(ctx_at, ctx_size)?);
            let registers = [
                "Edi", "Esi", "Ebx", "Edx", "Ecx", "Eax", "Ebp", "Eip", "SegCs", "EFlags", "Esp",
                "SegSs",
            ];
            for (index, name) in registers.into_iter().enumerate() {
                let value = context.u32(0x9c + index * 4)?;
                println!("  {name} = 0x{value:08X} {}", location(u64::from(value)));
            }
        }
    }
    println!("modules loaded: {}", modules.len());
    for module in &modules {
        if module.filename().to_ascii_lowercase().contains("inventory") {
            println!("  {:#x} {}", module.base, module.name);
        }
    }
    Ok(())
}
