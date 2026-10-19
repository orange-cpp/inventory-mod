# L4D2 internals used by the mod

Everything here was read out of the game's DLLs with `tools/re/pe_tools.py` (capstone + pefile) and then checked in
the running game. It is for **one build**: game 2.2.4.3, `server.dll` PE timestamp `0x6A440E53`
(sha256 `a93ec916...`). `game::init()` refuses to run on any other `server.dll`, so a game update fails closed.
Addresses are RVAs from the preferred base `0x10000000`; the mod itself finds them at run time (see "how it is found").
In the code they are declared in `src/game/sdk.hpp`.

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

The hook (`hk_bump_weapon` in `src/game/pickup.cpp`) does, for the local player (entity 1) and a catalogue item only:
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
  `inv_use <box>`, `inv_drop <box> <count>`, `inv_store`, `inv_pickup`, `inv_unload`, `inv_move <from> <to>`,
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

Server side, on the main thread (see `src/game/infection.cpp`):

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
| clearing | `round_start`, `round_end`, `map_transition`, `mission_lost`, `finale_vehicle_leaving` and `finale_win` (the escape and its cutscene, which none of the others cover), and the server clock (`gpGlobals->curtime`) going back (a new campaign from the menu): every clock and every "no rescue" goes, and the host's own effect with them. A client out of a game (`IsInGame` false: quit, loading) drops the effect, and the stamina bar, by itself: before this, quitting while infected kept the effect into the next campaign |

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

## Options > Inventory Mod (a page of the game's menu)

The game's menus are `bin/GameUI.dll` (namespace `L4D360UI`, the Alien Swarm SDK's BaseModUI). The mod's addon
`inventory_mod_menu.vpk` (`tools/make_menu_addon.py`) is mounted at the head of the VPK list (AddVPKFile with 0), so
its files win over the game's; a layout file is read when its page is built, so the in-game menu shows it at once.
The main menu is usually built before the mod is injected, so its Options flyout was the game's own (no "Inventory
Mod") until the menu was built again after a game: the mod runs GameUI's `ui_reloadscheme` once, at the main menu (not
connected), after the addon is mounted. Seen working in game 2026-10-17.

| What | Where / how |
|---|---|
| the button | `resource/UI/L4D360UI/OptionsFlyout.res` (both menus' Options flyout) with `BtnInventoryMod` under `BtnCloud` (ypos 120, the background 145 tall), command `InventoryModSettings` |
| the main menu built again | `ui_reloadscheme` ("Reloads the resource files for the active UI window"), registered at `0x1015A5D0`, its function `0x10016130` (the scheme reloaded, then `CBaseModPanel::GetSingleton()` `0x10009A90`, the active window `0x1000A460` and its layout read again). Sent with `ClientCmd_Unrestricted` (engine `VEngineClient013` slot 107) while slot 27 `IsConnected` (`0x1005F1B0`: `cmp [eax+68h], 2 / setge al`) is false |
| the menus' commands | `MainMenu` (RTTI; vtable `0x1017AF1C`) and `InGameMainMenu` (`0x10178154`), slot 96 `OnCommand(const char*)` (`0x10068220`, `0x1005EE40`; they compare it with "Video", "Audio", "KeyboardMouse", "SteamController", "CloudSettings" ...). The mod's function in both slots takes `InventoryModSettings` |
| opening a page | `CBaseModPanel::GetSingleton()` `0x10009A90` (`mov eax, [0x101FFB10]`), `OpenWindow(const WINDOW_TYPE&, caller, hideCaller, KeyValues*)` `0x1000AC00` (thiscall); "AudioVideo" is window type 3, "Controller" 5 (`0x1005F221`) |
| the page | `AudioVideo` (the Xbox build's; vtable `0x1016908C`), its layout `resource/UI/L4D360UI/AudioVideo.res` replaced. Where it differs from its base `CL4DFrame` (`0x101636AC`): 28-30 class name, 32 destructor (kept); 88 `0x10025A00` kept (the base `0x1000F660`, then vgui slot 71 `SetPaintBackgroundEnabled(true)` `0x100CF120` (flag 8 at +0x60) and the frame's layout `0x1000F360`, as Video's `0x10074810` starts: without it the frame paints no dark band behind `ImgBackground`; seen when it was replaced by the base); 96 `OnCommand` `0x10025430` -> the mod's (else the base `0x100FB8B0`, vgui Frame's "Close" ...); 101 `0x10025220` (finds `SldBrightness` ... by name, only while not found: called again and again) -> the mod's: the base `0x100FC680`, then the values once after an opening; 121 `OnKeyCodePressed` `0x10025EE0` -> base `0x1000EB80`; 127 `0x100259E0` the title -> the mod's (`0x1000F8C0(title, 0, subtitle, 0, 0, 0, 0x7FFFFFFF)`, thiscall `ret 1Ch`); 238 `Activate` `0x10025CA0` -> the mod's: the base `0x100FA2E0`, then the values; 282 `0x10025850` (writes the video config) -> base `0x1000F580` |
| a drop-down | `FindChildByName(name, false)` `0x100CEB70` (thiscall `ret 8`); `DropDownMenu::SetCurrentSelection(command)` `0x1002D180` (the choice with that command shows; it also sends that command to the page, as a click does: the mod's own call from `OnCommand` looped until the stack overflowed); its flyout `0x1002CD50`, given the page's `FlyoutMenuListener` (+0x2BC) by `0x1002DA00`. A choice's command is its own text; the mod's are `InvMod <key> <value>` |
| the blur behind it | `CBaseModPanel`'s per-frame update `0x1000D2D0` fades the view behind the menus in and out: `[esi+0x2A4]` up or down, to `IVEngineClient` slot 165 (`[edx+0x294]`; engine `CEngineClient` vtable `0x10359DEC`, `0x1005E3A0`) -> client `CHLClient` (`0x1056441C`) slot 78 `SetBlurFade(float)` `0x100ABB60` -> `TerrorClientMode` (`0x105BD4FC`) slot 48 `0x1026F4F0`, `+0x90`, drawn by its slot 37 `0x10276460` through `0x101C7FE0` (`dev/fade_blur`). Which window types blur: at `0x1000D35D` `cmp eax, 15h / ja / movzx ecx, byte ptr [eax+0x1000D4CC] / jmp [ecx*4+0x1000D4C4]` on the active window's type (`0x1000A460`); the byte table `0x1000D4CC` (in `.text`) is 0 (no blur) for 0, 3 AudioVideo and 19-21, 1 for the rest. The mod sets entry 3 to 1 while the page is its own (`install_page_blur`). Seen working in game 2026-10-17, with the dark band (slot 88 below) |
| going back | as the Video page's "Back": `0x1000E950(0x73 KEY_XBUTTON_B, user)` (cdecl; the user from `0x10009B30`), then the page's slot 121 with that code |

## The infection's effect under the HUD

The infection's shader, and the stamina's vignette, can go under the game's HUD (settings `[infection]` /
`[stamina]` `cover_game_hud = false`): it then runs from the game's own rendering, just before the HUD is drawn,
instead of at `Present` (see `overlay.cpp`). Both in the same pass, the stamina's first.

| What | Where / how |
|---|---|
| where the HUD is drawn | `client.dll` `CTerrorViewRender` (RTTI), vtable `0x105C1424` (also `CCSViewRender` `0x105A45C4`, `CViewRender` `0x1058CA6C`). Slot 6 `0x101D6D80` renders a view: the VPROF scope "2D Client Rendering", `render->Push2DView` (`IVRenderView` `[0x10735E48]` slot 42), slot 37, then `whatToDraw & 2` and the "VGui_DrawHud" scope |
| the hook | slot 37 `Render2DEffectsPreHUD(const CViewSetup&)`, thiscall `ret 4`: `CViewRender`'s is `ret 4` (`0x101CA100`), `CCSViewRender`'s `0x1022ECD0` (`55 8B EC 5D E9`) jumps to `0x1022E5D0` (night vision). The mod writes its function into the slot (the vtable is read-only data: `VirtualProtect`) and puts it back when it unloads |
| the render thread | `materialsystem.dll` "VMaterialSystem080" slot 96 `GetRenderContext` `0x1000D960` (`56 57 8B F9 B9`: the thread's context, a reference added; the client's own use: `0x1022E763`, then slot 2 `BeginRender`, slot 3 `EndRender`, slot 1 `Release`). The context's slot 142 `GetCallQueue`: `CMatQueuedRenderContext` (vtable `0x1009BF9C`) `0x10023CD0` `lea eax, [ecx+2A4h]` (its `CCallQueueExternal`, vtable `0x1009A9F8`, written at `0x10014336`), `CMatRenderContext` (`0x1009EB6C`) `0x10022990` `xor eax, eax`. The queue's slot 0 `QueueFunctorInternal(CFunctor*)` `0x10010600` (`55 8B EC 56 8B 75 08 8B 06 8B 10`: AddRef, into the context's `CCallQueue`, Release) |
| a functor | `CMemberFunctor1<...>` vtable `0x1009C3B4`: 0 AddRef (`inc [ecx+0Ch]`), 1 Release, 2 the deleting destructor, 3 `operator()`. The mod's is a static object with that layout whose AddRef / Release do nothing |
| the pass | on the render thread (or right away without queued rendering): the current render target 0 copied, the shader drawn over it (sRGB writes off, as at Present), the D3D state captured and put back with a state block of its own. It uses the infection and the stamina the previous frame's HUD set (the HUD sets it at Present, after the game has drawn) |

## Stamina

Server side, in the GameFrame hook, for each living survivor (see `src/game/stamina.cpp`). All of it read out of
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
| less damage | `CBaseEntity::TakeDamage(const CTakeDamageInfo&)` `0x10055C70` (thiscall, `ret 4`, prologue `55 8B EC 83 EC 6C 57 8B F9 8B 0D`; warns "CBaseEntity::TakeDamage:  with inputInfo.GetDamageForce() == vec3_origin"), hooked (since [melee], by `src/game/melee.cpp`, which asks the stamina for its factor). `CTakeDamageInfo` is 0x60 bytes (its copy constructor `0x10010140`): `m_hInflictor` +0x30, `m_hAttacker` +0x34 (`GetAttacker` `0x1003D4B0`), `m_hWeapon` +0x38, `m_flDamage` +0x3C, `m_bitsDamageType` +0x48. Seen: a melee hit on a common is weapon and inflictor `weapon_melee`, type `0x80200004`, 50 damage (then a 0 one); exhausted it became 25 and the common took two hits |

Exhausted, the wait for the next swing or shove is stretched (seen: 0.99 s became 1.65 s at `swing_speed = 60`). The
stamina reaches the player as `@sta@ <per mille, -1: none> <1: exhausted>` (at most ten a second while it changes), or
straight into the host's session. The HUD draws the bar; postfx runs a second, small pixel shader (1512 bytes) before
the infection's: the edges darken and lose colour from half down, and while exhausted close in, blur and pulse.

## Melee ([melee], No More Room in Hell 2's)

See `src/game/melee.cpp` and `src/inv/melee.*`; the NMRiH2 side is in `docs/nmrih2-melee-notes.md`. Seen working in the
game on 2026-10-19 (a listen server and the dedicated server).

| What | Where / how |
|---|---|
| the swing (server) | `CTerrorMeleeWeapon` primary vtable `0x106D611C` (448 slots; RTTI `.?AVCTerrorMeleeWeapon@@`), slot 282 `PrimaryAttack` `0x103EC3B0` (void thiscall, prologue `56 57 8B F1 E8 .. .. .. .. 8B F8 85 FF 0F 84 .. .. .. .. 8B 87 D8 33 00 00`; "melee attack failed - !player->CanAttack()" / "!IsWeaponVisible()"). It calls `0x103EC080`, the swing's start: `m_flNextPrimaryAttack` = curtime + the refire (`0x1004C810`), the secondary time too. Slot 281 is `0x103D0930`, 283 `0x103EB1F0` (the shove). Hooked: after the original, the wait moved on = a swing started. Seen: a fireaxe's wait is 0.92 s (1.39 at `heavy_recovery = 150`), a katana's 1.09 at 150 |
| the buttons of the swing | `PrimaryAttack` runs while the player's user command is processed, so `CBasePlayer::m_nButtons` +0x1CB4 holds that command's buttons. `CUserCmd::buttons` goes to the server as 32 bits: bits 28 (winding up) and 29 (heavy), which the game does not use, arrive intact (seen) |
| the user command's buttons (client) | `CInput` (vtable `0x1057575C`; RTTI `.?AVCInput@@`) slot 2 `GetButtonBits(bool reset)` `0x10131570` (thiscall, `ret 4`, prologue `55 8B EC 83 EC 08 56 8B F1 8B 0D`): the kbuttons (+attack ...) as `IN_*` bits. Slot 3 `CreateMove` `0x101341F0` stores `GetButtonBits(1)` (`push 1; call [edx+8]` at `0x10134547`) in `CUserCmd::buttons` (+0x24); the prediction and the server both use that command. Hooked |
| a melee weapon in the hands (client) | `C_TerrorMeleeWeapon`'s primary vtable `0x10609BFC` (the one of its 6 whose complete object locator has offset 0; 440 slots); the client's active weapon `m_hActiveWeapon` +0x1084 |
| the weapon's wait (client) | `m_flNextPrimaryAttack` +0x960 (RecvProp at `0x10015974`: `push 0x960 / push "m_flNextPrimaryAttack"`; datamap entry `0x106BCBFC`): moves on when the client predicts the swing. The server's longer wait after a heavy swing reaches the client a round trip later as a prediction correction |
| the client's clock | `C_BaseCombatWeapon::ReloadOrSwitchWeapons` `0x100178B0` + 0x72: `mov eax, [0x106C49A8]` (the client's `CGlobalVars*`), `movss xmm0, [eax+0xC]` (curtime), `comiss xmm0, [esi+0x960]` |
| the head | `CBaseCombatCharacter::m_LastHitGroup` +0x1838 (datamap at `0x100449B3`: `mov [..], "m_LastHitGroup" / mov [..], 0x1838`), `HITGROUP_HEAD` 1, set by the swing's trace attack before the damage. Read only of the infected team (3): commons, the witch, the specials. `m_iHealth` +0xEC (SendProp at `0x1016D343`) for the log |
| the viewmodel (client) | `C_TerrorViewModel` primary vtable `0x105C1054` (230 slots; RTTI `.?AVC_TerrorViewModel@@`; `C_BaseViewModel`'s is `0x1053FADC`, whose own virtuals start at slot 219), slot 222 `CalcViewModelView(owner, const Vector& eye_position, const QAngle& eye_angles)` `0x10288F40` (thiscall, `ret 0xC`, prologue `55 8B EC 83 EC 48 A1 .. .. .. .. 33 C5 89 45 FC 8B 45 10`; asserts with "terrorviewmodel.cpp"): copies the eye position and angles, adds the bob and lag (`0x102880D0`, `0x10288BF0`, vtable +0x374), then `SetLocalOrigin` `0x10042C00` and `SetLocalAngles` `0x10042CE0`. Hooked: the wind-up pose goes into the eye position and angles it is given; the camera is not moved |
| slower while winding up | `CBasePlayer::m_flLaggedMovementValue` +0x20D0, as the stamina's (see "Stamina"): the stamina's tick multiplies `melee_move_scale` into its own; without `[stamina]` `melee_frame` sets it, on the ground only |
| the damage | the `TakeDamage` hook above. Seen: a heavy swing's hit 0.2 - 0.3 s after it started; one light swing into a crowd hit four commons within 0.1 s; each target hit gets a few more calls of 0 damage after the first (-1 for one that is dying) |

| the shove (server) | `CTerrorMeleeWeapon` slot 283 `SecondaryAttack` `0x103EB1F0` (prologue `56 57 8B F1 E8 .. .. .. .. 8B F8 85 FF 0F 84 .. .. .. .. 8B 06 8B 90 7C`): calls slot 435 (`0x103F06F0`, "StartSwing": `z_gun_swing_interval` 0.7, `z_gun_swing_duration` 0.2, `z_gun_range` 75), which sets both attack times. Hooked like `PrimaryAttack` for the heavy shove |
| staggering a player | `CTerrorPlayer::OnStaggered(CBaseEntity* source, const Vector* from)` `0x1032CE80` (thiscall, `ret 8`, not virtual; prologue `53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 83 EC 48 56 57 8B F1 E8 .. .. .. .. 84 C0`): what the VScript `Stagger` (`0x1032D730`) wraps; away from `from`; nothing for a ghost or the incapacitated; breaks pins. The game's shove of a special is `OnShovedBySurvivor` `0x1032BB00` |
| staggering a common | `Infected` (primary vtable `0x106D9DB4`) slot 82 `MyInfectedPointer` (`0x10299380`), its `INextBot` part at +0x1A40 (vtable `0x106D9C6C`) slot 28 `OnShoved(CBaseEntity* pusher)` `0x10298FE0` (thiscall, `ret 4`; prologue `55 8B EC 56 57 8B F9 8B 07 8B 50 04 FF D2 8B F0 85 F6 74 1F 53 8B 5D 08 8B 06 8B 50 70`): it stumbles away from the pusher's centre, as the melee weapon's collateral shove does (`0x103EBCA0`). Byte +0x1D94 set to 1 for the call: push only (else a shove from behind kills a wandering common: `0x10400600`) |
| a common's full health | `CBaseEntity::m_iMaxHealth` +0xE8 (SendProp at `0x102FEEA7`): `z_health` (50), set by `Infected::Spawn` `0x10402450` |
| the view's kick | `CBasePlayer::ViewPunch(const QAngle&)` `0x1007CD30` (thiscall, `ret 4`; prologue `55 8B EC A1 .. .. .. .. 83 EC 0C 83 78 30 00 56 8B F1 0F 85`): `m_vecPunchAngleVel` (+0x1AC0; `m_Local` +0x1A44, `m_vecPunchAngle` +0x1AB4) += it * 20; nothing while `sv_suppress_viewpunch` |
| a swing into the world | `CBaseCombatWeapon::WeaponSound(int type, float time)` `0x1004BDA0` (slot 301, not overridden by the melee weapon; prologue `55 8B EC 81 EC CC 00 00 00 A1 .. .. .. .. 33 C5 89 45 FC 53 8B 5D 08 56 8B F1 8B 06 8B 90 3C 05 00 00`). The script's sound names at `0x107A1878`: melee_miss 8, melee_hit 9, melee_hit_world 10. The swing's trace (`0x103EB2E0`, from the driver `0x103EBDC0`) plays 10 at `0x103EBD53` when what it hit is not a combat character (the world, a prop, a door), once per entity per swing. No game event is fired. Hooked; the floor and props are told apart by a world-only trace ahead |
| the world-only trace | `UTIL_TraceLine(start, end, mask, filter, trace_t*)` `0x100101E0` (cdecl; it calls `EngineTraceServer003` slot 5 TraceRay, the pointer at `0x107F76BC`). `CTraceFilterWorldOnly` is an object of one vtable pointer, `0x1068E3AC` (`xor al,al; ret 8` / `mov eax,1; ret`). `Ray_t` is 0x50 bytes (start, delta, start offset, extents, a world-axis-transform pointer +0x40, is_ray +0x44, is_swept +0x45); `trace_t` 0x54 (normal +0x18, fraction +0x2C, startsolid +0x37, hitgroup +0x44, m_pEnt +0x4C). `MASK_SOLID` 0x0200400B, the melee swing's `MASK_SHOT_HULL` 0x4600400B |
| the swing's choice | `primaryattacks` of `scripts/melee/<kind>.txt` (parsed by `0x103D5780`; one entry by `0x103D42C0`): the `CMeleeWeaponInfo` (database `0x10868F08`, looked up by `0x103D5FF0(handle)`, the weapon's `m_hMeleeWeaponInfo` +0x17DC) has them at +0xC20 (count +0xC2C), 0x30 bytes each: +0x04 third-person activity, +0x08 its idle one, +0x0C startdir (N 0, S 1, E 2, W 3, NE 4, SE 5, SW 6, NW 7), +0x14 / +0x18 start / end time, +0x1C duration. Chosen by `0x103EAF30` (the client's `0x1030DA10`; thiscall (info), `ret 4`) from `m_iNextPrimaryAttackIndex` (server +0x17F8, client +0xD08), reset to 0 after `melee_combo_reset_time` (1.2 s) since `m_flLastAttackTime` (+0x1828, client +0xD38). Both hooked: a heavy swing takes the weapon's swing from above. The scripts' `strongattacks` are never read (no such string in either DLL) |
| the gesture | `CTerrorPlayer::DoAnimationEvent(int event, int data)` slot 510 `0x102FBB30` (thiscall, `ret 8`): event 66 plays the activity `data` in gesture slot 0 when free, networked through `m_NetGestureSequence` / `Activity` / `StartTime` (+0x2ADC, +0x2AF8, +0x2B14). The swing itself is event 0x28 (40); shoves 0x22 - 0x25. Players' animation overlays are not networked, only these |
| the throw | `Weapon_Drop` (`0x10170AC0`) ignores the velocity it is given (a toss at the feet); the weapon's `Teleport` (slot 118, `0x10031090` -> `CBaseEntity::Teleport` `0x10066940`) moves its physics object with a velocity: seen flying about 880 units a second |
| a broken weapon | `Weapon_Drop` switches to the next best weapon itself (`SwitchToNextBestWeapon` `0x100462F0`), then `UTIL_Remove`. Nothing gives a pistol back for an empty secondary slot (only incapacitation does, `0x1032EB40`) |

The server's rules reach a player's client as `@mel@ <heavy_hold ms> <queue ms> <heavy_shove_hold ms> <overhead>`
(the weapon in the hands' hold; `0 0 0 0`: none), when they change and once the player is known to run the mod; the
host's own client half reads the same line. A hit's impact goes to the attacker as `@mhi@ <1: heavy>`. The client's
`m_flNextSecondaryAttack` is +0x964 (`ReloadOrSwitchWeapons` compares both). The wear of a melee weapon in the world is
kept by its handle; a stored one's is the inventory's (the save file's last field of a weapon line).

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
| its own pack | `models/inv_mod/rounds_<kind>.mdl` when the game finds it (`filesystem_stdio.dll` "VBaseFileSystem012", the IBaseFileSystem of its one `CFileSystem_Stdio` at `0x1005260C`, vtable `0x10047DFC`; slot 10 `FileExists(name, pathID)` `0x1000E340`, thiscall, `ret 8`, path "GAME"), untinted. The addon (`tools/make_rounds_addon.py`) holds, per type, a copy of `w_eq_incendiary_ammopack` (from `update/pak01_dir.vpk`: MDL v49, one bone, 736 triangles) with two strings changed in place, its name (`studiohdr_t` +12, 64 bytes) and its one texture's path (`models/props/terror/incendiary_ammopack`, 39 characters, made `models/inv_mod/rounds_<kind>`), so the `.vvd` / `.vtx` checksums still match; and a VTF 7.2 (DXT5 1024x512, 11 mips, a 16x8 DXT1 thumbnail, flag 0x2000) of `assets/textures` with the game pack's alpha (the envmap mask). A single-file VPK v1 (archive index 0x7FFF). The game lists a new addon in `addonlist.txt` as "1" when it starts. Seen: `rounds_autoshotgun.mdl` dropped and precached |
| mounting the addons | every `.vpk` in the `addons` folder next to the DLL (`inventory_mod_rounds.vpk`), when the mod loads (`game::init`): `filesystem_stdio.dll` "VFileSystem018" (its one `CFileSystem_Stdio`, `0x10052608`, vtable `0x10047E44`) slot 107 `AddVPKFile(name, addType)` `0x1000A110` (thiscall, `ret 8`; prologue `55 8B EC 81 EC 0C 01 00 00 A1`): the name made absolute, one already in its VPK list (`+0x1CC`, count `+0x1D8`) left alone, else a `CPackedStore` (0x2AC4 bytes) opened for it, dropped when empty, put at the tail (1) or the head (0). `AddSearchPath`'s internal function (`0x1000C630`, slot 7 `AddSearchPath` `0x1000C950` calls it) calls slot 107 for a folder's `pakNN_dir.vpk`. Slot 108 `RemoveVPKFile(name)` `0x100068B0` (`ret 4`) takes one out; the mod does not (models read from it stay in use when it unloads) |
| its keyvalues | what a map's own pile has (c2m1_highway's entity lump): `"solid" "6"`, `"spawnflags" "2"` (1 lets it be knocked about), `"disableshadows" "1"`, `"angles"`, and `"origin"` set before it spawns. The crash in vphysics.dll +0xF3B1C (an IVP function, `0x100F3B00`, given a null compact edge; its caller is lost, the crash handler writes over the stack) came five times: 03:42 (a spawner from `ent_create`), 15:23 and 16:23 (dropped piles), and 2026-10-17 05:59 two seconds after five packs were put on the map, with "solid" set. The cause: the piles were spawned at the map's origin and moved afterwards (Teleport, then DropToFloor), as `ent_create` moves what it spawns. Their physics object is a static one made at spawn, and a static IVP object is not meant to move. So the floor is found first with a throwaway `info_target` (no physics: DropToFloor, read `m_vecOrigin`, removed; `on_floor`) and the pile is made there ("origin" before `DispatchSpawn`, never moved; the pack model's origin is at its bottom, hull z -0.25 .. 8.56). Rounds dropped within 40 units of a pile of their type are added to it; a new pile is put 24 units aside of the others, not into them |
| finite map piles | `[ammo_piles]`: the same Use hook, for a pile that is not the mod's. Its first +use by a survivor rolls a type of `kinds` and `min..max` magazines (`inv::ammo_magazine`) and tints it ("rendercolor"); each +use adds to `m_iAmmo` as much as fits (the type's most, the weight limit for the player whose inventory it is), the rest stays, empty is `UTIL_Remove`. A bot takes only when one of its weapons has that `m_iPrimaryAmmoType` (+0x140C). What is left is kept per pile pointer with its EHANDLE (`gEntList` index \| serial << 12, found by a scan of the list: an entity made again at the same address is a new pile); `game_time()` going back (a new map) clears it |
| the map's packs | `[ammo_piles] replace_map_piles` / `extra`: 1.5 s after the event `round_start` (`on_game_event` sets the time, `GameFrame` runs `place_map_packs` on the main thread), once a round (not when a pile named `inv_maprounds_` or `inv_rounds_` is on the map already). Every `weapon_ammo_spawn` (position `m_vecOrigin` +0x388, rotation `m_angRotation` +0x394: the SendProps push `0x394` / "m_angRotation" at `0x100595AD`) is removed and a pack of a rolled type put where it stood; `extra` packs go 20 units off a random `weapon_*_spawn` (not within 700 units of a survivor, 300 of another extra, 150 of a pile). Their targetname is `inv_maprounds_<kind>_<rounds>`: a bot takes from them (rounds for a gun it has), not from what players dropped |
| map guns, part of a magazine | `[map_guns]`: at the start of `bump_weapon` (slot 426, the pick-up, which a spawner's fresh copy goes through too: GiveNamedItem) and of `CTerrorGun::Use`, a survivor's gun with an ammo kind (not the chainsaw, the pistols, melee) whose EHANDLE is not known gets `m_iClip1` (+0x1414, full on such a gun) cut to `min_clip..max_clip` %, then is known. Known: held by any survivor (their `m_hMyWeapons`, every 100 ms in cap_reserves), dropped from a box by the mod, given by the mod (`g_giving`), or cut once. `round_start`, or `game_time()` going back by more than a second (a new map), clears it. Not any step back: while a player's command runs (the pick-up) `curtime` is that player's tick base, a little behind the frame's clock that GameFrame sees; clearing on that cut one auto shotgun again on every swap (5 -> 2 -> 1 -> 0 in the log, 2026-10-17). The finite piles' memory had the same check |
| map guns, the reserve | a map's weapon spawner fills the reserve of its gun's ammo type after it has handed the gun out, outside the pick-up (seen: `bump_weapon` returned with the reserve unchanged, then 72 shotgun shells, the most), and also when the player already has that gun and gets none. So `spawn_use` reads the player's `m_iAmmo[32]` (+0x1874) before the original Use and, when it returns, takes anything above that plus the map gun's `min_reserve..max_reserve` magazines back (0 when no new gun came). The pick-up inside it (`bump_weapon`, `CTerrorGun::Use`) sets the bonus on that one (`g_spawn_reserve`); a loose gun's pick-up gets its own limit. A map gun's `m_iExtraPrimaryAmmo` (+0x14F4) is zeroed (what a box would add) |
| single-use spawners | `[map_guns] spawners_once`: in `CWeaponSpawn::Use`'s hook, for a survivor and a spawner whose weapon id (slot 240, its name by the weapon-id table) is a gun or melee weapon (not an inventory item): the player's `m_hMyWeapons` before and after the original Use, and the stored count of that item; a new handle, or one more stored, is "given", and the spawner is `UTIL_Remove`d (its count, and an endless spawner, do not matter) |
| the game moves the reserve with the guns | a gun dropped (server.dll `0x102476B0`, when no other carried gun takes its ammo type) gets the player's reserve of its type: `GetAmmoCount(type)` `0x10046DF0`, the gun's `SetExtraAmmo` `0x102455E0` (writes `m_iExtraPrimaryAmmo` +0x14F4, with the network-change marking; found as the only writer of +0x14F4 besides the "extraAmmo" keyvalue at `0x102557C5`), then `SetAmmoCount(0, type)` `0x10046DC0` (thiscall `(count, type)`, `ret 8`). A gun picked up with rounds of its own has them zeroed and the player's count SET to them (not added): the slot-426 pick-up `0x1031D080` at `0x1031D186` and `0x1031D34C`, `CTerrorGun::Use` at `0x103E87DB`, also `0x10246E80`. So the game's swap (no free weapon box) lost the dropped gun's type of rounds (seen in the log 2026-10-17: a Sniper Rifle swapped for an auto shotgun, the auto shotgun for a chrome shotgun). `KeepRounds` lives through the game's pick-up / swap (`bump_weapon`, `gun_use`) and the mod's `do_drop_weapon`: `m_iAmmo[32]` before; afterwards no type is lower, a picked gun's own rounds (its `m_iExtraPrimaryAmmo` was > 0 and is 0 now) are added to what the player had (up to the type's most), and a gun no longer in `m_hMyWeapons` whose type was put back gets `m_iExtraPrimaryAmmo` 0 |
| the weight cap | ten times a second (GameFrame) each survivor with an inventory here (the host, the served players) has their reserve compared with the last look; a rise that takes the load past the limit is taken back down to what fits (an ammo pile, a gun picked up) |

`IServerTools` slot 11 is `SetKeyValue(ent, key, const Vector&)` (`0x104CF140`, to the entity's KeyValue slot 32
`0x1006D4D0`, which formats `"%f %f %f"`); the text one is slot 13 (`0x104CF100`, entity slot 34 `0x10070670`, which
compares "rendercolor" ...). MSVC puts overloads in the vtable last first. The mod used 11 until 2026-10-16: the pile's
model and targetname became numbers ("17757245829489361000.000000 ..."), so it was the ERROR model and was not
recognised; a stored melee weapon dropped from a box got the same garbled `melee_script_name`.

## A death, a new game

| What | Where / how |
|---|---|
| the map's name | `CGlobalVars::mapname` +0x3C (a `string_t`, the `const char*`): server.dll reads it so at `0x1009B903` (`mov eax, [0x107F7690]; mov eax, [eax+0x3C]`, "" for null) for `"maps/%s_commentary.txt"`. `0x107F7690` is the `gpGlobals` that `GetGlobalVars` returns (see the game's clock) |
| a round starting | the events `map_transition` (the survivors reached the saferoom) and `round_start`, in the FireEvent hook. `round_start` after a `map_transition`: the next chapter, the inventories are kept. On the same map as the last `round_start`: the round again (a lost round, a restarted chapter, versus' second round), each inventory gets back its copy (`Inventory::to_text`) from that last `round_start`. Any other map (or the first `round_start` the mod sees): a new game, every inventory (the local one unless it mirrors a dedicated server's, and every player's the server keeps) is emptied, and `g_new_game` (the time) makes `server_player` skip a player's file last written before it |
| a chapter transition, the reserve | the game saves each survivor at the change (`CTerrorPlayer` `0x102B7E20`, thiscall with the target map, "Saving %s for transition at %f %f %f"; bots skip it): the KeyValues of `0x102B6C40` ("userID", "pos", "health" ..., then one `"weapon"` subkey per carried weapon from `0x102B6880`: "weaponID", "weaponSlot", "currentMagazine" (`m_iClip1`), "upgradedAmmoCount", "extraAmmo" = `GetAmmoCount(type)` `0x10046DF0`, the player's reserve of that gun's type). In the next chapter `CTransitioningPlayer::Restore(CTerrorPlayer*)` `0x102B8B60` (thiscall, `ret 4`, nothing returned; `this` holds the KeyValues at +0; called by the takeover of the survivor bot that stood in for the player, `0x102BA070` "[TAKEOVER]: Restoring %s(%d) after transition.") puts it back onto the new player entity: each weapon through `0x102B7970` ("currentMagazine" onto `+0x1414`, "extraAmmo" with `SetAmmoCount` `0x10046DC0`). So `m_iAmmo` of the new entity holds only the types of the guns in the hands; a stored gun's type (its reserve is the player's, see "rounds") was 0. Seen on the dedicated server 2026-10-18 (c2m1 to c2m2). The mod: `map_transition` copies every survivor's `m_iAmmo[32]` into their session (`transition_reserve`, the host's local one or the served player's by SteamID, which outlives the reconnection), and the hook on `0x102B8B60` (prologue `55 8B EC 83 EC 60 53 8B 5D 08 8B 43 28 56 8B F1`) writes back, after the original, the types no carried gun uses (`carries_ammo_type`), up to `inv::ammo_max`, then marks the edict changed and forgets `cap_reserve`'s look. Not yet seen working in game |
| a death | the event `player_death` of a survivor (team 2): its `userid` and the player's `m_vecOrigin` then (a dead player's origin follows the spectator camera afterwards) are queued; the next `GameFrame` (not inside the event) drops the inventory of the host, or of a served player with the mod (an `Activation`): every stored item with `spawn_dropped` (as `do_drop`: create, spawn, Teleport, DropToFloor; loose weapons, a golden-angle spiral within 18 units), every ammo type of `m_iAmmo` > 0 as a pile of dropped rounds (`spawn_pile`, on a ring at least 28 units out and 24 apart: piles in each other crash vphysics), then the inventory is cleared and those reserves are 0 |

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
(`select_weapon` / `use`, `drop_weapon` / `drop`), so it works on a dedicated server unchanged.

Since 2026-10-17 the wheel is drawn with the game's own VGUI surface, in the client scheme's fonts (see "The VGUI
surface" below); ImGui draws the same shapes only when that is missing (a check failed, or nothing painted for 250 ms).
The pick no longer waits for a drawing: every drawing leaves its layout (middle, dead zone, each segment's end angle and
what it picks) under `g_wheel_mutex`, and the window procedure works out the segment from each `WM_MOUSEMOVE` (and from
the release's own coordinates), so letting go takes what is under the cursor at that moment. A new segment under the
cursor plays `ui/menu_focus.wav` through the surface's `PlaySound` (settings: `wheel_sounds`). The ImGui fallback's
text is the toolkit's 12 px fonts scaled by height / 720.

## The VGUI surface (client side)

Found 2026-10-17 with `tools/re/pe_tools.py` (the DLLs carry no RTTI names for these classes); not yet seen in game.

- **The objects.** Each `CreateInterface` registration is `push "<name>"; push <create fn>; mov ecx, <InterfaceReg>;
  call`, and the create function is `mov eax, <global>; ret`. vgui2.dll: `VGUI_Panel009` (registered at `0x100425B0`)
  -> `0x10054324`, a constant object whose vtable is `0x100480CC`; `VGUI_Scheme010` (`0x10042470`) -> `0x100587B0`,
  built by `0x1000F080` (`mov [edi], 0x10046FCC`). vguimatsurface.dll: `VGUI_Surface031` (`0x100BCCF0`) ->
  `0x10128560`, built by `0x10014490` (`mov [esi], 0x100BEF2C`). The mod checks each object's vtable against these.
- **IPanel** (`VPanelWrapper`): 36 `GetName(VPANEL)` (`0x1001EAC0`: the VPanel's own slot 35, `+0x8C`), 41
  `PaintTraverse(VPANEL, bool, bool)` (`0x1001EBE0`, `ret 0xC`: `Client(vpanel)` (slot 65, `+0x104`) then that client
  panel's slot 3). Slot 0 is IBaseInterface's destructor, so these are one past the SDK's numbering. Hooked with
  MinHook: the mod paints after the panel named `FocusOverlayPanel`. engine.dll's `CEngineVGui::Init` makes it at
  `0x1021F9A5` (`new` 0x14C, its constructor `0x1021E710` with the static panel `[esi+0x78]` as parent), then
  `SetZPos(150)` (`push 0x96`): the last of the game's own panels to paint each frame.
- **ISchemeManager** (`CSchemeManager`, vtable `0x10046FCC`): 4 `GetDefaultScheme` (`mov eax, 1`), 5
  `GetScheme(tag)` (`0x1000C520`: compares each scheme's name, its slot 12, with the tag), 8 `GetIScheme(handle)`
  (`0x1000BD90`: bounds check, then the array). **IScheme** (`CScheme`, vtable `0x10047010`, stored by `0x1000E4E0`):
  3 `GetFont(name, proportional)` (`0x1000C810`, `ret 8`), 4 `GetFontName(const HFont&)` (`0x1000C7B0`: "<Unknown
  font>"). The wheel asks `ClientScheme` for `PlayerDisplayName` (14), `HudHintText` (12), `HudAmmoSmall` (12,
  additive, the ammo counter's) and `MessageText` (10), proportional: the scheme gives "Stubble bold" or "Trade Gothic
  Bold" (`resource/TGB.vfont`, a CustomFontFiles entry) by `$WIN32HIDEF`, scaled by height / 480 like the HUD's own.
- **ISurface** (`CMatSystemSurface`, vtable `0x100BEF2C`): 0..4 are IAppSystem's (2 is QueryInterface: it compares
  with "VGUI_Surface031"). Told apart by their argument sizes (`ret N`) and bodies, in the Alien Swarm SDK's order:
  11 `DrawSetColor(r, g, b, a)` (`0x100081E0`: bytes at `+0x24..0x27`, alpha times `+0x10`; 10 is the `Color` one,
  calling 11), 12 `DrawFilledRect` (`ret 0x10`; 14 DrawOutlinedRect calls it four times), 17 `DrawSetTextFont`
  (`+0x294`), 19 `DrawSetTextColor(r, g, b, a)` (18 calls it), 20 `DrawSetTextPos` (`+0x2C`, `+0x30`), 22
  `DrawPrintText(text, length, draw type)` (`ret 0xC`), 25 `DrawGetTextureId(file)` (the texture dictionary's slot 12,
  -1 when none), 27 `DrawSetTextureFile(id, file, filter, reload)`, 29 `DrawSetTexture(id)` (`+0x2A0`), 31
  `DrawTexturedRect`, 34 `CreateNewTextureID(procedural)` (the dictionary's slot 0), 35 `GetScreenSize(&w, &h)`,
  63 `CreateFont` / 64 `SetFontGlyphSet` (`ret 0x24`) / 66 `GetFontTall` / 72 `GetTextSize(font, text, &w, &h)` (the
  font manager's, `0x10015F20`), 75 `PlaySound(file)` (the engine's callback at `+0x102D0`), 87..89 the workspace /
  window bounds and proportional base (`ret 0x10`, `0x10`, 8), 93 / 94 SurfaceGet/SetCursorPos, 95
  `DrawTexturedLine`, 96 `DrawOutlinedCircle` (`ret 0x10`), 97 `DrawTexturedPolyLine`, 98 `DrawTexturedSubRect(x0, y0,
  x1, y1, s0, t0, s1, t1)` (`ret 0x20`), 99 `DrawTexturedPolygon(n, Vertex_t*, clip)` (`ret 0xC`; `Vertex_t` is
  `{x, y, u, v}` floats, clipped by `0x100024C0` against the current panel and drawn with the bound texture). The ring
  segments are convex quads of `vgui/white`, the icons `DrawTexturedSubRect`s of the HUD's own sheets
  (`vgui/hud/iconsheet*`, `vgui/hud/icon_<melee>`), tinted by the colour. Each material gets a texture id of the mod's
  own (`CreateNewTextureID` + `DrawSetTextureFile`), never the one `DrawGetTextureId` finds: that id belongs to a game
  panel that may `DestroyTextureID` it when it goes, and the dictionary can give the slot to another texture.

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

## Unloading a gun

The unload key (`U`, `do_unload`, on the main thread: the pump, or `inv_unload` from a remote client) empties a gun's
magazine into the player's reserve of its ammo type (`inv::split_unload`: up to the type's most and the weight limit;
the rest through `do_drop_ammo` as a pile). The gun is the one on the ground the player looks straight at (the
pick-up's search, `item_in_front`, with a cone of cos 0.9 instead of 0.6, reach 160), or a map's gun spawner there
(whichever is closer), or else the one in the hands. Seen working in game 2026-10-17 (local game, c1m2): a sniper rifle
in the hands (8 rounds out, then 28 after a reload: all of them each time, nothing kept back), a dropped one on the
ground, an auto shotgun in the hands, and a rifle and an SMG spawner. On the dedicated server (2026-10-17, a client with
the mod connected to `srcds -port 27020`; the game itself keeps 27015 open, so a server started on 27015 moved to 27016
and answered nobody): `inv_unload` for a silenced SMG in the hands (32 rounds), a pump shotgun spawner (5 of 8, the
spawner gone) and that shotgun in the hands, the refusal for a pistol shown on the client; the reload let through on
the server and the client together at the first fire.

| What | Where / how |
|---|---|
| `m_iClip1` | `+0x1414` (see "Storing weapons") |
| setting it | `CBaseCombatWeapon::ScriptSetClip1(int)` `0x103F0650` (thiscall, `ret 4`; prologue `55 8B EC 56 57 8B 7D 08 8B F1 39 BE 14 14 00 00`): the VScript `SetClip1`, registered at `0x103F2F1C` (`mov eax, 0x103F0650` beside the names "SetClip1" / "ScriptSetClip1"). It writes `+0x1414` and marks the edict changed (`+0x64` set: `+0x68 \|= 1`, else `edict \|= 0x101` and the change accessor through `0x100ED4B0`), as every writer of `m_iClip1` in server.dll does. The mod writes the field itself when the prologue does not match |
| `m_bInReload` | `+0x144D` (SendProp at `0x1004C5A6`: `push 0x144D / push "m_bInReload"`). A gun in the hands that is reloading is not unloaded: its `FinishReload` would fill the magazine again from the reserve the rounds just went into |
| the reload | `CBaseCombatWeapon::FinishReload` (`0x1004E3F0`): `n = min(GetMaxClip1() - m_iClip1, GetAmmoCount(type))` (`GetMaxClip1` is vtable slot 323, `[vt+0x50C]`; `GetAmmoCount` `0x10046DF0`), `m_iClip1 += n`, `RemoveAmmo(n, type)` (`[player vt+0x44C]`). The one-shell-at-a-time reload (at `0x1004E281`, in `0x1004DD90`) adds 1 at a time. Firing (`0x103E9370`: the "weapon_fire" event, then `m_iClip1 - 1` at `0x103E95DB`) takes 1 off `m_iClip1`. A scan of `.text` for writes of `[reg+0x1414]` finds 57; the ones read (the reloads, firing, the dual pistols' split into `+0x17E4`, half the clip each, `SetClip1`) keep no round aside: the server has no round "in the chamber" |
| special rounds | `m_nUpgradedPrimaryAmmoLoaded` `+0x1788` set to 0 and the upgrade bits 1 / 2 (`m_upgradeBitVec` `+0x17E0`) cleared: they were rounds of the magazine; they come out as plain rounds of the type. The laser sight (4) stays |
| a gun on the ground | its `m_iClip1` only (its own reserve, `m_iExtraPrimaryAmmo` `+0x14F4`, stays with it). It is made known to `[map_guns]` (`know_gun`) so its magazine is not cut or refilled when someone picks it up |
| the automatic reload | the game reloads an empty gun by itself (seen: a sniper rifle unloaded from 8 rounds had 28 again a moment later, the reserve 0). Server: `Reload()` is slot 281 of the guns' vtables, `bool __thiscall`: `CTerrorGun::Reload` `0x103E8860` (prologue `55 8B EC 83 EC 08 53 56 8B F1 E8`; `CTerrorGun` `0x106D5544`, `CRifle_Desert`, `CSniperRifle` ...) and the shotguns' `0x103C2F80` (`55 8B EC 83 EC 08 56 57 8B F1 E8`; `CPumpShotgun` `0x106CC994`, `CShotgun_Chrome`, `CAutoShotgun`, `CShotgun_SPAS`). Both hooked: a gun unloaded in the hands answers false until its owner's `m_nButtons` (`+0x1CB4`) has `IN_ATTACK` 1 or `IN_RELOAD` 0x2000. `DefaultReload(clip1, clip2, activity)` is `0x1004DD90` (`ret 0xC`; sets `m_bInReload`), called by `0x1004E0AA` and `CTerrorGun`'s `0x1024F750` |
| the client's prediction | the client predicts its own weapon: with only the server's hook it started the reload every frame (seen: the gun jittered, no animation). client.dll gets the same rule: `m_nButtons` is client `+0x13A4` (datamap entry `0x106C1664`: type 5, "m_nButtons"), `m_bInReload` `+0x9BD` (RecvProp at `0x10015BF4`). The client's `Reload()` is what its `ReloadOrSwitchWeapons` (`+0x400`, `0x100178B0`) calls: `+0x434`, slot 269 of the primary vtable (complete object locator offset 0; the RTTI tool also lists the secondary ones): `C_TerrorGun` `0x10609034`, `C_SniperRifle`, `C_Rifle_Desert` -> `0x1030C150` (`56 57 8B F1 E8`), `C_PumpShotgun` `0x105FF6FC`, `C_Shotgun_Chrome`, `C_AutoShotgun`, `C_Shotgun_SPAS` -> `0x102F1CF0` (`55 8B EC 51 56 57 8B F1 E8`). Slot 266 (`+0x428`, `0x10018160` / shotguns `0x102F2200`), which `ItemPostFrame` (`0x10017A40`) calls first when `UsesClipsForAmmo1` (`+0x514`), is `CheckReload`: it finishes a running reload (`FinishReload`, `+0x42C`). Hooked by mistake at first, it let the client start its own reload and finish it at the first fire: the client's magazine was full in that frame and it predicted a real shot while the server's was empty (seen: the client's fire body `0x1030C4C0` ran with `m_iClip1` 19; the server reloaded: a "ghost shot" with no bullet). The client marks its gun in the hands when the key is pressed (the pump, before the request goes out). Seen 2026-10-17: no jitter, fire and R reload with the animation, no ghost shot |
| fire on an empty gun | the guns' frame update, the base `CBaseCombatWeapon::ItemPostFrame` (`0x1004D7C0`), sees `IN_ATTACK` with `m_iClip1 <= 0` (`0x1004D9F8`) and calls `HandleFireOnEmpty` (`+0x450`, `0x1004ABD0`): while `m_bFireOnEmpty` (datamap, boolean, `+0x144E`; client `+0x9BE`, its datamap entry at `0x106BCECC`) is false it dry-fires and sets it, then calls `ReloadOrSwitchWeapons` (`+0x434`, `0x1004D590`: `m_iClip1` 0 and both attack times passed -> `Reload` `+0x464`; seen: `Reload` called from server.dll+0x4D714). Both sides hook it (server `0x1004ABD0`, `56 8B F1 80 BE 4E 14 00 00 00`; client `0x100156D0`, `56 8B F1 80 BE BE 09 00 00 00`; `void __thiscall`) and set the flag first when the magazine is empty and the owner has rounds of its type, so fire on an unloaded gun reloads at once. (Two earlier tries did nothing: hooks on the fire body `0x103E9370` / client `0x1030C4C0` and on `CWeaponCSBase::ItemPostFrame` `0x102502A0` are not on the guns' path for this; client `C_WeaponCSBase::ItemPostFrame` is `0x10232300`) |
| a map's spawner | `weapon_rifle_spawn` ...: an entity whose slot 106 is `CWeaponSpawn::Use` (`0x1029DCB0`), its gun by slot 240 and `0x1024EB70` (see "Map item spawners"), not the ammo pile (its own Use). One gun comes out: made with "origin" and "angles" (`m_angRotation` `+0x394`) before `DispatchSpawn`, its magazine cut by `[map_guns]`, known; the spawner's count (`+0x1444`) goes down by one, and at 1 (or with `spawners_once`) it is `UTIL_Remove`d and the gun lies where it stood; one that stays (endless, or more) gets the gun 24 units aside on the floor (`on_floor`). Then that gun is unloaded. Seen: a rifle spawner (count 10, spawners_once) gave 17 of 50 rounds, no crash |
| the sound | the game's own pick-up sound on the player, for everyone near to hear: the soundscript `Player.PickupWeapon` (`scripts/game_sounds.txt`: `CHAN_WEAPON`, `SNDLVL_NORM`, volume 0.7, `)items/itempickup.wav`) through VScript `EmitSoundOn` (`emit_sound_on`, the `script` command's handler as the infection's coughs use it). A pile of rounds taken (`take_pile`) plays it too. Seen working in game 2026-10-17 (local game): heard on unload and on an ammo pack taken |
| which guns | `inv::unloadable`: a gun with an ammo kind but the M60 (its reserve's most is 0) and the chainsaw. The gun's `m_iPrimaryAmmoType` (`+0x140C`) must be that kind's index |

The game's default `config.cfg` binds U to `messagemode2` (team chat); while the player is a living survivor the mod's
window procedure swallows its own keys, so the mod's U wins over the game's then.
