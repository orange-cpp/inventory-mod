#pragma once
// [self_revive]: an incapacitated survivor gets up by themselves with an item from their inventory.  Server side, main
// thread.

#include "inv/items.hpp"

namespace game::detail
{

    // What the game says of a survivor (see sdk.hpp: CTerrorPlayer's VScript bindings lead to them).  False when the
    // functions were not found.
    bool incapacitated(void* player); // down, or hanging from a ledge
    bool hanging_from_ledge(void* player);
    bool pinned(void* player); // held by a smoker, hunter, jockey or charger

    // The game's progress bar on `player`'s screen, `seconds` long from now (0: none).
    void show_progress(void* player, float seconds);
    // Takes it off again, unless something else has put one up since (one started at `since`, game time).
    void clear_progress(void* player, float since);

    // The request to take `def` in hand (from inventory box `box`, or -1: the one in the player's own slot for it):
    // when it gets an incapacitated survivor up ([self_revive]), it starts that (or says why not) and returns true;
    // false otherwise, and the request goes on as usual.
    bool start_self_revive(void* player, const inv::ItemDef& def, int box);
    // Every game frame: the self-revives under way (called off, or done).
    void revive_frame();

    // Finds the game's functions (optional: without them there is no self-revive).
    void install_revive();

} // namespace game::detail
