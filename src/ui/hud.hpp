#pragma once
// The inventory HUD: the boxes, the ammo counter, the stamina bar, the melee ring and the radial wheel, drawn with the
// game's VGUI surface (paint_vgui; ImGui when that is missing), and the inventory window (I), ImGui with the nullcore
// look.

#include <windows.h>

namespace hud
{

    // Once, before the first frame (loads the config).
    void init();
    // Every frame, between ImGui::NewFrame() and ImGui::Render(): the window, the screen effects' values, and the rest
    // of the HUD only while the game's VGUI is not painting it.
    void draw();
    // Every frame the game paints its panels (main thread, ui/vgui.cpp): everything but the inventory window, with the
    // game's VGUI surface.
    void paint_vgui();
    // The inventory window or the radial wheel is open, so the overlay takes the mouse from the game.
    bool wants_input();
    // ... and the keyboard too (the window; the wheel leaves it to the game, so the player can keep walking).
    bool wants_keyboard();
    // The inventory window is open (it is drawn at Present: what must go over it is drawn there too).
    bool window_open();
    // A window message the game is about to get, for the hotkeys.  True: handled, do not pass it on.
    bool on_wnd_message(UINT msg, WPARAM wparam, LPARAM lparam);
    // Ctrl+End was pressed (or request_unload was called).
    bool unload_requested();
    void request_unload();
    // Any thread: opens the window on its Settings tab (Options > Inventory Mod when the game's menu has no page for it).
    void open_settings();
    // Any thread: opens the window ("Inventory window" on the game's Options > Inventory Mod page).
    void open_window();
    // Any thread: the game's menu has the Options > Inventory Mod page (overlay.cpp): the settings are there, and the
    // window has no Settings tab.
    void set_settings_page(bool available);
    // Any thread: a setting chosen on the game's Options > Inventory Mod page ("show_hotbar" "1" ...), applied and saved
    // on the next frame (see overlay.cpp, k_page_settings).
    void apply_setting(const char* key, const char* value);

} // namespace hud
