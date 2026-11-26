use std::{io, ops::Range};

#[derive(Clone, Copy)]
pub struct Bytes<'a>(pub &'a [u8]);

impl<'a> Bytes<'a> {
    pub fn at(self, offset: usize, count: usize) -> io::Result<&'a [u8]> {
        let range = offset
            .checked_add(count)
            .map(|end| Range { start: offset, end });
        range
            .and_then(|range| self.0.get(range))
            .ok_or_else(|| invalid("truncated binary data"))
    }

    pub fn u16(self, offset: usize) -> io::Result<u16> {
        Ok(u16::from_le_bytes(self.at(offset, 2)?.try_into().unwrap()))
    }

    pub fn u32(self, offset: usize) -> io::Result<u32> {
        Ok(u32::from_le_bytes(self.at(offset, 4)?.try_into().unwrap()))
    }

    pub fn i32(self, offset: usize) -> io::Result<i32> {
        Ok(self.u32(offset)? as i32)
    }

    pub fn u64(self, offset: usize) -> io::Result<u64> {
        Ok(u64::from_le_bytes(self.at(offset, 8)?.try_into().unwrap()))
    }

    pub fn f32(self, offset: usize) -> io::Result<f32> {
        Ok(f32::from_bits(self.u32(offset)?))
    }

    pub fn string(self, offset: usize) -> io::Result<String> {
        let tail = self.at(offset, self.0.len().saturating_sub(offset))?;
        let end = tail
            .iter()
            .position(|&byte| byte == 0)
            .ok_or_else(|| invalid("unterminated binary string"))?;
        Ok(String::from_utf8_lossy(&tail[..end]).into_owned())
    }
}

pub fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}
