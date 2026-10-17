#pragma once
// Other players: in srcds.exe every human player, and on a listen server every guest, has an inventory of their own,
// kept by SteamID.  While the game code serves one of them, an Activation makes them the local player and their
// inventory the active session.  Main thread.

#include "game/backend.hpp"
#include "game/entities.hpp"

#include <map>
#include <memory>
#include <string>
#include <windows.h>

namespace game::detail
{

    // The start of the line a player's client DLL reads their inventory from (see send_state).
    constexpr char k_state_marker[] = "@inv@";

    extern ClientPrintfFn g_client_printf; // VEngineServer022 slot 46 ClientPrintf: a line to one player's console

    struct ServerPlayer
    {
        std::string id;
        std::wstring file;
        Session session{.inventory = inv::Inventory(config_limits())};
        std::uint32_t saved_revision = 0;
        bool has_mod = false; // their HUD has talked to us
    };
    extern std::map<std::string, std::unique_ptr<ServerPlayer>> g_players;
    // When the last new game began (zero: none since the mod loaded): an inventory file written before then belongs to
    // an earlier game and is not read.
    extern FILETIME g_new_game;

    // The player's inventory (loaded on first use); nullptr for a bot or anything that is not a connected player.
    ServerPlayer* server_player(void* player);

    // Makes `player` the local player and their inventory the active session for its lifetime; ok() is false for a
    // bot, and with `require_mod` for a player whose game does not run the mod (nothing changes then).  Nests: the
    // outermost one sends the player their inventory when it ends.
    class Activation
    {
    public:
        explicit Activation(void* player, bool require_mod = true);
        ~Activation();
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
    bool served_remotely(void* player);

    // The rules file's [ammo] maxima into the game's own settings, once.
    void set_ammo_limits();

    // engine.dll's ServerCommand (for set_ammo_limits); g_engine_server must be set.
    void install_server_command();
    // GetPlayerNetworkIDString, ClientPrintf and the ClientCommand hook: the inv_* requests of other players.
    void install_client_commands();

} // namespace game::detail
