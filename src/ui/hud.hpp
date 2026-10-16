#pragma once
// The inventory HUD: a hotbar that is always on and an inventory window with the nullcore look.

#include <windows.h>

namespace hud
{

    // Once, before the first frame (loads the config).
    void init();
    // Every frame, between ImGui::NewFrame() and ImGui::Render().
    void draw();
    // The inventory window is open, so the overlay takes the mouse and the keyboard from the game.
    bool wants_input();
    // A window message the game is about to get, for the hotkeys.  True: handled, do not pass it on.
    bool on_wnd_message(UINT msg, WPARAM wparam, LPARAM lparam);
    // Ctrl+End was pressed (or request_unload was called).
    bool unload_requested();
    void request_unload();

} // namespace hud
