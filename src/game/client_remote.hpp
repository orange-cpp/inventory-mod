#pragma once
// Playing on a server this game does not run (a dedicated server, someone else's listen server): the HUD's requests go
// to it as console commands, and the inventory shown is the one it sends back.  Client side, main thread.

#include "game/sdk.hpp"

#include <string>

namespace game::detail
{

    extern void* g_engine_client; // engine.dll VEngineClient013 (nullptr: not found)
    extern sdk::IsInGameFn g_is_in_game;

    // The engine's client interface and the ProcessPrint hook, then what the player sees (client_view).
    void install_client_remote();

    bool in_remote_game();
    // Connected to a server, or loading into one (false at the main menu, and when IsConnected was not found).
    bool connected_to_server();
    // Whether connected_to_server() can tell.
    bool knows_connection();
    void send_to_server(const std::string& command);
    // The local inventory comes back when the player is no longer on someone else's server.
    void leave_remote();

} // namespace game::detail
