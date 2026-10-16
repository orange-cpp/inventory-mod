#pragma once
// The mod's settings, kept in inventory_mod_settings.toml next to the DLL (an old inventory_mod.ini is moved into it).

#include <string>

namespace core
{

    struct Config
    {
        // Virtual-key codes.
        int key_toggle = 'I'; // opens / closes the inventory window
        int key_prev = 0xDB; // VK_OEM_4 '['  hotbar: previous item
        int key_next = 0xDD; // VK_OEM_6 ']'  hotbar: next item
        int key_use = 0xDC; // VK_OEM_5 '\'  use the selected item
        int key_drop = 'G'; // drop the selected item
        int key_store = 0xBA; // VK_OEM_1 ';'  store the item in your hands
        int key_pickup = 'H'; // take the item you are looking at into the inventory
        // Off (the default): the game's own weapon panel stays, with its keys 1 and 2 for the gun and melee; the
        // inventory is a grid under it (the throwable, kit and pills in their boxes), keys 3..9 and 0 take boxes 1..8.
        // On: the keys 1..9 and 0 select hotbar slots 1..10 and the mouse wheel steps through the items; the game's own
        // weapon panel is hidden and its slot keys / wheel no longer reach it.
        bool replace_native = false;
        // How the inventory is shown and picked from (the window, I, is always there):
        //   view_both:  the slots (grid, weapon boxes) and the radial wheel (middle mouse)
        //   view_slots: the slots only; the middle mouse button is the game's
        //   view_wheel: the wheel only; the game's own throwable / kit / pills boxes come back, and the number keys and
        //               the mouse wheel are the game's
        int inventory_view = 0;

        // The most boxes a player can have; the HUD shows only those in use (and one free item box), so the weight
        // limit is what really limits.
        int slots = 24;
        float max_weight = 0.f; // 0: unlimited
        int weapon_slots = 8; // the most boxes for stored guns and melee weapons (0..8), apart from the item boxes
        bool show_hotbar = true;
        bool auto_store = false; // +use on a catalogue item always stores it (otherwise only when the player's slot is full)
        float hud_scale = 1.f;
        // inventory_mod.toml (the rules) was read: its limits win over the settings' (see rules_load)
        bool rules_file = false;
    };

    Config& config();

    constexpr int view_both = 0, view_slots = 1, view_wheel = 2;
    inline bool shows_slots()
    {
        return config().inventory_view != view_wheel;
    }
    inline bool shows_wheel()
    {
        return config().inventory_view != view_slots;
    }
    // The settings (inventory_mod_settings.toml), then the rules file.
    void config_load();
    // inventory_mod.toml next to the DLL (inv/rules.hpp): the slots, the weight limit and the item weights / stacks.
    // On a dedicated server it is the rules for every player; their game clients show the server's limits.
    void rules_load();
    void config_save();
    // "I", "[", "F5" ...
    std::string key_name(int vk);

    // The inventory's contents (Inventory::to_text), kept in inventory_mod.inventory next to the DLL.
    std::string inventory_file_load();
    void inventory_file_save(const std::string& text);

} // namespace core
