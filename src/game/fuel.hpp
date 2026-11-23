#pragma once
// [chainsaw]: the chainsaw runs on the fuel of the gas cans the survivors carry.  Server side, main thread.

namespace game::detail
{

    // What gas can `can` (an entity on the ground or in someone's hands) lacks of its fuel: a can keeps it while it
    // leaves the inventory and comes back.  0: full, or one the mod never saw.
    int can_spent(void* can);
    void set_can_spent(void* can, int spent);

    // Every game frame: the reload key with the chainsaw in hand starts a refuel from the inventory's gas cans; the
    // refuels under way (called off, or done).
    void chainsaw_frame();

    // Hooks the chainsaw's ItemPostFrame (optional: without it there is no [chainsaw]).
    void install_chainsaw();

} // namespace game::detail
