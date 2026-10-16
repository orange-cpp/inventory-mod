#pragma once

namespace game
{

    // Finds the game's interfaces and functions for the build this mod was made for (server.dll 2.2.4.3) and, when they
    // all check out, makes the real backend the active one.  False (with the reason in the log) leaves the stub.
    // `dedicated`: running in srcds.exe (per-player inventories, no HUD).
    bool init(bool dedicated = false);

} // namespace game
