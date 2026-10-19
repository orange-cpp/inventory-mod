#pragma once
// [melee]: the melee of No More Room in Hell 2 (light and heavy swings, the wind-up, the queue, the cleave).  The
// server's half (PrimaryAttack and TakeDamage hooks, from GameFrame the wind-up each player's client reports) and the
// client's (the attack button held back while winding up: the GetButtonBits hook).  Main thread.

#include "game/mem.hpp"

namespace game::detail
{

    // The start of the line a player's client DLL reads the server's [melee] from.
    constexpr char k_melee_marker[] = "@mel@";

    // The melee weapon's PrimaryAttack and CBaseEntity::TakeDamage (the melee damage: [melee] and an exhausted
    // survivor's); after install_stamina (gpGlobals).
    void install_melee();
    // From GameFrame: the wind-up each player reports, and the rules to their client.
    void melee_frame();
    // The player's movement speed while winding up, as a factor (1: not winding): from 1 to wind_speed % as the
    // charge grows.  The stamina multiplies it into its own; without [stamina] melee_frame applies it.
    float melee_move_scale(void* player);

    // The client's GetButtonBits hook.
    void install_client_melee(const mem::ModuleInfo& client);
    // "@mel@ <hold ms> <queue ms>" (0 0: no [melee]) from the server.
    void apply_melee_line(const char* text);

} // namespace game::detail
