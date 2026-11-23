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
    constexpr std::uintptr_t k_off_health = 0xEC; // m_iHealth (SendProp at 0x1016D343: push 0xEC / push "m_iHealth")
    constexpr std::uintptr_t k_off_max_health = 0xE8; // m_iMaxHealth (SendProp at 0x102FEEA7; a common: z_health, 50)
    // A common infected (class Infected, primary vtable 0x106D9DB4): slot 82 MyInfectedPointer (0x10299380: mov eax,
    // ecx); its INextBot part at +0x1A40 (vtable 0x106D9C6C), whose slot 28 OnShoved(CBaseEntity* pusher) (0x10298FE0,
    // thiscall, ret 4) makes it stumble away from the pusher's centre, as a shove does (the melee weapon's collateral
    // shove calls it so, 0x103EBCA0).  While byte +0x1D94 is 1 a shove from behind only pushes (else it kills: the
    // game sets it so while AllowBash says "push only")
    constexpr int k_vt_my_infected = 82;
    constexpr std::uintptr_t k_off_infected_nextbot = 0x1A40;
    constexpr std::uintptr_t k_rva_infected_nextbot_vtable = 0x6D9C6C;
    constexpr int k_vt_nextbot_on_shoved = 28;
    constexpr std::uintptr_t k_off_infected_push_only = 0x1D94;
    using MyInfectedFn = void*(__thiscall*)(void* entity);
    using OnShovedFn = void(__thiscall*)(void* nextbot, void* pusher);
    constexpr std::uintptr_t k_off_effects = 0xCC; // m_fEffects (SendProp / datamap: push 0xCC / push "m_fEffects"); EF_NODRAW 0x20
    constexpr std::uint32_t k_ef_nodraw = 0x20;
    constexpr std::uintptr_t k_off_team = 0x238; // m_iTeamNum
    constexpr std::uintptr_t k_off_entity_name = 0x140; // m_iName (targetname)
    constexpr std::uintptr_t k_off_model_name = 0x24C; // m_ModelName (datamap, FIELD_MODELNAME: a pointer to the text)
    constexpr std::uintptr_t k_off_abs_velocity = 0x260; // m_vecAbsVelocity (datamap)
    constexpr std::uintptr_t k_off_velocity = 0x2D8; // m_vecVelocity
    constexpr std::uintptr_t k_off_origin = 0x388; // m_vecOrigin (the SendProps: push 0x388 / push 0xC)
    constexpr std::uintptr_t k_off_angles = 0x394; // m_angRotation (SendProp: push 0x394 / "m_angRotation")
    constexpr std::uintptr_t k_off_spawner_angles = 0x394; // the same field, read off a spawner
    // CBaseEntity::m_Glow (SendPropDataTable at 0x10059364: push 0x1DC / push "m_Glow"), a DT_GlowProperty whose props,
    // sent by the table at 0x10105BE0, sit at m_iGlowType +4, m_nGlowRange +8, m_nGlowRangeMin +0xC,
    // m_glowColorOverride +0x10 (R | G << 8 | B << 16) and m_bFlashing +0x14.  m_iGlowType 3 is the constant outline
    // seen through walls; range 0 is endless.
    constexpr std::uintptr_t k_off_glow_type = 0x1DC + 0x4, k_off_glow_range = 0x1DC + 0x8, k_off_glow_range_min = 0x1DC + 0xC,
                             k_off_glow_color = 0x1DC + 0x10, k_off_glow_flashing = 0x1DC + 0x14;
    constexpr int k_glow_constant = 3;
    // CTerrorPlayer::m_zombieClass (the SendProp in DT_TerrorPlayer, the table of 0x102FE490: push 0x2BB8 / push
    // "m_zombieClass" at 0x102FEAF8): 1 Smoker, 2 Boomer, 3 Hunter, 4 Spitter, 5 Jockey, 6 Charger, 8 Tank
    constexpr std::uintptr_t k_off_zombie_class = 0x2BB8;

    // CBaseEntity::TakeDamage(const CTakeDamageInfo&) (0x10055C70, thiscall, ret 4) and CTakeDamageInfo (0x60 bytes)
    constexpr std::uintptr_t k_rva_take_damage = 0x55C70;
    constexpr std::size_t k_damage_info_size = 0x60;
    constexpr std::uintptr_t k_off_damage_inflictor = 0x30, k_off_damage_attacker = 0x34, k_off_damage_weapon = 0x38,
                             k_off_damage = 0x3C, k_off_damage_type = 0x48, k_off_damage_force = 0x0, k_off_damage_position = 0xC;
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
    // CBaseCombatCharacter::m_LastHitGroup (datamap at 0x100449B3: "m_LastHitGroup", 0x1838): where the last trace
    // attack hit, set before the damage is applied (HITGROUP_HEAD 1)
    constexpr std::uintptr_t k_off_last_hit_group = 0x1838;
    constexpr int k_hitgroup_head = 1;
    constexpr std::uintptr_t k_off_flags = 0x13C; // CBasePlayer::m_fFlags (FL_ONGROUND 1)
    constexpr std::uintptr_t k_off_player_ammo = 0x1874; // CBasePlayer::m_iAmmo[32]: the reserve of each ammo type
    constexpr std::uintptr_t k_off_my_weapons = 0x18F4; // CBaseCombatCharacter::m_hMyWeapons
    constexpr int k_max_weapons = 56;
    constexpr std::uintptr_t k_off_active_weapon = 0x19D4; // CBaseCombatCharacter::m_hActiveWeapon
    constexpr std::uintptr_t k_off_buttons = 0x1CB4; // CBasePlayer::m_nButtons (datamap)
    constexpr std::uintptr_t k_off_lagged_movement = 0x20D0; // CBasePlayer::m_flLaggedMovementValue
    constexpr std::uintptr_t k_off_shove_penalty = 0x2CF0; // CTerrorPlayer::m_iShovePenalty (SendProp)
    // CTerrorPlayer::m_bAdrenalineActive (SendProp; what IsAdrenalineActive reads: mov al, [ecx+0x328C]; ret)
    constexpr std::uintptr_t k_off_adrenaline_active = 0x328C;
    constexpr std::uintptr_t k_rva_is_adrenaline_active = 0x2C8DA0;

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

    // Incapacitated, pinned, revived ([self_revive]).  CTerrorPlayer's VScript bindings (the table built at 0x10330690:
    // name at +0, function at +0x28 of each 0x30-byte entry) lead to them:
    //   IsIncapacitated (0x102FDCF0: jmp [eax+0x7E4]) is vtable slot 505 (0x10325540): alive (m_lifeState +0xF0 == 0)
    //       and m_isIncapacitated (+0x2DA8, the SendProp at 0x102FEB38), which a survivor hanging from a ledge has too;
    //   IsHangingFromLedge (0x100F3740): m_isHangingFromLedge, the byte at +0x3A05 (SendProp at 0x102FF694);
    //   IsDominatedBySpecialInfected (0x102CB240, non-virtual, thiscall, bool): m_pummelAttacker +0x3E5C, m_pounceAttacker
    //       +0x3E7C, m_tongueOwner +0x33D8, m_jockeyAttacker +0x3EF4, m_carryAttacker +0x2A5C (each a SendProp of the
    //       same names) any of them a live handle;
    //   ReviveFromIncap (0x102FDD10: jmp [eax+0x91C]) is vtable slot 583, CTerrorPlayer::OnRevived (0x1031F900, thiscall,
    //       no arguments): for a living incapacitated survivor; up off a ledge, or up from the ground with the next
    //       m_currentReviveCount (+0x2E68: SurvivorMaxIncapacitatedCount makes it the last life, "Player.Heartbeat"),
    //       fires "revive_success" with the reviver m_reviveOwner (+0x2E38) or, with none, the survivor themselves.
    //   UseAdrenaline is CTerrorPlayer::OnAdrenalineUsed(float seconds) (0x10318750, thiscall, ret 4): the rush, no
    //       health ("Cause the effect of Adrenaline - but no HP").
    // The progress bar the game draws (DT_CSPlayer's SendProps, the table of 0x10240940): m_flProgressBarStartTime
    // +0x2910 (game time) and m_flProgressBarDuration +0x2914 (seconds; 0: none).
    constexpr int k_vt_is_incapacitated = 505;
    constexpr std::uintptr_t k_rva_is_incapacitated = 0x325540;
    constexpr int k_vt_on_revived = 583;
    constexpr std::uintptr_t k_rva_on_revived = 0x31F900;
    constexpr std::uintptr_t k_rva_is_dominated = 0x2CB240;
    constexpr std::uintptr_t k_rva_on_adrenaline_used = 0x318750;
    constexpr std::uintptr_t k_off_hanging_from_ledge = 0x3A05;
    constexpr std::uintptr_t k_off_revive_owner = 0x2E38;
    constexpr std::uintptr_t k_off_progress_start = 0x2910, k_off_progress_duration = 0x2914;
    using IsIncapacitatedFn = bool(__thiscall*)(void* player);
    using OnRevivedFn = void(__thiscall*)(void* player);
    using IsDominatedFn = bool(__thiscall*)(void* player);
    using OnAdrenalineUsedFn = void(__thiscall*)(void* player, float seconds);

    // CTerrorPlayer::OnStaggered(CBaseEntity* source, const Vector* from) (0x1032CE80, non-virtual, thiscall, ret 8;
    // what the VScript "Stagger" wraps, 0x1032D730): the player staggers away from `from` (or from source's origin);
    // nothing for a ghost or an incapacitated player; it breaks pins.  Specials: z_max_stagger_duration, survivors:
    // survivor_max_tongue_stagger_duration / _distance (1.5 s, 200)
    constexpr std::uintptr_t k_rva_stagger_player = 0x32CE80;
    using StaggerPlayerFn = void(__fastcall*)(void* player, void* edx, void* source, const Vec3* from);
    // CBasePlayer::ViewPunch(const QAngle&) (0x1007CD30, non-virtual, thiscall, ret 4): m_vecPunchAngleVel += it * 20,
    // the game's spring-damped kick (nothing while sv_suppress_viewpunch)
    constexpr std::uintptr_t k_rva_view_punch = 0x7CD30;
    using ViewPunchFn = void(__fastcall*)(void* player, void* edx, const Vec3* angles);

    // ---- server.dll: weapons -----------------------------------------------------------------------------------------

    // CBaseCombatWeapon (SendProp / datamap definitions: push <offset> / push "<name>")
    constexpr std::uintptr_t k_off_weapon_owner = 0x13E8; // m_hOwner (SendProp: push 0x13E8 / "m_hOwner")
    constexpr std::uintptr_t k_off_next_primary = 0x13F0; // m_flNextPrimaryAttack (game time)
    constexpr std::uintptr_t k_off_next_secondary = 0x13F4; // m_flNextSecondaryAttack
    constexpr std::uintptr_t k_off_ammo_type = 0x140C; // m_iPrimaryAmmoType (SendProp, 8 bits, at 0x1004AFEC)
    // The game's ammo types of the pump shotguns (AMMO_TYPE_SHOTGUN) and the auto shotguns (AMMO_TYPE_AUTOSHOTGUN);
    // the mod makes the auto shotguns take the first (retarget_ammo_types)
    constexpr int k_ammo_type_shotgun = 7, k_ammo_type_autoshotgun = 8;
    // The pistols' (AMMO_TYPE_PISTOL, AMMO_TYPE_MAGNUM) and the types the mod makes them take instead: the SMGs'
    // (AMMO_TYPE_SMG 5) and the sniper rifles' (AMMO_TYPE_SNIPERRIFLE 10); see retarget_ammo_types
    constexpr int k_ammo_type_pistol = 1, k_ammo_type_magnum = 2, k_ammo_type_smg = 5, k_ammo_type_sniperrifle = 10;
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

    // CTerrorMeleeWeapon (primary vtable 0x106D611C, 448 slots): slot 282 PrimaryAttack (0x103EC3B0, void __thiscall:
    // "melee attack failed - !player->CanAttack()"), which starts the swing (0x103EC080: m_flNextPrimaryAttack =
    // curtime + the weapon's refire, the secondary time too)
    constexpr std::uintptr_t k_rva_melee_vtable = 0x6D611C;
    constexpr int k_vt_primary_attack = 282;
    constexpr std::uintptr_t k_rva_melee_primary_attack = 0x3EC3B0;
    using PrimaryAttackFn = void(__fastcall*)(void* weapon, void* edx);
    // slot 283 SecondaryAttack (0x103EB1F0, void thiscall): the shove.  It calls slot 435 (0x103F06F0, "StartSwing":
    // z_gun_swing_interval, z_gun_swing_duration, z_gun_range), which sets both attack times to curtime + interval
    constexpr int k_vt_secondary_attack = 283;
    constexpr std::uintptr_t k_rva_melee_secondary_attack = 0x3EB1F0;
    constexpr char k_melee_secondary_prologue[] = "56 57 8B F1 E8 ?? ?? ?? ?? 8B F8 85 FF 0F 84 ?? ?? ?? ?? 8B 06 8B 90 7C";
    constexpr std::size_t k_melee_secondary_prologue_length = 24;
    // The melee weapon's swings (its script's "primaryattacks"; scripts/melee/<kind>.txt, parsed by 0x103D5780): the
    // CMeleeWeaponInfo database (0x10868F08) by the weapon's m_hMeleeWeaponInfo (+0x17DC), looked up by 0x103D5FF0
    // (thiscall (db, handle), ret 4; the swing start 0x103EC080 calls it so: `mov eax, [esi+0x17DC]; push eax; mov ecx,
    // db; call`).  The info's primary attacks are a CUtlVector of 0x30-byte entries (memory +0xC20, count +0xC2C):
    // +0x04 the third-person activity, +0x08 its idle one, +0x0C startdir (N 0, S 1, E 2, W 3, NE 4, SE 5, SW 6, NW 7).
    // The next one is the weapon's m_iNextPrimaryAttackIndex (+0x17F8; the client's +0xD08), reset to 0 when
    // melee_combo_reset_time (1.2 s) passed since m_flLastAttackTime (+0x1828; the client's +0xD38).  Chosen by
    // 0x103EAF30 (the client's 0x1030DA10): thiscall (weapon, info), ret 4, returns the viewmodel activity.  The game
    // never reads the scripts' "strongattacks" (no such string in either DLL).
    constexpr std::uintptr_t k_off_melee_info_handle = 0x17DC;
    constexpr std::uintptr_t k_off_melee_next_attack = 0x17F8, k_off_melee_last_attack = 0x1828;
    constexpr std::uintptr_t k_cw_melee_next_attack = 0xD08, k_cw_melee_last_attack = 0xD38;
    constexpr std::uintptr_t k_off_info_attacks = 0xC20, k_off_info_attack_count = 0xC2C;
    constexpr std::size_t k_attack_size = 0x30;
    constexpr std::uintptr_t k_off_attack_idle_activity = 0x08, k_off_attack_start_dir = 0x0C;
    constexpr std::uintptr_t k_rva_melee_swing_start = 0x3EC080;
    constexpr std::uintptr_t k_rva_choose_attack = 0x3EAF30, k_rva_c_choose_attack = 0x30DA10;
    using ChooseAttackFn = int(__fastcall*)(void* weapon, void* edx, void* info);
    using MeleeInfoLookupFn = void*(__thiscall*)(void* database, std::uint32_t handle);
    // CTerrorPlayer::DoAnimationEvent(int event, int data) (slot 510, 0x102FBB30, thiscall, ret 8): event 66 plays
    // the activity `data` in gesture slot 0 when it is free, networked through m_NetGesture* (+0x2ADC ...)
    constexpr int k_vt_do_animation_event = 510;
    constexpr std::uintptr_t k_rva_do_animation_event = 0x2FBB30;
    constexpr int k_anim_event_gesture = 66;
    using DoAnimationEventFn = void(__thiscall*)(void* player, int event, int data);

    // UTIL_TraceLine(const Vector& start, const Vector& end, unsigned mask, const ITraceFilter*, trace_t*) (0x100101E0,
    // cdecl; it builds a Ray_t and calls the engine's EngineTraceServer003 slot 5 TraceRay).  A CTraceFilterWorldOnly
    // is a bare object whose vtable is 0x1068E3AC (ShouldHitEntity: false; GetTraceType: 1, the world only).
    // trace_t is 0x54 bytes: plane.normal +0x18, fraction +0x2C, startsolid +0x37, m_pEnt +0x4C.  MASK_SOLID 0x0200400B.
    constexpr std::uintptr_t k_rva_trace_line = 0x101E0;
    constexpr std::uintptr_t k_rva_trace_filter_world_only = 0x68E3AC;
    constexpr std::size_t k_trace_size = 0x54;
    constexpr std::uintptr_t k_off_trace_normal = 0x18, k_off_trace_fraction = 0x2C, k_off_trace_start_solid = 0x37;
    constexpr unsigned k_mask_solid = 0x0200400Bu;
    using TraceLineFn = void(__cdecl*)(const Vec3* start, const Vec3* end, unsigned mask, const void* filter, void* trace);

    // The survivors' look-at line (what `vocalize smartlook` says; server.dll's ClientCommand, 0x1033B2AA): the speech concept
    // for what the player looks at, found by CTerrorPlayer::SpeakSmartLook (0x103120E0, thiscall, ret 0xC: (CBaseEntity*
    // looked_at or null for the player's own use target, char* modifiers, int size); the modifiers buffer starts as
    // "smartlooktype:manual" and gets ",weaponname:<name>" or ",SpecialType:<name>" appended; it returns the concept's name:
    // "PlayerSpotWeapon", "PlayerWarnSpecial", ... or "PlayerLook" when it knew nothing; a CBaseEntity that is an
    // IResponseRulesWeaponSource, a weapon or a weapon spawner, is a weapon).  The command then builds an AI_Concept with
    // 0x10028B10 (thiscall, ret 4: the name; 8 bytes: word 0xFFFF, dword -1 until filled) and calls CTerrorPlayer's vtable
    // slot 492 (0x10344750, thiscall, ret 0x1C) Speak(AI_Concept, int priority 2, const char* modifiers, char* out, size_t
    // out_size, IRecipientFilter* filter), with the last three 0.
    constexpr std::uintptr_t k_rva_smart_look = 0x3120E0;
    constexpr std::uintptr_t k_rva_ai_concept_ctor = 0x28B10;
    constexpr int k_vt_speak = 492;
    constexpr std::uintptr_t k_rva_speak = 0x344750;
    struct AiConcept
    {
        std::uint16_t id;
        std::uint16_t pad;
        std::int32_t value;
    };
    using SmartLookFn = const char*(__thiscall*)(void* player, void* looked_at, char* modifiers, int size);
    using ConceptCtorFn = void*(__thiscall*)(AiConcept* spoken, const char* name);
    using SpeakFn = bool(__thiscall*)(void* player, AiConcept spoken, int priority, const char* modifiers, char* out,
                                      std::size_t out_size, void* filter);

    // CBaseCombatWeapon::WeaponSound(WeaponSound_t type, float soundtime) (0x1004BDA0, slot 301, not overridden by the
    // melee weapon; thiscall, ret 8): the melee swing's trace plays type 10 (melee_hit_world, of the script's sound
    // names at 0x107A1878: melee_miss 8, melee_hit 9, melee_hit_world 10) when it hits something that is not a combat
    // character: the world, a prop, a door
    constexpr int k_vt_weapon_sound = 301;
    constexpr std::uintptr_t k_rva_weapon_sound = 0x4BDA0;
    constexpr int k_sound_melee_hit_world = 10;
    using WeaponSoundFn = void(__fastcall*)(void* weapon, void* edx, int type, float soundtime);

    // The chainsaw ([chainsaw]).  CChainsaw's primary vtable is 0x106BC7FC (its RTTI .?AVCChainsaw@@); slot 272, its
    // ItemPostFrame (0x103CA970, void __thiscall, the only reference to it is that slot), runs while it is in the hands:
    // first, when m_iClip1 (the fuel) is 0, it takes the chainsaw away from the owner (0x102485B0) and gives them a
    // "weapon_pistol" (the owner's slot 431, GiveNamedItem); then it burns 1 fuel per tick of its timer while it runs,
    // and at 0 plays "Chainsaw.Die" and stops the motor; it ends with the base ItemPostFrame (0x103F1C20).  The owner
    // is at +0x1800.  CBaseCombatWeapon::GetMaxClip1 is slot 323 (the VScript GetMaxClip1, 0x103ECD60: jmp [eax+0x50C]).
    constexpr std::uintptr_t k_rva_chainsaw_vtable = 0x6BC7FC;
    constexpr int k_vt_item_post_frame = 272;
    constexpr std::uintptr_t k_rva_chainsaw_post_frame = 0x3CA970;
    constexpr int k_vt_get_max_clip1 = 323;
    using ItemPostFrameFn = void(__fastcall*)(void* weapon, void* edx);
    using GetMaxClip1Fn = int(__thiscall*)(void* weapon);

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
    // CTransitioningPlayer::Restore(CTerrorPlayer*) (0x102B8B60, thiscall, ret 4, nothing returned): a survivor's saved
    // state (the KeyValues at +0 of `this`) back onto their new player entity in the next chapter: health, position,
    // each "weapon" subkey ("currentMagazine" onto m_iClip1, "extraAmmo" with SetAmmoCount for that gun's type).
    // Called by the takeover of a survivor bot ("[TAKEOVER]: Restoring %s(%d) after transition.", 0x102BA070).
    constexpr std::uintptr_t k_rva_transition_restore = 0x2B8B60;
    constexpr std::uintptr_t k_rva_server_engine = 0x7F7698; // server.dll's own IVEngineServer pointer
    constexpr std::uintptr_t k_rva_change_accessor_call = 0xED4B0; // what calls the engine's slot 97 (SetShovePenalty's way)
    // CGlobalVars (gpGlobals: PlayerInfoManager002 slot 1 GetGlobalVars)
    constexpr std::uintptr_t k_off_curtime = 0xC;
    constexpr std::uintptr_t k_off_mapname = 0x3C; // a string_t: the const char*
    // The director's ConVars and commands the mutators use (horde_tide, hunted), found at run time by name
    // (find_server_convar), all FCVAR_CHEAT: director_force_panic_event (ConCommand 0x1083E4DC, callback 0x10287900:
    // TheDirector 0x1083B1B8 -> ForcePanicEvent 0x1025CC60 (0, 0)); z_<class>_limit (ConVars, default "1": smoker
    // 0x1083D228, boomer 0x1083D1E0, hunter 0x1083D420, spitter 0x1083D4B0, jockey 0x1083D540, charger 0x1083D5D0),
    // a table of them by zombie class at 0x107A5288 that the director's limit of a class (0x1025C7C0) falls back to
    // when no director script sets one.
    // The crowd, witch and tank mutators set z_common_limit, z_mob_spawn_min_size, z_mob_spawn_max_size,
    // z_mega_mob_size, z_witch_health, z_witch_anger_rate, z_witch_personal_space, z_witch_damage, z_tank_health and
    // z_tank_speed the same way (each registered by a static constructor: `push flags; push "<default>"; push
    // "<name>"; mov ecx, <ConVar>`; z_witch_anger_rate's at 0x105A82A0, flags 0x4000, "0.2", ConVar 0x10871250).
    constexpr int k_fcvar_cheat = 0x4000;
    constexpr std::uintptr_t k_off_convar_flags = 0x14; // ConCommandBase::m_nFlags
    constexpr std::uintptr_t k_off_convar_parent = 0x1C; // ConVar::m_pParent (the value lives there)
    // ConVar::m_fValue (0x1041D806 reads z_witch_anger_rate so: `mov eax, [0x1087126C]` (its m_pParent); `movss xmm0,
    // [eax+0x2C]`)
    constexpr std::uintptr_t k_off_convar_float = 0x2C;
    constexpr std::uintptr_t k_off_convar_int = 0x30; // ConVar::m_nValue (0x1025C7C0 reads it so)

    using ClientCommandFn = void(__cdecl*)(void* player, const void* args);
    using ScriptCommandFn = void(__cdecl*)(const void* command);
    using SetRescueSurvivorFn = void(__fastcall*)(void* rescue, void* edx, void* survivor);
    using TransitionRestoreFn = void(__fastcall*)(void* transitioning, void* edx, void* player);
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
    using IsInGameFn = bool(__thiscall*)(void* engine); // VEngineClient013 slot 26 (also slot 27 IsConnected)
    constexpr int k_vt_is_connected = 27;
    using ClientCmdFn = void(__thiscall*)(void* engine, const char* text); // VEngineClient013 slot 107
    using GetLocalPlayerFn = int(__thiscall*)(void* engine); // VEngineClient013 slot 12
    using GameUiVisibleFn = bool(__thiscall*)(void* vgui); // VEngineVGui001 slot 2

    // VEngineClient013 slot 37 WorldToScreenMatrix() (0x1005D620, thiscall: `mov ecx, [0x10427BC0]; mov eax, [ecx]; mov edx,
    // [eax+0x34]; jmp edx`, a call of the renderer's vtable slot 13), slot 38 WorldToViewMatrix (the same through slot 12).
    // The renderer (0x10427BC0) keeps a stack of views of 0x194 bytes each; slot 13 returns the top one's world-to-screen
    // VMatrix (+0x150 in it) or, with one view only, its own +0x9C; slot 12 the world-to-view one (+0xD0 / +0x1C).  A
    // VMatrix is 4x4 floats, row-major: clip = M * (x, y, z, 1), then x / w and y / w are -1 .. 1 across the picture.  The
    // pointer is not constant (it follows the stack), so it is asked for each time.  (Slot 36, 0x1005D610, is not it: it
    // returns the address of a global that is no matrix.)
    constexpr int k_vt_world_to_screen_matrix = 37;
    using WorldToScreenMatrixFn = const float*(__thiscall*)(void* engine);
    // ---- vgui2.dll, vguimatsurface.dll: the game's 2D drawing (see src/ui/vgui.cpp) -----------------------------------
    //
    // Each interface's object is a global its CreateInterface hands out; the vtables are the ones its constructor stores.
    // Every slot below is checked at run time against the RVA here and its prologue.

    // vgui2.dll "VGUI_Panel009" (VPanelWrapper, vtable 0x100480CC): slot 36 GetName(VPANEL), slot 41
    // PaintTraverse(VPANEL, bool force_repaint, bool allow_force)
    constexpr std::uintptr_t k_rva_panel_vtable = 0x480CC;
    constexpr int k_vt_panel_get_name = 36, k_vt_panel_paint_traverse = 41;
    constexpr std::uintptr_t k_rva_panel_get_name = 0x1EAC0, k_rva_panel_paint_traverse = 0x1EBE0;
    using PanelNameFn = const char*(__thiscall*)(void* panels, unsigned vpanel);
    // VPanelWrapper forwards each slot to the VPanel's own (vtable 0x10047FFC; L4D2's order is not the SDK's): slot 5
    // GetSize(vpanel, int& wide, int& tall) (0x1001E6E0 -> VPanel slot 13 0x1001D8A0: +0x2C, +0x2E) and slot 10
    // GetAbsPos(vpanel, int& x, int& y) (0x1001E780 -> VPanel slot 18 0x1001D9D0: the layout's absolute position at
    // +0x44 / +0x46, through the surface's +0x20C).  For the scoreboard's title (hud: the chapter's mutators).
    constexpr int k_vt_panel_get_size = 5, k_vt_panel_get_abs_pos = 10;
    using PanelRectFn = void(__thiscall*)(void* panels, unsigned vpanel, int& a, int& b);
    using PaintTraverseFn = void(__fastcall*)(void* panels, void* edx, unsigned vpanel, bool force_repaint, bool allow_force);

    // vgui2.dll "VGUI_Scheme010" (CSchemeManager, vtable 0x10046FCC): slot 5 GetScheme(tag), slot 8 GetIScheme(handle);
    // CScheme (vtable 0x10047010): slot 3 GetFont(name, proportional), slot 4 GetFontName(const HFont&)
    constexpr std::uintptr_t k_rva_scheme_manager_vtable = 0x46FCC, k_rva_scheme_vtable = 0x47010;
    constexpr int k_vt_schemes_get_scheme = 5, k_vt_schemes_get_ischeme = 8;
    constexpr int k_vt_scheme_get_font = 3, k_vt_scheme_font_name = 4;
    using GetSchemeFn = unsigned(__thiscall*)(void* schemes, const char* tag);
    using GetISchemeFn = void*(__thiscall*)(void* schemes, unsigned scheme);
    using SchemeFontFn = unsigned(__thiscall*)(void* scheme, const char* name, bool proportional);
    using SchemeFontNameFn = const char*(__thiscall*)(void* scheme, const unsigned& font);

    // vguimatsurface.dll "VGUI_Surface031" (CMatSystemSurface, vtable 0x100BEF2C).  0..4 are IAppSystem's; MSVC puts
    // overloads last first, so DrawSetColor(int r, g, b, a) is 11 and DrawSetColor(Color) 10 (which calls 11), the same
    // for DrawSetTextColor (19, and 18 calling it).
    constexpr std::uintptr_t k_rva_surface_vtable = 0xBEF2C;
    constexpr int k_vt_surface_set_color = 11; // (r, g, b, a): stored at +0x24, a times the alpha multiplier
    constexpr int k_vt_surface_filled_rect = 12; // (x0, y0, x1, y1)
    constexpr int k_vt_surface_text_font = 17; // (HFont): stored at +0x294
    constexpr int k_vt_surface_text_color = 19; // (r, g, b, a)
    constexpr int k_vt_surface_text_pos = 20; // (x, y): +0x2C, +0x30
    constexpr int k_vt_surface_print_text = 22; // (const wchar_t*, length, FontDrawType_t 0: the font's own)
    constexpr int k_vt_surface_texture_id = 25; // DrawGetTextureId(file): the id bound to it, or -1
    constexpr int k_vt_surface_texture_file = 27; // DrawSetTextureFile(id, material, hardware_filter, force_reload)
    constexpr int k_vt_surface_set_texture = 29; // DrawSetTexture(id): +0x2A0
    constexpr int k_vt_surface_new_texture = 34; // CreateNewTextureID(procedural)
    constexpr int k_vt_surface_screen_size = 35; // GetScreenSize(int& w, int& h)
    constexpr int k_vt_surface_text_size = 72; // GetTextSize(HFont, const wchar_t*, int& w, int& h)
    constexpr int k_vt_surface_play_sound = 75; // PlaySound(file under sound/): the engine's callback at +0x102D0
    constexpr int k_vt_surface_textured_sub_rect = 98; // (x0, y0, x1, y1, s0, t0, s1, t1)
    constexpr int k_vt_surface_textured_polygon = 99; // (n, Vertex_t*, clip)
    // DrawGetAlphaMultiplier (0x10009F10: `fld [ecx+0x10]; ret`; slot 105, 0x10009EE0, sets the field clamped to 0 .. 1;
    // DrawSetTextColor, slot 19 0x10008800, multiplies its alpha by it): the scoreboard line reads its fade
    constexpr int k_vt_surface_get_alpha_multiplier = 106;
    struct SurfaceVertex // vgui::Vertex_t
    {
        float x, y, u, v;
    };
    using SurfaceColorFn = void(__thiscall*)(void* surface, int r, int g, int b, int a);
    using SurfaceRectFn = void(__thiscall*)(void* surface, int x0, int y0, int x1, int y1);
    using SurfaceFontFn = void(__thiscall*)(void* surface, unsigned font);
    using SurfacePosFn = void(__thiscall*)(void* surface, int x, int y);
    using SurfacePrintFn = void(__thiscall*)(void* surface, const wchar_t* text, int length, int draw_type);
    using SurfaceTextureIdFn = int(__thiscall*)(void* surface, const char* file);
    using SurfaceTextureFileFn = void(__thiscall*)(void* surface, int id, const char* file, int hardware_filter, bool force_reload);
    using SurfaceSetTextureFn = void(__thiscall*)(void* surface, int id);
    using SurfaceNewTextureFn = int(__thiscall*)(void* surface, bool procedural);
    using SurfaceScreenSizeFn = void(__thiscall*)(void* surface, int& w, int& h);
    using SurfaceTextSizeFn = void(__thiscall*)(void* surface, unsigned font, const wchar_t* text, int& w, int& h);
    using SurfaceSoundFn = void(__thiscall*)(void* surface, const char* file);
    using SurfaceAlphaFn = float(__thiscall*)(void* surface);
    using SurfaceSubRectFn = void(__thiscall*)(void* surface, int x0, int y0, int x1, int y1, float s0, float t0, float s1, float t1);
    using SurfacePolygonFn = void(__thiscall*)(void* surface, int n, SurfaceVertex* vertices, bool clip);

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

    // weapon m_flNextPrimaryAttack (RecvProp at 0x10015974: push 0x960 / "m_flNextPrimaryAttack"; datamap 0x106BCBFC)
    constexpr std::uintptr_t k_cw_next_primary = 0x960;
    constexpr std::uintptr_t k_cw_next_secondary = 0x964; // m_flNextSecondaryAttack (ReloadOrSwitchWeapons compares both)
    // C_BaseCombatWeapon::ReloadOrSwitchWeapons + 0x72 (0x10017922): `mov eax, [gpGlobals]` (the client's
    // CGlobalVars*, at 0x106C49A8), then curtime (+0xC) compared with m_flNextPrimaryAttack (+0x960)
    constexpr std::uintptr_t k_rva_c_globals_read = 0x17922;
    // C_TerrorViewModel (primary vtable 0x105C1054, 230 slots) slot 222 CalcViewModelView(C_BasePlayer* owner, const
    // Vector& eye_position, const QAngle& eye_angles) (0x10288F40, thiscall, ret 0xC; terrorviewmodel.cpp): the
    // viewmodel's place each frame, from the eyes; it ends with SetLocalOrigin (0x10042C00) and SetLocalAngles
    // (0x10042CE0)
    constexpr std::uintptr_t k_rva_c_viewmodel_vtable = 0x5C1054;
    constexpr int k_c_vt_calc_viewmodel_view = 222;
    constexpr std::uintptr_t k_rva_c_calc_viewmodel_view = 0x288F40;
    using CalcViewModelViewFn = void(__fastcall*)(void* viewmodel, void* edx, void* owner, const Vec3* eye_position,
                                                  const Vec3* eye_angles);
    // C_TerrorMeleeWeapon's primary vtable (its complete object locator's offset 0)
    constexpr std::uintptr_t k_rva_c_melee_vtable = 0x609BFC;
    // CInput (vtable 0x1057575C) slot 2 GetButtonBits(bool reset) (0x10131570, thiscall, ret 4): the buttons of the
    // user command; CInput::CreateMove (slot 3, 0x101341F0) stores GetButtonBits(1) in CUserCmd::buttons (+0x24)
    constexpr std::uintptr_t k_rva_c_input_vtable = 0x57575C;
    constexpr int k_c_vt_get_button_bits = 2;
    constexpr std::uintptr_t k_rva_c_get_button_bits = 0x131570;
    using GetButtonBitsFn = int(__fastcall*)(void* input, void* edx, int reset);

    // Reload (slot 269 of the primary vtable): C_TerrorGun's (vtable 0x10609034 -> 0x1030C150) and C_PumpShotgun's
    // (0x105FF6FC -> 0x102F1CF0); HandleFireOnEmpty 0x100156D0
    constexpr int k_c_vt_reload = 269;
    constexpr std::uintptr_t k_rva_c_gun_vtable = 0x609034, k_rva_c_pump_vtable = 0x5FF6FC;
    constexpr std::uintptr_t k_rva_c_gun_reload = 0x30C150, k_rva_c_shotgun_reload = 0x2F1CF0;
    constexpr std::uintptr_t k_rva_c_fire_on_empty = 0x156D0;

} // namespace game::sdk
