// The mod's addons (.vpk next to the DLL) mounted into the game's file system, and what the game can load.

#include "game/addons.hpp"

#include "core/log.hpp"
#include "game/mem.hpp"
#include "game/sdk.hpp"

#include <cstring>
#include <string>

namespace game::detail
{

    using namespace sdk;

    // filesystem_stdio.dll "VBaseFileSystem012": the IBaseFileSystem of its one CFileSystem_Stdio (0x1005260C, vtable
    // 0x10047DFC, set at 0x10045509); slot 10 FileExists(name, pathID) 0x1000E340 (thiscall, ret 8, the profiler
    // scope "CBaseFileSystem::FileExists").  The same file in the game and the dedicated server.
    // The file system's interface `name` ("VFileSystem018", "VBaseFileSystem012"): the game's filesystem_stdio.dll
    // serves it through CreateInterface.  The dedicated server has no filesystem_stdio.dll loaded: its dedicated.dll
    // carries the same code ("..\filesystem\filesystem_stdio.cpp"; its CFileSystem_Stdio at 0x100E6898, vtables
    // 0x100B7C64 and 0x100B7C1C, slot 107 0x1000EB30 and slot 10 0x10012D50 with the same prologues) and hands it to
    // the engine through a factory of its own (0x10001010), not its exported CreateInterface: "VFileSystem018" is
    // the pointer at 0x100DE824, "VBaseFileSystem012" the one at 0x100DE828.  (The callers check the slots they use.)
    void* filesystem_interface(const char* name)
    {
        if (HMODULE module = GetModuleHandleW(L"filesystem_stdio.dll"))
        {
            const auto create = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"));
            return create != nullptr ? create(name, nullptr) : nullptr;
        }
        if (HMODULE module = GetModuleHandleW(L"dedicated.dll"))
        {
            const auto base = reinterpret_cast<std::uintptr_t>(module);
            const bool full = std::strcmp(name, "VFileSystem018") == 0;
            return mem::read_or<void*>(base + (full ? 0xDE824 : 0xDE828));
        }
        return nullptr;
    }

    // The mod's addons, every .vpk in the addons folder next to the DLL (inventory_mod_rounds.vpk, the ammo packs:
    // tools/make_rounds_addon.py), are mounted as the game mounts an addon, so they need no copy in
    // left4dead2/addons: filesystem_stdio.dll "VFileSystem018" (its one CFileSystem_Stdio, 0x10052608, vtable
    // 0x10047E44) slot 107 AddVPKFile(name, addType) 0x1000A110 (thiscall, ret 8): the name made absolute; one
    // already in its list of VPKs (+0x1CC, count +0x1D8) is left alone, else a CPackedStore is opened for it (an
    // empty or unreadable one is dropped) and put at the tail (1) or the head (0).  AddSearchPath's internal function
    // (0x1000C630) calls it for a folder's pakNN_dir.vpk.  Slot 108 RemoveVPKFile(name) 0x100068B0 would take one
    // out; the mod leaves them mounted when it unloads (models read from them stay in use), and mounting them again
    // when the mod loads again changes nothing.

    bool game_file_exists(const char* name);

    void mount_addons()
    {
        const std::wstring folder = std::wstring(core::module_dir()) + L"addons\\";
        WIN32_FIND_DATAW found{};
        HANDLE search = FindFirstFileW((folder + L"*.vpk").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE)
        {
            INV_LOG("game: no addons next to the DLL (addons\\*.vpk): dropped rounds are the incendiary pack, unless "
                    "inventory_mod_rounds.vpk is in left4dead2/addons");
            return;
        }
        void* fs = filesystem_interface("VFileSystem018");
        const std::uintptr_t at = fs != nullptr ? mem::vslot(fs, k_vt_add_vpk_file) : 0;
        if (at == 0 || mem::find_pattern(at, 10, "55 8B EC 81 EC 0C 01 00 00 A1") != at)
        {
            INV_LOG("game: the file system's AddVPKFile does not look as expected (file system %p, slot 107 %p): the mod's "
                    "addons are not mounted",
                    fs, reinterpret_cast<void*>(at));
            FindClose(search);
            return;
        }
        do
        {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            const std::wstring path = folder + found.cFileName;
            char name[MAX_PATH * 2] = {};
            if (WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, name, sizeof(name), nullptr, nullptr) == 0)
                continue;
            // at the head (0): ahead of the game's own files, so the menu's flyout in inventory_mod_menu.vpk is the
            // one the game reads (the others hold only the mod's own files)
            const bool ok = mem::guarded([&] { reinterpret_cast<AddVpkFileFn>(at)(fs, name, 0); });
            INV_LOG("game: mounted %s%s", name, ok ? "" : " (the call failed)");
        } while (FindNextFileW(search, &found));
        FindClose(search);
        INV_LOG("game: the ammo packs %s", game_file_exists("models/inv_mod/rounds_smg.mdl") ? "are there" : "are not found");
    }

    // The game can load `name` (its own files, the addons): main thread.
    bool game_file_exists(const char* name)
    {
        static void* fs = nullptr;
        static FileExistsFn file_exists = [] {
            fs = filesystem_interface("VBaseFileSystem012");
            const std::uintptr_t at = fs != nullptr ? mem::vslot(fs, k_vt_file_exists) : 0;
            if (at != 0 && mem::find_pattern(at, 9, "55 8B EC 81 EC 7C 01 00 00") == at)
                return reinterpret_cast<FileExistsFn>(at);
            INV_LOG("game: the file system's FileExists does not look as expected: piles are the incendiary pack");
            return static_cast<FileExistsFn>(nullptr);
        }();
        bool found = false;
        if (file_exists != nullptr)
            mem::guarded([&] { found = file_exists(fs, name, "GAME"); });
        return found;
    }

} // namespace game::detail
