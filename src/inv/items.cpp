#include "inv/items.hpp"

namespace inv
{

    namespace
    {
        constexpr std::uint32_t rgb(std::uint32_t r, std::uint32_t g, std::uint32_t b)
        {
            return r | (g << 8) | (b << 16) | (0xFFu << 24);
        }

        constexpr ItemDef k_items[] = {
            {"weapon_first_aid_kit", "First Aid Kit", "FA", "Heals most of your missing health.", 2, 3.0f, rgb(210, 60, 60), 3},
            {"weapon_defibrillator", "Defibrillator", "DF", "Revives a dead survivor.", 1, 3.0f, rgb(230, 200, 60), 3},
            {"weapon_pain_pills", "Pain Pills", "PP", "Temporary health.", 4, 1.0f, rgb(235, 235, 235), 4},
            {"weapon_adrenaline", "Adrenaline", "AD", "Run faster and shrug off damage for a while.", 4, 1.0f,
             rgb(120, 190, 240), 4},
            {"weapon_pipe_bomb", "Pipe Bomb", "PB", "Attracts the horde, then explodes.", 3, 2.0f, rgb(150, 150, 150), 2},
            {"weapon_molotov", "Molotov", "MO", "Sets the ground on fire.", 3, 2.0f, rgb(240, 130, 40), 2},
            {"weapon_vomitjar", "Bile Jar", "BJ", "Draws infected to the target.", 3, 2.0f, rgb(150, 190, 70), 2},
            {"weapon_upgradepack_incendiary", "Incendiary Ammo", "IA", "Incendiary ammo upgrade pack.", 1, 3.0f,
             rgb(240, 90, 40), 3},
            {"weapon_upgradepack_explosive", "Explosive Ammo", "EA", "Explosive ammo upgrade pack.", 1, 3.0f,
             rgb(250, 170, 40), 3},
            {"weapon_gascan", "Gas Can", "GC", "Fuel for the generator.", 1, 6.0f, rgb(200, 60, 40), 5},
            {"weapon_propanetank", "Propane Tank", "PT", "Explodes when shot.", 1, 5.0f, rgb(220, 220, 230), 5},
            {"weapon_oxygentank", "Oxygen Tank", "OT", "Explodes when shot.", 1, 5.0f, rgb(110, 170, 220), 5},
            {"weapon_fireworkcrate", "Fireworks", "FW", "Loud and attractive.", 1, 4.0f, rgb(200, 80, 160), 5},
            {"weapon_gnome", "Gnome", "GN", "Gnome Chompski.", 1, 2.0f, rgb(90, 160, 90), 5},
            {"weapon_cola_bottles", "Cola Bottles", "CB", "Delivery item.", 1, 4.0f, rgb(180, 40, 40), 5},
        };
    } // namespace

    std::span<const ItemDef> all_items()
    {
        return k_items;
    }

    const ItemDef* find_item(std::string_view classname)
    {
        for (const ItemDef& def : k_items)
            if (classname == def.classname)
                return &def;
        return nullptr;
    }

} // namespace inv
