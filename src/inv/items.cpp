#include "inv/items.hpp"

namespace inv
{

    namespace
    {
        constexpr std::uint32_t rgb(std::uint32_t r, std::uint32_t g, std::uint32_t b)
        {
            return r | (g << 8) | (b << 16) | (0xFFu << 24);
        }

        constexpr std::uint32_t k_gun = rgb(150, 160, 175);
        constexpr std::uint32_t k_side = rgb(170, 150, 120);

        // The weights and stack sizes are the defaults: the rules file can change them (set_item_weight / set_item_stack)
        ItemDef k_items[] = {
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

            // guns and melee weapons: one per box, with their rounds (WeaponState); weights roughly by size
            {"weapon_pistol", "Pistol", "P", "Sidearm.", 1, 1.0f, k_side, 1},
            {"weapon_pistol_magnum", "Magnum", "MG", "Heavy sidearm.", 1, 1.5f, k_side, 1},
            {"weapon_melee", "Melee Weapon", "ML", "Close combat.", 1, 2.0f, k_side, 1},
            {"weapon_chainsaw", "Chainsaw", "CS", "Loud and fuel hungry.", 1, 6.0f, k_side, 1},
            {"weapon_smg", "SMG", "SM", "Submachine gun.", 1, 3.0f, k_gun, 0},
            {"weapon_smg_silenced", "Silenced SMG", "SS", "Quiet submachine gun.", 1, 3.0f, k_gun, 0},
            {"weapon_smg_mp5", "MP5", "M5", "Submachine gun.", 1, 3.0f, k_gun, 0},
            {"weapon_pumpshotgun", "Pump Shotgun", "PS", "Pump-action shotgun.", 1, 4.0f, k_gun, 0},
            {"weapon_shotgun_chrome", "Chrome Shotgun", "CH", "Pump-action shotgun.", 1, 4.0f, k_gun, 0},
            {"weapon_autoshotgun", "Auto Shotgun", "AS", "Automatic shotgun.", 1, 5.0f, k_gun, 0},
            {"weapon_shotgun_spas", "Combat Shotgun", "SP", "Automatic shotgun.", 1, 5.0f, k_gun, 0},
            {"weapon_rifle", "M16", "M1", "Assault rifle.", 1, 4.0f, k_gun, 0},
            {"weapon_rifle_ak47", "AK-47", "AK", "Assault rifle.", 1, 4.5f, k_gun, 0},
            {"weapon_rifle_desert", "Combat Rifle", "SC", "Burst rifle.", 1, 4.5f, k_gun, 0},
            {"weapon_rifle_sg552", "SG552", "SG", "Assault rifle with a scope.", 1, 4.5f, k_gun, 0},
            {"weapon_hunting_rifle", "Hunting Rifle", "HR", "Sniper rifle.", 1, 5.0f, k_gun, 0},
            {"weapon_sniper_military", "Military Sniper", "MS", "Sniper rifle.", 1, 5.5f, k_gun, 0},
            {"weapon_sniper_awp", "AWP", "AW", "Sniper rifle.", 1, 5.5f, k_gun, 0},
            {"weapon_sniper_scout", "Scout", "SO", "Light sniper rifle.", 1, 4.0f, k_gun, 0},
            {"weapon_rifle_m60", "M60", "60", "Machine gun.", 1, 8.0f, k_gun, 0},
            {"weapon_grenade_launcher", "Grenade Launcher", "GL", "Explosive rounds.", 1, 6.0f, k_gun, 0},
        };

        ItemDef* find_mutable(std::string_view classname)
        {
            for (ItemDef& def : k_items)
                if (classname == def.classname)
                    return &def;
            return nullptr;
        }
    } // namespace

    std::span<const ItemDef> all_items()
    {
        return k_items;
    }

    const ItemDef* find_item(std::string_view classname)
    {
        return find_mutable(classname);
    }

    float weapon_weight(std::string_view classname)
    {
        const ItemDef* def = find_item(classname);
        return def != nullptr && def->weapon() ? def->weight : 0.f;
    }

    bool set_item_weight(std::string_view classname, float weight)
    {
        ItemDef* def = find_mutable(classname);
        if (def == nullptr || weight < 0.f)
            return false;
        def->weight = weight;
        return true;
    }

    bool set_item_stack(std::string_view classname, int max_stack)
    {
        ItemDef* def = find_mutable(classname);
        if (def == nullptr || max_stack < 1 || def->weapon()) // a weapon stays one per box: it has its own rounds
            return false;
        def->max_stack = max_stack;
        return true;
    }

} // namespace inv
