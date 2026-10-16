#pragma once
// The items the inventory knows: the carryable L4D2 weapons / items, by their entity classname.

#include <cstdint>
#include <span>
#include <string_view>

namespace inv
{

    struct ItemDef
    {
        const char* classname; // the game's entity classname, the id the saved files use
        const char* name; // display name
        const char* tag; // 2-letter mark drawn in a slot until real icons are loaded
        const char* description;
        int max_stack; // per slot
        float weight; // counts against Limits::max_weight
        std::uint32_t color; // r | g << 8 | b << 16 | a << 24 (ImGui's ImU32 layout)
        int game_slot; // the player's L4D2 inventory slot: 2 throwable, 3 medkit / upgrade, 4 pills, 5 carried item
    };

    std::span<const ItemDef> all_items();
    // nullptr when the classname is not an inventory item.
    const ItemDef* find_item(std::string_view classname);

    // The rules file's changes to an item (rules.hpp).  Done at start-up, before the inventories use the table.  False
    // for an unknown classname or a value out of range (weight < 0, stack < 1).
    bool set_item_weight(std::string_view classname, float weight);
    bool set_item_stack(std::string_view classname, int max_stack);

    // The guns and melee weapons are not inventory items, but what the player carries in the game's slots 0 and 1
    // counts against the weight limit too.  0 for a classname that is not one of them.
    float weapon_weight(std::string_view classname);
    // The rules file's [weight] entry for a weapon; false for an unknown classname or a negative weight.
    bool set_weapon_weight(std::string_view classname, float weight);

} // namespace inv
