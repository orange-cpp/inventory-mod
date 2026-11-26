# Working on Inventory Mod

This is a Rust 2024 Cargo workspace. The shared root crate handles inventory, item data, saves, rules and the multiplayer protocol. Platform crates isolate Source engine and operating-system calls so the shared model can run under ordinary Rust tests.

## Build and test

```text
cargo fmt --all --check
cargo test --locked --workspace
cargo clippy --locked --workspace --all-targets -- -D warnings
cargo build --locked --release -p inventory-mod-client --target i686-pc-windows-msvc
cargo build --locked --release -p inventory-mod-server --target i686-unknown-linux-gnu
```

Install the target for the platform you are building. Windows uses the MSVC linker and Windows SDK. Linux needs the 32-bit system libraries. L4D2 runs as an x86 process on both platforms, so host-target tests alone do not compile the native plugin code. Run Clippy and tests on the corresponding x86 target too.

`inventory-tools` generates the asset VPKs and both release archives. `injector.exe --help` describes the Windows test-game commands. Start the game and test servers using `-insecure`. Unload the Windows DLL before rebuilding or replacing it if it is in use.

## Engine bindings

- Keep gameplay policy in shared code when both platforms use the same rule.
- Use the public Source interfaces where available. Check a verified build, function or vtable before using a private binding. Keep the ABI, offsets and calling conventions in platform modules. Record observations and actual runtime tests in [game-notes.md](docs/game-notes.md).
- Use `inventory-tools pe`, `mdmp`, `vpk`, `vtf`, and `model` for local inspection. PE tool addresses are from the preferred image base. Native bindings use RVAs to account for relocation.
- Keep Source entity and UI calls on the appropriate game thread. Restore hooks, native settings and other game state on unload.
- Keep shared save and wire formats compatible with existing clients and inventories. Use the compatibility tests in `tests/` to check format changes.
- Update [README.md](README.md), [the package guide](packaging/HOW-TO.txt) and the example rules when behavior or user-facing commands change. Git LFS tracks the original asset formats. Cargo output and locally generated releases are ignored.
