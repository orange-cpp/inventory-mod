#pragma once
// [map_guns]: a gun nobody has held yet has only a part of its magazine when a survivor first picks it up, and adds
// only a few magazines to the reserve; and the rounds are the player's, not the gun's, through the game's own
// pick-ups and drops.  Main thread.

#include "game/weapons.hpp"

#include <string>

namespace game::detail
{

    // A gun is known once anyone has held it: it keeps its magazine.
    void know_gun(void* weapon);
    void know_held_guns(void* player);
    // A round started: the map's guns are new ones.
    void forget_map_guns();

    // Limits what a pick-up (or a spawner's whole Use) adds to the reserve of the gun's ammo type: read before, the
    // rise past `before` + `bonus` is taken back when this goes out of scope.  `player` nullptr: nothing to limit.
    struct MapGunReserve
    {
        void* player = nullptr; // nullptr: nothing to limit
        int type = -1;
        int before = 0;
        int bonus = 0;
        std::string kind;
        bool spawner = false; // the spawner's: `before` per ammo type below, read before its Use
        int pool_before[32] = {};

        MapGunReserve() = default;
        MapGunReserve(const MapGunReserve&) = delete;
        MapGunReserve& operator=(const MapGunReserve&) = delete;
        ~MapGunReserve();
    };
    extern MapGunReserve* g_spawn_reserve; // a spawner's Use is running (see spawn_use)

    // Lives through a pick-up or a drop the game does: afterwards no ammo type of the player's reserve is below what
    // it was, a gun's own rounds join it, and a gun let go of keeps its magazine only.  `player` nullptr: nothing.
    struct KeepRounds
    {
        void* player = nullptr;
        void* gun = nullptr; // the gun being picked up (nullptr: a drop)
        int gun_extra = 0;
        AmmoPool before;
        std::uint32_t carried[k_max_weapons] = {};

        // A gun with rounds of its own (a CTerrorGun: not a pistol, the chainsaw or a melee weapon).
        static const char* gun_kind(void* weapon, char* name, int length);

        KeepRounds(void* player_, void* gun_);
        KeepRounds(const KeepRounds&) = delete;
        KeepRounds& operator=(const KeepRounds&) = delete;
        ~KeepRounds();

        void settle();
    };

    // [map_guns]: min_clip..max_clip % (rolled) of a `full` magazine.
    int rolled_map_clip(int full);
    // A survivor is picking `weapon` up: a gun nobody has held gets a part of its magazine, and `own_reserve` (or the
    // running spawner's) is set to limit what the pick-up adds to the reserve.
    void cut_map_gun(void* player, void* weapon, MapGunReserve& own_reserve);

} // namespace game::detail
