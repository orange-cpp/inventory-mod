#pragma once
// [melee]: the melee of No More Room in Hell 2 (light and heavy swings, the wind-up, the cleave, the weapon
// classes, wear, block and deflect, the heavy shove, the throw).  The server's half (melee.cpp: PrimaryAttack,
// SecondaryAttack, TakeDamage and WeaponSound hooks; from GameFrame what each player's client reports) and the
// client's (melee_client.cpp: the buttons held back, the GetButtonBits hook; the pose over the viewmodel, the
// CalcViewModelView hook).  Main thread.

#include "game/mem.hpp"

namespace game::detail
{

    // The start of the line a player's client DLL reads the server's [melee] from: "@mel@ <heavy_hold ms>
    // <heavy_shove_hold ms> <features: 1 overhead, 2 throw>" (the hold is the weapon in the hands'), and "@mhi@ <1: heavy>" when their
    // swing hit (the impact).
    constexpr char k_melee_marker[] = "@mel@";
    constexpr char k_melee_hit_marker[] = "@mhi@";

    // The melee weapon's PrimaryAttack and SecondaryAttack, CBaseEntity::TakeDamage (the melee damage: [melee] and
    // an exhausted survivor's), and the extras (stagger, view kick, wall, overhead, gesture); after install_stamina
    // (gpGlobals).
    void install_melee();
    // From GameFrame: the wind-up each player reports, and the rules to their client.
    void melee_frame();
    // The player's movement speed while winding up or blocking, as a factor (1: neither): from 1 to wind_speed % as
    // the charge grows, block_speed % while blocking.  The stamina multiplies it into its own; without [stamina] melee_frame applies it.
    float melee_move_scale(void* player);

    // A melee weapon entity's wear (0 .. inv::k_wear_broken), kept by its handle while it exists; the inventory keeps a
    // stored one's (WeaponState::wear).
    int melee_wear(void* weapon);
    void set_melee_wear(void* weapon, int wear);

    // The client's GetButtonBits and CalcViewModelView hooks (melee_client.cpp).
    void install_client_melee(const mem::ModuleInfo& client);
    // The server's lines (see k_melee_marker); the host's own hit straight from the server's half.
    void apply_melee_line(const char* text);
    void apply_melee_hit_line(const char* text);
    void melee_hit_feedback(bool heavy);

} // namespace game::detail
