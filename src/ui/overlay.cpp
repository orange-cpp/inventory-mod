#include "ui/overlay.hpp"

#include "core/config.hpp"
#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/mem.hpp"
#include "ui/audio.hpp"
#include "ui/icons.hpp"
#include "ui/hud.hpp"
#include "ui/postfx.hpp"
#include "ui/vgui.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <d3d9.h>
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>
#include <imgui_nullcore/imgui_nullcore.hpp>
#include <intrin.h>

namespace nc = imgui_nullcore;

// (imgui_impl_win32.h keeps the declaration out of the header)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace ui::overlay
{

    namespace
    {
        using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
        using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
        using SetCursorPosFn = BOOL(WINAPI*)(int, int);
        using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);

        core::Hook<PresentFn> g_present;
        core::Hook<ResetFn> g_reset;
        core::Hook<SetCursorPosFn> g_set_cursor_pos;
        core::Hook<GetCursorPosFn> g_get_cursor_pos;

        IDirect3DDevice9* g_device = nullptr;
        IDirect3DStateBlock9* g_state = nullptr;
        HWND g_hwnd = nullptr;
        WNDPROC g_orig_wndproc = nullptr;
        bool g_ready = false; // ImGui is up (render thread only)
        bool g_broken = false; // a fault in a frame: the overlay stays off
        std::atomic<bool> g_capture{false}; // the game's cursor handling is overridden
        std::atomic<int> g_mouse_x{-1}, g_mouse_y{-1}; // the last WM_MOUSEMOVE, client coordinates
        std::atomic<bool> g_shutdown_requested{false};
        std::atomic<bool> g_shutdown_done{false};

        // ---- the D3D9 vtable ----------------------------------------------------------------------------------------

        constexpr int k_vtbl_reset = 16;
        constexpr int k_vtbl_present = 17;

        // Makes a throwaway device and reads the vtable of IDirect3DDevice9 (shared by every device of d3d9.dll).
        bool find_device_methods(void** present, void** reset)
        {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"inventory_mod_dummy";
            RegisterClassExW(&wc);
            HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr,
                                        wc.hInstance, nullptr);
            if (hwnd == nullptr)
                return false;

            bool ok = false;
            IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
            if (d3d != nullptr)
            {
                D3DPRESENT_PARAMETERS pp{};
                pp.Windowed = TRUE;
                pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
                pp.BackBufferFormat = D3DFMT_UNKNOWN;
                pp.hDeviceWindow = hwnd;
                IDirect3DDevice9* device = nullptr;
                const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp,
                                                     &device);
                if (SUCCEEDED(hr) && device != nullptr)
                {
                    void** vtbl = *reinterpret_cast<void***>(device);
                    *present = vtbl[k_vtbl_present];
                    *reset = vtbl[k_vtbl_reset];
                    device->Release();
                    ok = true;
                }
                else
                    INV_LOG("dummy D3D9 device failed: 0x%08lX", static_cast<unsigned long>(hr));
                d3d->Release();
            }
            DestroyWindow(hwnd);
            UnregisterClassW(wc.lpszClassName, wc.hInstance);
            return ok;
        }

        // ---- the cursor ---------------------------------------------------------------------------------------------
        // While the inventory is open the game must not recentre or read the cursor (it would spin the view and pin the
        // mouse to the middle of the window), but our own ImGui code still needs the real one.

        bool called_from_self(void* return_address)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(&__ImageBase);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
            const auto address = reinterpret_cast<std::uintptr_t>(return_address);
            return address >= base && address < base + nt->OptionalHeader.SizeOfImage;
        }

        BOOL WINAPI hk_set_cursor_pos(int x, int y)
        {
            if (g_capture.load(std::memory_order_relaxed) && !called_from_self(_ReturnAddress()))
                return TRUE;
            return g_set_cursor_pos.original()(x, y);
        }

        BOOL WINAPI hk_get_cursor_pos(LPPOINT point)
        {
            const BOOL result = g_get_cursor_pos.original()(point);
            if (result && point != nullptr && g_capture.load(std::memory_order_relaxed) && g_hwnd != nullptr &&
                !called_from_self(_ReturnAddress()))
            {
                RECT rc{};
                if (GetClientRect(g_hwnd, &rc))
                {
                    POINT center{(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
                    ClientToScreen(g_hwnd, &center);
                    *point = center; // the game sees a cursor that never moved
                }
            }
            return result;
        }

        // ---- window messages ----------------------------------------------------------------------------------------

        bool is_input_message(UINT msg)
        {
            return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
                   msg == WM_INPUT || msg == WM_MOUSEHWHEEL;
        }

        LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        {
            // the game's window procedure runs on its main thread: the backend's requests are carried out here
            if (msg == game::k_msg_pump)
            {
                game::backend().pump();
                return 0;
            }
            if (msg == game::k_msg_unload)
            {
                hud::request_unload();
                return 0;
            }
            if (msg == WM_MOUSEMOVE)
            {
                g_mouse_x = static_cast<int>(static_cast<short>(LOWORD(lparam)));
                g_mouse_y = static_cast<int>(static_cast<short>(HIWORD(lparam)));
            }
            if (g_ready && !g_broken)
            {
                if (hud::on_wnd_message(msg, wparam, lparam))
                    return 0;
                if (hud::wants_input())
                {
                    // The cursor is ours while the window is open: a leave (the pointer sits at the window's edge, or
                    // the cursor is elsewhere) must not make ImGui forget where it is.
                    const bool keyboard = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
                    // the radial wheel takes only the mouse: the player keeps walking with the keyboard
                    if (keyboard && !hud::wants_keyboard())
                        return CallWindowProcW(g_orig_wndproc, hwnd, msg, wparam, lparam);
                    if (msg != WM_MOUSELEAVE)
                        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
                    if (is_input_message(msg))
                        return 0; // the game never sees it
                }
            }
            return CallWindowProcW(g_orig_wndproc, hwnd, msg, wparam, lparam);
        }

        // ---- the infection under the game's HUD ----------------------------------------------------------------------
        //
        // With [infection] cover_game_hud = false (settings) the infection's effect goes on the game's 3D picture only
        // (and the same for the stamina's vignette with [stamina] cover_game_hud = false):
        // it runs from the game's own rendering, just before the game draws its HUD.  client.dll's CTerrorViewRender
        // (RTTI; vtable 0x105C1424) renders a view in slot 6 (0x101D6D80: "2D Client Rendering", Push2DView, then slot 37,
        // then "VGui_DrawHud" when the HUD is drawn); slot 37 is Render2DEffectsPreHUD(const CViewSetup&) (thiscall, ret
        // 4; CViewRender's own is an empty `ret 4` at 0x101CA100, CCSViewRender's 0x1022ECD0 jumps to 0x1022E5D0, the
        // night vision).  The mod puts its own function in that slot: the original runs, then the pass.
        //
        // That runs on the game's main thread, and with queued rendering (mat_queue_mode) the D3D work happens on the
        // render thread later, in order: the pass is queued there as a CFunctor.  materialsystem.dll VMaterialSystem080
        // (CMaterialSystem, vtable 0x1009C824) slot 96 GetRenderContext (0x1000D960: the thread's render context, a
        // reference added); the context's slot 142 GetCallQueue (the queued context's 0x10023CD0 `lea eax, [ecx+2A4h]`:
        // its CCallQueueExternal, vtable 0x1009A9F8; the plain context's 0x10022990 `xor eax, eax`: none, the pass runs
        // right away) and slot 1 Release; the queue's slot 0 QueueFunctorInternal(CFunctor*) (0x10010600).  A CFunctor's
        // vtable (CMemberFunctor1's 0x1009C3B4): 0 AddRef, 1 Release, 2 the deleting destructor, 3 operator().  The mod's
        // functor is one static object whose reference count does nothing.

        constexpr std::uintptr_t k_rva_terror_view_render_vtable = 0x5C1424;
        constexpr int k_vt_render_2d_effects_pre_hud = 37;
        constexpr std::uintptr_t k_rva_pre_hud = 0x22ECD0;
        constexpr int k_vt_get_render_context = 96;
        constexpr int k_vt_context_release = 1;
        constexpr int k_vt_context_get_call_queue = 142;

        using PreHudFn = void(__fastcall*)(void* self, void* edx, const void* view);
        using GetRenderContextFn = void*(__thiscall*)(void* materials);
        using ContextFn = void*(__thiscall*)(void* context);
        using QueueFunctorFn = void(__thiscall*)(void* queue, void* functor);
        using CreateInterfaceFn = void*(__cdecl*)(const char* name, int* return_code);

        void** g_pre_hud_slot = nullptr; // the vtable entry (in client.dll's read-only data)
        PreHudFn g_pre_hud_original = nullptr;
        void* g_materials = nullptr;
        IDirect3DStateBlock9* g_pre_hud_state = nullptr; // render thread

        void release_state_block()
        {
            if (g_state != nullptr)
            {
                g_state->Release();
                g_state = nullptr;
            }
            if (g_pre_hud_state != nullptr)
            {
                g_pre_hud_state->Release();
                g_pre_hud_state = nullptr;
            }
        }

        // The render thread (or the main thread, without queued rendering), just before the game draws its HUD.
        void under_game_hud()
        {
            IDirect3DDevice9* device = g_device;
            if (!g_ready || g_broken || device == nullptr || g_shutdown_requested.load())
                return;
            if (g_pre_hud_state == nullptr && FAILED(device->CreateStateBlock(D3DSBT_ALL, &g_pre_hud_state)))
                return;
            IDirect3DSurface9* target = nullptr;
            if (FAILED(device->GetRenderTarget(0, &target)) || target == nullptr)
                return;
            g_pre_hud_state->Capture();
            device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE); // (as at Present: the picture is already in gamma)
            device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                               D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
            postfx::render(device, target, postfx::Layer::UnderGameHud);
            g_pre_hud_state->Apply();
            target->Release();
        }

        void under_game_hud_guarded()
        {
            if (!game::mem::guarded([] { under_game_hud(); }))
            {
                g_broken = true;
                INV_LOG("fault in the pass under the game's HUD: overlay disabled");
            }
        }

        // the functor the render thread runs
        int __fastcall functor_add_ref(void*, void*) { return 1; }
        int __fastcall functor_release(void*, void*) { return 1; }
        void* __fastcall functor_destroy(void* self, void*, int) { return self; }
        void __fastcall functor_run(void*, void*) { under_game_hud_guarded(); }
        void* g_functor_vtable[4] = {reinterpret_cast<void*>(functor_add_ref), reinterpret_cast<void*>(functor_release),
                                     reinterpret_cast<void*>(functor_destroy), reinterpret_cast<void*>(functor_run)};
        struct Functor
        {
            void** vtable;
        } g_functor{g_functor_vtable};

        void __fastcall hk_pre_hud(void* self, void* edx, const void* view)
        {
            g_pre_hud_original(self, edx, view);
            if (!postfx::wants_under_game_hud() || g_materials == nullptr || g_shutdown_requested.load())
                return;
            game::mem::guarded([] {
                const auto get_context = reinterpret_cast<GetRenderContextFn>(game::mem::vslot(g_materials, k_vt_get_render_context));
                void* context = get_context(g_materials);
                if (context == nullptr)
                    return;
                const std::uintptr_t get_queue = game::mem::vslot(context, k_vt_context_get_call_queue);
                // the two GetCallQueue there are: the queued context's (its queue) and the plain one's (none)
                const bool queued = game::mem::find_pattern(get_queue, 7, "8D 81 A4 02 00 00 C3") == get_queue;
                const bool plain = game::mem::find_pattern(get_queue, 3, "33 C0 C3") == get_queue;
                void* queue = queued ? reinterpret_cast<ContextFn>(get_queue)(context) : nullptr;
                if (queue != nullptr)
                    reinterpret_cast<QueueFunctorFn>(game::mem::vslot(queue, 0))(queue, &g_functor);
                else if (plain)
                    under_game_hud_guarded();
                reinterpret_cast<ContextFn>(game::mem::vslot(context, k_vt_context_release))(context);
            });
        }

        // Main thread or render thread, once: the slot and the material system, checked.
        void install_pre_hud()
        {
            const auto client = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"client.dll"));
            HMODULE materials_dll = GetModuleHandleW(L"materialsystem.dll");
            const auto create = materials_dll != nullptr
                                    ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(materials_dll, "CreateInterface"))
                                    : nullptr;
            void* materials = create != nullptr ? create("VMaterialSystem080", nullptr) : nullptr;
            void** slot = client != 0 ? reinterpret_cast<void**>(client + k_rva_terror_view_render_vtable +
                                                                  4u * k_vt_render_2d_effects_pre_hud)
                                      : nullptr;
            const std::uintptr_t pre_hud = slot != nullptr ? game::mem::read_or<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(slot)) : 0;
            const std::uintptr_t get_context = materials != nullptr ? game::mem::vslot(materials, k_vt_get_render_context) : 0;
            const std::uintptr_t queue_vtable = materials_dll != nullptr ? reinterpret_cast<std::uintptr_t>(materials_dll) + 0x9A9F8 : 0;
            const std::uintptr_t queue_functor = queue_vtable != 0 ? game::mem::read_or<std::uintptr_t>(queue_vtable) : 0;
            if (pre_hud == 0 || pre_hud != client + k_rva_pre_hud || game::mem::find_pattern(pre_hud, 5, "55 8B EC 5D E9") != pre_hud ||
                get_context == 0 || game::mem::find_pattern(get_context, 5, "56 57 8B F9 B9") != get_context || queue_functor == 0 ||
                game::mem::find_pattern(queue_functor, 11, "55 8B EC 56 8B 75 08 8B 06 8B 10") != queue_functor)
            {
                INV_LOG("overlay: the game's rendering does not look as expected: the infection always covers the game's HUD");
                return;
            }
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
                return;
            g_materials = materials;
            g_pre_hud_original = reinterpret_cast<PreHudFn>(pre_hud);
            g_pre_hud_slot = slot;
            *slot = reinterpret_cast<void*>(hk_pre_hud);
            VirtualProtect(slot, sizeof(void*), old, &old);
            INV_LOG("overlay: the infection can go under the game's HUD (Render2DEffectsPreHUD %p, materials %p)",
                    reinterpret_cast<void*>(pre_hud), materials);
        }

        void uninstall_pre_hud()
        {
            if (g_pre_hud_slot == nullptr)
                return;
            DWORD old = 0;
            if (VirtualProtect(g_pre_hud_slot, sizeof(void*), PAGE_READWRITE, &old))
            {
                *g_pre_hud_slot = reinterpret_cast<void*>(g_pre_hud_original);
                VirtualProtect(g_pre_hud_slot, sizeof(void*), old, &old);
            }
            g_pre_hud_slot = nullptr;
        }

        // ---- Options > Inventory Mod ----------------------------------------------------------------------------------
        //
        // The game's Options flyout (main menu and in-game menu: resource/UI/L4D360UI/OptionsFlyout.res) gets an
        // "Inventory Mod" button from the mod's addon inventory_mod_menu.vpk (tools/make_menu_addon.py; the mod mounts
        // its addons ahead of the game's files).  Its command "InventoryModSettings" reaches the menus' OnCommand like
        // "Video" or "Audio" do: GameUI.dll (bin), L4D360UI::MainMenu (RTTI; vtable 0x1017AF1C) and
        // L4D360UI::InGameMainMenu (0x10178154), slot 96 OnCommand(const char*) (thiscall, ret 4: 0x10068220 and
        // 0x1005EE40, which compare it with "Video", "Audio", "KeyboardMouse", "SteamController", "CloudSettings" ...).
        // The mod puts its function in both slots: that command opens its window on the Settings tab, the rest go on.

        constexpr const char* k_settings_command = "InventoryModSettings";
        constexpr int k_vt_menu_on_command = 96;
        using OnCommandFn = void(__fastcall*)(void* self, void* edx, const char* command);

        struct MenuHook
        {
            std::uintptr_t vtable_rva;
            std::uintptr_t function_rva;
            const char* prologue;
            void** slot = nullptr;
            OnCommandFn original = nullptr;
        };
        MenuHook g_menu_hooks[2] = {
            {0x17AF1C, 0x68220, "55 8B EC 81 EC CC 01 00 00"}, // MainMenu
            {0x178154, 0x5EE40, "55 8B EC 81 EC C0 00 00 00"}, // InGameMainMenu
        };

        bool patch_slot(void** slot, void* value)
        {
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
                return false;
            *slot = value;
            VirtualProtect(slot, sizeof(void*), old, &old);
            return true;
        }

        // ---- the page: GameUI's AudioVideo, taken over ----
        //
        // The page is a real page of the game's menu: L4D360UI::AudioVideo (RTTI; vtable 0x1016908C), the Xbox build's
        // Audio / Video page, which the PC menu never opens.  Its layout is the mod's (resource/UI/L4D360UI/AudioVideo.res
        // in inventory_mod_menu.vpk: a drop-down per setting, as on the Video page).  Its own code, which looks for its
        // own controls, is switched off: of the slots where it differs from its base L4D360UI::CL4DFrame (vtable
        // 0x101636AC), these get the base's back or the mod's:
        //    88 0x10025A00 stays: the base's 0x1000F660, then SetPaintBackgroundEnabled(true) (vgui slot 71, flag 8 at
        //       +0x60) and the frame's layout 0x1000F360, as the Video page's 0x10074810 starts.  Without the
        //       background the dark band behind the controls (ImgBackground) is not drawn.
        //    96 OnCommand 0x10025430 -> the mod's: "InvMod <key> <value>" (a choice), "InventoryModWindow", "Back";
        //       the rest to the base 0x100FB8B0 (vgui Frame's: "Close", "CloseModal" ...)
        //   101 0x10025220 -> the mod's: the base's 0x100FC680 (the layout loaded, the controls made), then the values
        //       (its own looks up SldBrightness, DrpColorMode ... by name)
        //   121 OnKeyCodePressed 0x10025EE0 -> 0x1000EB80 (B / Escape: back; its own asks to restart for a language)
        //   127 0x100259E0 -> the mod's: the title, through 0x1000F8C0(title, 0, subtitle, 0, 0, 0, 0x7FFFFFFF)
        //       (thiscall, ret 1Ch)
        //   238 Activate 0x10025CA0 -> the mod's: the base's 0x100FA2E0, then each drop-down's current choice
        //   282 0x10025850 -> 0x1000F580 (it writes the video config when the page closes)
        // (28-30: its class name, 32: its destructor, are its own and stay.)
        // A drop-down: FindChildByName(name, false) 0x100CEB70 (thiscall, ret 8); DropDownMenu::SetCurrentSelection(
        // command) 0x1002D180 (ret 4: the choice whose command that is shows); its flyout 0x1002CD50 (ret), which gets the
        // page's FlyoutMenuListener (+0x2BC) through 0x1002DA00 (ret 4), as AudioVideo's own Activate does.
        // Opening it: CBaseModPanel::GetSingleton() 0x10009A90 (`mov eax, [0x101FFB10]`), then OpenWindow(const
        // WINDOW_TYPE&, caller, hideCaller, KeyValues*) 0x1000AC00 (thiscall); WT_AUDIOVIDEO is 3 (the in-game menu's
        // "AudioVideo" command).  Going back: as the Video page's "Back", the B button: 0x1000E950(0x73 KEY_XBUTTON_B,
        // the panel's user 0x10009B30) (cdecl), then the page's own slot 121 with that code.
        // The blur behind it: CBaseModPanel's per-frame update (0x1000D2D0) fades the game's view behind the menus in and
        // out through IVEngineClient slot 165 (engine 0x1005E3A0) -> the client's CHLClient slot 78 SetBlurFade
        // (0x100ABB60) -> TerrorClientMode +0x90, drawn with dev/fade_blur (0x101C7FE0).  Whether the active window
        // blurs comes from its type through a byte table at 0x1000D4CC (at 0x1000D35D: `cmp eax, 15h / ja / movzx ecx,
        // byte ptr [eax+0x1000D4CC] / jmp [ecx*4+0x1000D4C4]`; 0 no blur, 1 blur), where WT_NONE, WT_AUDIOVIDEO (3) and
        // 19-21 have 0.  While the page is the mod's, its entry is 1, as the Video page's (type 4) is.

        struct PageSetting
        {
            const char* key;
            enum
            {
                Bool,
                Int,
                Float
            } kind;
            std::vector<float> values; // Float: the page's choices (tools/make_menu_addon.py SETTINGS)
        };
        const PageSetting k_page_settings[] = {
            {"show_hotbar", PageSetting::Bool, {}},
            {"inventory_view", PageSetting::Int, {}},
            {"auto_store", PageSetting::Bool, {}},
            {"replace_native", PageSetting::Bool, {}},
            {"hud_scale", PageSetting::Float, {0.75f, 1.f, 1.25f, 1.5f, 2.f, 2.5f, 3.f}},
            {"infection_effect", PageSetting::Bool, {}},
            {"infection_cover_game_hud", PageSetting::Bool, {}},
            {"infection_cover_mod_hud", PageSetting::Bool, {}},
            {"infection_sound", PageSetting::Bool, {}},
            {"infection_screamer", PageSetting::Bool, {}},
            {"screamer_chance", PageSetting::Float, {0.f, 0.1f, 0.25f, 0.5f, 0.75f, 1.f}},
            {"stamina_bar", PageSetting::Bool, {}},
            {"stamina_vignette", PageSetting::Bool, {}},
            {"stamina_cover_game_hud", PageSetting::Bool, {}},
            {"stamina_cover_mod_hud", PageSetting::Bool, {}},
            {"key_toggle", PageSetting::Int, {}},
            {"key_prev", PageSetting::Int, {}},
            {"key_next", PageSetting::Int, {}},
            {"key_use", PageSetting::Int, {}},
            {"key_drop", PageSetting::Int, {}},
            {"key_store", PageSetting::Int, {}},
            {"key_pickup", PageSetting::Int, {}},
            {"key_unload", PageSetting::Int, {}},
            {"slots", PageSetting::Float, {4.f, 6.f, 8.f, 10.f, 12.f, 16.f, 20.f, 24.f}},
            {"weapon_slots", PageSetting::Float, {0.f, 1.f, 2.f, 3.f, 4.f, 6.f, 8.f}},
            {"max_weight", PageSetting::Float, {0.f, 10.f, 15.f, 20.f, 25.f, 30.f, 40.f, 50.f}},
        };

        // The setting's current value as the page's choice ("1", "0.25" ...).
        std::string page_value(const PageSetting& s)
        {
            const core::Config& c = core::config();
            const std::string_view k(s.key);
            float number = 0.f;
            if (k == "show_hotbar") number = c.show_hotbar;
            else if (k == "inventory_view") number = static_cast<float>(c.inventory_view);
            else if (k == "auto_store") number = c.auto_store;
            else if (k == "replace_native") number = c.replace_native;
            else if (k == "hud_scale") number = c.hud_scale;
            else if (k == "infection_effect") number = c.infection_effect;
            else if (k == "infection_cover_game_hud") number = c.infection_cover_game_hud;
            else if (k == "infection_cover_mod_hud") number = c.infection_cover_mod_hud;
            else if (k == "infection_sound") number = c.infection_sound;
            else if (k == "infection_screamer") number = c.infection_screamer;
            else if (k == "screamer_chance") number = c.screamer_chance;
            else if (k == "stamina_bar") number = c.stamina_bar;
            else if (k == "stamina_vignette") number = c.stamina_vignette;
            else if (k == "stamina_cover_game_hud") number = c.stamina_cover_game_hud;
            else if (k == "stamina_cover_mod_hud") number = c.stamina_cover_mod_hud;
            else if (k == "key_toggle") number = static_cast<float>(c.key_toggle);
            else if (k == "key_prev") number = static_cast<float>(c.key_prev);
            else if (k == "key_next") number = static_cast<float>(c.key_next);
            else if (k == "key_use") number = static_cast<float>(c.key_use);
            else if (k == "key_drop") number = static_cast<float>(c.key_drop);
            else if (k == "key_store") number = static_cast<float>(c.key_store);
            else if (k == "key_pickup") number = static_cast<float>(c.key_pickup);
            else if (k == "key_unload") number = static_cast<float>(c.key_unload);
            else if (k == "slots") number = static_cast<float>(c.slots);
            else if (k == "weapon_slots") number = static_cast<float>(c.weapon_slots);
            else if (k == "max_weight") number = c.max_weight;
            if (s.kind == PageSetting::Float && !s.values.empty())
                number = *std::min_element(s.values.begin(), s.values.end(),
                                           [&](float a, float b) { return std::fabs(a - number) < std::fabs(b - number); });
            char text[32];
            std::snprintf(text, sizeof(text), "%g", number);
            return text;
        }

        constexpr std::uintptr_t k_rva_page_vtable = 0x16908C; // AudioVideo
        constexpr std::uintptr_t k_rva_get_singleton = 0x9A90, k_rva_open_window = 0xAC00;
        constexpr std::uintptr_t k_rva_find_child = 0xCEB70, k_rva_set_selection = 0x2D180, k_rva_get_flyout = 0x2CD50,
                                 k_rva_set_listener = 0x2DA00, k_rva_set_title = 0xF8C0, k_rva_user = 0x9B30,
                                 k_rva_button_code = 0xE950, k_rva_base_command = 0xFB8B0, k_rva_base_activate = 0xFA2E0,
                                 k_rva_base_layout = 0xFC680;
        constexpr int k_wt_audio_video = 3;
        constexpr std::uintptr_t k_off_flyout_listener = 0x2BC;
        constexpr std::uintptr_t k_rva_blur_switch = 0xD35D, k_rva_blur_table = 0xD4CC;

        using GetSingletonFn = void*(__cdecl*)();
        using OpenWindowFn = void*(__thiscall*)(void* panel, const int* type, void* caller, bool hide_caller, void* params);
        using FindChildFn = void*(__thiscall*)(void* panel, const char* name, bool recurse);
        using SetSelectionFn = void(__thiscall*)(void* drop_down, const char* command);
        using GetFlyoutFn = void*(__thiscall*)(void* drop_down);
        using SetListenerFn = void(__thiscall*)(void* flyout, void* listener);
        using SetTitleFn = void(__thiscall*)(void* frame, const char* title, int, const char* subtitle, int, int, int, int);
        using UserFn = int(__thiscall*)(void* panel);
        using ButtonCodeFn = int(__cdecl*)(int code, int user);
        using KeyCodeFn = void(__thiscall*)(void* frame, int code);
        using ActivateFn = void(__thiscall*)(void* frame);

        std::uintptr_t g_gameui = 0;
        bool g_page_ready = false; // the AudioVideo vtable is the mod's
        struct PageSlot
        {
            int slot;
            std::uintptr_t own_rva; // what AudioVideo has there
            std::uintptr_t base_rva; // the base's (0: the mod's function)
            void* original = nullptr;
        };
        PageSlot g_page_slots[] = {
            {88, 0x25A00, 0x25A00}, {96, 0x25430, 0},       {101, 0x25220, 0xFC680}, {121, 0x25EE0, 0xEB80},
            {127, 0x259E0, 0},      {238, 0x25CA0, 0},      {282, 0x25850, 0xF580},
        };

        template <class F> F gameui_fn(std::uintptr_t rva) { return reinterpret_cast<F>(g_gameui + rva); }

        // The page's entry in the blur table (see above): the byte, and what it was (-1: not patched).  The table is
        // in .text, among the code: the page stays executable.
        std::uint8_t* g_page_blur = nullptr;
        int g_page_blur_old = -1;

        bool patch_byte(std::uint8_t* at, std::uint8_t value)
        {
            DWORD old = 0;
            if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old))
                return false;
            *at = value;
            VirtualProtect(at, 1, old, &old);
            return true;
        }

        void install_page_blur()
        {
            const std::uintptr_t at = g_gameui + k_rva_blur_switch;
            if (game::mem::find_pattern(at, 15, "83 F8 15 77 ?? 0F B6 88 ?? ?? ?? ?? FF 24 8D") != at ||
                game::mem::read_or<std::uintptr_t>(at + 8) != g_gameui + k_rva_blur_table ||
                game::mem::read_or<std::uint8_t>(g_gameui + k_rva_blur_table + k_wt_audio_video, 0xFF) > 1)
            {
                INV_LOG("overlay: GameUI's menu blur does not look as expected: the page has no blur behind it");
                return;
            }
            auto* entry = reinterpret_cast<std::uint8_t*>(g_gameui + k_rva_blur_table + k_wt_audio_video);
            const std::uint8_t old = *entry;
            if (patch_byte(entry, 1))
            {
                g_page_blur = entry;
                g_page_blur_old = old;
            }
        }

        void uninstall_page_blur()
        {
            if (g_page_blur != nullptr && g_page_blur_old >= 0)
                patch_byte(g_page_blur, static_cast<std::uint8_t>(g_page_blur_old));
            g_page_blur = nullptr;
            g_page_blur_old = -1;
        }

        void __fastcall page_title(void* self, void*)
        {
            game::mem::guarded([&] {
                gameui_fn<SetTitleFn>(k_rva_set_title)(self, "Inventory Mod", 0,
                                                       "The inventory mod's settings: its HUD, the infection and stamina.", 0,
                                                       0, 0, 0x7FFFFFFF);
            });
        }

        bool g_page_quiet = false; // main thread: the mod is setting the drop-downs (their own commands come back: ignored)

        // Each drop-down shows the setting's current choice.  SetCurrentSelection sends the choice's command to the page,
        // as a click does: those are ignored meanwhile.
        bool g_page_values_due = false; // main thread: the page opened, its values are not shown yet

        // true when the drop-downs were there
        bool page_show_values(void* self)
        {
            bool found = false;
            g_page_quiet = true;
            game::mem::guarded([&] {
                for (const PageSetting& s : k_page_settings)
                {
                    const std::string name = std::string("Drp_") + s.key;
                    void* drop_down = gameui_fn<FindChildFn>(k_rva_find_child)(self, name.c_str(), false);
                    if (drop_down == nullptr)
                        continue;
                    found = true;
                    const std::string command = std::string("InvMod ") + s.key + " " + page_value(s);
                    gameui_fn<SetSelectionFn>(k_rva_set_selection)(drop_down, command.c_str());
                    if (void* flyout = gameui_fn<GetFlyoutFn>(k_rva_get_flyout)(drop_down))
                        gameui_fn<SetListenerFn>(k_rva_set_listener)(flyout, static_cast<char*>(self) + k_off_flyout_listener);
                }
            });
            g_page_quiet = false;
            return found;
        }

        // slot 238 (Activate), each time the page opens: the base's, then the values (on the first opening the layout
        // is not loaded yet: then page_layout shows them)
        void __fastcall page_activate(void* self, void*)
        {
            gameui_fn<ActivateFn>(k_rva_base_activate)(self);
            g_page_values_due = !page_show_values(self);
        }

        // slot 101 (the game calls it again and again: AudioVideo's own looks its controls up only while it has not found
        // them): the base's (0x100FC680), then the values, once after an opening that could not show them
        void __fastcall page_layout(void* self, void*)
        {
            gameui_fn<ActivateFn>(k_rva_base_layout)(self);
            if (g_page_values_due)
                g_page_values_due = !page_show_values(self);
        }

        void __fastcall page_command(void* self, void* edx, const char* command)
        {
            if (command != nullptr && std::strncmp(command, "InvMod ", 7) == 0)
            {
                const std::string text(command + 7);
                const std::size_t space = text.find(' ');
                // (a click: the drop-down shows the choice by itself; selecting it again here sent the command again, without
                // end, and the stack overflowed)
                if (space != std::string::npos && !g_page_quiet)
                {
                    const std::string key = text.substr(0, space), value = text.substr(space + 1);
                    hud::apply_setting(key.c_str(), value.c_str());
                }
                return;
            }
            if (command != nullptr && _stricmp(command, "InventoryModWindow") == 0)
            {
                hud::open_window();
                return;
            }
            if (command != nullptr && (_stricmp(command, "Back") == 0 || _stricmp(command, "Done") == 0))
            {
                game::mem::guarded([&] {
                    void* panel = gameui_fn<GetSingletonFn>(k_rva_get_singleton)();
                    const int code = gameui_fn<ButtonCodeFn>(k_rva_button_code)(0x73, gameui_fn<UserFn>(k_rva_user)(panel));
                    reinterpret_cast<KeyCodeFn>(game::mem::vslot(self, 121))(self, code);
                });
                return;
            }
            reinterpret_cast<OnCommandFn>(g_gameui + k_rva_base_command)(self, edx, command);
        }

        void install_page()
        {
            void* mine[] = {nullptr, reinterpret_cast<void*>(page_command), reinterpret_cast<void*>(page_layout), nullptr,
                            reinterpret_cast<void*>(page_title), reinterpret_cast<void*>(page_activate), nullptr};
            void** vtable = reinterpret_cast<void**>(g_gameui + k_rva_page_vtable);
            // every slot first checked: the build this was read from
            for (const PageSlot& s : g_page_slots)
                if (game::mem::read_or<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(vtable + s.slot)) != g_gameui + s.own_rva)
                {
                    INV_LOG("overlay: GameUI's AudioVideo page does not look as expected (slot %d): no settings page", s.slot);
                    return;
                }
            if (game::mem::find_pattern(g_gameui + k_rva_open_window, 10, "55 8B EC 83 EC 0C 53 8B 5D 08") != g_gameui + k_rva_open_window ||
                game::mem::find_pattern(g_gameui + k_rva_find_child, 10, "55 8B EC 83 EC 0C 53 56 89 4D") != g_gameui + k_rva_find_child ||
                game::mem::find_pattern(g_gameui + k_rva_set_selection, 9, "55 8B EC 81 EC 08 01 00 00") != g_gameui + k_rva_set_selection ||
                game::mem::find_pattern(g_gameui + k_rva_get_flyout, 6, "56 8B F1 83 BE 88") != g_gameui + k_rva_get_flyout ||
                game::mem::find_pattern(g_gameui + k_rva_set_listener, 9, "55 8B EC 8B 45 08 89 81 8C") != g_gameui + k_rva_set_listener ||
                game::mem::find_pattern(g_gameui + k_rva_set_title, 9, "55 8B EC 81 EC 44 04 00 00") != g_gameui + k_rva_set_title ||
                game::mem::find_pattern(g_gameui + k_rva_get_singleton, 1, "A1") != g_gameui + k_rva_get_singleton ||
                game::mem::find_pattern(g_gameui + k_rva_user, 6, "8B 81 84 02 00 00") != g_gameui + k_rva_user ||
                game::mem::find_pattern(g_gameui + k_rva_button_code, 6, "55 8B EC 8B 45 08") != g_gameui + k_rva_button_code ||
                game::mem::find_pattern(g_gameui + k_rva_base_activate, 4, "53 56 8B F1") != g_gameui + k_rva_base_activate)
            {
                INV_LOG("overlay: GameUI's functions do not look as expected: no settings page");
                return;
            }
            for (int i = 0; i < static_cast<int>(std::size(g_page_slots)); ++i)
            {
                PageSlot& s = g_page_slots[i];
                s.original = reinterpret_cast<void*>(g_gameui + s.own_rva);
                patch_slot(vtable + s.slot, mine[i] != nullptr ? mine[i] : reinterpret_cast<void*>(g_gameui + s.base_rva));
            }
            install_page_blur();
            g_page_ready = true;
            hud::set_settings_page(true);
        }

        void uninstall_page()
        {
            if (!g_page_ready)
                return;
            void** vtable = reinterpret_cast<void**>(g_gameui + k_rva_page_vtable);
            for (const PageSlot& s : g_page_slots)
                patch_slot(vtable + s.slot, s.original);
            uninstall_page_blur();
            g_page_ready = false;
            hud::set_settings_page(false);
        }

        // The menus' "InventoryModSettings": the page (the mod's own window when there is none).
        void open_page(void* caller)
        {
            if (!g_page_ready)
            {
                hud::open_settings();
                return;
            }
            const int type = k_wt_audio_video;
            if (!game::mem::guarded([&] {
                    void* panel = gameui_fn<GetSingletonFn>(k_rva_get_singleton)();
                    gameui_fn<OpenWindowFn>(k_rva_open_window)(panel, &type, caller, true, nullptr);
                }))
                hud::open_settings();
        }

        template <int I>
        void __fastcall hk_menu_command(void* self, void* edx, const char* command)
        {
            if (command != nullptr && _stricmp(command, k_settings_command) == 0)
            {
                open_page(self);
                return;
            }
            g_menu_hooks[I].original(self, edx, command);
        }

        void install_menu_hooks()
        {
            const auto gameui = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"GameUI.dll"));
            void* hooks[2] = {reinterpret_cast<void*>(hk_menu_command<0>), reinterpret_cast<void*>(hk_menu_command<1>)};
            int installed = 0;
            for (int i = 0; i < 2 && gameui != 0; ++i)
            {
                MenuHook& h = g_menu_hooks[i];
                void** slot = reinterpret_cast<void**>(gameui + h.vtable_rva + 4u * k_vt_menu_on_command);
                const std::uintptr_t target = game::mem::read_or<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(slot));
                if (target != gameui + h.function_rva || game::mem::find_pattern(target, 9, h.prologue) != target)
                    continue;
                h.original = reinterpret_cast<OnCommandFn>(target);
                if (patch_slot(slot, hooks[i]))
                {
                    h.slot = slot;
                    ++installed;
                }
            }
            g_gameui = gameui;
            if (installed > 0)
                install_page();
            INV_LOG("overlay: Options > Inventory Mod %s (%d of 2 menus), %s", installed == 2 ? "ready" : "not available", installed,
                    g_page_ready ? "its page ready" : "the mod's window instead of a page");
        }

        void uninstall_menu_hooks()
        {
            uninstall_page();
            for (MenuHook& h : g_menu_hooks)
                if (h.slot != nullptr)
                {
                    patch_slot(h.slot, reinterpret_cast<void*>(h.original));
                    h.slot = nullptr;
                }
        }

        // ---- ImGui --------------------------------------------------------------------------------------------------

        bool init_imgui(IDirect3DDevice9* device)
        {
            D3DDEVICE_CREATION_PARAMETERS params{};
            if (FAILED(device->GetCreationParameters(&params)))
                return false;
            g_hwnd = params.hFocusWindow != nullptr ? params.hFocusWindow : FindWindowW(L"Valve001", nullptr);
            if (g_hwnd == nullptr)
                return false;

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // the cursor is drawn by ImGui (MouseDrawCursor)

            nc::LoadFonts(io); // before the backends' Init
            nc::StyleColorsNullcore();

            if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX9_Init(device))
            {
                INV_LOG("ImGui backend init failed");
                return false;
            }
            device->CreateStateBlock(D3DSBT_ALL, &g_state);

            g_orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                                                                         reinterpret_cast<LONG_PTR>(hk_wndproc)));
            g_device = device;
            game::set_window(g_hwnd);
            hud::init();
            icons::start();
            icons::set_device(device);
            postfx::init(device);
            install_pre_hud();
            install_menu_hooks();
            ui::vgui::install(); // the wheel in the game's own frame and fonts (else ImGui draws it)
            audio::install();
            INV_LOG("overlay ready (window %p, device %p)", static_cast<void*>(g_hwnd), static_cast<void*>(device));
            return true;
        }

        void teardown_imgui()
        {
            g_capture = false;
            if (g_hwnd != nullptr && g_orig_wndproc != nullptr &&
                reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC)) == hk_wndproc)
                SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
            release_state_block();
            icons::set_device(nullptr); // the textures go with the device's owner, on the render thread
            postfx::shutdown();
            if (g_ready)
            {
                ImGui_ImplDX9_Shutdown();
                ImGui_ImplWin32_Shutdown();
                nc::Shutdown();
                ImGui::DestroyContext();
            }
            g_ready = false;
            g_device = nullptr;
            INV_LOG("overlay torn down");
        }

        void draw_frame(IDirect3DDevice9* device)
        {
            IDirect3DSurface9* old_target = nullptr;
            IDirect3DSurface9* back_buffer = nullptr;
            device->GetRenderTarget(0, &old_target);
            if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer)))
                device->SetRenderTarget(0, back_buffer);

            if (g_state != nullptr)
                g_state->Capture();

            // The game leaves sRGB writes on (its shaders output linear colours); ImGui's colours are already final.
            device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                               D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
            device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            // The backend polls the OS cursor, which the game keeps recentring; the messages carry the real position.
            if (hud::wants_input() && g_mouse_x >= 0)
                ImGui::GetIO().AddMousePosEvent(static_cast<float>(g_mouse_x.load()), static_cast<float>(g_mouse_y.load()));
            ImGui::NewFrame();
            hud::draw();
            // Letting go of the mouse: the real cursor goes back to the middle first.  While captured the game saw a
            // cursor that never left the middle; without this its next read would be the whole way the pointer moved
            // in the meantime, and the view would flick toward it.
            const bool capture = hud::wants_input();
            if (!capture && g_capture.load() && g_hwnd != nullptr)
            {
                RECT rc{};
                if (GetClientRect(g_hwnd, &rc))
                {
                    POINT center{(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
                    ClientToScreen(g_hwnd, &center);
                    SetCursorPos(center.x, center.y); // (from this DLL: the cursor hook lets it through)
                }
            }
            g_capture = capture;
            ImGui::EndFrame();
            ImGui::Render();
            postfx::render(device, back_buffer, postfx::Layer::UnderModHud); // (an infection that leaves the mod's HUD clear)
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            postfx::render(device, back_buffer, postfx::Layer::OverAll); // the screamer, the stamina, the infection over all

            if (g_state != nullptr)
                g_state->Apply();
            if (old_target != nullptr)
            {
                device->SetRenderTarget(0, old_target);
                old_target->Release();
            }
            if (back_buffer != nullptr)
                back_buffer->Release();
        }

        // (a separate function: __try cannot share one with objects that need unwinding)
        bool draw_frame_guarded(IDirect3DDevice9* device)
        {
            __try
            {
                draw_frame(device);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        HRESULT WINAPI hk_present(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window,
                                  const RGNDATA* dirty)
        {
            if (g_shutdown_requested.load())
            {
                if (!g_shutdown_done.load())
                {
                    teardown_imgui();
                    g_shutdown_done = true;
                }
            }
            else if (!g_broken)
            {
                // a few times a second the main thread gets to run the backend's housekeeping
                static ULONGLONG last_tick = 0;
                if (const ULONGLONG now = GetTickCount64(); now - last_tick > 100)
                {
                    last_tick = now;
                    game::wake_main_thread();
                }
                if (!g_ready && g_device == nullptr)
                    g_ready = init_imgui(device);
                if (g_ready && device == g_device && !draw_frame_guarded(device))
                {
                    g_broken = true;
                    g_capture = false;
                    INV_LOG("fault while drawing a frame: overlay disabled");
                }
            }
            return g_present.original()(device, src, dst, window, dirty);
        }

        HRESULT WINAPI hk_reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params)
        {
            if (g_ready && device == g_device)
            {
                release_state_block();
                postfx::invalidate();
                ImGui_ImplDX9_InvalidateDeviceObjects();
            }
            const HRESULT hr = g_reset.original()(device, params);
            if (g_ready && device == g_device && SUCCEEDED(hr))
            {
                ImGui_ImplDX9_CreateDeviceObjects();
                device->CreateStateBlock(D3DSBT_ALL, &g_state);
            }
            return hr;
        }
    } // namespace

    bool install()
    {
        void* present = nullptr;
        void* reset = nullptr;
        if (!find_device_methods(&present, &reset))
        {
            INV_LOG("could not read the D3D9 vtable");
            return false;
        }
        INV_LOG("D3D9 Present %p, Reset %p", present, reset);

        bool ok = g_present.create("Present", present, hk_present) && g_reset.create("Reset", reset, hk_reset);
        // user32's cursor functions (a missing hook only costs the free mouse; the HUD still works)
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
        {
            g_set_cursor_pos.create("SetCursorPos", reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos")),
                                    hk_set_cursor_pos);
            g_get_cursor_pos.create("GetCursorPos", reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos")),
                                    hk_get_cursor_pos);
        }
        if (!ok)
            return false;
        g_set_cursor_pos.enable();
        g_get_cursor_pos.enable();
        return g_present.enable() && g_reset.enable();
    }

    void uninstall()
    {
        uninstall_pre_hud(); // first: a pass queued already still finds the DLL (the waits below)
        ui::vgui::uninstall();
        uninstall_menu_hooks();
        g_shutdown_requested = true;
        for (int i = 0; i < 40 && !g_shutdown_done.load(); ++i)
            Sleep(50); // the render thread tears ImGui down in its next Present
        if (!g_shutdown_done.load())
            INV_LOG("no Present to tear down on: leaving the overlay's D3D objects to the process");
        audio::uninstall();
        g_present.remove();
        g_reset.remove();
        g_set_cursor_pos.remove();
        g_get_cursor_pos.remove();
        Sleep(200); // let a call that is inside a detour leave
        icons::shutdown();
    }

} // namespace ui::overlay
