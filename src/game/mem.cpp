#include "game/mem.hpp"

#include <cstring>
#include <vector>

namespace game::mem
{

    bool safe_copy(void* dst, const void* src, std::size_t n)
    {
        __try
        {
            std::memcpy(dst, src, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::uintptr_t vslot(const void* object, int index)
    {
        std::uintptr_t vtable = 0;
        if (object == nullptr || !read(reinterpret_cast<std::uintptr_t>(object), vtable) || vtable == 0)
            return 0;
        return read_or<std::uintptr_t>(vtable + static_cast<std::uintptr_t>(index) * sizeof(void*));
    }

    bool module_info(const wchar_t* name, ModuleInfo& out)
    {
        const HMODULE module = GetModuleHandleW(name);
        if (module == nullptr)
            return false;
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS nt{};
        if (!read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            !read(base + static_cast<std::uintptr_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE)
            return false;
        out.base = base;
        out.size = nt.OptionalHeader.SizeOfImage;
        out.timestamp = nt.FileHeader.TimeDateStamp;
        return true;
    }

    std::uintptr_t find_pattern(std::uintptr_t start, std::size_t length, const char* pattern)
    {
        std::vector<int> bytes; // -1: any
        for (const char* p = pattern; *p != '\0';)
        {
            if (*p == ' ')
            {
                ++p;
                continue;
            }
            if (*p == '?')
            {
                bytes.push_back(-1);
                while (*p == '?')
                    ++p;
                continue;
            }
            char* end = nullptr;
            bytes.push_back(static_cast<int>(std::strtoul(p, &end, 16)));
            p = end;
        }
        if (bytes.empty() || length < bytes.size())
            return 0;

        std::vector<std::uint8_t> window(length);
        if (!safe_copy(window.data(), reinterpret_cast<const void*>(start), length))
            return 0;
        for (std::size_t i = 0; i + bytes.size() <= length; ++i)
        {
            bool match = true;
            for (std::size_t j = 0; j < bytes.size() && match; ++j)
                match = bytes[j] < 0 || window[i + j] == static_cast<std::uint8_t>(bytes[j]);
            if (match)
                return start + i;
        }
        return 0;
    }

} // namespace game::mem
