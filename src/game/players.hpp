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

    // Runs `work` with `player`'s inventory the active session: the host's own, or another player's through an
    // Activation.  False (nothing run) for a bot or a player whose game does not run the mod: they have none.
    template <class F>
    bool with_inventory_of(void* player, F&& work)
    {
        if (player == nullptr)
            return false;
        if (player == g_active_player || (g_active_player == nullptr && player == host_player()))
        {
            work();
            return true;
        }
        if (!served_remotely(player))
            return false;
        Activation active(player);
        if (!active.ok())
            return false;
        work();
        return true;
    }

    // The rules file's [ammo] maxima into the game's own settings, once.
    void set_ammo_limits();
    // server.dll's ConVar or ConCommand `name`, found by its registration (`push "<name>"; mov ecx, <object>`); 0 when
    // not found.
    std::uintptr_t find_server_convar(const char* name);
    // Takes `flags` (0x2 development only, 0x4000 cheat) off a ConVar's flags and its parent's, or a ConCommand's
    // (ConCommandBase::m_nFlags +0x14, ConVar::m_pParent +0x1C).
    void unlock_convar(std::uintptr_t convar, int flags);
    // A console command on the server, queued (it runs at the engine's next pass of the command buffer); false
    // without ServerCommand.
    bool server_command(const char* text);

    // engine.dll's ServerCommand (for set_ammo_limits); g_engine_server must be set.
    void install_server_command();
    // GetPlayerNetworkIDString, ClientPrintf and the ClientCommand hook: the inv_* requests of other players.
    void install_client_commands();

} // namespace game::detail
