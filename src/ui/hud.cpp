#include "ui/hud.hpp"

#include "core/config.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "ui/audio.hpp"
#include "ui/icons.hpp"
#include "ui/postfx.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <imgui_nullcore/imgui_nullcore.hpp>
#include <mutex>
#include <random>
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
            bool wheel = false; // the radial wheel is open (middle mouse button held)
        } g;

        // What the wheel's segment under the cursor is: written while drawing, read by the window procedure when the
        // button is let go (they may run on different threads).
        struct WheelPick
        {
            bool valid = false;
            bool native = false; // a game weapon (Session::native index), else an inventory box
            int index = -1;
            std::string classname;
        };
        std::mutex g_wheel_mutex;
        WheelPick g_wheel_pick;

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

        // The hotbar entry for a key of the keyboard's top row.  Replacing the game's panel: 1..9 -> entries 0..8 and
        // 0 -> entry 9.  Beside it: 1 and 2 stay the game's (gun, melee / pistol), 3..9 and 0 -> the inventory's boxes
        // 0..7 (the throwable, kit and pills live in their boxes, so the game's own 3..5 are not needed).
        int digit_slot(int vk)
        {
            if (!core::shows_slots())
                return -1; // wheel only: the number keys are the game's
            const bool replace = core::config().replace_native;
            if (vk == '0')
                return replace ? 9 : 7;
            if (vk < '1' || vk > '9')
                return -1;
            if (replace)
                return vk - '1';
            return vk >= '3' ? vk - '3' : -1;
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
        // never move between boxes when the player switches.  Beside the game's own panel there are none: the entries
        // are the inventory's boxes only.
        constexpr int k_hotbar_native = 2;
        int hotbar_native(const game::Session& session)
        {
            if (!core::config().replace_native)
                return 0;
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
        // The game's own icon for a classname, fitted (aspect kept) and centred in the box; false when it has none.
        bool draw_icon(ImDrawList* dl, int x0, int y0, int x1, int y1, const char* classname, ImU32 tint = IM_COL32_WHITE)
        {
            const icons::Icon* icon = icons::find(classname);
            if (icon == nullptr)
                return false;
            float w = static_cast<float>(x1 - x0), h = static_cast<float>(y1 - y0);
            if (w / h > icon->aspect)
                w = h * icon->aspect;
            else
                h = w / icon->aspect;
            const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
            dl->AddImage(reinterpret_cast<ImTextureID>(icon->texture), ImVec2(cx - w * 0.5f, cy - h * 0.5f),
                         ImVec2(cx + w * 0.5f, cy + h * 0.5f), ImVec2(icon->u0, icon->v0), ImVec2(icon->u1, icon->v1), tint);
            return true;
        }

        // The player's reserve of the ammo gun `classname` takes (shared by every gun of that type), -1: none (a melee
        // weapon, a pistol: endless).
        int shared_reserve(const game::Session& session, const char* classname)
        {
            const char* kind = inv::ammo_kind(classname);
            const int index = kind != nullptr ? inv::ammo_index(kind) : -1;
            return index >= 0 && index < 32 ? session.ammo_pool[index] : -1;
        }

        // Boxes on demand: the weight is what limits, so the HUD shows the item boxes up to the last one in use and one
        // free box after it (where the next item can go), and the weapon boxes up to the last one in use.  A box emptied
        // in the middle stays (items keep their boxes); the grid shrinks again when the last ones empty.
        int shown_items(const inv::Inventory& inventory)
        {
            int last = -1;
            for (int i = 0; i < inventory.slot_count(); ++i)
                if (!inventory.slot(i).empty())
                    last = i;
            return std::min(inventory.slot_count(), last + 2);
        }

        int shown_weapons(const inv::Inventory& inventory)
        {
            int shown = 0;
            for (int k = 0; k < inventory.weapon_slot_count(); ++k)
                if (!inventory.slot(inventory.first_weapon_slot() + k).empty())
                    shown = k + 1;
            return shown;
        }

        // A melee script name as a title: "frying_pan" -> "Frying Pan" ("fireaxe" stays "Fireaxe").
        std::string melee_title(const std::string& script)
        {
            std::string out = script;
            bool start = true;
            for (char& ch : out)
            {
                if (ch == '_')
                {
                    ch = ' ';
                    start = true;
                }
                else if (start)
                {
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                    start = false;
                }
            }
            return out;
        }

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
                if (!draw_icon(dl, x + 2, y + 2, x + size - 2, y + size - 2,
                               stack.weapon.melee.empty() ? stack.def->classname : stack.weapon.melee.c_str()))
                {
                    const int tw = nc::CalcTextWidth(fonts.Header, stack.def->tag);
                    nc::AddText(dl, fonts.Header, x + (size - tw) / 2, y + (size - line) / 2, ImVec4(1.f, 1.f, 1.f, 1.f),
                                stack.def->tag);
                }
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

            // laid out for 900 lines, larger on taller screens (times the hud_scale setting)
            const float scale = cfg.hud_scale * std::max(1.f, display.y / 900.f);
            const int size = static_cast<int>(40.f * scale);
            const int gap = static_cast<int>(4.f * scale);
            const int n = shown_items(inventory);
            const int m = hotbar_native(session);
            const int wide = 2 * size; // the gun boxes: the game's gun icons are 2:1 to 4:1
            const int total = m * wide + n * size + (m + n - 1) * gap;
            // centred between the teammates' panels (left) and the player's own (right)
            const int x0 = static_cast<int>(display.x * 0.62f) - total / 2;
            const int y0 = static_cast<int>(display.y) - size - static_cast<int>(10.f * scale);
            const bool native_valid = g.native_sel >= 0 && g.native_sel < m &&
                                      !session.native[static_cast<std::size_t>(g.native_sel)].classname.empty();

            // one continuous bar: the game's gun slots (fixed), then the inventory, all drawn the same way
            for (int i = 0; i < m; ++i)
            {
                const game::NativeWeapon& weapon = session.native[static_cast<std::size_t>(i)];
                const int x = x0 + i * (wide + gap);
                const bool selected_here = native_valid && i == g.native_sel;
                // a gun or a melee weapon (the gun slots hold nothing of the catalogue): a neutral colour
                nc::FillRect(dl, x, y0, x + wide, y0 + size, pal.BgAlt);
                if (!weapon.classname.empty())
                {
                    const int pad = size / 6;
                    nc::FillRect(dl, x + pad, y0 + pad, x + wide - pad, y0 + size - pad, ImVec4(0.22f, 0.24f, 0.28f, 1.f));
                    nc::OutlineRect(dl, x + pad, y0 + pad, x + wide - pad, y0 + size - pad, ImVec4(0.6f, 0.64f, 0.7f, 1.f));
                    if (!draw_icon(dl, x + 2, y0 + 2, x + wide - 2, y0 + size - 2, (weapon.melee.empty() ? weapon.classname : weapon.melee).c_str()))
                    {
                        const int tw = nc::CalcTextWidth(fonts.Header, weapon.tag);
                        nc::AddText(dl, fonts.Header, x + (wide - tw) / 2, y0 + (size - static_cast<int>(fonts.Size)) / 2,
                                    ImVec4(1.f, 1.f, 1.f, 1.f), weapon.tag);
                    }
                }
                char number[8];
                std::snprintf(number, sizeof(number), "%d", i + 1);
                nc::AddText(dl, fonts.Normal, x + 3, y0 + 1, pal.TextAlt, number);
                nc::OutlineRect(dl, x, y0, x + wide, y0 + size, selected_here ? pal.BorderSelected : pal.Border);
            }
            const int items_x = x0 + m * (wide + gap);
            for (int i = 0; i < n; ++i)
                draw_slot(dl, items_x + i * (size + gap), y0, size, inventory.slot(i),
                          !native_valid && i == inventory.selected(), false, m + i + 1);

            // the weapon in the hands: rounds in the clip / in reserve, right of the bar (the game's own counter is part
            // of the weapon panel the mod hides)
            // (an item in the hands also reports a "clip": not shown)
            const bool item_in_hands = std::any_of(session.native.begin(), session.native.end(), [](const game::NativeWeapon& w) {
                const inv::ItemDef* def = inv::find_item(w.classname);
                return w.active && def != nullptr && !def->weapon();
            });
            if (!item_in_hands && (session.ammo_clip >= 0 || session.ammo_reserve > 0))
            {
                char ammo[32];
                if (session.ammo_clip >= 0 && session.ammo_reserve > 0)
                    std::snprintf(ammo, sizeof(ammo), "%d / %d", session.ammo_clip, session.ammo_reserve);
                else
                    std::snprintf(ammo, sizeof(ammo), "%d", session.ammo_clip >= 0 ? session.ammo_clip : session.ammo_reserve);
                const int line = static_cast<int>(fonts.Size);
                const int ax = x0 + total + 3 * gap;
                const int ay = y0 + (size - line) / 2;
                nc::AddText(dl, fonts.Header, ax + 1, ay + 1, ImVec4(0.f, 0.f, 0.f, 0.8f), ammo);
                nc::AddText(dl, fonts.Header, ax, ay,
                            session.ammo_clip == 0 ? ImVec4(0.95f, 0.35f, 0.3f, 1.f) : ImVec4(1.f, 1.f, 1.f, 1.f), ammo);
            }

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

        // Beside the game's own weapon panel (replace_native=0) the panel shows only the gun and the melee / pistol (its
        // throwable, kit and pills boxes are not drawn, see game.cpp hk_item_boxes); under them the mod draws the
        // inventory, three boxes per row.  What the player carries in those game slots lives in its own inventory box
        // (Stack::held: refresh_native gives an item picked up in the game a box), green while in the hands, so every
        // item has one place.
        struct Cell
        {
            int row, column;
        };

        Cell grid_cell(int slot)
        {
            return {slot / 3, slot % 3};
        }

        // One entry of the wheel's order: a game weapon (Session::native index) or an inventory box.
        struct GridEntry
        {
            bool native;
            int index;
        };

        // Whether `stack` is the item in the player's hands.
        bool in_hands(const game::Session& session, const inv::Stack& stack)
        {
            return stack.held && std::any_of(session.native.begin(), session.native.end(), [&](const game::NativeWeapon& w) {
                       return w.active && w.classname == stack.def->classname;
                   });
        }

        // Everything the wheel steps through, top to bottom: the gun, the melee / pistol, the stored weapons, then the
        // item boxes row by row, left to right, empty boxes left out.  A carried item is its box; a carried thing that
        // has no box (the inventory is full, or a gas can) comes before the item boxes.
        std::vector<GridEntry> grid_entries(const game::Session& session)
        {
            const inv::Inventory& inventory = session.inventory;
            std::vector<GridEntry> entries;
            const auto add_native = [&](int index) {
                const game::NativeWeapon& weapon = session.native[static_cast<std::size_t>(index)];
                const inv::ItemDef* def = weapon.classname.empty() ? nullptr : inv::find_item(weapon.classname);
                if (!weapon.classname.empty() && (index < 2 || def == nullptr || inventory.held_slot(*def) < 0))
                    entries.push_back({true, index});
            };
            const int natives = static_cast<int>(session.native.size());
            for (int index = 0; index < std::min(2, natives); ++index)
                add_native(index);
            for (int i = inventory.first_weapon_slot(); i < inventory.total_slots(); ++i)
                if (!inventory.slot(i).empty())
                    entries.push_back({false, i});
            for (int index = 2; index < natives; ++index)
                add_native(index);
            for (int i = 0; i < inventory.slot_count(); ++i)
                if (!inventory.slot(i).empty())
                    entries.push_back({false, i});
            return entries;
        }

        // The wheel beside the game's panel: from what is in the hands to the next / previous entry, wrapping.
        void step_grid(int direction)
        {
            GridEntry target{};
            std::string weapon;
            {
                game::Session& session = game::local_session();
                std::lock_guard lock(session.mutex);
                const std::vector<GridEntry> entries = grid_entries(session);
                if (entries.empty())
                    return;
                int position = -1;
                for (std::size_t e = 0; e < entries.size(); ++e)
                    if (entries[e].native ? session.native[static_cast<std::size_t>(entries[e].index)].active
                                          : in_hands(session, session.inventory.slot(entries[e].index)))
                        position = static_cast<int>(e);
                const int count = static_cast<int>(entries.size());
                position = position < 0 ? (direction > 0 ? 0 : count - 1) : (position + direction + count) % count;
                target = entries[static_cast<std::size_t>(position)];
                if (target.native)
                    weapon = session.native[static_cast<std::size_t>(target.index)].classname;
                else
                    session.inventory.select(target.index);
            }
            // the lock is released: the backend may take it
            if (target.native)
                game::backend().select_weapon(weapon);
            else
                game::backend().use(target.index);
        }

        void draw_column(const game::Session& session)
        {
            const inv::Inventory& inventory = session.inventory;
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const nc::Fonts& fonts = nc::GetFonts();
            const int line = static_cast<int>(fonts.Size);

            // The panel's place comes from scripts/hudlayout.res (HudWeaponSelection: 480-line units, right-aligned)
            // and was measured in the game: its item boxes are 23.3 units square, 26.7 apart, their right edge 8.3
            // units from the screen's, the pills box ends 54 units below the centre.  The grid uses the same sizes
            // (not hud_scale), so it lines up with the gun and melee boxes above it, and its first row is where the
            // panel's throwable box would be.  (Not drawn as such: a carried throwable is shown in its own box.)
            const float unit = display.y / 480.f;
            const float size_f = 23.3f * unit, pitch_f = 26.7f * unit;
            const int size = static_cast<int>(size_f);
            const float right = display.x - 8.3f * unit;
            const float thickness = std::max(1.f, unit * 0.5f);

            // The stored weapons: wide boxes like the panel's melee box (53.3 x 20 units, measured the same way; the
            // throwable box below it started 6.7 units lower), one under the other where the panel's item boxes began.
            const float weapon_w = 53.3f * unit, weapon_h = 20.f * unit, weapon_pitch = 26.7f * unit;
            const float weapons_top = display.y * 0.5f - 22.7f * unit;
            for (int k = 0; k < shown_weapons(inventory); ++k)
            {
                const int i = inventory.first_weapon_slot() + k;
                const inv::Stack& stack = inventory.slot(i);
                const ImVec2 p0(right - weapon_w, weapons_top + k * weapon_pitch), p1(right, p0.y + weapon_h);
                if (stack.empty())
                {
                    dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 60));
                    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 50), 0.f, 0, thickness);
                    continue;
                }
                dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 120));
                dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 120), 0.f, 0, thickness);
                const int x0 = static_cast<int>(p0.x), y0 = static_cast<int>(p0.y);
                const int x1 = static_cast<int>(p1.x), y1 = static_cast<int>(p1.y);
                const int pad = static_cast<int>(unit * 2.f);
                if (!stack.weapon.melee.empty())
                {
                    // a melee weapon: its kind's icon (scripts/melee/<kind>.txt), else its name
                    if (!draw_icon(dl, x0 + pad, y0 + pad, x1 - pad, y1 - pad, stack.weapon.melee.c_str()))
                        nc::AddText(dl, fonts.Header, x0 + pad * 2, y0 + (y1 - y0 - line) / 2, ImVec4(1.f, 1.f, 1.f, 1.f),
                                    melee_title(stack.weapon.melee).c_str());
                    continue;
                }
                // a gun: like the panel's gun box, its rounds on the left (magazine over reserve), its icon on the right
                const int text_w = static_cast<int>(weapon_w * 0.32f);
                if (!draw_icon(dl, x0 + text_w, y0 + pad, x1 - pad, y1 - pad, stack.def->classname))
                {
                    const int tw = nc::CalcTextWidth(fonts.Header, stack.def->name);
                    nc::AddText(dl, fonts.Header, x1 - tw - pad, y0 + (y1 - y0 - line) / 2, ImVec4(1.f, 1.f, 1.f, 1.f),
                                stack.def->name);
                }
                if (stack.weapon.clip >= 0)
                {
                    char clip[16], reserve[16];
                    std::snprintf(clip, sizeof(clip), "%d", stack.weapon.clip);
                    nc::AddText(dl, fonts.Header, x0 + pad * 2, y0 + pad, ImVec4(1.f, 1.f, 1.f, 1.f), clip);
                    // under it, like the game's gun box: the reserve it would use (the player's, shared by its type)
                    if (const int shared = shared_reserve(session, stack.def->classname); shared >= 0)
                    {
                        std::snprintf(reserve, sizeof(reserve), "%d", shared);
                        nc::AddText(dl, fonts.Normal, x0 + pad * 2, y1 - line - pad, ImVec4(0.66f, 0.66f, 0.66f, 1.f), reserve);
                    }
                }
            }

            // the item grid, under the weapon boxes
            const float first_bottom = display.y * 0.5f + 54.f * unit - 2.f * pitch_f +
                                       static_cast<float>(shown_weapons(inventory)) * weapon_pitch; // the bottom of row 0

            const ImU32 green = IM_COL32(142, 214, 57, 255);
            const int n = shown_items(inventory);
            for (int i = 0; i < n; ++i)
            {
                const Cell cell = grid_cell(i);
                const int x1 = static_cast<int>(right - (2 - cell.column) * pitch_f), x0 = x1 - size;
                const int y1 = static_cast<int>(first_bottom + cell.row * pitch_f), y0 = y1 - size;
                const inv::Stack& stack = inventory.slot(i);
                const bool selected = i == inventory.selected();
                if (stack.empty())
                {
                    // an empty box: only its place, faintly
                    dl->AddRectFilled(ImVec2(static_cast<float>(x0), static_cast<float>(y0)),
                                      ImVec2(static_cast<float>(x1), static_cast<float>(y1)), IM_COL32(0, 0, 0, 60));
                    dl->AddRect(ImVec2(static_cast<float>(x0), static_cast<float>(y0)),
                                ImVec2(static_cast<float>(x1), static_cast<float>(y1)),
                                selected ? IM_COL32(255, 255, 255, 170) : IM_COL32(255, 255, 255, 50), 0.f, 0, thickness);
                }
                else
                {
                    // green, like the game's panel, for the one in the hands; the box counts the one carried too
                    const bool hands = in_hands(session, stack);
                    const ImU32 ink = hands ? green : IM_COL32_WHITE;
                    dl->AddRectFilled(ImVec2(static_cast<float>(x0), static_cast<float>(y0)),
                                      ImVec2(static_cast<float>(x1), static_cast<float>(y1)), IM_COL32(0, 0, 0, 120));
                    const ImU32 outline = hands ? green : (selected ? IM_COL32(255, 255, 255, 230) : IM_COL32(255, 255, 255, 120));
                    dl->AddRect(ImVec2(static_cast<float>(x0), static_cast<float>(y0)),
                                ImVec2(static_cast<float>(x1), static_cast<float>(y1)), outline, 0.f, 0,
                                hands || selected ? thickness * 2.f : thickness);
                    const int pad = size / 10;
                    if (!draw_icon(dl, x0 + pad, y0 + pad, x1 - pad, y1 - pad, stack.def->classname, ink))
                    {
                        const int tw = nc::CalcTextWidth(fonts.Header, stack.def->tag);
                        nc::AddText(dl, fonts.Header, x0 + (size - tw) / 2, y0 + (size - line) / 2,
                                    ImGui::ColorConvertU32ToFloat4(ink), stack.def->tag);
                    }
                    const int shown = stack.count + (stack.held ? 1 : 0);
                    if (shown > 1)
                    {
                        char text[16];
                        std::snprintf(text, sizeof(text), "%d", shown);
                        const int tx = x1 - nc::CalcTextWidth(fonts.Header, text) - 3, ty = y1 - line - 1;
                        nc::AddText(dl, fonts.Header, tx + 1, ty + 1, ImVec4(0.f, 0.f, 0.f, 0.9f), text);
                        nc::AddText(dl, fonts.Header, tx, ty, ImGui::ColorConvertU32ToFloat4(ink), text);
                    }
                }
                // the key that takes it in hand: 3..9, 0
                if (i < 8)
                {
                    const char key[2] = {static_cast<char>(i == 7 ? '0' : '3' + i), 0};
                    nc::AddText(dl, fonts.Normal, x0 + 3, y0 + 1, ImVec4(1.f, 1.f, 1.f, 0.55f), key);
                }
            }

            // the weight limit (inventory_mod.toml, or the settings file): a bar under the grid, red once nothing more fits.  On a
            // dedicated server the weight is the server's count (its rules may weigh the items differently).
            const float max_weight = inventory.limits().max_weight;
            if (max_weight > 0.f && n > 0)
            {
                const float weight = session.remote_weight >= 0.f ? session.remote_weight : inventory.weight();
                const int rows = grid_cell(n - 1).row + 1;
                const float x0 = right - 3.f * pitch_f + (pitch_f - size_f), x1 = right;
                const float y0 = first_bottom + (rows - 1) * pitch_f + (pitch_f - size_f), y1 = y0 + static_cast<float>(line) + 4.f;
                const float fraction = std::clamp(weight / max_weight, 0.f, 1.f);
                const bool full = weight >= max_weight - 1e-3f;
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 120));
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + (x1 - x0) * fraction, y1),
                                  full ? IM_COL32(200, 50, 40, 170) : IM_COL32(255, 255, 255, 60));
                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), full ? IM_COL32(230, 70, 60, 230) : IM_COL32(255, 255, 255, 120), 0.f, 0,
                            thickness);
                char text[32];
                std::snprintf(text, sizeof(text), "%.1f / %.1f", weight, max_weight);
                const int tw = nc::CalcTextWidth(fonts.Normal, text);
                nc::AddText(dl, fonts.Normal, static_cast<int>((x0 + x1) * 0.5f) - tw / 2, static_cast<int>(y0) + 2,
                            ImVec4(1.f, 1.f, 1.f, 0.9f), text);
            }
            // no item name or status line here: the boxes say it (the messages still go to inventory_mod.log)
        }

        // ---- the infection's effect -----------------------------------------------------------------------------------
        //
        // While infected (Session::infection_until): the shader of postfx.cpp over the game's frame and the sound of
        // audio.cpp, beating like a heart that speeds up as the clock runs out; no clock, no text.  How bad it is grows
        // from 0 (just infected) to 1 (turning).  Without the shader: a sickly green tint and a dark vignette that
        // closes in.  The settings' [infection] turns the picture and the sound off.
        bool g_infection_preview = false; // the Debug tab's preview: the effect without an infection
        float g_infection_preview_severity = 0.5f;
        double g_beats = 0.0; // heartbeats counted while infected (the picture's and the sound's)
        ULONGLONG g_beat_tick = 0;

        void draw_infection(const game::Session& session)
        {
            const ULONGLONG now = GetTickCount64();
            float remaining = 0.f, severity = 0.f;
            if (g_infection_preview)
            {
                severity = g_infection_preview_severity;
                remaining = (1.f - severity) * 120.f;
            }
            else
            {
                if (session.infection_until <= now || session.infection_total_ms <= 0)
                    return;
                remaining = static_cast<float>(session.infection_until - now) / 1000.f;
                const float total = static_cast<float>(session.infection_total_ms) / 1000.f;
                severity = std::clamp(1.f - remaining / total, 0.f, 1.f);
            }
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImDrawList* dl = ImGui::GetBackgroundDrawList();

            // the heartbeat: 50 to 150 a minute, a sharp beat and a softer one after it (counted, so a faster heart
            // does not jump the phase)
            const float bpm = 50.f + 100.f * severity;
            const ULONGLONG since = g_beat_tick != 0 && now - g_beat_tick < 500 ? now - g_beat_tick : 0;
            g_beat_tick = now;
            g_beats += static_cast<double>(since) / 1000.0 * bpm / 60.0;
            const float phase = static_cast<float>(g_beats - std::floor(g_beats));
            const float beat = std::exp(-phase * 14.f) + 0.6f * std::exp(-std::fabs(phase - 0.25f) * 18.f);
            const float pulse = std::clamp(beat, 0.f, 1.2f);
            const float last = remaining < 10.f ? 1.f - remaining / 10.f : 0.f;
            const core::Config& cfg = core::config();
            if (cfg.infection_sound)
                audio::set_infection(severity, g_beats);
            if (!cfg.infection_effect)
                return;
            if (postfx::ready())
            {
                postfx::set_infection(severity, std::min(pulse, 1.f), last);
                return;
            }

            // the tint
            const int tint = static_cast<int>(std::clamp(18.f + 70.f * severity + 25.f * severity * pulse, 0.f, 140.f));
            dl->AddRectFilled(ImVec2(0.f, 0.f), display, IM_COL32(90, 125, 25, tint));

            // the vignette: four edge gradients, deeper and darker as it gets worse
            const float depth = (0.10f + 0.30f * severity + 0.05f * severity * pulse) * std::min(display.x, display.y);
            const int edge = static_cast<int>(std::clamp(90.f + 150.f * severity + 30.f * pulse, 0.f, 245.f));
            const ImU32 dark = IM_COL32(10, 30, 5, edge), clear = IM_COL32(10, 30, 5, 0);
            dl->AddRectFilledMultiColor(ImVec2(0.f, 0.f), ImVec2(display.x, depth), dark, dark, clear, clear);
            dl->AddRectFilledMultiColor(ImVec2(0.f, display.y - depth), display, clear, clear, dark, dark);
            dl->AddRectFilledMultiColor(ImVec2(0.f, 0.f), ImVec2(depth, display.y), dark, clear, clear, dark);
            dl->AddRectFilledMultiColor(ImVec2(display.x - depth, 0.f), display, clear, dark, dark, clear);
            // the last seconds: red flashes on the beat
            if (remaining < 10.f)
                dl->AddRectFilled(ImVec2(0.f, 0.f), display, IM_COL32(150, 20, 10, static_cast<int>(60.f * pulse * last)));
        }

        // ---- stamina ------------------------------------------------------------------------------------------------
        //
        // Session::stamina (the server's [stamina]): a thin bar under the crosshair while it is not full (it fades a
        // moment after it is full again; orange when low, red and pulsing while exhausted), and the vignette of
        // postfx.cpp (the edges darken from half down; while exhausted they close in and pulse with each gasp).
        // Without the shader the HUD darkens the edges itself.  The settings' [stamina] turns either off.
        bool g_stamina_preview = false; // the Debug tab's preview
        float g_stamina_preview_value = 0.3f;
        bool g_stamina_preview_exhausted = false;
        float g_stamina_shown = 1.f; // eased toward the server's value (it comes ten times a second)
        float g_stamina_alpha = 0.f; // the bar's
        float g_stamina_exhausted = 0.f; // eased 0 .. 1
        ULONGLONG g_stamina_tick = 0;
        ULONGLONG g_stamina_full_since = 0;

        void draw_stamina(const game::Session& session)
        {
            const ULONGLONG now = GetTickCount64();
            const float dt = g_stamina_tick != 0 && now - g_stamina_tick < 250 ? static_cast<float>(now - g_stamina_tick) / 1000.f : 0.f;
            g_stamina_tick = now;
            float value = session.stamina;
            bool exhausted = session.stamina_exhausted;
            if (g_stamina_preview)
            {
                value = g_stamina_preview_value;
                exhausted = g_stamina_preview_exhausted;
            }
            if (value < 0.f)
            {
                g_stamina_shown = 1.f;
                g_stamina_alpha = g_stamina_exhausted = 0.f;
                return;
            }
            g_stamina_shown += (value - g_stamina_shown) * std::min(1.f, dt * 12.f);
            g_stamina_exhausted = exhausted ? std::min(1.f, g_stamina_exhausted + dt / 0.4f) : std::max(0.f, g_stamina_exhausted - dt / 0.8f);
            if (value < 0.999f)
                g_stamina_full_since = 0;
            else if (g_stamina_full_since == 0)
                g_stamina_full_since = now;
            const bool show = g_stamina_full_since == 0 || now - g_stamina_full_since < 1200;
            g_stamina_alpha = show ? std::min(1.f, g_stamina_alpha + dt * 6.f) : std::max(0.f, g_stamina_alpha - dt * 2.f);

            const core::Config& cfg = core::config();
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            // the gasps for air: about 50 a minute
            const float t = static_cast<float>(now % 1000000ULL) / 1000.f;
            const float breath = 0.5f - 0.5f * std::cos(t * 6.2831853f * 0.85f);
            const float tired = std::clamp((0.5f - g_stamina_shown) / 0.5f, 0.f, 1.f);
            if (cfg.stamina_vignette && (tired > 0.f || g_stamina_exhausted > 0.f))
            {
                if (postfx::stamina_ready())
                    postfx::set_stamina(tired, g_stamina_exhausted, breath);
                else
                {
                    ImDrawList* dl = ImGui::GetBackgroundDrawList();
                    const float depth = (0.12f + 0.16f * tired + 0.06f * g_stamina_exhausted * breath) * std::min(display.x, display.y);
                    const int edge = static_cast<int>(std::clamp(120.f * tired + 70.f * g_stamina_exhausted * (0.6f + 0.4f * breath), 0.f, 220.f));
                    const ImU32 dark = IM_COL32(0, 0, 0, edge), clear = IM_COL32(0, 0, 0, 0);
                    dl->AddRectFilledMultiColor(ImVec2(0.f, 0.f), ImVec2(display.x, depth), dark, dark, clear, clear);
                    dl->AddRectFilledMultiColor(ImVec2(0.f, display.y - depth), display, clear, clear, dark, dark);
                    dl->AddRectFilledMultiColor(ImVec2(0.f, 0.f), ImVec2(depth, display.y), dark, clear, clear, dark);
                    dl->AddRectFilledMultiColor(ImVec2(display.x - depth, 0.f), display, clear, dark, dark, clear);
                }
            }
            if (cfg.stamina_bar && g_stamina_alpha > 0.01f)
            {
                ImDrawList* dl = ImGui::GetBackgroundDrawList();
                const float scale = cfg.hud_scale * std::max(1.f, display.y / 900.f);
                const float w = std::round(150.f * scale), h = std::max(3.f, std::round(5.f * scale));
                const float x0 = std::round((display.x - w) * 0.5f), y0 = std::round(display.y * 0.5f + 70.f * scale);
                const float a = g_stamina_alpha;
                dl->AddRectFilled(ImVec2(x0 - 1.f, y0 - 1.f), ImVec2(x0 + w + 1.f, y0 + h + 1.f),
                                  IM_COL32(0, 0, 0, static_cast<int>(120.f * a)), h * 0.5f);
                ImU32 color = IM_COL32(235, 235, 225, static_cast<int>(210.f * a));
                if (exhausted)
                    color = IM_COL32(225, 45, 30, static_cast<int>(255.f * a * (0.55f + 0.45f * breath)));
                else if (g_stamina_shown < 0.3f)
                    color = IM_COL32(240, 150, 40, static_cast<int>(230.f * a));
                const float fill = std::clamp(g_stamina_shown, 0.f, 1.f) * w;
                if (fill >= 1.f)
                    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + fill, y0 + h), color, h * 0.5f);
            }
        }

        // ---- the screamer -------------------------------------------------------------------------------------------
        //
        // When the clock runs out (Session::infection_turned_at): the settings' picture over everything for a moment,
        // lunging at the player and shaking, with the scream (audio.cpp).
        constexpr float k_screamer_seconds = 1.7f;
        ULONGLONG g_screamer_at = 0;
        ULONGLONG g_screamer_seen = 0; // the last Session::infection_turned_at looked at

        void start_screamer()
        {
            g_screamer_at = GetTickCount64();
            audio::scream();
        }

        void draw_screamer(const game::Session& session)
        {
            const ULONGLONG now = GetTickCount64();
            if (session.infection_turned_at != g_screamer_seen)
            {
                g_screamer_seen = session.infection_turned_at;
                if (core::config().infection_screamer && now - session.infection_turned_at < 3000)
                {
                    // rolled each time (settings: screamer_chance, 0 never .. 1 always)
                    static std::minstd_rand random(static_cast<unsigned>(now * 2654435761u));
                    const float roll = static_cast<float>(random() % 10000u) / 10000.f;
                    const float chance = core::config().screamer_chance;
                    INV_LOG("screamer: %s (rolled %.2f, chance %.2f)", roll < chance ? "shown" : "skipped", roll, chance);
                    if (roll < chance)
                        start_screamer();
                }
            }
            if (g_screamer_at == 0)
                return;
            const float t = static_cast<float>(now - g_screamer_at) / 1000.f;
            if (t >= k_screamer_seconds)
            {
                g_screamer_at = 0;
                return;
            }
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const float fade = t > k_screamer_seconds - 0.45f ? (k_screamer_seconds - t) / 0.45f : 1.f;
            const float flash = t < 0.08f ? 220.f / 255.f * (1.f - t / 0.08f) : 0.f; // the first instant: a white flash
            float aspect = 1.f;
            void* texture = postfx::screamer_texture(&aspect);
            // with the screen effect: drawn into the frame under the infection shader, at its worst
            const bool through_shader = core::config().infection_effect && postfx::ready();
            if (!through_shader)
                dl->AddRectFilled(ImVec2(0.f, 0.f), display, IM_COL32(0, 0, 0, static_cast<int>(255.f * fade)));

            if (texture != nullptr)
            {
                // it lunges in, then keeps creeping closer; the shake dies down
                const float zoom = 1.04f + 0.30f * (1.f - std::exp(-t * 9.f)) + 0.06f * t;
                const float cover = std::max(display.x / aspect, display.y) * zoom;
                const ImVec2 size(cover * aspect, cover);
                const std::uint32_t r = static_cast<std::uint32_t>(now * 2654435761u);
                const float shake = 34.f * std::max(0.f, 1.f - t / 1.2f) * (display.y / 1080.f);
                const ImVec2 jitter(shake * (static_cast<float>(r & 255u) / 127.5f - 1.f),
                                    shake * (static_cast<float>((r >> 8) & 255u) / 127.5f - 1.f));
                const ImVec2 at((display.x - size.x) * 0.5f + jitter.x, (display.y - size.y) * 0.5f + jitter.y);
                // flickers: a few dark frames, brighter on the hit
                const bool dark_frame = ((r >> 16) & 7u) == 0 && t > 0.2f;
                const int light = dark_frame ? 110 : 255;
                // a red sting that pulses
                const float sting = 0.25f + 0.2f * std::sin(t * 40.f);
                if (through_shader)
                {
                    const float rect[4] = {at.x, at.y, at.x + size.x, at.y + size.y};
                    const unsigned tint = (static_cast<unsigned>(255.f * fade) << 24) |
                                          (static_cast<unsigned>(light) * 0x010101u);
                    postfx::set_screamer(texture, rect, tint, fade, 70.f / 255.f * sting * fade, flash);
                    // the effect at its worst, throbbing with the shake
                    postfx::set_infection(0.92f, 0.5f + 0.5f * std::sin(t * 40.f), 0.7f);
                }
                else
                {
                    dl->AddImage(reinterpret_cast<ImTextureID>(texture), at, ImVec2(at.x + size.x, at.y + size.y),
                                 ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), IM_COL32(light, light, light, static_cast<int>(255.f * fade)));
                    dl->AddRectFilled(ImVec2(0.f, 0.f), display, IM_COL32(150, 0, 0, static_cast<int>(70.f * sting * fade)));
                }
            }
            else
                dl->AddRectFilled(ImVec2(0.f, 0.f), display, IM_COL32(170, 0, 0, static_cast<int>(200.f * fade * (0.6f + 0.4f * std::sin(t * 40.f)))));
            if (flash > 0.f && !(through_shader && texture != nullptr))
                dl->AddRectFilled(ImVec2(0.f, 0.f), display, IM_COL32(255, 255, 255, static_cast<int>(255.f * flash)));
        }

        // ---- the radial wheel (hold the middle mouse button) ---------------------------------------------------------
        //
        // Like No More Room in Hell's inventory wheel: everything the player carries around the middle of the screen,
        // in the wheel's order (gun, melee / pistol, stored weapons, items); the segment the cursor points at (by its
        // direction from the middle) is taken in hand when the button is let go or on a left click, dropped on a right
        // click.  The outer ring is the weight: red for what is carried, grey for what is left.

        struct WheelEntry
        {
            GridEntry entry;
            std::string classname; // the weapon / item: what taking it asks the game for
            std::string icon; // the icon's key: the classname, or a melee weapon's kind ("katana")
            std::string label; // the name in the middle; a melee weapon's kind
            std::string detail; // rounds, or how many
            bool active = false; // in the hands
        };

        std::vector<WheelEntry> wheel_entries(const game::Session& session)
        {
            std::vector<WheelEntry> out;
            for (const GridEntry& e : grid_entries(session))
            {
                WheelEntry w{e};
                char text[48] = {};
                if (e.native)
                {
                    const game::NativeWeapon& weapon = session.native[static_cast<std::size_t>(e.index)];
                    w.classname = weapon.classname;
                    w.icon = weapon.melee.empty() ? weapon.classname : weapon.melee; // a melee weapon's icon is its kind's
                    w.label = weapon.melee.empty() ? weapon.name : melee_title(weapon.melee);
                    w.active = weapon.active;
                    if (weapon.active && session.ammo_clip >= 0 && session.ammo_reserve >= 0)
                        std::snprintf(text, sizeof(text), "%d / %d", session.ammo_clip, session.ammo_reserve);
                }
                else
                {
                    const inv::Stack& stack = session.inventory.slot(e.index);
                    w.classname = stack.def->classname;
                    w.icon = w.classname;
                    w.label = stack.def->name;
                    w.active = in_hands(session, stack);
                    if (!stack.weapon.melee.empty())
                    {
                        w.icon = stack.weapon.melee;
                        w.label = melee_title(stack.weapon.melee);
                    }
                    else if (stack.def->weapon() && stack.weapon.clip >= 0)
                    {
                        // its magazine / the player's reserve of its ammo type (shared by the guns of that type); a gun
                        // stored before the reserves were shared also shows what it still brings
                        const int reserve = shared_reserve(session, stack.def->classname);
                        if (reserve < 0)
                            std::snprintf(text, sizeof(text), "%d", stack.weapon.clip);
                        else if (stack.weapon.ammo > 0)
                            std::snprintf(text, sizeof(text), "%d / %d +%d", stack.weapon.clip, reserve, stack.weapon.ammo);
                        else
                            std::snprintf(text, sizeof(text), "%d / %d", stack.weapon.clip, reserve);
                    }
                    else if (const int n = stack.count + (stack.held ? 1 : 0); n > 1)
                        std::snprintf(text, sizeof(text), "x%d", n);
                }
                w.detail = text;
                out.push_back(std::move(w));
            }
            return out;
        }

        // A ring segment from angle a0 to a1 (radians, 0 = right, clockwise on screen), filled.
        void ring_segment(ImDrawList* dl, ImVec2 c, float r0, float r1, float a0, float a1, ImU32 color)
        {
            const int steps = std::max(2, static_cast<int>((a1 - a0) * 24.f));
            for (int i = 0; i < steps; ++i)
            {
                const float t0 = a0 + (a1 - a0) * static_cast<float>(i) / steps;
                const float t1 = a0 + (a1 - a0) * static_cast<float>(i + 1) / steps;
                dl->AddQuadFilled(ImVec2(c.x + std::cos(t0) * r0, c.y + std::sin(t0) * r0),
                                  ImVec2(c.x + std::cos(t0) * r1, c.y + std::sin(t0) * r1),
                                  ImVec2(c.x + std::cos(t1) * r1, c.y + std::sin(t1) * r1),
                                  ImVec2(c.x + std::cos(t1) * r0, c.y + std::sin(t1) * r0), color);
            }
        }

        void draw_wheel(const game::Session& session)
        {
            constexpr float k_pi = 3.14159265f;
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const nc::Fonts& fonts = nc::GetFonts();
            const ImVec2 c(display.x * 0.5f, display.y * 0.5f);
            const float r_out = display.y * 0.27f, r_in = display.y * 0.10f;
            const ImU32 green = IM_COL32(142, 214, 57, 255);
            // the toolkit's fonts are drawn for 12 px cells: the wheel's text grows with the screen
            const float text_size = fonts.Size * std::max(1.f, display.y / 720.f);
            const int line = static_cast<int>(text_size);
            const auto text_at = [&](ImFont* font, float size, float x_centre, float y_top, ImU32 colour, const std::string& text) {
                const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.f, text.c_str());
                dl->AddText(font, size, ImVec2(std::floor(x_centre - extent.x * 0.5f) + 1.f, std::floor(y_top) + 1.f),
                            IM_COL32(0, 0, 0, 200), text.c_str());
                dl->AddText(font, size, ImVec2(std::floor(x_centre - extent.x * 0.5f), std::floor(y_top)), colour, text.c_str());
            };

            const std::vector<WheelEntry> entries = wheel_entries(session);
            const int n = static_cast<int>(entries.size());

            // the segment the cursor points at: by direction, once it is off the middle
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const float dx = mouse.x - c.x, dy = mouse.y - c.y;
            int hover = -1;
            if (n > 0 && dx * dx + dy * dy > (r_in * 0.4f) * (r_in * 0.4f))
            {
                float angle = std::atan2(dy, dx) + k_pi * 0.5f; // 0 at the top, growing clockwise
                if (angle < 0.f)
                    angle += 2.f * k_pi;
                hover = std::min(n - 1, static_cast<int>(angle / (2.f * k_pi / n)));
            }
            {
                std::lock_guard lock(g_wheel_mutex);
                g_wheel_pick = hover >= 0 ? WheelPick{true, entries[static_cast<std::size_t>(hover)].entry.native,
                                                      entries[static_cast<std::size_t>(hover)].entry.index,
                                                      entries[static_cast<std::size_t>(hover)].classname}
                                          : WheelPick{};
            }

            // the segments
            dl->AddCircleFilled(c, r_out + display.y * 0.004f, IM_COL32(0, 0, 0, 90), 96);
            const float span = n > 0 ? 2.f * k_pi / n : 2.f * k_pi;
            const float gap = n > 1 ? 0.012f : 0.f;
            for (int i = 0; i < n; ++i)
            {
                const WheelEntry& w = entries[static_cast<std::size_t>(i)];
                const float a0 = -k_pi * 0.5f + span * i + gap, a1 = a0 + span - 2.f * gap;
                ring_segment(dl, c, r_in, r_out, a0, a1, i == hover ? IM_COL32(142, 214, 57, 90) : IM_COL32(0, 0, 0, 150));
                // the icon (or a melee weapon's kind) in the middle of the segment
                const float mid = (a0 + a1) * 0.5f, rm = (r_in + r_out) * 0.5f;
                const ImVec2 p(c.x + std::cos(mid) * rm, c.y + std::sin(mid) * rm);
                const float chord = std::min((r_out - r_in) * 0.8f, 2.f * rm * std::sin(std::min(span, k_pi) * 0.5f) * 0.8f);
                const float half_w = chord * 0.5f, half_h = std::min(chord, (r_out - r_in) * 0.5f) * 0.45f;
                const ImU32 ink = w.active ? green : IM_COL32_WHITE;
                if (!draw_icon(dl, static_cast<int>(p.x - half_w), static_cast<int>(p.y - half_h),
                                               static_cast<int>(p.x + half_w), static_cast<int>(p.y + half_h),
                                               w.icon.c_str(), ink))
                    text_at(fonts.Header, text_size * 1.2f, p.x, p.y - line * 0.6f, ink, w.label);
                if (!w.detail.empty())
                    text_at(fonts.Normal, text_size, p.x, p.y + half_h + 2.f, IM_COL32(230, 230, 230, 230), w.detail);
            }

            // the weight: a ring outside, red for what is carried, grey for what is left
            const float max_weight = session.inventory.limits().max_weight;
            const float weight = session.remote_weight >= 0.f ? session.remote_weight : session.inventory.weight();
            if (max_weight > 0.f)
            {
                const float w0 = r_out + display.y * 0.008f, w1 = w0 + display.y * 0.012f;
                const float fraction = std::clamp(weight / max_weight, 0.f, 1.f);
                ring_segment(dl, c, w0, w1, -k_pi * 0.5f, -k_pi * 0.5f + 2.f * k_pi, IM_COL32(110, 110, 110, 170));
                if (fraction > 0.f)
                    ring_segment(dl, c, w0, w1, -k_pi * 0.5f, -k_pi * 0.5f + 2.f * k_pi * fraction, IM_COL32(200, 40, 35, 230));
            }

            // the middle: what is pointed at, and the weight
            dl->AddCircleFilled(c, r_in * 0.95f, IM_COL32(0, 0, 0, 160), 64);
            float y = c.y - line * 2.2f;
            const auto centred = [&](ImFont* font, float size, const std::string& text, ImU32 colour) {
                text_at(font, size, c.x, y, colour, text);
                y += size + 4.f;
            };
            if (hover >= 0)
            {
                const WheelEntry& w = entries[static_cast<std::size_t>(hover)];
                centred(fonts.Header, text_size * 1.3f, w.label, IM_COL32_WHITE);
                centred(fonts.Normal, text_size, w.detail, IM_COL32(205, 205, 205, 255));
            }
            else
            {
                centred(fonts.Header, text_size * 1.3f, n == 0 ? std::string("Nothing carried") : std::string("Inventory"),
                        IM_COL32_WHITE);
                y += text_size + 4.f;
            }
            if (max_weight > 0.f)
            {
                char text[32];
                std::snprintf(text, sizeof(text), "%.1f / %.1f", weight, max_weight);
                centred(fonts.Normal, text_size, text,
                        weight >= max_weight - 1e-3f ? IM_COL32(240, 90, 75, 255) : IM_COL32(190, 190, 190, 255));
            }
            centred(fonts.Normal, text_size * 0.85f, "release: take   right click: drop", IM_COL32(150, 150, 150, 255));
        }

        void slot_grid(inv::Inventory& inventory)
        {
            constexpr int k_columns = 4;
            const int size = 56;
            const int gap = 4;
            // the item boxes, then the weapon boxes on rows of their own
            const int n = inventory.total_slots();
            const int first_weapon = inventory.first_weapon_slot();

            for (int i = 0; i < n; ++i)
            {
                ImGui::PushID(i);
                if (i == first_weapon)
                    ImGui::Dummy(ImVec2(0.f, 2.f));
                if ((i < first_weapon ? i : i - first_weapon) % k_columns != 0)
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
                {
                    if (!stack.weapon.melee.empty())
                        nc::SetItemTooltip("%s (%s)\nWeight: %.1f", stack.def->name, stack.weapon.melee.c_str(), stack.def->weight);
                    else if (stack.def->weapon() && stack.weapon.clip >= 0)
                        nc::SetItemTooltip("%s\nMagazine: %d (the reserve is yours, shared by its ammo type)\nWeight: %.1f",
                                           stack.def->name, stack.weapon.clip, stack.def->weight);
                    else
                        nc::SetItemTooltip("%s\n%s\nWeight: %.1f", stack.def->name, stack.def->description,
                                           stack.def->weight);
                }
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

        // The reserve rounds (see game.cpp "rounds"): each ammo type the player carries, what it weighs, and its drops
        // (a magazine's worth, or all of it).  `pool` is the player's m_iAmmo, by the game's index.
        void rounds_rows(const int (&pool)[32])
        {
            bool any = false;
            for (const char* kind : inv::ammo_kinds())
            {
                const int rounds = pool[inv::ammo_index(kind)];
                if (rounds <= 0)
                    continue;
                if (!any)
                {
                    ImGui::Dummy(ImVec2(0.f, 4.f));
                    nc::Text("Rounds");
                    any = true;
                }
                ImGui::PushID(kind);
                const int magazine = std::min(inv::ammo_magazine(kind), rounds);
                char label[24];
                std::snprintf(label, sizeof(label), "Drop %d", magazine);
                if (nc::Button(label, ImVec2(64.f, 0.f)))
                    game::backend().drop_ammo(kind, magazine);
                nc::SetItemTooltip("Puts %d on the ground as an ammo pile; anyone can pick it up with E.", magazine);
                ImGui::SameLine();
                if (nc::Button("Drop all", ImVec2(64.f, 0.f)))
                    game::backend().drop_ammo(kind, 0);
                ImGui::SameLine();
                nc::Text("%s %d (%.1f)", inv::ammo_title(kind), rounds, static_cast<float>(rounds) * inv::round_weight(kind));
                ImGui::PopID();
            }
        }

        void items_tab(inv::Inventory& inventory, const int (&pool)[32])
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
            rounds_rows(pool);
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
                if (nc::Button("Infect me (test)"))
                    game::backend().infect_self();
                nc::SetItemTooltip("Starts the infection clock, as a grab or a vomit would ([infection] in inventory_mod.toml\n"
                                   "must be on). Pain pills put it back to the start (no_cure) or cure it.");
                if (nc::Button("Screamer (test)"))
                    start_screamer();
                nc::Checkbox("Preview the infection effect", &g_infection_preview);
                if (g_infection_preview)
                    nc::SliderFloat("Infection:", &g_infection_preview_severity, 0.f, 1.f, "%.2f");
                nc::Checkbox("Preview the stamina bar and vignette", &g_stamina_preview);
                if (g_stamina_preview)
                {
                    nc::SliderFloat("Stamina:", &g_stamina_preview_value, 0.f, 1.f, "%.2f");
                    nc::Checkbox("Exhausted", &g_stamina_preview_exhausted);
                }
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
                static const char* k_views[] = {"Slots and wheel", "Slots only", "Wheel only"};
                changed |= nc::Combo("Inventory view:", &cfg.inventory_view, k_views, 3);
                nc::TextDisabled("Wheel: hold the middle mouse button. Wheel only: no item boxes at all (the wheel shows");
                nc::TextDisabled("everything); the number keys and the mouse wheel are the game's again.");
                changed |= nc::Checkbox("Store items on use", &cfg.auto_store);
                changed |= nc::Checkbox("Replace the game's weapon panel (1-0 keys, wheel); off: a grid under it, keys 3-0", &cfg.replace_native);
                changed |= nc::SliderFloat("Scale:", &cfg.hud_scale, 0.5f, 3.f, "%.2f");
            }
            nc::EndGroupBox();

            if (nc::BeginGroupBox("Infection", ImVec2(0, 0)))
            {
                changed |= nc::Checkbox("Screen effect", &cfg.infection_effect);
                changed |= nc::Checkbox("Sound distortion and heartbeat", &cfg.infection_sound);
                changed |= nc::Checkbox("Screamer when the clock runs out", &cfg.infection_screamer);
                if (cfg.infection_screamer)
                    changed |= nc::SliderFloat("Screamer chance:", &cfg.screamer_chance, 0.f, 1.f, "%.2f");
                nc::TextDisabled("The picture and the scream: screamer_image / screamer_sound in inventory_mod_settings.toml.");
            }
            nc::EndGroupBox();

            if (nc::BeginGroupBox("Stamina", ImVec2(0, 0)))
            {
                changed |= nc::Checkbox("Bar under the crosshair", &cfg.stamina_bar);
                changed |= nc::Checkbox("Vignette as it runs low", &cfg.stamina_vignette);
                nc::TextDisabled("Whether there is stamina at all is [stamina] in the host's inventory_mod.toml.");
            }
            nc::EndGroupBox();

            if (nc::BeginGroupBox("Limits", ImVec2(0, 0)))
            {
                int slots = cfg.slots;
                if (nc::InputInt("Max item boxes:", &slots, 1, 4))
                {
                    cfg.slots = slots < 1 ? 1 : (slots > 24 ? 24 : slots);
                    changed = true;
                }
                int weapon_slots = cfg.weapon_slots;
                if (nc::InputInt("Max weapon boxes:", &weapon_slots, 1, 2))
                {
                    cfg.weapon_slots = weapon_slots < 0 ? 0 : (weapon_slots > 8 ? 8 : weapon_slots);
                    changed = true;
                }
                if (nc::InputFloat("Max weight:", &cfg.max_weight, 1.f, 5.f, "%.1f"))
                {
                    cfg.max_weight = cfg.max_weight < 0.f ? 0.f : cfg.max_weight;
                    changed = true;
                }
                nc::TextDisabled("0 = no weight limit. Slots and weight apply after a restart.");
                if (cfg.rules_file)
                    nc::TextDisabled("inventory_mod.toml sets these (and the item weights): it wins over this tab.");
            }
            nc::EndGroupBox();

            if (changed)
                core::config_save();
        }

        void inventory_window(inv::Inventory& inventory, const int (&pool)[32])
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
                        items_tab(inventory, pool);
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
        ImGui::GetIO().MouseDrawCursor = g.open || g.wheel;

        std::string to_save;
        {
            game::Session& session = game::local_session();
            std::lock_guard lock(session.mutex);
            if (core::config().show_hotbar && session.playing && core::shows_slots())
            {
                if (core::config().replace_native)
                    draw_hotbar(session);
                else
                    draw_column(session);
            }
            if (session.playing || g_stamina_preview)
                draw_stamina(session);
            if (session.playing || g_infection_preview)
                draw_infection(session);
            draw_screamer(session);
            if (g.wheel && !session.playing)
                g.wheel = false; // died, a menu opened ...
            if (g.wheel)
                draw_wheel(session);
            if (g.open)
                inventory_window(session.inventory, session.ammo_pool);
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
        return g.open || g.wheel;
    }

    bool wants_keyboard()
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

    namespace
    {
        bool playing()
        {
            game::Session& session = game::local_session();
            std::lock_guard lock(session.mutex);
            return session.playing;
        }

        // The wheel opens with the cursor in the middle of the window (SetCursorPos from this DLL passes the
        // overlay's cursor hook), so the first move picks a direction.
        void open_wheel()
        {
            {
                std::lock_guard lock(g_wheel_mutex);
                g_wheel_pick = WheelPick{};
            }
            if (HWND window = GetActiveWindow())
            {
                RECT rc{};
                if (GetClientRect(window, &rc))
                {
                    POINT centre{(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
                    ClientToScreen(window, &centre);
                    SetCursorPos(centre.x, centre.y);
                }
            }
            g.wheel = true;
        }

        WheelPick take_wheel_pick()
        {
            std::lock_guard lock(g_wheel_mutex);
            WheelPick pick = g_wheel_pick;
            g_wheel_pick = WheelPick{};
            return pick;
        }

        // Closes the wheel; `take`: what it points at goes into the hands, as the grid's wheel would.
        void close_wheel(bool take)
        {
            g.wheel = false;
            const WheelPick pick = take_wheel_pick();
            if (!take || !pick.valid)
                return;
            if (pick.native)
                game::backend().select_weapon(pick.classname);
            else
            {
                {
                    game::Session& session = game::local_session();
                    std::lock_guard lock(session.mutex);
                    session.inventory.select(pick.index);
                }
                game::backend().use(pick.index);
            }
        }

        // Right click on the wheel: what it points at goes on the ground (the wheel stays open).
        void drop_wheel_pick()
        {
            const WheelPick pick = take_wheel_pick();
            if (!pick.valid)
                return;
            if (pick.native)
            {
                INV_LOG("hud: wheel right click drops %s", pick.classname.c_str());
                game::backend().drop_weapon(pick.classname);
            }
            else
                game::backend().drop(pick.index, 1);
        }
    } // namespace

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
        // the radial wheel: open while the middle mouse button is held
        if (msg == WM_MBUTTONDOWN && core::shows_wheel() && !g.open && !g.wheel && playing() && !game::backend().typing())
        {
            open_wheel();
            return true;
        }
        if (g.wheel)
        {
            if (msg == WM_MBUTTONUP || msg == WM_LBUTTONDOWN)
            {
                close_wheel(true);
                return true;
            }
            if (msg == WM_RBUTTONDOWN)
            {
                drop_wheel_pick();
                return true;
            }
            if ((msg == WM_KEYDOWN && wparam == VK_ESCAPE) || msg == WM_KILLFOCUS)
            {
                close_wheel(false);
                return msg != WM_KILLFOCUS;
            }
            if (msg == WM_MOUSEWHEEL || msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONDOWN)
                return true;
        }
        // the wheel steps through the items (the open window takes the wheel itself, and the console keeps it)
        // (beside the game's panel: through the game's weapons and the grid, row by row)
        if (msg == WM_MOUSEWHEEL && (core::config().replace_native || core::config().show_hotbar) && core::shows_slots() && !g.open && playing() && !game::backend().typing())
        {
            const int delta = static_cast<short>(HIWORD(wparam));
            if (delta != 0)
            {
                if (core::config().replace_native)
                    step_entry(delta > 0 ? -1 : 1);
                else
                    step_grid(delta > 0 ? -1 : 1);
            }
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
        // in a menu, as a spectator or as infected the keys are the game's (the window can still be opened and closed)
        if (!playing() && vk != core::config().key_toggle)
            return false;
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
            if (cfg.replace_native)
                step_entry(vk == cfg.key_prev ? -1 : 1);
            else
                step_grid(vk == cfg.key_prev ? -1 : 1);
            return true;
        }
        if (vk == cfg.key_drop && !cfg.replace_native)
        {
            // beside the game's panel the drop key does what the game's own does: drops what is in the hands (a gun,
            // or an inventory item taken in hand; its box lets go of it).  Stored items are dropped from the window.
            std::string weapon;
            {
                game::Session& session = game::local_session();
                std::lock_guard lock(session.mutex);
                for (const game::NativeWeapon& w : session.native)
                    if (w.active)
                        weapon = w.classname;
            }
            if (!weapon.empty())
            {
                INV_LOG("hud: drop key (%s) drops %s, in the hands", core::key_name(vk).c_str(), weapon.c_str());
                game::backend().drop_weapon(weapon);
            }
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
                {
                    INV_LOG("hud: drop key (%s) drops %s, selected", core::key_name(vk).c_str(), weapon.c_str());
                    game::backend().drop_weapon(weapon);
                }
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
