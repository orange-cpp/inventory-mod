// What the player sees (client side): whether the hotbar shows, the ammo counter, an unloaded gun held empty.

#include "game/client_view.hpp"

#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/client_remote.hpp"
#include "game/entities.hpp"
#include "game/mutators.hpp"
#include "game/ping.hpp"

#include <algorithm>
#include <cstring>
#include <intrin.h>
#include <vector>

namespace game::detail
{

    using namespace sdk;

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
        if (!in_game)
            mutators_client_left(); // (the next map's own fog is read first; not under the lock: it may call the game)
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
        if (!in_game)
            ping_clear();
        if (!in_game)
            s.melee_hold = 0.f; // the next server says again whether it has [melee]
        if (!in_game && !s.mutators.empty())
            s.mutators.clear(); // (the next server tells its mutators again)
    }

    // The client's half of reload_held_back.  The game predicts its own weapon on the client: client.dll runs the
    // same auto-reload of an empty gun.  When only the server said no, the gun jittered and no animation played; and
    // a hook on the wrong client function (slot 266, 0x10018160, which is CheckReload: it finishes a running reload
    // through FinishReload, +0x42C) let the client start its reload and finish it at the first fire: the client's
    // magazine was full again in that frame and it predicted a real shot while the server's was empty (seen: the
    // client's fire body 0x1030C4C0 ran with m_iClip1 19, the server reloaded: a shot with no bullet).  The client's
    // Reload is the one its ReloadOrSwitchWeapons (+0x400, 0x100178B0: m_iClip1 0 and both attack times passed)
    // calls: +0x434, slot 269 of the primary vtable (its complete object locator's offset 0): C_TerrorGun
    // (0x10609034), C_SniperRifle, C_Rifle_Desert -> 0x1030C150 (56 57 8B F1 E8); C_PumpShotgun (0x105FF6FC),
    // C_Shotgun_Chrome, C_AutoShotgun, C_Shotgun_SPAS -> 0x102F1CF0 (55 8B EC 51 56 57 8B F1 E8); both
    // bool __thiscall Reload().  The rule is the server's: the gun in the hands when the unload key was pressed,
    // until the local player's m_nButtons (client datamap: "m_nButtons", FIELD_INTEGER, +0x13A4, entry at
    // 0x106C1664) has IN_ATTACK or IN_RELOAD.
    core::Hook<ReloadFn> g_c_gun_reload, g_c_shotgun_reload;
    struct ClientUnloaded
    {
        void* weapon;
        std::uint32_t handle;
    };
    std::vector<ClientUnloaded> g_c_unloaded; // main thread (the client's prediction runs there too)

    void* client_local_player()
    {
        if (g_get_client_entity == nullptr || g_is_in_game == nullptr)
            return nullptr;
        void* player = nullptr;
        mem::guarded([&] {
            if (g_is_in_game(g_engine_client))
                player = g_get_client_entity(g_client_entities, g_get_local_player(g_engine_client));
        });
        return player;
    }

    void* client_active_weapon(void* player)
    {
        if (player == nullptr || g_get_client_entity_from_handle == nullptr)
            return nullptr;
        const std::uint32_t handle = mem::read_or<std::uint32_t>(reinterpret_cast<std::uintptr_t>(player) + k_c_active_weapon, k_invalid_handle);
        void* weapon = nullptr;
        mem::guarded([&] { weapon = g_get_client_entity_from_handle(g_client_entities, handle); });
        return weapon;
    }

    // The unload key was pressed: the gun in the hands (as the client sees it) does not reload by itself.
    void client_mark_unloaded()
    {
        void* player = client_local_player();
        if (player == nullptr || !g_c_gun_reload.installed())
            return;
        const std::uint32_t handle = mem::read_or<std::uint32_t>(reinterpret_cast<std::uintptr_t>(player) + k_c_active_weapon, k_invalid_handle);
        void* weapon = nullptr;
        mem::guarded([&] { weapon = g_get_client_entity_from_handle(g_client_entities, handle); });
        if (weapon == nullptr)
            return;
        std::erase_if(g_c_unloaded, [](const ClientUnloaded& u) {
            void* now = nullptr;
            mem::guarded([&] { now = g_get_client_entity_from_handle(g_client_entities, u.handle); });
            return now != u.weapon;
        });
        if (std::none_of(g_c_unloaded.begin(), g_c_unloaded.end(), [&](const ClientUnloaded& u) { return u.weapon == weapon; }))
            g_c_unloaded.push_back({weapon, handle});

    }

    std::uintptr_t g_client_base = 0;

    bool client_reload_held_back(void* weapon, void* caller)
    {
        if (g_c_unloaded.empty())
            return false;
        const auto it = std::find_if(g_c_unloaded.begin(), g_c_unloaded.end(), [&](const ClientUnloaded& u) { return u.weapon == weapon; });
        if (it == g_c_unloaded.end())
            return false;
        void* player = client_local_player();
        const int buttons = player != nullptr ? mem::read_or<int>(reinterpret_cast<std::uintptr_t>(player) + k_c_buttons, 0) : 0;
        if (player == nullptr || (buttons & (k_in_attack | k_in_reload)) != 0)
        {
            INV_LOG("unloaded gun %p (client): reload let through (buttons %X, called from client.dll+0x%X)", weapon,
                    static_cast<unsigned>(buttons), static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(caller) - g_client_base));
            g_c_unloaded.erase(it);
            return false;
        }
        return true;
    }

    // The client's half of hk_fire_on_empty (see k_off_fire_on_empty), for the local player's gun.
    core::Hook<FireOnEmptyFn> g_c_fire_on_empty;

    void __fastcall hk_c_fire_on_empty(void* weapon, void* edx)
    {
        const auto w = reinterpret_cast<std::uintptr_t>(weapon);
        if (mem::read_or<int>(w + k_cw_clip, -1) == 0)
        {
            const auto pl = reinterpret_cast<std::uintptr_t>(client_local_player());
            const int type = mem::read_or<int>(w + k_cw_ammo_type, -1);
            if (pl != 0 && type >= 0 && type < 32 && mem::read_or<int>(pl + k_c_ammo + 4u * static_cast<unsigned>(type), 0) > 0)
                mem::guarded([&] { *reinterpret_cast<std::uint8_t*>(w + k_cw_fire_on_empty) = 1; });
        }
        g_c_fire_on_empty.original()(weapon, edx);
    }

    bool __fastcall hk_c_gun_reload(void* weapon, void* edx)
    {
        return client_reload_held_back(weapon, _ReturnAddress()) ? false : g_c_gun_reload.original()(weapon, edx);
    }

    bool __fastcall hk_c_shotgun_reload(void* weapon, void* edx)
    {
        return client_reload_held_back(weapon, _ReturnAddress()) ? false : g_c_shotgun_reload.original()(weapon, edx);
    }

    void install_client_reload_hooks(const mem::ModuleInfo& client)
    {
        g_client_base = client.base;
        const std::uintptr_t gun = client.base + k_rva_c_gun_reload;
        const std::uintptr_t shotgun = client.base + k_rva_c_shotgun_reload;
        if (mem::read_or<std::uintptr_t>(client.base + k_rva_c_gun_vtable + 4u * k_c_vt_reload) == gun &&
            mem::find_pattern(gun, 5, "56 57 8B F1 E8") == gun &&
            mem::read_or<std::uintptr_t>(client.base + k_rva_c_pump_vtable + 4u * k_c_vt_reload) == shotgun &&
            mem::find_pattern(shotgun, 9, "55 8B EC 51 56 57 8B F1 E8") == shotgun &&
            g_c_gun_reload.create("ClientGunReload", reinterpret_cast<void*>(gun), hk_c_gun_reload) &&
            g_c_shotgun_reload.create("ClientShotgunReload", reinterpret_cast<void*>(shotgun), hk_c_shotgun_reload))
        {
            g_c_gun_reload.enable();
            g_c_shotgun_reload.enable();
            const std::uintptr_t fire_on_empty = client.base + k_rva_c_fire_on_empty;
            if (mem::find_pattern(fire_on_empty, 10, "56 8B F1 80 BE BE 09 00 00 00") == fire_on_empty &&
                g_c_fire_on_empty.create("ClientFireOnEmpty", reinterpret_cast<void*>(fire_on_empty), hk_c_fire_on_empty))
                g_c_fire_on_empty.enable();
            else
                INV_LOG("game: the client's HandleFireOnEmpty does not look as expected: fire on an unloaded gun dry-fires first");
        }
        else
        {
            g_c_gun_reload.remove();
            INV_LOG("game: the client's Reload does not look as expected: a gun unloaded in the hands jitters until it is reloaded");
        }
    }

    void install_client_view(CreateInterfaceFn engine_create)
    {
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

} // namespace game::detail
