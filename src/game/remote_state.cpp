#include "game/remote_state.hpp"

#include <algorithm>
#include <cstdio>

namespace game::detail
{
    void reconcile_native_inventory(inv::Inventory& inventory, std::span<const NativeWeapon> native,
                                    float rounds_weight)
    {
        for (int slot = 0; slot < inventory.slot_count(); ++slot)
        {
            const inv::Stack& stack = inventory.slot(slot);
            if (!stack.held)
                continue;
            const bool carried = std::any_of(native.begin(), native.end(), [&](const NativeWeapon& weapon) {
                return weapon.classname == stack.def->classname;
            });
            if (!carried || inventory.held_slot(*stack.def) != slot)
                inventory.clear_held(slot);
        }

        float weapons_weight = 0.f;
        for (const NativeWeapon& weapon : native)
        {
            const inv::ItemDef* def = inv::find_item(weapon.classname);
            if (def != nullptr && !def->weapon() && inventory.held_slot(*def) < 0)
                inventory.set_held(inventory.home_for(*def), *def);
            weapons_weight += inv::weapon_weight(weapon.classname);
        }
        inventory.set_weapons_weight(weapons_weight);
        inventory.set_ammo_weight(rounds_weight);
    }

    std::string encode_remote_state(const inv::Inventory& inventory, std::span<const NativeWeapon> native,
                                    std::string_view message)
    {
        char head[64];
        std::snprintf(head, sizeof(head), "%s %d %.2f %.2f %d|", k_state_marker, inventory.slot_count(),
                      inventory.limits().max_weight, inventory.weight(), inventory.weapon_slot_count());
        std::string line = head;
        for (int i = 0; i < inventory.total_slots(); ++i)
        {
            const inv::Stack& stack = inventory.slot(i);
            if (stack.empty())
                continue;
            line += std::to_string(i) + ' ' + stack.def->classname + ' ' + std::to_string(stack.count) + ' ' +
                    (stack.held ? '1' : '0');
            if (stack.def->weapon()) // a weapon's rounds and kind, as in the save file
                line += ' ' + std::to_string(stack.weapon.clip) + ' ' + std::to_string(stack.weapon.ammo) + ' ' +
                        (stack.weapon.melee.empty() ? std::string("-") : stack.weapon.melee) + ' ' +
                        std::to_string(stack.weapon.upgrades) + ' ' + std::to_string(stack.weapon.special) + ' ' +
                        std::to_string(stack.weapon.wear);
            else if (stack.def->fuel > 0) // a gas can's ([chainsaw]): what the box lacks, and what a full one holds
                line += ' ' + std::to_string(stack.spent) + ' ' + std::to_string(stack.def->fuel);
            line += ',';
        }
        line += '|';
        for (std::size_t i = 0; i < native.size(); ++i)
        {
            if (i != 0)
                line += ',';
            if (native[i].classname.empty())
                line += '-';
            else
                line += (native[i].active ? "*" : "") + native[i].classname +
                        (native[i].melee.empty() ? std::string() : ":" + native[i].melee) +
                        (native[i].melee.empty() || native[i].wear < 0 ? std::string() : ":" + std::to_string(native[i].wear));
        }
        line += '|';
        line += message;
        line += '\n';
        return line;
    }
}
