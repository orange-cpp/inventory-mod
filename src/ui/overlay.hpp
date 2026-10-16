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

} // namespace ui::overlay
