#pragma once

#include "inv/inventory.hpp"

#include <span>
#include <string>
#include <string_view>

namespace game
{
    constexpr int k_native_slots = 6; // primary, secondary, throwable, kit, pills, carried

    // A weapon or item the player carries in the game's own slots (the ones the native weapon panel lists).
    struct NativeWeapon
    {
        std::string classname; // "weapon_rifle_ak47"
        std::string name; // "Rifle Ak47"
        char tag[3] = {}; // two letters for the slot
        bool active = false; // in the player's hands now
        std::string melee; // weapon_melee: which one ("fireaxe" ...), for its icon
        int wear = -1; // weapon_melee: its wear ([melee] durability: 0 .. inv::k_wear_broken; -1: none)
    };

    namespace detail
    {
        inline constexpr char k_state_marker[] = "@inv@";

        void reconcile_native_inventory(inv::Inventory& inventory, std::span<const NativeWeapon> native,
                                        float rounds_weight);

        // An inventory update sent through the Source server's ClientPrintf.
        std::string encode_remote_state(const inv::Inventory& inventory, std::span<const NativeWeapon> native,
                                        std::string_view message);
    }
}
