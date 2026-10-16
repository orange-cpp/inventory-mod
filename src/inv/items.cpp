#include "inv/items.hpp"

namespace inv
{

    namespace
    {
        constexpr std::uint32_t rgb(std::uint32_t r, std::uint32_t g, std::uint32_t b)
        {
            return r | (g << 8) | (b << 16) | (0xFFu << 24);
        }

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

    namespace
    {
        struct WeaponWeight
        {
            const char* classname;
            float weight;
        };

        // The defaults, roughly by size; the rules file can change them
        WeaponWeight k_weapons[] = {
            {"weapon_pistol", 1.0f},
            {"weapon_pistol_magnum", 1.5f},
            {"weapon_melee", 2.0f},
            {"weapon_chainsaw", 6.0f},
            {"weapon_smg", 3.0f},
            {"weapon_smg_silenced", 3.0f},
            {"weapon_smg_mp5", 3.0f},
            {"weapon_pumpshotgun", 4.0f},
            {"weapon_shotgun_chrome", 4.0f},
            {"weapon_autoshotgun", 5.0f},
            {"weapon_shotgun_spas", 5.0f},
            {"weapon_rifle", 4.0f},
            {"weapon_rifle_ak47", 4.5f},
            {"weapon_rifle_desert", 4.5f},
            {"weapon_rifle_sg552", 4.5f},
            {"weapon_hunting_rifle", 5.0f},
            {"weapon_sniper_military", 5.5f},
            {"weapon_sniper_awp", 5.5f},
            {"weapon_sniper_scout", 4.0f},
            {"weapon_rifle_m60", 8.0f},
            {"weapon_grenade_launcher", 6.0f},
        };

        WeaponWeight* find_weapon(std::string_view classname)
        {
            for (WeaponWeight& w : k_weapons)
                if (classname == w.classname)
                    return &w;
            return nullptr;
        }
    } // namespace

    float weapon_weight(std::string_view classname)
    {
        const WeaponWeight* w = find_weapon(classname);
        return w != nullptr ? w->weight : 0.f;
    }

    bool set_weapon_weight(std::string_view classname, float weight)
    {
        WeaponWeight* w = find_weapon(classname);
        if (w == nullptr || weight < 0.f)
            return false;
        w->weight = weight;
        return true;
    }

    namespace
    {
        ItemDef* find_mutable(std::string_view classname)
        {
            for (ItemDef& def : k_items)
                if (classname == def.classname)
                    return &def;
            return nullptr;
        }
    } // namespace

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
        if (def == nullptr || max_stack < 1)
            return false;
        def->max_stack = max_stack;
        return true;
    }

} // namespace inv
