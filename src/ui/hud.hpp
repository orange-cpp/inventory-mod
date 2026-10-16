#pragma once
// The inventory HUD: a hotbar that is always on and an inventory window with the nullcore look.

#include <windows.h>

namespace hud
{

    // Once, before the first frame (loads the config).
    void init();
    // Every frame, between ImGui::NewFrame() and ImGui::Render().
    void draw();
    // The inventory window or the radial wheel is open, so the overlay takes the mouse from the game.
    bool wants_input();
    // ... and the keyboard too (the window; the wheel leaves it to the game, so the player can keep walking).
    bool wants_keyboard();
    // A window message the game is about to get, for the hotkeys.  True: handled, do not pass it on.
    bool on_wnd_message(UINT msg, WPARAM wparam, LPARAM lparam);
    // Ctrl+End was pressed (or request_unload was called).
    bool unload_requested();
    void request_unload();

} // namespace hud
