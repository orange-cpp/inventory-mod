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
        int game_slot; // the player's L4D2 inventory slot: 0 gun, 1 melee / pistol, 2 throwable, 3 medkit / upgrade,
                       // 4 pills, 5 carried item
        // What a full one holds of fuel ([chainsaw] in the rules: the gas can), 0 for an item without any.  A box of
        // them keeps what its cans lack (Stack::spent).
        int fuel = 0;

        // A gun or melee weapon (slots 0 and 1): stored one per box with its rounds (WeaponState), shown in the hands
        // by the game's own panel.
        bool weapon() const
        {
            return game_slot < 2;
        }
    };

    std::span<const ItemDef> all_items();
    // nullptr when the classname is not an inventory item.
    const ItemDef* find_item(std::string_view classname);

    // The rules file's changes to an item (rules.hpp).  Done at start-up, before the inventories use the table.  False
    // for an unknown classname or a value out of range (weight < 0, stack < 1).
    bool set_item_weight(std::string_view classname, float weight);
    bool set_item_stack(std::string_view classname, int max_stack);
    // What a full one holds of fuel (ItemDef::fuel; 0: none).  False for an unknown classname, a weapon or fuel < 0.
    bool set_item_fuel(std::string_view classname, int fuel);

    // What a gun or melee weapon weighs (what the player carries in the game's slots 0 and 1 counts against the weight
    // limit too); 0 for a classname that is not one of them.
    float weapon_weight(std::string_view classname);

    // The reserve ammo types the guns share, by the game's names: the setting ammo_<kind>_max (server.dll) is the most a
    // player can carry of it.  M16, AK-47, SCAR and SG552 take "assaultrifle", the SMGs "smg", every shotgun "shotgun"
    // (the game's auto shotguns have a type of their own, autoshotgun 8, which the server merges into it), the
    // hunting rifle "huntingrifle", the other sniper rifles
    // "sniperrifle", then "grenadelauncher", "m60", "chainsaw".  nullptr for a weapon with none (melee, pistols).
    const char* ammo_kind(std::string_view classname);
    // Every kind ammo_kind returns, in a fixed order.
    std::span<const char* const> ammo_kinds();
    // The most rounds of `kind` a player can carry: the rules file's [ammo] value, else the game's default (360 for
    // "assaultrifle" ...).  -1 for an unknown kind.
    int ammo_max(std::string_view kind);
    // The game's index of ammo type `kind` (m_iAmmo[index], m_iPrimaryAmmoType): the order server.dll registers them in
    // (0x102CFEE0, CAmmoDef counting from 1): pistol 1, magnum 2, assaultrifle 3, minigun 4, smg 5, m60 6, shotgun 7,
    // (autoshotgun 8, not used), huntingrifle 9, sniperrifle 10, ..., grenadelauncher 17, ..., chainsaw 19.  -1 for an unknown kind.
    int ammo_index(std::string_view kind);
    // Whether the rules file set `kind` (so the game's setting is to be changed to it).
    bool ammo_max_set(std::string_view kind);
    bool set_ammo_max(std::string_view kind, int rounds); // -1: not set (the game's default)

    // What one round of `kind` weighs (the rules file's [round_weight], else the default: a rifle round 0.01, so 360 weigh
    // 3.6; a shotgun shell 0.04; a grenade 0.15 ...); 0 for an unknown kind.
    float round_weight(std::string_view kind);
    bool set_round_weight(std::string_view kind, float weight);
    void reset_round_weights(); // the defaults again
    // A magazine's worth of `kind`: what one drop of its rounds takes (rifle 50, shotgun 8, grenade launcher 1 ...).
    int ammo_magazine(std::string_view kind);
    // Its name in the HUD ("Rifle rounds"); nullptr for an unknown kind.
    const char* ammo_title(std::string_view kind);
    // Its colour, 0xRRGGBB: a dropped pile of it is tinted so (rifle yellow, SMG blue, shotgun red ...); 0xFFFFFF for
    // an unknown kind.
    unsigned ammo_color(std::string_view kind);
    // The kind of the game's ammo type `index` (m_iAmmo[index]); nullptr for one the mod does not know (pistols ...).
    const char* ammo_kind_at(int index);
    // What a player's reserve weighs: `pool` is m_iAmmo, the rounds of each of the game's 32 ammo types.
    float ammo_weight(const int (&pool)[32]);

    // Unloading a gun (the unload key): its magazine goes into the player's reserve of its ammo type.  Only guns whose
    // rounds a player can carry: not the M60 (its reserve's most is 0, the game refuses it any ammo) or the chainsaw
    // (fuel), and not the pistols or melee weapons (no ammo kind).
    bool unloadable(std::string_view classname);
    // Where the `rounds` of `kind` that come out of a magazine go: into the reserve as many as fit (the type's most, less
    // the `reserve` the player has; and `weight_room`, what the player can still carry, < 0 for no limit), the rest on
    // the ground as a pile of rounds.  Nothing stays in the gun.
    struct UnloadSplit
    {
        int to_reserve = 0;
        int to_ground = 0;
    };
    UnloadSplit split_unload(std::string_view kind, int rounds, int reserve, float weight_room);

} // namespace inv
