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
boxes, between the game's melee box and the item grid. Each keeps what is in it: a gun its magazine, a melee weapon
which one it is ("fireaxe", "katana" ...). The reserve is yours and shared, as in the game, by every gun of the same
ammo type (M16, AK-47, SCAR and SG552 share rifle rounds; the SMGs, the pump shotguns, the auto shotguns, the sniper
rifles each share theirs): storing a gun leaves it with you, a gun taken out uses it (the game does not hand out a
fresh reserve), and a gun picked up off the ground into a box adds the rounds it carried, up to the most you can
carry of that type. That maximum is the rules file's `[ammo]` table (`assault_rifle = 360`, `smg`, `shotgun`,
`auto_shotgun`, `hunting_rifle`, `sniper_rifle`, `grenade_launcher`, `m60`, `chainsaw`); the mod also sets the game's
own `ammo_<type>_max` to it, so ammo piles stop there too. A stored gun shows its magazine and the reserve it would use
(your reserve of its type, e.g. "50 / 290"). The store key stores the gun or melee weapon in your
hands, the pick-up key takes one off the ground, and taking one out (the wheel, double click in the window) swaps it
with the one in that game slot, which goes into the same box. The wheel shows the weapons, then the items, each
keeping the place it first had (a swap does not move them). E on a weapon puts it in your hands when that game slot is empty; when you already have one there, it goes into
a free weapon box with its rounds; with no free box (or too heavy for one) it is the game's own swap, if that is not
too heavy.

The weight is checked on every pickup and store. It can still end up over the limit without one (a teammate hands you
pills, or the rules file is changed to heavier weights): the bar then turns red and nothing more can be picked up or
stored until you drop something.

## The infection

With `[infection]` in the host's `inventory_mod.toml` (on in the example): a survivor knocked down by the infected,
grabbed by a smoker, hunter, jockey or charger, or vomited on by a boomer is infected and a clock starts (`seconds`,
120). Each new hit takes `hit_seconds` (20) off it; pain pills put it back to the start, they do not cure it
(`no_cure = false`: they do). When it runs out the survivor dies and a random
special infected (`specials`) rises where they stood, and that survivor cannot come back before the next chapter: their
body is removed (no defibrillator) and no rescue closet calls for them. There is no clock on the screen: the player
sees it. A shader over the game's picture (and the mod's own boxes, wheel and window) throbs with a heartbeat that speeds up as time runs out; the world sways,
the edges blur and split into red and blue, the colour drains to a sick yellow green, dark veins creep in from the
edges, the vignette closes in, then double vision and dimming near the end and a red beat in the last ten seconds.
(It needs Windows' `d3dcompiler_47.dll`; without it a plain green tint and vignette.) `bots = true` infects the
survivor bots too. With `cough = true` (the default) an infected survivor coughs now and then with their own voice
(the coughs they use in smoke), heard by everyone around: about every 26 s at first, every 4 s near the end. With
`insane = true` (the default), from `insane_at` (70) % of the clock gone they go insane: they laugh for no reason or
scream (their own death screams, and "get it off!" at a special that is not there), every 18 s or so at first and
every 5 s at the end, screaming more as it gets worse. The window's Debug tab has "Infect me" to try it and a preview slider for the effect alone. (In co-op the round is lost when every human player is dead, so playing alone, turning ends the round.)

The sound goes too: the game's own mix gets more muffled and warped (its pitch bends up and down), a heartbeat thumps
under it in time with the picture, the ears ring past a third of the way and the sound crushes near the end. When the
clock runs out a screamer jumps at the player: a face over the whole screen, lunging and shaking, with a scream. Each
player chooses for themselves in the Settings tab ("Infection") or under `[infection]` in `inventory_mod_settings.toml`:
`effect`, `sound`, `screamer` (true / false), `screamer_chance` (how likely the screamer is each time the clock runs
out: 0 never, 1 always, 0.3 about one time in three), and `screamer_image` (next to the DLL or a full path; PNG, JPEG, BMP;
`inventory_mod_screamer.jpg` by default, shipped with the mod) and `screamer_sound` (a 16-bit PCM .wav under the game's
`left4dead2/sound`, next to the DLL or a full path; the witch's scream by default). The Debug tab has "Screamer (test)".

## Rounds

The reserve rounds you carry weigh, by `[round_weight]` in the host's `inventory_mod.toml` (one round: rifle 0.01, so
360 weigh 3.6; SMG 0.006; shotgun shell 0.04; sniper and hunting rifle 0.02; grenade 0.15; 0 makes a type weightless).
The window's Items tab lists each type you carry with what it weighs, and drops it a magazine's worth at a time
("Drop 50") or all of it: the rounds land as an ammo pile that holds exactly those rounds, which anyone (not the bots)
takes back with E, as many as fit; the rest stays in the pile. With the addon `inventory_mod_rounds.vpk` in
`left4dead2/addons` each round type is its own ammo pack (the textures in `assets/textures`, built by
`python -I tools/make_rounds_addon.py --install`); without it, and for M60 rounds and chainsaw fuel, the pile is the
incendiary ammo pack tinted the type's colour. Every player needs the addon to see the packs. An ammo pile of the map, or a gun picked up, fills you
only up to the weight limit.

With `[ammo_piles]` in the host's `inventory_mod.toml` (on in the example) the map's ammo piles are not endless: a pile
holds `min_magazines` to `max_magazines` magazines (1 to 3) of a random ammo type of `kinds`, rolled the first time
someone uses it (it then takes that type's colour). Anyone takes as many as they can carry, the rest stays in the
pile, and an empty pile is gone. A bot takes only rounds for a gun it has.

With `[map_guns]` (on in the example) a gun nobody has held yet, lying in the map or from a weapon spawner, has only
`min_clip` to `max_clip` % of its magazine (10 to 60) when it is first picked up. A gun someone has held, or one
dropped from a box, keeps what it had. With `spawners_once` (on) a weapon spawner of the map, for guns or melee
weapons, gives one and is gone; item spawners (pills, kits) are the game's.

## Stamina

With `[stamina]` in the host's `inventory_mod.toml` (on in the example): running uses stamina (`run` a second), and so
do jumps (`jump`), melee swings (`swing`) and shoves (`shove`), out of 100. `regen_delay` seconds after the last use it
comes back (`regen` a second; half that while an exhausted survivor keeps running). A survivor who runs out is
exhausted until it is back to `recover`: they move at `slow` % of their speed (on the ground; jumps and falls are not
slowed), wait longer between swings and shoves (`swing_speed` %), and their melee hits do `damage` % (at 50, a common
infected takes two hits). While it is on, the game's own shove fatigue (each shove in a row waiting longer) is off:
stamina is the limit. `bots = true` gives the survivor bots stamina too.

Each player chooses how it shows, in the Settings tab ("Stamina") or under `[stamina]` in `inventory_mod_settings.toml`:
`bar` (a thin bar under the crosshair while it is not full; orange when low, red and pulsing when exhausted) and
`vignette` (the screen's edges darken and lose their colour from half down; exhausted, they close in and pulse with
each gasp). The Debug tab has a preview of both.

## Keys (all can be rebound in the window's Settings tab)

| Key | Action |
|---|---|
| `I` | open / close the inventory window |
| middle mouse (hold) | the radial wheel, as in No More Room in Hell: everything you carry around the middle of the screen (gun, melee, stored weapons, items, then your reserve rounds in their colours), each segment as large as its share of the weight, with the weight as a ring that turns from green to red as the load nears the limit. Point at one and let go (or left click) to take it in hand, right click drops it (rounds: a magazine's worth, or all of that type with the button held half a second), Escape closes. The view stops turning while it is open (and does not jump when it closes); the keyboard stays the game's, so you keep walking |
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
