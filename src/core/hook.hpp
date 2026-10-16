#pragma once
// A typed wrapper over MinHook: one inline hook of a function, with the trampoline to call the original.

#include <MinHook.h>

#include "core/log.hpp"

namespace core
{

    template <class Fn>
    class Hook
    {
    public:
        // Creates the hook (disabled).  `name` is for the log.
        bool create(const char* name, void* target, Fn detour)
        {
            name_ = name;
            void* original = nullptr;
            const MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(detour), &original);
            if (status != MH_OK)
            {
                INV_LOG("hook %s: MH_CreateHook failed: %s", name, MH_StatusToString(status));
                return false;
            }
            target_ = target;
            original_ = reinterpret_cast<Fn>(original);
            return true;
        }

        bool enable()
        {
            if (target_ == nullptr)
                return false;
            const MH_STATUS status = MH_EnableHook(target_);
            if (status != MH_OK)
                INV_LOG("hook %s: MH_EnableHook failed: %s", name_, MH_StatusToString(status));
            return status == MH_OK;
        }

        void remove()
        {
            if (target_ == nullptr)
                return;
            MH_DisableHook(target_);
            MH_RemoveHook(target_);
            target_ = nullptr;
        }

        Fn original() const
        {
            return original_;
        }
        bool installed() const
        {
            return target_ != nullptr;
        }

    private:
        const char* name_ = "";
        void* target_ = nullptr;
        Fn original_ = nullptr;
    };

} // namespace core
