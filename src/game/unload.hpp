#pragma once
// The unload key: the gun in the player's hands, or one lying in front of them (or a map's gun spawner's), is emptied
// into the player's reserve; what does not fit is dropped as a pile of rounds.  Main thread.

namespace game::detail
{

    // CTerrorGun's and the shotguns' Reload, and HandleFireOnEmpty: a gun unloaded in the hands stays empty.
    void install_reload_hooks();
    void do_unload();

} // namespace game::detail
