#pragma once
// The infection's and the stamina's screen effects: a pixel shader run over the game's finished frame (a copy of the back buffer) before
// the HUD is drawn.  The shaders are compiled at run time with Windows' d3dcompiler_47.dll; without it (or when a
// shader does not compile) ready() stays false and the HUD draws a plain tint and vignette instead.

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace postfx
{

    // Render thread, once the device is known: compiles the shaders.  False: the effect is not available.
    bool init(IDirect3DDevice9* device);
    bool ready();
    // Render thread, while the HUD draws: the effect for this frame.  severity 0 (just infected) .. 1 (turning), beat
    // the heartbeat 0 .. 1, last 0 .. 1 over the clock's last seconds.
    void set_infection(float severity, float beat, float last);
    // The stamina's vignette (its own shader, run before the infection's): whether it compiled, and this frame's
    // (render thread, while the HUD draws).  tired 0 (half full or more) .. 1 (empty), exhausted 0 .. 1 (eased in and
    // out), breath 0 .. 1 (each gasp while exhausted).
    bool stamina_ready();
    void set_stamina(float tired, float exhausted, float breath);
    // Render thread, while the HUD draws: the screamer for this frame, drawn over the frame and the HUD before the
    // effect (set_infection) so the effect runs over it too: a black backdrop (its alpha 0 .. 1), the picture over
    // `rect` (x0, y0, x1, y1 in pixels) tinted `color` (0xAARRGGBB), then a red wash and a white flash (their alphas).
    void set_screamer(void* texture, const float rect[4], unsigned color, float backdrop, float red, float flash);
    // Render thread, after the HUD was rendered: runs the effect over the back buffer (the game's frame and the mod's
    // HUD) when one was set this frame.
    void render(IDirect3DDevice9* device, IDirect3DSurface9* back_buffer);
    // Render thread: the screamer's picture (settings: screamer_image) as an IDirect3DTexture9 (an ImTextureID), read
    // with WIC on first use and again when the setting changes; nullptr when there is none.  `aspect`: width / height.
    void* screamer_texture(float* aspect);
    // Before the device's Reset (the frame's copy lives in the default pool).
    void invalidate();
    // Frees everything (render thread).
    void shutdown();

} // namespace postfx
