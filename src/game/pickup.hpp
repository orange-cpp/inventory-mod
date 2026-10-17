#pragma once
// +use on a weapon or an item lying in the world: the player's pick-up (slot 426), a gun's own Use and the map's item
// spawners.  The inventory takes what the game's slots have no room for.  Main thread.

#include "game/entities.hpp"

#include <cstdint>

namespace game::detail
{

    extern WeaponIdNameFn g_weapon_id_name; // weapon ID -> "weapon_first_aid_kit" (nullptr: not found)
    extern std::uintptr_t g_spawner_use; // CWeaponSpawn::Use: slot 106 of every weapon_*_spawn but the ammo pile's

    void install_pickup_hook(); // CTerrorPlayer's +use pick-up of a weapon (slot 426)
    void install_spawner_hooks(); // CWeaponSpawn::Use and Weapon_GetSlot
    void install_gun_use(); // CTerrorGun::Use

} // namespace game::detail
