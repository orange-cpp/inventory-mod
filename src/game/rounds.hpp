#pragma once
// The player's reserve as a thing of its own: it weighs, it is dropped as a pile of rounds (one of the game's own ammo
// piles, named for what it holds), and +use on such a pile gives its rounds.  Main thread.

#include "game/entities.hpp"

#include <string>

namespace game::detail
{

    // What a pile the mod made holds (`map`: one it put on the map, not one a player dropped); false for any other
    // entity.
    bool pile_contents(void* entity, std::string& kind, int& rounds, bool* map = nullptr);
    // How many of `rounds` of `kind` the player can take: up to the type's most and, with `weigh`, the weight limit of
    // session()'s inventory.
    int rounds_that_fit(void* player, const std::string& kind, int rounds, bool weigh);
    // The floor under `at` (256 units down at most), or `at` itself when there is none.
    Vec3 on_floor(const Vec3& at);
    // An ammo pile of `rounds` of `kind` on the floor under `position` (`map_spot`: exactly there), turned to `yaw`.
    // `map`: one the mod put on the map.  nullptr when it could not be made.
    void* spawn_pile(const std::string& kind, int rounds, const Vec3& position, float yaw, bool map, std::string& model_used,
                     bool& loaded, bool map_spot = false);
    // Remembers a pile dropped at `at`, so rounds dropped next to it join it.
    void track_dropped_pile(void* pile, const Vec3& at);
    // +use on a pile the mod made: its rounds, as many as fit.
    void take_pile(void* pile, void* player, const std::string& kind, int rounds, bool map);
    // Drops `count` (0: all) of the player's reserve of `kind` in front of them, as an ammo pile.
    void do_drop_ammo(const std::string& kind, int count);

} // namespace game::detail
