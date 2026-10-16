#pragma once
// The infection's screen effect: a pixel shader run over the game's finished frame (a copy of the back buffer) before
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
    // Render thread, after the HUD was drawn and before it is rendered: runs the effect over the back buffer when one
    // was set this frame.
    void render(IDirect3DDevice9* device, IDirect3DSurface9* back_buffer);
    // Before the device's Reset (the frame's copy lives in the default pool).
    void invalidate();
    // Frees everything (render thread).
    void shutdown();

} // namespace postfx
