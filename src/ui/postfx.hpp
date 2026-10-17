#pragma once
// The infection's and the stamina's screen effects: a pixel shader run over the game's frame (a copy of the render
// target).  Each can go on one of three layers (the settings' [infection] and [stamina] cover_game_hud / cover_mod_hud):
// under the game's HUD (run from the game's own rendering just before it draws its HUD, see overlay.cpp), over the
// game's HUD but under the mod's (at Present, before the mod's HUD), or over everything (at Present, last).  The shaders are compiled at run time with Windows' d3dcompiler_47.dll; without it (or when a
// shader does not compile) ready() stays false and the HUD draws a plain tint and vignette instead.

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace postfx
{

    // Render thread, once the device is known: compiles the shaders.  False: the effect is not available.
    bool init(IDirect3DDevice9* device);
    bool ready();
    enum class Layer
    {
        UnderGameHud, // the game's 3D picture only (the HUDs over it)
        UnderModHud, // the game's picture and HUD, not the mod's
        OverAll, // everything, the mod's HUD and window too
    };
    // Render thread, while the HUD draws: the effect for this frame.  severity 0 (just infected) .. 1 (turning), beat
    // the heartbeat 0 .. 1, last 0 .. 1 over the clock's last seconds; on `layer` (UnderGameHud: from the next frame
    // on, which the game draws before this frame's Present).
    void set_infection(float severity, float beat, float last, Layer layer = Layer::OverAll);
    // Whether the next frame wants the pass under the game's HUD (any thread: the game's main thread asks).
    bool wants_under_game_hud();
    // The stamina's vignette (its own shader, run before the infection's): whether it compiled, and this frame's
    // (render thread, while the HUD draws).  tired 0 (half full or more) .. 1 (empty), exhausted 0 .. 1 (eased in and
    // out), breath 0 .. 1 (each gasp while exhausted); on `layer`, as set_infection.
    bool stamina_ready();
    void set_stamina(float tired, float exhausted, float breath, Layer layer = Layer::OverAll);
    // Render thread, while the HUD draws: the screamer for this frame, drawn over the frame and the HUD before the
    // effect (set_infection) so the effect runs over it too: a black backdrop (its alpha 0 .. 1), the picture over
    // `rect` (x0, y0, x1, y1 in pixels) tinted `color` (0xAARRGGBB), then a red wash and a white flash (their alphas).
    void set_screamer(void* texture, const float rect[4], unsigned color, float backdrop, float red, float flash);
    // Render thread: the effects of `layer` over `target` (the back buffer, or the game's render target before its
    // HUD).  OverAll is the frame's last pass (after the mod's HUD): the screamer, then the stamina and the infection
    // set for it; it also ends the frame (what was set is forgotten, what goes under the next frame's HUD is kept).
    void render(IDirect3DDevice9* device, IDirect3DSurface9* target, Layer layer);
    // Render thread: the screamer's picture (settings: screamer_image) as an IDirect3DTexture9 (an ImTextureID), read
    // with WIC on first use and again when the setting changes; nullptr when there is none.  `aspect`: width / height.
    void* screamer_texture(float* aspect);
    // Before the device's Reset (the frame's copy lives in the default pool).
    void invalidate();
    // Frees everything (render thread).
    void shutdown();

} // namespace postfx
