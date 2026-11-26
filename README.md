# Inventory Mod for Left 4 Dead 2

Inventory Mod adds a persistent survivor inventory, a radial equipment wheel, and optional survival rules. The project is a Rust workspace with a Windows client and listen server, dedicated server plugins for Windows and Linux, a Windows launcher, and asset and release tools.

Run Left 4 Dead 2 and its dedicated servers with `-insecure`. Use the mod in single player or on private servers.

## Play on Windows

1. Extract the Windows release ZIP. Keep the DLL, launcher and `addons` directory together.
2. Add `-insecure` to the game's Steam launch options and start the game.
3. Run `injector.exe` from the extracted directory. To start the game and load the DLL in one step, run `injector.exe start-client`. An optional map name starts a listen game.
4. Start a map or join a server running the mod. Press `I` to open your inventory and `Ctrl+End` to unload the mod.

The mod writes `inventory_mod.log`, `inventory_mod_settings.toml`, and your local `inventory_mod.inventory` beside the DLL. It imports settings from the old `inventory_mod.ini` on first launch. Full releases include a VPK for the game's **Options > Inventory Mod** page. You can also edit the settings file while the game is closed.

The Windows native bindings check the Source binaries before using private game interfaces. They include the original 2.2.4.3 Windows build and the newer Windows client available during the port. Check the log if a Steam update changes a binding.

## Controls

| Default | Action |
| --- | --- |
| `I` | Open or close the inventory |
| Middle mouse | Hold to open the equipment wheel. Release or click to equip. Right click to drop. |
| `;` | Store the item or weapon in your hands |
| `H` | Store the item or weapon you are looking at |
| `E` | Use the game's pickup. The inventory can store items when your native slot is occupied. |
| `U` | Unload rounds from a supported gun |
| `V` | Mark a location, item or enemy for the team when pings are enabled |
| `G` | Drop the held item, or the selected item while the inventory is open |
| `3`–`9`, `0` | Equip inventory slots 1–8 when slot controls are enabled |
| Mouse wheel, `[`, `]` | Cycle inventory selection |
| `\` | Equip the selected item |
| `R` | Reload, or refuel a chainsaw when the host enables fuel |
| `Ctrl+End` | Unload the DLL |

Click an inventory box to select it, double-click to equip it, or drag it to another box. The right-click menu equips the item or drops one or all from the stack. The Items tab shows stored weapons, the carry limit, and buttons to drop reserve rounds by the magazine or by type. In the wheel, hold the right button for half a second to drop the entire reserve of the pointed-at type.

The inventory opens to the four-column grid with the original item and weapon icons from the installed game. The compact HUD sits beneath the native weapon panel by default. The wheel and HUD draw through the game's VGUI surface, with a Direct3D fallback when VGUI is unavailable. The infection and stamina settings control the original Direct3D effects, including whether the effects cover the game's HUD and the inventory window.

## Configure a game

Copy [inventory_mod.example.toml](inventory_mod.example.toml) to `inventory_mod.toml` next to the Windows DLL, or to the Linux server's `left4dead2/addons/inventory_mod/` directory. The example documents each option and includes sample values. The host's rules apply to the session. Each client's settings control its keys, HUD, infection audio, and screamer.

- **Inventory and ammo.** Configure item slots, weapon slots, stack sizes, item weights, and overall carry weight. Weapons keep their magazine, upgrades, melee type, and durability in saved inventories. Reserve ammo belongs to the survivor and is shared by weapons that use the same ammo type.
- **World pickups.** `[ammo_piles]` makes ammo piles finite and can add packs of separate ammo types. `[map_guns]` changes the amount of ammo in newly found weapons and can make map weapon spawners single-use. The server can store a pickup when a matching native slot is occupied. To store stackable items on use even with a free slot, set `auto_store = true` under `[play]` in the host's settings file.
- **Team pings.** `[ping]` controls range, lifetime, cooldown and the number of marks each player can keep.
- **Infection.** `[infection]` enables a countdown after designated infected attacks. Further damage advances it. Pills reset it by default, and expiry turns the survivor. Infection audio and the screamer can be toggled in client settings. Use `inventory_mod_screamer.jpg` or set a PNG, JPEG, or BMP in `screamer_image`.
- **Stamina and combat.** `[stamina]` governs running, jumping, swinging and shoving. `[melee]` configures hold-and-release heavy swings, blocking, throwing, impacts, and durability. `[chainsaw]` enables refuelling from stored gas cans. `[self_revive]` allows a designated consumable to revive an incapacitated survivor.
- **Chapter mutators.** `[mutators]` controls the chance, pool and per-map overrides. Fog, Scarcity, Heavy Load, Contagion, Brittle, Horde Tide, Hunted, Crowd, Witch and Tank adjust the chapter's presentation or gameplay.

The inventory is authoritative on the host. A listen server uses its local save and keeps separate inventories for guests. Dedicated servers keep individual saves for Steam IDs. After joining, mod clients send inventory commands and receive state through the existing multiplayer protocol.

## Dedicated servers

### Windows

Run `injector.exe start-server [map] [port]` to install the plugin from the release folder and launch the locally installed dedicated server in LAN mode. Use `L4D2_SERVER_DIR` for another server installation. You can also place `inventory_mod.dll` under `left4dead2/addons/inventory_mod/` and start the server directly:

```text
srcds.exe -console -game left4dead2 -insecure +plugin_load addons/inventory_mod/inventory_mod +map c1m1_hotel
```

Put the rules and optional `addons` folder next to the server DLL.

### Linux

Extract the Linux tarball into the dedicated server root and start the server:

```sh
./srcds_run -game left4dead2 -insecure +plugin_load addons/inventory_mod_server +map c1m1_hotel
```

The plugin checks `server_srv.so` against the supported **2.2.4.3** build. Rules and player saves live under `left4dead2/addons/inventory_mod/`. The Linux plugin uses built-in models for its ammo packs.

## Build and package

Checkout with Git LFS so the source textures and screamer are available. Run `cargo test --locked --workspace` for the portable workspace tests.

**Windows.** Use Rust with the MSVC toolchain and the Visual Studio x86 linker and Windows SDK.

```powershell
rustup target add i686-pc-windows-msvc
cargo test --locked --workspace --target i686-pc-windows-msvc
cargo build --locked --release -p inventory-mod-client --target i686-pc-windows-msvc
cargo run --locked --release -p inventory-tools -- package-windows
```

Cargo writes `inventory_mod_client.dll` and `injector.exe` to `target/i686-pc-windows-msvc/release/`. The ZIP renames the DLL to `inventory_mod.dll`. The launcher recognizes either filename. Use `--game <directory>` for game assets outside Steam's default installation or `--skip-addons` to package the binaries without them. The `menu-addon` and `rounds-addon` commands also generate the VPKs individually.

**Linux.** Install a 32-bit linker and system libraries (`gcc-multilib` and `libc6-dev-i386` on Debian or Ubuntu).

```sh
rustup target add i686-unknown-linux-gnu
cargo test --locked -p inventory-mod -p inventory-mod-server --target i686-unknown-linux-gnu
cargo build --locked --release -p inventory-mod-server --target i686-unknown-linux-gnu
cargo run --locked --release -p inventory-tools -- package-linux
```

The packages are `dist/inventory-mod-windows-x86.zip` and `dist/inventory-mod-linux-x86.tar.gz`. CI builds both after changes to the workspace. The release workflow generates Windows add-ons from the SteamCMD dedicated-server assets and publishes both archives when a version tag is pushed.

## Source layout

| Path | Responsibility |
| --- | --- |
| `src/` | Portable inventory, item catalog, saves, rules, mutators, melee rules and multiplayer protocol |
| `crates/windows-client/` | Windows client, VGUI and input, Source listen/dedicated-server integration, and launcher |
| `crates/linux-server/` | Source Linux dedicated-server plugin |
| `crates/tools/` | Asset generation, release packaging, and game binary and asset inspection |
| `tests/` | Save, settings, rules, item data and protocol compatibility tests |

Use `injector.exe --help` for development commands to drive a local test game. See [game-notes.md](docs/game-notes.md) for verified engine details and historical reverse-engineering notes.
