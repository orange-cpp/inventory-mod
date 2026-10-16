#include "game/backend.hpp"

#include "core/config.hpp"
#include "core/log.hpp"

#include <atomic>
#include <cstdio>

namespace game
{

    namespace
    {
        std::atomic<HWND> g_window{nullptr};
        std::atomic<Backend*> g_backend{nullptr};

        inv::Limits limits_from_config()
        {
            return {.slots = core::config().slots, .max_weight = core::config().max_weight};
        }

        // Nothing of the game behind it: edits the list only, so the HUD can be driven on its own.
        class StubBackend final : public Backend
        {
        public:
            const char* name() const override
            {
                return "stub";
            }
            bool connected() const override
            {
                return false;
            }
            void use(int slot) override
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                const inv::Stack stack = s.inventory.slot(slot);
                if (!stack.empty())
                {
                    INV_LOG("[stub] use %s", stack.def->classname);
                    s.inventory.remove_from_slot(slot, 1);
                }
            }
            void drop(int slot, int count) override
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                const inv::Stack stack = s.inventory.slot(slot);
                if (!stack.empty())
                {
                    INV_LOG("[stub] drop %s x%d", stack.def->classname, count);
                    s.inventory.remove_from_slot(slot, count);
                }
            }
            void store_held() override
            {
                notify("No game connection: nothing to store from");
            }
            void pickup_nearby() override
            {
                notify("No game connection: nothing to pick up");
            }
        };

        StubBackend g_stub;
    } // namespace

    Session& local_session()
    {
        static Session instance{.inventory = inv::Inventory(limits_from_config())};
        return instance;
    }

    namespace
    {
        std::atomic<Session*> g_active{nullptr};
    } // namespace

    Session& session()
    {
        Session* active = g_active.load();
        return active != nullptr ? *active : local_session();
    }

    void set_active_session(Session* s)
    {
        g_active = s;
    }

    void session_apply_limits()
    {
        Session& s = local_session();
        std::lock_guard lock(s.mutex);
        inv::Inventory rebuilt(limits_from_config());
        for (int i = 0; i < s.inventory.slot_count(); ++i)
        {
            const inv::Stack& stack = s.inventory.slot(i);
            if (!stack.empty())
                rebuilt.add(*stack.def, stack.count);
        }
        s.inventory = std::move(rebuilt);
    }

    std::string refusal(const inv::ItemDef& def)
    {
        Session& s = session();
        std::lock_guard lock(s.mutex);
        const inv::Inventory& inventory = s.inventory;
        if (inventory.room_for(def) == 0)
        {
            char text[96];
            std::snprintf(text, sizeof(text), "Too heavy: %.1f / %.1f, %s weighs %.1f", inventory.weight(),
                          inventory.limits().max_weight, def.name, def.weight);
            return text;
        }
        return "The inventory is full";
    }

    void notify(const std::string& text)
    {
        INV_LOG("notice: %s", text.c_str());
        Session& s = session();
        std::lock_guard lock(s.mutex);
        s.message = text;
        s.message_until = GetTickCount64() + 3500;
    }

    Backend& backend()
    {
        Backend* b = g_backend.load();
        return b != nullptr ? *b : g_stub;
    }

    void set_backend(Backend* backend)
    {
        g_backend = backend;
    }

    void set_window(HWND window)
    {
        g_window = window;
    }

    void wake_main_thread()
    {
        if (HWND window = g_window.load())
            PostMessageW(window, k_msg_pump, 0, 0);
    }

} // namespace game
