// The real backend: puts items into the player's hands, onto the ground and back into the inventory.
//
// Everything here was read out of server.dll build 2.2.4.3 (PE timestamp 0x6A440E53) with tools/re/pe_tools.py; init()
// refuses to run on any other build.  What each number is:
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
//
// The game's code is not thread safe: every call into it happens in pump(), which runs on the game's main thread
// (the window procedure gets k_msg_pump).

#include "game/game.hpp"

#include "core/config.hpp"
#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/mem.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace game
{

    namespace
    {
        constexpr std::uint32_t k_server_timestamp = 0x6A440E53;
        constexpr std::uintptr_t k_rva_player_vtable = 0x6973B4;
        constexpr std::uintptr_t k_rva_ent_create = 0x57690;
        // client.dll: CHudWeaponSelection's CHudElement vtable and its ShouldDraw (slot 12)
        constexpr std::uintptr_t k_rva_weapon_select_vtable = 0x5D5A84;
        constexpr std::uintptr_t k_rva_weapon_select_should_draw = 0x2CE900;
        constexpr std::uintptr_t k_rva_weapon_select_panel_vtable = 0x5D5734; // its vgui::Panel part
        constexpr std::uintptr_t k_rva_weapon_select_paint = 0x2D11E0; // slot 128 of that vtable
        constexpr int k_vt_use_pickup = 426;
        // map item spawners (weapon_*_spawn): see "Map item spawners" in docs/game-notes.md
        constexpr std::uintptr_t k_rva_spawn_vtable = 0x6796E4; // CWeaponSpawn (CWeaponSpawnConfigurable shares its Use)
        constexpr int k_vt_entity_use = 106; // CBaseEntity::Use(activator, caller, type, value)
        constexpr int k_vt_spawn_weapon_id = 240; // the spawner's weapon ID
        constexpr std::uintptr_t k_rva_weapon_get_slot = 0x2C98F0; // CTerrorPlayer: the carried weapon of a weapon ID's slot
        constexpr std::uintptr_t k_rva_weapon_id_name = 0x24EB70; // cdecl: weapon ID -> "weapon_first_aid_kit"
        constexpr int k_vt_tools_set_key_value = 13; // IServerTools::SetKeyValue(entity, field, const char*)
        constexpr int k_vt_precache = 26;
        constexpr int k_vt_teleport = 118;
        constexpr std::uintptr_t k_off_my_weapons = 0x18F4;
        constexpr int k_max_weapons = 56;
        constexpr std::uintptr_t k_off_active_weapon = 0x19D4;
        constexpr std::uintptr_t k_off_weapon_owner = 0x13E8; // CBaseCombatWeapon::m_hOwner (SendProp: push 0x13E8 / "m_hOwner")
        constexpr int k_vt_give_named_item = 431;
        constexpr int k_vt_weapon_drop = 283; // Weapon_Drop(weapon, target, velocity): the VScript DropItem calls it so (0x102FBCD0)
        constexpr int k_vt_cannot_drop = 401; // checked first by DropItem: non-zero means no
        constexpr int k_vt_select_item = 429; // what the `use <weapon>` console command calls (server.dll 0x1008ED30)
        constexpr int k_vt_eye_position = 140;
        constexpr int k_vt_eye_angles = 141;
        constexpr std::uint32_t k_invalid_handle = 0xFFFFFFFFu;
        constexpr std::uintptr_t k_off_origin = 0x388; // CBaseEntity::m_vecOrigin (the SendProps: push 0x388 / push 0xC)
        // A weapon's rounds (SendProp / datamap definitions: push <offset> / push "<name>"), for storing guns:
        constexpr std::uintptr_t k_off_clip = 0x1414; // CBaseCombatWeapon::m_iClip1
        constexpr std::uintptr_t k_off_ammo_type = 0x140C; // CBaseCombatWeapon::m_iPrimaryAmmoType
        constexpr std::uintptr_t k_off_extra_ammo = 0x14F4; // CTerrorGun::m_iExtraPrimaryAmmo: a gun's reserve while it lies
        constexpr std::uintptr_t k_off_player_ammo = 0x1874; // CBasePlayer::m_iAmmo[32]: the reserve of each ammo type
        // A gun's upgrades (SendProps: push 0x17E0 / "m_upgradeBitVec" in CTerrorGun's table; push 0x1788 /
        // "m_nUpgradedPrimaryAmmoLoaded" in CTerrorWeapon's LocalL4DWeaponData): 1 incendiary, 2 explosive, 4 laser
        // sight, and the incendiary / explosive rounds still loaded.
        constexpr std::uintptr_t k_off_upgrades = 0x17E0;
        constexpr std::uintptr_t k_off_special_loaded = 0x1788;
        // CTerrorMeleeWeapon::m_strMapSetScriptName (datamap entry 0x107BDF20: FIELD_STRING, keyvalue
        // "melee_script_name"): which melee weapon it is, "fireaxe", "katana" ...
        constexpr std::uintptr_t k_off_melee_name = 0x17E0;
        constexpr float k_pickup_reach = 160.f;

        struct Vec3
        {
            float x, y, z;
        };

        using CreateInterfaceFn = void*(__cdecl*)(const char* name, int* return_code);
        using GetKeyValueFn = bool(__thiscall*)(void* tools, void* entity, const char* field, char* out, int length);
        using SetKeyValueFn = bool(__thiscall*)(void* tools, void* entity, const char* field, const char* value);
        using CreateEntityFn = void*(__thiscall*)(void* tools, const char* classname);
        using DispatchSpawnFn = void(__thiscall*)(void* tools, void* entity);
        using RemoveFn = void(__cdecl*)(void* entity);
        using PrecacheFn = void(__thiscall*)(void* entity);
        using TeleportFn = void(__thiscall*)(void* entity, const Vec3* position, const Vec3* angles,
                                             const Vec3* velocity);
        using DropToFloorFn = int(__cdecl*)(void* entity, unsigned mask, void* ignore);
        using GiveFn = void*(__thiscall*)(void* player, const char* name, int a, int b, int c);
        using WeaponDropFn = void(__thiscall*)(void* player, void* weapon, const Vec3* target, const Vec3* velocity);
        using CannotDropFn = int(__thiscall*)(void* player);
        using SelectItemFn = void*(__thiscall*)(void* player, const char* classname, int subtype);
        using EyePositionFn = Vec3*(__thiscall*)(void* player, Vec3* out);
        using EyeAnglesFn = const Vec3*(__thiscall*)(void* player);

        struct Env
        {
            mem::ModuleInfo server;
            void* tools = nullptr;
            std::uintptr_t ent_list = 0;
            std::uintptr_t player_vtable = 0;
            GetKeyValueFn get_key_value = nullptr;
            SetKeyValueFn set_key_value = nullptr;
            CreateEntityFn create_entity = nullptr;
            DispatchSpawnFn dispatch_spawn = nullptr;
            RemoveFn util_remove = nullptr;
            DropToFloorFn drop_to_floor = nullptr; // optional
            WeaponDropFn quiet_drop = nullptr; // CBaseCombatCharacter::Weapon_Drop: no "weapon_drop" event (optional)
            void* vgui_input = nullptr; // vgui2.dll's VGUI_Input005, probed for keyboard focus (optional)
        } g;

        // ---- entities -----------------------------------------------------------------------------------------------

        struct EntInfo
        {
            void* entity;
            std::uint32_t serial;
            void* prev;
            void* next;
        };

        void* entity_at_index(int index)
        {
            EntInfo info{};
            if (index < 0 || index >= 0x1000 ||
                !mem::read(g.ent_list + 4 + static_cast<std::uintptr_t>(index) * sizeof(EntInfo), info))
                return nullptr;
            return info.entity;
        }

        void* entity_from_handle(std::uint32_t handle)
        {
            if (handle == k_invalid_handle)
                return nullptr;
            EntInfo info{};
            if (!mem::read(g.ent_list + 4 + static_cast<std::uintptr_t>(handle & 0xFFF) * sizeof(EntInfo), info))
                return nullptr;
            return info.serial == (handle >> 12) ? info.entity : nullptr;
        }

        // An entity's EHANDLE, by a scan of the list (k_invalid_handle: not there).
        std::uint32_t handle_of(void* entity)
        {
            for (int index = 0; index < 0x1000; ++index)
            {
                EntInfo info{};
                if (mem::read(g.ent_list + 4 + static_cast<std::uintptr_t>(index) * sizeof(EntInfo), info) && info.entity == entity)
                    return static_cast<std::uint32_t>(index) | (info.serial << 12);
            }
            return k_invalid_handle;
        }

        // The game code works on "the local player".  That is the host on a listen server (entity 1).  While it serves
        // another player (every player in a dedicated server, a guest on a listen server; see "Other players" below)
        // it is that player.  Main thread only.
        bool g_dedicated = false;
        void* g_engine_server = nullptr; // engine.dll VEngineServer022 (see "other players")
        void* g_active_player = nullptr;

        // The listen server's host: entity 1, when it is a player; nullptr in a dedicated server or out of a game.
        void* host_player()
        {
            if (g_dedicated)
                return nullptr;
            void* player = entity_at_index(1);
            if (player == nullptr || mem::read_or<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(player)) != g.player_vtable)
                return nullptr;
            return player;
        }

        void* local_player()
        {
            return g_active_player != nullptr ? g_active_player : host_player();
        }

        bool classname_of(void* entity, char* out, int length)
        {
            out[0] = '\0';
            bool ok = false;
            if (!mem::guarded([&] { ok = g.get_key_value(g.tools, entity, "classname", out, length); }))
                return false;
            out[length - 1] = '\0';
            return ok && out[0] != '\0';
        }

        // The inventory item an entity is, if it is one.
        const inv::ItemDef* item_of(void* entity)
        {
            char name[64];
            return entity != nullptr && classname_of(entity, name, sizeof(name)) ? inv::find_item(name) : nullptr;
        }

        std::uint32_t handle_at(void* player, std::uintptr_t offset)
        {
            return mem::read_or<std::uint32_t>(reinterpret_cast<std::uintptr_t>(player) + offset, k_invalid_handle);
        }

        // What the player carries, as the inventory's items (the primary and secondary weapons are not in the catalogue).
        std::string carried_list(void* player)
        {
            std::string out;
            for (int i = 0; i < k_max_weapons; ++i)
            {
                void* weapon = entity_from_handle(handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i)));
                if (weapon == nullptr)
                    continue;
                char name[64];
                if (classname_of(weapon, name, sizeof(name)))
                    out += std::string(out.empty() ? "" : ", ") + name;
            }
            return out;
        }

        bool player_has_slot(void* player, int game_slot)
        {
            for (int i = 0; i < k_max_weapons; ++i)
            {
                const inv::ItemDef* def = item_of(
                    entity_from_handle(handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i))));
                if (def != nullptr && def->game_slot == game_slot)
                    return true;
            }
            return false;
        }

        // ---- the actions --------------------------------------------------------------------------------------------

        void take_from_inventory(const inv::ItemDef& def, int count)
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            s.inventory.remove_item(def, count);
        }

        void do_select_weapon(const std::string& classname);

        // The catalogue item the player carries in `game_slot`, or nullptr.
        void* carried_in_slot(void* player, int game_slot, const inv::ItemDef** def_out)
        {
            for (int i = 0; i < k_max_weapons; ++i)
            {
                void* weapon = entity_from_handle(handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i)));
                const inv::ItemDef* def = item_of(weapon);
                if (def != nullptr && def->game_slot == game_slot)
                {
                    *def_out = def;
                    return weapon;
                }
            }
            return nullptr;
        }

        // ---- guns and melee weapons --------------------------------------------------------------------------------
        //
        // A stored weapon keeps its magazine (a melee weapon: which one it is).  The reserve is the player's: m_iAmmo of
        // each ammo type, shared by every gun that takes it (M16, AK-47, SCAR and SG552 share rifle rounds ...), the way
        // the game shares it between the guns a player picks up.  So a stored gun leaves its reserve with the player, a
        // gun taken out uses what the player has of its type, and a gun picked up off the ground into a box adds what it
        // carried (m_iExtraPrimaryAmmo) to the player's.

        // What `weapon` is in: its magazine, and for a gun lying in the world the reserve it carries (-1: none, or a
        // fresh gun from the map, which has 0 there).  A carried gun's reserve is the player's, so not part of it.
        // A gun (CTerrorGun): not the melee weapon or the chainsaw.
        bool is_gun(const inv::ItemDef& def)
        {
            return def.weapon() && std::strcmp(def.classname, "weapon_melee") != 0 && std::strcmp(def.classname, "weapon_chainsaw") != 0;
        }

        void state_changed(void* entity); // see "stamina"
        void know_gun(void* weapon); // see "the map's guns"
        int team_of(void* entity); // see "the infection"

        // A gun's upgrades and special rounds onto `weapon` (one just given, or dropped into the world).
        void write_upgrades(void* weapon, const inv::ItemDef& def, const inv::WeaponState& state)
        {
            if (!is_gun(def) || (state.upgrades == 0 && state.special == 0))
                return;
            const auto w = reinterpret_cast<std::uintptr_t>(weapon);
            if (mem::guarded([&] {
                    *reinterpret_cast<int*>(w + k_off_upgrades) = state.upgrades;
                    *reinterpret_cast<int*>(w + k_off_special_loaded) = state.special;
                }))
                state_changed(weapon);
        }

        inv::WeaponState weapon_state(void* player, void* weapon, const inv::ItemDef& def, bool carried)
        {
            (void)player;
            const bool is_melee = std::strcmp(def.classname, "weapon_melee") == 0;
            inv::WeaponState state;
            const auto w = reinterpret_cast<std::uintptr_t>(weapon);
            mem::guarded([&] {
                state.clip = mem::read_or<int>(w + k_off_clip, -1);
                if (!carried && !is_melee)
                    if (const int extra = mem::read_or<int>(w + k_off_extra_ammo, 0); extra > 0)
                        state.ammo = extra;
                char name[64] = {};
                if (const char* melee = is_melee ? mem::read_or<const char*>(w + k_off_melee_name) : nullptr;
                    melee != nullptr && mem::safe_copy(name, melee, sizeof(name) - 1) && name[0] != '\0')
                    state.melee = name;
                if (is_gun(def))
                {
                    state.upgrades = mem::read_or<int>(w + k_off_upgrades, 0) & 7;
                    state.special = std::max(0, mem::read_or<int>(w + k_off_special_loaded, 0));
                }
            });
            return state;
        }

        int ammo_type_of(void* weapon)
        {
            const int type = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_ammo_type, -1);
            return type >= 0 && type < 32 ? type : -1;
        }

        // The player's reserve of every ammo type.
        struct AmmoPool
        {
            int count[32] = {};
            bool read = false;
        };

        AmmoPool read_pool(void* player)
        {
            AmmoPool pool;
            pool.read = mem::safe_copy(pool.count, reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(player) + k_off_player_ammo),
                                       sizeof(pool.count));
            return pool;
        }

        // Puts the reserve back as it was: whatever the game did to it while weapons were dropped and given (a given
        // gun would come with the game's own amount, a dropped one may take the reserve along).
        void write_pool(void* player, const AmmoPool& pool)
        {
            if (!pool.read)
                return;
            mem::guarded([&] {
                std::memcpy(reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(player) + k_off_player_ammo), pool.count,
                            sizeof(pool.count));
            });
        }

        // `rounds` of ammo type `type` (the ammo of gun `classname`) into the player's reserve, up to the most a player
        // can carry of it (inv::ammo_max: the rules file's [ammo], else the game's ammo_<type>_max default).
        void add_to_pool(void* player, const char* classname, int type, int rounds)
        {
            if (player == nullptr || type < 0 || type >= 32 || rounds <= 0)
                return;
            const char* kind = classname != nullptr ? inv::ammo_kind(classname) : nullptr;
            const int max = kind != nullptr ? inv::ammo_max(kind) : -1;
            mem::guarded([&] {
                int& reserve = *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + k_off_player_ammo +
                                                       4u * static_cast<unsigned>(type));
                const int before = reserve;
                reserve = max >= 0 ? std::min(before + rounds, std::max(before, max)) : before + rounds;
                if (reserve - before < rounds)
                    INV_LOG("ammo: %d of %d %s rounds taken (the most is %d)", reserve - before, rounds, kind, max);
            });
        }

        // A stored weapon's state onto the one the player was just given: its magazine; a reserve of its own (a gun
        // stored before the reserves were shared) joins the player's.
        void apply_weapon_state(void* player, void* weapon, const inv::WeaponState& state)
        {
            mem::guarded([&] {
                if (state.clip >= 0)
                    *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_clip) = state.clip;
            });
            char name[64];
            const bool named = classname_of(weapon, name, sizeof(name));
            if (const inv::ItemDef* def = named ? inv::find_item(name) : nullptr)
                write_upgrades(weapon, *def, state); // incendiary / explosive rounds, laser sight
            add_to_pool(player, named ? name : nullptr, ammo_type_of(weapon), state.ammo);
        }

        // What GiveNamedItem gets for a stored weapon: a melee weapon by its script name (as `give fireaxe` does).
        std::string give_name(const inv::ItemDef& def, const inv::WeaponState& state)
        {
            return def.weapon() && !state.melee.empty() ? state.melee : std::string(def.classname);
        }

        // Takes one stored `def` out of inventory slot `slot`; false when the slot no longer has one (the HUD changed it).
        bool take_from_slot(int slot, const inv::ItemDef& def)
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            return s.inventory.slot(slot).def == &def && s.inventory.remove_from_slot(slot, 1) == 1;
        }

        // Every item keeps its own box.  An item in the player's hands is shown in its box (Stack::held); when it leaves
        // the hands for the inventory it goes back into that box, and an item taken in hand from a box makes that box its
        // home.  So nothing moves between boxes when the player switches items.

        // Takes `weapon` (carried in `def`'s game slot) off the player and back into its box (or anywhere it fits when
        // it has none).  UTIL_Remove alone is not enough: it only deletes at the end of the frame, so the weapon would
        // still be in m_hMyWeapons when GiveNamedItem runs right after, and the game would drop what occupies the slot
        // onto the ground.  Weapon_Drop (slot 283) takes it out of m_hMyWeapons now; the dropped entity is then removed.
        bool swap_out(void* player, void* weapon, const inv::ItemDef& def)
        {
            if (const auto cannot = reinterpret_cast<CannotDropFn>(mem::vslot(player, k_vt_cannot_drop));
                cannot != nullptr && cannot(player) != 0)
                return false;
            // the base class's Weapon_Drop: CTerrorPlayer's own one also fires "weapon_drop", and the client then pins a
            // "dropped item" icon in the world where the item never was
            const auto drop = g.quiet_drop != nullptr ? g.quiet_drop
                                                      : reinterpret_cast<WeaponDropFn>(mem::vslot(player, k_vt_weapon_drop));
            if (drop == nullptr)
                return false;
            int home = -1;
            bool full = false;
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                home = s.inventory.held_slot(def);
                full = home < 0 && s.inventory.add(def, 1) == 0;
            }
            if (full)
            {
                notify(refusal(def)); // notify() takes the session lock itself
                return false;
            }
            if (!mem::guarded([&] { drop(player, weapon, nullptr, nullptr); }) || player_has_slot(player, def.game_slot))
            {
                if (home < 0)
                    take_from_inventory(def, 1);
                return false;
            }
            mem::guarded([&] { g.util_remove(weapon); });
            if (home >= 0)
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                s.inventory.release_held(home);
            }
            INV_LOG("swap: %s went back into box %d", def.classname, home);
            return true;
        }

        // Takes the carried weapon `weapon` (a gun or melee weapon, `def`) off the player into inventory box `index`
        // (-1: the first empty one), with its rounds; the player keeps none of its ammo type.  Weapon_Drop first, as
        // in swap_out, so it is out of m_hMyWeapons before anything is given.  The box, or -1 (said why).
        int store_carried_weapon(void* player, void* weapon, const inv::ItemDef& def, int index = -1)
        {
            if (const auto cannot = reinterpret_cast<CannotDropFn>(mem::vslot(player, k_vt_cannot_drop));
                cannot != nullptr && cannot(player) != 0)
            {
                notify("The game does not let you put that away now");
                return -1;
            }
            const auto drop = g.quiet_drop != nullptr ? g.quiet_drop
                                                      : reinterpret_cast<WeaponDropFn>(mem::vslot(player, k_vt_weapon_drop));
            if (drop == nullptr)
                return -1;
            const inv::WeaponState state = weapon_state(player, weapon, def, true);
            int box = -1;
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                // it leaves the hands for the box: what it weighs in the hands does not count twice
                const float carried = s.inventory.weapons_weight();
                s.inventory.set_weapons_weight(std::max(0.f, carried - def.weight));
                box = s.inventory.add_weapon(def, state, index);
                s.inventory.set_weapons_weight(carried);
            }
            if (box < 0)
            {
                notify(index >= 0 ? std::string("No room for your ") + def.name : refusal(def));
                return -1;
            }
            // the reserve stays the player's (shared by the guns of its ammo type), whatever the drop does with it
            const AmmoPool pool = read_pool(player);
            const bool dropped = mem::guarded([&] { drop(player, weapon, nullptr, nullptr); }) && !player_has_slot(player, def.game_slot);
            write_pool(player, pool);
            if (!dropped)
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                s.inventory.remove_from_slot(box, 1);
                return -1;
            }
            mem::guarded([&] { g.util_remove(weapon); });
            INV_LOG("stored %s (%s) into box %d: magazine %d, upgrades %d, %d special rounds (the reserve stays with the player)",
                    def.classname, state.melee.c_str(), box, state.clip, state.upgrades, state.special);
            return box;
        }

        // A carried item (gas can, propane / oxygen tank, fireworks, gnome, cola: game slot 5) cannot stay with the
        // player while the hands hold something else: the game drops it as soon as they switch.  Before the mod switches
        // (wheel, keys, the item boxes), it goes back into its box instead, the way the store key puts it away; with no
        // room for it the game drops it as before.
        void put_away_carried(void* player, const char* switching_to)
        {
            void* active = entity_from_handle(handle_at(player, k_off_active_weapon));
            const inv::ItemDef* def = item_of(active);
            if (def == nullptr || def->game_slot != 5 || (switching_to != nullptr && std::strcmp(switching_to, def->classname) == 0))
                return;
            if (swap_out(player, active, *def))
                INV_LOG("switching to %s: %s put away", switching_to != nullptr ? switching_to : "?", def->classname);
        }

        bool g_giving = false; // main thread only: do_use is handing an item out
        bool g_switching = false; // main thread only: do_use is swapping / handing out (game events are muted)
        ULONGLONG g_switch_mute_until = 0; // main thread only: "weapon_pickup" is muted until then (see hk_create_event)

        // engine.dll's IGameEventManager2 (GAMEEVENTSMANAGER002, vtable 0x10388774) slot 6 CreateEvent(name, force,
        // cookie), ret 0xC.  The server's code checks the result for null and then skips firing.  While the mod swaps
        // items the events the game creates for it ("item_pickup", ...) are not real pickups, and the client answers
        // them with item icons floating in the world, so they are not created at all.
        using CreateEventFn = void*(__fastcall*)(void* self, void* edx, const char* name, bool force, int* cookie);
        core::Hook<CreateEventFn> g_create_event;

        void* __fastcall hk_create_event(void* self, void* edx, const char* name, bool force, int* cookie)
        {
            if (g_switching && name != nullptr)
            {
                INV_LOG("event %s muted during a switch", name);
                return nullptr;
            }
            // the client's own "weapon_pickup" comes a frame or two later, when the new weapon reaches it; that is the
            // one the floating pickup icon belongs to
            if (name != nullptr && GetTickCount64() < g_switch_mute_until && std::strcmp(name, "weapon_pickup") == 0)
            {
                INV_LOG("event %s muted after a switch", name);
                return nullptr;
            }
            return g_create_event.original()(self, edx, name, force, cookie);
        }

        // A stored gun or melee weapon into the hands.  The one in its game slot goes into the same box (the two swap
        // places), then the stored one is given and gets its rounds back.
        void do_use_weapon(void* player, int slot)
        {
            const inv::ItemDef* def = nullptr;
            inv::WeaponState state;
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                const inv::Stack& stack = s.inventory.slot(slot);
                if (stack.empty() || stack.def == nullptr || !stack.def->weapon())
                    return;
                def = stack.def;
                state = stack.weapon;
                s.inventory.remove_from_slot(slot, 1); // the box is free for the weapon it replaces
            }
            const auto put_back = [&] {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                if (s.inventory.add_weapon(*def, state, slot) < 0 && s.inventory.add_weapon(*def, state) < 0)
                    INV_LOG("use %s: no box to put it back in", def->classname); // (cannot happen: the box was just freed)
            };
            INV_LOG("use %s (box %d, %s): the player carries [%s]", def->classname, slot, state.melee.c_str(),
                    carried_list(player).c_str());
            g_switching = true;
            struct Unmute
            {
                ~Unmute()
                {
                    g_switching = false;
                    g_switch_mute_until = GetTickCount64() + 500;
                }
            } unmute;
            const inv::ItemDef* held_def = nullptr;
            if (void* held = carried_in_slot(player, def->game_slot, &held_def))
            {
                if (store_carried_weapon(player, held, *held_def, slot) < 0)
                {
                    put_back();
                    return;
                }
            }
            const auto give = reinterpret_cast<GiveFn>(mem::vslot(player, k_vt_give_named_item));
            const std::string name = give_name(*def, state);
            // the gun takes what the player has of its ammo type: not the game's own amount for a given gun
            const AmmoPool pool = read_pool(player);
            g_giving = true;
            const bool gave = give != nullptr && mem::guarded([&] { give(player, name.c_str(), 0, 0, 0); });
            g_giving = false;
            write_pool(player, pool);
            const inv::ItemDef* given_def = nullptr;
            void* given = carried_in_slot(player, def->game_slot, &given_def);
            if (!gave || given == nullptr)
            {
                // the one that went into the box stays there; this one goes into the first free box
                Session& s = session();
                std::lock_guard lock(s.mutex);
                if (s.inventory.add_weapon(*def, state, slot) < 0 && s.inventory.add_weapon(*def, state) < 0)
                    INV_LOG("use %s: the game refused it and no box is free: it is lost", def->classname);
                notify(std::string("The game refused the ") + def->name);
                return;
            }
            apply_weapon_state(player, given, state);
            if (state.upgrades != 0 || state.special != 0)
                INV_LOG("upgrades back on %s: %d (now %d), %d special rounds (now %d)", given_def->classname, state.upgrades,
                        mem::read_or<int>(reinterpret_cast<std::uintptr_t>(given) + k_off_upgrades),
                        state.special, mem::read_or<int>(reinterpret_cast<std::uintptr_t>(given) + k_off_special_loaded));
            do_select_weapon(given_def->classname);
            notify(std::string("Equipped ") + (state.melee.empty() ? def->name : state.melee));
        }

        void do_use(int slot)
        {
            const inv::ItemDef* def = nullptr;
            bool stored = false;
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                const inv::Stack& stack = s.inventory.slot(slot);
                if (stack.empty())
                    return;
                def = stack.def;
                stored = stack.count > 0;
            }
            void* player = local_player();
            if (player == nullptr)
            {
                notify("Not in a game");
                return;
            }
            put_away_carried(player, def->classname);
            if (def->weapon())
            {
                do_use_weapon(player, slot);
                return;
            }
            INV_LOG("use %s (box %d): the player carries [%s]", def->classname, slot, carried_list(player).c_str());
            g_switching = true;
            struct Unmute
            {
                ~Unmute()
                {
                    g_switching = false;
                    g_switch_mute_until = GetTickCount64() + 500;
                }
            } unmute;
            const inv::ItemDef* held_def = nullptr;
            void* held = carried_in_slot(player, def->game_slot, &held_def);
            // that item is in the hands already (this box shows the held one, or the same item is held): just switch to it
            if (!stored || (held != nullptr && held_def == def))
            {
                do_select_weapon(def->classname);
                notify(std::string("Equipped ") + def->name);
                return;
            }
            // like the game's panel switching slots: what occupies the slot goes back into its own box, then ours comes out
            if (held != nullptr && !swap_out(player, held, *held_def))
            {
                notify(std::string("Your hands are full: no room for ") + def->name);
                return;
            }
            if (!take_from_slot(slot, *def))
                return;
            const auto give = reinterpret_cast<GiveFn>(mem::vslot(player, k_vt_give_named_item));
            g_giving = true; // the +use hook must not store what we are handing out
            const bool gave = give != nullptr && mem::guarded([&] { give(player, def->classname, 0, 0, 0); });
            g_giving = false;
            // GiveNamedItem's result is not reliable: look whether the player has it now
            if (!gave || !player_has_slot(player, def->game_slot))
            {
                {
                    Session& s = session();
                    std::lock_guard lock(s.mutex);
                    if (s.inventory.add_to_slot(slot, *def, 1) == 0)
                        s.inventory.add(*def, 1);
                }
                notify(gave ? std::string("Could not equip ") + def->name : std::string("The game refused the item"));
                return;
            }
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                s.inventory.set_held(slot, *def); // this box is the held item's home
            }
            do_select_weapon(def->classname); // the game does not switch to a given item by itself
            notify(std::string("Equipped ") + def->name);
        }

        // Where the player's eyes are and which way they look.
        bool player_view(void* player, Vec3& eye, Vec3& forward)
        {
            const auto eye_position = reinterpret_cast<EyePositionFn>(mem::vslot(player, k_vt_eye_position));
            const auto eye_angles = reinterpret_cast<EyeAnglesFn>(mem::vslot(player, k_vt_eye_angles));
            Vec3 angles{};
            if (eye_position == nullptr || eye_angles == nullptr ||
                !mem::guarded([&] {
                    Vec3 tmp{};
                    eye = *eye_position(player, &tmp);
                    angles = *eye_angles(player);
                }))
                return false;
            constexpr float k_pi = 3.14159265f;
            const float pitch = angles.x * k_pi / 180.f;
            const float yaw = angles.y * k_pi / 180.f;
            forward = {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
            return true;
        }

        // Takes the closest inventory item that lies in front of the player and is not carried by anyone.
        void do_pickup_nearby()
        {
            void* player = local_player();
            Vec3 eye{}, forward{};
            if (player == nullptr || !player_view(player, eye, forward))
            {
                notify("Not in a game");
                return;
            }

            // what the survivors hold is not on the ground
            std::vector<void*> carried;
            for (int index = 1; index <= 32; ++index)
            {
                void* other = entity_at_index(index);
                if (other == nullptr ||
                    mem::read_or<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(other)) != g.player_vtable)
                    continue;
                for (int i = 0; i < k_max_weapons; ++i)
                    if (void* weapon =
                            entity_from_handle(handle_at(other, k_off_my_weapons + 4u * static_cast<unsigned>(i))))
                        carried.push_back(weapon);
            }

            void* best = nullptr;
            const inv::ItemDef* best_def = nullptr;
            float best_score = 1e30f;
            for (int index = 1; index < 0x1000; ++index)
            {
                void* entity = entity_at_index(index);
                Vec3 origin{};
                if (entity == nullptr || !mem::read(reinterpret_cast<std::uintptr_t>(entity) + k_off_origin, origin))
                    continue;
                const Vec3 d{origin.x - eye.x, origin.y - eye.y, origin.z - eye.z};
                const float distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                if (distance > k_pickup_reach || distance < 1.f)
                    continue;
                const float facing = (d.x * forward.x + d.y * forward.y + d.z * forward.z) / distance;
                if (facing < 0.6f)
                    continue;
                const inv::ItemDef* def = item_of(entity);
                if (def == nullptr || std::find(carried.begin(), carried.end(), entity) != carried.end())
                    continue;
                const float score = distance * (2.f - facing);
                if (score < best_score)
                {
                    best_score = score;
                    best = entity;
                    best_def = def;
                }
            }
            if (best == nullptr)
            {
                notify("Nothing to pick up in front of you");
                return;
            }

            int stored = 0;
            int weapon_box = -1;
            int reserve = -1, reserve_type = -1; // a gun's own reserve: into the player's (shared by its ammo type)
            {
                inv::WeaponState state = best_def->weapon() ? weapon_state(player, best, *best_def, false) : inv::WeaponState{};
                reserve = state.ammo;
                reserve_type = ammo_type_of(best);
                state.ammo = -1;
                Session& s = session();
                std::lock_guard lock(s.mutex);
                if (best_def->weapon())
                {
                    weapon_box = s.inventory.add_weapon(*best_def, state); // its magazine stays with it
                    stored = weapon_box >= 0 ? 1 : 0;
                }
                else
                    stored = s.inventory.add(*best_def, 1);
            }
            if (stored == 0)
            {
                notify(refusal(*best_def));
                return;
            }
            if (!mem::guarded([&] { g.util_remove(best); }))
            {
                if (weapon_box >= 0)
                {
                    Session& s = session();
                    std::lock_guard lock(s.mutex);
                    s.inventory.remove_from_slot(weapon_box, 1);
                }
                else
                    take_from_inventory(*best_def, 1);
                notify("The game refused to take the item away");
                return;
            }
            if (weapon_box >= 0)
                add_to_pool(player, best_def->classname, reserve_type, reserve);
            notify(std::string("Picked up ") + best_def->name);
        }

        void do_drop_weapon(const std::string& classname);

        void do_drop(int slot, int count)
        {
            const inv::ItemDef* def = nullptr;
            int stored = 0;
            inv::WeaponState state;
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                const inv::Stack& stack = s.inventory.slot(slot);
                if (stack.empty())
                    return;
                def = stack.def;
                stored = stack.count;
                state = stack.weapon;
            }
            // the box only shows the item in the player's hands: that one is dropped the game's way
            if (stored <= 0)
            {
                do_drop_weapon(def->classname);
                return;
            }
            count = std::min(count, stored);
            void* player = local_player();
            if (player == nullptr)
            {
                notify("Not in a game");
                return;
            }
            Vec3 eye{}, forward{};
            if (!player_view(player, eye, forward))
            {
                notify("Cannot read where you are looking");
                return;
            }

            int dropped = 0;
            for (int i = 0; i < count; ++i)
            {
                // in front of the eyes, a little lower, and a little further for each one so they do not overlap
                const float distance = 48.f + 6.f * static_cast<float>(i);
                const Vec3 position{eye.x + forward.x * distance, eye.y + forward.y * distance,
                                    eye.z + forward.z * distance - 14.f};
                void* entity = nullptr;
                const bool ok = mem::guarded([&] {
                    entity = g.create_entity(g.tools, def->classname);
                    if (entity == nullptr)
                        return;
                    // which melee weapon: its keyvalue, read when it spawns
                    if (def->weapon() && !state.melee.empty())
                        g.set_key_value(g.tools, entity, "melee_script_name", state.melee.c_str());
                    if (const auto precache = reinterpret_cast<PrecacheFn>(mem::vslot(entity, k_vt_precache)))
                        precache(entity);
                    g.dispatch_spawn(g.tools, entity);
                    if (const auto teleport = reinterpret_cast<TeleportFn>(mem::vslot(entity, k_vt_teleport)))
                        teleport(entity, &position, nullptr, nullptr);
                    if (g.drop_to_floor != nullptr)
                        g.drop_to_floor(entity, 0x200400Bu, nullptr);
                    // a gun keeps its rounds: the magazine, and the reserve the way a dropped gun keeps it
                    // (m_iClip1 is every weapon's; m_iExtraPrimaryAmmo only a CTerrorGun's: not the melee weapon, the
                    // chainsaw or the pistols, whose reserve is endless)
                    if (def->weapon() && std::strcmp(def->classname, "weapon_melee") != 0)
                    {
                        const auto e = reinterpret_cast<std::uintptr_t>(entity);
                        if (state.clip >= 0)
                            *reinterpret_cast<int*>(e + k_off_clip) = state.clip;
                        if (state.ammo >= 0 && std::strcmp(def->classname, "weapon_chainsaw") != 0 &&
                            std::strstr(def->classname, "pistol") == nullptr)
                            *reinterpret_cast<int*>(e + k_off_extra_ammo) = state.ammo;
                    }
                    write_upgrades(entity, *def, state);
                });
                if (ok && entity != nullptr && def->weapon())
                    know_gun(entity);
                if (!ok || entity == nullptr)
                {
                    notify(std::string("Could not drop ") + def->name);
                    break;
                }
                INV_LOG("drop %s: entity %p at %.1f %.1f %.1f", def->classname, entity, position.x, position.y, position.z);
                ++dropped;
            }
            if (dropped > 0)
            {
                {
                    Session& s = session();
                    std::lock_guard lock(s.mutex);
                    const int taken = s.inventory.slot(slot).def == def ? s.inventory.remove_from_slot(slot, dropped) : 0;
                    if (taken < dropped)
                        s.inventory.remove_item(*def, dropped - taken);
                }
                notify(std::string("Dropped ") + def->name + (dropped > 1 ? " x" + std::to_string(dropped) : ""));
            }
        }

        // Development: puts a new catalogue item on the ground in front of the player, as a drop does, without
        // taking anything from the inventory (the server command inv_spawn <classname>).
        void do_spawn(const inv::ItemDef& def)
        {
            void* player = local_player();
            Vec3 eye{}, forward{};
            if (player == nullptr || !player_view(player, eye, forward))
                return;
            const Vec3 position{eye.x + forward.x * 56.f, eye.y + forward.y * 56.f, eye.z + forward.z * 56.f - 14.f};
            void* entity = nullptr;
            const bool ok = mem::guarded([&] {
                entity = g.create_entity(g.tools, def.classname);
                if (entity == nullptr)
                    return;
                if (const auto precache = reinterpret_cast<PrecacheFn>(mem::vslot(entity, k_vt_precache)))
                    precache(entity);
                g.dispatch_spawn(g.tools, entity);
                if (const auto teleport = reinterpret_cast<TeleportFn>(mem::vslot(entity, k_vt_teleport)))
                    teleport(entity, &position, nullptr, nullptr);
                if (g.drop_to_floor != nullptr)
                    g.drop_to_floor(entity, 0x200400Bu, nullptr);
            });
            if (!ok || entity == nullptr)
            {
                notify(std::string("Could not spawn ") + def.name);
                return;
            }
            INV_LOG("spawn %s: entity %p at %.1f %.1f %.1f", def.classname, entity, position.x, position.y, position.z);
            notify(std::string("Spawned ") + def.name);
        }

        // Takes the item in the player's hands into the inventory; when they hold a gun, the first utility item they
        // carry instead.
        void do_store_held()
        {
            void* player = local_player();
            if (player == nullptr)
            {
                notify("Not in a game");
                return;
            }
            INV_LOG("store: the player carries [%s]", carried_list(player).c_str());

            void* item = entity_from_handle(handle_at(player, k_off_active_weapon));
            const inv::ItemDef* def = item_of(item);
            for (int i = 0; def == nullptr && i < k_max_weapons; ++i)
            {
                void* weapon = entity_from_handle(handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i)));
                if (const inv::ItemDef* candidate = item_of(weapon); candidate != nullptr && !candidate->weapon())
                {
                    item = weapon;
                    def = candidate;
                }
            }
            if (def == nullptr)
            {
                notify("Nothing to store");
                return;
            }
            // a gun or melee weapon in the hands: into a box of its own, with its rounds
            if (def->weapon())
            {
                if (store_carried_weapon(player, item, *def) >= 0)
                    notify(std::string("Stored ") + def->name);
                return;
            }

            // back into its own box when it has one
            int home = -1;
            bool full = false;
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                home = s.inventory.held_slot(*def);
                if (home >= 0)
                    s.inventory.release_held(home);
                else
                    full = s.inventory.add(*def, 1) == 0;
            }
            if (full)
            {
                notify(refusal(*def)); // notify() takes the session lock itself
                return;
            }
            if (!mem::guarded([&] { g.util_remove(item); }))
            {
                {
                    Session& s = session();
                    std::lock_guard lock(s.mutex);
                    if (home >= 0 && s.inventory.remove_from_slot(home, 1) == 1)
                        s.inventory.set_held(home, *def); // the item is still in the player's hands
                    else
                        s.inventory.remove_item(*def, 1);
                }
                notify("The game refused to take the item away");
                return;
            }
            notify(std::string("Stored ") + def->name);
        }

        // ---- the pickup hook ----------------------------------------------------------------------------------------

        // CTerrorPlayer vtable slot 426 (server.dll + 0x31D080) is what a +use on a weapon entity calls (seen in the
        // game by hooking it together with slots 280 and 281):
        //   bool __thiscall (CTerrorPlayer* this, CBaseEntity* weapon), ret 4
        // It asks slot 280 (Weapon_CanUse, 0x10300600), on success equips with slot 281 and fires "weapon_given"; it
        // returns false when the weapon is refused, e.g. when the player's slot for it is already taken.  That is where
        // the inventory steps in.
        using BumpWeaponFn = bool(__fastcall*)(void* player, void* edx, void* weapon);
        core::Hook<BumpWeaponFn> g_bump_weapon;

        using ShouldDrawFn = bool(__fastcall*)(void* self, void* edx);
        core::Hook<ShouldDrawFn> g_weapon_select;

        bool __fastcall hk_weapon_select_should_draw(void* self, void* edx)
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                INV_LOG("CHudWeaponSelection::ShouldDraw called (this %p), replace_native %d", self,
                        core::config().replace_native ? 1 : 0);
            }
            if (core::config().replace_native)
                return false;
            return g_weapon_select.original()(self, edx);
        }

        // CHudWeaponSelection's Paint (panel vtable 0x105D5734 slot 128, client.dll + 0x2D11E0) draws its boxes one
        // after the other, unrolled: the gun (slot 0), the melee / pistol (slot 1), a part that ends with the key label
        // of box 5 and draws the throwable box too (seen in the game: skipping only the next three parts left it), then
        // slots 2..4.  Beside the game's panel the mod draws the item boxes itself, in its grid, so the hook below sits
        // on the first instruction after the melee part (after `push 1; mov ecx, ebx; call 0x102CE980`, its key) and,
        // while g_hide_item_boxes is set, jumps to the code after the slot-4 part (the same stack depth: both are
        // statement boundaries in the same function).
        volatile bool g_hide_item_boxes = false;
        std::uintptr_t g_item_boxes_end = 0;
        using ItemBoxesFn = void (*)();
        core::Hook<ItemBoxesFn> g_item_boxes;
        std::uintptr_t g_item_boxes_original = 0;

        __declspec(naked) void hk_item_boxes()
        {
            __asm {
                cmp byte ptr [g_hide_item_boxes], 0
                je draw
                jmp dword ptr [g_item_boxes_end]
            draw:
                jmp dword ptr [g_item_boxes_original]
            }
        }

        // With nothing in slot 1 (no melee or pistol: seen in the game, the item boxes came back) the melee part is
        // left by `je` straight to its last statement, after the hook above (0x102D17BF -> 0x102D1B59, `mov eax,
        // [ebp-0x64]; push eax; mov ecx, ebx; call 0x102CDDF0`): a second hook there, to the same end.
        core::Hook<ItemBoxesFn> g_item_boxes_empty;
        std::uintptr_t g_item_boxes_empty_original = 0;

        __declspec(naked) void hk_item_boxes_empty()
        {
            __asm {
                cmp byte ptr [g_hide_item_boxes], 0
                je draw
                jmp dword ptr [g_item_boxes_end]
            draw:
                jmp dword ptr [g_item_boxes_empty_original]
            }
        }

        // Takes `weapon` (a catalogue item lying in the world) into the inventory.
        bool store_world_item(void* weapon, const inv::ItemDef& def)
        {
            int stored = 0;
            int weapon_box = -1;
            int reserve = -1, reserve_type = -1; // a gun's own reserve: into the player's (shared by its ammo type)
            {
                // a gun keeps its magazine, a melee weapon which one it is
                inv::WeaponState state = def.weapon() ? weapon_state(nullptr, weapon, def, false) : inv::WeaponState{};
                reserve = state.ammo;
                reserve_type = ammo_type_of(weapon);
                state.ammo = -1;
                Session& s = session();
                std::lock_guard lock(s.mutex);
                if (def.weapon())
                {
                    weapon_box = s.inventory.add_weapon(def, state);
                    stored = weapon_box >= 0 ? 1 : 0;
                }
                else
                    stored = s.inventory.add(def, 1);
            }
            if (stored == 0)
            {
                notify(refusal(def));
                return false;
            }
            if (!mem::guarded([&] { g.util_remove(weapon); }))
            {
                if (weapon_box >= 0)
                {
                    Session& s = session();
                    std::lock_guard lock(s.mutex);
                    s.inventory.remove_from_slot(weapon_box, 1);
                }
                else
                    take_from_inventory(def, 1);
                notify("The game refused to take the item away");
                return false;
            }
            if (weapon_box >= 0)
                add_to_pool(local_player(), def.classname, reserve_type, reserve);
            notify(std::string("Stored ") + def.name);
            return true;
        }

        void refresh_native();
        int native_slot_of(const char* classname);
        NativeWeapon describe_weapon(const char* classname);

        // A gun or melee weapon the local player is about to pick up would take them over the weight limit (the one it
        // replaces in its game slot, if any, is dropped by the game, so only the difference counts).  Says why.
        bool weapon_too_heavy(void* weapon)
        {
            char name[64];
            if (!classname_of(weapon, name, sizeof(name)))
                return false;
            const float weight = inv::weapon_weight(name);
            if (weight <= 0.f)
                return false;
            mem::guarded([] { refresh_native(); }); // what they carry right now (a server player's may be stale)
            char text[128] = {};
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                const float max_weight = s.inventory.limits().max_weight;
                if (max_weight <= 0.f)
                    return false;
                const std::size_t slot = static_cast<std::size_t>(native_slot_of(name));
                const float replaced = slot < s.native.size() ? inv::weapon_weight(s.native[slot].classname) : 0.f;
                const float total = s.inventory.weight() - replaced + weight;
                INV_LOG("+use on %s: weight %.1f - %.1f (replaced) + %.1f = %.1f of %.1f", name, s.inventory.weight(),
                        replaced, weight, total, max_weight);
                if (total <= max_weight + 1e-3f)
                    return false;
                std::snprintf(text, sizeof(text), "Too heavy: %.1f / %.1f, %s weighs %.1f", s.inventory.weight(), max_weight,
                              describe_weapon(name).name.c_str(), weight);
            }
            INV_LOG("+use on %s: too heavy to carry, refused", name);
            notify(text);
            return true;
        }

        // A stored copy from a map spawner names the player as its owner (m_hOwner), so GiveNamedItem reports success
        // and the spawner counts it (see bump_weapon).
        void claim_for(void* player, void* weapon)
        {
            for (int index = 1; index <= 64; ++index) // the player's EHANDLE: players are entities 1..maxplayers
            {
                EntInfo info{};
                if (!mem::read(g.ent_list + 4 + static_cast<std::uintptr_t>(index) * sizeof(EntInfo), info) ||
                    info.entity != player)
                    continue;
                const std::uint32_t handle = static_cast<std::uint32_t>(index) | (info.serial << 12);
                mem::guarded([&] {
                    *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_weapon_owner) = handle;
                });
                return;
            }
        }

        // +use on a gun or melee weapon by the local (or served) player: into the hands when that game slot is free (the
        // game's own pick-up); when the hands have one there already, into a free weapon box with its rounds; with no
        // free box (or too heavy for one) the game's own swap, as long as the swap is not too heavy.
        enum class WeaponPickup
        {
            Game, // the game's own pick-up or swap
            Stored, // into a box: the game must not equip it
            Refused, // too heavy
        };

        WeaponPickup weapon_pickup(void* player, void* weapon, const inv::ItemDef* def)
        {
            if (def != nullptr && player_has_slot(player, def->game_slot))
            {
                bool box = false;
                {
                    Session& s = session();
                    std::lock_guard lock(s.mutex);
                    box = s.inventory.has_box_for(*def) && s.inventory.room_for(*def) >= 1;
                }
                INV_LOG("+use on %s: that game slot is taken, %s", def->classname, box ? "into a weapon box" : "no weapon box: the game's swap");
                if (box && store_world_item(weapon, *def))
                {
                    claim_for(player, weapon);
                    return WeaponPickup::Stored;
                }
            }
            return weapon_too_heavy(weapon) ? WeaponPickup::Refused : WeaponPickup::Game;
        }

        // ---- the map's guns, a part of a magazine ([map_guns]) ---------------------------------------------------------
        //
        // With [map_guns] on, a gun nobody has held yet (one lying in the map, or a spawner's fresh copy) has only
        // min_clip..max_clip % of its magazine (m_iClip1, full on such a gun) when a survivor first picks it up.  A gun
        // is known once anyone has held it (every survivor's m_hMyWeapons, ten times a second: see cap_reserves), the
        // mod dropped it from a box or gave it out, or it was cut down here; a known gun keeps its magazine.  By EHANDLE;
        // the clock going back (a new map) forgets them.

        std::set<std::uint32_t> g_known_guns; // main thread only
        float g_known_guns_time = 0.f;

        float game_time();

        void known_guns_clock()
        {
            const float now = game_time();
            if (now < g_known_guns_time)
                g_known_guns.clear(); // a new map
            g_known_guns_time = now;
        }

        void know_gun(void* weapon)
        {
            if (weapon == nullptr || !inv::map_gun_rules().enabled)
                return;
            known_guns_clock();
            if (const std::uint32_t handle = handle_of(weapon); handle != k_invalid_handle)
                g_known_guns.insert(handle);
        }

        // Every gun a survivor holds is known (cap_reserves).
        void know_held_guns(void* player)
        {
            if (!inv::map_gun_rules().enabled)
                return;
            known_guns_clock();
            for (int i = 0; i < k_max_weapons; ++i)
            {
                const std::uint32_t handle = handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i));
                if (handle != k_invalid_handle && entity_from_handle(handle) != nullptr)
                    g_known_guns.insert(handle);
            }
        }

        // A survivor is picking `weapon` up: a gun nobody has held gets a part of its magazine.
        void cut_map_gun(void* player, void* weapon)
        {
            const inv::MapGunRules& rules = inv::map_gun_rules();
            if (!rules.enabled || weapon == nullptr || player == nullptr || team_of(player) != 2)
                return;
            if (g_giving)
            {
                know_gun(weapon);
                return;
            }
            char name[64];
            const char* kind = classname_of(weapon, name, sizeof(name)) ? inv::ammo_kind(name) : nullptr;
            if (kind == nullptr || std::strcmp(kind, "chainsaw") == 0)
                return; // (not a gun with rounds: a melee weapon, a pistol, an item)
            known_guns_clock();
            const std::uint32_t handle = handle_of(weapon);
            if (handle == k_invalid_handle || !g_known_guns.insert(handle).second)
                return; // known
            const auto at = reinterpret_cast<std::uintptr_t>(weapon) + k_off_clip;
            const int full = mem::read_or<int>(at, -1);
            if (full <= 0)
                return;
            static std::minstd_rand random(static_cast<unsigned>(GetTickCount64() * 3266489917u));
            const int percent = rules.min_clip + static_cast<int>(random() % static_cast<unsigned>(rules.max_clip - rules.min_clip + 1));
            const int clip = std::clamp(static_cast<int>(std::lround(full * percent / 100.0)), 0, full);
            mem::guarded([&] { *reinterpret_cast<int*>(at) = clip; });
            state_changed(weapon);
            INV_LOG("map gun %s (%p): %d of %d in the magazine (%d%%)", name, weapon, clip, full, percent);
        }

        bool bump_weapon(void* player, void* edx, void* weapon)
        {
            cut_map_gun(player, weapon);
            const inv::ItemDef* def = nullptr;
            const bool local = !g_giving && player != nullptr && player == local_player();
            if (local)
                def = item_of(weapon);
            // a gun or melee weapon: see weapon_pickup
            if (def == nullptr || def->weapon())
            {
                const WeaponPickup pickup = local ? weapon_pickup(player, weapon, def) : WeaponPickup::Game;
                if (pickup != WeaponPickup::Game)
                    return pickup == WeaponPickup::Stored;
                return g_bump_weapon.original()(player, edx, weapon);
            }

            const bool always = core::config().auto_store;
            const bool full = player_has_slot(player, def->game_slot);
            INV_LOG("+use on %s (%p): slot %s, auto_store %d", def->classname, weapon, full ? "full" : "free",
                    always ? 1 : 0);
            // Handled: the game must not equip it as well, but the answer is "picked up".  A map's item spawner
            // (weapon_*_spawn, CWeaponSpawn::Use 0x1029DCB0) gives a fresh copy through GiveItem (0x1029E410) ->
            // GiveNamedItem(name, 0, 1, 0), which calls this; with that third argument GiveNamedItem removes the copy and
            // reports failure unless the copy's owner (m_hOwner) is the player (0x10311773), and the spawner takes one
            // off its count (and disappears when empty) only on success.  Answering false, or true without the owner,
            // left the spawner full, so its item could be stored again and again.  So the stored copy (already marked
            // for deletion) names the player as its owner.  A weapon lying in the world (CBaseCombatWeapon::Use
            // 0x1024F4A0) ignores both.
            const bool tried = always || full;
            if (tried && store_world_item(weapon, *def))
            {
                claim_for(player, weapon);
                return true;
            }
            // not stored: the game equips it into the free slot, and refreshing the game slots gives it a box, so it
            // counts against the weight limit like a stored one.  Too heavy: refused here, before the game equips it.
            bool too_heavy = false;
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                too_heavy = !full && s.inventory.room_for(*def) == 0;
            }
            if (too_heavy)
            {
                INV_LOG("+use on %s: too heavy to carry, refused", def->classname);
                if (!tried) // (a failed store has said why already)
                    notify(refusal(*def));
                return false;
            }
            return g_bump_weapon.original()(player, edx, weapon);
        }

        // ---- rounds: the reserve as a thing of its own -----------------------------------------------------------------
        //
        // The player's reserve (m_iAmmo, per ammo type, shared by the guns of a type) weighs (inv::round_weight,
        // [round_weight] in the rules: refresh_native counts it in) and can be dropped, a magazine's worth at a time or
        // all of a type.  The rounds land as one of the game's own ammo piles, weapon_ammo_spawn (CWeaponAmmoSpawn, see
        // below), as an incendiary ammo pack tinted the colour of its type, with a targetname that says what it holds:
        // "inv_rounds_<kind>_<rounds>".
        // +use on it goes through the pile's own Use, hooked (hk_ammo_spawn_use): instead of the endless refill it gives
        // those rounds, as many as fit (the type's most, the weight limit); what does not fit stays in the pile.  The
        // bots do not take them.  Ammo piles and guns picked up fill a reserve as the game does; cap_reserve takes a rise
        // past the weight limit back down to what fits.

        constexpr char k_pile_prefix[] = "inv_rounds_";
        constexpr std::uintptr_t k_off_entity_name = 0x140; // CBaseEntity::m_iName (targetname)
        // weapon_ammo_spawn is CWeaponAmmoSpawn (vtable 0x10682CF4, set by its factory 0x102A22F0 over CWeaponSpawn's):
        // its Precache (slot 26, 0x1029F5A0) precaches models/props/terror/ammo_stack.mdl, its model (slot 239,
        // 0x1029F560) is the "model" keyvalue or that one, its Use (slot 106, 0x1029F5F0) is its own (the refill).
        // The pile is its round type's own ammo pack, models/inv_mod/rounds_<kind>.mdl, when the game has it: the addon
        // inventory_mod_rounds.vpk in left4dead2/addons (tools/make_rounds_addon.py: the incendiary pack with the
        // texture of assets/textures for that type).  Without it (or for a type with no pack: the M60, the chainsaw) it
        // is the closed incendiary ammo pack (the upgrade pack's world model, loaded with every map), tinted the colour of
        // its round type (inv::ammo_color, the entity's "rendercolor" as "r g b 255": three numbers leave the alpha 0 and
        // it is invisible); ammo_stack.mdl when that cannot be precached.  Every player needs the addon to see the packs:
        // a model the server has and a player does not is the ERROR model for that player.
        constexpr const char* k_pile_model = "models/w_models/weapons/w_eq_incendiary_ammopack.mdl";
        // filesystem_stdio.dll "VBaseFileSystem012": the IBaseFileSystem of its one CFileSystem_Stdio (0x1005260C, vtable
        // 0x10047DFC, set at 0x10045509); slot 10 FileExists(name, pathID) 0x1000E340 (thiscall, ret 8, the profiler
        // scope "CBaseFileSystem::FileExists").  The same file in the game and the dedicated server.
        constexpr int k_vt_file_exists = 10;
        using FileExistsFn = bool(__thiscall*)(void* fs, const char* name, const char* path_id);

        // The game can load `name` (its own files, the addons): main thread.
        bool game_file_exists(const char* name)
        {
            static void* fs = nullptr;
            static FileExistsFn file_exists = [] {
                HMODULE module = GetModuleHandleW(L"filesystem_stdio.dll");
                const auto create =
                    module != nullptr ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface")) : nullptr;
                fs = create != nullptr ? create("VBaseFileSystem012", nullptr) : nullptr;
                const std::uintptr_t at = fs != nullptr ? mem::vslot(fs, k_vt_file_exists) : 0;
                if (at != 0 && mem::find_pattern(at, 9, "55 8B EC 81 EC 7C 01 00 00") == at)
                    return reinterpret_cast<FileExistsFn>(at);
                INV_LOG("game: the file system's FileExists does not look as expected: piles are the incendiary pack");
                return static_cast<FileExistsFn>(nullptr);
            }();
            bool found = false;
            if (file_exists != nullptr)
                mem::guarded([&] { found = file_exists(fs, name, "GAME"); });
            return found;
        }
        constexpr int k_vt_is_model_precached = 9; // VEngineServer022 IsModelPrecached(name)
        using IsModelPrecachedFn = bool(__thiscall*)(void* engine, const char* name);
        constexpr const char* k_pile_model_fallback = "models/props/terror/ammo_stack.mdl";
        // server.dll CBaseEntity::PrecacheModel (0x1006B690, cdecl int (const char*): engine slot 5 PrecacheModel(name,
        // true), with a warning when it is late)
        constexpr std::uintptr_t k_rva_precache_model = 0x6B690;
        using PrecacheModelFn = int(__cdecl*)(const char* name);

        PrecacheModelFn precache_model_fn()
        {
            static PrecacheModelFn fn = [] {
                const std::uintptr_t at = g.server.base + k_rva_precache_model;
                return mem::find_pattern(at, 11, "55 8B EC 56 8B 75 08 85 F6 74 ??") == at ? reinterpret_cast<PrecacheModelFn>(at)
                                                                                         : nullptr;
            }();
            return fn;
        }

        int team_of(void* entity); // see "the infection"
        bool is_bot(void* player);

        // What a pile the mod dropped holds; false for any other entity.
        bool pile_contents(void* entity, std::string& kind, int& rounds)
        {
            // CBaseEntity::m_iName +0x140 (datamap "m_iName": a pooled string; IServerTools GetKeyValue "targetname"
            // gives the pointer's bytes, not the text, as for the model)
            char name[96] = {};
            const char* text = mem::read_or<const char*>(reinterpret_cast<std::uintptr_t>(entity) + k_off_entity_name);
            if (text == nullptr || !mem::safe_copy(name, text, sizeof(name) - 1))
                return false;
            if (std::strncmp(name, k_pile_prefix, sizeof(k_pile_prefix) - 1) != 0)
                return false;
            const char* rest = name + sizeof(k_pile_prefix) - 1;
            const char* separator = std::strrchr(rest, '_');
            if (separator == nullptr)
                return false;
            kind.assign(rest, static_cast<std::size_t>(separator - rest));
            rounds = std::atoi(separator + 1);
            return inv::ammo_index(kind) >= 0 && rounds > 0;
        }

        void set_pile_contents(void* entity, const std::string& kind, int rounds)
        {
            const std::string name = k_pile_prefix + kind + "_" + std::to_string(rounds);
            mem::guarded([&] { g.set_key_value(g.tools, entity, "targetname", name.c_str()); });
        }

        int* reserve_of(void* player, int type)
        {
            return reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + k_off_player_ammo + 4u * static_cast<unsigned>(type));
        }

        // How many of `rounds` of `kind` the player can take: up to the type's most and, for the player whose inventory
        // session() is, the weight limit.
        int rounds_that_fit(void* player, const std::string& kind, int rounds, bool weigh)
        {
            const int type = inv::ammo_index(kind);
            const AmmoPool pool = read_pool(player);
            if (type < 0 || !pool.read)
                return 0;
            const int max = inv::ammo_max(kind);
            int n = max >= 0 ? std::min(rounds, std::max(0, max - pool.count[type])) : rounds;
            const float each = inv::round_weight(kind);
            if (weigh && each > 0.f)
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                const float max_weight = s.inventory.limits().max_weight;
                const float load = s.inventory.weight() - s.inventory.ammo_weight() + inv::ammo_weight(pool.count);
                if (max_weight > 0.f)
                    n = std::min(n, max_weight <= load ? 0 : static_cast<int>(std::floor((max_weight - load) / each + 1e-4f)));
            }
            return std::max(0, n);
        }

        // The piles dropped this map (by EHANDLE: a removed one, or one of another map, is no entity any more), with where
        // they were put: rounds dropped next to a pile of their type join it, and a new pile is put beside the others,
        // not into them.
        struct DroppedPile
        {
            std::uint32_t handle;
            Vec3 at;
        };
        std::vector<DroppedPile> g_dropped_piles; // main thread only

        // Drops `count` (0: all) of the player's reserve of `kind` in front of them, as an ammo pile.
        void do_drop_ammo(const std::string& kind, int count)
        {
            void* player = local_player();
            const int type = inv::ammo_index(kind);
            const char* title = inv::ammo_title(kind);
            if (player == nullptr || type < 0 || title == nullptr)
            {
                notify("Not in a game");
                return;
            }
            Vec3 eye{}, forward{};
            if (!player_view(player, eye, forward))
            {
                notify("Cannot read where you are looking");
                return;
            }
            const int have = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(reserve_of(player, type)), 0);
            const int n = count <= 0 ? have : std::min(count, have);
            if (n <= 0)
            {
                notify(std::string("No ") + title + " to drop");
                return;
            }
            Vec3 position{eye.x + forward.x * 48.f, eye.y + forward.y * 48.f, eye.z + forward.z * 48.f - 14.f};

            // a pile of this type close by takes them; the piles that are gone are forgotten
            g_dropped_piles.erase(std::remove_if(g_dropped_piles.begin(), g_dropped_piles.end(),
                                                 [](const DroppedPile& d) { return entity_from_handle(d.handle) == nullptr; }),
                                  g_dropped_piles.end());
            const auto close_to = [](const Vec3& a, const Vec3& b, float radius) {
                const float dx = a.x - b.x, dy = a.y - b.y;
                return dx * dx + dy * dy < radius * radius && std::fabs(a.z - b.z) < 72.f;
            };
            for (const DroppedPile& d : g_dropped_piles)
            {
                std::string held;
                int rounds = 0;
                void* other = entity_from_handle(d.handle);
                if (!close_to(d.at, position, 40.f) || !pile_contents(other, held, rounds) || held != kind)
                    continue;
                set_pile_contents(other, kind, rounds + n);
                mem::guarded([&] { *reserve_of(player, type) = have - n; });
                state_changed(player);
                INV_LOG("rounds: dropped %d %s (%d left) onto pile %p, which holds %d now", n, kind.c_str(), have - n, other, rounds + n);
                notify("Dropped " + std::to_string(n) + " " + title + " (onto the pile there)");
                return;
            }
            // not into another pile (piles inside each other crashed the game's physics, see below): beside it
            for (int step = 1; step <= 8; ++step)
            {
                bool clear = true;
                for (const DroppedPile& d : g_dropped_piles)
                    clear = clear && !close_to(d.at, position, 22.f);
                if (clear)
                    break;
                const float side = (step % 2 == 1 ? 1.f : -1.f) * 24.f * static_cast<float>((step + 1) / 2);
                position = Vec3{eye.x + forward.x * 48.f - forward.y * side, eye.y + forward.y * 48.f + forward.x * side,
                                eye.z + forward.z * 48.f - 14.f};
            }
            const PrecacheModelFn precache_model = precache_model_fn();
            if (precache_model == nullptr)
            {
                notify("Cannot drop rounds: the game's PrecacheModel does not look as expected");
                return;
            }
            const std::string own_model = "models/inv_mod/rounds_" + kind + ".mdl";
            const bool own = game_file_exists(own_model.c_str()); // its type's pack (the addon)
            const char* model = own ? own_model.c_str() : k_pile_model;
            bool loaded = false; // by the map already (else it is precached now, in the middle of the round)
            if (const auto is_precached = g_engine_server != nullptr
                                              ? reinterpret_cast<IsModelPrecachedFn>(mem::vslot(g_engine_server, k_vt_is_model_precached))
                                              : nullptr)
                mem::guarded([&] { loaded = is_precached(g_engine_server, model); });
            int model_index = -1;
            mem::guarded([&] { model_index = precache_model(model); });
            if (model_index < 0 && own)
            {
                model = k_pile_model;
                mem::guarded([&] { model_index = precache_model(model); });
            }
            if (model_index < 0)
            {
                model = k_pile_model_fallback;
                mem::guarded([&] { model_index = precache_model(model); });
            }
            if (model_index < 0)
            {
                notify(std::string("Could not drop ") + title + ": no ammo pile model");
                return;
            }
            const unsigned color = own ? 0xFFFFFFu : inv::ammo_color(kind); // (its own pack has its colour)
            char tint[16];
            std::snprintf(tint, sizeof(tint), "%u %u %u 255", (color >> 16) & 255u, (color >> 8) & 255u, color & 255u); // (a missing alpha is 0: invisible)
            char angles[32];
            std::snprintf(angles, sizeof(angles), "0 %.0f 0", std::atan2(forward.y, forward.x) * 57.29578f);
            void* pile = nullptr;
            mem::guarded([&] {
                pile = g.create_entity(g.tools, "weapon_ammo_spawn");
                if (pile == nullptr)
                    return;
                g.set_key_value(g.tools, pile, "model", model);
                g.set_key_value(g.tools, pile, "rendercolor", tint);
                // what a map's own pile has (c2m1_highway: "solid" "6", "spawnflags" "2", "disableshadows" "1"): without
                // "solid" the game crashed in vphysics.dll (+0xF3B1C, a null edge in IVP's collision code) a while later,
                // as with a spawner made by ent_create
                g.set_key_value(g.tools, pile, "solid", "6");
                g.set_key_value(g.tools, pile, "spawnflags", "2");
                g.set_key_value(g.tools, pile, "disableshadows", "1");
                g.set_key_value(g.tools, pile, "angles", angles);
                set_pile_contents(pile, kind, n);
                if (const auto precache = reinterpret_cast<PrecacheFn>(mem::vslot(pile, k_vt_precache)))
                    precache(pile);
                g.dispatch_spawn(g.tools, pile);
                if (const auto teleport = reinterpret_cast<TeleportFn>(mem::vslot(pile, k_vt_teleport)))
                    teleport(pile, &position, nullptr, nullptr);
                if (g.drop_to_floor != nullptr)
                    g.drop_to_floor(pile, 0x200400Bu, nullptr);
            });
            if (pile == nullptr)
            {
                notify(std::string("Could not drop ") + title);
                return;
            }
            if (const std::uint32_t handle = handle_of(pile); handle != k_invalid_handle)
                g_dropped_piles.push_back({handle, position});
            mem::guarded([&] { *reserve_of(player, type) = have - n; });
            state_changed(player);
            INV_LOG("rounds: dropped %d %s (%d left) as pile %p (%s, %s, tint %s) at %.1f %.1f %.1f", n, kind.c_str(), have - n,
                    pile, model, loaded ? "loaded with the map" : "precached now", tint, position.x, position.y, position.z);
            notify("Dropped " + std::to_string(n) + " " + title);
        }

        // +use on a pile the mod dropped: its rounds, as many as fit.
        void take_pile(void* pile, void* player, const std::string& kind, int rounds)
        {
            if (player == nullptr || team_of(player) != 2 || is_bot(player))
                return;
            const int type = inv::ammo_index(kind);
            const char* title = inv::ammo_title(kind);
            const bool own = player == local_player(); // the host, or the player being served: their weight counts
            const int n = rounds_that_fit(player, kind, rounds, own);
            if (n <= 0)
            {
                const int have = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(reserve_of(player, type)), 0);
                if (own)
                    notify(have >= inv::ammo_max(kind) ? std::string("You carry all the ") + title + " you can"
                                                       : std::string("Too heavy for more ") + title);
                return;
            }
            mem::guarded([&] { *reserve_of(player, type) += n; });
            state_changed(player);
            if (n >= rounds)
                mem::guarded([&] { g.util_remove(pile); });
            else
                set_pile_contents(pile, kind, rounds - n);
            INV_LOG("rounds: took %d of the %d %s in pile %p", n, rounds, kind.c_str(), pile);
            if (own)
                notify("Picked up " + std::to_string(n) + " " + title + (n < rounds ? " (the rest is too much)" : ""));
        }

        // A map's item spawner refuses by itself when the player already carries the same item: CWeaponSpawn::Use
        // (0x1029DCB0) asks Weapon_GetSlot(player, weaponId) (0x102C98F0) and, finding that item, takes a branch that
        // gives nothing, so the +use pickup hook never runs.  While the spawner serves the local player an item the
        // inventory takes (slot full, or auto_store), that one question answers "nothing there": the spawner then gives
        // through GiveNamedItem as for an empty slot, the +use hook stores the copy, and the spawner counts it.
        using SpawnUseFn = void(__fastcall*)(void* spawner, void* edx, void* activator, void* caller, int type, float value);
        using GetSlotFn = void*(__fastcall*)(void* player, void* edx, int weapon_id);
        using WeaponIdFn = int(__thiscall*)(void* spawner);
        using WeaponIdNameFn = const char*(__cdecl*)(int weapon_id);
        core::Hook<SpawnUseFn> g_spawn_use;
        core::Hook<GetSlotFn> g_get_slot;
        WeaponIdNameFn g_weapon_id_name = nullptr;
        int g_hidden_weapon_id = 0; // main thread only: Weapon_GetSlot answers null for it (0: none)

        void* __fastcall hk_get_slot(void* player, void* edx, int weapon_id)
        {
            if (g_hidden_weapon_id != 0 && weapon_id == g_hidden_weapon_id && player == local_player())
                return nullptr;
            return g_get_slot.original()(player, edx, weapon_id);
        }

        // The weapons a player holds (their EHANDLEs), sorted.
        std::vector<std::uint32_t> held_handles(void* player)
        {
            std::vector<std::uint32_t> out;
            for (int i = 0; i < k_max_weapons; ++i)
            {
                const std::uint32_t handle = handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i));
                if (handle != k_invalid_handle && entity_from_handle(handle) != nullptr)
                    out.push_back(handle);
            }
            std::sort(out.begin(), out.end());
            return out;
        }

        void spawn_use(void* spawner, void* edx, void* activator, void* caller, int type, float value)
        {
            int weapon_id = 0;
            const inv::ItemDef* def = nullptr;
            char item[64] = {};
            const bool survivor = activator != nullptr && team_of(activator) == 2;
            if (survivor && g_weapon_id_name != nullptr)
            {
                const auto id_of = reinterpret_cast<WeaponIdFn>(mem::vslot(spawner, k_vt_spawn_weapon_id));
                mem::guarded([&] {
                    weapon_id = id_of != nullptr ? id_of(spawner) : 0;
                    if (const char* name = weapon_id > 0 ? g_weapon_id_name(weapon_id) : nullptr)
                    {
                        mem::safe_copy(item, name, sizeof(item) - 1);
                        def = inv::find_item(item);
                    }
                });
            }
            // [map_guns] spawners_once: a weapon spawner (a gun, a melee weapon: not an item) is gone once it has given one
            const bool once = survivor && item[0] != '\0' && inv::map_gun_rules().enabled && inv::map_gun_rules().spawners_once &&
                              (def == nullptr || def->weapon());
            const bool local = activator != nullptr && activator == local_player();
            std::vector<std::uint32_t> before;
            int stored_before = 0;
            if (once)
            {
                before = held_handles(activator);
                if (local && def != nullptr)
                {
                    Session& s = session();
                    std::lock_guard lock(s.mutex);
                    stored_before = s.inventory.count_of(*def);
                }
            }
            if (!local)
                def = nullptr; // (what follows is the inventory's: the local or served player's)
            bool take = false;
            if (def != nullptr && !def->weapon() && player_has_slot(activator, def->game_slot))
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                // what the +use hook will do: store it (slot full); only when it has room, else the game refuses as usual
                inv::Inventory probe = s.inventory;
                take = probe.add(*def, 1) == 1;
            }
            if (take)
            {
                INV_LOG("spawner %p: %s with the slot full, the game is told the slot is free", spawner, def->classname);
                g_hidden_weapon_id = weapon_id;
            }
            g_spawn_use.original()(spawner, edx, activator, caller, type, value);
            g_hidden_weapon_id = 0;
            if (once)
            {
                // given: a weapon in the hands that was not there, or a copy stored in a box
                const std::vector<std::uint32_t> after = held_handles(activator);
                bool given = false;
                for (const std::uint32_t handle : after)
                    given |= !std::binary_search(before.begin(), before.end(), handle);
                if (!given && local)
                    if (const inv::ItemDef* stored = inv::find_item(item))
                    {
                        Session& s = session();
                        std::lock_guard lock(s.mutex);
                        given = s.inventory.count_of(*stored) > stored_before;
                    }
                if (given)
                {
                    INV_LOG("spawner %p (%s): gave one, it is gone (spawners_once)", spawner, item);
                    mem::guarded([&] { g.util_remove(spawner); });
                }
            }
        }

        // ---- other players: a dedicated server, guests on a listen server -----------------------------------------
        //
        // In srcds.exe every human player, and on a listen server every guest (everyone but the host, who uses the
        // local inventory), has an inventory of their own, kept by SteamID and saved next to the DLL
        // (inventory_<steamid>.inventory).  Only players whose game runs the mod are served (has_mod: their HUD has
        // sent an inv_* command); the others keep the game's own behaviour, since they could not see stored items.  The game code above works on "the local player" and session(): while it
        // serves a player, Activation makes that player the local one and their Session the active one.  When the
        // outermost Activation ends, the player gets their inventory as one console line (ClientPrintf) that their
        // client DLL catches before it is printed (k_state_marker).  Their HUD sends its requests as console commands
        // (inv_use 3 ...), which the client forwards to the server because it does not know them.
        //
        //   engine.dll VEngineServer022 slot 17 GetPlayerNetworkIDString(edict), the call CBasePlayer::
        //       GetNetworkIDString (server.dll 0x1016D9F0) makes with the player's edict (+0x28); slot 46
        //       ClientPrintf(edict, text) (it calls the IClient's ClientPrintf("%s", text)).
        //   server.dll 0x10090C80 ClientCommand(CBasePlayer*, const CCommand&), cdecl: what CServerGameClients::
        //       ClientCommand (vtable 0x1060CAD4 slot 5) calls for every command a client sends; CCommand: argc at +0,
        //       argv[] at +0x408.

        constexpr char k_state_marker[] = "@inv@";
        constexpr std::uintptr_t k_off_edict = 0x28;
        constexpr std::uintptr_t k_rva_client_command = 0x90C80;
        constexpr std::uintptr_t k_off_args_argv = 0x408;

        using NetworkIdFn = const char*(__thiscall*)(void* engine, void* edict);
        using ClientPrintfFn = void(__thiscall*)(void* engine, void* edict, const char* text);
        using ClientCommandFn = void(__cdecl*)(void* player, const void* args);

        NetworkIdFn g_network_id = nullptr;
        ClientPrintfFn g_client_printf = nullptr;
        core::Hook<ClientCommandFn> g_client_command;
        using ServerCommandFn = void(__thiscall*)(void* engine, const char* command);
        ServerCommandFn g_server_command = nullptr;
        bool g_ammo_limits_set = false;

        // The rules file's [ammo] maxima into the game's own settings (ammo_<type>_max, FCVAR_REPLICATED, not a cheat:
        // server.dll registers them with flags 0x2000), so ammo piles stop there too.  Once, on the main thread, by
        // whoever runs the server: the dedicated server, or the host (a client of someone else's server must not).
        // The ConVar `name` of server.dll, found by its registration (a static initializer: `push 0x2000; push
        // "<default>"; push "<name>"; mov ecx, <ConVar>; call ConVar::ConVar`).  nullptr when not found.
        std::uintptr_t find_server_convar(const char* name)
        {
            std::string text_pattern;
            char hex[4];
            for (const char* c = name;; ++c)
            {
                std::snprintf(hex, sizeof(hex), "%02X ", static_cast<unsigned char>(*c));
                text_pattern += hex;
                if (*c == '\0')
                    break;
            }
            const std::uintptr_t text = mem::find_pattern(g.server.base, g.server.size, text_pattern.c_str());
            if (text == 0)
                return 0;
            char code[64];
            std::snprintf(code, sizeof(code), "68 %02X %02X %02X %02X B9", static_cast<unsigned>(text & 0xFF),
                          static_cast<unsigned>((text >> 8) & 0xFF), static_cast<unsigned>((text >> 16) & 0xFF),
                          static_cast<unsigned>((text >> 24) & 0xFF));
            const std::uintptr_t push = mem::find_pattern(g.server.base, g.server.size, code);
            return push != 0 ? mem::read_or<std::uintptr_t>(push + 6) : 0;
        }

        // L4D2 marks the ammo maxima FCVAR_DEVELOPMENTONLY (flags 0x2006 seen in the game: development only, game DLL,
        // replicated), and the console refuses those as an "Unknown command".  ConCommandBase::m_nFlags is at +0x14 and
        // ConVar::m_pParent at +0x1C (the value lives in the parent: on a listen server the client's copy); the flag is
        // taken off both so the command below can set it.
        void unlock_convar(std::uintptr_t convar)
        {
            mem::guarded([&] {
                *reinterpret_cast<int*>(convar + 0x14) &= ~0x2;
                if (const auto parent = mem::read_or<std::uintptr_t>(convar + 0x1C); parent != 0 && parent != convar)
                    *reinterpret_cast<int*>(parent + 0x14) &= ~0x2;
            });
        }

        void set_ammo_limits()
        {
            if (g_ammo_limits_set || g_server_command == nullptr)
                return;
            g_ammo_limits_set = true;
            for (const char* kind : inv::ammo_kinds())
            {
                if (!inv::ammo_max_set(kind))
                    continue;
                char name[48];
                std::snprintf(name, sizeof(name), "ammo_%s_max", kind);
                const std::uintptr_t convar = find_server_convar(name);
                if (convar == 0)
                {
                    INV_LOG("ammo: %s not found in server.dll: not set", name);
                    continue;
                }
                unlock_convar(convar);
                char command[64];
                std::snprintf(command, sizeof(command), "ammo_%s_max %d\n", kind, inv::ammo_max(kind));
                mem::guarded([&] { g_server_command(g_engine_server, command); });
                INV_LOG("ammo: %s", command);
            }
        }

        void infect(void* player, const char* how); // see "the infection"

        struct ServerPlayer
        {
            std::string id;
            std::wstring file;
            Session session{.inventory = inv::Inventory(config_limits())};
            std::uint32_t saved_revision = 0;
            bool has_mod = false; // their HUD has talked to us
        };
        std::map<std::string, std::unique_ptr<ServerPlayer>> g_players;
        ServerPlayer* g_active_server_player = nullptr;

        void* edict_of(void* player)
        {
            return mem::read_or<void*>(reinterpret_cast<std::uintptr_t>(player) + k_off_edict);
        }

        // The player's inventory (loaded on first use); nullptr for a bot or anything that is not a connected player.
        ServerPlayer* server_player(void* player)
        {
            if (player == nullptr || g_network_id == nullptr ||
                mem::read_or<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(player)) != g.player_vtable)
                return nullptr;
            void* edict = edict_of(player);
            const char* raw = nullptr;
            if (edict == nullptr || !mem::guarded([&] { raw = g_network_id(g_engine_server, edict); }) || raw == nullptr)
                return nullptr;
            char id[64] = {};
            if (!mem::safe_copy(id, raw, sizeof(id) - 1))
                return nullptr;
            if (id[0] == '\0' || std::strcmp(id, "BOT") == 0)
                return nullptr;

            std::unique_ptr<ServerPlayer>& entry = g_players[id];
            if (!entry)
            {
                entry = std::make_unique<ServerPlayer>();
                entry->id = id;
                std::wstring safe;
                for (const char* c = id; *c != '\0'; ++c)
                    safe += std::isalnum(static_cast<unsigned char>(*c)) ? static_cast<wchar_t>(*c) : L'_';
                entry->file = std::wstring(core::module_dir()) + L"inventory_" + safe + L".inventory";
                if (FILE* f = _wfopen(entry->file.c_str(), L"rb"))
                {
                    std::string text;
                    char buffer[1024];
                    for (std::size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;)
                        text.append(buffer, n);
                    std::fclose(f);
                    entry->session.inventory.from_text(text);
                }
                entry->saved_revision = entry->session.inventory.revision();
                INV_LOG("server: inventory of %s (%d slots used)", id, entry->session.inventory.used_slots());
            }
            return entry.get();
        }

        void refresh_native();

        // The player's inventory, as the line their client DLL reads:
        //   @inv@ <slots> <max_weight> <weight> <weapon slots>|<index> <classname> <count> <held>,...|<game slot 0>,...,<game slot 5>|<message>
        // (<weight>: what the stored items weigh by the server's rules; a client that reads only two numbers still works)
        // A game slot is "-" when empty, a leading '*' marks the weapon in the hands, a melee weapon has ":<kind>".  A stored weapon's entry goes on
        // with "<clip> <ammo> <melee script name or ->".
        void send_state(void* player, ServerPlayer& sp)
        {
            mem::guarded([] { refresh_native(); });
            std::string line;
            std::string to_save;
            {
                Session& s = sp.session;
                std::lock_guard lock(s.mutex);
                const inv::Inventory& inventory = s.inventory;
                char head[64];
                std::snprintf(head, sizeof(head), "%s %d %.2f %.2f %d|", k_state_marker, inventory.slot_count(),
                              inventory.limits().max_weight, inventory.weight(), inventory.weapon_slot_count());
                line = head;
                for (int i = 0; i < inventory.total_slots(); ++i)
                {
                    const inv::Stack& stack = inventory.slot(i);
                    if (stack.empty())
                        continue;
                    line += std::to_string(i) + ' ' + stack.def->classname + ' ' + std::to_string(stack.count) + ' ' +
                            (stack.held ? '1' : '0');
                    if (stack.def->weapon()) // a weapon's rounds and kind, as in the save file
                        line += ' ' + std::to_string(stack.weapon.clip) + ' ' + std::to_string(stack.weapon.ammo) + ' ' +
                                (stack.weapon.melee.empty() ? std::string("-") : stack.weapon.melee) + ' ' +
                                std::to_string(stack.weapon.upgrades) + ' ' + std::to_string(stack.weapon.special);
                    line += ',';
                }
                line += '|';
                for (std::size_t i = 0; i < s.native.size(); ++i)
                {
                    if (i != 0)
                        line += ',';
                    if (s.native[i].classname.empty())
                        line += '-';
                    else
                        line += (s.native[i].active ? "*" : "") + s.native[i].classname +
                                (s.native[i].melee.empty() ? std::string() : ":" + s.native[i].melee);
                }
                line += '|';
                line += s.message; // sent once
                s.message.clear();
                if (inventory.revision() != sp.saved_revision)
                {
                    sp.saved_revision = inventory.revision();
                    to_save = inventory.to_text();
                }
            }
            line += '\n';
            if (!to_save.empty())
                if (FILE* f = _wfopen(sp.file.c_str(), L"wb"))
                {
                    std::fwrite(to_save.data(), 1, to_save.size(), f);
                    std::fclose(f);
                }
            void* edict = edict_of(player);
            if (g_client_printf != nullptr && edict != nullptr)
                mem::guarded([&] { g_client_printf(g_engine_server, edict, line.c_str()); });
        }

        // Makes `player` the local player and their inventory the active session for its lifetime; ok() is false for a
        // bot, and with `require_mod` for a player whose game does not run the mod (nothing changes then).  Nests: the
        // outermost one sends the player their inventory when it ends.
        class Activation
        {
        public:
            explicit Activation(void* player, bool require_mod = true) : player_(player), sp_(server_player(player))
            {
                if (sp_ != nullptr && require_mod && !sp_->has_mod)
                    sp_ = nullptr;
                if (sp_ == nullptr)
                    return;
                prev_player_ = g_active_player;
                prev_sp_ = g_active_server_player;
                g_active_player = player;
                g_active_server_player = sp_;
                set_active_session(&sp_->session);
                ++depth_;
            }
            ~Activation()
            {
                if (sp_ == nullptr)
                    return;
                if (depth_ == 1)
                    send_state(player_, *sp_);
                --depth_;
                g_active_player = prev_player_;
                g_active_server_player = prev_sp_;
                set_active_session(prev_sp_ != nullptr ? &prev_sp_->session : nullptr);
            }
            Activation(const Activation&) = delete;
            Activation& operator=(const Activation&) = delete;
            bool ok() const
            {
                return sp_ != nullptr;
            }

        private:
            void* player_;
            ServerPlayer* sp_;
            void* prev_player_ = nullptr;
            ServerPlayer* prev_sp_ = nullptr;
            static inline int depth_ = 0;
        };

        // Served through an Activation (not the host, and the server side is up).
        bool served_remotely(void* player)
        {
            return player != nullptr && g_network_id != nullptr && g_active_player == nullptr && player != host_player();
        }

        bool __fastcall hk_bump_weapon(void* player, void* edx, void* weapon)
        {
            if (!g_giving && served_remotely(player))
            {
                Activation active(player);
                if (!active.ok())
                    return g_bump_weapon.original()(player, edx, weapon);
                return bump_weapon(player, edx, weapon);
            }
            return bump_weapon(player, edx, weapon);
        }

        void __fastcall hk_spawn_use(void* spawner, void* edx, void* activator, void* caller, int type, float value)
        {
            if (served_remotely(activator))
            {
                Activation active(activator);
                if (!active.ok())
                {
                    g_spawn_use.original()(spawner, edx, activator, caller, type, value);
                    return;
                }
                spawn_use(spawner, edx, activator, caller, type, value);
                return;
            }
            spawn_use(spawner, edx, activator, caller, type, value);
        }

        // +use on a gun lying in the world does not always come through the pick-up above (slot 426): with incendiary
        // or explosive rounds in the gun on the ground or in the hands (and for guns a player dropped), the game calls
        // the gun's own Use instead,
        // CTerrorGun::Use (0x103E8700, slot 106 of every gun's vtable; thiscall (activator, caller, type, value),
        // ret 0x10), which equips it with Weapon_Equip (player slot 281) and so swaps out, and drops, the gun in the
        // hands.  It gets the same choice (weapon_pickup).
        constexpr std::uintptr_t k_rva_gun_use = 0x3E8700;
        core::Hook<SpawnUseFn> g_gun_use;

        void gun_use(void* gun, void* edx, void* activator, void* caller, int type, float value)
        {
            cut_map_gun(activator, gun);
            if (!g_giving && activator != nullptr && activator == local_player())
            {
                const WeaponPickup pickup = weapon_pickup(activator, gun, item_of(gun));
                if (pickup != WeaponPickup::Game)
                    return;
            }
            g_gun_use.original()(gun, edx, activator, caller, type, value);
        }

        void __fastcall hk_gun_use(void* gun, void* edx, void* activator, void* caller, int type, float value)
        {
            if (served_remotely(activator))
            {
                Activation active(activator);
                if (!active.ok())
                {
                    g_gun_use.original()(gun, edx, activator, caller, type, value);
                    return;
                }
                gun_use(gun, edx, activator, caller, type, value);
                return;
            }
            gun_use(gun, edx, activator, caller, type, value);
        }

        // +use on an ammo pile: CWeaponAmmoSpawn::Use (0x1029F5F0, slot 106 of its vtable 0x10682CF4; thiscall
        // (activator, caller, type, value), ret 0x10) is its own, not CWeaponSpawn's.  A pile of rounds the mod dropped
        // gives its rounds (take_pile), never the endless refill; any other pile is the game's.
        constexpr std::uintptr_t k_rva_ammo_spawn_use = 0x29F5F0;
        constexpr std::uintptr_t k_rva_ammo_spawn_vtable = 0x682CF4;
        core::Hook<SpawnUseFn> g_ammo_spawn_use;

        // ---- the map's ammo piles, finite ([ammo_piles]) ---------------------------------------------------------------
        //
        // With [ammo_piles] on, a map's ammo pile (not one of dropped rounds) holds a set amount: on its first +use it
        // gets a random ammo type of ammo_piles.kinds and min..max magazines of it (inv::ammo_magazine), and it is tinted
        // that type's colour.  Anyone takes as many as fit (take_pile's rule: the type's most, the weight limit; one
        // survivor can empty it), the rest stays; empty, it is removed.  A bot takes only rounds for a gun it carries.
        // What is left is kept here by pile, with its EHANDLE (another entity made at the same address is a new pile);
        // the clock going back (a new map) forgets it all.

        struct FinitePile
        {
            std::uint32_t handle = k_invalid_handle;
            std::string kind;
            int rounds = 0; // left
        };
        std::map<void*, FinitePile> g_finite_piles; // main thread only
        float g_finite_piles_time = 0.f;

        std::minstd_rand& pile_random()
        {
            static std::minstd_rand random(static_cast<unsigned>(GetTickCount64() * 2246822519u));
            return random;
        }

        // The player carries a gun that shoots ammo type `type`.
        bool carries_ammo_type(void* player, int type)
        {
            for (int i = 0; i < k_max_weapons; ++i)
                if (void* weapon = entity_from_handle(handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i))))
                    if (ammo_type_of(weapon) == type)
                        return true;
            return false;
        }

        void use_finite_pile(void* pile, void* activator)
        {
            if (activator == nullptr || team_of(activator) != 2)
                return;
            const inv::AmmoPileRules& rules = inv::ammo_pile_rules();
            const float now = game_time();
            if (now < g_finite_piles_time)
                g_finite_piles.clear(); // a new map
            g_finite_piles_time = now;
            const std::uint32_t handle = handle_of(pile);
            FinitePile& state = g_finite_piles[pile];
            if (state.handle != handle || state.kind.empty())
            {
                // its first +use: what it holds
                std::minstd_rand& random = pile_random();
                const std::string& kind = rules.kinds[random() % rules.kinds.size()];
                const int magazines =
                    rules.min_magazines + static_cast<int>(random() % static_cast<unsigned>(rules.max_magazines - rules.min_magazines + 1));
                state = FinitePile{handle, kind, magazines * std::max(1, inv::ammo_magazine(kind))};
                const unsigned color = inv::ammo_color(kind);
                char tint[16];
                std::snprintf(tint, sizeof(tint), "%u %u %u 255", (color >> 16) & 255u, (color >> 8) & 255u, color & 255u);
                mem::guarded([&] { g.set_key_value(g.tools, pile, "rendercolor", tint); });
                INV_LOG("ammo pile %p: holds %d %s (%d magazine(s))", pile, state.rounds, kind.c_str(), magazines);
            }

            const int type = inv::ammo_index(state.kind);
            const char* title = inv::ammo_title(state.kind);
            const bool bot = is_bot(activator);
            const bool own = !bot && activator == local_player(); // the host, or the player being served: their weight counts
            if (bot && !carries_ammo_type(activator, type))
                return; // (no gun for these rounds)
            const int n = rounds_that_fit(activator, state.kind, state.rounds, own);
            if (n <= 0)
            {
                const int have = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(reserve_of(activator, type)), 0);
                if (own)
                    notify(have >= inv::ammo_max(state.kind) ? std::string("You carry all the ") + title + " you can"
                                                             : std::string("Too heavy for more ") + title);
                return;
            }
            mem::guarded([&] { *reserve_of(activator, type) += n; });
            state_changed(activator);
            state.rounds -= n;
            INV_LOG("ammo pile %p: %s took %d %s, %d left", pile, bot ? "a bot" : "a survivor", n, state.kind.c_str(), state.rounds);
            if (own)
                notify("Took " + std::to_string(n) + " " + title +
                       (state.rounds > 0 ? " (" + std::to_string(state.rounds) + " left in the pile)" : std::string()));
            if (state.rounds <= 0)
            {
                mem::guarded([&] { g.util_remove(pile); });
                g_finite_piles.erase(pile);
            }
        }

        void __fastcall hk_ammo_spawn_use(void* pile, void* edx, void* activator, void* caller, int type, float value)
        {
            std::string kind;
            int rounds = 0;
            if (!pile_contents(pile, kind, rounds))
            {
                if (!inv::ammo_pile_rules().enabled)
                {
                    g_ammo_spawn_use.original()(pile, edx, activator, caller, type, value);
                    return;
                }
                if (served_remotely(activator) && !is_bot(activator))
                {
                    Activation active(activator); // (their weight counts when it is theirs)
                    use_finite_pile(pile, activator);
                    return;
                }
                use_finite_pile(pile, activator);
                return;
            }
            if (served_remotely(activator))
            {
                Activation active(activator); // (their weight counts when it is theirs)
                take_pile(pile, activator, kind, rounds);
                return;
            }
            take_pile(pile, activator, kind, rounds);
        }

        void do_drop_weapon(const std::string& classname);

        // The requests a client's HUD sends (forwarded console commands).  Anything else goes to the game.
        void __cdecl hk_client_command(void* player, const void* args)
        {
            set_ammo_limits(); // (the main thread of the server: a player's command is a good time)
            const auto base = reinterpret_cast<std::uintptr_t>(args);
            const int argc = mem::read_or<int>(base);
            const auto arg = [&](int i) -> std::string {
                const char* text =
                    i >= 0 && i < argc && i < 64 ? mem::read_or<const char*>(base + k_off_args_argv + 4u * static_cast<unsigned>(i)) : nullptr;
                char buffer[128] = {};
                if (text != nullptr)
                    mem::safe_copy(buffer, text, sizeof(buffer) - 1);
                return buffer;
            };
            const std::string command = argc > 0 ? arg(0) : std::string();
            if (player == nullptr || command.rfind("inv_", 0) != 0 || (!g_dedicated && player == host_player()))
            {
                g_client_command.original()(player, args);
                return;
            }
            if (ServerPlayer* sp = server_player(player); sp != nullptr && !sp->has_mod)
            {
                sp->has_mod = true;
                INV_LOG("server: %s runs the mod", sp->id.c_str());
            }
            Activation active(player);
            if (!active.ok())
                return;
            const int a1 = std::atoi(arg(1).c_str());
            const int a2 = std::atoi(arg(2).c_str());
            const std::string text = arg(1);
            const bool weapon_name = text.rfind("weapon_", 0) == 0;
            mem::guarded([&] {
                if (command == "inv_use")
                    do_use(a1);
                else if (command == "inv_drop")
                    do_drop(a1, a2 > 0 ? a2 : 1);
                else if (command == "inv_store")
                    do_store_held();
                else if (command == "inv_pickup")
                    do_pickup_nearby();
                else if (command == "inv_move")
                {
                    Session& s = session();
                    std::lock_guard lock(s.mutex);
                    s.inventory.move(a1, a2);
                }
                else if (command == "inv_select" && weapon_name)
                    do_select_weapon(text);
                else if (command == "inv_dropweapon" && weapon_name)
                {
                    INV_LOG("server: a player asks to drop %s", text.c_str());
                    do_drop_weapon(text);
                }
                else if (command == "inv_dropammo" && inv::ammo_index(text) >= 0)
                    do_drop_ammo(text, a2);
                else if (command == "inv_infect")
                    infect(player, "test");
                else if (command == "inv_spawn")
                {
                    if (const inv::ItemDef* def = inv::find_item(text))
                        do_spawn(*def);
                }
                // inv_sync: nothing to do, the state goes out when `active` ends
            });
        }

        // ---- the infection ([infection] in the rules) -----------------------------------------------------------------
        //
        // A survivor knocked down by the infected, grabbed by a special or vomited on by a boomer is infected; a new hit
        // takes time off the clock, pain pills reset it.  When it runs out the survivor dies (CommitSuicide) and a random
        // special infected rises where they stood (the game's VScript ZSpawn); they cannot be revived before the next
        // chapter: their body (survivor_death_model) is removed, so no defibrillator, and no rescue closet calls for them
        // (CSurvivorRescue::SetSurvivor is told "nobody").  All of it runs on the server's main thread: game events
        // through IGameEventManager2::FireEvent, the clock in IServerGameDLL::GameFrame.
        //
        //   engine.dll IGameEventManager2 slot 7 FireEvent(IGameEvent*, bool), 0x10180830: logs "Game Event Fired: %s"
        //       with the event's GetName.  CGameEvent (vtable 0x103881E4): slot 1 GetName(), slot 6 GetInt(key, default)
        //       (both forward to the event's KeyValues at +8).
        //   engine.dll VEngineServer022 slot 16 GetPlayerUserId(edict), 0x10131FB0: the client of that edict's +0x38.
        //   server.dll ServerGameDLL005 (CServerGameDLL, vtable 0x1060C904) slot 4 GameFrame(bool), 0x100EF4B0 (slot 3
        //       takes three arguments, ServerActivate; 4 and 5 take one bool: GameFrame, PreClientUpdate).
        //   CTerrorPlayer slot 476 CommitSuicide(bool explode, bool force), 0x10174210: what the `kill` command calls
        //       (0x1008EAB0: `call [vtable + 0x770]`).
        //   server.dll 0x1021CB90: the `script` command's handler (cdecl, const CCommand&; the command is FCVAR_CHEAT, its
        //       handler is not): it runs what follows "script " in the command's text in the server's VScript VM.
        //   server.dll 0x102CEAF0 CSurvivorRescue::SetSurvivor(CTerrorPlayer*): a rescue closet starts (or with nullptr
        //       stops) calling for that dead survivor ("(RESCUE) Rescue spot at ... activated").
        //   CBaseEntity m_iTeamNum +0x238 (2 survivors, 3 infected), CBasePlayer m_lifeState +0xF0 (0 alive).

        constexpr std::uintptr_t k_off_team = 0x238;
        constexpr std::uintptr_t k_off_life_state = 0xF0;
        constexpr int k_vt_commit_suicide = 476;
        constexpr std::uintptr_t k_rva_script_command = 0x21CB90;
        constexpr std::uintptr_t k_rva_rescue_set_survivor = 0x2CEAF0;
        constexpr char k_infection_marker[] = "@inf@";

        using EventNameFn = const char*(__thiscall*)(void* event);
        using EventIntFn = int(__thiscall*)(void* event, const char* key, int fallback);
        using FireEventFn = bool(__fastcall*)(void* self, void* edx, void* event, bool dont_broadcast);
        using GameFrameFn = void(__fastcall*)(void* self, void* edx, bool simulating);
        using UserIdFn = int(__thiscall*)(void* engine, void* edict);
        using CommitSuicideFn = void(__thiscall*)(void* player, bool explode, bool force);
        using ScriptCommandFn = void(__cdecl*)(const void* command);
        using SetRescueSurvivorFn = void(__fastcall*)(void* rescue, void* edx, void* survivor);

        core::Hook<FireEventFn> g_fire_event;
        core::Hook<GameFrameFn> g_game_frame;
        core::Hook<SetRescueSurvivorFn> g_rescue_set_survivor;
        UserIdFn g_user_id = nullptr;
        ScriptCommandFn g_script_command = nullptr;

        struct Infection
        {
            ULONGLONG until = 0;
            int total_ms = 0;
            ULONGLONG next_cough = 0; // [infection] cough
            ULONGLONG next_insane = 0; // [infection] insane: the next laugh or scream (0: not that far gone yet)
        };
        std::map<int, Infection> g_infections; // by userid
        std::vector<int> g_no_rescue; // userids that died of it: no defibrillator, no rescue closet before the next chapter
        struct BodyRemoval
        {
            Vec3 at;
            ULONGLONG when;
            int tries;
        };
        std::vector<BodyRemoval> g_body_removals;

        bool contains(const std::vector<int>& list, int value)
        {
            return std::find(list.begin(), list.end(), value) != list.end();
        }

        int user_id_of(void* player)
        {
            void* edict = player != nullptr ? edict_of(player) : nullptr;
            int id = -1;
            if (edict != nullptr && g_user_id != nullptr)
                mem::guarded([&] { id = g_user_id(g_engine_server, edict); });
            return id;
        }

        void* player_by_user_id(int user_id)
        {
            if (user_id <= 0)
                return nullptr;
            for (int index = 1; index <= 64; ++index)
            {
                // a player of any kind: the survivor bots (SurvivorBot) and the specials have vtables of their own, so
                // the classname, not CTerrorPlayer's vtable
                void* player = entity_at_index(index);
                char name[16];
                if (player != nullptr && classname_of(player, name, sizeof(name)) && std::strcmp(name, "player") == 0 &&
                    user_id_of(player) == user_id)
                    return player;
            }
            return nullptr;
        }

        int team_of(void* entity)
        {
            return mem::read_or<int>(reinterpret_cast<std::uintptr_t>(entity) + k_off_team, 0);
        }

        bool alive(void* player)
        {
            return mem::read_or<std::uint8_t>(reinterpret_cast<std::uintptr_t>(player) + k_off_life_state, 1) == 0;
        }

        bool is_bot(void* player)
        {
            const char* raw = nullptr;
            void* edict = edict_of(player);
            char id[16] = {};
            if (edict == nullptr || g_network_id == nullptr || !mem::guarded([&] { raw = g_network_id(g_engine_server, edict); }) ||
                raw == nullptr || !mem::safe_copy(id, raw, sizeof(id) - 1))
                return false;
            return std::strcmp(id, "BOT") == 0;
        }

        // Runs VScript in the server's VM through the `script` command's handler, with a CCommand made here: argc at +0,
        // the whole line at +8 (the handler reads past "script "), argv[] at +0x408 (it checks argv[1]).
        bool run_script(const std::string& code)
        {
            if (g_script_command == nullptr || code.size() > 480)
                return false;
            struct Command
            {
                int argc;
                int argv0_size;
                char args[512];
                char argv_buffer[512];
                const char* argv[64];
            };
            auto command = std::make_unique<Command>();
            std::memset(command.get(), 0, sizeof(Command));
            std::snprintf(command->args, sizeof(command->args), "script %s", code.c_str());
            std::snprintf(command->argv_buffer, sizeof(command->argv_buffer), "script");
            command->argc = 2;
            command->argv0_size = 6;
            command->argv[0] = command->argv_buffer;
            command->argv[1] = command->args + 7;
            return mem::guarded([&] { g_script_command(command.get()); });
        }

        // The infection clock to the player: the host's own session, or a line their client DLL reads
        // ("@inf@ <ms left> <ms in all> <1: turned now>").
        void tell_infection(void* player, const Infection* infection, bool turned = false)
        {
            const ULONGLONG now = GetTickCount64();
            const int remaining = infection != nullptr && infection->until > now ? static_cast<int>(infection->until - now) : 0;
            const int total = infection != nullptr ? infection->total_ms : 0;
            if (player == host_player())
            {
                Session& s = local_session();
                std::lock_guard lock(s.mutex);
                s.infection_until = remaining > 0 ? now + static_cast<ULONGLONG>(remaining) : 0;
                s.infection_total_ms = total;
                if (turned)
                    s.infection_turned_at = now;
                return;
            }
            ServerPlayer* sp = server_player(player);
            if (sp == nullptr || !sp->has_mod || g_client_printf == nullptr)
                return;
            char line[64];
            std::snprintf(line, sizeof(line), "%s %d %d %d\n", k_infection_marker, remaining, total, turned ? 1 : 0);
            void* edict = edict_of(player);
            if (edict != nullptr)
                mem::guarded([&] { g_client_printf(g_engine_server, edict, line); });
        }

        ULONGLONG cough_delay(float severity);

        void infect(void* player, const char* how)
        {
            const inv::InfectionRules& rules = inv::infection_rules();
            if (!rules.enabled)
                return;
            const char* skip = player == nullptr ? "no such player"
                               : team_of(player) != 2 ? "not a survivor"
                               : !alive(player)       ? "dead"
                               : !rules.bots && is_bot(player) ? "a bot (bots = false)"
                                                                : nullptr;
            const int user_id = player != nullptr ? user_id_of(player) : -1;
            if (skip == nullptr && user_id <= 0)
                skip = "no userid";
            if (skip != nullptr)
            {
                INV_LOG("infection: %s not infected (%s): %s", how, skip, player != nullptr ? "" : "the event's victim was not found");
                return;
            }
            const ULONGLONG now = GetTickCount64();
            Infection& infection = g_infections[user_id];
            if (infection.until == 0)
            {
                infection.total_ms = rules.seconds * 1000;
                infection.until = now + static_cast<ULONGLONG>(infection.total_ms);
                infection.next_cough = now + cough_delay(0.f);
                INV_LOG("infection: userid %d infected (%s), %d s", user_id, how, rules.seconds);
            }
            else
            {
                const ULONGLONG cut = static_cast<ULONGLONG>(rules.hit_seconds) * 1000;
                infection.until = infection.until > now + cut + 1000 ? infection.until - cut : now + 1000;
                INV_LOG("infection: userid %d hit again (%s), %llu s left", user_id, how, (infection.until - now) / 1000);
            }
            tell_infection(player, &infection);
        }

        // Pain pills: the clock back to its full time (no_cure, the default), or cured.
        void cure(void* player, const char* how)
        {
            const int user_id = user_id_of(player);
            const auto it = g_infections.find(user_id);
            if (it == g_infections.end())
                return;
            if (inv::infection_rules().no_cure)
            {
                it->second.until = GetTickCount64() + static_cast<ULONGLONG>(it->second.total_ms);
                it->second.next_insane = 0;
                INV_LOG("infection: userid %d took pills (%s): the clock is back to %d s", user_id, how, it->second.total_ms / 1000);
                tell_infection(player, &it->second);
                return;
            }
            g_infections.erase(it);
            INV_LOG("infection: userid %d cured (%s)", user_id, how);
            tell_infection(player, nullptr);
        }

        // How bad it is: 0 just infected .. 1 turning.
        float severity_of(const Infection& infection, ULONGLONG now)
        {
            if (infection.total_ms <= 0 || infection.until <= now)
                return 1.f;
            return std::clamp(1.f - static_cast<float>(infection.until - now) / static_cast<float>(infection.total_ms), 0.f, 1.f);
        }

        // The infected cough now and then, more often as the clock runs out: about every 26 s at first, every 4 s at
        // the end, each wait 60 .. 140 % of that.
        ULONGLONG cough_delay(float severity)
        {
            static std::minstd_rand random(static_cast<unsigned>(GetTickCount64()));
            const float mean = 26.f - 22.f * severity;
            const float spread = 0.6f + 0.8f * static_cast<float>(random() % 1000u) / 1000.f;
            return static_cast<ULONGLONG>(std::max(2.5f, mean * spread) * 1000.f);
        }

        // One of the survivor's own coughs (the soundscripts they cough with in smoke), played on them for everyone to
        // hear.  The survivor is told apart by their model: survivor_coach.mdl -> Coach_Cough01..08, the L4D1 four's
        // entries start with "Player.".  (VScript: GetPlayerFromUserID, GetModelName, PrecacheScriptSound, EmitSoundOn,
        // all in server.dll 2.2.4.3.)  One script line (run_script takes at most 480 characters; a function defined
        // with "::name <- function" by one line was not there for the next, so it is all in one).  An error is written
        // to left4dead2/ems/inv_cough_error.txt.
        void cough(int user_id)
        {
            char code[512];
            const int n = std::snprintf(
                code, sizeof(code),
                "try{local p=GetPlayerFromUserID(%d);if(p){local m=p.GetModelName();"
                "foreach(s in [[\"coac\",\"Coach\",8],[\"gamb\",\"Gambler\",4],[\"mech\",\"Mechanic\",4],[\"prod\",\"Producer\",7],"
                "[\"bike\",\"Player.Biker\",7],[\"mana\",\"Player.Manager\",5],[\"namv\",\"Player.NamVet\",6],"
                "[\"teen\",\"Player.TeenGirl\",6]])if(m.find(s[0])!=null){local n=s[1]+\"_Cough0\"+RandomInt(1,s[2]);"
                "p.PrecacheScriptSound(n);EmitSoundOn(n,p);break;}}}"
                "catch(e){StringToFile(\"inv_cough_error.txt\",\"\"+e)}",
                user_id);
            if (n <= 0 || n >= 480 || !run_script(code))
                INV_LOG("infection: userid %d could not cough (the script failed)", user_id);
        }

        // The last stage ([infection] insane, from insane_at % of the clock gone): the survivor goes insane, laughing
        // for no reason or screaming (their death screams, and "get it off!" at a special that is not there), about
        // every 18 s at first and every 5 s at the end, screams more often as it gets worse.  The soundscripts are
        // the survivors' own (scripts/game_sounds_<survivor>.txt: "<Hearty / Short / Nervous laugh>", "[Loud painful
        // / Short quick death scream]", ScreamWhilePounced); the survivor is told apart by their model:
        // CBaseEntity::m_ModelName +0x24C (datamap "m_ModelName", FIELD_MODELNAME 18: a pointer to
        // "models/survivors/survivor_gambler.mdl" ...; IServerTools GetKeyValue "model" gives that pointer's bytes, not
        // the text).  An error goes to left4dead2/ems/inv_insane_error.txt.
        constexpr std::uintptr_t k_off_model_name = 0x24C;
        struct InsaneVoice
        {
            const char* model; // a part of the model's name
            std::vector<const char*> laughs, screams;
        };
        const std::vector<InsaneVoice>& insane_voices()
        {
            static const std::vector<InsaneVoice> voices = {
                {"coach",
                 {"Coach_Laughter01", "Coach_Laughter04", "Coach_Laughter06", "Coach_Laughter07", "Coach_Laughter13",
                  "Coach_Laughter14", "Coach_Laughter22"},
                 {"Coach_DeathScream01", "Coach_DeathScream02", "Coach_DeathScream03", "Coach_DeathScream04",
                  "Coach_DeathScream05", "Coach_ScreamWhilePounced01", "Coach_ScreamWhilePounced03",
                  "Coach_ScreamWhilePounced05"}},
                {"gambler",
                 {"Gambler_Laughter01", "Gambler_Laughter03", "Gambler_Laughter06", "Gambler_Laughter15",
                  "Gambler_Laughter16", "Gambler_Laughter17"},
                 {"Gambler_DeathScream01", "Gambler_DeathScream02", "Gambler_DeathScream03", "Gambler_DeathScream05",
                  "Gambler_ScreamWhilePounced01", "Gambler_ScreamWhilePounced03", "Gambler_ScreamWhilePounced05"}},
                {"mechanic",
                 {"Mechanic_Laughter04", "Mechanic_Laughter05", "Mechanic_Laughter06", "Mechanic_Laughter09",
                  "Mechanic_Laughter13b", "Mechanic_Laughter13c", "Mechanic_Laughter14"},
                 {"Mechanic_DeathScream01", "Mechanic_DeathScream02", "Mechanic_DeathScream03", "Mechanic_DeathScream04",
                  "Mechanic_ScreamWhilePounced01", "Mechanic_ScreamWhilePounced04", "Mechanic_ScreamWhilePounced04a"}},
                {"producer",
                 {"Producer_Laughter01", "Producer_Laughter04", "Producer_Laughter12", "Producer_Laughter13",
                  "Producer_Laughter14", "Producer_Laughter17"},
                 {"Producer_DeathScream01", "Producer_DeathScream02", "Producer_ScreamWhilePounced01",
                  "Producer_ScreamWhilePounced02", "Producer_ScreamWhilePounced03", "Producer_ScreamWhilePounced05"}},
                {"biker",
                 {"npc.Biker_Laughter01", "npc.Biker_Laughter02", "npc.Biker_Laughter04", "npc.Biker_Laughter05",
                  "npc.Biker_Laughter06", "npc.Biker_Laughter12", "npc.Biker_Laughter13"},
                 {"Player.Biker_DeathScream06", "Player.Biker_DeathScream07", "Player.Biker_DeathScream08",
                  "Player.Biker_DeathScream09", "Player.Biker_ScreamWhilePounced01", "Player.Biker_ScreamWhilePounced02",
                  "Player.Biker_ScreamWhilePounced04"}},
                {"manager",
                 {"npc.Manager_Laughter01", "npc.Manager_Laughter02", "npc.Manager_Laughter04", "npc.Manager_Laughter05",
                  "npc.Manager_Laughter13", "npc.Manager_Laughter14", "npc.Manager_Laughter20"},
                 {"Player.Manager_DeathScream02", "Player.Manager_DeathScream03", "Player.Manager_DeathScream04",
                  "Player.Manager_DeathScream06", "Player.Manager_ScreamWhilePounced01", "Player.Manager_ScreamWhilePounced02"}},
                {"namvet",
                 {"npc.NamVet_Laughter01", "npc.NamVet_Laughter02", "npc.NamVet_Laughter04", "npc.NamVet_Laughter05",
                  "npc.NamVet_Laughter06", "npc.NamVet_Laughter07"},
                 {"Player.NamVet_DeathScream01", "Player.NamVet_DeathScream03", "Player.NamVet_DeathScream04",
                  "Player.NamVet_DeathScream05", "Player.NamVet_ScreamWhilePounced01", "Player.NamVet_ScreamWhilePounced03"}},
                {"teenangst",
                 {"npc.TeenGirl_Laughter02", "npc.TeenGirl_Laughter04", "npc.TeenGirl_Laughter05", "npc.TeenGirl_Laughter09",
                  "npc.TeenGirl_Laughter10", "npc.TeenGirl_Laughter12", "npc.TeenGirl_Laughter13"},
                 {"Player.TeenGirl_DeathScream05", "Player.TeenGirl_DeathScream07", "Player.TeenGirl_DeathScream09",
                  "Player.TeenGirl_DeathScream10", "Player.TeenGirl_ScreamWhilePounced01",
                  "Player.TeenGirl_ScreamWhilePounced02", "Player.TeenGirl_ScreamWhilePounced06"}},
            };
            return voices;
        }

        std::minstd_rand& insane_random()
        {
            static std::minstd_rand random(static_cast<unsigned>(GetTickCount64() * 2654435761u));
            return random;
        }

        // stage: 0 when it starts (insane_at) .. 1 turning
        ULONGLONG insane_delay(float stage)
        {
            const float mean = 18.f - 13.f * stage;
            const float spread = 0.6f + 0.8f * static_cast<float>(insane_random()() % 1000u) / 1000.f;
            return static_cast<ULONGLONG>(std::max(3.f, mean * spread) * 1000.f);
        }

        void go_insane(void* player, int user_id, float stage)
        {
            char model[128] = {};
            const char* name = mem::read_or<const char*>(reinterpret_cast<std::uintptr_t>(player) + k_off_model_name);
            if (name == nullptr || !mem::safe_copy(model, name, sizeof(model) - 1))
                return;
            for (char* c = model; *c != '\0'; ++c)
                *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
            const InsaneVoice* voice = nullptr;
            for (const InsaneVoice& v : insane_voices())
                if (std::strstr(model, v.model) != nullptr)
                    voice = &v;
            if (voice == nullptr)
            {
                INV_LOG("infection: userid %d cannot go insane: no voice for the model \"%s\"", user_id, model);
                return;
            }
            std::minstd_rand& random = insane_random();
            const bool scream = static_cast<float>(random() % 1000u) / 1000.f < 0.3f + 0.4f * stage;
            const std::vector<const char*>& list = scream ? voice->screams : voice->laughs;
            const char* sound = list[random() % list.size()];
            char code[256];
            std::snprintf(code, sizeof(code),
                          "try{local p=GetPlayerFromUserID(%d);if(p){p.PrecacheScriptSound(\"%s\");EmitSoundOn(\"%s\",p);}}"
                          "catch(e){StringToFile(\"inv_insane_error.txt\",\"\"+e)}",
                          user_id, sound, sound);
            const bool ok = run_script(code);
            INV_LOG("infection: userid %d goes insane: %s (%s, stage %.0f%%)", user_id, sound, ok ? "played" : "the script failed",
                    stage * 100.f);
        }

        // The clock ran out: the survivor dies and a random special rises where they stood.
        void turn(void* player, int user_id)
        {
            Vec3 at{};
            mem::read(reinterpret_cast<std::uintptr_t>(player) + k_off_origin, at);
            g_infections.erase(user_id);
            tell_infection(player, nullptr, true);
            const auto suicide = reinterpret_cast<CommitSuicideFn>(mem::vslot(player, k_vt_commit_suicide));
            if (suicide != nullptr)
                mem::guarded([&] { suicide(player, false, true); });
            if (alive(player))
            {
                INV_LOG("infection: userid %d did not die (CommitSuicide refused)", user_id);
                return;
            }
            if (!contains(g_no_rescue, user_id))
                g_no_rescue.push_back(user_id);
            g_body_removals.push_back({at, GetTickCount64() + 200, 0});
            const std::vector<int>& specials = inv::infection_rules().specials;
            const int type = specials.empty() ? 3 : specials[static_cast<std::size_t>(GetTickCount64() % specials.size())];
            // (seen in the game: ZSpawn answered true, by a StringToFile of its result into left4dead2/ems)
            char code[160];
            std::snprintf(code, sizeof(code), "ZSpawn({type = %d, pos = Vector(%.1f, %.1f, %.1f)})", type, at.x, at.y, at.z + 4.f);
            const bool spawned = run_script(code);
            INV_LOG("infection: userid %d turned at %.0f %.0f %.0f: %s (%s)", user_id, at.x, at.y, at.z, code,
                    spawned ? "run" : "failed");
        }

        // The body a defibrillator would use: survivor_death_model, the one nearest to where they died.
        void remove_bodies(ULONGLONG now)
        {
            for (auto it = g_body_removals.begin(); it != g_body_removals.end();)
            {
                if (it->when > now)
                {
                    ++it;
                    continue;
                }
                void* best = nullptr;
                float best_distance = 250.f;
                for (int index = 1; index < 0x1000; ++index)
                {
                    void* entity = entity_at_index(index);
                    char name[32];
                    if (entity == nullptr || !classname_of(entity, name, sizeof(name)) || std::strcmp(name, "survivor_death_model") != 0)
                        continue;
                    Vec3 origin{};
                    if (!mem::read(reinterpret_cast<std::uintptr_t>(entity) + k_off_origin, origin))
                        continue;
                    const float dx = origin.x - it->at.x, dy = origin.y - it->at.y, dz = origin.z - it->at.z;
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (distance < best_distance)
                    {
                        best_distance = distance;
                        best = entity;
                    }
                }
                if (best != nullptr && mem::guarded([&] { g.util_remove(best); }))
                {
                    INV_LOG("infection: body removed (no defibrillator)");
                    it = g_body_removals.erase(it);
                }
                else if (++it->tries >= 15)
                {
                    INV_LOG("infection: no body found to remove");
                    it = g_body_removals.erase(it);
                }
                else
                    (it++)->when = now + 200;
            }
        }

        // ---- stamina ([stamina] in the rules) -------------------------------------------------------------------------
        //
        // Every server tick (GameFrame), for each living survivor: running (on the ground, faster than a walk), a jump
        // (leaving the ground going up), a melee swing (the melee weapon's next attack time moves on) and a shove (the
        // active weapon's next secondary attack time moves on) use stamina; regen_delay seconds after the last use it
        // comes back.  At 0 the survivor is exhausted until it is back to `recover`: their m_flLaggedMovementValue
        // (the game's own movement speed scale) eases down to `slow` while on the ground, the wait after each swing or
        // shove is stretched to `swing_speed`, and CBaseEntity::TakeDamage scales the damage their melee weapon does
        // to `damage`.  The game's own shove fatigue (m_iShovePenalty) is kept at 0 meanwhile.
        //
        // Checked in the game (2026-10-16): a shove waits 0.77 s (pistol or melee) and moves both attack times on, a
        // melee swing too (with the shove penalty not raised); a reload moves the secondary time a tenth or two;
        // gunfire only the primary.  Exhausted, a swing's 0.99 s wait became 1.65 s and a common's 50 damage 25.
        //   CTerrorPlayer m_iShovePenalty +0x2CF0 (SendProp), SetShovePenalty(int) 0x102CB750 (thiscall), which
        //       marks the edict changed; CBasePlayer m_nButtons +0x1CB4, CBaseEntity m_vecAbsVelocity +0x260 (datamap).
        //
        //   CBasePlayer m_fFlags +0x13C (FL_ONGROUND 1), CBaseEntity m_vecVelocity +0x2D8, CBasePlayer
        //       m_flLaggedMovementValue +0x20D0 (the SendProp / datamap definitions: push <offset> / push "<name>").
        //   CBaseCombatWeapon m_flNextPrimaryAttack +0x13F0, m_flNextSecondaryAttack +0x13F4 (game time).
        //   gpGlobals: server.dll PlayerInfoManager002 slot 1 GetGlobalVars (0x101897E0: mov eax, [0x107F7690]);
        //       CGlobalVars curtime +0xC (what most of server.dll reads of it, compared with m_flNextPrimaryAttack).
        //   server.dll 0x10055C70 CBaseEntity::TakeDamage(const CTakeDamageInfo&) (thiscall, ret 4; it warns
        //       "CBaseEntity::TakeDamage:  with inputInfo.GetDamageForce() == vec3_origin").  CTakeDamageInfo is 0x60
        //       bytes (its copy constructor 0x10010140): m_hInflictor +0x30, m_hAttacker +0x34 (what its GetAttacker,
        //       0x1003D4B0, reads), m_hWeapon +0x38, m_flDamage +0x3C, m_bitsDamageType +0x48.

        constexpr std::uintptr_t k_off_flags = 0x13C;
        constexpr std::uintptr_t k_off_velocity = 0x2D8;
        constexpr std::uintptr_t k_off_lagged_movement = 0x20D0;
        constexpr std::uintptr_t k_off_next_primary = 0x13F0;
        constexpr std::uintptr_t k_off_next_secondary = 0x13F4;
        constexpr std::uintptr_t k_off_curtime = 0xC;
        constexpr std::uintptr_t k_off_buttons = 0x1CB4; // CBasePlayer::m_nButtons (datamap)
        constexpr std::uintptr_t k_off_abs_velocity = 0x260; // CBaseEntity::m_vecAbsVelocity (datamap)
        constexpr std::uintptr_t k_off_shove_penalty = 0x2CF0; // CTerrorPlayer::m_iShovePenalty (SendProp)
        constexpr std::uintptr_t k_rva_set_shove_penalty = 0x2CB750;
        constexpr std::uintptr_t k_rva_server_engine = 0x7F7698; // server.dll's own IVEngineServer pointer
        constexpr std::uintptr_t k_rva_change_accessor_call = 0xED4B0;
        constexpr int k_vt_change_accessor = 97;
        constexpr int k_in_attack2 = 1 << 11;
        constexpr std::uintptr_t k_rva_take_damage = 0x55C70;
        constexpr std::size_t k_damage_info_size = 0x60;
        constexpr std::uintptr_t k_off_damage_inflictor = 0x30, k_off_damage_attacker = 0x34, k_off_damage_weapon = 0x38,
                                 k_off_damage = 0x3C;
        constexpr float k_run_speed = 130.f; // faster than this on the ground is running (a walk is 85, a run 220, a limp 150)
        constexpr char k_stamina_marker[] = "@sta@";

        using TakeDamageFn = int(__fastcall*)(void* self, void* edx, const void* info);
        core::Hook<TakeDamageFn> g_take_damage;
        void* g_globals = nullptr; // CGlobalVars
        using SetShovePenaltyFn = void(__thiscall*)(void* player, int penalty);
        SetShovePenaltyFn g_set_shove_penalty = nullptr;

        struct Stamina
        {
            void* player = nullptr;
            float value = 100.f; // 0 .. 100
            bool exhausted = false;
            float used_at = -1000.f; // game time of the last use
            bool on_ground = true;
            void* weapon = nullptr; // in the hands at the last tick
            bool melee = false; // it is a weapon_melee
            float next_primary = 0.f, next_secondary = 0.f;
            float speed = 1.f; // the slowdown, eased (1: none)
            float applied = 1.f; // the m_flLaggedMovementValue set (1: left alone)
            int sent = -2; // what the player was last told (per mille, -1: none)
            bool sent_exhausted = false;
            ULONGLONG sent_at = 0;
            bool seen = false;
        };
        std::map<int, Stamina> g_stamina; // by userid
        float g_stamina_time = 0.f;
        bool g_any_exhausted = false;

        float game_time()
        {
            return g_globals != nullptr ? mem::read_or<float>(reinterpret_cast<std::uintptr_t>(g_globals) + k_off_curtime) : 0.f;
        }

        // A networked field written here goes out to the players only when the entity says it changed, as the
        // game's own setters do (CBaseEdict::StateChanged: m_fStateFlags |= FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED,
        // and the change accessor's serial number 0; VEngineServer022 slot 97 GetChangeAccessor(edict), what
        // SetShovePenalty, 0x102CB750, calls through 0x100ED4B0).
        using ChangeAccessorFn = void*(__thiscall*)(void* engine, void* edict);
        ChangeAccessorFn g_change_accessor = nullptr;

        void state_changed(void* entity)
        {
            void* edict = edict_of(entity);
            if (edict == nullptr || g_change_accessor == nullptr)
                return;
            mem::guarded([&] {
                *static_cast<int*>(edict) |= 0x101;
                if (void* accessor = g_change_accessor(g_engine_server, edict))
                    *reinterpret_cast<std::uint16_t*>(static_cast<char*>(accessor) + 2) = 0;
            });
        }

        void write_float(void* entity, std::uintptr_t offset, float value)
        {
            if (mem::guarded([&] { *reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(entity) + offset) = value; }))
                state_changed(entity);
        }

        // The stamina to the player: the host's own session, or "@sta@ <per mille, -1: none> <1: exhausted>" for their
        // client DLL, at most ten times a second while it changes.
        void tell_stamina(void* player, Stamina& st, bool gone)
        {
            const int permille = gone ? -1 : static_cast<int>(std::lround(st.value * 10.f));
            const bool exhausted = !gone && st.exhausted;
            if (permille == st.sent && exhausted == st.sent_exhausted)
                return;
            const ULONGLONG now = GetTickCount64();
            if (player == host_player())
            {
                Session& s = local_session();
                std::lock_guard lock(s.mutex);
                s.stamina = permille < 0 ? -1.f : static_cast<float>(permille) / 1000.f;
                s.stamina_exhausted = exhausted;
            }
            else
            {
                const bool edge = gone || exhausted != st.sent_exhausted || permille == 0 || permille == 1000;
                if (!edge && now - st.sent_at < 100)
                    return;
                ServerPlayer* sp = server_player(player);
                void* edict = edict_of(player);
                if (sp != nullptr && sp->has_mod && g_client_printf != nullptr && edict != nullptr)
                {
                    char line[48];
                    std::snprintf(line, sizeof(line), "%s %d %d\n", k_stamina_marker, permille, exhausted ? 1 : 0);
                    mem::guarded([&] { g_client_printf(g_engine_server, edict, line); });
                }
            }
            st.sent = permille;
            st.sent_exhausted = exhausted;
            st.sent_at = now;
        }

        // Done with a player (gone, dead, stamina switched off): their speed back, their bar away.
        void stamina_release(int user_id, Stamina& st)
        {
            void* player = player_by_user_id(user_id);
            if (player == nullptr || player != st.player)
                return;
            if (st.applied != 1.f)
                write_float(player, k_off_lagged_movement, 1.f);
            tell_stamina(player, st, true);
        }

        void stamina_clear()
        {
            for (auto& [user_id, st] : g_stamina)
                stamina_release(user_id, st);
            g_stamina.clear();
            g_any_exhausted = false;
        }

        void stamina_use(int user_id, Stamina& st, float cost, float now, const char* what)
        {
            if (cost <= 0.f)
                return;
            st.value = std::max(0.f, st.value - cost);
            st.used_at = now;
            if (st.value <= 0.f && !st.exhausted)
            {
                st.exhausted = true;
                INV_LOG("stamina: userid %d is exhausted (%s)", user_id, what);
            }
        }

        void stamina_tick(void* player, int user_id, Stamina& st, const inv::StaminaRules& rules, float now, float dt)
        {
            const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(player);
            const bool on_ground = (mem::read_or<int>(p + k_off_flags) & 1) != 0;
            Vec3 velocity{};
            mem::read(p + k_off_velocity, velocity);
            const bool running = on_ground && std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y) > k_run_speed;
            const bool was_exhausted = st.exhausted;

            if (running && !st.exhausted && rules.run > 0.f)
                stamina_use(user_id, st, rules.run * dt, now, "running");
            if (st.on_ground && !on_ground && velocity.z > 150.f)
                stamina_use(user_id, st, rules.jump, now, "a jump");
            // Leaving the ground slowed (a jump, a step off a ledge): the slowdown is m_flLaggedMovementValue, which
            // slows the player's whole movement clock, falls too, so in the air it is 1 and the slowness goes into
            // the speed they leave the ground with instead.
            if (st.on_ground && !on_ground && st.applied < 1.f)
            {
                const float keep = st.applied;
                mem::guarded([&] {
                    for (const std::uintptr_t offset : {k_off_velocity, k_off_abs_velocity})
                    {
                        float* v = reinterpret_cast<float*>(p + offset);
                        v[0] *= keep;
                        v[1] *= keep;
                    }
                });
                state_changed(player);
            }
            st.on_ground = on_ground;

            // the game's own shove fatigue (each shove in a row waits longer) is off: stamina is the limit.  Every
            // shove adds to it, so it also tells a shove from a melee swing (both move both attack times on).
            const int shove_penalty = mem::read_or<int>(p + k_off_shove_penalty);
            if (g_set_shove_penalty != nullptr && shove_penalty != 0)
                mem::guarded([&] { g_set_shove_penalty(player, 0); });

            // a swing or a shove: the weapon's wait for its next (primary / secondary) attack moved on
            void* weapon = entity_from_handle(handle_at(player, k_off_active_weapon));
            const std::uintptr_t w = reinterpret_cast<std::uintptr_t>(weapon);
            if (weapon != st.weapon)
            {
                char name[32];
                st.weapon = weapon;
                st.melee = weapon != nullptr && classname_of(weapon, name, sizeof(name)) && std::strcmp(name, "weapon_melee") == 0;
                st.next_primary = weapon != nullptr ? mem::read_or<float>(w + k_off_next_primary) : 0.f;
                st.next_secondary = weapon != nullptr ? mem::read_or<float>(w + k_off_next_secondary) : 0.f;
            }
            else if (weapon != nullptr)
            {
                float primary = mem::read_or<float>(w + k_off_next_primary);
                float secondary = mem::read_or<float>(w + k_off_next_secondary);
                // a melee swing moves both on, so the button held tells them apart (a shove: IN_ATTACK2)
                const int buttons = mem::read_or<int>(p + k_off_buttons);
                const bool primary_moved = primary > st.next_primary + 0.05f && primary > now;
                // (a shove waits 0.77 s; a reload moves the secondary time on by a tenth or two, so it is not one)
                const bool secondary_moved = secondary > st.next_secondary + 0.05f && secondary > now + 0.4f;
                const bool shove =
                    secondary_moved && (shove_penalty != 0 || (buttons & k_in_attack2) != 0 || !st.melee || !primary_moved);
                const bool swing = !shove && st.melee && primary_moved;
                if (shove || swing)
                {
                    if (was_exhausted && rules.swing_speed < 100.f)
                    {
                        // the wait for the next one, longer
                        const float stretch = 100.f / rules.swing_speed;
                        if (primary > now)
                            write_float(weapon, k_off_next_primary, primary = now + (primary - now) * stretch);
                        if (shove && secondary > now)
                            write_float(weapon, k_off_next_secondary, secondary = now + (secondary - now) * stretch);
                    }
                    stamina_use(user_id, st, shove ? rules.shove : rules.swing, now, shove ? "a shove" : "a swing");
                }
                st.next_primary = primary;
                st.next_secondary = secondary;
            }

            // rest: it comes back (at half the rate while an exhausted survivor keeps running)
            if (now - st.used_at >= rules.regen_delay && st.value < 100.f)
                st.value = std::min(100.f, st.value + rules.regen * dt * (running ? 0.5f : 1.f));
            if (st.exhausted && st.value >= std::max(rules.recover, 0.1f))
            {
                st.exhausted = false;
                INV_LOG("stamina: userid %d got their breath back (%.0f)", user_id, st.value);
            }

            // the speed eases to `slow` and back; on the ground only (see above)
            const float target = st.exhausted ? rules.slow / 100.f : 1.f;
            const float step = 1.5f * dt;
            st.speed = st.speed < target ? std::min(target, st.speed + step) : std::max(target, st.speed - step);
            const float apply = on_ground ? st.speed : 1.f;
            if (apply != st.applied || (apply != 1.f && mem::read_or<float>(p + k_off_lagged_movement, apply) != apply))
            {
                write_float(player, k_off_lagged_movement, apply);
                st.applied = apply;
            }
            tell_stamina(player, st, false);
        }

        void stamina_frame()
        {
            const inv::StaminaRules& rules = inv::stamina_rules();
            if (!rules.enabled || g_globals == nullptr)
            {
                if (!g_stamina.empty())
                    stamina_clear();
                return;
            }
            const float now = game_time();
            float dt = now - g_stamina_time;
            if (dt == 0.f)
                return; // the same tick
            g_stamina_time = now;
            if (dt < 0.f || dt > 0.25f)
                dt = 0.f; // a new map, a long hitch
            for (auto& [user_id, st] : g_stamina)
                st.seen = false;
            for (int index = 1; index <= 64; ++index)
            {
                void* player = entity_at_index(index);
                char name[16];
                if (player == nullptr || !classname_of(player, name, sizeof(name)) || std::strcmp(name, "player") != 0 ||
                    team_of(player) != 2 || !alive(player) || (!rules.bots && is_bot(player)))
                    continue;
                const int user_id = user_id_of(player);
                if (user_id <= 0)
                    continue;
                Stamina& st = g_stamina[user_id];
                if (st.player != player)
                {
                    st = Stamina{};
                    st.player = player;
                    st.applied = mem::read_or<float>(reinterpret_cast<std::uintptr_t>(player) + k_off_lagged_movement, 1.f);
                    if (st.applied != 1.f)
                        INV_LOG("stamina: userid %d starts with a speed scale of %.2f", user_id, st.applied);
                }
                st.seen = true;
                stamina_tick(player, user_id, st, rules, now, dt);
            }
            g_any_exhausted = false;
            for (auto it = g_stamina.begin(); it != g_stamina.end();)
            {
                if (!it->second.seen)
                {
                    stamina_release(it->first, it->second);
                    it = g_stamina.erase(it);
                    continue;
                }
                g_any_exhausted |= it->second.exhausted;
                ++it;
            }
        }

        // An exhausted survivor's melee weapon does less damage.
        int __fastcall hk_take_damage(void* self, void* edx, const void* info)
        {
            if (!g_stamina.empty() && info != nullptr)
            {
                alignas(16) unsigned char copy[k_damage_info_size];
                bool scaled = false;
                mem::guarded([&] {
                    const std::uintptr_t i = reinterpret_cast<std::uintptr_t>(info);
                    void* attacker = entity_from_handle(*reinterpret_cast<const std::uint32_t*>(i + k_off_damage_attacker));
                    if (attacker == nullptr)
                        return;
                    for (const auto& entry : g_stamina)
                    {
                        const Stamina& st = entry.second;
                        if (st.player != attacker)
                            continue;
                        if (!st.exhausted || !st.melee || st.weapon == nullptr)
                            return;
                        if (entity_from_handle(*reinterpret_cast<const std::uint32_t*>(i + k_off_damage_weapon)) != st.weapon &&
                            entity_from_handle(*reinterpret_cast<const std::uint32_t*>(i + k_off_damage_inflictor)) != st.weapon)
                            return;
                        std::memcpy(copy, info, sizeof(copy));
                        // (seen in the game: a common infected's 50 became 25, so it took two hits)
                        *reinterpret_cast<float*>(copy + k_off_damage) *= inv::stamina_rules().damage / 100.f;
                        scaled = true;
                        return;
                    }
                });
                if (scaled)
                    return g_take_damage.original()(self, edx, copy);
            }
            return g_take_damage.original()(self, edx, info);
        }

        // Nobody is infected any more: a new round or map, the escape.  The host's own effect goes in any case (the host
        // may not be a player yet, or any more).
        void infection_all_clear(const char* why)
        {
            if (!g_infections.empty() || !g_no_rescue.empty())
                INV_LOG("infection: %s, all clear", why);
            for (const auto& [user_id, infection] : g_infections)
                if (void* player = player_by_user_id(user_id))
                    tell_infection(player, nullptr);
            g_infections.clear();
            g_no_rescue.clear();
            g_body_removals.clear();
            if (!g_dedicated)
            {
                Session& s = local_session();
                std::lock_guard lock(s.mutex);
                s.infection_until = 0;
                s.infection_total_ms = 0;
            }
        }

        void on_game_event(void* event)
        {
            if (!inv::infection_rules().enabled)
                return;
            const auto name_fn = reinterpret_cast<EventNameFn>(mem::vslot(event, 1));
            const auto int_fn = reinterpret_cast<EventIntFn>(mem::vslot(event, 6));
            const char* name = name_fn != nullptr ? name_fn(event) : nullptr;
            if (name == nullptr || int_fn == nullptr)
                return;
            const auto get = [&](const char* key) { return int_fn(event, key, 0); };
            const std::string_view n(name);
            if (n == "tongue_grab" || n == "lunge_pounce" || n == "jockey_ride" || n == "charger_carry_start" ||
                n == "charger_pummel_start" || n == "player_now_it" || n == "player_incapacitated" || n == "pills_used")
                INV_LOG("infection: event %s (userid %d, victim %d, subject %d, attacker %d)", name, get("userid"),
                        get("victim"), get("subject"), get("attacker"));
            if (n == "tongue_grab" || n == "lunge_pounce" || n == "jockey_ride" || n == "charger_carry_start" ||
                n == "charger_pummel_start")
                infect(player_by_user_id(get("victim")), name);
            else if (n == "player_now_it" && (get("by_boomer") != 0 || get("exploded") != 0)) // a boomer's vomit or burst
                infect(player_by_user_id(get("userid")), name);
            else if (n == "player_incapacitated")
            {
                // by the infected: a special (a player on team 3), a common infected or a witch
                bool by_infected = false;
                if (void* attacker = player_by_user_id(get("attacker")))
                    by_infected = team_of(attacker) == 3;
                else if (void* entity = entity_at_index(get("attackerentid")))
                {
                    char cls[32];
                    by_infected = classname_of(entity, cls, sizeof(cls)) &&
                                  (std::strcmp(cls, "infected") == 0 || std::strcmp(cls, "witch") == 0);
                }
                if (by_infected)
                    infect(player_by_user_id(get("userid")), name);
            }
            else if (n == "pills_used")
            {
                const int subject = get("subject");
                cure(player_by_user_id(subject > 0 ? subject : get("userid")), name);
            }
            else if (n == "player_death")
            {
                const int user_id = get("userid");
                if (g_infections.erase(user_id) != 0)
                    if (void* player = player_by_user_id(user_id))
                        tell_infection(player, nullptr);
            }
            else if (n == "round_start" || n == "map_transition" || n == "round_end" || n == "mission_lost" ||
                     n == "finale_vehicle_leaving" || n == "finale_win") // (the last two: the escape, its cutscene)
                infection_all_clear(name);
        }

        bool __fastcall hk_fire_event(void* self, void* edx, void* event, bool dont_broadcast)
        {
            if (event != nullptr)
                mem::guarded([&] { on_game_event(event); });
            return g_fire_event.original()(self, edx, event, dont_broadcast);
        }

        // Ammo piles (and guns picked up) fill the reserve as the game does, up to the type's most; with a weight limit
        // only what fits by weight is kept (see "rounds"): ten times a second the reserves of the players who have an
        // inventory here (the host, the served players) are compared with the last look, and a rise that takes the load
        // past the limit is taken back down to what fits.
        struct ReserveLook
        {
            void* player = nullptr;
            int pool[32] = {};
        };
        std::map<int, ReserveLook> g_reserves; // by userid
        ULONGLONG g_reserves_at = 0;

        void cap_reserve(void* player, int user_id, Session& s, bool host)
        {
            AmmoPool pool = read_pool(player);
            if (!pool.read)
                return;
            ReserveLook& look = g_reserves[user_id];
            if (look.player != player)
            {
                look.player = player;
                std::memcpy(look.pool, pool.count, sizeof(look.pool));
                return;
            }
            float max_weight = 0.f, load = 0.f;
            {
                std::lock_guard lock(s.mutex);
                max_weight = s.inventory.limits().max_weight;
                load = s.inventory.weight() - s.inventory.ammo_weight() + inv::ammo_weight(pool.count);
            }
            bool changed = false;
            if (max_weight > 0.f && load > max_weight + 1e-3f)
                for (const char* kind : inv::ammo_kinds())
                {
                    const int type = inv::ammo_index(kind);
                    const int rise = pool.count[type] - look.pool[type];
                    const float each = inv::round_weight(kind);
                    if (rise <= 0 || each <= 0.f)
                        continue;
                    const int back = std::min(rise, static_cast<int>(std::ceil((load - max_weight) / each - 1e-4f)));
                    pool.count[type] -= back;
                    load -= static_cast<float>(back) * each;
                    changed = true;
                    INV_LOG("rounds: userid %d got %d %s, %d fit by weight (%.1f of %.1f)", user_id, rise, kind, rise - back,
                            load, max_weight);
                    if (host)
                        notify("Too heavy: only " + std::to_string(rise - back) + " " + inv::ammo_title(kind) + " fit");
                    if (load <= max_weight + 1e-3f)
                        break;
                }
            if (changed)
            {
                write_pool(player, pool);
                state_changed(player);
            }
            std::memcpy(look.pool, pool.count, sizeof(look.pool));
        }

        void cap_reserves()
        {
            const ULONGLONG now = GetTickCount64();
            if (now - g_reserves_at < 100)
                return;
            g_reserves_at = now;
            void* host = host_player();
            for (int index = 1; index <= 64; ++index)
            {
                void* player = entity_at_index(index);
                char name[16];
                if (player == nullptr || !classname_of(player, name, sizeof(name)) || std::strcmp(name, "player") != 0 ||
                    team_of(player) != 2 || !alive(player))
                    continue;
                know_held_guns(player);
                Session* s = nullptr;
                if (player == host)
                    s = &local_session();
                else if (ServerPlayer* sp = server_player(player); sp != nullptr && sp->has_mod)
                    s = &sp->session;
                const int user_id = s != nullptr ? user_id_of(player) : -1;
                if (user_id > 0)
                    cap_reserve(player, user_id, *s, player == host);
            }
        }

        void __fastcall hk_game_frame(void* self, void* edx, bool simulating)
        {
            g_game_frame.original()(self, edx, simulating);
            if (simulating)
                mem::guarded([] {
                    stamina_frame();
                    cap_reserves();
                });
            // a new map without the events above (a new campaign from the menu): the game's clock went back
            static float last_time = 0.f;
            if (g_globals != nullptr)
            {
                const float time = game_time();
                if (time + 1.f < last_time && (!g_infections.empty() || !g_no_rescue.empty()))
                    infection_all_clear("a new map");
                last_time = time;
            }
            if (g_infections.empty() && g_body_removals.empty())
                return;
            const ULONGLONG now = GetTickCount64();
            mem::guarded([&] {
                std::vector<int> due;
                for (auto& [user_id, infection] : g_infections)
                {
                    if (infection.until <= now)
                    {
                        due.push_back(user_id);
                        continue;
                    }
                    const inv::InfectionRules& rules = inv::infection_rules();
                    const float severity = severity_of(infection, now);
                    if (rules.cough && infection.next_cough <= now)
                    {
                        infection.next_cough = now + cough_delay(severity);
                        if (void* player = player_by_user_id(user_id); player != nullptr && alive(player))
                        {
                            cough(user_id);
                            infection.next_insane = std::max(infection.next_insane, now + 2500); // not over each other
                            INV_LOG("infection: userid %d coughs (%.0f%%, the next in %llu s)", user_id, severity * 100.f,
                                    (infection.next_cough - now) / 1000);
                        }
                    }
                    // the last stage: insane (see go_insane)
                    if (!rules.insane || severity * 100.f < static_cast<float>(rules.insane_at))
                        infection.next_insane = 0;
                    else if (infection.next_insane == 0)
                        infection.next_insane = now + 2000 + static_cast<ULONGLONG>(GetTickCount64() % 4000); // soon after
                    else if (infection.next_insane <= now)
                    {
                        const float stage =
                            rules.insane_at >= 100 ? 1.f
                                                   : std::clamp((severity * 100.f - rules.insane_at) / (100.f - rules.insane_at), 0.f, 1.f);
                        infection.next_insane = now + insane_delay(stage);
                        if (void* player = player_by_user_id(user_id); player != nullptr && alive(player))
                        {
                            go_insane(player, user_id, stage);
                            infection.next_cough = std::max(infection.next_cough, now + 3000);
                        }
                    }
                }
                for (const int user_id : due)
                {
                    void* player = player_by_user_id(user_id);
                    if (player == nullptr || !alive(player))
                        g_infections.erase(user_id);
                    else
                        turn(player, user_id);
                }
                remove_bodies(now);
            });
        }

        void __fastcall hk_rescue_set_survivor(void* rescue, void* edx, void* survivor)
        {
            if (survivor != nullptr && !g_no_rescue.empty() && contains(g_no_rescue, user_id_of(survivor)))
            {
                static ULONGLONG logged = 0;
                if (GetTickCount64() - logged > 10000)
                {
                    logged = GetTickCount64();
                    INV_LOG("infection: a rescue closet was going to call for a survivor who died of it: nobody instead");
                }
                survivor = nullptr;
            }
            g_rescue_set_survivor.original()(rescue, edx, survivor);
        }

        // True while a vgui panel has the keyboard: the console, the chat line, a menu.  Slot 4 of vgui2.dll's
        // VGUI_Input005 (vgui::CInputSystem) is GetFocus(): 0 in the game, a panel while the console or chat is open
        // (checked in the game).  Must run on the main thread, which is where the window procedure calls it.
        bool input_has_focus()
        {
            if (g.vgui_input == nullptr)
                return false;
            const auto get_focus = reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(mem::vslot(g.vgui_input, 4));
            std::uintptr_t focus = 0;
            if (get_focus == nullptr || !mem::guarded([&] { focus = get_focus(g.vgui_input); }))
                return false;
            return focus != 0;
        }

        // ---- the game's own slots -----------------------------------------------------------------------------------

        // "weapon_rifle_ak47" -> name "Rifle Ak47", tag "RA"
        NativeWeapon describe_weapon(const char* classname)
        {
            NativeWeapon out;
            out.classname = classname;
            const char* p = classname;
            if (strncmp(p, "weapon_", 7) == 0)
                p += 7;
            bool word_start = true;
            int tag_len = 0;
            for (; *p != '\0'; ++p)
            {
                const char c = *p == '_' ? ' ' : *p;
                out.name += word_start ? static_cast<char>(toupper(c)) : c;
                if (word_start && c != ' ' && tag_len < 2)
                    out.tag[tag_len++] = static_cast<char>(toupper(c));
                word_start = c == ' ';
            }
            if (tag_len < 2 && out.name.size() > 1)
                out.tag[tag_len++] = static_cast<char>(toupper(out.name[1]));
            return out;
        }

        // The game slot a carried weapon is in: the catalogue knows the items; of the rest, pistols and melee weapons are
        // the secondary slot (1), everything else is a primary (0).
        int native_slot_of(const char* classname)
        {
            if (const inv::ItemDef* def = inv::find_item(classname))
                return def->game_slot;
            if (std::strstr(classname, "pistol") != nullptr || std::strcmp(classname, "weapon_melee") == 0 ||
                std::strcmp(classname, "weapon_chainsaw") == 0)
                return 1;
            return 0;
        }

        // Main thread: what the player carries in m_hMyWeapons, into the session for the HUD, one entry per game slot.
        void refresh_native()
        {
            std::vector<NativeWeapon> list(k_native_slots);
            float rounds_weight = 0.f; // the reserve's (see "rounds")
            if (void* player = local_player())
            {
                const AmmoPool pool = read_pool(player);
                if (pool.read)
                    rounds_weight = inv::ammo_weight(pool.count);
                const std::uint32_t active = handle_at(player, k_off_active_weapon);
                for (int i = 0; i < k_max_weapons; ++i)
                {
                    const std::uint32_t handle = handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i));
                    void* weapon = entity_from_handle(handle);
                    char name[64];
                    if (weapon == nullptr || !classname_of(weapon, name, sizeof(name)))
                        continue;
                    const int slot = native_slot_of(name);
                    if (slot < 0 || slot >= k_native_slots || !list[static_cast<std::size_t>(slot)].classname.empty())
                        continue;
                    NativeWeapon entry = describe_weapon(name);
                    entry.active = handle == active;
                    if (std::strcmp(name, "weapon_melee") == 0) // which melee weapon, for its icon
                        if (const inv::ItemDef* melee = inv::find_item(name))
                            entry.melee = weapon_state(player, weapon, *melee, true).melee;
                    list[static_cast<std::size_t>(slot)] = std::move(entry);
                }
            }
            Session& s = session();
            std::lock_guard lock(s.mutex);
            // the "in hand" marks of the inventory's boxes follow what the player really carries
            inv::Inventory& inventory = s.inventory;
            for (int i = 0; i < inventory.slot_count(); ++i)
            {
                const inv::Stack& stack = inventory.slot(i);
                if (!stack.held)
                    continue;
                const bool carried = std::any_of(list.begin(), list.end(), [&](const NativeWeapon& w) {
                    return w.classname == stack.def->classname;
                });
                if (!carried || inventory.held_slot(*stack.def) != i)
                    inventory.clear_held(i);
            }
            for (const NativeWeapon& weapon : list)
            {
                const inv::ItemDef* def = weapon.classname.empty() ? nullptr : inv::find_item(weapon.classname);
                if (def != nullptr && !def->weapon() && inventory.held_slot(*def) < 0)
                    inventory.set_held(inventory.home_for(*def), *def); // picked up in the game: it gets a box
            }
            // the gun and the melee / pistol count against the weight limit too
            float weapons = 0.f;
            for (const NativeWeapon& weapon : list)
                weapons += inv::weapon_weight(weapon.classname);
            inventory.set_weapons_weight(weapons);
            inventory.set_ammo_weight(rounds_weight);
            s.native = std::move(list);
        }

        // Drops a carried weapon the way the game's own DropItem does.
        void do_drop_weapon(const std::string& classname)
        {
            void* player = local_player();
            if (player == nullptr)
                return;
            if (const auto cannot = reinterpret_cast<CannotDropFn>(mem::vslot(player, k_vt_cannot_drop));
                cannot != nullptr && cannot(player) != 0)
            {
                notify("The game does not let you drop now");
                return;
            }
            for (int i = 0; i < k_max_weapons; ++i)
            {
                void* weapon = entity_from_handle(handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i)));
                char name[64];
                if (weapon == nullptr || !classname_of(weapon, name, sizeof(name)) || classname != name)
                    continue;
                const auto drop = reinterpret_cast<WeaponDropFn>(mem::vslot(player, k_vt_weapon_drop));
                if (drop == nullptr)
                    return;
                drop(player, weapon, nullptr, nullptr);
                INV_LOG("dropped %s", name);
                notify(std::string("Dropped ") + describe_weapon(name).name);
                return;
            }
        }

        char g_name_buffer[64];

        // `use <classname>`: the console command calls CBasePlayer::SelectItem (slot 429) with the name
        void do_select_weapon(const std::string& classname)
        {
            void* player = local_player();
            if (player == nullptr)
                return;
            put_away_carried(player, classname.c_str());
            const auto select_item = reinterpret_cast<SelectItemFn>(mem::vslot(player, k_vt_select_item));
            if (select_item != nullptr)
            {
                select_item(player, classname.c_str(), 0);
                INV_LOG("select %s: active weapon is now %s", classname.c_str(),
                        classname_of(entity_from_handle(handle_at(player, k_off_active_weapon)), g_name_buffer, sizeof(g_name_buffer))
                            ? g_name_buffer
                            : "?");
            }
        }

        // ---- playing on a dedicated server (client side) ----------------------------------------------------------
        //
        // When the game client is on a server it does not run (no local player in its own server.dll, but the engine
        // is in a game), the HUD's requests go to the server as console commands, and the inventory shown is the one the
        // server sends back (send_state above).
        //
        //   engine.dll VEngineClient013 slot 26 IsInGame() (signon state == 6), slot 107 ClientCmd_Unrestricted(text)
        //       (Cbuf_AddText; a command the client does not know is forwarded to the server).
        //   engine.dll 0x10052100 CBaseClientState::ProcessPrint(SVC_Print*), thiscall, ret 4: ConMsg("%s", msg + 0x54).
        //       The state line is taken out there, before it reaches the console.

        constexpr std::uintptr_t k_rva_process_print = 0x52100;
        constexpr std::uintptr_t k_off_print_text = 0x54;
        constexpr std::uint32_t k_engine_timestamp = 0x6A440ED6;

        using IsInGameFn = bool(__thiscall*)(void* engine);
        using ClientCmdFn = void(__thiscall*)(void* engine, const char* text);
        using ProcessPrintFn = bool(__fastcall*)(void* state, void* edx, void* msg);

        void* g_engine_client = nullptr;
        IsInGameFn g_is_in_game = nullptr;
        ClientCmdFn g_client_cmd = nullptr;
        core::Hook<ProcessPrintFn> g_process_print;
        ULONGLONG g_last_sync = 0;

        bool in_remote_game()
        {
            if (g_dedicated || g_engine_client == nullptr || g_is_in_game == nullptr || local_player() != nullptr)
                return false;
            bool in_game = false;
            mem::guarded([&] { in_game = g_is_in_game(g_engine_client); });
            return in_game;
        }

        void send_to_server(const std::string& command)
        {
            if (g_client_cmd != nullptr)
                mem::guarded([&] { g_client_cmd(g_engine_client, command.c_str()); });
        }

        // The local inventory comes back when the player is no longer on a dedicated server.
        void leave_remote()
        {
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                if (!s.remote)
                    return;
                s.remote = false;
                s.remote_weight = -1.f;
                s.stamina = -1.f; // the server's; a local game sets its own
                s.stamina_exhausted = false;
            }
            session_apply_limits();
            const std::string saved = core::inventory_file_load();
            Session& s = session();
            std::lock_guard lock(s.mutex);
            inv::Inventory local(config_limits());
            if (!saved.empty())
                local.from_text(saved);
            s.inventory = std::move(local);
            s.native.assign(k_native_slots, NativeWeapon{});
            INV_LOG("remote: back to the local inventory");
        }

        // Reads a state line (see send_state) into the session.
        void apply_state(const char* text)
        {
            std::string line(text + sizeof(k_state_marker) - 1);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            std::vector<std::string> parts;
            for (std::size_t start = 0;;)
            {
                const std::size_t bar = line.find('|', start);
                if (bar == std::string::npos || parts.size() == 3)
                {
                    parts.push_back(line.substr(start));
                    break;
                }
                parts.push_back(line.substr(start, bar - start));
                start = bar + 1;
            }
            if (parts.size() < 4)
                return;
            int slots = 0;
            float max_weight = 0.f;
            float weight = -1.f;
            int weapon_slots = 0; // a server from before the weapon boxes sends none
            if (std::sscanf(parts[0].c_str(), "%d %f %f %d", &slots, &max_weight, &weight, &weapon_slots) < 1 || slots < 1 || slots > 64)
                return;
            inv::Inventory inventory({.slots = slots, .max_weight = max_weight, .weapon_slots = weapon_slots});
            for (std::size_t start = 0; start < parts[1].size();)
            {
                std::size_t comma = parts[1].find(',', start);
                if (comma == std::string::npos)
                    comma = parts[1].size();
                int index = 0, count = 0, held = 0;
                char name[64] = {};
                inv::WeaponState state;
                char melee[64] = {};
                const int read = std::sscanf(parts[1].substr(start, comma - start).c_str(), "%d %63s %d %d %d %d %63s %d %d",
                                             &index, name, &count, &held, &state.clip, &state.ammo, melee, &state.upgrades,
                                             &state.special);
                if (read >= 4)
                    if (const inv::ItemDef* def = inv::find_item(name))
                    {
                        if (def->weapon())
                        {
                            if (read >= 7 && std::strcmp(melee, "-") != 0)
                                state.melee = melee;
                            inventory.add_weapon(*def, state, index);
                        }
                        else if (count > 0)
                            inventory.add_to_slot(index, *def, count);
                        if (held != 0 && !def->weapon())
                            inventory.set_held(index, *def);
                    }
                start = comma + 1;
            }
            std::vector<NativeWeapon> native(k_native_slots);
            std::size_t slot = 0;
            for (std::size_t start = 0; start <= parts[2].size() && slot < native.size(); ++slot)
            {
                std::size_t comma = parts[2].find(',', start);
                if (comma == std::string::npos)
                    comma = parts[2].size();
                std::string name = parts[2].substr(start, comma - start);
                const bool active = !name.empty() && name[0] == '*';
                if (active)
                    name.erase(0, 1);
                std::string melee; // "weapon_melee:fireaxe"
                if (const std::size_t colon = name.find(':'); colon != std::string::npos)
                {
                    melee = name.substr(colon + 1);
                    name.erase(colon);
                }
                if (!name.empty() && name != "-")
                {
                    native[slot] = describe_weapon(name.c_str());
                    native[slot].active = active;
                    native[slot].melee = melee;
                }
                start = comma + 1;
            }
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                inventory.select(s.inventory.selected());
                s.inventory = std::move(inventory);
                s.native = std::move(native);
                s.remote_weight = weight;
                if (!s.remote)
                    INV_LOG("remote: the server keeps this inventory now (%d slots)", slots);
                s.remote = true;
            }
            if (!parts[3].empty())
                notify(parts[3]);
        }

        bool __fastcall hk_process_print(void* state, void* edx, void* msg)
        {
            const char* text = mem::read_or<const char*>(reinterpret_cast<std::uintptr_t>(msg) + k_off_print_text);
            char head[sizeof(k_state_marker)] = {};
            if (text != nullptr && mem::safe_copy(head, text, sizeof(head) - 1) &&
                std::strcmp(head, k_state_marker) == 0)
            {
                mem::guarded([&] { apply_state(text); });
                return true; // not printed
            }
            // the infection clock: "@inf@ <ms left> <ms in all> <turned>" (0 0: none; turned 1: the clock ran out)
            if (text != nullptr && std::strcmp(head, k_infection_marker) == 0)
            {
                mem::guarded([&] {
                    int remaining = 0, total = 0, turned = 0;
                    std::sscanf(text + sizeof(k_infection_marker) - 1, "%d %d %d", &remaining, &total, &turned);
                    Session& s = local_session();
                    std::lock_guard lock(s.mutex);
                    s.infection_until = remaining > 0 ? GetTickCount64() + static_cast<ULONGLONG>(remaining) : 0;
                    s.infection_total_ms = total;
                    if (turned != 0)
                        s.infection_turned_at = GetTickCount64();
                });
                return true;
            }
            // stamina: "@sta@ <per mille, -1: none> <1: exhausted>"
            if (text != nullptr && std::strcmp(head, k_stamina_marker) == 0)
            {
                mem::guarded([&] {
                    int permille = -1, exhausted = 0;
                    std::sscanf(text + sizeof(k_stamina_marker) - 1, "%d %d", &permille, &exhausted);
                    Session& s = local_session();
                    std::lock_guard lock(s.mutex);
                    s.stamina = permille < 0 ? -1.f : std::min(permille, 1000) / 1000.f;
                    s.stamina_exhausted = exhausted != 0;
                });
                return true;
            }
            return g_process_print.original()(state, edx, msg);
        }

        // ---- what the player sees (client side) -----------------------------------------------------------------
        //
        // Whether the hotbar shows, and the ammo counter, come from the client's own entities, so they work the same
        // on a listen server, as a guest and on a dedicated server.
        //
        //   client.dll VClientEntityList003 (CClientEntityList, vtable 0x10564B60) slot 3 GetClientEntity(index),
        //       slot 4 GetClientEntityFromHandle(handle); both return the C_BaseEntity.
        //   engine.dll VEngineClient013 slot 12 GetLocalPlayer() (local player slot + 1).
        //   engine.dll VEngineVGui001 slot 2 IsGameUIVisible() (GameUI's IsGameUIActive: the main or pause menu).
        //   client netprops (RecvProp definitions in client.dll: push offset / push "name"): m_iTeamNum +0xE4 (2: the
        //   survivors), m_lifeState +0x147 (byte, 0: alive), m_iObserverMode +0x1450 (0: not spectating),
        //   m_hActiveWeapon +0x1084, m_iAmmo +0xF24 (32 ints); weapon m_iPrimaryAmmoType +0x97C, m_iClip1 +0x984.

        constexpr std::uintptr_t k_c_team = 0xE4;
        constexpr std::uintptr_t k_c_life_state = 0x147;
        constexpr std::uintptr_t k_c_observer_mode = 0x1450;
        constexpr std::uintptr_t k_c_active_weapon = 0x1084;
        constexpr std::uintptr_t k_c_ammo = 0xF24;
        constexpr std::uintptr_t k_cw_ammo_type = 0x97C;
        constexpr std::uintptr_t k_cw_clip = 0x984;
        constexpr int k_team_survivors = 2;

        using GetClientEntityFn = void*(__thiscall*)(void* list, int index);
        using GetClientEntityFromHandleFn = void*(__thiscall*)(void* list, std::uint32_t handle);
        using GetLocalPlayerFn = int(__thiscall*)(void* engine);
        using GameUiVisibleFn = bool(__thiscall*)(void* vgui);

        void* g_client_entities = nullptr;
        GetClientEntityFn g_get_client_entity = nullptr;
        GetClientEntityFromHandleFn g_get_client_entity_from_handle = nullptr;
        GetLocalPlayerFn g_get_local_player = nullptr;
        void* g_engine_vgui = nullptr;
        GameUiVisibleFn g_game_ui_visible = nullptr;

        // Main thread: is the player a living survivor in a game with no menu open, and the ammo of what they hold.
        void refresh_view()
        {
            if (g_get_client_entity == nullptr || g_is_in_game == nullptr)
                return; // not found: the hotbar always shows, as before
            bool playing = false, in_game = false;
            int clip = -1;
            int reserve = -1;
            int pool[32] = {};
            mem::guarded([&] {
                in_game = g_is_in_game(g_engine_client);
                if (!in_game || g_game_ui_visible(g_engine_vgui))
                    return;
                void* player = g_get_client_entity(g_client_entities, g_get_local_player(g_engine_client));
                if (player == nullptr)
                    return;
                const auto base = reinterpret_cast<std::uintptr_t>(player);
                mem::safe_copy(pool, reinterpret_cast<const void*>(base + k_c_ammo), sizeof(pool));
                playing = mem::read_or<int>(base + k_c_team) == k_team_survivors &&
                          mem::read_or<std::uint8_t>(base + k_c_life_state, 1) == 0 &&
                          mem::read_or<int>(base + k_c_observer_mode, 1) == 0;
                void* weapon = g_get_client_entity_from_handle(
                    g_client_entities, mem::read_or<std::uint32_t>(base + k_c_active_weapon, k_invalid_handle));
                if (weapon == nullptr)
                    return;
                const auto w = reinterpret_cast<std::uintptr_t>(weapon);
                clip = mem::read_or<int>(w + k_cw_clip, -1);
                const int type = mem::read_or<int>(w + k_cw_ammo_type, -1);
                if (type >= 0 && type < 32)
                    reserve = mem::read_or<int>(base + k_c_ammo + 4u * static_cast<unsigned>(type), -1);
            });
            Session& s = local_session();
            std::lock_guard lock(s.mutex);
            s.playing = playing;
            s.ammo_clip = clip;
            s.ammo_reserve = reserve;
            std::memcpy(s.ammo_pool, pool, sizeof(pool));
            // out of a game (quit, or loading the next map): no infection, no stamina to show; a server that still has
            // them says so again
            if (!in_game && (s.infection_until != 0 || s.stamina >= 0.f))
            {
                INV_LOG("not in a game: the infection and stamina effects are off");
                s.infection_until = 0;
                s.infection_total_ms = 0;
                s.stamina = -1.f;
                s.stamina_exhausted = false;
            }
        }

        // ---- the backend --------------------------------------------------------------------------------------------

        class RealBackend final : public Backend
        {
        public:
            const char* name() const override
            {
                return "game";
            }
            bool connected() const override
            {
                return true;
            }
            void use(int slot) override
            {
                post({Command::Use, slot, 1});
            }
            void drop(int slot, int count) override
            {
                post({Command::Drop, slot, count});
            }
            void store_held() override
            {
                post({Command::Store, 0, 0});
            }
            void pickup_nearby() override
            {
                post({Command::Pickup, 0, 0});
            }
            void select_weapon(const std::string& classname) override
            {
                post({Command::Select, 0, 0, classname});
            }
            void drop_weapon(const std::string& classname) override
            {
                post({Command::DropWeapon, 0, 0, classname});
            }
            void drop_ammo(const std::string& kind, int count) override
            {
                post({Command::DropAmmo, 0, count, kind});
            }
            void infect_self() override
            {
                post({Command::Infect, 0, 0, {}});
            }
            void move(int from, int to) override
            {
                post({Command::Move, from, to});
            }

            bool typing() const override
            {
                return input_has_focus();
            }

            void pump() override
            {
                // beside the game's panel its item boxes are the mod's grid's (see hk_item_boxes); with the wheel only
                // they are hidden too: the wheel shows the items
                g_hide_item_boxes = !core::config().replace_native &&
                                    (core::config().inventory_view == core::view_wheel ||
                                     (core::config().show_hotbar && core::shows_slots()));
                refresh_view();
                for (;;)
                {
                    Command command{};
                    {
                        std::lock_guard lock(queue_mutex_);
                        if (queue_.empty())
                            break;
                        command = queue_.front();
                        queue_.pop_front();
                    }
                    const bool remote = in_remote_game();
                    if (!mem::guarded([&] { remote ? run_remote(command) : run(command); }))
                    {
                        INV_LOG("fault while running command %d", static_cast<int>(command.kind));
                        notify("The game faulted: see inventory_mod.log");
                    }
                }
                if (in_remote_game())
                {
                    // the server answers every request with the inventory; this keeps it fresh (used up, thrown...)
                    if (const ULONGLONG now = GetTickCount64(); now - g_last_sync > 1000)
                    {
                        g_last_sync = now;
                        send_to_server("inv_sync");
                    }
                    return;
                }
                leave_remote();
                if (host_player() != nullptr)
                    set_ammo_limits(); // this game is the server (a listen server, or playing alone)
                mem::guarded([] { refresh_native(); });
            }

        private:
            struct Command
            {
                enum Kind
                {
                    Use,
                    Drop,
                    Store,
                    Pickup,
                    Select,
                    DropWeapon,
                    DropAmmo, // text: the ammo kind, count: how many (0: all)
                    Infect, // development: infects the player
                    Move // slot: from, count: to (the HUD moved it locally already)
                } kind;
                int slot;
                int count;
                std::string text;
            };

            void post(Command command)
            {
                {
                    std::lock_guard lock(queue_mutex_);
                    queue_.push_back(command);
                }
                wake_main_thread();
            }

            static void run(const Command& command)
            {
                switch (command.kind)
                {
                case Command::Use:
                    do_use(command.slot);
                    break;
                case Command::Drop:
                    do_drop(command.slot, command.count);
                    break;
                case Command::Store:
                    do_store_held();
                    break;
                case Command::Pickup:
                    do_pickup_nearby();
                    break;
                case Command::Select:
                    do_select_weapon(command.text);
                    break;
                case Command::DropWeapon:
                    do_drop_weapon(command.text);
                    break;
                case Command::DropAmmo:
                    do_drop_ammo(command.text, command.count);
                    break;
                case Command::Infect:
                    if (void* player = local_player())
                        infect(player, "test");
                    break;
                case Command::Move:
                    break; // the local inventory was moved by the HUD itself
                }
            }

            // On a dedicated server: the same requests, as console commands for the server's copy of the mod.
            static void run_remote(const Command& command)
            {
                char text[160];
                switch (command.kind)
                {
                case Command::Use:
                    std::snprintf(text, sizeof(text), "inv_use %d", command.slot);
                    // the swap happens on the server, but the client's own "weapon_pickup" (the floating icon) is
                    // created here when the new weapon arrives: mute it a little longer, for the round trip
                    g_switch_mute_until = GetTickCount64() + 1000;
                    break;
                case Command::Drop:
                    std::snprintf(text, sizeof(text), "inv_drop %d %d", command.slot, command.count);
                    break;
                case Command::Store:
                    std::snprintf(text, sizeof(text), "inv_store");
                    break;
                case Command::Pickup:
                    std::snprintf(text, sizeof(text), "inv_pickup");
                    break;
                case Command::Select:
                    std::snprintf(text, sizeof(text), "inv_select %s", command.text.c_str());
                    break;
                case Command::DropWeapon:
                    std::snprintf(text, sizeof(text), "inv_dropweapon %s", command.text.c_str());
                    break;
                case Command::DropAmmo:
                    std::snprintf(text, sizeof(text), "inv_dropammo %s %d", command.text.c_str(), command.count);
                    break;
                case Command::Infect:
                    std::snprintf(text, sizeof(text), "inv_infect");
                    break;
                case Command::Move:
                    std::snprintf(text, sizeof(text), "inv_move %d %d", command.slot, command.count);
                    break;
                default:
                    return;
                }
                send_to_server(text);
            }

            std::mutex queue_mutex_;
            std::deque<Command> queue_;
        };

        RealBackend g_real;
    } // namespace

    bool init(bool dedicated)
    {
        g_dedicated = dedicated;
        if (!mem::module_info(L"server.dll", g.server))
        {
            INV_LOG("game: server.dll is not loaded");
            return false;
        }
        if (g.server.timestamp != k_server_timestamp)
        {
            INV_LOG("game: server.dll build %08X is not the one this mod was made for (%08X): game access is off",
                    g.server.timestamp, k_server_timestamp);
            return false;
        }

        const auto create_interface = reinterpret_cast<CreateInterfaceFn>(
            GetProcAddress(GetModuleHandleW(L"server.dll"), "CreateInterface"));
        if (create_interface == nullptr)
        {
            INV_LOG("game: server.dll has no CreateInterface");
            return false;
        }
        g.tools = create_interface("VSERVERTOOLS001", nullptr);
        if (g.tools == nullptr)
        {
            INV_LOG("game: no VSERVERTOOLS001");
            return false;
        }

        const std::uintptr_t first_entity = mem::vslot(g.tools, 7);
        const std::uintptr_t destroy_by_hammer = mem::vslot(g.tools, 16);
        g.get_key_value = reinterpret_cast<GetKeyValueFn>(mem::vslot(g.tools, 10));
        // (13, not 11: see the top of this file)
        g.set_key_value = reinterpret_cast<SetKeyValueFn>(mem::vslot(g.tools, k_vt_tools_set_key_value));
        g.create_entity = reinterpret_cast<CreateEntityFn>(mem::vslot(g.tools, 14));
        g.dispatch_spawn = reinterpret_cast<DispatchSpawnFn>(mem::vslot(g.tools, 15));
        const auto set_key = reinterpret_cast<std::uintptr_t>(g.set_key_value);
        if (set_key == 0 || mem::find_pattern(set_key, 17, "55 8B EC 8B 55 10 8B 4D 08 8B 01 8B 80 88 00 00 00") != set_key)
        {
            INV_LOG("game: IServerTools::SetKeyValue(const char*) does not look as expected");
            g.set_key_value = nullptr;
        }
        if (first_entity == 0 || destroy_by_hammer == 0 || g.get_key_value == nullptr || g.set_key_value == nullptr ||
            g.create_entity == nullptr || g.dispatch_spawn == nullptr)
        {
            INV_LOG("game: IServerTools slots unreadable");
            return false;
        }

        // gEntList, out of FirstEntity's body: push 0 / mov ecx, &gEntList / call
        const std::uintptr_t list_load = mem::find_pattern(first_entity, 24, "6A 00 B9 ?? ?? ?? ??");
        if (list_load == 0)
        {
            INV_LOG("game: FirstEntity does not look as expected");
            return false;
        }
        g.ent_list = mem::read_or<std::uint32_t>(list_load + 3);
        if (g.ent_list < g.server.base || g.ent_list >= g.server.base + g.server.size)
        {
            INV_LOG("game: gEntList %p is outside server.dll", reinterpret_cast<void*>(g.ent_list));
            return false;
        }

        // UTIL_Remove, out of DestroyEntityByHammerId's body: push eax / call UTIL_Remove / add esp, 4 / mov al, 1
        const std::uintptr_t remove_call = mem::find_pattern(destroy_by_hammer, 0x60, "50 E8 ?? ?? ?? ?? 83 C4 04 B0 01");
        if (remove_call == 0)
        {
            INV_LOG("game: DestroyEntityByHammerId does not look as expected");
            return false;
        }
        const auto relative = mem::read_or<std::int32_t>(remove_call + 2);
        const std::uintptr_t util_remove = remove_call + 6 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(relative));
        if (mem::read_or<std::uint8_t>(util_remove) != 0x55)
        {
            INV_LOG("game: UTIL_Remove at %p does not start with a frame setup", reinterpret_cast<void*>(util_remove));
            return false;
        }
        g.util_remove = reinterpret_cast<RemoveFn>(util_remove);

        // UTIL_DropToFloor, out of ent_create's body: push 0 / push 0x200400B / push edi / call / add esp, 0xC
        const std::uintptr_t floor_call =
            mem::find_pattern(g.server.base + k_rva_ent_create, 0x300, "6A 00 68 0B 40 00 02 57 E8 ?? ?? ?? ?? 83 C4 0C");
        if (floor_call != 0)
        {
            const auto rel = mem::read_or<std::int32_t>(floor_call + 9);
            const std::uintptr_t target = floor_call + 13 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
            if (mem::read_or<std::uint8_t>(target) == 0x55)
                g.drop_to_floor = reinterpret_cast<DropToFloorFn>(target);
        }
        if (g.drop_to_floor == nullptr)
            INV_LOG("game: UTIL_DropToFloor not found (ent_create %p, call site %p): dropped items are not put on the floor",
                    reinterpret_cast<void*>(g.server.base + k_rva_ent_create), reinterpret_cast<void*>(floor_call));

        // CTerrorPlayer::Weapon_Drop (player vtable slot 283, 0x10300770) starts by calling the base class's one with the
        // same arguments (push eax / push ecx / push edi / mov ecx, ebx / call), then fires "weapon_drop".  The base one
        // (0x10170AC0, also CBasePlayer's slot 283) is what a swap uses, so no event and no icon.
        {
            const std::uintptr_t player_drop = mem::read_or<std::uintptr_t>(g.server.base + k_rva_player_vtable + 4u * k_vt_weapon_drop);
            const std::uintptr_t call = player_drop != 0 ? mem::find_pattern(player_drop, 0x20, "50 51 57 8B CB E8 ?? ?? ?? ??") : 0;
            if (call != 0)
            {
                const auto rel = mem::read_or<std::int32_t>(call + 6);
                const std::uintptr_t target = call + 10 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
                if (mem::read_or<std::uint8_t>(target) == 0x55)
                    g.quiet_drop = reinterpret_cast<WeaponDropFn>(target);
            }
            if (g.quiet_drop == nullptr)
                INV_LOG("game: base Weapon_Drop not found: a swap shows the game's dropped-item icon");
        }

        if (HMODULE vgui = GetModuleHandleW(L"vgui2.dll"))
            if (const auto vgui_create = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(vgui, "CreateInterface")))
                g.vgui_input = vgui_create("VGUI_Input005", nullptr);

        g.player_vtable = g.server.base + k_rva_player_vtable;
        INV_LOG("game: server.dll %p, IServerTools %p, gEntList %p, UTIL_Remove %p, CTerrorPlayer vtable %p",
                reinterpret_cast<void*>(g.server.base), g.tools, reinterpret_cast<void*>(g.ent_list),
                reinterpret_cast<void*>(util_remove), reinterpret_cast<void*>(g.player_vtable));

        // muting the game events a switch creates (optional): see hk_create_event
        if (HMODULE engine = GetModuleHandleW(L"engine.dll"))
            if (const auto engine_create = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, "CreateInterface")))
                if (void* events = engine_create("GAMEEVENTSMANAGER002", nullptr))
                {
                    const std::uintptr_t create_event = mem::vslot(events, 6);
                    if (create_event != 0 && mem::find_pattern(create_event, 11, "55 8B EC 83 EC 08 53 56 57 8B F9") == create_event)
                    {
                        if (g_create_event.create("CreateEvent", reinterpret_cast<void*>(create_event), hk_create_event))
                            g_create_event.enable();
                    }
                    else
                        INV_LOG("game: IGameEventManager2::CreateEvent (%p) does not look as expected: switches show icons",
                                reinterpret_cast<void*>(create_event));
                    // the infection's triggers: FireEvent (see "the infection")
                    const std::uintptr_t fire_event = mem::vslot(events, 7);
                    if (fire_event != 0 && mem::find_pattern(fire_event, 12, "55 8B EC A1 ?? ?? ?? ?? 83 78 30 00") == fire_event &&
                        g_fire_event.create("FireEvent", reinterpret_cast<void*>(fire_event), hk_fire_event))
                        g_fire_event.enable();
                    else
                        INV_LOG("game: IGameEventManager2::FireEvent (%p) does not look as expected: no infection",
                                reinterpret_cast<void*>(fire_event));
                }

        // the pickup hook (optional): the function must start the way it did when it was read
        const std::uintptr_t pickup = mem::read_or<std::uintptr_t>(g.player_vtable + 4u * k_vt_use_pickup);
        if (pickup != 0 && mem::find_pattern(pickup, 11, "55 8B EC 83 EC 08 53 57 8B 7D 08") == pickup)
        {
            if (g_bump_weapon.create("UsePickup", reinterpret_cast<void*>(pickup), hk_bump_weapon))
                g_bump_weapon.enable();
        }
        else
            INV_LOG("game: the +use pickup function (%p) does not look as expected: no pickup hook",
                    reinterpret_cast<void*>(pickup));

        // map item spawners (optional): CWeaponSpawn::Use and Weapon_GetSlot, both checked against what was read
        {
            const std::uintptr_t use = mem::read_or<std::uintptr_t>(g.server.base + k_rva_spawn_vtable + 4u * k_vt_entity_use);
            const std::uintptr_t get_slot = g.server.base + k_rva_weapon_get_slot;
            const std::uintptr_t id_name = g.server.base + k_rva_weapon_id_name;
            if (use != 0 && mem::find_pattern(use, 13, "55 8B EC 83 EC 2C 53 57 8B 7D 08 8B D9") == use &&
                mem::find_pattern(get_slot, 14, "55 8B EC 8B 45 08 0F B6 84 08 00 18 00 00") == get_slot &&
                mem::find_pattern(id_name, 12, "55 8B EC 8B 45 08 8D 48 FF 83 F9 36") == id_name)
            {
                g_weapon_id_name = reinterpret_cast<WeaponIdNameFn>(id_name);
                if (g_get_slot.create("WeaponGetSlot", reinterpret_cast<void*>(get_slot), hk_get_slot) &&
                    g_spawn_use.create("WeaponSpawnUse", reinterpret_cast<void*>(use), hk_spawn_use))
                {
                    g_get_slot.enable();
                    g_spawn_use.enable();
                }
            }
            else
                INV_LOG("game: the item spawner functions do not look as expected: a spawner refuses when the slot is full");
        }

        // an ammo pile's own Use (optional, see hk_ammo_spawn_use): what a pile of dropped rounds gives
        {
            const std::uintptr_t ammo_use = g.server.base + k_rva_ammo_spawn_use;
            if (mem::read_or<std::uintptr_t>(g.server.base + k_rva_ammo_spawn_vtable + 4u * k_vt_entity_use) == ammo_use &&
                mem::find_pattern(ammo_use, 12, "55 8B EC 51 53 8B 5D 08 85 DB 0F 84") == ammo_use &&
                g_ammo_spawn_use.create("AmmoSpawnUse", reinterpret_cast<void*>(ammo_use), hk_ammo_spawn_use))
                g_ammo_spawn_use.enable();
            else
                INV_LOG("game: CWeaponAmmoSpawn::Use does not look as expected: dropped rounds cannot be picked up");
        }

        // a gun's own Use (optional, see hk_gun_use): slot 106 of CTerrorGun's vtable (0x106D5544, RTTI)
        {
            const std::uintptr_t gun_use = g.server.base + k_rva_gun_use;
            if (mem::read_or<std::uintptr_t>(g.server.base + 0x6D5544 + 4u * 106) == gun_use &&
                mem::find_pattern(gun_use, 15, "55 8B EC 83 EC 08 56 57 8B 7D 08 8B F1 85 FF") == gun_use &&
                g_gun_use.create("GunUse", reinterpret_cast<void*>(gun_use), hk_gun_use))
                g_gun_use.enable();
            else
                INV_LOG("game: CTerrorGun::Use does not look as expected: +use on a gun with special rounds swaps it");
        }

        // The game's own weapon-selection panel (client.dll, CHudWeaponSelection): hidden while the mod replaces it.
        // ShouldDraw is vtable slot 12 of the panel's CHudElement part (vtable RVA 0x5D5A84).
        mem::ModuleInfo client;
        if (mem::module_info(L"client.dll", client))
        {
            const std::uintptr_t should_draw = mem::read_or<std::uintptr_t>(client.base + k_rva_weapon_select_vtable + 4u * 12);
            if (should_draw == client.base + k_rva_weapon_select_should_draw &&
                mem::find_pattern(should_draw, 7, "56 57 6A FF 8B F9 E8") == should_draw)
            {
                if (g_weapon_select.create("WeaponSelectShouldDraw", reinterpret_cast<void*>(should_draw),
                                           hk_weapon_select_should_draw))
                {
                    g_weapon_select.enable();
                    INV_LOG("game: native weapon panel hook at %p", reinterpret_cast<void*>(should_draw));
                }
            }
            else
                INV_LOG("game: CHudWeaponSelection::ShouldDraw (%p) does not look as expected: the native panel stays",
                        reinterpret_cast<void*>(should_draw));

            // its Paint: the part that draws the item boxes (see hk_item_boxes)
            const std::uintptr_t paint = mem::read_or<std::uintptr_t>(client.base + k_rva_weapon_select_panel_vtable + 4u * 128);
            const std::uintptr_t start =
                paint == client.base + k_rva_weapon_select_paint
                    ? mem::find_pattern(paint, 0xD80, "6A 01 8B CB E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 89 45 AC DB 45")
                    : 0;
            const std::uintptr_t end =
                start != 0 ? mem::find_pattern(start, 0x500, "6A 04 8B CB E8 ?? ?? ?? ?? 83 BD 78 FF FF FF 00 7C") : 0;
            if (start != 0 && end != 0 &&
                g_item_boxes.create("WeaponSelectItemBoxes", reinterpret_cast<void*>(start + 9), hk_item_boxes))
            {
                g_item_boxes_end = end + 9;
                g_item_boxes_original = reinterpret_cast<std::uintptr_t>(g_item_boxes.original());
                g_item_boxes.enable();
                INV_LOG("game: native item boxes from %p to %p", reinterpret_cast<void*>(start + 9),
                        reinterpret_cast<void*>(g_item_boxes_end));

                // the way past the melee part when slot 1 is empty: push 1; mov ecx, ebx; mov [ebp-0x3C], edi;
                // call 0x102CEB20 (the weapon in slot 1); mov [ebp-0x54], eax; test eax, eax; je <target>
                const std::uintptr_t check =
                    mem::find_pattern(paint, 0xD80, "6A 01 8B CB 89 7D C4 E8 ?? ?? ?? ?? 89 45 AC 85 C0 0F 84");
                const std::uintptr_t target =
                    check != 0 ? check + 0x17 + static_cast<std::uintptr_t>(mem::read_or<std::int32_t>(check + 0x13)) : 0;
                if (target > check && target < start + 9 + 0x100 &&
                    mem::find_pattern(target, 7, "8B 45 9C 50 8B CB E8") == target &&
                    g_item_boxes_empty.create("WeaponSelectItemBoxesEmpty", reinterpret_cast<void*>(target), hk_item_boxes_empty))
                {
                    g_item_boxes_empty_original = reinterpret_cast<std::uintptr_t>(g_item_boxes_empty.original());
                    g_item_boxes_empty.enable();
                    INV_LOG("game: native item boxes (no melee) from %p", reinterpret_cast<void*>(target));
                }
                else
                    INV_LOG("game: the no-melee way into the item boxes (%p) does not look as expected: with slot 1 empty "
                            "they show", reinterpret_cast<void*>(target));
            }
            else
                INV_LOG("game: CHudWeaponSelection::Paint (%p) does not look as expected: its item boxes stay",
                        reinterpret_cast<void*>(paint));
        }

        // other players (a dedicated server's, a listen server's guests): their inventories, their requests and the
        // answers (see "other players")
        {
            HMODULE engine = GetModuleHandleW(L"engine.dll");
            const auto engine_create =
                engine != nullptr ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, "CreateInterface")) : nullptr;
            g_engine_server = engine_create != nullptr ? engine_create("VEngineServer022", nullptr) : nullptr;
            const std::uintptr_t network_id = g_engine_server != nullptr ? mem::vslot(g_engine_server, 17) : 0;
            const std::uintptr_t printf_fn = g_engine_server != nullptr ? mem::vslot(g_engine_server, 46) : 0;
            const std::uintptr_t command = g.server.base + k_rva_client_command;
            // slot 37 ServerCommand(const char*) (engine 0x10130D40, `ServerCommand with NULL string`): queues a console
            // command for the server; it has to end with '\n' or ';'
            const std::uintptr_t server_command = g_engine_server != nullptr ? mem::vslot(g_engine_server, 37) : 0;
            if (server_command != 0 && mem::find_pattern(server_command, 10, "55 8B EC 56 8B 75 08 85 F6 75") == server_command)
                g_server_command = reinterpret_cast<ServerCommandFn>(server_command);
            else
                INV_LOG("game: the engine's ServerCommand does not look as expected: the [ammo] limits are not set in the game");

            // the infection (see "the infection"): who a game event means, the clock, turning, the rescue closets
            const std::uintptr_t user_id = g_engine_server != nullptr ? mem::vslot(g_engine_server, 16) : 0;
            const std::uintptr_t script = g.server.base + k_rva_script_command;
            const std::uintptr_t rescue = g.server.base + k_rva_rescue_set_survivor;
            const std::uintptr_t suicide = mem::read_or<std::uintptr_t>(g.player_vtable + 4u * k_vt_commit_suicide);
            const auto server_create = reinterpret_cast<CreateInterfaceFn>(
                GetProcAddress(reinterpret_cast<HMODULE>(g.server.base), "CreateInterface"));
            void* game_dll = server_create != nullptr ? server_create("ServerGameDLL005", nullptr) : nullptr;
            const std::uintptr_t game_frame = game_dll != nullptr ? mem::vslot(game_dll, 4) : 0;
            if (user_id != 0 && mem::find_pattern(user_id, 12, "55 8B EC 83 3D ?? ?? ?? ?? 02 56 7C") == user_id &&
                mem::find_pattern(script, 12, "55 8B EC 56 8B 75 08 83 3E 01 7E 0B") == script &&
                mem::find_pattern(rescue, 14, "55 8B EC 51 8B 15 ?? ?? ?? ?? 53 56 8B F1") == rescue &&
                suicide == g.server.base + 0x174210 && mem::find_pattern(suicide, 6, "55 8B EC 83 EC 64") == suicide &&
                game_frame == g.server.base + 0xEF4B0 && mem::find_pattern(game_frame, 6, "55 8B EC 83 EC 14") == game_frame &&
                g_fire_event.installed())
            {
                g_user_id = reinterpret_cast<UserIdFn>(user_id);
                g_script_command = reinterpret_cast<ScriptCommandFn>(script);
                if (g_game_frame.create("GameFrame", reinterpret_cast<void*>(game_frame), hk_game_frame))
                    g_game_frame.enable();
                if (g_rescue_set_survivor.create("RescueSetSurvivor", reinterpret_cast<void*>(rescue), hk_rescue_set_survivor))
                    g_rescue_set_survivor.enable();
                INV_LOG("game: infection ready (GameFrame %p; on when inventory_mod.toml has [infection])",
                        reinterpret_cast<void*>(game_frame));
            }
            else
                INV_LOG("game: the infection's functions do not look as expected: no infection");

            // stamina (see "stamina"): the game's clock, and TakeDamage for an exhausted survivor's melee damage
            void* info_manager = server_create != nullptr ? server_create("PlayerInfoManager002", nullptr) : nullptr;
            const std::uintptr_t get_globals = info_manager != nullptr ? mem::vslot(info_manager, 1) : 0;
            const std::uintptr_t take_damage = g.server.base + k_rva_take_damage;
            if (get_globals != 0 && mem::find_pattern(get_globals, 6, "A1 ?? ?? ?? ?? C3") == get_globals)
                g_globals = mem::read_or<void*>(mem::read_or<std::uintptr_t>(get_globals + 1));
            const std::uintptr_t set_shove_penalty = g.server.base + k_rva_set_shove_penalty;
            const std::uintptr_t accessor_call = g.server.base + k_rva_change_accessor_call;
            // the server's own engine pointer (what 0x100ED4B0 calls slot 97 of) is the interface used here
            const std::uintptr_t change_accessor =
                g_engine_server != nullptr ? mem::vslot(g_engine_server, k_vt_change_accessor) : 0;
            const bool same_engine = mem::read_or<void*>(g.server.base + k_rva_server_engine) == g_engine_server;
            if (g_globals != nullptr && same_engine && change_accessor != 0 &&
                mem::find_pattern(accessor_call, 18, "51 8B 0D ?? ?? ?? ?? 8B 11 8B 82 84 01 00 00 FF D0 C3") == accessor_call &&
                mem::find_pattern(set_shove_penalty, 16, "55 8B EC 56 57 8B 7D 08 8B F1 39 BE F0 2C 00 00") == set_shove_penalty &&
                g_game_frame.installed() &&
                mem::find_pattern(take_damage, 11, "55 8B EC 83 EC 6C 57 8B F9 8B 0D") == take_damage &&
                g_take_damage.create("TakeDamage", reinterpret_cast<void*>(take_damage), hk_take_damage) && g_take_damage.enable())
            {
                g_change_accessor = reinterpret_cast<ChangeAccessorFn>(change_accessor);
                g_set_shove_penalty = reinterpret_cast<SetShovePenaltyFn>(set_shove_penalty);
                INV_LOG("game: stamina ready (gpGlobals %p, game time %.1f; on when inventory_mod.toml has [stamina])", g_globals,
                        game_time());
            }
            else
            {
                g_globals = nullptr;
                INV_LOG("game: the stamina's functions do not look as expected: no stamina");
            }
            if (network_id != 0 && mem::find_pattern(network_id, 10, "55 8B EC 83 3D ?? ?? ?? ?? 02") == network_id &&
                printf_fn != 0 && mem::find_pattern(printf_fn, 8, "55 8B EC 8B 45 08 50 E8") == printf_fn &&
                mem::find_pattern(command, 10, "55 8B EC 81 EC 18 01 00 00 A1") == command)
            {
                g_network_id = reinterpret_cast<NetworkIdFn>(network_id);
                g_client_printf = reinterpret_cast<ClientPrintfFn>(printf_fn);
                if (g_client_command.create("ClientCommand", reinterpret_cast<void*>(command), hk_client_command))
                    g_client_command.enable();
                INV_LOG("game: other players with the mod get inventories of their own (client commands inv_*)");
            }
            else
                INV_LOG("game: the engine's server interface does not look as expected: only the host has an inventory");
        }
        if (!g_dedicated)
        {
            // a game client: playing on someone else's server (dedicated or listen) through the commands above
            mem::ModuleInfo engine;
            HMODULE engine_module = GetModuleHandleW(L"engine.dll");
            const auto engine_create = engine_module != nullptr
                                           ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine_module, "CreateInterface"))
                                           : nullptr;
            if (mem::module_info(L"engine.dll", engine) && engine.timestamp == k_engine_timestamp && engine_create != nullptr)
            {
                g_engine_client = engine_create("VEngineClient013", nullptr);
                const std::uintptr_t in_game = g_engine_client != nullptr ? mem::vslot(g_engine_client, 26) : 0;
                const std::uintptr_t cmd = g_engine_client != nullptr ? mem::vslot(g_engine_client, 107) : 0;
                const std::uintptr_t print = engine.base + k_rva_process_print;
                if (in_game != 0 && mem::find_pattern(in_game, 15, "E8 ?? ?? ?? ?? 33 C9 83 78 68 06 0F 94 C0 C3") == in_game &&
                    cmd != 0 && mem::find_pattern(cmd, 11, "55 8B EC A1 ?? ?? ?? ?? 8B 40 30") == cmd &&
                    mem::find_pattern(print, 17, "55 8B EC 8B 45 08 8B 48 54 51 68 ?? ?? ?? ?? FF 15") == print)
                {
                    g_is_in_game = reinterpret_cast<IsInGameFn>(in_game);
                    g_client_cmd = reinterpret_cast<ClientCmdFn>(cmd);
                    if (g_process_print.create("ProcessPrint", reinterpret_cast<void*>(print), hk_process_print))
                        g_process_print.enable();

                    // what the player sees (see "what the player sees")
                    const std::uintptr_t local_player_fn = mem::vslot(g_engine_client, 12);
                    g_engine_vgui = engine_create("VEngineVGui001", nullptr);
                    const std::uintptr_t ui_visible = g_engine_vgui != nullptr ? mem::vslot(g_engine_vgui, 2) : 0;
                    HMODULE client = GetModuleHandleW(L"client.dll");
                    const auto client_create =
                        client != nullptr ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(client, "CreateInterface")) : nullptr;
                    g_client_entities = client_create != nullptr ? client_create("VClientEntityList003", nullptr) : nullptr;
                    const std::uintptr_t get_entity = g_client_entities != nullptr ? mem::vslot(g_client_entities, 3) : 0;
                    const std::uintptr_t from_handle = g_client_entities != nullptr ? mem::vslot(g_client_entities, 4) : 0;
                    if (local_player_fn != 0 &&
                        mem::find_pattern(local_player_fn, 18, "6A FF E8 ?? ?? ?? ?? 8B 80 D8 00 00 00 83 C4 04 40 C3") == local_player_fn &&
                        ui_visible != 0 && mem::find_pattern(ui_visible, 8, "83 B9 84 00 00 00 00 74") == ui_visible &&
                        get_entity != 0 && mem::find_pattern(get_entity, 15, "55 8B EC 8B 45 08 85 C0 78 ?? 3D 00 10 00 00") == get_entity &&
                        from_handle != 0 && mem::find_pattern(from_handle, 16, "55 8B EC 8B 55 08 51 8B C4 89 10 8B 01 8B 50 08") == from_handle)
                    {
                        g_get_local_player = reinterpret_cast<GetLocalPlayerFn>(local_player_fn);
                        g_game_ui_visible = reinterpret_cast<GameUiVisibleFn>(ui_visible);
                        g_get_client_entity_from_handle = reinterpret_cast<GetClientEntityFromHandleFn>(from_handle);
                        g_get_client_entity = reinterpret_cast<GetClientEntityFn>(get_entity);
                    }
                    else
                        INV_LOG("game: the client's entity interfaces do not look as expected: the hotbar always shows, no ammo");
                }
                else
                {
                    INV_LOG("game: the engine's client interface (%p: IsInGame %p, ClientCmd %p, ProcessPrint %p) does not look as "
                            "expected: no dedicated server play",
                            g_engine_client, reinterpret_cast<void*>(in_game), reinterpret_cast<void*>(cmd),
                            reinterpret_cast<void*>(print));
                    g_engine_client = nullptr;
                }
            }
            else
                INV_LOG("game: engine.dll is not the build this mod was made for: no dedicated server play");
        }

        set_backend(&g_real);
        return true;
    }

} // namespace game
