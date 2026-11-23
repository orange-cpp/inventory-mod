#pragma once
// Guns, melee weapons and items between the player's hands, the inventory's boxes and the ground: the HUD's requests
// (use, drop, store, pick up), and the rounds a weapon and the player carry.  Main thread.

#include "game/entities.hpp"
#include "inv/inventory.hpp"

#include <string>
#include <windows.h>

namespace game::detail
{

    extern bool g_giving; // do_use is handing an item out: the +use hooks must not store it
    extern ULONGLONG g_switch_mute_until; // "weapon_pickup" is muted until then (see hk_create_event)

    // Hooks IGameEventManager2::CreateEvent (slot 6 of `events`) to mute the events a switch creates (optional).
    void install_create_event(void* events);

    inv::WeaponState weapon_state(void* player, void* weapon, const inv::ItemDef& def, bool carried);
    int ammo_type_of(void* weapon); // -1: none; the auto shotguns' is the shotguns' (see retarget_ammo_types)
    bool carries_ammo_type(void* player, int type);

    // The player's reserve of every ammo type.
    struct AmmoPool
    {
        int count[32] = {};
        bool read = false;
    };
    AmmoPool read_pool(void* player);
    void write_pool(void* player, const AmmoPool& pool);
    void add_to_pool(void* player, const char* classname, int type, int rounds);
    int* reserve_of(void* player, int type);
    // Every tick: the auto shotguns survivors carry take the pump shotguns' shells (see weapons.cpp).
    void retarget_ammo_types();

    void take_from_inventory(const inv::ItemDef& def, int count);
    void put_away_carried(void* player, const char* switching_to);

    // Where the player's eyes are and which way they look.
    bool player_view(void* player, Vec3& eye, Vec3& forward);
    // The closest catalogue item lying in front of the eyes (within k_pickup_reach, at least `min_facing` the cosine
    // of the angle off the view) that nobody carries and that `wanted` accepts; nullptr when there is none.
    void* item_in_front(const Vec3& eye, const Vec3& forward, float min_facing, bool (*wanted)(const inv::ItemDef&),
                        const inv::ItemDef** found_def, float* found_score = nullptr);
    // `spent`: a gas can's, what it lacks of its fuel ([chainsaw], see fuel.cpp).
    void* spawn_dropped(const inv::ItemDef& def, const inv::WeaponState& state, const Vec3& position, int spent = 0);

    void do_use(int slot);
    void do_drop(int slot, int count);
    void do_store_held();
    void do_pickup_nearby();
    void do_spawn(const inv::ItemDef& def);

} // namespace game::detail
