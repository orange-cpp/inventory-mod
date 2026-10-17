#pragma once
// What the player sees (client side): whether the hotbar shows, the ammo counter, and a gun unloaded in the hands held
// empty by the client's prediction too.  Main thread.

#include "game/mem.hpp"
#include "game/sdk.hpp"

namespace game::detail
{

    // The client's entity list, the local player and whether GameUI is up; g_engine_client must be set.
    void install_client_view(sdk::CreateInterfaceFn engine_create);
    // The client's Reload and HandleFireOnEmpty hooks.
    void install_client_reload_hooks(const mem::ModuleInfo& client);

    // Is the player a living survivor in a game with no menu open, and the ammo of what they hold.
    void refresh_view();
    // The unload key was pressed: the gun in the hands (as the client sees it) does not reload by itself.
    void client_mark_unloaded();

} // namespace game::detail
