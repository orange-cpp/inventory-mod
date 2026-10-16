#pragma once
// The mod's settings, kept in inventory_mod.ini next to the DLL.

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
        // The keys 1..9 and 0 select hotbar slots 1..10 and the mouse wheel steps through the items; the game's own
        // weapon panel is hidden and its slot keys / wheel no longer reach it.
        bool replace_native = true;

        int slots = 8;
        float max_weight = 0.f; // 0: unlimited
        bool show_hotbar = true;
        bool auto_store = false; // +use on a catalogue item always stores it (otherwise only when the player's slot is full)
        float hud_scale = 1.f;
    };

    Config& config();
    void config_load();
    void config_save();
    // "I", "[", "F5" ...
    std::string key_name(int vk);

    // The inventory's contents (Inventory::to_text), kept in inventory_mod.inventory next to the DLL.
    std::string inventory_file_load();
    void inventory_file_save(const std::string& text);

} // namespace core
