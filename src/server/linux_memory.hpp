#pragma once

#include "server/linux_sdk.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace game::linux_memory
{
    template <typename Value>
    Value& field(CBaseEntity* entity, std::uintptr_t offset)
    {
        return *reinterpret_cast<Value*>(reinterpret_cast<std::uintptr_t>(entity) + offset);
    }

    template <typename Function>
    bool write_vtable_entry(Function* slot, Function fn)
    {
        const long page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0)
            return false;
        const auto address = reinterpret_cast<std::uintptr_t>(slot);
        auto* page = reinterpret_cast<void*>(address - address % static_cast<std::uintptr_t>(page_size));
        if (mprotect(page, page_size, PROT_READ | PROT_WRITE) != 0)
            return false;
        *slot = fn;
        if (mprotect(page, page_size, PROT_READ | PROT_EXEC) != 0)
            std::fprintf(stderr, "inventory_mod: couldn't restore server vtable page permissions\n");
        return true;
    }

    inline bool server_vtable_slot(std::uintptr_t base, const void* slot)
    {
        const auto offset = reinterpret_cast<std::uintptr_t>(slot) - base;
        return offset >= linux_sdk::read_only_data_start && offset < linux_sdk::read_only_data_end;
    }

    // The supported GCC prologue contains six bytes of complete instructions with no relative operands.
    template <typename Function>
    class SavedFrameHook
    {
    public:
        SavedFrameHook() = default;
        SavedFrameHook(const SavedFrameHook&) = delete;
        SavedFrameHook& operator=(const SavedFrameHook&) = delete;

        bool install(Function target, Function replacement)
        {
            auto* address = reinterpret_cast<unsigned char*>(target);
            if (original_ != nullptr || address == nullptr || std::memcmp(address, prefix_.data(), prefix_.size()) != 0)
                return false;
            auto* trampoline = static_cast<unsigned char*>(mmap(nullptr, trampoline_size, PROT_READ | PROT_WRITE,
                                                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
            if (trampoline == MAP_FAILED)
                return false;
            std::memcpy(trampoline, prefix_.data(), prefix_.size());
            jump(trampoline + prefix_.size(), trampoline + prefix_.size(),
                 reinterpret_cast<std::uintptr_t>(address + prefix_.size()));
            if (mprotect(trampoline, trampoline_size, PROT_READ | PROT_EXEC) != 0)
            {
                munmap(trampoline, trampoline_size);
                return false;
            }
            std::array<unsigned char, 6> patch{0, 0, 0, 0, 0, 0x90};
            jump(patch.data(), address, reinterpret_cast<std::uintptr_t>(replacement));
            if (!write_code(address, patch))
            {
                munmap(trampoline, trampoline_size);
                return false;
            }
            target_ = address;
            trampoline_ = trampoline;
            original_ = reinterpret_cast<Function>(trampoline);
            return true;
        }

        Function original() const { return original_; }

        bool remove()
        {
            if (original_ == nullptr)
                return true;
            if (!write_code(target_, prefix_))
                return false;
            munmap(trampoline_, trampoline_size);
            target_ = nullptr;
            trampoline_ = nullptr;
            original_ = nullptr;
            return true;
        }

    private:
        static void jump(unsigned char* bytes, const unsigned char* at, std::uintptr_t target)
        {
            bytes[0] = 0xe9;
            const auto relative = static_cast<std::uint32_t>(target - reinterpret_cast<std::uintptr_t>(at + 5));
            std::memcpy(bytes + 1, &relative, sizeof(relative));
        }

        static bool write_code(unsigned char* address, const std::array<unsigned char, 6>& bytes)
        {
            const auto page_size = sysconf(_SC_PAGESIZE);
            const auto at = reinterpret_cast<std::uintptr_t>(address);
            if (page_size <= 0 || at % page_size + bytes.size() > static_cast<std::size_t>(page_size))
                return false;
            auto* page = reinterpret_cast<void*>(at - at % page_size);
            if (mprotect(page, page_size, PROT_READ | PROT_WRITE) != 0)
                return false;
            std::memcpy(address, bytes.data(), bytes.size());
            if (mprotect(page, page_size, PROT_READ | PROT_EXEC) != 0)
                std::fprintf(stderr, "inventory_mod: couldn't restore server code page permissions\n");
            return true;
        }

        static constexpr std::array<unsigned char, 6> prefix_ = {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53};
        static constexpr std::size_t trampoline_size = prefix_.size() + 5;
        unsigned char* target_ = nullptr;
        unsigned char* trampoline_ = nullptr;
        Function original_ = nullptr;
    };
}
