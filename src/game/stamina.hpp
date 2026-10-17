#pragma once
// [stamina]: running, jumping, a melee swing and a shove use it; at 0 a survivor is exhausted: slow, slower to swing,
// and their melee weapon does less damage.  Server side, main thread.

namespace game::detail
{

    // The start of the line a player's client DLL reads their stamina from.
    constexpr char k_stamina_marker[] = "@sta@";

    // gpGlobals, the change accessor, SetShovePenalty and the TakeDamage hook; GameFrame must be hooked already.
    void install_stamina();
    // From GameFrame.
    void stamina_frame();

} // namespace game::detail
