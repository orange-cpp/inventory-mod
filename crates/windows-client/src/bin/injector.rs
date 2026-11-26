#[cfg(all(windows, target_arch = "x86"))]
#[path = "injector/launcher.rs"]
mod launcher;

#[cfg(all(windows, target_arch = "x86"))]
fn main() -> std::process::ExitCode {
    let result = match launcher::run() {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{error}");
            std::process::ExitCode::FAILURE
        }
    };
    launcher::wait_if_launched_from_explorer();
    result
}

#[cfg(not(all(windows, target_arch = "x86")))]
fn main() -> std::process::ExitCode {
    eprintln!("Build the Left 4 Dead 2 launcher for i686-pc-windows-msvc.");
    std::process::ExitCode::FAILURE
}
