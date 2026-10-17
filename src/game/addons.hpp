#pragma once
// The mod's addons: every .vpk in the addons folder next to the DLL, mounted into the game's file system.

namespace game::detail
{

    void mount_addons();
    // The game can load `name` (its own files, the addons): main thread.
    bool game_file_exists(const char* name);

} // namespace game::detail
