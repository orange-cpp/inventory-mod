// The game's own 2D drawing for the HUD (see vgui.hpp).
//
// The three interfaces come from CreateInterface: vgui2.dll's "VGUI_Panel009" (the global 0x10054324, VPanelWrapper,
// vtable 0x100480CC) and "VGUI_Scheme010" (the global 0x100587B0, CSchemeManager, vtable 0x10046FCC, stored by its
// constructor 0x1000F080), vguimatsurface.dll's "VGUI_Surface031" (the global 0x10128560, CMatSystemSurface, vtable
// 0x100BEF2C, stored by its constructor 0x10014490).  Each object's vtable is checked to be that one, and each slot used
// by its prologue (game/sdk.hpp lists them; docs/game-notes.md says how each was told apart).
//
// Where to draw: IPanel::PaintTraverse (slot 41, 0x1001EBE0) paints a panel and, through the client panel's own
// PaintTraverse, every child of it, so it sees each visible panel every frame.  The engine's CEngineVGui::Init makes
// "FocusOverlayPanel" (engine.dll 0x1021F9A5: a CFocusOverlayPanel on the static panel, SetZPos(150)): the last of the
// game's panels to paint, over its HUD and under nothing but GameUI's popups.  Once it has painted the surface is still
// between the root's StartDrawing and FinishDrawing, with the parent's (the whole screen's) context current, so the
// mod's drawing calls land in the game's frame as the HUD's do.  This runs on the game's main thread.

#include "ui/vgui.hpp"

#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/mem.hpp"
#include "game/sdk.hpp"
#include "ui/hud.hpp"
#include "ui/overlay.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <vector>
#include <windows.h>

namespace ui::vgui
{

    using namespace game::sdk;
    namespace mem = game::mem;

    namespace
    {
        void* g_surface = nullptr;
        void* g_panels = nullptr;
        void* g_schemes = nullptr;

        PanelNameFn g_panel_name = nullptr;
        PanelRectFn g_panel_size = nullptr;
        PanelRectFn g_panel_abs_pos = nullptr;
        GetSchemeFn g_get_scheme = nullptr;
        GetISchemeFn g_get_ischeme = nullptr;
        SurfaceColorFn g_set_color = nullptr;
        SurfaceRectFn g_filled_rect = nullptr;
        SurfaceFontFn g_text_font = nullptr;
        SurfaceColorFn g_text_color = nullptr;
        SurfacePosFn g_text_pos = nullptr;
        SurfacePrintFn g_print_text = nullptr;
        SurfaceTextureFileFn g_texture_file = nullptr;
        SurfaceSetTextureFn g_set_texture = nullptr;
        SurfaceNewTextureFn g_new_texture = nullptr;
        SurfaceScreenSizeFn g_screen_size = nullptr;
        SurfaceTextSizeFn g_text_size = nullptr;
        SurfaceSoundFn g_play_sound = nullptr;
        SurfaceSubRectFn g_sub_rect = nullptr;
        SurfacePolygonFn g_polygon = nullptr;
        SurfaceAlphaFn g_alpha_multiplier = nullptr; // (optional: the scoreboard line's fade)

        core::Hook<PaintTraverseFn> g_paint_traverse;
        std::atomic<unsigned> g_overlay_panel{0}; // FocusOverlayPanel's VPANEL, once seen
        std::atomic<ULONGLONG> g_last_paint{0};
        bool g_broken = false; // a fault while drawing: the HUD goes back to ImGui

        // Main thread (paint): the client scheme's fonts at the current screen size (proportional fonts are made
        // again when it changes), and the texture id of each material drawn.
        struct FontSpec
        {
            const char* name;
            int fallback_tall; // a line's height when the font is missing
        };
        constexpr FontSpec k_fonts[] = {{"PlayerDisplayName", 30}, {"HudHintText", 26}, {"HudAmmoSmall", 26}, {"MessageText", 22}};
        static_assert(sizeof(k_fonts) / sizeof(k_fonts[0]) == static_cast<std::size_t>(Font::Count));
        unsigned g_fonts[static_cast<int>(Font::Count)] = {};
        int g_fonts_w = 0, g_fonts_h = 0;
        std::map<std::string, int> g_textures;

        bool slot_is(void* object, int slot, const char* prologue)
        {
            const std::uintptr_t fn = mem::vslot(object, slot);
            return fn != 0 && mem::find_pattern(fn, 16, prologue) == fn;
        }

        bool vtable_is(void* object, std::uintptr_t module, std::uintptr_t rva)
        {
            return object != nullptr && mem::read_or<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(object)) == module + rva;
        }

        template <class Fn>
        Fn slot_fn(void* object, int slot)
        {
            return reinterpret_cast<Fn>(mem::vslot(object, slot));
        }

        // The scheme's fonts: looked up again when the screen's size changed (a resolution change remakes the
        // proportional ones), and while one is still missing (the client's scheme is loaded with client.dll) every
        // couple of seconds: a custom HUD's clientscheme.res may not have one at all, and asking (and logging) every
        // frame would go on for the whole game.  A font is logged when its handle changes.
        void resolve_fonts()
        {
            constexpr ULONGLONG k_retry_ms = 2000;
            static ULONGLONG last_try = 0;
            int w = 0, h = 0;
            g_screen_size(g_surface, w, h);
            bool missing = false;
            for (unsigned font : g_fonts)
                missing |= font == 0;
            const bool resized = w != g_fonts_w || h != g_fonts_h;
            const ULONGLONG now = GetTickCount64();
            if (!resized && (!missing || now - last_try < k_retry_ms))
                return;
            last_try = now;
            g_fonts_w = w;
            g_fonts_h = h;
            const unsigned handle = g_get_scheme(g_schemes, "ClientScheme");
            void* scheme = handle != 0 ? g_get_ischeme(g_schemes, handle) : nullptr;
            const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"vgui2.dll"));
            if (scheme == nullptr || !vtable_is(scheme, module, k_rva_scheme_vtable) ||
                !slot_is(scheme, k_vt_scheme_get_font, "55 8B EC 56 8B F1 8D 4E 18 E8") ||
                !slot_is(scheme, k_vt_scheme_font_name, "55 8B EC 8B 41 5C 48 56 57 78"))
            {
                INV_LOG("vgui: the client scheme (%u, %p) does not look as expected: no fonts", handle, scheme);
                return;
            }
            const auto get_font = slot_fn<SchemeFontFn>(scheme, k_vt_scheme_get_font);
            const auto font_name = slot_fn<SchemeFontNameFn>(scheme, k_vt_scheme_font_name);
            for (int i = 0; i < static_cast<int>(Font::Count); ++i)
            {
                unsigned font = get_font(scheme, k_fonts[i].name, true);
                if (font == 0)
                    font = get_font(scheme, k_fonts[i].name, false);
                if (font == g_fonts[i] && !resized)
                    continue;
                g_fonts[i] = font;
                INV_LOG("vgui: font %s -> %u (%s), screen %dx%d", k_fonts[i].name, font, font != 0 ? font_name(scheme, font) : "-",
                        w, h);
            }
        }

        std::wstring widen(const std::string& text)
        {
            if (text.empty())
                return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<std::size_t>(std::max(n, 0)), L'\0');
            if (n > 0)
                MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
            return out;
        }

        void set_color(std::uint32_t c)
        {
            g_set_color(g_surface, static_cast<int>(c & 255u), static_cast<int>((c >> 8) & 255u),
                        static_cast<int>((c >> 16) & 255u), static_cast<int>(c >> 24));
        }

        // The texture id of a material: one of the mod's own, made the first time.  Never one the game already bound
        // to it (DrawGetTextureId): that id belongs to a panel, which may destroy it (DestroyTextureID) when it goes,
        // on a map change or a disconnect, and the slot can then be given to another texture (an avatar's RGBA) while
        // the HUD still draws with it.  Each load of the mod makes a few new ids; the game never frees them, which
        // costs a dictionary entry per material per reload.
        int texture(const std::string& material)
        {
            if (const auto known = g_textures.find(material); known != g_textures.end())
                return known->second;
            const int id = g_new_texture(g_surface, false);
            g_texture_file(g_surface, id, material.c_str(), 1, false);
            g_textures[material] = id;
            return id;
        }

        void paint()
        {
            resolve_fonts();
            hud::paint_vgui();
            // then the screamer and the screen effects over all of it, into the same frame (queued after the HUD's own
            // drawing on the render thread), so that a stream's capture has them
            ui::overlay::queue_in_frame_pass();
        }

        // The scoreboard's mutators line.  The scoreboard's title label ("MissionTitle" of Resource/UI/ScoreBoard.res:
        // "Dead Center, Normal") is found as it paints: its screen rectangle, and the alpha multiplier the scoreboard
        // set for its children from its fade (1 when it shows fully, less while it fades in or out).  The line is
        // drawn when the outermost PaintTraverse (the root's) returns: still inside the surface's StartDrawing, with the
        // root's (the whole screen's) context, and after every panel.  Drawn in the HUD's pass (after FocusOverlayPanel)
        // or right after the title, it was under something the game paints over the top of the screen later in the
        // frame (seen 2026-10-20: dimmed, with the multiplier at 1).  The multiplier is 1 again there, so the fade is
        // the title's, applied by hand.
        struct Title
        {
            int x = 0, y = 0, w = 0, h = 0;
            float alpha = 1.f;
            bool pending = false; // painted in this frame, the line not drawn yet
        } g_title;
        int g_depth = 0; // PaintTraverse calls under way (main thread)

        void find_title(void* panels, unsigned vpanel)
        {
            Title title;
            g_panel_abs_pos(panels, vpanel, title.x, title.y);
            g_panel_size(panels, vpanel, title.w, title.h);
            title.alpha = g_alpha_multiplier != nullptr ? g_alpha_multiplier(g_surface) : 1.f;
            title.pending = true;
            g_title = title;
        }

        bool scoreboard_guarded(void* panels, unsigned vpanel, bool draw)
        {
            __try
            {
                if (draw)
                    hud::paint_scoreboard(g_title.x, g_title.y, g_title.w, g_title.h, g_title.alpha);
                else
                    find_title(panels, vpanel);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // (a separate function: __try cannot share one with objects that need unwinding)
        bool paint_guarded()
        {
            __try
            {
                paint();
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        void __fastcall hk_paint_traverse(void* panels, void* edx, unsigned vpanel, bool force_repaint, bool allow_force)
        {
            ++g_depth;
            g_paint_traverse.original()(panels, edx, vpanel, force_repaint, allow_force);
            --g_depth;
            // the scoreboard's title: only painted while the scoreboard shows (every panel's name is looked at: a
            // VPANEL may be made again, and a name is one virtual call); the line once the root has painted
            if (g_panel_size != nullptr && g_panel_abs_pos != nullptr && !g_broken && g_fonts_w != 0)
            {
                bool ok = true;
                if (g_depth > 0)
                {
                    const char* name = nullptr;
                    mem::guarded([&] { name = g_panel_name(panels, vpanel); });
                    if (name != nullptr && name[0] == 'M' && std::strcmp(name, "MissionTitle") == 0)
                        ok = scoreboard_guarded(panels, vpanel, false);
                }
                else if (g_title.pending)
                {
                    g_title.pending = false;
                    ok = scoreboard_guarded(panels, vpanel, true);
                }
                if (!ok)
                {
                    g_panel_size = nullptr; // (no more tries)
                    INV_LOG("vgui: fault while painting the scoreboard's mutators line: it is off");
                }
            }
            unsigned overlay = g_overlay_panel.load(std::memory_order_relaxed);
            if (overlay == 0)
            {
                const char* name = nullptr;
                mem::guarded([&] { name = g_panel_name(panels, vpanel); });
                if (name == nullptr || std::strcmp(name, "FocusOverlayPanel") != 0)
                    return;
                overlay = vpanel;
                g_overlay_panel = vpanel;
                INV_LOG("vgui: FocusOverlayPanel is %08X: the HUD is drawn after it", vpanel);
            }
            if (vpanel != overlay || g_broken)
                return;
            g_last_paint = GetTickCount64();
            if (!paint_guarded())
            {
                g_broken = true;
                g_last_paint = 0;
                INV_LOG("vgui: fault while painting: the HUD goes back to the overlay");
            }
        }
    } // namespace

    bool install()
    {
        const auto vgui2 = GetModuleHandleW(L"vgui2.dll");
        const auto matsurface = GetModuleHandleW(L"vguimatsurface.dll");
        const auto vgui_create = vgui2 != nullptr ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(vgui2, "CreateInterface")) : nullptr;
        const auto surface_create =
            matsurface != nullptr ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(matsurface, "CreateInterface")) : nullptr;
        if (vgui_create == nullptr || surface_create == nullptr)
        {
            INV_LOG("vgui: no vgui2.dll / vguimatsurface.dll: the HUD is drawn by the overlay");
            return false;
        }
        const auto vgui_base = reinterpret_cast<std::uintptr_t>(vgui2);
        const auto surface_base = reinterpret_cast<std::uintptr_t>(matsurface);
        void* panels = vgui_create("VGUI_Panel009", nullptr);
        void* schemes = vgui_create("VGUI_Scheme010", nullptr);
        void* surface = surface_create("VGUI_Surface031", nullptr);
        const std::uintptr_t paint_traverse = vgui_base + k_rva_panel_paint_traverse;
        const bool ok =
            vtable_is(panels, vgui_base, k_rva_panel_vtable) && vtable_is(schemes, vgui_base, k_rva_scheme_manager_vtable) &&
            vtable_is(surface, surface_base, k_rva_surface_vtable) &&
            mem::vslot(panels, k_vt_panel_get_name) == vgui_base + k_rva_panel_get_name &&
            slot_is(panels, k_vt_panel_get_name, "55 8B EC 8B 4D 08 8B 01 8B 90 8C 00 00 00 FF D2") &&
            mem::vslot(panels, k_vt_panel_paint_traverse) == paint_traverse &&
            slot_is(panels, k_vt_panel_paint_traverse, "55 8B EC 8B 01 8B 55 08 8B 80 04 01 00 00 52 FF") &&
            slot_is(schemes, k_vt_schemes_get_scheme, "55 8B EC 53 56 57 8B F9 BE 01 00 00 00 39 77 10") &&
            slot_is(schemes, k_vt_schemes_get_ischeme, "55 8B EC 8B 45 08 3B 41 10 72 06 33 C0") &&
            slot_is(surface, k_vt_surface_set_color, "55 8B EC 8A 45 08 8A 55 0C") &&
            slot_is(surface, k_vt_surface_filled_rect, "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00") &&
            slot_is(surface, k_vt_surface_text_font, "55 8B EC 8B 45 08 89 81 94 02 00 00") &&
            slot_is(surface, k_vt_surface_text_color, "55 8B EC 53 56 8B F1 0F B6 46 28") &&
            slot_is(surface, k_vt_surface_text_pos, "55 8B EC 8B 45 08 8B 55 0C 89 41 2C 89 51 30") &&
            slot_is(surface, k_vt_surface_print_text, "55 8B EC 8B 45 10 8B 55 0C 50 8B 45 08 6A 00 52") &&
            slot_is(surface, k_vt_surface_texture_file, "55 8B EC 56 57 8B F1 E8 ?? ?? ?? ?? 8B 4D 0C") &&
            slot_is(surface, k_vt_surface_set_texture, "55 8B EC 56 57 8B 7D 08 8B F1 3B BE A0 02 00 00") &&
            slot_is(surface, k_vt_surface_new_texture, "55 8B EC E8 ?? ?? ?? ?? 8B 10 8B C8 8B 12 5D FF") &&
            slot_is(surface, k_vt_surface_screen_size, "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00") &&
            slot_is(surface, k_vt_surface_text_size, "55 8B EC 8B 45 14 8B 4D 10 8B 55 0C 50 8B 45 08") &&
            slot_is(surface, k_vt_surface_play_sound, "55 8B EC 8B 81 D0 02 01 00 85 C0") &&
            slot_is(surface, k_vt_surface_textured_sub_rect, "55 8B EC 83 EC 50 56 8B F1 80 7E 27 00") &&
            slot_is(surface, k_vt_surface_textured_polygon, "55 8B EC 51 8B 45 08 56 8B F1 85 C0");
        if (!ok)
        {
            INV_LOG("vgui: the game's VGUI (panels %p, schemes %p, surface %p) does not look as expected: the HUD is "
                    "drawn by the overlay",
                    panels, schemes, surface);
            return false;
        }
        g_panels = panels;
        g_schemes = schemes;
        g_surface = surface;
        g_panel_name = slot_fn<PanelNameFn>(panels, k_vt_panel_get_name);
        // (optional: only the scoreboard's mutators line needs them)
        if (slot_is(surface, k_vt_surface_get_alpha_multiplier, "D9 41 10 C3"))
            g_alpha_multiplier = slot_fn<SurfaceAlphaFn>(surface, k_vt_surface_get_alpha_multiplier);
        if (slot_is(panels, k_vt_panel_get_size, "55 8B EC 8B 55 10 8B 4D 08 8B 01 8B 40 34 52 8B") &&
            slot_is(panels, k_vt_panel_get_abs_pos, "55 8B EC 8B 55 10 8B 4D 08 8B 01 8B 40 48 52 8B"))
        {
            g_panel_size = slot_fn<PanelRectFn>(panels, k_vt_panel_get_size);
            g_panel_abs_pos = slot_fn<PanelRectFn>(panels, k_vt_panel_get_abs_pos);
        }
        else
            INV_LOG("vgui: IPanel GetSize / GetAbsPos (slots 5, 10) do not look as expected: no mutators on the scoreboard");
        g_get_scheme = slot_fn<GetSchemeFn>(schemes, k_vt_schemes_get_scheme);
        g_get_ischeme = slot_fn<GetISchemeFn>(schemes, k_vt_schemes_get_ischeme);
        g_set_color = slot_fn<SurfaceColorFn>(surface, k_vt_surface_set_color);
        g_filled_rect = slot_fn<SurfaceRectFn>(surface, k_vt_surface_filled_rect);
        g_text_font = slot_fn<SurfaceFontFn>(surface, k_vt_surface_text_font);
        g_text_color = slot_fn<SurfaceColorFn>(surface, k_vt_surface_text_color);
        g_text_pos = slot_fn<SurfacePosFn>(surface, k_vt_surface_text_pos);
        g_print_text = slot_fn<SurfacePrintFn>(surface, k_vt_surface_print_text);
        g_texture_file = slot_fn<SurfaceTextureFileFn>(surface, k_vt_surface_texture_file);
        g_set_texture = slot_fn<SurfaceSetTextureFn>(surface, k_vt_surface_set_texture);
        g_new_texture = slot_fn<SurfaceNewTextureFn>(surface, k_vt_surface_new_texture);
        g_screen_size = slot_fn<SurfaceScreenSizeFn>(surface, k_vt_surface_screen_size);
        g_text_size = slot_fn<SurfaceTextSizeFn>(surface, k_vt_surface_text_size);
        g_play_sound = slot_fn<SurfaceSoundFn>(surface, k_vt_surface_play_sound);
        g_sub_rect = slot_fn<SurfaceSubRectFn>(surface, k_vt_surface_textured_sub_rect);
        g_polygon = slot_fn<SurfacePolygonFn>(surface, k_vt_surface_textured_polygon);
        if (!g_paint_traverse.create("PaintTraverse", reinterpret_cast<void*>(paint_traverse), hk_paint_traverse) ||
            !g_paint_traverse.enable())
        {
            g_paint_traverse.remove();
            return false;
        }
        INV_LOG("vgui: ready (surface %p, PaintTraverse %p)", surface, reinterpret_cast<void*>(paint_traverse));
        return true;
    }

    void uninstall()
    {
        g_paint_traverse.remove();
        g_last_paint = 0;
    }

    bool painting()
    {
        const ULONGLONG last = g_last_paint.load();
        return last != 0 && GetTickCount64() - last < 250;
    }

    void screen_size(int& w, int& h)
    {
        g_screen_size(g_surface, w, h);
    }

    void rect(int x0, int y0, int x1, int y1, std::uint32_t colour)
    {
        set_color(colour);
        g_filled_rect(g_surface, x0, y0, x1, y1);
    }

    void polygon(const Vertex* vertices, int n, std::uint32_t colour)
    {
        if (n < 3)
            return;
        thread_local std::vector<SurfaceVertex> points;
        points.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            points[static_cast<std::size_t>(i)] = SurfaceVertex{vertices[i].x, vertices[i].y, 0.5f, 0.5f};
        g_set_texture(g_surface, texture("vgui/white"));
        set_color(colour);
        g_polygon(g_surface, n, points.data(), true);
    }

    int font_tall(Font font)
    {
        const unsigned handle = g_fonts[static_cast<int>(font)];
        if (handle == 0)
            return k_fonts[static_cast<int>(font)].fallback_tall;
        int w = 0, h = 0;
        g_text_size(g_surface, handle, L"Ag", w, h);
        return h;
    }

    int text_width(Font font, const std::string& text)
    {
        const unsigned handle = g_fonts[static_cast<int>(font)];
        if (handle == 0 || text.empty())
            return 0;
        const std::wstring wide = widen(text);
        int w = 0, h = 0;
        g_text_size(g_surface, handle, wide.c_str(), w, h);
        return w;
    }

    void text(Font font, int x, int y, std::uint32_t colour, const std::string& text)
    {
        const unsigned handle = g_fonts[static_cast<int>(font)];
        if (handle == 0 || text.empty())
            return;
        const std::wstring wide = widen(text);
        g_text_font(g_surface, handle);
        g_text_color(g_surface, static_cast<int>(colour & 255u), static_cast<int>((colour >> 8) & 255u),
                     static_cast<int>((colour >> 16) & 255u), static_cast<int>(colour >> 24));
        g_text_pos(g_surface, x, y);
        g_print_text(g_surface, wide.c_str(), static_cast<int>(wide.size()), 0);
    }

    void image(const std::string& material, int x0, int y0, int x1, int y1, float u0, float v0, float u1, float v1,
               std::uint32_t colour)
    {
        g_set_texture(g_surface, texture(material));
        set_color(colour);
        g_sub_rect(g_surface, x0, y0, x1, y1, u0, v0, u1, v1);
    }

    void play_sound(const char* file)
    {
        if (g_play_sound != nullptr && g_surface != nullptr && !g_broken)
            mem::guarded([&] { g_play_sound(g_surface, file); });
    }

} // namespace ui::vgui
