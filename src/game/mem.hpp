#pragma once
// Reads and calls into the game's memory, guarded: a bad pointer logs and fails instead of taking the game down.

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace game::mem
{

    // A copy that survives a fault; false when the source cannot be read.
    bool safe_copy(void* dst, const void* src, std::size_t n);

    template <class T>
    bool read(std::uintptr_t address, T& out)
    {
        return safe_copy(&out, reinterpret_cast<const void*>(address), sizeof(T));
    }

    template <class T>
    T read_or(std::uintptr_t address, T fallback = T{})
    {
        T value{};
        return read(address, value) ? value : fallback;
    }

    // Runs `f`, returning false when it faulted.  (Takes a reference so this function has nothing to unwind.)
    template <class F>
    bool guarded(F&& f)
    {
        __try
        {
            f();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The function behind slot `index` of the object's vtable (0 when it cannot be read).
    std::uintptr_t vslot(const void* object, int index);

    struct ModuleInfo
    {
        std::uintptr_t base = 0;
        std::size_t size = 0;
        std::uint32_t timestamp = 0; // the PE header's: tells which build of the game DLL this is
    };
    bool module_info(const wchar_t* name, ModuleInfo& out);

    // Finds `pattern` ("6A 00 B9 ?? ?? ?? ??") in the `length` bytes from `start`.  0 when not found.
    std::uintptr_t find_pattern(std::uintptr_t start, std::size_t length, const char* pattern);

} // namespace game::mem
