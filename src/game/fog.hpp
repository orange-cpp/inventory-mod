#pragma once
// A fog over the map, from the game's own: client.dll's fog_* ConVars (the engine's depth-correct distance fog, which
// the world, the props and the sky all take).  The Debug tab's switch.  Main thread.

#include "game/mem.hpp"

namespace game::detail
{

    // Finds the ConVars in client.dll (by their registrations) and takes the cheat flag off them; nothing is hooked.
    void install_client_fog(const mem::ModuleInfo& client);
    // Fog on (density 0 .. 1: how thick, from a clear near distance to a thick far one) or back to the map's own.  It keeps the map's fog colour.
    void set_fog(bool on, float density);
    // Every pump: switches the fog on once the map's own values have been copied (fog_override -1).
    void fog_tick();

} // namespace game::detail
