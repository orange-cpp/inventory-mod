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
| `VSERVERTOOLS001` | `CServerTools`, vtable `0x106fb31c`. Slots: 7 `FirstEntity`, 8 `NextEntity`, 10 `GetKeyValue`, 13 `SetKeyValue(const char*)` (11 is the Vector one: see "Rounds"), 14 `CreateEntityByName`, 15 `DispatchSpawn`, 16 `DestroyEntityByHammerId` |
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

A gun does not always come through slot 426. With incendiary or explosive rounds in the gun in the hands (seen), and
also for guns a player dropped, +use calls the gun's own `CTerrorGun::Use` (`0x103E8700`, slot 106 of `CTerrorGun`'s
vtable `0x106D5544` and of every gun class's, e.g. `CRifle_Desert`; thiscall `(activator, caller, type, value)`,
`ret 0x10`, prologue `55 8B EC 83 EC 08 56 57 8B 7D 08 8B F1 85 FF`). It checks the activator (`IsPlayer`,
`Weapon_CanUse` slot 280) and equips with `Weapon_Equip` (slot 281) itself, so the gun in the hands was swapped out and
thrown, never stored. The mod hooks it too and makes the same choice as for slot 426 (`weapon_pickup`: into a free
weapon box when that game slot is taken). Seen: holding a gun, E on an M16 and an auto shotgun on the floor came
through it and both went into boxes; with the slot free the game's own pick-up.

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
| `Paint` | slot 128 of the panel's `vgui::Panel` vtable `0x105D5734`: `0x102D11E0`. Unrolled, one part per box: `0x102CEB20(slot)` (thiscall on the `CHudElement` part, `esi - 0x48`) gets the weapon in a slot, then `0x102CE5B0` / `0x102CE4C0` draw the box and `0x102CE980` its key. The gun is slot 0 (ends with `push 0; ...; call 0x102CE980`), the melee / pistol slot 1 (ends with `push 1; mov ecx, ebx; call 0x102CE980` at `0x102D1B2D`), then a part that calls `0x102CDDF0` and ends with key label 5 (`0x102D1C16`), then slots 2, 3, 4 (`push 2/3/4` before `0x102CEB20` at `0x102D1C48` / `0x102D1D37` / `0x102D1E2D`) up to `0x102D1EFC` (after `push 4; ...; call 0x102CE980`, then `cmp [ebp-0x88], 0`). Skipping only slots 2-4 (from `0x102D1C1F`) still left the throwable box on screen, so the mod hooks `0x102D1B36` (MinHook, a naked detour) and jumps to `0x102D1EFC` while its grid is shown. With slot 1 empty (no melee or pistol) the melee part is left by `je 0x102D1B59` at `0x102D17BF`, past that hook (seen in the game: the item boxes came back), so `0x102D1B59` (`mov eax, [ebp-0x64]; push eax; mov ecx, ebx; call 0x102CDDF0`, found from the `je`'s target) gets the same detour: the gun and melee boxes stay, the mod takes the keys `3`..`5` for its boxes |
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
- server -> client, after every request and pickup: `@inv@ <slots> <max_weight> <weight>|<box> <classname> <count> <held>,...|
  <game slot 0..5 classname or ->,...|<notice>` (`*` before the classname in the player's hands). `<weight>` is what
  the stored items weigh by the server's rules (`inventory_mod.toml` may weigh items differently from the client's
  table), so the client's weight bar shows the server's count; older clients read only the first two numbers.

**Rules file:** `inventory_mod.toml` next to the DLL (`[limits] slots / max_weight`, `[weight]`, `[stack]`, read with
toml++ in `inv::apply_rules`) is read after the settings (`inventory_mod_settings.toml`), at load. The server's limits apply to every player's inventory
it creates; a refused store says why ("Too heavy: ..." when the weight limit is what stopped it). The weight includes
the items in the game slots (`Stack::held`, the box each carried catalogue item gets), so the +use hook (slot 426)
also answers false, before the original equips it, for an item that would go into a free game slot but is too heavy;
a map spawner then keeps its item (GiveNamedItem fails, see the spawner notes). The gun and melee / pistol in game
slots 0 and 1 count too (`inv::weapon_weight`, `[weight]` entries for weapon classnames): `refresh_native` sets
`Inventory::set_weapons_weight`, and the same hook refuses a weapon whose weight, less the weight of the one it replaces
in its slot, would go over the limit (`weapon_too_heavy`, which refreshes the slots first since a server player's may
be stale).

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

## The infection

Server side, on the main thread (see "the infection" in game.cpp):

| What | Where / how |
|---|---|
| events | engine `IGameEventManager2` slot 7 `FireEvent(IGameEvent*, bool)` `0x10180830` (logs "Game Event Fired: %s"; prologue `55 8B EC A1 ?? ?? ?? ?? 83 78 30 00`), hooked. `CGameEvent` (vtable `0x103881E4`, KeyValues at +8): slot 1 `GetName`, slot 6 `GetInt(key, default)`, slot 9 `GetString`. Fields (resource/modevents.res): `tongue_grab`, `lunge_pounce`, `jockey_ride`, `charger_carry_start`, `charger_pummel_start` -> `victim`; `player_now_it` -> `userid`, `by_boomer`, `exploded`; `player_incapacitated` -> `userid`, `attacker`, `attackerentid`; `pills_used` -> `subject` (who swallowed) |
| userid -> player | engine `VEngineServer022` slot 16 `GetPlayerUserId(edict)` `0x10131FB0` (the client of that edict's +0x38), over players 1..64 and their edict (+0x28) |
| team, life | `m_iTeamNum` +0x238 (2 survivors, 3 infected), `m_lifeState` +0xF0 |
| the clock | server `ServerGameDLL005` (`CServerGameDLL`, vtable `0x1060C904`) slot 4 `GameFrame(bool)` `0x100EF4B0`: slot 3 takes three arguments (ServerActivate), 4 and 5 one bool (GameFrame, PreClientUpdate); hooked, it ticks with the server |
| dying | `CTerrorPlayer` slot 476 `CommitSuicide(bool explode, bool force)` `0x10174210`: what the `kill` command calls (`0x1008EAB0`: `call [vtable + 0x770]`) |
| rising | the VScript `ZSpawn({type, pos})` (`Script_ZSpawn` `0x1035FA60`; the native spawn `0x1035AF70` passes registers its own way), run through the `script` command's handler `0x1021CB90` (cdecl `const CCommand&`; the command is FCVAR_CHEAT, the handler is not) with a CCommand built by the mod (argc +0, the line "script <code>" at +8, argv[] at +0x408). Seen in the game: ZSpawn answered true (its result written with `StringToFile` into left4dead2/ems) |
| no defibrillator | the `survivor_death_model` nearest to where they died is removed (UTIL_Remove) a moment after |
| no rescue closet | server `0x102CEAF0` `CSurvivorRescue::SetSurvivor(CTerrorPlayer*)` ("(RESCUE) Rescue spot at ... activated / deactivated"; `+0x192C` the survivor's handle), hooked: for a survivor who died of the infection it is given nullptr |
| clearing | `round_start`, `round_end`, `map_transition`: every clock and every "no rescue" goes |

The clock reaches the player's game as `@inf@ <ms left> <ms in all>` (ClientPrintf, caught like `@inv@`), or straight
into the host's session. `pills_used` puts a running clock back to its full time (`no_cure`, the default) or ends it.

The effect (`src/ui/postfx.cpp`) is a vs_3_0 / ps_3_0 pair compiled at run time with `d3dcompiler_47.dll`
(`D3DCompile` through GetProcAddress, nothing linked). In the Present hook, after the mod's ImGui frame is rendered
(so the mod's HUD, wheel and window are infected too), the back buffer is copied with `StretchRect` into a render-target texture of the same size and
format (default pool: released before Reset, made again on the next frame) and drawn back over the whole back buffer
through the pixel shader (full-screen quad in clip space, half-pixel offset, sRGB reads and writes off: the game's back
buffer already holds final colours). The overlay's D3DSBT_ALL state block puts the game's state back. Seen in the game:
"postfx: infection shader ready (11552 bytes)", the frame intact with the effect at the start of the clock.

### The sound

| what | where |
|---|---|
| the device | `CAudioDirectSound` (vtable `0x103518EC`; engine.dll loads dsound.dll itself, "DirectSoundCreate" by GetProcAddress). The whole mix goes into one looping `IDirectSoundBuffer` kept in `0x1043F3A4` (seen in the game: 2 channels, 44100 Hz, 16 bits, 65536 bytes) |
| the transfer | `TransferSamples` `0x1000A8E0`: Lock (vtable `+0x2C`, slot 11) at `0x1000A9C6`, the paint buffer copied in, Unlock (`+0x4C`, slot 19) at `0x1000AA54` (pattern `8B 75 FC 8B 0D ?? ?? ?? ?? 8B 11 56 50 8B 45 F8 50 8B 45 08 50 51 8B 4A 4C FF D1`, the global at +5, returns to `0x1000AA56`). The surround transfer `0x100091E0` Unlocks the same buffer from `0x10009409` (`8B 55 E8 A1 ?? ?? ?? ?? 8B 08 6A 00 6A 00 52 8B 55 F8 52 50 8B 41 4C FF D0`). Slot 14 `0x10009790` is ClearBuffer (Locks the whole buffer, memset, Unlock) |
| the hook | dsound.dll's Unlock, read from that buffer's vtable, hooked (MinHook): only calls returning to those two places are processed, and never one covering the whole buffer (a clear). Seen in the game: "audio: engine buffer ..., Unlock ... hooked (from engine+0xAA56 and engine+0x9409)" and the buffer's format logged from the first call |

What `src/ui/audio.cpp` does to it, by the severity: a delay line swept by two slow waves (the pitch bends), two
one-pole low-passes (15 kHz down to about 500 Hz), the level ducked on each beat and a falling 52 Hz thump synthesized
on it (lub, dub a quarter later; the beat count comes from the HUD so it falls on the screen's throbs), a 7.1 kHz ring
past 0.35, a soft saturation past 0.75 (level-matched for quiet sounds; an earlier crusher that cut the levels was
all hiss at the top); faded in and out over 0.6 s (out when the HUD
stops reporting for 300 ms). The engine applies its master volume while it mixes, so what the mod adds (the scream, the
thump, the ring) is scaled by the `volume` ConVar itself: `0x1044A560`, registered at `0x10338200` (`B9 [ConVar] 6A 01
68 ["Sound volume"] 68 80 00 00 01 68 ["1.0"] 68 ["volume"] E8`), `m_fValue` at +0x2C of `m_pParent` (+0x1C). Seen in the
game: 0.12, as in cfg/config.cfg. The scream goes over the game's mix (at 0.6) through a soft limiter above 0.8. The game's sounds are loose files:
`left4dead2/sound/npc/witch/voice/mad/female_ls_d_madscream01.wav` is 16-bit mono 44100 Hz PCM.

### Coughing

The server makes each infected survivor cough (`[infection] cough`): the wait is drawn from 26 s down to 4 s as the
clock runs out (60 .. 140 % of that, at least 2.5 s), checked in the GameFrame hook. The cough is one VScript line
through the `script` handler: `GetPlayerFromUserID(id)`, `GetModelName()` (survivor_coach.mdl ...), one of the
survivor's soundscripts, `PrecacheScriptSound`, `EmitSoundOn` (all in server.dll 2.2.4.3). The soundscripts, from
scripts/game_sounds_<survivor>.txt ("[Coughing from smoke]"): `Coach_Cough01..08`, `Gambler_Cough01..04`,
`Mechanic_Cough01..04`, `Producer_Cough01..07`, `Player.Biker_Cough01..07`, `Player.Manager_Cough01..05`,
`Player.NamVet_Cough01..06`, `Player.TeenGirl_Cough01..06` (Zoey's model is survivor_teenangst). Seen in the game: Nick
coughed `Gambler_Cough02`, then `Gambler_Cough04` (written by a StringToFile after EmitSoundOn).

The `script` console command is a cheat ("Can't use cheat command script in multiplayer"), its handler is not: the
mod calls the handler. A script line is at most 480 characters there (the mod's CCommand), and an `::InvCough <-
function` defined by one line was missing ("the index 'InvCough' does not exist") for the next, so the cough is one
line with a try / catch; an error goes to left4dead2/ems/inv_cough_error.txt.

### Going insane

From `insane_at` % of the clock gone (`[infection] insane`), the survivor laughs or screams now and then: a wait of 18 s
down to 5 s over the rest of the clock (60 .. 140 % of that, at least 3 s), a scream 30 % of the time at first and 70 %
at the end, never within 2.5 s of a cough. The server picks the soundscript itself (one script line has room for one
name, not the lists): the survivor by `CBaseEntity::m_ModelName` +0x24C (datamap "m_ModelName", `FIELD_MODELNAME` 18,
a pointer to "models/survivors/survivor_gambler.mdl"; IServerTools `GetKeyValue("model")` gives the pointer's bytes,
not the text), then `PrecacheScriptSound` and `EmitSoundOn` as for the cough. The names are from
scripts/game_sounds_<survivor>.txt: "<Hearty / Short / Nervous laugh>" (`Gambler_Laughter01`, `npc.Biker_Laughter01`
...; the numbers have gaps), "[Loud painful / Short quick death scream]" (`Coach_DeathScream01`,
`Player.TeenGirl_DeathScream05` ...) and `*_ScreamWhilePounced*` ("GET IT OFF!"). Seen on the dedicated server: Nick
played `Gambler_ScreamWhilePounced03` and `Gambler_DeathScream02` / `03`, nothing in left4dead2/ems/inv_insane_error.txt.

### The screamer

The server's `turn` sends `@inf@ 0 0 1` (the third number: turned now; older clients read only two), or sets the
host's `Session::infection_turned_at`. The HUD rolls the player's `screamer_chance` (0 .. 1; logged "screamer:
shown / skipped (rolled .., chance ..)") and then shows the picture for 1.7 s (a managed-pool A8R8G8B8 texture read
with WIC) and starts the scream. With the screen effect on, postfx draws it into the back buffer itself (fixed function,
pre-transformed quads over the frame and the mod's UI: black backdrop, the picture, a red wash, the first instant's
white flash) before its pass, and the pass runs at severity 0.92
over it; with the effect off it goes on ImGui's foreground draw list.

## Stamina

Server side, in the GameFrame hook, for each living survivor (see "stamina" in game.cpp). All of it read out of
server.dll's SendProp / datamap definitions (`push <offset> / push "<name>"`, or the datamap's `mov [..], "<name>" /
mov [..], <offset>`) and checked in the game:

| What | Where / how |
|---|---|
| the game's clock | `PlayerInfoManager002` (`CPlayerInfoManager`, vtable `0x1062EFEC`) slot 1 `GetGlobalVars` `0x101897E0` (`mov eax, [0x107F7690]; ret`). `CGlobalVars::curtime` at +0xC: what most of server.dll reads of it, and what it compares `m_flNextPrimaryAttack` with |
| on the ground | `CBasePlayer::m_fFlags` +0x13C, `FL_ONGROUND` 1 |
| running, jumping | `CBaseEntity::m_vecVelocity` +0x2D8 (also `m_vecAbsVelocity` +0x260): running is on the ground faster than 130 (a walk is 85, a run 220, a limp 150); a jump leaves the ground with z above 150 |
| swings, shoves | the active weapon's `m_flNextPrimaryAttack` +0x13F0 / `m_flNextSecondaryAttack` +0x13F4 moving on. Seen: a shove (pistol or melee) waits 0.77 s and moves both; a melee swing moves both too; a reload moves the secondary a tenth or two (so a shove must be at least 0.4 s out); gunfire only the primary. A shove raises `CTerrorPlayer::m_iShovePenalty` +0x2CF0, a swing does not; `CBasePlayer::m_nButtons` +0x1CB4 (`IN_ATTACK2` 0x800) often misses a quick press |
| the game's shove fatigue | `m_iShovePenalty` set back to 0 with `SetShovePenalty(int)` `0x102CB750` (thiscall, `cmp [esi+0x2CF0], edi`), which marks the edict changed. Seen: shoves in a row stay at 0.77 s |
| slower | `CBasePlayer::m_flLaggedMovementValue` +0x20D0, the game's movement speed scale (the client predicts with it). It slows the player's whole movement clock, falls too, so it is set on the ground only; leaving the ground the horizontal velocity is scaled instead |
| networked writes | a field written by the mod only reaches the players if the entity says it changed: `edict->m_fStateFlags \|= 0x101` (FL_EDICT_CHANGED \| FL_FULL_EDICT_CHANGED) and the change accessor's serial (+2) 0, as `0x100ED4B0` does through `VEngineServer022` slot 97 `GetChangeAccessor(edict)` (the server's own engine pointer `0x107F7698` is the same interface) |
| less damage | `CBaseEntity::TakeDamage(const CTakeDamageInfo&)` `0x10055C70` (thiscall, `ret 4`, prologue `55 8B EC 83 EC 6C 57 8B F9 8B 0D`; warns "CBaseEntity::TakeDamage:  with inputInfo.GetDamageForce() == vec3_origin"), hooked. `CTakeDamageInfo` is 0x60 bytes (its copy constructor `0x10010140`): `m_hInflictor` +0x30, `m_hAttacker` +0x34 (`GetAttacker` `0x1003D4B0`), `m_hWeapon` +0x38, `m_flDamage` +0x3C, `m_bitsDamageType` +0x48. Seen: a melee hit on a common is weapon and inflictor `weapon_melee`, type `0x80200004`, 50 damage (then a 0 one); exhausted it became 25 and the common took two hits |

Exhausted, the wait for the next swing or shove is stretched (seen: 0.99 s became 1.65 s at `swing_speed = 60`). The
stamina reaches the player as `@sta@ <per mille, -1: none> <1: exhausted>` (at most ten a second while it changes), or
straight into the host's session. The HUD draws the bar; postfx runs a second, small pixel shader (1512 bytes) before
the infection's: the edges darken and lose colour from half down, and while exhausted close in, blur and pulse.

## Ammo maxima

The most reserve rounds of each type are server.dll's ConVars `ammo_<type>_max` (assaultrifle 360, smg 650, shotgun 72,
autoshotgun 90, huntingrifle 150, sniperrifle 180, grenadelauncher 30, m60 0, chainsaw 20: their registrations, e.g.
`0x10598CB0`: `push 0x2000; push "360"; push "ammo_assaultrifle_max"; mov ecx, 0x1084BBC0; call ConVar::ConVar`). The
ammo types use them as their max carry (`0x102CFEE0` registers `AMMO_TYPE_ASSAULTRIFLE` with that name). At run time
their flags read `0x2006`: FCVAR_DEVELOPMENTONLY too, so the console answers "Unknown command". The rules file's
`[ammo]` values are set once, on the server's main thread (the dedicated server's first client command, or the host's
pump): the ConVar is found by that registration (its name in server.dll, then `68 <name> B9 <ConVar>`), the 0x2 flag is
taken off it (`m_nFlags` +0x14) and off its parent (`m_pParent` +0x1C; on a listen server the client's copy, which holds
the value: flags `0x2008`), then `VEngineServer022` slot 37 ServerCommand (engine `0x10130D40`, prologue
`55 8B EC 56 8B 75 08 85 F6 75`; it queues the line, which must end in `\n` or `;`) runs `ammo_<type>_max <n>`. Seen in
the game: the parent's `m_nValue` (+0x30) went from 360 to 200 with `assault_rifle = 200`.

## Rounds (the reserve as a thing of its own)

The reserve (`m_iAmmo[32]`, player +0x1874) weighs: `inv::ammo_weight` of it (`[round_weight]`, defaults rifle 0.01,
SMG 0.006, shells 0.04, sniper / hunting 0.02, grenade 0.15) is the inventory's `ammo_weight`, set in `refresh_native`
and sent to remote clients inside the total weight. The Items tab lists each type carried (the client's own
`m_iAmmo`) with "Drop <magazine>" and "Drop all" (`inv_dropammo <kind> <count>` on a server).

| What | Where / how |
|---|---|
| the pile | `weapon_ammo_spawn` is `CWeaponAmmoSpawn` (RTTI), vtable `0x10682CF4`, which its factory (`0x102A22F0`, registered for "weapon_ammo_spawn" at `0x10596370`) writes over `CWeaponSpawn`'s (`0x106796E4`). Slot 26 Precache `0x1029F5A0` (precaches `models/props/terror/ammo_stack.mdl`), 239 its model `0x1029F560` (the "model" keyvalue, else that one), 240 its weapon id (`0x36`), 106 Use `0x1029F5F0` (its own: the refill, or `ammo_pile_weapon_cant_use_ammo`) |
| what it holds | its targetname, `inv_rounds_<kind>_<rounds>`, read from `CBaseEntity::m_iName` +0x140 (datamap "m_iName", a pooled string) |
| picking it up | `CWeaponAmmoSpawn::Use` hooked (prologue `55 8B EC 51 53 8B 5D 08 85 DB 0F 84`, checked against the vtable): a pile of the mod's gives its rounds to a survivor (not a bot), as many as fit by the type's most and the weight limit; the rest stays in it. Any other pile is the game's refill. Seen: 50 rifle rounds and 10 auto shotgun shells dropped and taken back |
| its model | `models/w_models/weapons/w_eq_incendiary_ammopack.mdl` (the closed incendiary pack; loaded with the map, seen in c1m3 with `VEngineServer022` slot 9 `IsModelPrecached`), tinted the type's colour with the "rendercolor" keyvalue as "r g b 255" (three numbers leave the alpha 0: the pack was invisible); `ammo_stack.mdl` when it cannot be precached (`CBaseEntity::PrecacheModel` `0x1006B690`, cdecl; engine slot 5 `PrecacheModel(name, true)`) |
| the weight cap | ten times a second (GameFrame) each survivor with an inventory here (the host, the served players) has their reserve compared with the last look; a rise that takes the load past the limit is taken back down to what fits (an ammo pile, a gun picked up) |

`IServerTools` slot 11 is `SetKeyValue(ent, key, const Vector&)` (`0x104CF140`, to the entity's KeyValue slot 32
`0x1006D4D0`, which formats `"%f %f %f"`); the text one is slot 13 (`0x104CF100`, entity slot 34 `0x10070670`, which
compares "rendercolor" ...). MSVC puts overloads in the vtable last first. The mod used 11 until 2026-10-16: the pile's
model and targetname became numbers ("17757245829489361000.000000 ..."), so it was the ERROR model and was not
recognised; a stored melee weapon dropped from a box got the same garbled `melee_script_name`.

## Boxes on demand

The weight is the limit: `slots` (24) and `weapon_slots` (8) are only caps (an ini from before, `layout` < 4, is raised
to them). The inventory keeps all its boxes (numbers stay put, items never move), and the HUD draws the item boxes up
to the last one in use plus one free box, and the weapon boxes up to the last one in use (`shown_items` /
`shown_weapons`); the grid moves up and down with them.

## Carried items and switching

A game-slot-5 item (gas can, tanks, fireworks, gnome, cola) is dropped by the game as soon as the hands switch to
anything else. Every switch the mod makes (`do_select_weapon`, `do_use`) first calls `put_away_carried`, which, when
the active weapon is such an item (and not the target), stores it the way `swap_out` does: into its box (or any),
the quiet `Weapon_Drop`, then `UTIL_Remove`. The game's own slot keys are not the mod's, so they still drop it.

## The radial wheel (client side)

Held middle mouse button (`WM_MBUTTONDOWN` / `WM_MBUTTONUP` in the mod's window procedure). While it is open the HUD
asks for input like the inventory window does: the overlay swallows the input messages (`WM_INPUT` too, so the view
stops), hooks `GetCursorPos` / `SetCursorPos` for the game and draws ImGui's cursor; the wheel puts the cursor in the
middle of the window when it opens (SetCursorPos from the DLL passes the hook). Unlike the window it leaves the
keyboard messages to the game (`hud::wants_keyboard()`), so the player keeps walking. When the HUD lets go of the mouse
(wheel or window closed) the overlay puts the real cursor back in the middle first: the game saw a cursor pinned to the
middle while captured, and its next read would otherwise be the whole way the pointer travelled, a flick of the view.
The segment is picked by the cursor's
direction from the middle. Its entries are the grid's wheel order; taking one sends the same requests as the grid
(`select_weapon` / `use`, `drop_weapon` / `drop`), so it works on a dedicated server unchanged. The toolkit's fonts are
for 12 px cells, so the wheel draws its text through `ImDrawList::AddText(font, size, ...)` scaled by height / 720.

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
trimmed to its visible pixels (alpha > 24). The carryables (gas can, tanks, fireworks, gnome, cola) have no icon there
and keep their two-letter tag. `tools/re/vpk.py` lists and extracts VPK files for checking.

Melee weapons: `scripts/melee/<kind>.txt` (13 of them: baseball_bat, cricket_bat, crowbar, electric_guitar, fireaxe,
frying_pan, golfclub, katana, knife, machete, pitchfork, shovel, tonfa). The update archive's say `TextureData {
"sprite_active" { "file" "vgui/hud/icon_fireaxe" "x" "0" "y" "0" "width" "128" ... } }`, the base game's only
`"sprite_active" "icon_fireaxe"` (the whole of `materials/vgui/hud/icon_fireaxe.vtf`, 128x64 DXT5, VTF 7.2). The icons
are keyed by the kind, so the HUD looks a melee weapon up by its `m_strMapSetScriptName`, not by `weapon_melee`.

## vgui2.dll

`VGUI_Input005` is `vgui::CInputSystem` (vtable `0x100457EC`). Its slot 4 returns the panel that has the keyboard (0 in
normal play, non-zero while the console or the chat line is open). The mod's hotkeys are ignored while it is non-zero.

## Item classnames the mod knows

`weapon_first_aid_kit`, `weapon_defibrillator`, `weapon_pain_pills`, `weapon_adrenaline`, `weapon_pipe_bomb`,
`weapon_molotov`, `weapon_vomitjar`, `weapon_upgradepack_incendiary`, `weapon_upgradepack_explosive`, `weapon_gascan`,
`weapon_propanetank`, `weapon_oxygentank`, `weapon_fireworkcrate`, `weapon_gnome`, `weapon_cola_bottles`. Their L4D2
inventory slots (2 throwable, 3 medkit/upgrade, 4 pills, 5 carried) are in `src/inv/items.cpp`; the mod does not give
an item whose slot the player already fills.

Guns and melee weapons (slot 0: `weapon_smg`, `weapon_smg_silenced`, `weapon_smg_mp5`, `weapon_pumpshotgun`,
`weapon_shotgun_chrome`, `weapon_autoshotgun`, `weapon_shotgun_spas`, `weapon_rifle`, `weapon_rifle_ak47`,
`weapon_rifle_desert`, `weapon_rifle_sg552`, `weapon_hunting_rifle`, `weapon_sniper_military`, `weapon_sniper_awp`,
`weapon_sniper_scout`, `weapon_rifle_m60`, `weapon_grenade_launcher`; slot 1: `weapon_pistol`, `weapon_pistol_magnum`,
`weapon_melee`, `weapon_chainsaw`) are items too (`ItemDef::weapon()`), one per box, with a `WeaponState`, in the
inventory's weapon boxes (numbered after the item boxes: `first_weapon_slot()`; the state line's header carries their
count). The HUD draws them as wide boxes the size of the panel's melee box (53.3 x 20 units, measured like the item
boxes), 26.7 units apart from where the panel's throwable box began, and moves the item grid down by as much.

## Storing weapons (server.dll)

| What | Where / how |
|---|---|
| `m_iClip1` | weapon `+0x1414` (SendProp at `0x1004C5C6`: `push 0x1414 / push "m_iClip1"`) |
| `m_iPrimaryAmmoType` | weapon `+0x140C` (`0x1004AFEC`) |
| `m_iAmmo[32]` | player `+0x1874` (`0x1016CE1D`): a carried gun's reserve is the player's count of its ammo type |
| `m_iExtraPrimaryAmmo` | `CTerrorGun` `+0x14F4` (`0x1024F100`, datamap entry `0x107A4160`): the reserve a gun lying in the world carries. A gun the map placed has 0 there (the game gives its own amount on pick-up), so 0 reads as "the game's amount" |
| `m_strMapSetScriptName` | `CTerrorMeleeWeapon` `+0x17E0`, FIELD_STRING (a `const char*`), datamap entry `0x107BDF20` with the keyvalue name `"melee_script_name"`: which melee weapon (`fireaxe`, `katana`, ...; `scripts/melee/*.txt`) |
| `m_upgradeBitVec` | `CTerrorGun` `+0x17E0` (`0x103E724F`: `push 0x17E0 / push "m_upgradeBitVec"`, beside `m_hasDualWeapons` `0x17DD`): 1 incendiary ammo, 2 explosive ammo, 4 laser sight (the game tests its low byte). The same offset is the melee weapon's script name, so it is read and written for guns only (not `weapon_melee` or `weapon_chainsaw`) |
| `m_nUpgradedPrimaryAmmoLoaded` | `CTerrorWeapon` `+0x1788` (`0x103ED099`, in `LocalL4DWeaponData`; an int, read with `mov eax, [ecx+0x1788]`): the incendiary / explosive rounds still loaded |

The reserve is the player's `m_iAmmo` of each type, shared by the guns of that type as in the game; a stored gun
keeps only its magazine. Storing a carried weapon: read the clip (and the melee name), then the quiet `Weapon_Drop` and
`UTIL_Remove` (as `swap_out`), with `m_iAmmo[32]` copied before and written back after (whatever the drop does to it).
Taking one out: what is in that game slot is stored the same way into the box being emptied, then `GiveNamedItem` with
the classname, or a melee weapon's script name (as `give fireaxe` does), again with `m_iAmmo` put back as it was (a
given gun would bring the game's own amount), and the stored clip is written onto the new weapon, with its upgrades and
special rounds (then the edict is marked changed, see "Stamina", so the client sees them). Seen in the game: a Combat
Rifle stored with incendiary ammo (upgrade 1, 60 rounds) came back with both, fired down to 57, and kept 57 through
another switch. A gun dropped from a box gets them written the same way (not yet seen in the game). A gun stored from the
ground (pick-up key, E with the slot taken) adds its `m_iExtraPrimaryAmmo` to the player's reserve of its type (not
capped at the type's maximum, `inv::ammo_max`); a gun saved before the shared reserves still carries one, which joins the player's
when it is taken out. Dropping one:
`weapon_melee` gets `melee_script_name` with SetKeyValue before DispatchSpawn; a gun gets `m_iClip1` and
`m_iExtraPrimaryAmmo` after it (not the pistols or the chainsaw, whose reserve is endless or none). +use on a weapon
(slot-426 hook): with that game slot free the game's own pick-up (weight checked); with it taken, the weapon is stored
in a free weapon box with its rounds (`store_world_item`, and `claim_for` writes the player into `m_hOwner` as for
items, so a spawner counts it); with no free box, the game's own swap, refused when the swap is too heavy. The hook
logs each weight check ("weight 25.0 - 3.0 (replaced) + 4.0 = 26.0 of 30.0"). A melee weapon's icon is its kind's
(see Icons); the game slot of the state line carries the kind of the melee weapon in the hands (`weapon_melee:crowbar`).
