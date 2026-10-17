// Other players: a dedicated server's, a listen server's guests, each with an inventory of their own.

#include "game/players.hpp"

#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/infection.hpp"
#include "game/native_slots.hpp"
#include "game/rounds.hpp"
#include "game/unload.hpp"
#include "game/weapons.hpp"
#include "inv/rules.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace game::detail
{

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

    ClientPrintfFn g_client_printf = nullptr;
    core::Hook<ClientCommandFn> g_client_command;
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

    std::map<std::string, std::unique_ptr<ServerPlayer>> g_players;
    ServerPlayer* g_active_server_player = nullptr;
    FILETIME g_new_game{}; // (see "a death, a new game")

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
            WIN32_FILE_ATTRIBUTE_DATA file{};
            const bool earlier = (g_new_game.dwLowDateTime != 0 || g_new_game.dwHighDateTime != 0) &&
                                 GetFileAttributesExW(entry->file.c_str(), GetFileExInfoStandard, &file) &&
                                 CompareFileTime(&file.ftLastWriteTime, &g_new_game) < 0;
            if (earlier)
                INV_LOG("server: the inventory file of %s is from an earlier game: they start with nothing", id);
            else if (FILE* f = _wfopen(entry->file.c_str(), L"rb"))
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

    Activation::Activation(void* player, bool require_mod) : player_(player), sp_(server_player(player))
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

    Activation::~Activation()
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

    // Served through an Activation (not the host, and the server side is up).
    bool served_remotely(void* player)
    {
        return player != nullptr && g_network_id != nullptr && g_active_player == nullptr && player != host_player();
    }

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
            else if (command == "inv_unload")
                do_unload();
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

    void install_server_command()
    {
        // slot 37 ServerCommand(const char*) (engine 0x10130D40, `ServerCommand with NULL string`): queues a console
        // command for the server; it has to end with '\n' or ';'
        const std::uintptr_t server_command = g_engine_server != nullptr ? mem::vslot(g_engine_server, 37) : 0;
        if (server_command != 0 && mem::find_pattern(server_command, 10, "55 8B EC 56 8B 75 08 85 F6 75") == server_command)
            g_server_command = reinterpret_cast<ServerCommandFn>(server_command);
        else
            INV_LOG("game: the engine's ServerCommand does not look as expected: the [ammo] limits are not set in the game");
    }

    void install_client_commands()
    {
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

} // namespace game::detail
