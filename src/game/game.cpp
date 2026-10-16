// The real backend: puts items into the player's hands, onto the ground and back into the inventory.
//
// Everything here was read out of server.dll build 2.2.4.3 (PE timestamp 0x6A440E53) with tools/re/pe_tools.py; init()
// refuses to run on any other build.  What each number is:
//
//   IServerTools ("VSERVERTOOLS001", server.dll's CreateInterface), vtable slots
//       7 FirstEntity     (its body loads &gEntList: `6A 00 B9 <gEntList> E8`)
//      10 GetKeyValue(ent, field, buf, len)     11 SetKeyValue(ent, field, const char*)
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

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
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

        // The game code works on "the local player".  That is the host on a listen server (entity 1).  While it serves
        // another player (every player in a dedicated server, a guest on a listen server; see "Other players" below)
        // it is that player.  Main thread only.
        bool g_dedicated = false;
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
        // A stored weapon keeps its rounds.  A carried gun's reserve is the player's m_iAmmo of its ammo type (a gun
        // lying in the world keeps it in m_iExtraPrimaryAmmo); a melee weapon is told apart by its script name.

        inv::WeaponState weapon_state(void* player, void* weapon, const inv::ItemDef& def, bool carried)
        {
            const bool is_melee = std::strcmp(def.classname, "weapon_melee") == 0;
            inv::WeaponState state;
            const auto w = reinterpret_cast<std::uintptr_t>(weapon);
            mem::guarded([&] {
                state.clip = mem::read_or<int>(w + k_off_clip, -1);
                if (carried)
                {
                    const int type = mem::read_or<int>(w + k_off_ammo_type, -1);
                    if (type >= 0 && type < 32)
                        state.ammo = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(player) + k_off_player_ammo +
                                                           4u * static_cast<unsigned>(type),
                                                       -1);
                }
                else if (const int extra = mem::read_or<int>(w + k_off_extra_ammo, 0); extra > 0)
                    state.ammo = extra; // 0 on a fresh gun from the map: the game's own amount on pick-up (-1)
                char name[64] = {};
                if (const char* melee = is_melee ? mem::read_or<const char*>(w + k_off_melee_name) : nullptr;
                    melee != nullptr && mem::safe_copy(name, melee, sizeof(name) - 1) && name[0] != '\0')
                    state.melee = name;
            });
            return state;
        }

        // The rounds of a stored weapon onto the one the player was just given.
        void apply_weapon_state(void* player, void* weapon, const inv::WeaponState& state)
        {
            const auto w = reinterpret_cast<std::uintptr_t>(weapon);
            mem::guarded([&] {
                if (state.clip >= 0)
                    *reinterpret_cast<int*>(w + k_off_clip) = state.clip;
                const int type = mem::read_or<int>(w + k_off_ammo_type, -1);
                if (state.ammo >= 0 && type >= 0 && type < 32)
                    *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + k_off_player_ammo +
                                            4u * static_cast<unsigned>(type)) = state.ammo;
            });
        }

        // The reserve goes with a stored gun: the player keeps none of that ammo type.
        void clear_reserve(void* player, void* weapon)
        {
            mem::guarded([&] {
                const int type = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_ammo_type, -1);
                if (type >= 0 && type < 32)
                    *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + k_off_player_ammo +
                                            4u * static_cast<unsigned>(type)) = 0;
            });
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
            clear_reserve(player, weapon);
            if (!mem::guarded([&] { drop(player, weapon, nullptr, nullptr); }) || player_has_slot(player, def.game_slot))
            {
                apply_weapon_state(player, weapon, state); // the reserve back
                Session& s = session();
                std::lock_guard lock(s.mutex);
                s.inventory.remove_from_slot(box, 1);
                return -1;
            }
            mem::guarded([&] { g.util_remove(weapon); });
            INV_LOG("stored %s (%s) into box %d: clip %d, reserve %d", def.classname, state.melee.c_str(), box, state.clip,
                    state.ammo);
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
            g_giving = true;
            const bool gave = give != nullptr && mem::guarded([&] { give(player, name.c_str(), 0, 0, 0); });
            g_giving = false;
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
            {
                const inv::WeaponState state = best_def->weapon() ? weapon_state(player, best, *best_def, false) : inv::WeaponState{};
                Session& s = session();
                std::lock_guard lock(s.mutex);
                if (best_def->weapon())
                {
                    weapon_box = s.inventory.add_weapon(*best_def, state); // a gun keeps what is left in it
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
                });
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
            {
                // a gun keeps what is in it, a melee weapon which one it is
                const inv::WeaponState state = def.weapon() ? weapon_state(nullptr, weapon, def, false) : inv::WeaponState{};
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

        bool bump_weapon(void* player, void* edx, void* weapon)
        {
            const inv::ItemDef* def = nullptr;
            const bool local = !g_giving && player != nullptr && player == local_player();
            if (local)
                def = item_of(weapon);
            // a gun or melee weapon: into the hands when that game slot is free (the game's own pick-up); when the
            // hands have one there already, into a free weapon box with its rounds; with no free box (or too heavy for
            // one) the game's own swap, as long as the swap is not too heavy
            if (def == nullptr || def->weapon())
            {
                if (local && def != nullptr && player_has_slot(player, def->game_slot))
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
                        return true;
                    }
                }
                if (local && weapon_too_heavy(weapon))
                    return false;
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

        void spawn_use(void* spawner, void* edx, void* activator, void* caller, int type, float value)
        {
            int weapon_id = 0;
            const inv::ItemDef* def = nullptr;
            if (activator != nullptr && activator == local_player() && g_weapon_id_name != nullptr)
            {
                const auto id_of = reinterpret_cast<WeaponIdFn>(mem::vslot(spawner, k_vt_spawn_weapon_id));
                mem::guarded([&] {
                    weapon_id = id_of != nullptr ? id_of(spawner) : 0;
                    if (const char* name = weapon_id > 0 ? g_weapon_id_name(weapon_id) : nullptr)
                        def = inv::find_item(name);
                });
            }
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

        void* g_engine_server = nullptr;
        NetworkIdFn g_network_id = nullptr;
        ClientPrintfFn g_client_printf = nullptr;
        core::Hook<ClientCommandFn> g_client_command;

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
                                (stack.weapon.melee.empty() ? std::string("-") : stack.weapon.melee);
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

        void do_drop_weapon(const std::string& classname);

        // The requests a client's HUD sends (forwarded console commands).  Anything else goes to the game.
        void __cdecl hk_client_command(void* player, const void* args)
        {
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
                    do_drop_weapon(text);
                else if (command == "inv_spawn")
                {
                    if (const inv::ItemDef* def = inv::find_item(text))
                        do_spawn(*def);
                }
                // inv_sync: nothing to do, the state goes out when `active` ends
            });
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
            if (void* player = local_player())
            {
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
                const int read = std::sscanf(parts[1].substr(start, comma - start).c_str(), "%d %63s %d %d %d %d %63s",
                                             &index, name, &count, &held, &state.clip, &state.ammo, melee);
                if (read >= 4)
                    if (const inv::ItemDef* def = inv::find_item(name))
                    {
                        if (def->weapon())
                        {
                            if (read == 7 && std::strcmp(melee, "-") != 0)
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
            bool playing = false;
            int clip = -1;
            int reserve = -1;
            mem::guarded([&] {
                if (!g_is_in_game(g_engine_client) || g_game_ui_visible(g_engine_vgui))
                    return;
                void* player = g_get_client_entity(g_client_entities, g_get_local_player(g_engine_client));
                if (player == nullptr)
                    return;
                const auto base = reinterpret_cast<std::uintptr_t>(player);
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
                // beside the game's panel its item boxes are the mod's grid's (see hk_item_boxes)
                g_hide_item_boxes = !core::config().replace_native && core::config().show_hotbar && core::shows_slots();
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
        g.set_key_value = reinterpret_cast<SetKeyValueFn>(mem::vslot(g.tools, 11));
        g.create_entity = reinterpret_cast<CreateEntityFn>(mem::vslot(g.tools, 14));
        g.dispatch_spawn = reinterpret_cast<DispatchSpawnFn>(mem::vslot(g.tools, 15));
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
