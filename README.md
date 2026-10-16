# inventory-mod

An inventory system for Left 4 Dead 2, as a DLL loaded into `left4dead2.exe`. The HUD uses the
[imgui-nullcore](../imgui-nullcore) look, drawn over the game by hooking `IDirect3DDevice9::Present`. The inventory
talks to the game's own functions (found by reverse engineering, see [docs/game-notes.md](docs/game-notes.md)).

L4D2 is a **32-bit** process, so everything here is built for x86.

> Run the game with `-insecure`. A foreign DLL in a VAC-secured session can get the account banned. Use this in
> single player, listen servers and private servers only.

## What it does

- A **hotbar** (always on) and an **inventory window** (grid, tooltips, right-click menu, drag and drop).
- **Store**: takes the item or weapon in your hands into the inventory.
- **Pick up nearby**: takes the item or weapon lying in front of you into the inventory.
- **Use**: puts the selected item in your hands, if the player's slot for it is free.
- **Drop**: puts the selected item on the ground in front of you, like `ent_create` does.
- **Auto-store**: pressing +use (E) on a supported item while your slot for it is full puts it into the inventory
  instead of leaving it on the floor. With `auto_store = true` (`[play]` in `inventory_mod_settings.toml`) (or "Store items on use" in Settings)
  every such +use stores the item, even when you could carry it.
- Stacks, a slot count and an optional weight limit. The contents are saved in `inventory_mod.inventory`.
- **Rules file** `inventory_mod.toml` next to the DLL (start from [inventory_mod.example.toml](inventory_mod.example.toml)):
  the boxes per player, the carry limit (`max_weight`), each item's weight and stack size, and each gun's and melee
  weapon's weight. On a dedicated server
  it is the rules for every player: the server refuses what is too heavy ("Too heavy: 18.0 / 20.0, ..."), and the
  clients show its numbers. With a limit set, a bar under the grid shows the weight, red when nothing more fits. The
  weight counts everything you carry: what is stored, the throwable, kit, pills or carried item in your game slots,
  and your gun and melee weapon / pistol. Taking an item in hand or putting it back changes nothing; picking an item
  or a weapon up in the game (E) is refused when it would be too heavy (a weapon that replaces yours counts only the
  difference).
- **Beside the game's weapon panel** (`replace_native=0`, the default): the game's own panel on the right stays as it
  is for the gun and the melee / pistol (ammo, slot keys `1`, `2`); its three item boxes are hidden and the inventory
  is a grid under them, three boxes per row, drawn the same way. Boxes appear as they are needed (the ones in use and
  one free box; up to `slots`, 24 by default), so the weight limit is what limits. The throwable, kit and pills you carry live in their
  own inventory box (counted in its number), green while in your hands, so every item has one place. The wheel (and
  `[` / `]`) steps through everything: gun, melee / pistol, then the grid row by row, left to right. `3`..`9`, `0` take inventory boxes 1-8 in hand, and
  the drop key drops what is in your hands (gun or item), like the game's own.
- **Replaces the game's weapon panel** (`replace_native=1`): the native panel is hidden (hook on
  `CHudWeaponSelection::ShouldDraw`) and the mod's bottom hotbar takes its place, with its slot keys and wheel. `Q`
  (last weapon) still works to get back to the gun.
- The column or hotbar shows only while you play a living survivor (not in menus, as a spectator or as infected).
- Hotkeys are ignored while the console, the chat line or a menu has the keyboard.
- **Inventory view** (Settings, `view` under `[hud]` in `inventory_mod_settings.toml`): slots and wheel ("both", the default), slots only ("slots": the
  middle mouse button stays the game's), or wheel only ("wheel": no grid or weapon boxes, and the game's own throwable / kit /
  pills boxes stay hidden too, the wheel shows everything; the number keys and the mouse wheel are the game's again).
- A carried item (gas can, propane / oxygen tank, fireworks, gnome, cola) is put back into its box when you switch
  with the mod (wheel, keys, boxes) instead of being dropped, as the game would; with no room it is dropped as before.
  The game's own keys `1` / `2` still drop it.

Supported items: first aid kit, defibrillator, pain pills, adrenaline, pipe bomb, molotov, bile jar, the two ammo
upgrade packs, gas can, propane / oxygen tank, fireworks, gnome, cola bottles, and every gun and melee weapon (pistol,
magnum, melee weapons, chainsaw, SMGs, shotguns, rifles, sniper rifles, M60, grenade launcher).

**Weapons** go into weapon boxes of their own (up to `weapon_slots`, 8 by default; a box appears only while it holds a weapon), drawn like the game's gun and melee
boxes, between the game's melee box and the item grid. Each keeps what is in it: a gun its magazine and its reserve
(shown like the game's gun box, magazine over reserve; the reserve leaves the player with it, so nothing is gained or
lost), a melee weapon which one it is ("fireaxe", "katana" ...). The store key stores the gun or melee weapon in your
hands, the pick-up key takes one off the ground, and taking one out (the wheel, double click in the window) swaps it
with the one in that game slot, which goes into the same box. The wheel goes gun, melee, stored weapons, then the
items. E on a weapon puts it in your hands when that game slot is empty; when you already have one there, it goes into
a free weapon box with its rounds; with no free box (or too heavy for one) it is the game's own swap, if that is not
too heavy.

The weight is checked on every pickup and store. It can still end up over the limit without one (a teammate hands you
pills, or the rules file is changed to heavier weights): the bar then turns red and nothing more can be picked up or
stored until you drop something.

## Keys (all can be rebound in the window's Settings tab)

| Key | Action |
|---|---|
| `I` | open / close the inventory window |
| middle mouse (hold) | the radial wheel, as in No More Room in Hell: everything you carry around the middle of the screen (gun, melee, stored weapons, items) with the weight as a red / grey ring. Point at one and let go (or left click) to take it in hand, right click drops it, Escape closes. The view stops turning while it is open (and does not jump when it closes); the keyboard stays the game's, so you keep walking |
| `1`..`5` | the game's own slots (beside the game's panel) |
| `3`..`9`, `0` | beside the game's panel: take inventory box 1..8 in your hands (an item whose game slot is taken swaps with it; that one goes back into its own box). `1` / `2` stay the game's gun and melee keys |
| `1`..`9`, `0` | with `replace_native=1`: select hotbar entry 1..10: first the game's two gun slots (primary, secondary; always shown, empty or not), then the inventory boxes. A throwable, kit, pills or carried item in your hands is shown in its own inventory box (green bar under it), so items never change boxes. Like the game's own panel, selecting any of them takes it in your hands; an inventory item whose game slot is occupied swaps with what is there (that one goes back into its own box) |
| mouse wheel | previous / next entry through all of them: beside the game's panel gun, melee / pistol, then the grid row by row; with `replace_native=1` the hotbar's entries (the game's wheel switching is swallowed either way) |
| `[` / `]` | the same as the wheel |
| `\` | use the selected item |
| `G` | beside the game's panel: drop what is in your hands (stored items are dropped from the window). With `replace_native=1`: drop the selected entry, an inventory item or a weapon / item you carry in the game's own slots |
| `;` | store the item in your hands |
| `H` | pick up the item in front of you |
| `Ctrl+End` | unload the DLL |

## Build

```
build.bat Release            # configure + build everything into out\Release
build.bat Release inventory_mod
```

`build.bat` uses the Visual Studio 18 toolchain (vcvarsall x86, its bundled CMake, Ninja and vcpkg). Libraries come
from vcpkg in manifest mode ([vcpkg.json](vcpkg.json), triplet `x86-windows-static`, pinned by `builtin-baseline`):
MinHook and toml++; `VCPKG_ROOT` picks another vcpkg. ImGui 1.92.8 is fetched by CMake, since imgui-nullcore builds
against its sources; imgui-nullcore is taken from the sibling folder (`-DINVENTORY_NULLCORE_DIR=...` to change). A
build folder configured before vcpkg needs a fresh configure (delete `build\x86-Release\CMakeCache.txt`).

Outputs in `out\Release`: `inventory_mod.dll`, `injector.exe`, `inventory_tests.exe` (the inventory model's tests).

## Run

1. Start the game with `-insecure` (add `-windowed -w 1280 -h 720` for development) and load a map.
2. `out\Release\injector.exe` loads `inventory_mod.dll` into the game (or use your own injector).
3. `inventory_mod.log`, `inventory_mod_settings.toml` (settings and keys; an old `inventory_mod.ini` is moved into it once) and `inventory_mod.inventory` are written next to the DLL; `inventory_mod.toml` (the rules) is only read.

The game access only turns on for game build 2.2.4.3 (`server.dll` timestamp `0x6A440E53`). On any other build the log
says why and the inventory only edits its own list.

## Layout

```
src/inv/       the model, no Windows or ImGui (tested on its own); rules.cpp reads inventory_mod.toml
src/core/      log, config, MinHook wrapper
src/ui/        overlay.cpp (D3D9 hooks, WndProc, cursor), hud.cpp (hotbar + window)
src/game/      backend.hpp (the seam), game.cpp (the real backend), mem.cpp (guarded reads and calls)
tests/         inventory_test.cpp
tools/         injector.cpp, game_test.ps1 (drives a test session), re/pe_tools.py (static analysis),
               mdmp_summary.py (reads a crash dump)
docs/          game-notes.md: every game address / slot / offset used, and how each was found
```

`tools/game_test.ps1` launches the game, injects, posts keys and clicks to the game window and captures only that
window to `%TEMP%`, so the real mouse and keyboard are left alone. `Reload-Mod` unloads the DLL (a posted message),
rebuilds it and loads it again without restarting the game.

## How it works

- The overlay hooks `Present` / `Reset` through the vtable of a throwaway device, draws ImGui between a captured and
  re-applied state block, and turns sRGB writes off (the game leaves them on, which washes out the colours).
- While the window is open the game gets none of the mouse or keyboard messages, `SetCursorPos` is ignored for the game
  and `GetCursorPos` returns the window's centre to it, so the view does not turn and the cursor is free.
- Game calls (`GiveNamedItem`, `UTIL_Remove`, spawning) are not thread safe and rendering is on another thread, so the
  UI only queues requests; they run in the window procedure, which is the game's main thread.

## Not done

- Only the +use pickup is intercepted (not items given by the game, e.g. at a map start or from a bot).
- No icons yet: slots show a coloured two-letter mark. Icons from the game's materials would need a VTF reader.
- Multiplayer: the inventory is the local player's only, and nothing is synced to other players.
- Game events (`item_pickup`, ...) are mapped in the notes but not used (they fire after a pickup, too late).
