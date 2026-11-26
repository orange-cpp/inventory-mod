use std::io;

use image::{
    Rgba, RgbaImage,
    imageops::{self, FilterType},
};
use texpresso::{Format, Params};

#[derive(Clone, Copy)]
enum Pixel {
    Rgba,
    Abgr,
    Rgb,
    Bgr,
    Bgra,
    Block(Format),
}

impl Pixel {
    fn from_id(id: u32) -> io::Result<Self> {
        Ok(match id {
            0 => Self::Rgba,
            1 => Self::Abgr,
            2 => Self::Rgb,
            3 => Self::Bgr,
            12 => Self::Bgra,
            13 | 20 => Self::Block(Format::Bc1),
            14 => Self::Block(Format::Bc2),
            15 => Self::Block(Format::Bc3),
            _ => return Err(invalid(&format!("unsupported VTF pixel format {id}"))),
        })
    }

    fn raw_channels(self) -> usize {
        match self {
            Self::Rgb | Self::Bgr => 3,
            _ => 4,
        }
    }

    fn byte_len(self, width: usize, height: usize) -> Option<usize> {
        match self {
            Self::Block(format) => width
                .div_ceil(4)
                .checked_mul(height.div_ceil(4))?
                .checked_mul(format.block_size()),
            _ => width.checked_mul(height)?.checked_mul(self.raw_channels()),
        }
    }
}

fn invalid(why: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, why)
}

fn word(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}

fn half(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes(bytes[offset..offset + 2].try_into().unwrap())
}

/// Read the first frame from the largest mip, including resources in VTF 7.3+.
pub fn decode(bytes: &[u8]) -> io::Result<RgbaImage> {
    if bytes.len() < 63 || &bytes[..4] != b"VTF\0" {
        return Err(invalid("not a VTF texture"));
    }
    let major = word(bytes, 4);
    let minor = word(bytes, 8);
    let header = word(bytes, 12) as usize;
    if major != 7 || !(63..=bytes.len()).contains(&header) {
        return Err(invalid("invalid VTF header"));
    }
    let width = usize::from(half(bytes, 16));
    let height = usize::from(half(bytes, 18));
    let frames = usize::from(half(bytes, 24));
    let format = Pixel::from_id(word(bytes, 52))?;
    let levels = bytes[56];
    if width == 0 || height == 0 || frames == 0 || !(1..=16).contains(&levels) {
        return Err(invalid("invalid VTF dimensions, frame count or mip count"));
    }
    let lo_format = word(bytes, 57);
    let lo_width = usize::from(bytes[61]);
    let lo_height = usize::from(bytes[62]);
    let lo_size = if lo_format == u32::MAX || lo_width == 0 || lo_height == 0 {
        0
    } else {
        Pixel::from_id(lo_format)?
            .byte_len(lo_width, lo_height)
            .ok_or_else(|| invalid("VTF thumbnail too large"))?
    };
    let mut high_offset = header
        .checked_add(lo_size)
        .ok_or_else(|| invalid("invalid VTF image offset"))?;
    if minor >= 3 {
        if header < 80 {
            return Err(invalid("invalid VTF resource header"));
        }
        let resources = word(bytes, 68) as usize;
        if resources > (header - 80) / 8 {
            return Err(invalid("invalid VTF resource count"));
        }
        for index in 0..resources {
            let start = 80 + 8 * index;
            if bytes[start..start + 3] == [0x30, 0, 0] {
                high_offset = word(bytes, start + 4) as usize;
                break;
            }
        }
    }
    let faces = if word(bytes, 20) & 0x4000 != 0 {
        if minor < 5 && half(bytes, 26) != u16::MAX {
            7
        } else {
            6
        }
    } else {
        1
    };
    let depth = if minor >= 2 && header >= 65 {
        usize::from(half(bytes, 63)).max(1)
    } else {
        1
    };
    for level in 1..levels {
        let size = format
            .byte_len((width >> level).max(1), (height >> level).max(1))
            .and_then(|size| size.checked_mul((depth >> level).max(1)))
            .and_then(|size| size.checked_mul(frames * faces))
            .and_then(|size| size.checked_add(high_offset))
            .ok_or_else(|| invalid("VTF mip or frame data exceeds the file size"))?;
        high_offset = size;
    }
    let len = format
        .byte_len(width, height)
        .ok_or_else(|| invalid("VTF image too large"))?;
    let end = high_offset
        .checked_add(len)
        .ok_or_else(|| invalid("VTF image too large"))?;
    let raw = bytes
        .get(high_offset..end)
        .ok_or_else(|| invalid("truncated VTF image"))?;
    let output_size = width
        .checked_mul(height)
        .and_then(|px| px.checked_mul(4))
        .filter(|&size| size <= isize::MAX as usize)
        .ok_or_else(|| invalid("VTF image too large"))?;
    let mut output = vec![0; output_size];
    if let Pixel::Block(format) = format {
        format.decompress(raw, width, height, &mut output);
    } else {
        for (target, source) in output
            .as_chunks_mut::<4>()
            .0
            .iter_mut()
            .zip(raw.chunks_exact(format.raw_channels()))
        {
            let pixel = match format {
                Pixel::Rgba => [source[0], source[1], source[2], source[3]],
                Pixel::Abgr => [source[3], source[2], source[1], source[0]],
                Pixel::Rgb => [source[0], source[1], source[2], 255],
                Pixel::Bgr => [source[2], source[1], source[0], 255],
                Pixel::Bgra => [source[2], source[1], source[0], source[3]],
                Pixel::Block(_) => unreachable!(),
            };
            *target = pixel;
        }
    }
    RgbaImage::from_raw(width as u32, height as u32, output)
        .ok_or_else(|| invalid("invalid VTF image buffer"))
}

fn compress(out: &mut Vec<u8>, format: Format, image: &RgbaImage) {
    let (width, height) = (image.width() as usize, image.height() as usize);
    let start = out.len();
    out.resize(start + format.compressed_size(width, height), 0);
    format.compress(image, width, height, Params::default(), &mut out[start..]);
}

/// Build VTF 7.2 with BC3 mips and the BC1 16x8 thumbnail.
pub fn encode(image: &RgbaImage) -> io::Result<Vec<u8>> {
    let (width, height) = image.dimensions();
    let width = u16::try_from(width)
        .ok()
        .filter(|&n| n > 0)
        .ok_or_else(|| invalid("invalid VTF width"))?;
    let height = u16::try_from(height)
        .ok()
        .filter(|&n| n > 0)
        .ok_or_else(|| invalid("invalid VTF height"))?;
    let levels = width.max(height).ilog2() as u8 + 1;
    let mut data = vec![0; 80];
    data[..4].copy_from_slice(b"VTF\0");
    data[4..8].copy_from_slice(&7_u32.to_le_bytes());
    data[8..12].copy_from_slice(&2_u32.to_le_bytes());
    data[12..16].copy_from_slice(&80_u32.to_le_bytes());
    data[16..18].copy_from_slice(&width.to_le_bytes());
    data[18..20].copy_from_slice(&height.to_le_bytes());
    data[20..24].copy_from_slice(&0x2000_u32.to_le_bytes());
    data[24..26].copy_from_slice(&1_u16.to_le_bytes());

    let count = u64::from(width) * u64::from(height);
    let rgb_sum = image.pixels().fold([0_u64; 3], |mut sum, pixel| {
        for channel in 0..3 {
            sum[channel] += u64::from(pixel[channel]);
        }
        sum
    });
    for (index, sum) in rgb_sum.into_iter().enumerate() {
        let reflectivity = (sum as f32 / count as f32 / 255.0).powf(2.2);
        data[32 + 4 * index..36 + 4 * index].copy_from_slice(&reflectivity.to_le_bytes());
    }
    data[48..52].copy_from_slice(&1.0_f32.to_le_bytes());
    data[52..56].copy_from_slice(&15_u32.to_le_bytes());
    data[56] = levels;
    data[57..61].copy_from_slice(&13_u32.to_le_bytes());
    data[61] = 16;
    data[62] = 8;
    data[63..65].copy_from_slice(&1_u16.to_le_bytes());

    let mut thumbnail = imageops::resize(image, 16, 8, FilterType::Lanczos3);
    for pixel in thumbnail.pixels_mut() {
        *pixel = Rgba([pixel[0], pixel[1], pixel[2], 255]);
    }
    compress(&mut data, Format::Bc1, &thumbnail);
    for level in (1..levels).rev() {
        let w = u32::from((width >> level).max(1));
        let h = u32::from((height >> level).max(1));
        compress(
            &mut data,
            Format::Bc3,
            &imageops::resize(image, w, h, FilterType::Lanczos3),
        );
    }
    compress(&mut data, Format::Bc3, image);
    Ok(data)
}
