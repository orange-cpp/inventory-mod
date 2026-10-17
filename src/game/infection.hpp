#pragma once
// [infection]: a survivor knocked down by the infected, grabbed by a special or vomited on is infected; when the
// clock runs out they die and a special rises where they stood.  Server side, main thread.

#include "game/entities.hpp"

#include <string>

namespace game::detail
{

    // The start of the line a player's client DLL reads the infection's clock from.
    constexpr char k_infection_marker[] = "@inf@";

    // GetPlayerUserId, the `script` command's handler, CommitSuicide, the rescue closets and GameFrame (through
    // lifecycle); FireEvent must be hooked already.
    void install_infection();

    // VScript in the server's VM (at most 480 characters); false when the `script` command's handler was not found.
    bool run_script(const std::string& code);
    // A soundscript of the game's played on the player, for everyone near them to hear (VScript EmitSoundOn).
    void emit_sound_on(void* player, const char* sound);
    // What a gun picked up sounds like (scripts/game_sounds.txt: CHAN_WEAPON, ")items/itempickup.wav", volume 0.7):
    // played by the unload key and by a pile of rounds taken, as by the game's own pick-ups.
    constexpr char k_pickup_sound[] = "Player.PickupWeapon";

    void infect(void* player, const char* how);
    // A game event (from FireEvent): who is infected, cured, gone; all clear.
    void infection_on_event(const char* name, void* event, EventIntFn int_fn);
    // From GameFrame.
    void infection_frame();

} // namespace game::detail
