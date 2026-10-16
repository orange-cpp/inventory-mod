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

} // namespace inv
