use std::{
    env,
    error::Error,
    ffi::OsString,
    fs, io,
    path::{Path, PathBuf},
    process::ExitCode,
};

use inventory_source_assets::{vpk, vtf};

mod binary;
mod menu;
mod minidump;
mod model;
mod pe;
mod release;
mod rounds;

type Result<T> = std::result::Result<T, Box<dyn Error>>;

const USAGE: &str = "Usage: inventory-tools <command> [options]\n\
    menu-addon [--game <directory>] [--out <file.vpk>]\n\
    rounds-addon [--game <directory>] [--textures <directory>] [--out <file.vpk>] [--install]\n\
    package-windows [--game <directory>] [--binaries <directory>] [--out <release.zip>] [--skip-addons]\n\
    package-linux [--plugin <file.so>] [--out <release.tar.gz>]\n\
    vpk <file.vpk> list [substring]\n\
    vpk <file.vpk> get <path inside> <output file>\n\
    vtf <texture.vtf> <output.png>\n\
    mdmp <crash.mdmp>\n\
    model <model.mdl> <output directory>\n\
    pe <image.dll> <strings|xrefs|rtti|vtable|disasm|func> [arguments]";

fn bad_args() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, USAGE)
}

fn main() -> ExitCode {
    match run(env::args_os().skip(1).collect()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{error}");
            ExitCode::FAILURE
        }
    }
}

fn run(args: Vec<OsString>) -> Result<()> {
    let (command, args) = args.split_first().ok_or_else(bad_args)?;
    match command.to_str() {
        Some("menu-addon") => build_addon(args, false),
        Some("mdmp") => {
            let [file] = args else {
                return Err(bad_args().into());
            };
            Ok(minidump::summarize(&fs::read(file)?)?)
        }
        Some("model") => {
            let [file, output] = args else {
                return Err(bad_args().into());
            };
            Ok(model::export(Path::new(file), Path::new(output))?)
        }
        Some("pe") => {
            let (file, args) = args.split_first().ok_or_else(bad_args)?;
            Ok(pe::inspect(&fs::read(file)?, args)?)
        }
        Some("rounds-addon") => build_addon(args, true),
        Some("package-windows") => package_windows(args),
        Some("package-linux") => package_linux(args),
        Some("vpk") => {
            let (archive, args) = args.split_first().ok_or_else(bad_args)?;
            let archive = vpk::Vpk::open(Path::new(archive))?;
            match args {
                [verb, rest @ ..] if verb == "list" && rest.len() <= 1 => {
                    let needle = rest
                        .first()
                        .map(|value| value.to_string_lossy().to_ascii_lowercase())
                        .unwrap_or_default();
                    for name in archive.names().filter(|name| name.contains(&needle)) {
                        println!("{name}");
                    }
                }
                [verb, name, output] if verb == "get" => {
                    write(Path::new(output), archive.read(&name.to_string_lossy())?)?;
                }
                _ => return Err(bad_args().into()),
            }
            Ok(())
        }
        Some("vtf") => {
            let [input, output] = args else {
                return Err(bad_args().into());
            };
            let image = vtf::decode(&fs::read(input)?)?;
            image.save(Path::new(output))?;
            println!(
                "{}x{} -> {}",
                image.width(),
                image.height(),
                Path::new(output).display()
            );
            Ok(())
        }
        Some("--help" | "-h") => {
            println!("{USAGE}");
            Ok(())
        }
        _ => Err(bad_args().into()),
    }
}

fn write(path: &Path, bytes: Vec<u8>) -> io::Result<()> {
    if let Some(dir) = path.parent().filter(|dir| !dir.as_os_str().is_empty()) {
        fs::create_dir_all(dir)?;
    }
    fs::write(path, bytes)
}

fn build_addon(args: &[OsString], rounds: bool) -> Result<()> {
    let repo = repository();
    let mut game = default_game();
    let addon = if rounds {
        "inventory_mod_rounds.vpk"
    } else {
        "inventory_mod_menu.vpk"
    };
    let mut output = repo
        .join("target/i686-pc-windows-msvc/release/addons")
        .join(addon);
    let mut textures = None;
    let mut install = false;
    let mut args = args.iter();
    while let Some(arg) = args.next() {
        match arg.to_str() {
            Some("--game") => game = args.next().ok_or_else(bad_args)?.into(),
            Some("--out") => output = args.next().ok_or_else(bad_args)?.into(),
            Some("--textures") if rounds => {
                textures = Some(PathBuf::from(args.next().ok_or_else(bad_args)?))
            }
            Some("--install") if rounds => install = true,
            _ => return Err(bad_args().into()),
        }
    }
    let (files, bytes) = release::addon(&repo, &game, rounds, textures.as_deref())?;
    let size = bytes.len();
    write(&output, bytes)?;
    println!("{}: {} files, {} KB", output.display(), files, size / 1024);
    if install {
        let filename = output.file_name().ok_or_else(bad_args)?;
        let target = game.join("left4dead2/addons").join(filename);
        fs::create_dir_all(target.parent().unwrap())?;
        fs::copy(&output, &target)?;
        println!("installed {}", target.display());
    }
    Ok(())
}

fn repository() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn default_game() -> PathBuf {
    env::var_os("L4D2_GAME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            PathBuf::from(r"C:\Program Files (x86)\Steam\steamapps\common\Left 4 Dead 2")
        })
}

fn package_windows(args: &[OsString]) -> Result<()> {
    let repo = repository();
    let mut output = repo.join("dist/inventory-mod-windows-x86.zip");
    let mut game = default_game();
    let mut binaries = repo.join("target/i686-pc-windows-msvc/release");
    let mut include_addons = true;
    let mut args = args.iter();
    while let Some(arg) = args.next() {
        match arg.to_str() {
            Some("--game") => game = args.next().ok_or_else(bad_args)?.into(),
            Some("--binaries") => binaries = args.next().ok_or_else(bad_args)?.into(),
            Some("--out") => output = args.next().ok_or_else(bad_args)?.into(),
            Some("--skip-addons") => include_addons = false,
            _ => return Err(bad_args().into()),
        }
    }
    release::windows(
        &repo,
        &binaries,
        &output,
        include_addons.then_some(game.as_path()),
    )?;
    println!("Release archive written to {}", output.display());
    Ok(())
}

fn package_linux(args: &[OsString]) -> Result<()> {
    let repo = repository();
    let mut output = repo.join("dist/inventory-mod-linux-x86.tar.gz");
    let mut plugin = repo.join("target/i686-unknown-linux-gnu/release/libinventory_mod_server.so");
    let mut args = args.iter();
    while let Some(arg) = args.next() {
        match arg.to_str() {
            Some("--plugin") => plugin = args.next().ok_or_else(bad_args)?.into(),
            Some("--out") => output = args.next().ok_or_else(bad_args)?.into(),
            _ => return Err(bad_args().into()),
        }
    }
    release::linux(&repo, &plugin, &output)?;
    println!("Release archive written to {}", output.display());
    Ok(())
}
