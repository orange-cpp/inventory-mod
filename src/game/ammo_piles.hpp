#pragma once
// [ammo_piles]: a map's ammo pile holds a set amount of one round type, and the mod puts packs of its own on the map
// when a round starts.  +use on any ammo pile comes through here.  Main thread.

namespace game::detail
{

    // CWeaponAmmoSpawn::Use: what a pile of dropped rounds, or a finite one, gives.
    void install_ammo_spawn_use();
    // A round started: the map's piles are new ones, and the mod's packs go on the map a moment later.
    void on_round_start_piles();
    // From GameFrame: the packs, when a round start asked for them a moment ago.
    void place_map_packs_due();

} // namespace game::detail
