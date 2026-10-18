#pragma once
// A death, a new game: the inventories follow the game's own rules.  And the two hooks every server-side feature runs
// from: IGameEventManager2::FireEvent (the game's events) and IServerGameDLL::GameFrame (every server frame).

#include <cstdint>

namespace game::detail
{

    // FireEvent: slot 7 of `events` (IGameEventManager2).
    void install_fire_event(void* events);
    bool fire_event_installed();
    // GameFrame at `at` (ServerGameDLL005 slot 4, checked by the caller).
    void install_game_frame(std::uintptr_t at);
    bool game_frame_installed();
    // The game's restore of a survivor after a chapter transition: the reserve of their stored guns comes back too.
    void install_transition_restore();

} // namespace game::detail
