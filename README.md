# inventory-mod

An inventory system for Left 4 Dead 2, as a DLL loaded into `left4dead2.exe`. The HUD uses the
[imgui-nullcore](../imgui-nullcore) look, drawn over the game by hooking `IDirect3DDevice9::Present`. The inventory
talks to the game's own functions (found by reverse engineering, see [docs/game-notes.md](docs/game-notes.md)).

L4D2 is a **32-bit** process, so everything here is built for x86.

> Run the game with `-insecure`. A foreign DLL in a VAC-secured session can get the account banned. Use this in
> single player, listen servers and private servers only.

## What it does

- A **hotbar** (always on) and an **inventory window** (grid, tooltips, right-click menu, drag and drop).
- **Store**: takes the item in your hands (or, holding a gun, the first utility item you carry) into the inventory.
- **Pick up nearby**: takes the item lying in front of you into the inventory.
- **Use**: puts the selected item in your hands, if the player's slot for it is free.
- **Drop**: puts the selected item on the ground in front of you, like `ent_create` does.
- **Auto-store**: pressing +use (E) on a supported item while your slot for it is full puts it into the inventory
  instead of leaving it on the floor. With `auto_store=1` in `inventory_mod.ini` (or "Store items on use" in Settings)
  every such +use stores the item, even when you could carry it.
- Stacks, a slot count and an optional weight limit. The contents are saved in `inventory_mod.inventory`.
- **Replaces the game's weapon panel** (`replace_native=1`, on by default): the native panel on the right is hidden
  (hook on `CHudWeaponSelection::ShouldDraw`), and its slot keys and wheel are swallowed. `Q` (last weapon) still works
  to get back to the gun.
- Hotkeys are ignored while the console, the chat line or a menu has the keyboard.

Supported items: first aid kit, defibrillator, pain pills, adrenaline, pipe bomb, molotov, bile jar, the two ammo
upgrade packs, gas can, propane / oxygen tank, fireworks, gnome, cola bottles.

## Keys (all can be rebound in the window's Settings tab)

| Key | Action |
|---|---|
| `I` | open / close the inventory window |
| `1`..`9`, `0` | select hotbar entry 1..10: first the game's two gun slots (primary, secondary; always shown, empty or not), then the inventory boxes. A throwable, kit, pills or carried item in your hands is shown in its own inventory box (green bar under it), so items never change boxes. Like the game's own panel, selecting any of them takes it in your hands; an inventory item whose game slot is occupied swaps with what is there (that one goes back into its own box) |
| mouse wheel | previous / next entry through all of them (the game's wheel switching is swallowed) |
| `[` / `]` | previous / next item on the hotbar |
| `\` | use the selected item |
| `G` | drop the selected entry: an inventory item or a weapon / item you carry in the game's own slots |
| `;` | store the item in your hands |
| `H` | pick up the item in front of you |
| `Ctrl+End` | unload the DLL |

## Build

```
build.bat Release            # configure + build everything into out\Release
build.bat Release inventory_mod
```

`build.bat` uses the Visual Studio 18 toolchain (vcvarsall x86, its bundled CMake and Ninja). ImGui 1.92.8 and MinHook
1.3.4 are fetched by CMake; imgui-nullcore is taken from the sibling folder (`-DINVENTORY_NULLCORE_DIR=...` to change).

Outputs in `out\Release`: `inventory_mod.dll`, `injector.exe`, `inventory_tests.exe` (the inventory model's tests).

## Run

1. Start the game with `-insecure` (add `-windowed -w 1280 -h 720` for development) and load a map.
2. `out\Release\injector.exe` loads `inventory_mod.dll` into the game (or use your own injector).
3. `inventory_mod.log`, `inventory_mod.ini` and `inventory_mod.inventory` are written next to the DLL.

The game access only turns on for game build 2.2.4.3 (`server.dll` timestamp `0x6A440E53`). On any other build the log
says why and the inventory only edits its own list.

## Layout

```
src/inv/       the model, no Windows or ImGui (tested on its own)
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
