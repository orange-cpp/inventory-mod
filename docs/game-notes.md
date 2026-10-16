# L4D2 internals used by the mod

Everything here was read out of the game's DLLs with `tools/re/pe_tools.py` (capstone + pefile) and then checked in
the running game. It is for **one build**: game 2.2.4.3, `server.dll` PE timestamp `0x6A440E53`
(sha256 `a93ec916...`). `game::init()` refuses to run on any other `server.dll`, so a game update fails closed.
Addresses are RVAs from the preferred base `0x10000000`; the mod itself finds them at run time (see "how it is found").

## Tool

```
python -m pip install --target %TEMP%\relibs capstone pefile
python -I tools/re/pe_tools.py <dll> strings <text>      # ASCII strings
python -I tools/re/pe_tools.py <dll> xrefs <va|string:text>
python -I tools/re/pe_tools.py <dll> rtti <class>        # MSVC RTTI -> vtable addresses
python -I tools/re/pe_tools.py <dll> vtable <va> [n]
python -I tools/re/pe_tools.py <dll> func <va>           # disassembly of one function
```

Cross references come from the base relocation table (every absolute address in the image is relocated), so they are
exact. Class vtables come from MSVC RTTI (`.?AVCTerrorPlayer@@` -> type descriptor -> complete object locator -> vtable).

## engine.dll

| What | Where |
|---|---|
| `GAMEEVENTSMANAGER002` | `CGameEventManager`, vtable `0x10388774`, 13 slots |
| `IGameEventManager2` slots | 3 `AddListener(listener, name, serverSide)`, 5 `RemoveListener`, 6 `CreateEvent(name, force, cookie)` (`0x10181310`, `ret 0xC`, prologue `55 8B EC 83 EC 08 53 56 57 8B F9`), 7 `FireEvent(event, dontBroadcast)` (`ret 8`) |
| `IGameEvent` (`CGameEvent`) slots | 1 `GetName`, 5 `GetBool`, 6 `GetInt`, 8 `GetFloat`, 9 `GetString` |
| listener the engine calls | slot 1 `FireGameEvent(event)`; slot 2 `GetEventDebugID()` must return `42` or the engine skips the listener |

The mod hooks `CreateEvent` and returns null while it swaps items in the player's hands (`do_use`): the server code
checks for null and skips the event, so the client does not show item icons floating in the world for a pickup that
was not one. Every muted name is logged (`event <name> muted during a switch`).

That alone did not remove the floating icon: it belongs to the **client's** `"weapon_pickup"` event, which client.dll
creates (`0x102CE42B`, `push "weapon_pickup" / call CreateEvent / test esi,esi / je`: it checks for null) a frame or
two later, when the new weapon reaches the client. Found by logging every `CreateEvent` for 1.5 s after a switch
(`weapon_pickup` ~15-50 ms later, on the main thread). The hook therefore also returns null for `"weapon_pickup"` for
500 ms after a switch. Checked in the game: pills <-> adrenaline swaps show no icon.

## server.dll

| What | Where / how |
|---|---|
| `VSERVERTOOLS001` | `CServerTools`, vtable `0x106fb31c`. Slots: 7 `FirstEntity`, 8 `NextEntity`, 10 `GetKeyValue`, 11 `SetKeyValue(const char*)`, 14 `CreateEntityByName`, 15 `DispatchSpawn`, 16 `DestroyEntityByHammerId` |
| `ServerGameEnts001` | `CServerGameEnts`, vtable `0x1060cab4`: 3 `BaseEntityToEdict`, 4 `EdictToBaseEntity` |
| `gEntList` | `0x107e0760`. Entry *i* at `+4 + 16*i` = `{entity, serial, prev, next}`. EHANDLE = `index (12 bits) | serial << 12`. Found from `FirstEntity`'s body: `6A 00 B9 <gEntList> E8` |
| `UTIL_Remove(CBaseEntity*)` | `0x102071E0`, cdecl. Found from `DestroyEntityByHammerId`'s body: `50 E8 <rel32> 83 C4 04 B0 01` |
| `UTIL_DropToFloor(ent, 0x200400B, 0)` | `0x10208F60`, cdecl. Found from the end of the `ent_create` callback (`0x10057690`): `6A 00 68 0B 40 00 02 57 E8 <rel32> 83 C4 0C` |
| `CTerrorPlayer` | primary vtable `0x106973B4` (587 slots) |
| `CTerrorPlayer` slot 431 | `GiveNamedItem(name, 0, 0, 0)`: the `give` console command (`0x102FA910`) calls it exactly so; the function compares its argument with `"health"` first |
| `CTerrorPlayer` slot 429 | `SelectItem(const char* classname, int subtype)`: the `use <weapon>` console command (`0x1008ED30`) calls `vt[0x6B4/4](name, 0)`. The mod uses it to switch to a weapon the player carries; the hotbar's list of them is read from `m_hMyWeapons` on the main thread every 100 ms |
| `CTerrorPlayer` slots 283 / 401 | `Weapon_Drop(weapon, 0, 0)` / a "cannot drop now" check (non-zero: refuse). The VScript `DropItem` (`0x102FBCD0`) calls both; the mod does the same to drop a carried game weapon (the dropped item is a normal world entity again, so +use or the pickup key can take it back) |
| `CTerrorPlayer` slots 140 / 141 | `EyePosition(Vector* out)` / `EyeAngles()` (the `IServerTools::GetPlayerPosition` body calls them) |
| any `CBaseEntity` slot 26 / 118 | `Precache()` / `Teleport(&pos, 0, 0)`: what `ent_create` calls after `CreateEntityByName` |
| `CTerrorPlayer` slot 426 | `server.dll + 0x31D080`, the **+use pickup**: `bool __thiscall (CTerrorPlayer* this, CBaseEntity* weapon)`, `ret 4`, prologue `55 8B EC 83 EC 08 53 57 8B 7D 08`. Returns 1 after equipping, 0 when refused. The mod hooks it (MinHook) and, when the player's slot for a catalogue item is taken (or `auto_store=1`), stores the item and returns false. See "How +use picks a weapon up" |
| `CTerrorPlayer` slots 280 / 281 | `Weapon_CanUse(weapon)` (`0x10300600`) / `Weapon_Equip(weapon)` (`0x10306070`) |
| `m_hMyWeapons` | `+0x18F4`, 56 EHANDLEs (`SendPropArray3`: `push 0x38 / push 4 / push 0x18F4 / "m_hMyWeapons"`) |
| `m_hActiveWeapon` | `+0x19D4` |
| `m_vecOrigin` | `+0x388` (`SendPropVector`: `push 0x388 / push 0xC / "m_vecOrigin"`) |
| entity index 1 | the first player slot: the human on a single-player or listen-server game |

Facts that cost time:

- `UTIL_Remove` only marks the entity; it is deleted at the end of the frame. A carried weapon removed that way is still
  in `m_hMyWeapons` for the rest of the frame, so a `GiveNamedItem` for the same game slot right after makes the game
  drop it on the ground. The swap in `do_use` therefore takes the old item off with `Weapon_Drop` (slot 283, `ret 0xC`)
  first, which empties the slot at once, and only then removes the dropped entity and gives the new item. Verified
  in c1m1_hotel: bile jar <-> molotov swaps leave nothing on the floor.
- `CTerrorPlayer::Weapon_Drop` (slot 283, `0x10300770`) is the base `CBaseCombatCharacter::Weapon_Drop` (`0x10170AC0`,
  also `CBasePlayer` slot 283) plus a `"weapon_drop"` game event (`userid`, `item`, `propid`). The client answers that
  event with a floating "dropped item" icon in the world, so a swap (which removes the dropped entity at once) left
  icons hanging in the air. The swap calls the base function instead: `init()` reads it from the call at the start of
  slot 283 (`50 51 57 8B CB E8 <rel32>`). The mod's own "drop" of a carried weapon keeps the full one (a real drop).

- `UTIL_GetLocalPlayer()` (`0x10209180`) returns null when `maxClients > 1`, which is always the case in L4D2, so the
  mod takes entity 1 from the entity list instead.
- Setting the `"origin"` key on a freshly created item does nothing useful for dropping: `ent_create` itself does
  `Precache`, `DispatchSpawn`, `Teleport`, `UTIL_DropToFloor`, and so does the mod.
- The game's code is not thread safe, and rendering may run on its own thread. The mod's game calls run in the window
  procedure (the game's main thread) through a posted `WM_APP + 0x49`.

## How +use picks a weapon up (found for the auto-store)

Searching `item_pickup` / `weapon_given` in server.dll (`xrefs string:item_pickup`) gives six functions. Four are event
registrations (`0x102A3CF0` registers the listeners) and two are pickup code, in the **same** place of two different
vtables: `0x10246E80` (slot 15 of a class that is not `CTerrorPlayer`; it fires `item_pickup`) and `0x1031D080`
(`CTerrorPlayer` slot 426; it fires `weapon_given`). Reading the code is not enough to tell which one a +use calls, so
the answer came from the running game: the mod hooked `CTerrorPlayer` slots 280, 281 and 426 with logging detours, the
player pressed E (a `WM_KEYDOWN` posted with the right scan code, see below) on an item on the floor, and the log showed
`426 -> 280 -> 281 -> back`, all on the main thread (the one the window procedure runs on), `this` = entity 1. The
`0x10246E80` function was hooked first and never ran for a +use, so it is **not** used.

Slot 426's flow: arg null -> flags; else `this->vt[0x460/4 = 280](arg)` (CanUse) and a gamerules check
(`[0x107F7F6C]->vt[0x148/4](this, arg)`); on success it equips through slot 281 and fires `weapon_given`; on refusal it
jumps to `0x1031D5DA` and returns 0. With the slot for the item already taken, CanUse is what says no.

The hook (`hk_bump_weapon` in `src/game/game.cpp`) does, for the local player (entity 1) and a catalogue item only:
`full = player_has_slot(...)`; if `full || auto_store`, add one to the inventory, `UTIL_Remove` the world entity, notify
`Stored <item>` and return false (the game must not equip it as well); otherwise call the original. It runs on the
main thread, so the game calls need no queueing. `init()` installs it only when slot 426 of the player vtable still
starts with the prologue above; otherwise the log says so and the rest of the mod works without it.

Game events (`IGameEventManager2`) were not needed: `item_pickup` / `weapon_given` fire only after a successful
pickup, so they cannot see a refusal.

Testing hints (`tools/game_test.ps1`): the game reads keys by **scan code**, so a posted `WM_KEYDOWN` needs
`lParam = 1 | MapVirtualKey(vk) << 16` (`Press-Key` uses scan code 1 = Escape, fine for the mod's hotkeys, wrong for the
game). With that, E (hold ~400 ms) is a real +use and the console (`` ` ``) takes typed `WM_CHAR`s. Verified in c1m1_hotel:
dropping a pipe bomb with the slot free is picked up normally (`slot free, auto_store 0`); dropping a molotov while
holding the pipe bomb prints `+use on weapon_molotov: slot full` and `Stored Molotov`, the entity is gone and the HUD
slot shows it; with `auto_store=1` an incendiary pack with a free slot is stored too.

## client.dll

| What | Where / how |
|---|---|
| `CHudWeaponSelection` | the game's own weapon panel (the list on the right of the screen). RTTI `.?AVCHudWeaponSelection@@` gives three vtables; the `CHudElement` one is `0x105D5A84` |
| `ShouldDraw` | slot 10 of that vtable, `0x101DC2C0`, `bool __thiscall (this)`, prologue `56 57 6A FF 8B F9 E8`. Slots 4 / 5 (`0x101DBE80`, `0x101DBEE0`) are `LevelInit` / `Reset`-like, slot 3 is `SetActive`. The mod hooks it and returns false while `replace_native=1`, which hides the panel; `init()` checks the vtable slot and the prologue first |
| `Paint` | slot 128 of the panel's `vgui::Panel` vtable `0x105D5734`: `0x102D11E0`. Unrolled, one part per box: `0x102CEB20(slot)` (thiscall on the `CHudElement` part, `esi - 0x48`) gets the weapon in a slot, then `0x102CE5B0` / `0x102CE4C0` draw the box and `0x102CE980` its key. The gun is slot 0 (ends with `push 0; ...; call 0x102CE980`), the melee / pistol slot 1 (ends with `push 1; mov ecx, ebx; call 0x102CE980` at `0x102D1B2D`), then a part that calls `0x102CDDF0` and ends with key label 5 (`0x102D1C16`), then slots 2, 3, 4 (`push 2/3/4` before `0x102CEB20` at `0x102D1C48` / `0x102D1D37` / `0x102D1E2D`) up to `0x102D1EFC` (after `push 4; ...; call 0x102CE980`, then `cmp [ebp-0x88], 0`). Skipping only slots 2-4 (from `0x102D1C1F`) still left the throwable box on screen, so the mod hooks `0x102D1B36` (MinHook, a naked detour) and jumps to `0x102D1EFC` while its grid is shown: the gun and melee boxes stay, the mod takes the keys `3`..`5` for its boxes |
| `m_iMaxSlots` | `"MaxSlots"` (5) is a `CPanelAnimationVar` at `this + 0x22C` of the Panel part (getter `0x102CD600`), not used by Paint for the boxes |

The game's slot keys (`1`..`5`) and the wheel (`invprev` / `invnext`) are not touched in the game: with
`replace_native=1` the mod's window procedure swallows `1`..`0` and `WM_MOUSEWHEEL` itself (unless the console, chat or
a menu has the keyboard). `Q` (`lastinv`) is left alone, so the player can still go back to the gun.

With `replace_native=0` (the default) the panel draws and keeps its keys `1` and `2`; the mod takes `3`..`9`, `0` (inventory boxes 0-7), the
wheel (its own order: gun, melee / pistol, then a 3-wide grid row by row, switching with slot 429 or the inventory's
use) and its drop key (drops the active weapon through slot 283). The panel's own throwable / kit / pills boxes are
skipped (see `Paint` above); the inventory is a grid of three boxes per row starting where the throwable box was, and a
carried item is shown in its home box (`Stack::held`, which `refresh_native` gives every catalogue item the player
carries, so an item picked up in the game gets one too). The panel's place is `HudWeaponSelection` in `scripts/hudlayout.res` (update
VPK): `xpos r98`, `ypos c-90`, 100 x 200, boxes stacked upwards, in 480-line units. Measured in the game at 2560x1440
(1 unit = 3 px): the small boxes (throwable, kit, pills) are 70 px (23.3 units) square, 80 px apart, right edge at
x = 2535 (8.3 units in from the right), the last one ends at y = 882 (centre + 54 units); the melee and gun boxes start
at x = 2375 (61.7 units in), and the gun icon sticks out of its box to x = 2355. Its colours: selected
`142 214 57`, unselected white. The grid uses exactly those sizes (not `hud_scale`), so its boxes line up with the
panel's. With more than about 12 slots the rows below reach the player's own health panel (about 425 units down).

## Map item spawners (weapon_*_spawn)

Items the director or the map places (pills on a table, the safe-room kits) are `CWeaponSpawn` entities (vtable
`0x106796E4`). Their `Use` (entity vtable slot 106, `0x1029DCB0`) hands out a fresh copy through
`GiveItem` (spawner slot 244, `0x1029E410`) -> `player->GiveNamedItem(name, 0, 1, 0)` (slot 431, `0x103111E0`), which
creates the weapon and calls the +use pickup (slot 426, the mod's hook). With that third argument `GiveNamedItem`
checks afterwards that the weapon's `m_hOwner` (`+0x13E8`, SendProp `push 0x13E8 / "m_hOwner"`) is the player
(`0x10311773`); if not it removes the copy and returns null. Only a non-null result makes the spawner fire
`spawner_give_item`, take one off its count (`+0x1444`, 0 = endless) and remove itself at zero (`0x1029DFBC`).

So when the hook stores an item it returns **true** and writes the player's EHANDLE into the copy's `m_hOwner`; the
copy is already marked for deletion (`UTIL_Remove`), and the spawner counts the pickup. Before this, a stored map item
stayed on the table and could be stored again and again (seen in the log as the same pointer twice: the allocator
reuses the freed copy's memory). A weapon lying in the world (`CBaseCombatWeapon::Use`, slot 106, `0x1024F4A0`)
ignores both the result and the owner.

When the player already carries the **same** item, the spawner refuses by itself and never reaches the +use hook:
`Use` asks `Weapon_GetSlot(player, weaponId)` (`0x102C98F0`, thiscall, `ret 4`; reads `[player + 0x1800 + id]` for
the slot and `m_hMyWeapons`) at `0x1029DD27`, and a carried weapon of the same class leads to a branch that gives
nothing. The mod hooks `Use` (`55 8B EC 83 EC 2C 53 57 8B 7D 08 8B D9`; `CWeaponSpawnConfigurable` shares it) and
`Weapon_GetSlot` (`55 8B EC 8B 45 08 0F B6 84 08 00 18 00 00`): for the local player, a catalogue item, the slot full
and room in the inventory, `Weapon_GetSlot` answers null for that weapon ID during that one `Use`, so the spawner
gives as for an empty slot and the +use hook stores the copy. The spawner's weapon ID is its vtable slot 240 (it
differs per class: `0x1029E3E0`, `0x1029CAB0`); ID -> classname is `0x1024EB70` (cdecl, a table at `0x107A3F50`:
12 `weapon_first_aid_kit`, 15 `weapon_pain_pills`, 23 `weapon_adrenaline`, ...).

Do not test with `ent_create weapon_pain_pills_spawn`: a spawner created from the console without its keyvalues
crashed the game in vphysics.dll (2026-10-16 03:42).

## Dedicated server (srcds.exe)

The `Left 4 Dead 2 Dedicated Server` install has the same `server.dll` (timestamp `0x6A440E53`) and `engine.dll`
(`0x6A440ED6`, sha256 `8a6d4680...`) as the game, so everything above works inside `srcds.exe` (Windows). The DLL
notices `srcds.exe` (`dllmain.cpp`), runs only the game side, logs to `inventory_mod_server.log`, and unloads when
`inventory_mod_server.unload` appears next to it. `injector.exe <dll> <pid>` picks one of several `srcds.exe`.

Each human player has an inventory of their own, keyed by SteamID, saved as `inventory_<steamid>.inventory` next to
the DLL. The game code is the same as for a listen server: an `Activation` makes the player "the local player" and
their `Session` the active one while a hook or a command serves them, and when it ends the player is sent their
inventory as one console line.

| What | Where / how |
|---|---|
| `VEngineServer022` (`CVEngineServer`, vtable `0x1037D9BC`, 138 slots) slot 17 | `GetPlayerNetworkIDString(edict)` -> `"STEAM_1:0:..."` / `"BOT"`: what `CBasePlayer::GetNetworkIDString` (server `0x1016D9F0`) calls with `[player + 0x28]` (the edict) |
| slot 46 | `ClientPrintf(edict, text)`, `ret 8`: calls the client's `ClientPrintf("%s", text)` (`"tried to sprint to a non-client"` otherwise) |
| slot 39 | `ClientCommand(edict, fmt, ...)` (cdecl, `this` on the stack; `"ClientCommand, 0 length string supplied."`). Not used |
| server `0x10090C80` | `ClientCommand(CBasePlayer*, const CCommand&)`, cdecl: what `CServerGameClients` (vtable `0x1060CAD4`) slot 5 calls for each command a client sends. `CCommand`: argc `+0`, argv[] `+0x408` (`argv[0]` is compared with `"killtarget"`, `"demorestart"`...). Hooked: `inv_*` commands are the mod's |
| `VEngineClient013` (`CEngineClient`, vtable `0x10359DEC`) slot 26 | `IsInGame()`: `GetBaseLocalClient()->m_nSignonState == 6`; slot 27 `IsConnected()` (`>= 2`) |
| slot 107 | `ClientCmd_Unrestricted(text)` (`Cbuf_AddText`); slot 7 `ClientCmd` (restricted), slot 103 `ExecuteClientCmd` |
| engine `0x10052100` | `CBaseClientState::ProcessPrint(SVC_Print*)`, thiscall `ret 4`: `ConMsg("%s", msg + 0x54)`. Hooked on the client: lines starting `@inv@` are the mod's and are not printed |

Protocol (all text):

- client -> server, console commands the client does not know, so the engine forwards them: `inv_sync`,
  `inv_use <box>`, `inv_drop <box> <count>`, `inv_store`, `inv_pickup`, `inv_move <from> <to>`,
  `inv_select <weapon_...>`, `inv_dropweapon <weapon_...>`. The HUD sends `inv_sync` once a second while the client
  is in a game it does not host (`IsInGame()` and no player in its own `server.dll`).
- server -> client, after every request and pickup: `@inv@ <slots> <max_weight>|<box> <classname> <count> <held>,...|
  <game slot 0..5 classname or ->,...|<notice>` (`*` before the classname in the player's hands).

**Listen servers:** the same server side runs in the host's game for every guest (everyone but entity 1, the host,
who keeps the local inventory). Guest inventories are saved on the host's machine as `inventory_<steamid>.inventory`
next to the DLL. Only players whose game runs the mod are served (`has_mod`, set by the first `inv_*` command their
HUD sends); for the others the game behaves as without the mod. The host's HUD always draws `local_session()`, while the
game code uses `session()`, which is the guest's while an `Activation` serves them. Verified so far only through the
dedicated server (same code path); a real guest on a listen server needs a second machine/account.

The client shows that inventory instead of its own (`Session::remote`; the local file is left alone and comes back
when the player leaves). Verified: client connected to `srcds -insecure +sv_lan 1 -port 27020`, the server logged
`inventory of STEAM_1:0:...` and the client `the server keeps this inventory now`; the HUD showed the server's empty
inventory and the pistol in game slot 2. (The server binds to the LAN address, so `connect 192.168.1.2:27020`, not
127.0.0.1.)

## What the player sees (client side)

The hotbar shows only while the local player is a living survivor in a game with no menu open, and it shows the ammo
of the weapon in the hands; both come from the client's own entities, so they work as host, guest and on a
dedicated server (refreshed in the pump, 10 times a second).

| What | Where / how |
|---|---|
| `VClientEntityList003` (client `CClientEntityList`, vtable `0x10564B60`) | slot 3 `GetClientEntity(index)` (`55 8B EC 8B 45 08 85 C0 78 ?? 3D 00 10 00 00`), slot 4 `GetClientEntityFromHandle(handle)` |
| `VEngineClient013` slot 12 | `GetLocalPlayer()`: `GetBaseLocalClient()->m_nPlayerSlot + 1` |
| `VEngineVGui001` slot 2 | `IsGameUIVisible()`: GameUI's `IsGameUIActive` (main menu, pause menu) |
| client netprops (`push <offset> / push "<name>"` before `RecvPropInt`...) | `m_iTeamNum` `+0xE4` (2 = survivors), `m_lifeState` `+0x147` (byte), `m_iObserverMode` `+0x1450`, `m_hActiveWeapon` `+0x1084`, `m_hMyWeapons` `+0xFA4`, `m_iAmmo` `+0xF24` (32 ints); weapon `m_iPrimaryAmmoType` `+0x97C`, `m_iClip1` `+0x984` |

Items in the hands report a clip too (adrenaline: 1), so the counter is not drawn when the active weapon is a
catalogue item.

## Icons

`src/ui/icons.cpp` reads the game's own icons at run time (nothing is copied into the mod): it indexes
`update/`, `left4dead2_dlc3..1/` and `left4dead2/` `pak01_dir.vpk` in that order (the first archive that has a file
wins, like the game's search path), parses each `scripts/weapon_<name>.txt` `TextureData { "weapon" { file x y width
height } }`, and decodes `materials/<file>.vtf` (7.4, DXT5 here; DXT1/3/5 and 8888 are handled; the largest mip is
the last). The `update` scripts use one texture per weapon (`vgui/hud/icon_smg`, 256x64; `icon_first_aid_kit`,
128x64 with the icon in the left 64x64); the base game's use the sheets `iconsheet`, `iconsheet2`. Each rectangle is
trimmed to its visible pixels (alpha > 24). The carryables (gas can, tanks, fireworks, gnome, cola) and melee have no
icon there and keep their two-letter tag. `tools/re/vpk.py` lists and extracts VPK files for checking.

## vgui2.dll

`VGUI_Input005` is `vgui::CInputSystem` (vtable `0x100457EC`). Its slot 4 returns the panel that has the keyboard (0 in
normal play, non-zero while the console or the chat line is open). The mod's hotkeys are ignored while it is non-zero.

## Item classnames the mod knows

`weapon_first_aid_kit`, `weapon_defibrillator`, `weapon_pain_pills`, `weapon_adrenaline`, `weapon_pipe_bomb`,
`weapon_molotov`, `weapon_vomitjar`, `weapon_upgradepack_incendiary`, `weapon_upgradepack_explosive`, `weapon_gascan`,
`weapon_propanetank`, `weapon_oxygentank`, `weapon_fireworkcrate`, `weapon_gnome`, `weapon_cola_bottles`. Their L4D2
inventory slots (2 throwable, 3 medkit/upgrade, 4 pills, 5 carried) are in `src/inv/items.cpp`; the mod does not give
an item whose slot the player already fills.
