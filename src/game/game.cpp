// The real backend: puts items into the player's hands, onto the ground and back into the inventory.
//
// Every address, vtable slot, offset and function signature it uses is declared in game/sdk.hpp (build 2.2.4.3 only:
// init() refuses to run on any other server.dll).  The work is done by the modules of game::detail, one per topic
// (entities, weapons, pickup, rounds, players, infection ...); this file queues the HUD's requests and finds the
// game's functions, each module checking and hooking its own.
//
// The game's code is not thread safe: every call into it happens in pump(), which runs on the game's main thread
// (the window procedure gets k_msg_pump).

#include "game/game.hpp"

#include "core/config.hpp"
#include "core/log.hpp"
#include "game/addons.hpp"
#include "game/ammo_piles.hpp"
#include "game/backend.hpp"
#include "game/client_remote.hpp"
#include "game/client_view.hpp"
#include "game/entities.hpp"
#include "game/infection.hpp"
#include "game/lifecycle.hpp"
#include "game/mem.hpp"
#include "game/native_slots.hpp"
#include "game/pickup.hpp"
#include "game/players.hpp"
#include "game/rounds.hpp"
#include "game/stamina.hpp"
#include "game/unload.hpp"
#include "game/weapon_panel.hpp"
#include "game/weapons.hpp"

#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

namespace game
{

    namespace
    {
        using namespace detail;

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

        ULONGLONG g_last_sync = 0;

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
            void unload_weapon() override
            {
                post({Command::Unload, 0, 0});
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
                hide_item_boxes(!core::config().replace_native &&
                                (core::config().inventory_view == core::view_wheel ||
                                 (core::config().show_hotbar && core::shows_slots())));
                refresh_view();
                reload_menus(); // once, at the main menu, after the menu's addon is mounted
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
                    if (command.kind == Command::Unload)
                        client_mark_unloaded();
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
                    Unload, // the gun in the hands, or the one on the ground in front of the player
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
                case Command::Unload:
                    do_unload();
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
                case Command::Unload:
                    std::snprintf(text, sizeof(text), "inv_unload");
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
        mount_addons(); // (every copy of the mod: the host, a player on a server, the dedicated server)
        if (!init_env())
            return false;

        // the game's events: muting the ones a switch creates (see hk_create_event); the infection's triggers, deaths
        // and round starts (FireEvent: see "the infection", "a death, a new game")
        if (HMODULE engine = GetModuleHandleW(L"engine.dll"))
            if (const auto engine_create = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, "CreateInterface")))
                if (void* events = engine_create("GAMEEVENTSMANAGER002", nullptr))
                {
                    install_create_event(events);
                    install_fire_event(events);
                }

        install_pickup_hook(); // the +use pickup (optional)
        install_spawner_hooks(); // map item spawners (optional)
        install_ammo_spawn_use(); // an ammo pile's own Use (optional): what a pile of dropped rounds gives
        install_gun_use(); // a gun's own Use (optional)
        install_reload_hooks(); // a gun unloaded in the hands stays empty (see reload_held_back)

        mem::ModuleInfo client;
        if (mem::module_info(L"client.dll", client))
        {
            install_client_reload_hooks(client); // a gun unloaded in the hands stays empty on the client too
            install_weapon_panel(client); // the game's own weapon-selection panel
        }

        // other players (a dedicated server's, a listen server's guests): their inventories, their requests and the
        // answers (see "other players"); the infection and stamina use the same engine interface
        {
            HMODULE engine = GetModuleHandleW(L"engine.dll");
            const auto engine_create =
                engine != nullptr ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, "CreateInterface")) : nullptr;
            g_engine_server = engine_create != nullptr ? engine_create("VEngineServer022", nullptr) : nullptr;
            install_server_command();
            install_infection();
            install_stamina();
            install_client_commands();
            install_transition_restore(); // a chapter transition keeps the reserve of the stored guns (see lifecycle)
        }
        if (!g_dedicated)
            install_client_remote(); // a game client: playing on someone else's server through the inv_* commands

        set_backend(&g_real);
        return true;
    }

} // namespace game
