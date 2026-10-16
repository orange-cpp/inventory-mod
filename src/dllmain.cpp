// inventory_mod.dll: loaded into left4dead2.exe (run the game with -insecure: VAC must not see a foreign DLL), or into a
// dedicated server's srcds.exe, where only the game side runs (no window, no HUD).
// DllMain only starts a thread; everything else happens there.

#include "core/config.hpp"
#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/game.hpp"
#include "ui/hud.hpp"
#include "ui/overlay.hpp"

#include <windows.h>

#include <initializer_list>
#include <string>

namespace
{
    HMODULE g_self = nullptr;

    bool wait_for_module(const wchar_t* name, int seconds)
    {
        for (int i = 0; i < seconds * 10; ++i)
        {
            if (GetModuleHandleW(name) != nullptr)
                return true;
            Sleep(100);
        }
        return false;
    }

    bool in_dedicated_server()
    {
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return false;
        const wchar_t* name = wcsrchr(path, L'\\');
        return _wcsicmp(name != nullptr ? name + 1 : path, L"srcds.exe") == 0;
    }

    // srcds.exe: the game side only.  It unloads when inventory_mod_server.unload appears next to the DLL (there is no
    // window to post the unload message to).
    void run_dedicated()
    {
        for (const wchar_t* name : {L"engine.dll", L"server.dll"})
        {
            if (!wait_for_module(name, 120))
            {
                INV_LOG("%ls is not loaded", name);
                core::log_shutdown();
                FreeLibraryAndExitThread(g_self, 1);
            }
        }
        if (MH_Initialize() != MH_OK)
        {
            INV_LOG("MH_Initialize failed");
            core::log_shutdown();
            FreeLibraryAndExitThread(g_self, 2);
        }
        core::config_load();
        if (!game::init(true))
            INV_LOG("no game access on this server");

        const std::wstring flag = std::wstring(core::module_dir()) + L"inventory_mod_server.unload";
        DeleteFileW(flag.c_str());
        INV_LOG("running in srcds.exe: create inventory_mod_server.unload to unload");
        while (GetFileAttributesW(flag.c_str()) == INVALID_FILE_ATTRIBUTES)
            Sleep(250);
        DeleteFileW(flag.c_str());

        INV_LOG("unloading");
        MH_Uninitialize();
        core::log_shutdown();
        FreeLibraryAndExitThread(g_self, 0);
    }

    DWORD WINAPI main_thread(LPVOID)
    {
        if (in_dedicated_server())
        {
            core::log_init(g_self, L"inventory_mod_server.log");
            INV_LOG("inventory_mod loaded into srcds.exe");
            run_dedicated();
        }
        core::log_init(g_self);
        INV_LOG("inventory_mod loaded");

        // the game's modules the mod needs, all loaded once the menu is up
        for (const wchar_t* name : {L"engine.dll", L"client.dll", L"server.dll", L"shaderapidx9.dll"})
        {
            if (!wait_for_module(name, 60))
            {
                INV_LOG("%ls is not loaded: is this left4dead2.exe with -dx9?", name);
                core::log_shutdown();
                FreeLibraryAndExitThread(g_self, 1);
            }
        }

        if (MH_Initialize() != MH_OK)
        {
            INV_LOG("MH_Initialize failed");
            core::log_shutdown();
            FreeLibraryAndExitThread(g_self, 2);
        }
        if (!game::init())
            INV_LOG("no game access: the inventory only edits its own list");
        if (!ui::overlay::install())
            INV_LOG("overlay could not be installed");

        INV_LOG("running: Ctrl+End unloads");
        while (!hud::unload_requested())
            Sleep(100);

        INV_LOG("unloading");
        ui::overlay::uninstall();
        MH_Uninitialize();
        core::log_shutdown();
        FreeLibraryAndExitThread(g_self, 0);
    }
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, main_thread, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}
