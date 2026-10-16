#include "ui/hud.hpp"

#include "core/config.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <imgui_nullcore/imgui_nullcore.hpp>
#include <mutex>
#include <string>
#include <vector>

namespace nc = imgui_nullcore;

namespace hud
{

    namespace
    {
        constexpr const char* k_slot_payload = "INV_SLOT";

        struct KeyRow
        {
            const char* label;
            int* vk;
        };

        // What the UI asked for while it was drawing under the session lock; carried out after the lock is released
        // (the backend locks the session itself).
        struct Intent
        {
            enum Kind
            {
                Use,
                Drop,
                Store,
                Pickup
            } kind;
            int slot;
            int count;
        };

        struct State
        {
            bool open = false;
            bool unload = false;
            int rebinding = -1; // index into key_rows(): the next key press is the new binding
            int context_slot = -1;
            int debug_item = 0;
            int debug_count = 1;
            int native_sel = -1; // the selected game weapon (index into Session::native), -1: an inventory slot is selected
            int swallow_char = 0; // the character a swallowed hotkey will still produce
            ULONGLONG swallow_until = 0;
            std::uint32_t saved_revision = 0; // the inventory revision last written to disk
            std::vector<Intent> intents;
        } g;

        std::vector<KeyRow> key_rows()
        {
            core::Config& c = core::config();
            return {{"Open / close:", &c.key_toggle},
                    {"Previous item:", &c.key_prev},
                    {"Next item:", &c.key_next},
                    {"Use item:", &c.key_use},
                    {"Drop item:", &c.key_drop},
                    {"Store held item:", &c.key_store},
                    {"Pick up nearby:", &c.key_pickup}};
        }

        // 1..9 -> slots 0..8, 0 -> slot 9 (the top row of the keyboard)
        int digit_slot(int vk)
        {
            if (!core::config().replace_native)
                return -1;
            if (vk >= '1' && vk <= '9')
                return vk - '1';
            return vk == '0' ? 9 : -1;
        }

        bool is_hotkey(int vk)
        {
            const core::Config& c = core::config();
            return vk != 0 && (digit_slot(vk) >= 0 || vk == c.key_toggle || vk == c.key_prev || vk == c.key_next || vk == c.key_use ||
                               vk == c.key_drop || vk == c.key_store || vk == c.key_pickup);
        }

        void run(const Intent& intent)
        {
            game::Backend& backend = game::backend();
            switch (intent.kind)
            {
            case Intent::Use:
                backend.use(intent.slot);
                break;
            case Intent::Drop:
                backend.drop(intent.slot, intent.count);
                break;
            case Intent::Store:
                backend.store_held();
                break;
            case Intent::Pickup:
                backend.pickup_nearby();
                break;
            }
        }

        // ---- the hotbar's selection: [the game's gun slots, inventory slots...] -------------------------------------

        // The hotbar shows only the game's two gun slots (primary, secondary) in front of the inventory: a throwable,
        // kit, pills or carried item in the player's hands is shown in its own inventory box (Stack::held), so items
        // never move between boxes when the player switches.
        constexpr int k_hotbar_native = 2;
        int hotbar_native(const game::Session& session)
        {
            return std::min(k_hotbar_native, static_cast<int>(session.native.size()));
        }

        // Selects entry `index`; like the game's own panel, a game weapon is switched to and an inventory item is taken in hand.
        void select_entry(int index)
        {
            std::string weapon;
            int use_slot = -1;
            {
                game::Session& session = game::local_session();
                std::lock_guard lock(session.mutex);
                const int n = hotbar_native(session);
                if (index < 0)
                    return;
                if (index < n)
                {
                    if (session.native[static_cast<std::size_t>(index)].classname.empty())
                        return; // an empty game slot: nothing to switch to
                    g.native_sel = index;
                    weapon = session.native[static_cast<std::size_t>(index)].classname;
                }
                else if (index - n < session.inventory.slot_count())
                {
                    g.native_sel = -1;
                    session.inventory.select(index - n);
                    if (!session.inventory.slot(index - n).empty())
                        use_slot = index - n;
                }
            }
            if (!weapon.empty())
                game::backend().select_weapon(weapon);
            else if (use_slot >= 0)
                game::backend().use(use_slot); // like the game's own panel: selecting an item takes it in hand
        }

        // The wheel and the prev / next keys: through the game's weapons and the occupied inventory slots, wrapping.
        void step_entry(int direction)
        {
            std::vector<int> entries;
            int position = -1;
            {
                game::Session& session = game::local_session();
                std::lock_guard lock(session.mutex);
                const int n = hotbar_native(session);
                const int current = g.native_sel >= 0 && g.native_sel < n ? g.native_sel : n + session.inventory.selected();
                for (int i = 0; i < n; ++i)
                    if (!session.native[static_cast<std::size_t>(i)].classname.empty())
                        entries.push_back(i);
                for (int i = 0; i < session.inventory.slot_count(); ++i)
                    if (!session.inventory.slot(i).empty())
                        entries.push_back(n + i);
                for (std::size_t i = 0; i < entries.size(); ++i)
                    if (entries[i] == current)
                        position = static_cast<int>(i);
            }
            if (entries.empty())
                return;
            const int count = static_cast<int>(entries.size());
            position = position < 0 ? (direction > 0 ? 0 : count - 1) : (position + direction + count) % count;
            select_entry(entries[static_cast<std::size_t>(position)]);
        }

        // The selected game weapon's classname, empty when an inventory slot is selected.
        std::string native_selection()
        {
            game::Session& session = game::local_session();
            std::lock_guard lock(session.mutex);
            if (g.native_sel >= 0 && g.native_sel < hotbar_native(session) &&
                !session.native[static_cast<std::size_t>(g.native_sel)].classname.empty())
                return session.native[static_cast<std::size_t>(g.native_sel)].classname;
            return {};
        }

        // ---- drawing ------------------------------------------------------------------------------------------------

        // One slot, in the toolkit's primitives: used by the hotbar and by the inventory window's grid.
        void draw_slot(ImDrawList* dl, int x, int y, int size, const inv::Stack& stack, bool selected, bool hovered,
                       int number)
        {
            const nc::Palette& pal = nc::GetPalette();
            const nc::Fonts& fonts = nc::GetFonts();
            const int line = static_cast<int>(fonts.Size);

            nc::FillRect(dl, x, y, x + size, y + size, hovered ? pal.BgFocused : pal.BgAlt);
            if (!stack.empty())
            {
                const ImVec4 c = ImGui::ColorConvertU32ToFloat4(stack.def->color);
                const int pad = size / 6;
                nc::FillRect(dl, x + pad, y + pad, x + size - pad, y + size - pad,
                             ImVec4(c.x * 0.4f, c.y * 0.4f, c.z * 0.4f, 1.f));
                nc::OutlineRect(dl, x + pad, y + pad, x + size - pad, y + size - pad, c);
                const int tw = nc::CalcTextWidth(fonts.Header, stack.def->tag);
                nc::AddText(dl, fonts.Header, x + (size - tw) / 2, y + (size - line) / 2, ImVec4(1.f, 1.f, 1.f, 1.f),
                            stack.def->tag);
                // the item in the player's hands lives in this box: a bar along the bottom, and it counts in the number
                if (stack.held)
                    nc::FillRect(dl, x + 3, y + size - 4, x + size - 3, y + size - 2, ImVec4(0.45f, 0.85f, 0.35f, 1.f));
                const int shown = stack.count + (stack.held ? 1 : 0);
                if (shown > 1)
                {
                    char text[16];
                    std::snprintf(text, sizeof(text), "%d", shown);
                    nc::AddText(dl, fonts.Normal, x + size - nc::CalcTextWidth(fonts.Normal, text) - 3, y + size - line,
                                pal.Text, text);
                }
            }
            if (number > 0)
            {
                char text[8];
                std::snprintf(text, sizeof(text), "%d", number);
                nc::AddText(dl, fonts.Normal, x + 3, y + 1, pal.TextAlt, text);
            }
            nc::OutlineRect(dl, x, y, x + size, y + size, selected ? pal.BorderSelected : pal.Border);
        }

        void draw_hotbar(const game::Session& session)
        {
            const inv::Inventory& inventory = session.inventory;
            const core::Config& cfg = core::config();
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const nc::Fonts& fonts = nc::GetFonts();
            const nc::Palette& pal = nc::GetPalette();

            const int size = static_cast<int>(40.f * cfg.hud_scale);
            const int gap = static_cast<int>(4.f * cfg.hud_scale);
            const int n = inventory.slot_count();
            const int m = hotbar_native(session);
            const int total = (m + n) * size + (m + n - 1) * gap;
            const int x0 = static_cast<int>(display.x * 0.58f) - total / 2; // clear of the game's survivor panels (left)
            const int y0 = static_cast<int>(display.y) - size - static_cast<int>(10.f * cfg.hud_scale);
            const bool native_valid = g.native_sel >= 0 && g.native_sel < m &&
                                      !session.native[static_cast<std::size_t>(g.native_sel)].classname.empty();

            // one continuous bar: the game's gun slots (fixed), then the inventory, all drawn the same way
            for (int i = 0; i < m; ++i)
            {
                const game::NativeWeapon& weapon = session.native[static_cast<std::size_t>(i)];
                const int x = x0 + i * (size + gap);
                const bool selected_here = native_valid && i == g.native_sel;
                if (weapon.classname.empty())
                {
                    draw_slot(dl, x, y0, size, inv::Stack{}, false, false, i + 1);
                    continue;
                }
                if (const inv::ItemDef* def = inv::find_item(weapon.classname))
                {
                    draw_slot(dl, x, y0, size, inv::Stack{def, 1}, selected_here, false, i + 1);
                    continue;
                }
                // a gun or a melee weapon: not in the catalogue, so a neutral colour
                nc::FillRect(dl, x, y0, x + size, y0 + size, pal.BgAlt);
                const int pad = size / 6;
                nc::FillRect(dl, x + pad, y0 + pad, x + size - pad, y0 + size - pad, ImVec4(0.22f, 0.24f, 0.28f, 1.f));
                nc::OutlineRect(dl, x + pad, y0 + pad, x + size - pad, y0 + size - pad, ImVec4(0.6f, 0.64f, 0.7f, 1.f));
                const int tw = nc::CalcTextWidth(fonts.Header, weapon.tag);
                nc::AddText(dl, fonts.Header, x + (size - tw) / 2, y0 + (size - static_cast<int>(fonts.Size)) / 2,
                            ImVec4(1.f, 1.f, 1.f, 1.f), weapon.tag);
                char number[8];
                std::snprintf(number, sizeof(number), "%d", i + 1);
                nc::AddText(dl, fonts.Normal, x + 3, y0 + 1, pal.TextAlt, number);
                nc::OutlineRect(dl, x, y0, x + size, y0 + size, selected_here ? pal.BorderSelected : pal.Border);
            }
            for (int i = 0; i < n; ++i)
                draw_slot(dl, x0 + (m + i) * (size + gap), y0, size, inventory.slot(i),
                          !native_valid && i == inventory.selected(), false, m + i + 1);

            int y = y0 - static_cast<int>(fonts.Size) - 3;
            // a status line from the game side, for a few seconds
            if (!session.message.empty() && GetTickCount64() < session.message_until)
            {
                const int tw = nc::CalcTextWidth(fonts.Header, session.message.c_str());
                nc::AddText(dl, fonts.Header, x0 + (total - tw) / 2, y, pal.TextSelected, session.message.c_str());
                y -= static_cast<int>(fonts.Size) + 2;
            }
            // the selected item's name above the bar
            const inv::Stack& selected = inventory.slot(inventory.selected());
            const char* name = native_valid ? session.native[static_cast<std::size_t>(g.native_sel)].name.c_str()
                                            : (selected.empty() ? nullptr : selected.def->name);
            if (name != nullptr)
            {
                const int tw = nc::CalcTextWidth(fonts.Normal, name);
                nc::AddText(dl, fonts.Normal, x0 + (total - tw) / 2, y, pal.TextFocused, name);
            }
        }

        void slot_grid(inv::Inventory& inventory)
        {
            constexpr int k_columns = 4;
            const int size = 56;
            const int gap = 4;
            const int n = inventory.slot_count();

            for (int i = 0; i < n; ++i)
            {
                ImGui::PushID(i);
                if (i % k_columns != 0)
                    ImGui::SameLine(0.f, static_cast<float>(gap));
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##slot", ImVec2(static_cast<float>(size), static_cast<float>(size)));
                const bool hovered = ImGui::IsItemHovered();
                const inv::Stack& stack = inventory.slot(i);

                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    {
                        inventory.select(i);
                        g.native_sel = -1;
                    }
                if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !stack.empty())
                    g.intents.push_back({Intent::Use, i, 1});
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !stack.empty())
                {
                    g.context_slot = i;
                    ImGui::OpenPopup("slot_menu");
                }

                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID) && !stack.empty())
                {
                    ImGui::SetDragDropPayload(k_slot_payload, &i, sizeof(i));
                    nc::Text("%s x%d", stack.def->name, stack.count);
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(k_slot_payload))
                    {
                        const int from = *static_cast<const int*>(payload->Data);
                        if (inventory.move(from, i))
                            game::backend().move(from, i); // only posts a request: fine with the session locked
                    }
                    ImGui::EndDragDropTarget();
                }

                draw_slot(ImGui::GetWindowDrawList(), static_cast<int>(pos.x), static_cast<int>(pos.y), size, stack,
                          i == inventory.selected(), hovered, 0);
                if (hovered && !stack.empty())
                    nc::SetItemTooltip("%s\n%s\nWeight: %.1f", stack.def->name, stack.def->description,
                                       stack.def->weight);
                ImGui::PopID();
            }

            if (nc::BeginPopup("slot_menu"))
            {
                const inv::Stack stack = inventory.slot(g.context_slot);
                if (stack.empty())
                    ImGui::CloseCurrentPopup();
                else
                {
                    if (nc::MenuItem("Use"))
                        g.intents.push_back({Intent::Use, g.context_slot, 1});
                    if (nc::MenuItem("Drop one"))
                        g.intents.push_back({Intent::Drop, g.context_slot, 1});
                    if (stack.count > 1 && nc::MenuItem("Drop all"))
                        g.intents.push_back({Intent::Drop, g.context_slot, stack.count});
                }
                nc::EndPopup();
            }
        }

        void items_tab(inv::Inventory& inventory)
        {
            slot_grid(inventory);
            ImGui::Dummy(ImVec2(0.f, 4.f));
            if (nc::Button("Store held item", ImVec2(120.f, 0.f)))
                g.intents.push_back({Intent::Store, 0, 0});
            nc::SetItemTooltip("Takes the item in your hands (or the first utility item you carry) into the inventory.");
            ImGui::SameLine();
            if (nc::Button("Pick up nearby", ImVec2(110.f, 0.f)))
                g.intents.push_back({Intent::Pickup, 0, 0});
            nc::SetItemTooltip("Takes the item in front of you from the ground into the inventory.");
            if (nc::Button("Use selected", ImVec2(120.f, 0.f)))
                g.intents.push_back({Intent::Use, inventory.selected(), 1});

            const inv::Limits& limits = inventory.limits();
            if (limits.max_weight > 0.f)
                nc::Text("Slots %d/%d    Weight %.1f/%.1f", inventory.used_slots(), inventory.slot_count(),
                         inventory.weight(), limits.max_weight);
            else
                nc::Text("Slots %d/%d    Weight %.1f", inventory.used_slots(), inventory.slot_count(),
                         inventory.weight());
            nc::TextDisabled("Game: %s", game::backend().connected() ? "connected" : "not connected (list only)");
            nc::TextDisabled("Click selects, double click uses, right click for more, drag to move.");
        }

        void debug_tab(inv::Inventory& inventory)
        {
            static std::vector<const char*> names;
            if (names.empty())
                for (const inv::ItemDef& def : inv::all_items())
                    names.push_back(def.name);

            if (nc::BeginGroupBox("Add items", ImVec2(0, 0)))
            {
                nc::Combo("Item:", &g.debug_item, names.data(), static_cast<int>(names.size()));
                nc::InputInt("Count:", &g.debug_count, 1, 4);
                g.debug_count = g.debug_count < 1 ? 1 : (g.debug_count > 99 ? 99 : g.debug_count);
                if (nc::Button("Add"))
                {
                    const inv::ItemDef& def = inv::all_items()[static_cast<std::size_t>(g.debug_item)];
                    const int added = inventory.add(def, g.debug_count);
                    INV_LOG("debug add %s x%d: %d stored", def.classname, g.debug_count, added);
                }
                if (nc::Button("Add one of each"))
                    for (const inv::ItemDef& def : inv::all_items())
                        inventory.add(def, 1);
                if (nc::Button("Clear"))
                    inventory.clear();
            }
            nc::EndGroupBox();
        }

        void settings_tab()
        {
            core::Config& cfg = core::config();
            bool changed = false;

            if (nc::BeginGroupBox("Keys", ImVec2(0, 0)))
            {
                std::vector<KeyRow> rows = key_rows();
                for (int i = 0; i < static_cast<int>(rows.size()); ++i)
                {
                    const std::string name =
                        g.rebinding == i ? "press a key..." : core::key_name(*rows[static_cast<std::size_t>(i)].vk);
                    ImGui::PushID(i);
                    if (nc::KeyBind(rows[static_cast<std::size_t>(i)].label, name.c_str()))
                        g.rebinding = i;
                    ImGui::PopID();
                }
            }
            nc::EndGroupBox();

            if (nc::BeginGroupBox("HUD", ImVec2(0, 0)))
            {
                changed |= nc::Checkbox("Show hotbar", &cfg.show_hotbar);
                changed |= nc::Checkbox("Store items on use", &cfg.auto_store);
                changed |= nc::Checkbox("Replace the game's weapon panel (1-0 keys, wheel)", &cfg.replace_native);
                changed |= nc::SliderFloat("Scale:", &cfg.hud_scale, 0.5f, 3.f, "%.2f");
            }
            nc::EndGroupBox();

            if (nc::BeginGroupBox("Limits", ImVec2(0, 0)))
            {
                int slots = cfg.slots;
                if (nc::InputInt("Slots:", &slots, 1, 4))
                {
                    cfg.slots = slots < 1 ? 1 : (slots > 24 ? 24 : slots);
                    changed = true;
                }
                if (nc::InputFloat("Max weight:", &cfg.max_weight, 1.f, 5.f, "%.1f"))
                {
                    cfg.max_weight = cfg.max_weight < 0.f ? 0.f : cfg.max_weight;
                    changed = true;
                }
                nc::TextDisabled("0 = no weight limit. Slots and weight apply after a restart.");
            }
            nc::EndGroupBox();

            if (changed)
                core::config_save();
        }

        void inventory_window(inv::Inventory& inventory)
        {
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowSize(ImVec2(300.f, 360.f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.45f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
            if (nc::Begin("Inventory", &g.open, ImGuiWindowFlags_NoCollapse))
            {
                if (nc::BeginTabBar("##tabs"))
                {
                    if (nc::BeginTabItem("Items"))
                    {
                        items_tab(inventory);
                        nc::EndTabItem();
                    }
                    if (nc::BeginTabItem("Debug"))
                    {
                        debug_tab(inventory);
                        nc::EndTabItem();
                    }
                    if (nc::BeginTabItem("Settings"))
                    {
                        settings_tab();
                        nc::EndTabItem();
                    }
                    nc::EndTabBar();
                }
            }
            nc::End();
        }
    } // namespace

    void init()
    {
        core::config_load();
        game::session_apply_limits();
        {
            game::Session& session = game::local_session();
            std::lock_guard lock(session.mutex);
            const std::string saved = core::inventory_file_load();
            if (!saved.empty() && session.inventory.from_text(saved))
                INV_LOG("inventory restored: %d slots used", session.inventory.used_slots());
            g.saved_revision = session.inventory.revision();
        }
        INV_LOG("hud ready, game backend: %s", game::backend().name());
    }

    void draw()
    {
        ImGui::GetIO().MouseDrawCursor = g.open;

        std::string to_save;
        {
            game::Session& session = game::local_session();
            std::lock_guard lock(session.mutex);
            if (core::config().show_hotbar)
                draw_hotbar(session);
            if (g.open)
                inventory_window(session.inventory);
            // a dedicated server keeps (and saves) a remote inventory; the local file is left alone meanwhile
            if (!session.remote && session.inventory.revision() != g.saved_revision)
            {
                g.saved_revision = session.inventory.revision();
                to_save = session.inventory.to_text();
            }
        }
        if (!to_save.empty())
            core::inventory_file_save(to_save);

        // the lock is released: the backend may take it
        std::vector<Intent> intents;
        intents.swap(g.intents);
        for (const Intent& intent : intents)
            run(intent);
    }

    bool wants_input()
    {
        return g.open;
    }

    bool unload_requested()
    {
        return g.unload;
    }

    void request_unload()
    {
        g.unload = true;
    }

    bool on_wnd_message(UINT msg, WPARAM wparam, LPARAM lparam)
    {
        // TranslateMessage turns the key press we swallow into a WM_CHAR the game's text boxes would still get.
        if (msg == WM_CHAR && g.swallow_char != 0 && GetTickCount64() < g.swallow_until)
        {
            const int ch = std::tolower(static_cast<int>(wparam));
            if (ch == g.swallow_char)
            {
                g.swallow_char = 0;
                return true;
            }
        }
        // the wheel steps through the items (the open window takes the wheel itself, and the console keeps it)
        if (msg == WM_MOUSEWHEEL && core::config().replace_native && !g.open && !game::backend().typing())
        {
            const int delta = static_cast<short>(HIWORD(wparam));
            if (delta != 0)
                step_entry(delta > 0 ? -1 : 1);
            return true;
        }
        const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        const bool up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
        if (!down && !up)
            return false;

        const int vk = static_cast<int>(wparam);
        const bool repeat = down && (lparam & (1 << 30)) != 0;

        if (down && g.rebinding >= 0)
        {
            if (!repeat)
            {
                std::vector<KeyRow> rows = key_rows();
                if (vk != VK_ESCAPE)
                    *rows[static_cast<std::size_t>(g.rebinding)].vk = vk;
                g.rebinding = -1;
                core::config_save();
            }
            return true;
        }
        if (down && vk == VK_END && (GetKeyState(VK_CONTROL) & 0x8000) != 0)
        {
            g.unload = true;
            return true;
        }
        if (down && vk == VK_ESCAPE && g.open)
        {
            g.open = false;
            return true;
        }
        if (!is_hotkey(vk))
            return false;
        if (game::backend().typing())
        {
            if (down && !repeat)
                INV_LOG("hotkey %s ignored: the game has the keyboard (console, chat or a menu)", core::key_name(vk).c_str());
            return false;
        }
        if (up)
            return true; // the game never saw the press, so it must not see the rest
        g.swallow_char = std::tolower(static_cast<int>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0x7FFF));
        g.swallow_until = GetTickCount64() + 250;
        if (repeat)
            return true;

        const core::Config& cfg = core::config();
        if (const int slot = digit_slot(vk); slot >= 0)
        {
            select_entry(slot);
            return true;
        }
        if (vk == cfg.key_toggle)
        {
            g.open = !g.open;
            return true;
        }
        if (vk == cfg.key_store)
        {
            run({Intent::Store, 0, 0});
            return true;
        }
        if (vk == cfg.key_pickup)
        {
            run({Intent::Pickup, 0, 0});
            return true;
        }

        if (vk == cfg.key_prev || vk == cfg.key_next)
        {
            step_entry(vk == cfg.key_prev ? -1 : 1);
            return true;
        }
        if (vk == cfg.key_use || vk == cfg.key_drop)
        {
            const std::string weapon = native_selection();
            if (!weapon.empty())
            {
                // a game weapon: use puts it in the hands, drop puts it on the ground
                if (vk == cfg.key_use)
                    game::backend().select_weapon(weapon);
                else
                    game::backend().drop_weapon(weapon);
                return true;
            }
        }
        int selected = 0;
        {
            game::Session& session = game::local_session();
            std::lock_guard lock(session.mutex);
            selected = session.inventory.selected();
        }
        if (vk == cfg.key_use)
            run({Intent::Use, selected, 1});
        else if (vk == cfg.key_drop)
            run({Intent::Drop, selected, 1});
        return true;
    }

} // namespace hud
