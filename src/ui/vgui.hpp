#pragma once
// The game's own 2D drawing: vgui::ISurface (vguimatsurface.dll) and the client scheme's fonts, so a part of the HUD
// (the radial wheel) is drawn the way the game draws its own: in its frame, under its menus, with its fonts (Trade
// Gothic Bold, or Stubble bold at high resolutions, scaled as the game scales them).  vgui2.dll's IPanel::PaintTraverse
// is hooked; after the engine's FocusOverlayPanel (the last of the game's panels, z 150) has painted, hud::paint_vgui
// draws.  See "The VGUI surface" in docs/game-notes.md.

#include <cstdint>
#include <string>

namespace ui::vgui
{

    // The client scheme's fonts the HUD uses (resource/clientscheme.res, proportional).
    enum class Font
    {
        Title, // PlayerDisplayName (14)
        Text, // HudHintText (12)
        Ammo, // HudAmmoSmall (12, additive): the game's ammo counter
        Hint, // MessageText (10)
        Count
    };

    struct Vertex
    {
        float x, y;
    };

    // Finds and checks the interfaces, then hooks PaintTraverse.  False: the HUD keeps drawing everything with ImGui.
    bool install();
    void uninstall();
    // Any thread: the hook has painted in the last moment (the game is drawing its panels: the HUD leaves the wheel to
    // hud::paint_vgui meanwhile).
    bool painting();

    // Main thread, inside hud::paint_vgui only.  Colours are ImU32's layout (IM_COL32: 0xAABBGGRR).
    void screen_size(int& w, int& h);
    void rect(int x0, int y0, int x1, int y1, std::uint32_t colour);
    // A convex polygon, filled.
    void polygon(const Vertex* vertices, int n, std::uint32_t colour);
    int font_tall(Font font);
    int text_width(Font font, const std::string& text);
    void text(Font font, int x, int y, std::uint32_t colour, const std::string& text);
    // A rectangle of a material ("vgui/hud/iconsheet"), tinted.
    void image(const std::string& material, int x0, int y0, int x1, int y1, float u0, float v0, float u1, float v1,
               std::uint32_t colour);

    // Main thread: a sound of the game's (under sound/: "ui/menu_focus.wav"), as its menus play them.
    void play_sound(const char* file);

} // namespace ui::vgui
