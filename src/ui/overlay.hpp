#pragma once
// Draws the HUD over the game: hooks IDirect3DDevice9::Present / Reset of the game's device, runs Dear ImGui (with
// imgui-nullcore's look) in the Present hook, and takes the mouse and keyboard from the game while the inventory is open.

#include <windows.h>

namespace ui::overlay
{

    // Finds the D3D9 vtable and hooks Present / Reset.  The first Present after that sets everything else up.
    bool install();
    // Tears the overlay down on the render thread (waits a little for the next Present), then removes the hooks.
    void uninstall();
    // Main thread, after the HUD's VGUI painting (ui/vgui.cpp): queues the pass that draws the screamer and the screen
    // effects into the game's own frame (postfx::render_in_frame), so a stream's capture has them.
    void queue_in_frame_pass();

} // namespace ui::overlay
