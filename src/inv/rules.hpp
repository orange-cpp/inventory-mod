#pragma once
// The rules file (inventory_mod.toml next to the DLL; on a dedicated server it sets the rules for every player): how
// many boxes a player has, how much they can carry and what each item weighs.
//
//   [limits]
//   slots = 8
//   weapon_slots = 2           # boxes for stored guns and melee weapons
//   max_weight = 20.0          # 0: no weight limit
//
//   [weight]                   # per item or weapon; "weapon_" may be left out
//   first_aid_kit = 4.0
//   pain_pills = 1
//   rifle_ak47 = 4.5           # the gun and the melee / pistol count too
//
//   [stack]                    # how many fit in one box
//   molotov = 2

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace inv
{

    struct Rules
    {
        std::optional<int> slots;
        std::optional<float> max_weight;
        std::optional<int> weapon_slots;
        int items_changed = 0; // weights and stack sizes set
        std::vector<std::string> errors; // "line 3: ..."
    };

    // Reads the rules and applies the item weights and stack sizes to the item table (set_item_weight /
    // set_item_stack); the limits are returned for the caller to use.  A file that does not parse changes nothing.
    Rules apply_rules(std::string_view text);

} // namespace inv
