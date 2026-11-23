#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>

class CBaseEntity;
class Vector;
class QAngle;
class CServerNetworkProperty;

namespace game::linux_sdk
{
    // Steam depot 222863, L4D2 2.2.4.3. Addresses are relative to server_srv.so's load bias.
    inline constexpr std::array<unsigned char, 20> server_build_id = {
        0xc9, 0xb0, 0xcd, 0x3d, 0x18, 0x00, 0xcb, 0x19, 0x08, 0x30,
        0x26, 0xf3, 0x49, 0xa9, 0xde, 0x15, 0x93, 0x3c, 0x05, 0xac};

    using WeaponInSlot = CBaseEntity* (*)(const CBaseEntity*, int);
    using ActiveWeapon = CBaseEntity* (*)(const CBaseEntity*);
    using Classname = const char* (*)(CBaseEntity*);
    using SelectItem = void (*)(CBaseEntity*, const char*, int);
    using EyeVectors = void (*)(CBaseEntity*, Vector*, Vector*, Vector*, Vector*);
    using Origin = const Vector& (*)(const CBaseEntity*);
    using Owner = CBaseEntity* (*)(const CBaseEntity*);
    using NetworkProperty = CServerNetworkProperty* (*)(CBaseEntity*);
    using MarkedForDeletion = bool (*)(const CServerNetworkProperty*);
    using Remove = void (*)(CBaseEntity*);
    using QuietDrop = void (*)(CBaseEntity*, CBaseEntity*, const Vector*, const Vector*);
    using GiveItem = CBaseEntity* (*)(CBaseEntity*, const char*, int, bool, CBaseEntity*);
    using CannotHandleItems = bool (*)(const CBaseEntity*);
    using SetAmmoCount = void (*)(CBaseEntity*, int, int);
    using Equip = void (*)(CBaseEntity*, CBaseEntity*);
    using NetworkChanged = void (*)(CBaseEntity*);
    using SetOrigin = void (*)(CBaseEntity*, const Vector&);
    using DispatchSpawn = int (*)(CBaseEntity*, bool);
    using PileUse = void (*)(CBaseEntity*, CBaseEntity*, CBaseEntity*, int, float);
    using DropToFloor = int (*)(CBaseEntity*, unsigned, CBaseEntity*);
    using Reload = bool (*)(CBaseEntity*);
    using FireOnEmpty = void (*)(CBaseEntity*);
    using BumpWeapon = bool (*)(CBaseEntity*, CBaseEntity*);
    using ZombieClass = int (*)(const CBaseEntity*);
    using SetShovePenalty = void (*)(CBaseEntity*, int);
    using AdrenalineActive = bool (*)(const CBaseEntity*);
    using OnTakeDamage = int (*)(CBaseEntity*, const void*);
    using SetGlowValue = void (*)(void*, int);
    using SetGlowColor = void (*)(void*, std::uint32_t);
    using ItemPostFrame = void (*)(CBaseEntity*);
    using ClipSize = int (*)(const CBaseEntity*);
    using OnRevived = void (*)(CBaseEntity*);
    using OnAdrenalineUsed = void (*)(CBaseEntity*, float);
    using PanicEvent = void (*)(void*, CBaseEntity*, int);
    using RescueCall = void (*)(CBaseEntity*, CBaseEntity*);
    using CommitSuicide = void (*)(CBaseEntity*, bool, bool);
    using SpawnSpecial = CBaseEntity* (*)(void*, int, const Vector&, const QAngle&);
    using PrecacheSound = void (*)(CBaseEntity*, const char*);
    using EmitSound = void (*)(CBaseEntity*, const char*, float, float*);
    using ViewPunch = void (*)(CBaseEntity*, const QAngle&);
    using StaggerPlayer = void (*)(CBaseEntity*, CBaseEntity*, const Vector*);
    using ShoveResponder = void (*)(void*, CBaseEntity*);
    using MeleeInfo = const void* (*)(const CBaseEntity*);
    using AnimationEvent = void (*)(CBaseEntity*, int, int);
    using WeaponSound = void (*)(CBaseEntity*, int, float);
    using Teleport = void (*)(CBaseEntity*, const Vector*, const QAngle*, const Vector*);
    using DamageInfo = void (*)(void*, CBaseEntity*, CBaseEntity*, CBaseEntity*, float, int, int);

    template <typename Function>
    struct Entry
    {
        std::uintptr_t offset;
        std::initializer_list<unsigned char> prologue;
    };

    inline constexpr Entry<WeaponInSlot> weapon_in_slot{0x3ee360, {0x55, 0x89, 0xe5, 0x57, 0x31, 0xff, 0x56, 0x53}};
    inline constexpr Entry<ActiveWeapon> active_weapon{0x3ee410, {0x55, 0x8b, 0x0d}};
    inline constexpr Entry<Classname> classname{0x607a90, {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x5d, 0x8b, 0x50, 0x7c}};
    inline constexpr Entry<SelectItem> select_item{0x406ee0, {0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x10}};
    inline constexpr Entry<EyeVectors> eye_vectors{0x406560, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x3c}};
    inline constexpr Entry<Origin> origin{0x6093a0, {0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14}};
    inline constexpr Entry<Owner> owner{0x3f5f30, {0x55, 0x8b, 0x0d}};
    inline constexpr Entry<NetworkProperty> network_property{0x3d0500, {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x5d, 0x83, 0xc0, 0x24}};
    inline constexpr Entry<MarkedForDeletion> marked_for_deletion{0x8114e0, {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x5d, 0x8b, 0x40, 0x08}};
    inline constexpr Entry<Remove> remove{0xb199d0, {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x85, 0xc0}};
    inline constexpr Entry<QuietDrop> quiet_drop{0x7afb80, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53}};
    inline constexpr Entry<QuietDrop> weapon_drop{0x9c6480, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c}};
    inline constexpr Entry<GiveItem> give_item{0x98b820, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0x5c, 0x01}};
    inline constexpr Entry<CannotHandleItems> incapacitated{0x9349f0, {0x55, 0x31, 0xc0, 0x89, 0xe5, 0x8b, 0x55, 0x08}};
    inline constexpr Entry<CannotHandleItems> dominated{0x4e63b0, {0x55, 0x89, 0xe5, 0x56, 0x8b, 0x55, 0x08, 0x53}};
    inline constexpr Entry<SetAmmoCount> set_ammo_count{0x3ee5e0, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x8d, 0x45, 0x0c}};
    inline constexpr Entry<Equip> equip{0x5e68a0, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x31, 0xf6}};
    inline constexpr Entry<NetworkChanged> network_changed{0x7bd6a0, {0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14}};
    inline constexpr Entry<Classname> melee_script_name{0x5580a0, {0x55, 0x89, 0xe5, 0x83, 0xec, 0x18}};
    inline constexpr Entry<SetOrigin> set_origin{0x5fc720, {0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x50}};
    inline constexpr Entry<DispatchSpawn> dispatch_spawn{0xb1dce0, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c}};
    inline constexpr Entry<PileUse> pile_use{0x8ccf10, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c}};
    inline constexpr Entry<DropToFloor> drop_to_floor{0xb19230, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0x8c}};
    inline constexpr Entry<PileUse> weapon_spawn_use{0x8c6700, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x5c}};
    inline constexpr Entry<Classname> spawned_weapon_name{0x8c6c00, {0x55, 0x89, 0xe5, 0x83, 0xec, 0x18}};
    inline constexpr Entry<Reload> gun_reload{0x555380, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c}};
    inline constexpr Entry<Reload> shotgun_reload{0x517780, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c}};
    inline constexpr Entry<FireOnEmpty> fire_empty{0x3f4a90, {0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14}};
    inline constexpr Entry<BumpWeapon> bump_weapon{0x98c240, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c}};
    inline constexpr Entry<PileUse> gun_use{0x554da0, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c}};
    inline constexpr Entry<Classname> melee_spawn_name{0x8c6c60, {0x55, 0x89, 0xe5, 0x83, 0xec, 0x18}};
    inline constexpr Entry<ZombieClass> zombie_class{0x9d4220, {0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14}};
    inline constexpr Entry<SetShovePenalty> set_shove_penalty{0x4e3880, {0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x10}};
    inline constexpr Entry<AdrenalineActive> adrenaline_active{0x4e6620, {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x5d, 0x0f, 0xb6}};
    inline constexpr Entry<SetGlowValue> set_glow_state{0x455e40, {0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x10}};
    inline constexpr Entry<SetGlowValue> set_glow_range{0x455e80, {0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x10}};
    inline constexpr Entry<SetGlowValue> set_glow_minimum{0x455eb0, {0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x10}};
    inline constexpr Entry<SetGlowColor> set_glow_color{0x455ef0, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x1c}};
    inline constexpr Entry<ItemPostFrame> chainsaw_frame{0x5233a0, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0xac, 0x01}};
    inline constexpr Entry<OnRevived> on_revived{0x9b36b0, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x6c}};
    inline constexpr Entry<CannotHandleItems> hanging_from_ledge{0x9d3fd0, {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x5d}};
    inline constexpr Entry<OnAdrenalineUsed> on_adrenaline_used{0x989450, {0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x10}};
    inline constexpr Entry<PanicEvent> panic_event{0x865130, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x1c}};
    inline constexpr Entry<RescueCall> rescue_call{0x959fc0, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x5c}};
    inline constexpr Entry<CommitSuicide> commit_suicide{0x799a10, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0x8c}};
    inline constexpr Entry<SpawnSpecial> spawn_special{0xa48f80, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x1c}};
    inline constexpr Entry<PrecacheSound> precache_sound{0x4a3150, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x4c}};
    inline constexpr Entry<EmitSound> emit_sound{0x4a46f0, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0xdc}};
    inline constexpr Entry<ItemPostFrame> melee_primary{0x55b6b0, {0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x10}};
    inline constexpr Entry<ItemPostFrame> melee_secondary{0x558fd0, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x1c}};
    inline constexpr Entry<ViewPunch> view_punch{0x408750, {0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x24}};
    inline constexpr Entry<StaggerPlayer> stagger_player{0x99e650, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x5c}};
    inline constexpr Entry<ShoveResponder> shove_responder{0x3d26e0, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x1c}};
    inline constexpr Entry<MeleeInfo> melee_info{0x558cf0, {0x55, 0x89, 0xe5, 0x83, 0xec, 0x18, 0x8b, 0x45, 0x08}};
    inline constexpr Entry<AnimationEvent> animation_event{0x986a80, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x1c}};
    inline constexpr Entry<WeaponSound> weapon_sound{0x3f6620, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0x1c, 0x01}};
    inline constexpr Entry<Teleport> teleport{0x605310, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0x9c}};
    inline constexpr Entry<DamageInfo> damage_info{0x4b7a60, {0x55, 0x89, 0xe5, 0x83, 0xec, 0x38, 0x8b, 0x45, 0x08}};
    inline constexpr Entry<OnTakeDamage> take_damage{0x5f5410, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x7c}};

    inline constexpr std::uintptr_t pile_use_slot = 0xce8c14;
    inline constexpr std::uintptr_t bump_weapon_slot = 0xd0b0f4;
    inline constexpr std::uintptr_t chainsaw_post_frame_slot = 0xc2608c;
    inline constexpr std::uintptr_t director = 0xfeff08;
    inline constexpr std::uintptr_t zombie_manager = 0x10170e0;
    inline constexpr std::uintptr_t entity_list = 0xf201bc;
    inline constexpr std::uintptr_t melee_primary_slot = 0xc41b54;
    inline constexpr std::uintptr_t melee_secondary_slot = 0xc41b58;
    inline constexpr std::uintptr_t melee_sound_slot = 0xc41ba0;
    inline constexpr std::uintptr_t read_only_data_start = 0xbe9740;
    inline constexpr std::uintptr_t read_only_data_end = 0xd486f8;
    inline constexpr std::uintptr_t text_start = 0x3656f0;
    inline constexpr std::uintptr_t text_end = 0xbe9724;
    inline constexpr int use_index = 107;
    inline constexpr int reload_index = 282;
    inline constexpr int fire_empty_index = 277;
    inline constexpr int damage_index = 73;
    inline constexpr int max_clip_index = 324;
    inline constexpr int melee_world_hit_sound = 10;
    inline constexpr int animation_gesture = 66;
    inline constexpr std::size_t damage_info_size = 0x60;
    inline constexpr std::uintptr_t damage_info_inflictor = 0x30;
    inline constexpr std::uintptr_t damage_info_attacker = 0x34;
    inline constexpr std::uintptr_t damage_info_weapon = 0x38;
    inline constexpr std::uintptr_t damage_info_value = 0x3c;
    inline constexpr std::uintptr_t entity_handle = 0x3b4;
    inline constexpr std::uintptr_t glow = 0x1f0;
    inline constexpr std::uintptr_t life_state = 0x104;
    inline constexpr std::uintptr_t health = 0x100;
    inline constexpr std::uintptr_t max_health = 0xfc;
    inline constexpr std::uintptr_t team = 0x24c;
    inline constexpr std::uintptr_t hit_group = 0x1838;
    inline constexpr std::uintptr_t infected_nextbot = 0x1a3c;
    inline constexpr std::uintptr_t infected_push_only = 0x1d90;
    inline constexpr std::uintptr_t melee_next_sequence = 0x17fc;
    inline constexpr std::uintptr_t melee_sequence_time = 0x182c;
    inline constexpr std::uintptr_t melee_activities = 0xc20;
    inline constexpr std::uintptr_t melee_activity_count = 0xc2c;
    inline constexpr std::size_t melee_activity_size = 0x30;
    inline constexpr std::uintptr_t melee_activity_idle = 0x08;
    inline constexpr std::uintptr_t melee_activity_direction = 0x0c;
    inline constexpr std::uintptr_t survivor_character = 0x2ba0;
    inline constexpr std::uintptr_t corpse_character = 0x17d8;
    inline constexpr std::uintptr_t spawn_count = 0x144c;
    inline constexpr std::uintptr_t clip = 0x1420;
    inline constexpr std::uintptr_t in_reload = 0x1459;
    inline constexpr std::uintptr_t fire_on_empty = 0x145a;
    inline constexpr std::uintptr_t buttons = 0x1cb0;
    inline constexpr std::uintptr_t flags = 0x150;
    inline constexpr std::uintptr_t local_velocity = 0x2ec;
    inline constexpr std::uintptr_t absolute_velocity = 0x274;
    inline constexpr std::uintptr_t lagged_movement = 0x20cc;
    inline constexpr std::uintptr_t next_primary = 0x13fc;
    inline constexpr std::uintptr_t next_secondary = 0x1400;
    inline constexpr std::uintptr_t shove_penalty = 0x2cdc;
    inline constexpr std::uintptr_t progress_start = 0x2904;
    inline constexpr std::uintptr_t progress_duration = 0x2908;
    inline constexpr std::uintptr_t primary_ammo_type = 0x1418;
    inline constexpr std::uintptr_t weapon_owner = 0x13f4;
    inline constexpr std::uintptr_t extra_primary_ammo = 0x14fc;
    inline constexpr std::uintptr_t player_ammo = 0x1874;
    inline constexpr std::uintptr_t upgrades = 0x17e4;
    inline constexpr std::uintptr_t special_rounds = 0x1790;
}
