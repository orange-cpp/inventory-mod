// Playing on a server this game does not run: the requests go to it, the inventory comes back from it.

#include "game/client_remote.hpp"

#include "core/config.hpp"
#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/client_view.hpp"
#include "game/entities.hpp"
#include "game/infection.hpp"
#include "game/melee.hpp"
#include "game/mutators.hpp"
#include "game/native_slots.hpp"
#include "game/ping.hpp"
#include "game/players.hpp"
#include "game/stamina.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace game::detail
{

    using namespace sdk;

    // ---- playing on a dedicated server (client side) ----------------------------------------------------------
    //
    // When the game client is on a server it does not run (no local player in its own server.dll, but the engine
    // is in a game), the HUD's requests go to the server as console commands, and the inventory shown is the one the
    // server sends back (send_state, in players.cpp).
    //
    //   engine.dll VEngineClient013 slot 26 IsInGame() (0x1005F1A0: signon state == 6), slot 27 IsConnected()
    //       (0x1005F1B0: signon state >= 2), slot 107 ClientCmd_Unrestricted(text) (Cbuf_AddText; a command the client
    //       does not know is forwarded to the server).
    //   engine.dll 0x10052100 CBaseClientState::ProcessPrint(SVC_Print*), thiscall, ret 4: ConMsg("%s", msg + 0x54).
    //       The state line is taken out there, before it reaches the console.

    void* g_engine_client = nullptr;
    IsInGameFn g_is_in_game = nullptr;
    IsInGameFn g_is_connected = nullptr;
    ClientCmdFn g_client_cmd = nullptr;
    core::Hook<ProcessPrintFn> g_process_print;


    bool in_remote_game()
    {
        if (g_dedicated || g_engine_client == nullptr || g_is_in_game == nullptr || local_player() != nullptr)
            return false;
        bool in_game = false;
        mem::guarded([&] { in_game = g_is_in_game(g_engine_client); });
        return in_game;
    }

    bool knows_connection() { return g_engine_client != nullptr && g_is_connected != nullptr && g_client_cmd != nullptr; }

    bool connected_to_server()
    {
        if (!knows_connection())
            return false;
        bool connected = false;
        mem::guarded([&] { connected = g_is_connected(g_engine_client); });
        return connected;
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
            const int read = std::sscanf(parts[1].substr(start, comma - start).c_str(), "%d %63s %d %d %d %d %63s %d %d %d",
                                         &index, name, &count, &held, &state.clip, &state.ammo, melee, &state.upgrades,
                                         &state.special, &state.wear);
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
                    {
                        // a gas can's fuel ([chainsaw]): "<spent> <full>" where a weapon has its rounds; the server's
                        // rules say what a full one holds, for the HUD
                        const int spent = read >= 6 ? state.clip : 0, full = read >= 6 ? state.ammo : 0;
                        if (full > 0 && def->fuel != full)
                            inv::set_item_fuel(def->classname, full);
                        inventory.add_to_slot(index, *def, count, std::max(0, spent));
                    }
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
            int wear = -1; // "fireaxe:1234": its wear
            if (const std::size_t colon = melee.find(':'); colon != std::string::npos)
            {
                wear = std::atoi(melee.c_str() + colon + 1);
                melee.erase(colon);
            }
            if (!name.empty() && name != "-")
            {
                native[slot] = describe_weapon(name.c_str());
                native[slot].active = active;
                native[slot].melee = melee;
                native[slot].wear = wear;
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
        // [melee]: "@mel@ <hold ms> <shove hold ms> <features>", "@mhi@ <heavy>"
        if (text != nullptr && std::strcmp(head, k_melee_marker) == 0)
        {
            mem::guarded([&] { apply_melee_line(text); });
            return true;
        }
        if (text != nullptr && std::strcmp(head, k_ping_marker) == 0)
        {
            mem::guarded([&] { apply_ping_line(text); });
            return true;
        }
        if (text != nullptr && std::strcmp(head, k_mutator_marker) == 0)
        {
            mem::guarded([&] { apply_mutator_line(text); });
            return true;
        }
        if (text != nullptr && std::strcmp(head, k_melee_hit_marker) == 0)
        {
            mem::guarded([&] { apply_melee_hit_line(text); });
            return true;
        }
        return g_process_print.original()(state, edx, msg);
    }

    void install_client_remote()
    {
        // a game client: playing on someone else's server (dedicated or listen) through the inv_* commands
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

                const std::uintptr_t connected = mem::vslot(g_engine_client, k_vt_is_connected);
                if (connected != 0 &&
                    mem::find_pattern(connected, 15, "E8 ?? ?? ?? ?? 33 C9 83 78 68 02 0F 9D C0 C3") == connected)
                    g_is_connected = reinterpret_cast<IsInGameFn>(connected);
                else
                    INV_LOG("game: the engine's IsConnected (slot 27, %p) does not look as expected: the main menu's "
                            "Options > Inventory Mod waits for the menu to be built again",
                            reinterpret_cast<void*>(connected));

                install_client_view(engine_create); // what the player sees (see "what the player sees")
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

} // namespace game::detail
