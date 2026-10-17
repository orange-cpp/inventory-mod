#pragma once
// What the mod knows of the game's binaries: build stamps, RVAs, vtable slots, struct offsets and the signatures of the
// functions it calls or hooks.  Only declarations live here; why each one is used, and the narrative of how it was
// found, stays next to the code that uses it (game.cpp), and every one is recorded in docs/game-notes.md.
//
// Everything was read out of build 2.2.4.3 with tools/re/pe_tools.py.  RVAs are from the module's base (the addresses in
// the comments are at the preferred base 0x10000000); game::init refuses to run on any other server.dll.  Each function
// is checked by its prologue (mem::find_pattern) before it is used.
//
//   IServerTools ("VSERVERTOOLS001", server.dll's CreateInterface), vtable slots
//       7 FirstEntity     (its body loads &gEntList: `6A 00 B9 <gEntList> E8`)
//      10 GetKeyValue(ent, field, buf, len)     13 SetKeyValue(ent, field, const char*)
//         (MSVC puts overloads in the vtable last first: 11 is SetKeyValue(ent, field, const Vector&), 0x104CF140,
//         which calls the entity's KeyValue(field, Vector) (slot 32, 0x1006D4D0: sprintf "%f %f %f"); 12 the float
//         one; 13, 0x104CF100, the entity's KeyValue(field, const char*) (slot 34, 0x10070670: "rendercolor" ...).
//         Slot 11 was used until 2026-10-16: every key got a string's bytes as three floats, "1775...e19 ...")
//      14 CreateEntityByName(classname)         15 DispatchSpawn(ent)
//      16 DestroyEntityByHammerId, whose body is `... 50 E8 <UTIL_Remove> 83 C4 04 B0 01`
//   gEntList: the entity list.  Entry i is at gEntList + 4 + 16 * i = {entity, serial, prev, next}; an EHANDLE is
//       index | serial << 12 (index 12 bits).  Entity index 1 is the first player slot: the host on a listen server.
//   CTerrorPlayer: primary vtable at server.dll + 0x6973B4 (587 slots), slot 431 GiveNamedItem(name, 0, 0, 0) (the
//       `give` console command calls it exactly so), 140 EyePosition(Vector* out), 141 EyeAngles().
//   Netprops of CBaseCombatCharacter (SendProp definitions in server.dll): m_hMyWeapons at +0x18F4, 56 EHANDLEs;
//       m_hActiveWeapon at +0x19D4.
//   UTIL_Remove(CBaseEntity*) is cdecl.
//   Spawning an item the way the `ent_create` console command does (callback at server.dll + 0x57690): create,
//       entity vtable slot 26 Precache(), DispatchSpawn, slot 118 Teleport(&pos, 0, 0), then UTIL_DropToFloor(ent,
//       0x200400B, 0) (cdecl; found by its call inside ent_create).

#include <cstddef>
#include <cstdint>

namespace game::sdk
{

    // ---- the builds --------------------------------------------------------------------------------------------------

    constexpr std::uint32_t k_server_timestamp = 0x6A440E53; // server.dll's PE timestamp
    constexpr std::uint32_t k_engine_timestamp = 0x6A440ED6; // engine.dll's

    // ---- types --------------------------------------------------------------------------------------------------------

    struct Vec3
    {
        float x, y, z;
    };

    // An entry of gEntList (see the top of this file).
    struct EntInfo
    {
        void* entity;
        std::uint32_t serial;
        void* prev;
        void* next;
    };

    constexpr std::uint32_t k_invalid_handle = 0xFFFFFFFFu;
    constexpr int k_team_survivors = 2; // m_iTeamNum (3: the infected)

    // CBasePlayer::m_nButtons bits
    constexpr int k_in_attack = 0x1, k_in_reload = 0x2000;
    constexpr int k_in_attack2 = 1 << 11;

    // ---- server.dll: IServerTools and the entity helpers ------------------------------------------------------------

    constexpr int k_vt_tools_set_key_value = 13; // IServerTools::SetKeyValue(entity, field, const char*)
    constexpr std::uintptr_t k_rva_ent_create = 0x57690; // the `ent_create` command's callback (UTIL_DropToFloor's call)

    using CreateInterfaceFn = void*(__cdecl*)(const char* name, int* return_code);
    using GetKeyValueFn = bool(__thiscall*)(void* tools, void* entity, const char* field, char* out, int length);
    using SetKeyValueFn = bool(__thiscall*)(void* tools, void* entity, const char* field, const char* value);
    using CreateEntityFn = void*(__thiscall*)(void* tools, const char* classname);
    using DispatchSpawnFn = void(__thiscall*)(void* tools, void* entity);
    using RemoveFn = void(__cdecl*)(void* entity);
    using DropToFloorFn = int(__cdecl*)(void* entity, unsigned mask, void* ignore);

    // server.dll CBaseEntity::PrecacheModel (0x1006B690, cdecl int (const char*): engine slot 5 PrecacheModel(name,
    // true), with a warning when it is late)
    constexpr std::uintptr_t k_rva_precache_model = 0x6B690;
    using PrecacheModelFn = int(__cdecl*)(const char* name);

    // ---- server.dll: CBaseEntity -------------------------------------------------------------------------------------

    constexpr int k_vt_precache = 26;
    constexpr int k_vt_entity_use = 106; // CBaseEntity::Use(activator, caller, type, value)
    constexpr int k_vt_teleport = 118;
    using PrecacheFn = void(__thiscall*)(void* entity);
    using TeleportFn = void(__thiscall*)(void* entity, const Vec3* position, const Vec3* angles, const Vec3* velocity);

    constexpr std::uintptr_t k_off_edict = 0x28;
    constexpr std::uintptr_t k_off_team = 0x238; // m_iTeamNum
    constexpr std::uintptr_t k_off_entity_name = 0x140; // m_iName (targetname)
    constexpr std::uintptr_t k_off_model_name = 0x24C; // m_ModelName (datamap, FIELD_MODELNAME: a pointer to the text)
    constexpr std::uintptr_t k_off_abs_velocity = 0x260; // m_vecAbsVelocity (datamap)
    constexpr std::uintptr_t k_off_velocity = 0x2D8; // m_vecVelocity
    constexpr std::uintptr_t k_off_origin = 0x388; // m_vecOrigin (the SendProps: push 0x388 / push 0xC)
    constexpr std::uintptr_t k_off_angles = 0x394; // m_angRotation (SendProp: push 0x394 / "m_angRotation")
    constexpr std::uintptr_t k_off_spawner_angles = 0x394; // the same field, read off a spawner

    // CBaseEntity::TakeDamage(const CTakeDamageInfo&) (0x10055C70, thiscall, ret 4) and CTakeDamageInfo (0x60 bytes)
    constexpr std::uintptr_t k_rva_take_damage = 0x55C70;
    constexpr std::size_t k_damage_info_size = 0x60;
    constexpr std::uintptr_t k_off_damage_inflictor = 0x30, k_off_damage_attacker = 0x34, k_off_damage_weapon = 0x38,
                             k_off_damage = 0x3C;
    using TakeDamageFn = int(__fastcall*)(void* self, void* edx, const void* info);

    // ---- server.dll: CBasePlayer, CTerrorPlayer ----------------------------------------------------------------------

    constexpr std::uintptr_t k_rva_player_vtable = 0x6973B4; // CTerrorPlayer's primary vtable (587 slots)
    constexpr int k_vt_eye_position = 140;
    constexpr int k_vt_eye_angles = 141;
    constexpr int k_vt_weapon_drop = 283; // Weapon_Drop(weapon, target, velocity): the VScript DropItem calls it so (0x102FBCD0)
    constexpr int k_vt_cannot_drop = 401; // checked first by DropItem: non-zero means no
    constexpr int k_vt_use_pickup = 426; // the +use pick-up of a weapon in the world
    constexpr int k_vt_select_item = 429; // what the `use <weapon>` console command calls (server.dll 0x1008ED30)
    constexpr int k_vt_give_named_item = 431;
    constexpr int k_vt_commit_suicide = 476; // CommitSuicide(bool explode, bool force), 0x10174210

    constexpr std::uintptr_t k_off_life_state = 0xF0; // CBasePlayer::m_lifeState (0: alive)
    constexpr std::uintptr_t k_off_flags = 0x13C; // CBasePlayer::m_fFlags (FL_ONGROUND 1)
    constexpr std::uintptr_t k_off_player_ammo = 0x1874; // CBasePlayer::m_iAmmo[32]: the reserve of each ammo type
    constexpr std::uintptr_t k_off_my_weapons = 0x18F4; // CBaseCombatCharacter::m_hMyWeapons
    constexpr int k_max_weapons = 56;
    constexpr std::uintptr_t k_off_active_weapon = 0x19D4; // CBaseCombatCharacter::m_hActiveWeapon
    constexpr std::uintptr_t k_off_buttons = 0x1CB4; // CBasePlayer::m_nButtons (datamap)
    constexpr std::uintptr_t k_off_lagged_movement = 0x20D0; // CBasePlayer::m_flLaggedMovementValue
    constexpr std::uintptr_t k_off_shove_penalty = 0x2CF0; // CTerrorPlayer::m_iShovePenalty (SendProp)

    constexpr std::uintptr_t k_rva_weapon_get_slot = 0x2C98F0; // CTerrorPlayer: the carried weapon of a weapon ID's slot
    constexpr std::uintptr_t k_rva_set_shove_penalty = 0x2CB750; // CTerrorPlayer::SetShovePenalty(int), thiscall

    using GiveFn = void*(__thiscall*)(void* player, const char* name, int a, int b, int c);
    using WeaponDropFn = void(__thiscall*)(void* player, void* weapon, const Vec3* target, const Vec3* velocity);
    using CannotDropFn = int(__thiscall*)(void* player);
    using SelectItemFn = void*(__thiscall*)(void* player, const char* classname, int subtype);
    using EyePositionFn = Vec3*(__thiscall*)(void* player, Vec3* out);
    using EyeAnglesFn = const Vec3*(__thiscall*)(void* player);
    using BumpWeaponFn = bool(__fastcall*)(void* player, void* edx, void* weapon);
    using GetSlotFn = void*(__fastcall*)(void* player, void* edx, int weapon_id);
    using CommitSuicideFn = void(__thiscall*)(void* player, bool explode, bool force);
    using SetShovePenaltyFn = void(__thiscall*)(void* player, int penalty);

    // ---- server.dll: weapons -----------------------------------------------------------------------------------------

    // CBaseCombatWeapon (SendProp / datamap definitions: push <offset> / push "<name>")
    constexpr std::uintptr_t k_off_weapon_owner = 0x13E8; // m_hOwner (SendProp: push 0x13E8 / "m_hOwner")
    constexpr std::uintptr_t k_off_next_primary = 0x13F0; // m_flNextPrimaryAttack (game time)
    constexpr std::uintptr_t k_off_next_secondary = 0x13F4; // m_flNextSecondaryAttack
    constexpr std::uintptr_t k_off_ammo_type = 0x140C; // m_iPrimaryAmmoType
    constexpr std::uintptr_t k_off_clip = 0x1414; // m_iClip1
    constexpr std::uintptr_t k_off_in_reload = 0x144D; // m_bInReload (SendProp at 0x1004C5A6: push 0x144D / push "m_bInReload")
    constexpr std::uintptr_t k_off_fire_on_empty = 0x144E; // m_bFireOnEmpty (datamap, FIELD_BOOLEAN)
    // CBaseCombatWeapon::ScriptSetClip1(int) (0x103F0650, the VScript "SetClip1"): thiscall, ret 4
    constexpr std::uintptr_t k_rva_set_clip1 = 0x3F0650;
    using SetClip1Fn = void(__thiscall*)(void* weapon, int clip);
    // CBaseCombatWeapon::HandleFireOnEmpty (0x1004ABD0, vtable +0x450), void __thiscall
    constexpr std::uintptr_t k_rva_fire_on_empty = 0x4ABD0;
    using FireOnEmptyFn = void(__fastcall*)(void* weapon, void* edx);

    // CTerrorWeapon / CTerrorGun: the upgrades (SendProps: push 0x17E0 / "m_upgradeBitVec" in CTerrorGun's table;
    // push 0x1788 / "m_nUpgradedPrimaryAmmoLoaded" in CTerrorWeapon's LocalL4DWeaponData): 1 incendiary, 2 explosive,
    // 4 laser sight, and the incendiary / explosive rounds still loaded.
    constexpr std::uintptr_t k_off_special_loaded = 0x1788;
    constexpr std::uintptr_t k_off_upgrades = 0x17E0;
    constexpr std::uintptr_t k_off_extra_ammo = 0x14F4; // CTerrorGun::m_iExtraPrimaryAmmo: a gun's reserve while it lies
    // CTerrorMeleeWeapon::m_strMapSetScriptName (datamap entry 0x107BDF20: FIELD_STRING, keyvalue "melee_script_name"):
    // which melee weapon it is, "fireaxe", "katana" ...  (The same offset as m_upgradeBitVec: another class.)
    constexpr std::uintptr_t k_off_melee_name = 0x17E0;

    // CTerrorGun::Use (0x103E8700, slot 106 of CTerrorGun's vtable 0x106D5544)
    constexpr std::uintptr_t k_rva_gun_use = 0x3E8700;
    // Reload (slot 281): CTerrorGun's (0x103E8860) and the shotguns' (0x103C2F80, CPumpShotgun's vtable 0x106CC994);
    // bool __thiscall Reload()
    constexpr int k_vt_reload = 281;
    constexpr std::uintptr_t k_rva_gun_reload = 0x3E8860;
    constexpr std::uintptr_t k_rva_shotgun_reload = 0x3C2F80;
    constexpr std::uintptr_t k_rva_pump_shotgun_vtable = 0x6CC994;
    using ReloadFn = bool(__fastcall*)(void* gun, void* edx);

    // ---- server.dll: map item spawners (weapon_*_spawn; see "Map item spawners" in docs/game-notes.md) ---------------

    constexpr std::uintptr_t k_rva_spawn_vtable = 0x6796E4; // CWeaponSpawn (CWeaponSpawnConfigurable shares its Use)
    constexpr int k_vt_spawn_weapon_id = 240; // the spawner's weapon ID
    constexpr std::uintptr_t k_off_spawn_count = 0x1444; // CWeaponSpawn's spawn count (0: endless)
    constexpr std::uintptr_t k_rva_weapon_id_name = 0x24EB70; // cdecl: weapon ID -> "weapon_first_aid_kit"
    // CWeaponAmmoSpawn::Use (0x1029F5F0, slot 106 of its vtable 0x10682CF4): its own, not CWeaponSpawn's
    constexpr std::uintptr_t k_rva_ammo_spawn_use = 0x29F5F0;
    constexpr std::uintptr_t k_rva_ammo_spawn_vtable = 0x682CF4;
    using SpawnUseFn = void(__fastcall*)(void* spawner, void* edx, void* activator, void* caller, int type, float value);
    using WeaponIdFn = int(__thiscall*)(void* spawner);
    using WeaponIdNameFn = const char*(__cdecl*)(int weapon_id);

    // ---- server.dll: the game ----------------------------------------------------------------------------------------

    constexpr std::uintptr_t k_rva_client_command = 0x90C80; // ClientCommand(CBasePlayer*, const CCommand&), cdecl
    constexpr std::uintptr_t k_off_args_argv = 0x408; // CCommand: argc at +0, argv[] at +0x408
    constexpr std::uintptr_t k_rva_script_command = 0x21CB90; // the `script` command's handler (cdecl, const CCommand&)
    constexpr std::uintptr_t k_rva_rescue_set_survivor = 0x2CEAF0; // CSurvivorRescue::SetSurvivor(CTerrorPlayer*)
    constexpr std::uintptr_t k_rva_server_engine = 0x7F7698; // server.dll's own IVEngineServer pointer
    constexpr std::uintptr_t k_rva_change_accessor_call = 0xED4B0; // what calls the engine's slot 97 (SetShovePenalty's way)
    // CGlobalVars (gpGlobals: PlayerInfoManager002 slot 1 GetGlobalVars)
    constexpr std::uintptr_t k_off_curtime = 0xC;
    constexpr std::uintptr_t k_off_mapname = 0x3C; // a string_t: the const char*

    using ClientCommandFn = void(__cdecl*)(void* player, const void* args);
    using ScriptCommandFn = void(__cdecl*)(const void* command);
    using SetRescueSurvivorFn = void(__fastcall*)(void* rescue, void* edx, void* survivor);
    using GameFrameFn = void(__fastcall*)(void* self, void* edx, bool simulating); // ServerGameDLL005 slot 4

    // ---- engine.dll: the server side ---------------------------------------------------------------------------------

    // VEngineServer022
    constexpr int k_vt_is_model_precached = 9; // IsModelPrecached(name)
    constexpr int k_vt_change_accessor = 97; // GetChangeAccessor(edict)
    using IsModelPrecachedFn = bool(__thiscall*)(void* engine, const char* name);
    using UserIdFn = int(__thiscall*)(void* engine, void* edict); // slot 16 GetPlayerUserId
    using NetworkIdFn = const char*(__thiscall*)(void* engine, void* edict); // slot 17 GetPlayerNetworkIDString
    using ServerCommandFn = void(__thiscall*)(void* engine, const char* command); // slot 37 ServerCommand
    using ClientPrintfFn = void(__thiscall*)(void* engine, void* edict, const char* text); // slot 46 ClientPrintf
    using ChangeAccessorFn = void*(__thiscall*)(void* engine, void* edict);

    // IGameEventManager2 (GAMEEVENTSMANAGER002): slot 6 CreateEvent, slot 7 FireEvent; CGameEvent slot 1 GetName,
    // slot 6 GetInt
    using CreateEventFn = void*(__fastcall*)(void* self, void* edx, const char* name, bool force, int* cookie);
    using FireEventFn = bool(__fastcall*)(void* self, void* edx, void* event, bool dont_broadcast);
    using EventNameFn = const char*(__thiscall*)(void* event);
    using EventIntFn = int(__thiscall*)(void* event, const char* key, int fallback);

    // ---- filesystem_stdio.dll (dedicated.dll on a dedicated server) ---------------------------------------------------

    constexpr int k_vt_file_exists = 10; // VBaseFileSystem012 FileExists(name, pathID)
    constexpr int k_vt_add_vpk_file = 107; // VFileSystem018 AddVPKFile(name, addType)
    using FileExistsFn = bool(__thiscall*)(void* fs, const char* name, const char* path_id);
    using AddVpkFileFn = void(__thiscall*)(void* fs, const char* name, int add_type);

    // ---- engine.dll: the client side ---------------------------------------------------------------------------------

    // CBaseClientState::ProcessPrint(SVC_Print*) (0x10052100, thiscall, ret 4): ConMsg("%s", msg + 0x54)
    constexpr std::uintptr_t k_rva_process_print = 0x52100;
    constexpr std::uintptr_t k_off_print_text = 0x54;
    using ProcessPrintFn = bool(__fastcall*)(void* state, void* edx, void* msg);
    using IsInGameFn = bool(__thiscall*)(void* engine); // VEngineClient013 slot 26
    using ClientCmdFn = void(__thiscall*)(void* engine, const char* text); // VEngineClient013 slot 107
    using GetLocalPlayerFn = int(__thiscall*)(void* engine); // VEngineClient013 slot 12
    using GameUiVisibleFn = bool(__thiscall*)(void* vgui); // VEngineVGui001 slot 2

    // ---- client.dll ----------------------------------------------------------------------------------------------------

    // CHudWeaponSelection: its CHudElement vtable and its ShouldDraw (slot 12); its vgui::Panel part and Paint (slot 128)
    constexpr std::uintptr_t k_rva_weapon_select_vtable = 0x5D5A84;
    constexpr std::uintptr_t k_rva_weapon_select_should_draw = 0x2CE900;
    constexpr std::uintptr_t k_rva_weapon_select_panel_vtable = 0x5D5734;
    constexpr std::uintptr_t k_rva_weapon_select_paint = 0x2D11E0;
    using ShouldDrawFn = bool(__fastcall*)(void* self, void* edx);
    using ItemBoxesFn = void (*)();

    // VClientEntityList003 slot 3 GetClientEntity(index), slot 4 GetClientEntityFromHandle(handle)
    using GetClientEntityFn = void*(__thiscall*)(void* list, int index);
    using GetClientEntityFromHandleFn = void*(__thiscall*)(void* list, std::uint32_t handle);

    // client netprops (RecvProp definitions: push offset / push "name")
    constexpr std::uintptr_t k_c_team = 0xE4; // m_iTeamNum
    constexpr std::uintptr_t k_c_life_state = 0x147; // m_lifeState (byte, 0: alive)
    constexpr std::uintptr_t k_c_ammo = 0xF24; // m_iAmmo (32 ints)
    constexpr std::uintptr_t k_c_active_weapon = 0x1084; // m_hActiveWeapon
    constexpr std::uintptr_t k_c_buttons = 0x13A4; // m_nButtons (client datamap, entry at 0x106C1664)
    constexpr std::uintptr_t k_c_observer_mode = 0x1450; // m_iObserverMode (0: not spectating)
    constexpr std::uintptr_t k_cw_ammo_type = 0x97C; // weapon m_iPrimaryAmmoType
    constexpr std::uintptr_t k_cw_clip = 0x984; // weapon m_iClip1
    constexpr std::uintptr_t k_cw_fire_on_empty = 0x9BE; // weapon m_bFireOnEmpty (datamap entry at 0x106BCECC)

    // Reload (slot 269 of the primary vtable): C_TerrorGun's (vtable 0x10609034 -> 0x1030C150) and C_PumpShotgun's
    // (0x105FF6FC -> 0x102F1CF0); HandleFireOnEmpty 0x100156D0
    constexpr int k_c_vt_reload = 269;
    constexpr std::uintptr_t k_rva_c_gun_vtable = 0x609034, k_rva_c_pump_vtable = 0x5FF6FC;
    constexpr std::uintptr_t k_rva_c_gun_reload = 0x30C150, k_rva_c_shotgun_reload = 0x2F1CF0;
    constexpr std::uintptr_t k_rva_c_fire_on_empty = 0x156D0;

} // namespace game::sdk
