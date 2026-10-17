# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

An inventory mod for Left 4 Dead 2 (game build 2.2.4.3 only), built as a 32-bit DLL injected into `left4dead2.exe` or
`srcds.exe`. It hooks D3D9 for its HUD and calls the game's own functions, found by reverse engineering. The game must
run with `-insecure`. README.md describes the features; docs/game-notes.md is the reference for every address.

## Commands

```
build.bat Release                    # configure + build everything into out\Release (x86, VS 18 toolchain, vcpkg, Ninja)
build.bat Release inventory_mod      # just the DLL
build.bat Release inventory_tests    # just the tests
out\Release\inventory_tests.exe      # the model's tests: one executable, a CHECK macro, prints "all inventory tests passed"
make_dist.bat [-SkipBuild]           # build + test + addon + dist\inventory-mod and dist\inventory-mod-<date>.zip
python -I tools/make_rounds_addon.py # the ammo packs' addon -> out\Release\addons\inventory_mod_rounds.vpk (needs Pillow + the game)
```

The tests can't be run one at a time; add new `CHECK`s to the existing blocks in `tests/inventory_test.cpp`. A
`CHECK((a == b{...}))` with braces needs the extra parentheses.

While the game has the DLL loaded, the link step fails with LNK1104. Unload the DLL first (`Unload-Mod`, or Ctrl+End in
the game), or use `Reload-Mod`.

### Driving the game: tools/game_test.ps1

Dot-source it in PowerShell (`. .\tools\game_test.ps1`):
- `Start-Game [-Connect]` / `Stop-Game` start and stop the game.
- `Inject-Mod` injects the DLL; `Reload-Mod` unloads it, rebuilds `inventory_mod` and injects it again, without
  restarting the game.
- `Mod-Log` reads `out\Release\inventory_mod.log`, which starts over on every load.
- `Shot <name>` captures the game window only.
- `Press-Key`, `Console`, `Click-At` drive the game.
- `Start-Server` / `Inject-Server` / `Reload-Server` / `Server-Log` do the same for a dedicated server. It uses its own
  copy in `out\Server`, with its own `inventory_mod.toml` rules and its own `addons\` copy.

Before testing in game, check whether the user is playing a local game or is on the dedicated server. While they play,
ask them to do the clicks instead of driving their input.

Crash dumps go to the game folder (`left4dead2_crash_*.mdmp`); `tools/mdmp_summary.py` reads them.

## Architecture

**Layers.**
- `src/inv` is the model: items, inventory, the TOML rules. It has no Windows or ImGui code and is tested alone.
- `src/game/backend.hpp` is the seam between the UI and the game.
- `src/game/game.cpp` is the real backend: every hook and game call, in one large file organised in `// ---- topic`
  sections.
- `src/ui` is the client side: `overlay.cpp` (D3D9 Present/Reset hooks, window procedure, cursor), `hud.cpp` (boxes,
  radial wheel, window, Settings and Debug tabs), `postfx.cpp` (infection and stamina shaders), `audio.cpp`,
  `icons.cpp` (the game's VTF icon sheets).
- `src/core` has the log, the settings (`inventory_mod_settings.toml`, per player) and the MinHook wrapper.

**Threads.** This is the main source of bugs here.
- Game calls (`GiveNamedItem`, `UTIL_Remove`, entity creation, `SetKeyValue`) must run on the game's main thread.
- The HUD draws inside Present, on the render thread, so the UI only queues requests.
- `backend.pump()` runs them from the game's window procedure, on `game::k_msg_pump`.
- Server-side logic (stamina, infection, reserve caps) runs in the hooked `GameFrame`.
- Render-thread work started from main-thread code must go through the material system's call queue. See
  "The infection's effect under the HUD" in game-notes.

**Server and client.**
- The inventory lives on the server. On a listen server the host is "the local player".
- `Session` holds one player's inventory and state. `session()` is the active one; `local_session()` is the HUD's.
- When the server serves another player, an `Activation` makes that player local and their session active. Remote
  clients that run the mod send `inv_*` console commands, and receive their state as `@...@` lines through
  `ClientPrintf`. Those lines are filtered out in the client's `ProcessPrint` hook.
- `served_remotely()` and `Activation` wrap every hook that a remote player can trigger.

**Rules and settings.**
- `inventory_mod.toml` holds the host's rules: `[limits]`, `[weight]`, `[ammo]`, `[round_weight]`, `[infection]`,
  `[stamina]`, `[ammo_piles]`, `[map_guns]`. It is parsed in `src/inv/rules.cpp`; a table switches its feature on.
- `inventory_mod.example.toml` is the documented template.
- `inventory_mod_settings.toml` holds each player's presentation settings (`src/core/config.cpp`).

**Addons.** The mod mounts every `.vpk` in the `addons` folder next to the DLL (filesystem AddVPKFile, in
`mount_addons`). `inventory_mod_rounds.vpk` holds the round types' ammo packs. `inventory_mod_menu.vpk` (`tools/make_menu_addon.py`) holds
the Options > Inventory Mod button and page (GameUI's Xbox-only AudioVideo page, taken over in `overlay.cpp`; its
`SETTINGS` must match `k_page_settings` there and `apply_queued_settings` in `hud.cpp`). Addons are mounted at the head,
so their files override the game's. It is generated from
`assets/textures/*.png` plus the game's own model by `tools/make_rounds_addon.py`, and is never committed (Valve
content).

## Reverse-engineering rules

- Verify every vtable slot, address and struct offset by disassembly before using it. Guard each one at runtime with
  `mem::find_pattern` on the function's prologue, and fall back with a log line when it doesn't match.
- Tools: `tools/re/pe_tools.py <dll> strings|xrefs|rtti|vtable|disasm|func`, `tools/re/vpk.py`,
  `tools/re/mdl_to_smd.py`, `tools/re/vtf_to_png.py`. Addresses are at the DLL's preferred base (0x10000000); code uses
  RVAs.
- Record every new address, slot or offset, and how it was found, in docs/game-notes.md. Note when it was seen working
  in game.
- `IServerTools::SetKeyValue(const char*)` is slot 13; slot 11 is the `Vector` overload, because MSVC orders overloads
  in reverse. `GetKeyValue` of a string field returns pointer bytes, so read `m_iName` (+0x140) and `m_ModelName`
  (+0x24C) directly.
- An entity created with `CreateEntityByName` gets only the keyvalues the mod sets. Copy what a real map entity has:
  read it from a BSP entity lump (BSP v21's `lump_t` is `{version, offset, length, fourCC}`). Set `"origin"` before
  `DispatchSpawn` and never move a spawner afterwards: its physics object is static. A moved one (Teleport,
  DropToFloor) crashed vphysics.dll at +0xF3B1C a few seconds later. To find the floor, drop a throwaway `info_target`
  first (`on_floor`).
- After changing an entity's networked field, mark the edict changed (`state_changed`).

## Conventions

- Comments are full explanatory prose: why, and which addresses. Match the surrounding density.
- README.md (features), docs/game-notes.md (addresses) and packaging/HOW-TO.txt (players' instructions, shipped in the
  release) are kept up to date with features.
- Git LFS covers `*.png`, `*.jpg`, `*.vtf`, `*.vpk` and the model files (`.gitattributes`).
- `out/`, `build/` and `dist/` are not tracked. imgui-nullcore is a sibling checkout (`../imgui-nullcore`) and is
  built from source.
- Commit and push only when asked.
