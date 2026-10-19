#pragma once
// [stamina]: running, jumping, a melee swing and a shove use it; at 0 a survivor is exhausted: slow, slower to swing,
// and their melee weapon does less damage.  Server side, main thread.

namespace game::detail
{

    // The start of the line a player's client DLL reads their stamina from.
    constexpr char k_stamina_marker[] = "@sta@";

    // gpGlobals, the change accessor and SetShovePenalty; GameFrame must be hooked already.
    void install_stamina();
    // From GameFrame.
    void stamina_frame();

    // What a hit of `attacker`'s melee weapon `weapon` does, as a factor: `damage` % while they are exhausted, else 1
    // (melee.cpp's TakeDamage hook asks).
    float stamina_damage_scale(void* attacker, void* weapon);
    bool stamina_exhausted(void* player);
    // Uses `cost` of the player's stamina (less than 0: gives it back); nothing without [stamina].
    void stamina_spend(void* player, float cost, const char* what);

} // namespace game::detail
