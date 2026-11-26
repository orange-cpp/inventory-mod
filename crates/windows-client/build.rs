use std::{
    env, fs,
    hash::{DefaultHasher, Hash, Hasher},
    path::PathBuf,
    process::Command,
};

fn main() {
    println!("cargo:rerun-if-changed=callbacks/resident.rs");
    println!("cargo:rerun-if-changed=callbacks/api.rs");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows")
        || env::var("CARGO_CFG_TARGET_ARCH").as_deref() != Ok("x86")
    {
        return;
    }
    let output = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("inventory_callbacks.dll");
    let mut rustc = Command::new(env::var_os("RUSTC").unwrap());
    rustc
        .args([
            "--edition=2024",
            "--crate-type=cdylib",
            "--crate-name=inventory_callbacks",
            "-Copt-level=s",
            "-Cpanic=abort",
            "--target",
        ])
        .arg(env::var_os("TARGET").unwrap());
    if let Some(linker) = env::var_os("RUSTC_LINKER") {
        rustc
            .arg("-C")
            .arg(format!("linker={}", linker.to_string_lossy()));
    }
    let status = rustc
        .arg("callbacks/resident.rs")
        .arg("-o")
        .arg(&output)
        .status()
        .expect("failed to start rustc for the x86 Source callback library");
    assert!(
        status.success(),
        "could not build the x86 Source callback library"
    );
    // Only a cache key. The loader checks every byte before loading this file.
    let mut key = DefaultHasher::new();
    fs::read(output).unwrap().hash(&mut key);
    println!(
        "cargo:rustc-env=INVENTORY_CALLBACK_IMAGE={:016x}",
        key.finish()
    );
}
