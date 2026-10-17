// The entity list, the players and the functions and interfaces game::init finds for them (Env).

#include "game/entities.hpp"

#include "core/log.hpp"
#include "game/backend.hpp"

#include <cstring>

namespace game::detail
{

    Env g;
    NetworkIdFn g_network_id = nullptr;

    // ---- entities -----------------------------------------------------------------------------------------------

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
    // another player (every player in a dedicated server, a guest on a listen server; see players.cpp)
    // it is that player.  Main thread only.
    bool g_dedicated = false;
    void* g_engine_server = nullptr; // engine.dll VEngineServer022 (see players.cpp)
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

    void* edict_of(void* player)
    {
        return mem::read_or<void*>(reinterpret_cast<std::uintptr_t>(player) + k_off_edict);
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

    UserIdFn g_user_id = nullptr;

    void* g_globals = nullptr; // CGlobalVars

    float game_time()
    {
        return g_globals != nullptr ? mem::read_or<float>(reinterpret_cast<std::uintptr_t>(g_globals) + k_off_curtime) : 0.f;
    }

    // A networked field written here goes out to the players only when the entity says it changed, as the
    // game's own setters do (CBaseEdict::StateChanged: m_fStateFlags |= FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED,
    // and the change accessor's serial number 0; VEngineServer022 slot 97 GetChangeAccessor(edict), what
    // SetShovePenalty, 0x102CB750, calls through 0x100ED4B0).
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

    // ---- finding them ---------------------------------------------------------------------------------------------

    bool init_env()
    {
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
        return true;
    }

} // namespace game::detail
