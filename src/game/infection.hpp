#pragma once
// [infection]: a survivor knocked down by the infected, grabbed by a special or vomited on is infected; when the
// clock runs out they die and a special rises where they stood.  Server side, main thread.

#include "game/entities.hpp"

namespace game::detail
{

    // The start of the line a player's client DLL reads the infection's clock from.
    constexpr char k_infection_marker[] = "@inf@";

    // GetPlayerUserId, the `script` command's handler, CommitSuicide, the rescue closets and GameFrame (through
    // lifecycle); FireEvent must be hooked already.
    void install_infection();

    void infect(void* player, const char* how);
    // A game event (from FireEvent): who is infected, cured, gone; all clear.
    void infection_on_event(const char* name, void* event, EventIntFn int_fn);
    // From GameFrame.
    void infection_frame();

} // namespace game::detail
