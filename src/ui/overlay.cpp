#include "ui/overlay.hpp"

#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "ui/icons.hpp"
#include "ui/hud.hpp"

#include <atomic>
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

        // ---- ImGui --------------------------------------------------------------------------------------------------

        void release_state_block()
        {
            if (g_state != nullptr)
            {
                g_state->Release();
                g_state = nullptr;
            }
        }

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
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

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
        g_shutdown_requested = true;
        for (int i = 0; i < 40 && !g_shutdown_done.load(); ++i)
            Sleep(50); // the render thread tears ImGui down in its next Present
        if (!g_shutdown_done.load())
            INV_LOG("no Present to tear down on: leaving the overlay's D3D objects to the process");
        g_present.remove();
        g_reset.remove();
        g_set_cursor_pos.remove();
        g_get_cursor_pos.remove();
        Sleep(200); // let a call that is inside a detour leave
        icons::shutdown();
    }

} // namespace ui::overlay
