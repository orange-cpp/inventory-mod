use std::{
    collections::BTreeMap,
    fs::File,
    io::{self, Read, Seek, SeekFrom},
    path::{Path, PathBuf},
};

const SIGNATURE: u32 = 0x55aa1234;
const INLINE: u16 = 0x7fff;

#[derive(Debug)]
pub struct Vpk {
    path: PathBuf,
    entries: BTreeMap<String, Entry>,
    data_offset: u64,
}

#[derive(Debug)]
struct Entry {
    crc: u32,
    archive: u16,
    offset: u32,
    length: u32,
    preload: Vec<u8>,
}

fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

fn take<'a>(input: &mut &'a [u8], len: usize) -> io::Result<&'a [u8]> {
    let (part, rest) = input
        .split_at_checked(len)
        .ok_or_else(|| invalid("truncated VPK tree"))?;
    *input = rest;
    Ok(part)
}

fn cstring(input: &mut &[u8]) -> io::Result<String> {
    let end = input
        .iter()
        .position(|&b| b == 0)
        .ok_or_else(|| invalid("unterminated VPK path"))?;
    Ok(take(input, end + 1)?[..end]
        .iter()
        .copied()
        .map(char::from)
        .collect())
}

fn u32_at(bytes: &[u8], index: usize) -> u32 {
    u32::from_le_bytes(bytes[index..index + 4].try_into().unwrap())
}

fn u16_at(bytes: &[u8], index: usize) -> u16 {
    u16::from_le_bytes(bytes[index..index + 2].try_into().unwrap())
}

impl Vpk {
    pub fn open(path: &Path) -> io::Result<Self> {
        Self::open_filtered(path, |_| true)
    }

    pub fn open_filtered(path: &Path, keep: impl Fn(&str) -> bool) -> io::Result<Self> {
        let mut reader = File::open(path)?;
        let mut header = [0; 12];
        reader.read_exact(&mut header)?;
        if u32_at(&header, 0) != SIGNATURE {
            return Err(invalid("not a VPK archive"));
        }
        let header_size = match u32_at(&header, 4) {
            1 => 12,
            2 => 28,
            _ => return Err(invalid("unsupported VPK version")),
        };
        let tree_size = u32_at(&header, 8);
        let data_offset = header_size + u64::from(tree_size);
        if data_offset > reader.metadata()?.len() {
            return Err(invalid("truncated VPK tree"));
        }
        reader.seek(SeekFrom::Start(header_size))?;
        let mut tree = Vec::new();
        reader.take(u64::from(tree_size)).read_to_end(&mut tree)?;
        if tree.len() != tree_size as usize {
            return Err(invalid("truncated VPK tree"));
        }
        let mut input = tree.as_slice();
        let mut entries = BTreeMap::new();
        loop {
            let extension = cstring(&mut input)?;
            if extension.is_empty() {
                break;
            }
            loop {
                let directory = cstring(&mut input)?;
                if directory.is_empty() {
                    break;
                }
                loop {
                    let name = cstring(&mut input)?;
                    if name.is_empty() {
                        break;
                    }
                    let fields = take(&mut input, 18)?;
                    if u16_at(fields, 16) != 0xffff {
                        return Err(invalid("invalid VPK entry terminator"));
                    }
                    let preload = take(&mut input, usize::from(u16_at(fields, 4)))?;
                    let mut full = if directory == " " {
                        String::new()
                    } else {
                        format!("{directory}/")
                    };
                    full.push_str(&name);
                    if extension != " " {
                        full.push('.');
                        full.push_str(&extension);
                    }
                    full.make_ascii_lowercase();
                    if keep(&full) {
                        entries.insert(
                            full,
                            Entry {
                                crc: u32_at(fields, 0),
                                preload: preload.to_owned(),
                                archive: u16_at(fields, 6),
                                offset: u32_at(fields, 8),
                                length: u32_at(fields, 12),
                            },
                        );
                    }
                }
            }
        }
        Ok(Self {
            path: path.to_owned(),
            entries,
            data_offset,
        })
    }

    pub fn names(&self) -> impl Iterator<Item = &str> {
        self.entries.keys().map(String::as_str)
    }

    pub fn contains(&self, name: &str) -> bool {
        self.entries.contains_key(&name.to_ascii_lowercase())
    }

    pub fn read(&self, name: &str) -> io::Result<Vec<u8>> {
        let entry = self
            .entries
            .get(&name.to_ascii_lowercase())
            .ok_or_else(|| {
                io::Error::new(
                    io::ErrorKind::NotFound,
                    format!("{name} is absent from {}", self.path.display()),
                )
            })?;
        let mut data = entry.preload.clone();
        if entry.length != 0 {
            let (mut file, offset) = if entry.archive == INLINE {
                (
                    File::open(&self.path)?,
                    self.data_offset + u64::from(entry.offset),
                )
            } else {
                let filename = self
                    .path
                    .file_name()
                    .and_then(|name| name.to_str())
                    .unwrap_or("");
                let prefix = filename
                    .strip_suffix("_dir.vpk")
                    .ok_or_else(|| invalid("external VPK archive requires a _dir.vpk filename"))?;
                let path = self
                    .path
                    .with_file_name(format!("{prefix}_{:03}.vpk", entry.archive));
                (File::open(path)?, u64::from(entry.offset))
            };
            if offset + u64::from(entry.length) > file.metadata()?.len() {
                return Err(invalid("truncated VPK file data"));
            }
            file.seek(SeekFrom::Start(offset))?;
            file.take(u64::from(entry.length)).read_to_end(&mut data)?;
        }
        if crc32fast::hash(&data) != entry.crc {
            return Err(invalid("VPK checksum mismatch"));
        }
        Ok(data)
    }
}

/// Version 1 with all data embedded, in deterministic extension/directory/name order.
pub fn pack(files: &BTreeMap<String, Vec<u8>>) -> io::Result<Vec<u8>> {
    let mut grouped = BTreeMap::<_, BTreeMap<_, BTreeMap<_, _>>>::new();
    for (path, bytes) in files {
        let (dir, filename) = path.rsplit_once('/').unwrap_or((" ", path.as_str()));
        let (stem, ext) = filename.rsplit_once('.').unwrap_or((filename, " "));
        if [dir, stem, ext]
            .into_iter()
            .any(|part| part.is_empty() || !part.is_ascii() || part.contains('\0'))
        {
            return Err(invalid("invalid VPK pathname"));
        }
        grouped
            .entry(ext)
            .or_default()
            .entry(dir)
            .or_default()
            .insert(stem, bytes);
    }

    let mut tree = Vec::new();
    let mut data = Vec::new();
    let write_name = |output: &mut Vec<u8>, name: &str| {
        output.extend_from_slice(name.as_bytes());
        output.push(0);
    };
    let size =
        |n: usize| u32::try_from(n).map_err(|_| invalid("VPK exceeds the version 1 size limit"));
    for (ext, directories) in grouped {
        write_name(&mut tree, ext);
        for (dir, entries) in directories {
            write_name(&mut tree, dir);
            for (stem, bytes) in entries {
                write_name(&mut tree, stem);
                tree.extend_from_slice(&crc32fast::hash(bytes).to_le_bytes());
                tree.extend_from_slice(&0_u16.to_le_bytes());
                tree.extend_from_slice(&INLINE.to_le_bytes());
                tree.extend_from_slice(&size(data.len())?.to_le_bytes());
                tree.extend_from_slice(&size(bytes.len())?.to_le_bytes());
                tree.extend_from_slice(&0xffff_u16.to_le_bytes());
                data.extend_from_slice(bytes);
            }
            tree.push(0);
        }
        tree.push(0);
    }
    tree.push(0);
    let mut vpk = Vec::with_capacity(12 + tree.len() + data.len());
    vpk.extend_from_slice(&SIGNATURE.to_le_bytes());
    vpk.extend_from_slice(&1_u32.to_le_bytes());
    vpk.extend_from_slice(&size(tree.len())?.to_le_bytes());
    vpk.extend(tree);
    vpk.extend(data);
    Ok(vpk)
}
