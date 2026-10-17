#pragma once
// The game's own HUD icons for weapons and items, read from its VPK archives at run time (nothing of the game is copied
// into the mod): each weapon script (scripts/weapon_<name>.txt) names a rectangle of a sprite sheet
// (materials/vgui/hud/iconsheet*.vtf) in its TextureData "weapon" block.

#include <string>

struct IDirect3DDevice9;

namespace icons
{

    struct Icon
    {
        void* texture = nullptr; // IDirect3DTexture9*, an ImTextureID for the DX9 backend
        float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
        float aspect = 1.f; // width / height of the rectangle
    };

    // Starts reading the archives on a thread of its own (the scripts and the sheets they use).
    void start();
    // Render thread: the device the textures are made on (nullptr: release them, e.g. when the overlay goes away).
    void set_device(IDirect3DDevice9* device);
    // Render thread: the icon of a weapon or item classname ("weapon_first_aid_kit"), or nullptr (none, or not loaded yet).
    const Icon* find(const char* classname);

    // The same icon as the game's own material draws it (the VGUI wheel, ui/vgui.cpp): the sheet's material path
    // ("vgui/hud/iconsheet", what materials/<path>.vmt names) and the rectangle on it.
    struct Sprite
    {
        std::string material;
        float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
        float aspect = 1.f;
    };
    // Any thread: false when there is none, or it is not loaded yet.
    bool find_sprite(const char* classname, Sprite& out);
    // Waits for the loading thread and frees everything.
    void shutdown();

} // namespace icons
