#pragma once
// The mod's addons: every .vpk in the addons folder next to the DLL, mounted into the game's file system.

namespace game::detail
{

    void mount_addons();
    // Main thread, from pump(): once the menu's addon is mounted and the game is at its menu, the menu is built again
    // from its layout files, so the Options flyout gets the mod's button.
    void reload_menus();
    // The game can load `name` (its own files, the addons): main thread.
    bool game_file_exists(const char* name);

} // namespace game::detail
