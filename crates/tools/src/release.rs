use std::{
    fs::{self, File, OpenOptions},
    io::{self, Write},
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

use flate2::{Compression, GzBuilder};
use zip::{CompressionMethod, ZipWriter, write::SimpleFileOptions};

use crate::{Result, menu, rounds, vpk};

pub struct GameAssets([vpk::Vpk; 2]);

impl GameAssets {
    pub fn open(game: &Path) -> io::Result<Self> {
        let open = |folder| vpk::Vpk::open(&game.join(folder).join("pak01_dir.vpk"));
        Ok(Self([open("update")?, open("left4dead2")?]))
    }

    pub fn read(&self, name: &str) -> io::Result<Vec<u8>> {
        self.0
            .iter()
            .find(|archive| archive.contains(name))
            .ok_or_else(|| {
                io::Error::new(
                    io::ErrorKind::NotFound,
                    format!("missing game asset {name}"),
                )
            })?
            .read(name)
    }
}

pub fn addon(
    repo: &Path,
    game: &Path,
    ammo: bool,
    textures: Option<&Path>,
) -> Result<(usize, Vec<u8>)> {
    let archive = GameAssets::open(game)?;
    let files = if ammo {
        let defaults = repo.join("assets/textures");
        rounds::files(textures.unwrap_or(&defaults), |name| archive.read(name))?
    } else {
        menu::files(&archive.read(menu::FLYOUT)?)?
    };
    let size = files.len();
    Ok((size, vpk::pack(&files)?))
}

/// Package the current player release binaries and the original add-on layout.
pub fn windows(repo: &Path, binaries: &Path, output: &Path, game: Option<&Path>) -> Result<()> {
    archive(output, |file| write_zip(file, repo, binaries, game))
}

fn archive(output: &Path, write: impl FnOnce(File) -> Result<()>) -> Result<()> {
    let output_parent = output
        .parent()
        .filter(|dir| !dir.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    fs::create_dir_all(output_parent)?;
    let stamp = SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos();
    let temporary =
        output_parent.join(format!(".inventory-mod-{}-{stamp}.tmp", std::process::id()));
    let file = OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&temporary)?;
    let result = write(file).and_then(|()| {
        fs::rename(&temporary, output)?;
        Ok(())
    });
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}

pub fn linux(repo: &Path, plugin: &Path, output: &Path) -> Result<()> {
    let mut library = File::open(plugin)?;
    let mut identity = [0_u8; 20];
    use std::io::{Read, Seek, SeekFrom};
    library.read_exact(&mut identity)?;
    if &identity[..6] != b"\x7FELF\x01\x01" || identity[16..20] != [3, 0, 3, 0] {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "expected an x86 Linux ELF shared library",
        )
        .into());
    }
    library.seek(SeekFrom::Start(0))?;
    let size = library.metadata()?.len();
    let config = fs::read(repo.join("inventory_mod.example.toml"))?;
    let licenses = include_bytes!("../../linux-server/DEPENDENCIES-LICENSES.txt");
    archive(output, |file| {
        let gzip = GzBuilder::new().write(file, Compression::default());
        let mut tar = tar::Builder::new(gzip);
        for (path, mode, size, content) in [
            (
                "left4dead2/addons/inventory_mod_server.so",
                0o755,
                size,
                &mut library as &mut dyn Read,
            ),
            (
                "left4dead2/addons/inventory_mod/inventory_mod.example.toml",
                0o644,
                config.len() as u64,
                &mut config.as_slice() as &mut dyn Read,
            ),
            (
                "DEPENDENCIES-LICENSES.txt",
                0o644,
                licenses.len() as u64,
                &mut licenses.as_slice() as &mut dyn Read,
            ),
        ] {
            let mut header = tar::Header::new_gnu();
            header.set_mode(mode);
            header.set_size(size);
            header.set_entry_type(tar::EntryType::Regular);
            tar.append_data(&mut header, path, content)?;
        }
        tar.into_inner()?.finish()?.sync_all()?;
        Ok(())
    })
}

fn write_zip(file: File, repo: &Path, release: &Path, game: Option<&Path>) -> Result<()> {
    let mut zip = ZipWriter::new(file);
    let compressed = SimpleFileOptions::default().compression_method(CompressionMethod::Deflated);
    let entries: [(&str, PathBuf); 5] = [
        (
            "inventory_mod.dll",
            release.join("inventory_mod_client.dll"),
        ),
        ("injector.exe", release.join("injector.exe")),
        ("HOW-TO.txt", repo.join("packaging/HOW-TO.txt")),
        (
            "inventory_mod.example.toml",
            repo.join("inventory_mod.example.toml"),
        ),
        (
            "inventory_mod_screamer.jpg",
            repo.join("assets/inventory_mod_screamer.jpg"),
        ),
    ];
    for (name, source) in entries {
        let mut source = File::open(&source).map_err(|error| {
            io::Error::new(error.kind(), format!("{}: {error}", source.display()))
        })?;
        zip.start_file(format!("inventory-mod/{name}"), compressed)?;
        io::copy(&mut source, &mut zip)?;
    }
    zip.start_file("inventory-mod/DEPENDENCIES-LICENSES.txt", compressed)?;
    zip.write_all(include_bytes!(
        "../../windows-client/DEPENDENCIES-LICENSES.txt"
    ))?;
    if let Some(game) = game {
        for (ammo, name) in [
            (true, "inventory_mod_rounds.vpk"),
            (false, "inventory_mod_menu.vpk"),
        ] {
            let (n, data) = addon(repo, game, ammo, None)?;
            zip.start_file(format!("inventory-mod/addons/{name}"), compressed)?;
            zip.write_all(&data)?;
            println!("{name}: {n} files");
        }
    }
    zip.finish()?.sync_all()?;
    Ok(())
}
