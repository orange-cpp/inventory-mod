#include "ui/hud.hpp"

#include "core/config.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/ping.hpp"
#include "inv/melee.hpp"
#include "ui/audio.hpp"
#include "ui/icons.hpp"
#include "ui/postfx.hpp"
#include "ui/vgui.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <imgui_nullcore/imgui_nullcore.hpp>
#include <map>
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
                Pickup,
                Unload,
                Ping
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
            std::string ammo; // a round type ("assaultrifle"): the reserve's segment, which only drops
        };
        std::mutex g_wheel_mutex;
        WheelPick g_wheel_pick;
        // A right button held on a rounds segment (under g_wheel_mutex): let go early, a magazine's worth drops; held
        // k_rounds_hold_ms, all of that type (draw_wheel fills a ring meanwhile and does the drop).
        constexpr ULONGLONG k_rounds_hold_ms = 500;
        struct RoundsHold
        {
            std::string kind; // empty: none
            ULONGLONG since = 0;
        };
        RoundsHold g_rounds_hold;

        std::vector<KeyRow> key_rows()
        {
            core::Config& c = core::config();
            return {{"Open / close:", &c.key_toggle},
                    {"Previous item:", &c.key_prev},
                    {"Next item:", &c.key_next},
                    {"Use item:", &c.key_use},
                    {"Drop item:", &c.key_drop},
                    {"Store held item:", &c.key_store},
                    {"Pick up nearby:", &c.key_pickup},
                    {"Unload gun:", &c.key_unload},
                    {"Ping:", &c.key_ping}};
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
                               vk == c.key_drop || vk == c.key_store || vk == c.key_pickup ||
                               vk == c.key_unload || vk == c.key_ping);
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
            case Intent::Unload:
                backend.unload_weapon();
                break;
            case Intent::Ping:
                backend.ping();
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

        // Everything the mod shows in game but the inventory window (the boxes, the ammo counter and the status line,
        // the stamina bar, the melee ring, the radial wheel and its pointer, the screen effects' fallbacks) is drawn the
        // way the game draws its own HUD: with its VGUI surface, in its frame, in the client scheme's fonts
        // (ui/vgui.cpp calls paint_vgui on the main thread, once the game's last panel has painted).  When that is not
        // there (vgui::install failed, or a fault while painting) the overlay draws the same shapes with ImGui, under
        // the inventory window.  Both go through a Painter, so each layout is written once.
        enum class HudFont
        {
            Title, // the wheel's name in the middle
            Text, // the boxes' tags and counts, the item's name, the weight
            Ammo, // the ammo counter, the wheel's rounds: the game's ammo counter's face
            Hint // the boxes' keys, the wheel's buttons
        };

        ImU32 faded(ImU32 colour, float alpha)
        {
            const float a = static_cast<float>(colour >> IM_COL32_A_SHIFT) * std::clamp(alpha, 0.f, 1.f);
            return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
        }

        ImU32 mixed(ImU32 a, ImU32 b, float t)
        {
            const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
            return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
                                                         x.w + (y.w - x.w) * t));
        }

        // (the toolkit's palette is ImVec4's; ColorConvertFloat4ToU32 is plain arithmetic, fine on any thread)
        ImU32 rgba(const ImVec4& c)
        {
            return ImGui::ColorConvertFloat4ToU32(c);
        }

        // The box (x0, y0, x1, y1) narrowed to `aspect` (width / height), centred.
        void fit(float aspect, float& x0, float& y0, float& x1, float& y1)
        {
            float w = x1 - x0, h = y1 - y0;
            if (h <= 0.f || aspect <= 0.f)
                return;
            if (w / h > aspect)
                w = h * aspect;
            else
                h = w / aspect;
            const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
            x0 = cx - w * 0.5f;
            x1 = cx + w * 0.5f;
            y0 = cy - h * 0.5f;
            y1 = cy + h * 0.5f;
        }

        class Painter
        {
        public:
            virtual ~Painter() = default;
            virtual void rect(float x0, float y0, float x1, float y1, ImU32 colour) = 0; // filled
            virtual void polygon(const ImVec2* points, int n, ImU32 colour) = 0; // convex, filled
            virtual float line(HudFont font) = 0;
            virtual float width(HudFont font, const std::string& text) = 0;
            // Top left.  `shadow`: a dark copy a pixel down and to the right under it.
            virtual void text(HudFont font, float x, float y, ImU32 colour, const std::string& text, bool shadow = true) = 0;
            // The game's icon for `key`, fitted (aspect kept) and centred in the box; false when it has none.
            virtual bool icon(const std::string& key, float x0, float y0, float x1, float y1, ImU32 tint) = 0;

            // A frame `thickness` wide just inside the rectangle.
            void outline(float x0, float y0, float x1, float y1, ImU32 colour, float thickness = 1.f)
            {
                const float t = std::max(1.f, std::round(thickness));
                rect(x0, y0, x1, y0 + t, colour);
                rect(x0, y1 - t, x1, y1, colour);
                rect(x0, y0 + t, x0 + t, y1 - t, colour);
                rect(x1 - t, y0 + t, x1, y1 - t, colour);
            }
        };

        class ImGuiPainter final : public Painter
        {
        public:
            // `text_scale`: the toolkit's fonts are drawn for 12 px cells; over the game the text grows with the screen.
            ImGuiPainter(ImDrawList* dl, float text_scale)
                : dl_(dl), fonts_(nc::GetFonts()), size_(fonts_.Size * text_scale)
            {
            }
            void rect(float x0, float y0, float x1, float y1, ImU32 colour) override
            {
                if (x1 > x0 && y1 > y0 && (colour >> IM_COL32_A_SHIFT) != 0)
                    dl_->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), colour);
            }
            void polygon(const ImVec2* points, int n, ImU32 colour) override
            {
                dl_->AddConvexPolyFilled(points, n, colour);
            }
            float line(HudFont font) override
            {
                return size_of(font);
            }
            float width(HudFont font, const std::string& text) override
            {
                return font_of(font)->CalcTextSizeA(size_of(font), FLT_MAX, 0.f, text.c_str()).x;
            }
            void text(HudFont font, float x, float y, ImU32 colour, const std::string& text, bool shadow) override
            {
                x = std::floor(x);
                y = std::floor(y);
                if (shadow)
                    dl_->AddText(font_of(font), size_of(font), ImVec2(x + 1.f, y + 1.f),
                                 faded(IM_COL32(0, 0, 0, 200), alpha_of(colour)), text.c_str());
                dl_->AddText(font_of(font), size_of(font), ImVec2(x, y), colour, text.c_str());
            }
            bool icon(const std::string& key, float x0, float y0, float x1, float y1, ImU32 tint) override
            {
                const icons::Icon* icon = icons::find(key.c_str());
                if (icon == nullptr)
                    return false;
                fit(icon->aspect, x0, y0, x1, y1);
                dl_->AddImage(reinterpret_cast<ImTextureID>(icon->texture), ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(icon->u0, icon->v0),
                              ImVec2(icon->u1, icon->v1), tint);
                return true;
            }

        private:
            static float alpha_of(ImU32 colour)
            {
                return static_cast<float>(colour >> IM_COL32_A_SHIFT) / 255.f;
            }
            ImFont* font_of(HudFont font) const
            {
                return font == HudFont::Title || font == HudFont::Ammo ? fonts_.Header : fonts_.Normal;
            }
            float size_of(HudFont font) const
            {
                switch (font)
                {
                case HudFont::Title:
                    return size_ * 1.3f;
                case HudFont::Hint:
                    return size_ * 0.85f;
                default:
                    return size_;
                }
            }
            ImDrawList* dl_;
            const nc::Fonts& fonts_;
            float size_;
        };

        class VguiPainter final : public Painter
        {
        public:
            void rect(float x0, float y0, float x1, float y1, ImU32 colour) override
            {
                const int ix0 = pixel(x0), iy0 = pixel(y0), ix1 = pixel(x1), iy1 = pixel(y1);
                if (ix1 > ix0 && iy1 > iy0 && (colour >> IM_COL32_A_SHIFT) != 0)
                    ui::vgui::rect(ix0, iy0, ix1, iy1, colour);
            }
            void polygon(const ImVec2* points, int n, ImU32 colour) override
            {
                ui::vgui::Vertex v[64];
                n = std::min(n, 64);
                for (int i = 0; i < n; ++i)
                    v[i] = ui::vgui::Vertex{points[i].x, points[i].y};
                ui::vgui::polygon(v, n, colour);
            }
            float line(HudFont font) override
            {
                return static_cast<float>(ui::vgui::font_tall(font_of(font)));
            }
            float width(HudFont font, const std::string& text) override
            {
                return static_cast<float>(ui::vgui::text_width(font_of(font), text));
            }
            void text(HudFont font, float x, float y, ImU32 colour, const std::string& text, bool shadow) override
            {
                const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
                // the ammo font adds its light (as the game's counter): a shadow would only grey what is under it
                if (shadow && font != HudFont::Ammo)
                    ui::vgui::text(font_of(font), ix + 1, iy + 1,
                                   faded(IM_COL32(0, 0, 0, 220), static_cast<float>(colour >> IM_COL32_A_SHIFT) / 255.f), text);
                ui::vgui::text(font_of(font), ix, iy, colour, text);
            }
            bool icon(const std::string& key, float x0, float y0, float x1, float y1, ImU32 tint) override
            {
                icons::Sprite sprite;
                if (!icons::find_sprite(key.c_str(), sprite))
                    return false;
                fit(sprite.aspect, x0, y0, x1, y1);
                ui::vgui::image(sprite.material, pixel(x0), pixel(y0), pixel(x1), pixel(y1), sprite.u0, sprite.v0, sprite.u1,
                                sprite.v1, tint);
                return true;
            }

        private:
            static int pixel(float v)
            {
                return static_cast<int>(std::floor(v + 0.5f));
            }
            static ui::vgui::Font font_of(HudFont font)
            {
                switch (font)
                {
                case HudFont::Title:
                    return ui::vgui::Font::Title;
                case HudFont::Ammo:
                    return ui::vgui::Font::Ammo;
                case HudFont::Hint:
                    return ui::vgui::Font::Hint;
                default:
                    return ui::vgui::Font::Text;
                }
            }
        };

        constexpr float k_pi = 3.14159265f;

        // A ring segment from angle a0 to a1 (radians, 0 = right, clockwise on screen), filled: a quad every few
        // degrees (under a third of a pixel off the circle at the wheel's size).
        void ring_segment(Painter& p, ImVec2 c, float r0, float r1, float a0, float a1, ImU32 color)
        {
            if (a1 <= a0 || r1 <= r0 || (color >> IM_COL32_A_SHIFT) == 0)
                return;
            const int steps = std::max(2, static_cast<int>(std::ceil((a1 - a0) * 12.f)));
            for (int i = 0; i < steps; ++i)
            {
                const float t0 = a0 + (a1 - a0) * static_cast<float>(i) / steps;
                const float t1 = a0 + (a1 - a0) * static_cast<float>(i + 1) / steps;
                const ImVec2 q[4] = {ImVec2(c.x + std::cos(t0) * r0, c.y + std::sin(t0) * r0),
                                     ImVec2(c.x + std::cos(t0) * r1, c.y + std::sin(t0) * r1),
                                     ImVec2(c.x + std::cos(t1) * r1, c.y + std::sin(t1) * r1),
                                     ImVec2(c.x + std::cos(t1) * r0, c.y + std::sin(t1) * r0)};
                p.polygon(q, 4, color);
            }
        }

        void disc(Painter& p, ImVec2 c, float r, ImU32 color)
        {
            constexpr int k_points = 48;
            ImVec2 points[k_points];
            for (int i = 0; i < k_points; ++i)
            {
                const float a = 6.2831853f * static_cast<float>(i) / k_points;
                points[i] = ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
            }
            p.polygon(points, k_points, color);
        }

        // A bar with round ends (one convex polygon, wound clockwise like disc).
        void capsule(Painter& p, float x0, float y0, float x1, float y1, ImU32 color)
        {
            const float r = std::min(y1 - y0, x1 - x0) * 0.5f;
            if (r <= 0.f)
                return;
            constexpr int k_cap = 8;
            ImVec2 points[2 * (k_cap + 1)];
            const float cy = (y0 + y1) * 0.5f;
            int n = 0;
            for (int i = 0; i <= k_cap; ++i) // the right end, top to bottom
            {
                const float a = -k_pi * 0.5f + k_pi * static_cast<float>(i) / k_cap;
                points[n++] = ImVec2(x1 - r + std::cos(a) * r, cy + std::sin(a) * r);
            }
            for (int i = 0; i <= k_cap; ++i) // the left end, bottom to top
            {
                const float a = k_pi * 0.5f + k_pi * static_cast<float>(i) / k_cap;
                points[n++] = ImVec2(x0 + r + std::cos(a) * r, cy + std::sin(a) * r);
            }
            p.polygon(points, n, color);
        }

        // The screen's edges darkened `depth` deep, `colour` (its alpha) at the edge fading to nothing: nested bands of
        // rectangles (VGUI's rectangles have one colour), each as see-through as makes the stack add up to that alpha.
        void vignette(Painter& p, ImVec2 display, float depth, ImU32 colour)
        {
            constexpr int k_bands = 12;
            const float edge = static_cast<float>(colour >> IM_COL32_A_SHIFT) / 255.f;
            if (depth < 1.f || edge <= 0.f)
                return;
            const float each = 1.f - std::pow(1.f - std::min(edge, 0.995f), 1.f / k_bands);
            const ImU32 band = faded(colour | IM_COL32_A_MASK, each);
            for (int i = 1; i <= k_bands; ++i)
            {
                const float d = std::round(depth * static_cast<float>(i) / k_bands);
                p.rect(0.f, 0.f, display.x, d, band);
                p.rect(0.f, display.y - d, display.x, display.y, band);
                p.rect(0.f, 0.f, d, display.y, band);
                p.rect(display.x - d, 0.f, display.x, display.y, band);
            }
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

        // [melee] durability: what is left of a melee weapon, a thin bar along the bottom of its box (green, yellow,
        // red as it wears); nothing while it is new.
        void draw_wear(Painter& p, float x0, float y1, float x1, int wear)
        {
            if (wear <= 0)
                return;
            const float left = 1.f - std::clamp(static_cast<float>(wear) / static_cast<float>(inv::k_wear_broken), 0.f, 1.f);
            const ImU32 color = left > 0.5f ? IM_COL32(115, 217, 89, 255) : left > 0.2f ? IM_COL32(242, 204, 64, 255)
                                                                                : IM_COL32(230, 64, 51, 255);
            const float h = 3.f;
            p.rect(x0 + 3.f, y1 - h - 2.f, x1 - 3.f, y1 - 2.f, IM_COL32(0, 0, 0, 153));
            p.rect(x0 + 3.f, y1 - h - 2.f, x0 + 3.f + std::floor((x1 - x0 - 6.f) * left), y1 - 2.f, color);
        }

        // One slot, in the toolkit's colours: used by the hotbar and by the inventory window's grid.
        void draw_slot(Painter& p, float x, float y, float size, const inv::Stack& stack, bool selected, bool hovered, int number)
        {
            const nc::Palette& pal = nc::GetPalette();
            p.rect(x, y, x + size, y + size, rgba(hovered ? pal.BgFocused : pal.BgAlt));
            if (!stack.empty())
            {
                const ImVec4 c = ImGui::ColorConvertU32ToFloat4(stack.def->color);
                const float pad = std::floor(size / 6.f);
                p.rect(x + pad, y + pad, x + size - pad, y + size - pad, rgba(ImVec4(c.x * 0.4f, c.y * 0.4f, c.z * 0.4f, 1.f)));
                p.outline(x + pad, y + pad, x + size - pad, y + size - pad, rgba(c));
                if (!p.icon(stack.weapon.melee.empty() ? stack.def->classname : stack.weapon.melee, x + 2.f, y + 2.f, x + size - 2.f,
                            y + size - 2.f, IM_COL32_WHITE))
                {
                    const float tw = p.width(HudFont::Text, stack.def->tag);
                    p.text(HudFont::Text, x + (size - tw) * 0.5f, y + (size - p.line(HudFont::Text)) * 0.5f, IM_COL32_WHITE,
                           stack.def->tag);
                }
                if (!stack.weapon.melee.empty())
                    draw_wear(p, x, y + size, x + size, stack.weapon.wear);
                // the item in the player's hands lives in this box: a bar along the bottom, and it counts in the number
                if (stack.held)
                    p.rect(x + 3.f, y + size - 4.f, x + size - 3.f, y + size - 2.f, IM_COL32(115, 217, 89, 255));
                const int shown = stack.count + (stack.held ? 1 : 0);
                if (shown > 1)
                {
                    const std::string text = std::to_string(shown);
                    p.text(HudFont::Text, x + size - p.width(HudFont::Text, text) - 3.f, y + size - p.line(HudFont::Text),
                           rgba(pal.Text), text, false);
                }
            }
            if (number > 0)
                p.text(HudFont::Hint, x + 3.f, y + 1.f, rgba(pal.TextAlt), std::to_string(number), false);
            p.outline(x, y, x + size, y + size, rgba(selected ? pal.BorderSelected : pal.Border));
        }

        void draw_hotbar(const game::Session& session, Painter& p, ImVec2 display)
        {
            const inv::Inventory& inventory = session.inventory;
            const core::Config& cfg = core::config();
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
                const float x = static_cast<float>(x0 + i * (wide + gap)), y = static_cast<float>(y0);
                const float w = static_cast<float>(wide), s = static_cast<float>(size);
                const bool selected_here = native_valid && i == g.native_sel;
                // a gun or a melee weapon (the gun slots hold nothing of the catalogue): a neutral colour
                p.rect(x, y, x + w, y + s, rgba(pal.BgAlt));
                if (!weapon.classname.empty())
                {
                    const float pad = std::floor(s / 6.f);
                    p.rect(x + pad, y + pad, x + w - pad, y + s - pad, IM_COL32(56, 61, 71, 255));
                    p.outline(x + pad, y + pad, x + w - pad, y + s - pad, IM_COL32(153, 163, 179, 255));
                    if (!p.icon(weapon.melee.empty() ? weapon.classname : weapon.melee, x + 2.f, y + 2.f, x + w - 2.f, y + s - 2.f,
                                IM_COL32_WHITE))
                    {
                        const float tw = p.width(HudFont::Text, weapon.tag);
                        p.text(HudFont::Text, x + (w - tw) * 0.5f, y + (s - p.line(HudFont::Text)) * 0.5f, IM_COL32_WHITE, weapon.tag);
                    }
                    if (!weapon.melee.empty())
                        draw_wear(p, x, y + s, x + w, weapon.wear);
                }
                p.text(HudFont::Hint, x + 3.f, y + 1.f, rgba(pal.TextAlt), std::to_string(i + 1), false);
                p.outline(x, y, x + w, y + s, rgba(selected_here ? pal.BorderSelected : pal.Border));
            }
            const int items_x = x0 + m * (wide + gap);
            for (int i = 0; i < n; ++i)
                draw_slot(p, static_cast<float>(items_x + i * (size + gap)), static_cast<float>(y0), static_cast<float>(size),
                          inventory.slot(i), !native_valid && i == inventory.selected(), false, m + i + 1);

            // the weapon in the hands: rounds in the clip / in reserve, right of the bar (the game's own counter is part
            // of the weapon panel the mod hides), in the face of the game's counter
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
                const float ax = static_cast<float>(x0 + total + 3 * gap);
                const float ay = static_cast<float>(y0) + (static_cast<float>(size) - p.line(HudFont::Ammo)) * 0.5f;
                p.text(HudFont::Ammo, ax, ay, session.ammo_clip == 0 ? IM_COL32(242, 89, 77, 255) : IM_COL32_WHITE, ammo);
            }

            float y = static_cast<float>(y0) - p.line(HudFont::Text) - 3.f;
            // a status line from the game side, for a few seconds
            if (!session.message.empty() && GetTickCount64() < session.message_until)
            {
                const float tw = p.width(HudFont::Text, session.message);
                p.text(HudFont::Text, static_cast<float>(x0) + (static_cast<float>(total) - tw) * 0.5f, y, rgba(pal.TextSelected),
                       session.message);
                y -= p.line(HudFont::Text) + 2.f;
            }
            // the selected item's name above the bar
            const inv::Stack& selected = inventory.slot(inventory.selected());
            const char* name = native_valid ? session.native[static_cast<std::size_t>(g.native_sel)].name.c_str()
                                            : (selected.empty() ? nullptr : selected.def->name);
            if (name != nullptr)
            {
                const float tw = p.width(HudFont::Text, name);
                p.text(HudFont::Text, static_cast<float>(x0) + (static_cast<float>(total) - tw) * 0.5f, y, rgba(pal.TextFocused), name);
            }
        }

        // Beside the game's own weapon panel (replace_native=0) the panel shows only the gun and the melee / pistol (its
        // throwable, kit and pills boxes are not drawn, see game/weapon_panel.cpp hk_item_boxes); under them the mod draws the
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

        void draw_column(const game::Session& session, Painter& p, ImVec2 display)
        {
            const inv::Inventory& inventory = session.inventory;
            const float line = p.line(HudFont::Text);

            // The panel's place comes from scripts/hudlayout.res (HudWeaponSelection: 480-line units, right-aligned)
            // and was measured in the game: its item boxes are 23.3 units square, 26.7 apart, their right edge 8.3
            // units from the screen's, the pills box ends 54 units below the centre.  The grid uses the same sizes
            // (not hud_scale), so it lines up with the gun and melee boxes above it, and its first row is where the
            // panel's throwable box would be.  (Not drawn as such: a carried throwable is shown in its own box.)
            const float unit = display.y / 480.f;
            const float size_f = 23.3f * unit, pitch_f = 26.7f * unit;
            const float size = std::floor(size_f);
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
                const float x0 = std::floor(right - weapon_w), y0 = std::floor(weapons_top + k * weapon_pitch);
                const float x1 = std::floor(right), y1 = std::floor(weapons_top + k * weapon_pitch + weapon_h);
                if (stack.empty())
                {
                    p.rect(x0, y0, x1, y1, IM_COL32(0, 0, 0, 60));
                    p.outline(x0, y0, x1, y1, IM_COL32(255, 255, 255, 50), thickness);
                    continue;
                }
                p.rect(x0, y0, x1, y1, IM_COL32(0, 0, 0, 120));
                p.outline(x0, y0, x1, y1, IM_COL32(255, 255, 255, 120), thickness);
                const float pad = std::floor(unit * 2.f);
                if (!stack.weapon.melee.empty())
                {
                    // a melee weapon: its kind's icon (scripts/melee/<kind>.txt), else its name
                    if (!p.icon(stack.weapon.melee, x0 + pad, y0 + pad, x1 - pad, y1 - pad, IM_COL32_WHITE))
                        p.text(HudFont::Text, x0 + pad * 2.f, y0 + (y1 - y0 - line) * 0.5f, IM_COL32_WHITE, melee_title(stack.weapon.melee));
                    draw_wear(p, x0, y1, x1, stack.weapon.wear);
                    continue;
                }
                // a gun: like the panel's gun box, its rounds on the left (magazine over reserve), its icon on the right
                const float text_w = std::floor(weapon_w * 0.32f);
                if (!p.icon(stack.def->classname, x0 + text_w, y0 + pad, x1 - pad, y1 - pad, IM_COL32_WHITE))
                {
                    const float tw = p.width(HudFont::Text, stack.def->name);
                    p.text(HudFont::Text, x1 - tw - pad, y0 + (y1 - y0 - line) * 0.5f, IM_COL32_WHITE, stack.def->name);
                }
                if (stack.weapon.clip >= 0)
                {
                    p.text(HudFont::Ammo, x0 + pad * 2.f, y0 + pad, IM_COL32_WHITE, std::to_string(stack.weapon.clip));
                    // under it, like the game's gun box: the reserve it would use (the player's, shared by its type)
                    if (const int shared = shared_reserve(session, stack.def->classname); shared >= 0)
                        p.text(HudFont::Hint, x0 + pad * 2.f, y1 - p.line(HudFont::Hint) - pad, IM_COL32(168, 168, 168, 255),
                               std::to_string(shared));
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
                const float x1 = std::floor(right - (2 - cell.column) * pitch_f), x0 = x1 - size;
                const float y1 = std::floor(first_bottom + cell.row * pitch_f), y0 = y1 - size;
                const inv::Stack& stack = inventory.slot(i);
                const bool selected = i == inventory.selected();
                if (stack.empty())
                {
                    // an empty box: only its place, faintly
                    p.rect(x0, y0, x1, y1, IM_COL32(0, 0, 0, 60));
                    p.outline(x0, y0, x1, y1, selected ? IM_COL32(255, 255, 255, 170) : IM_COL32(255, 255, 255, 50), thickness);
                }
                else
                {
                    // green, like the game's panel, for the one in the hands; the box counts the one carried too
                    const bool hands = in_hands(session, stack);
                    const ImU32 ink = hands ? green : IM_COL32_WHITE;
                    p.rect(x0, y0, x1, y1, IM_COL32(0, 0, 0, 120));
                    const ImU32 outline = hands ? green : (selected ? IM_COL32(255, 255, 255, 230) : IM_COL32(255, 255, 255, 120));
                    p.outline(x0, y0, x1, y1, outline, hands || selected ? thickness * 2.f : thickness);
                    const float pad = std::floor(size / 10.f);
                    if (!p.icon(stack.def->classname, x0 + pad, y0 + pad, x1 - pad, y1 - pad, ink))
                    {
                        const float tw = p.width(HudFont::Text, stack.def->tag);
                        p.text(HudFont::Text, x0 + (size - tw) * 0.5f, y0 + (size - line) * 0.5f, ink, stack.def->tag);
                    }
                    const int shown = stack.count + (stack.held ? 1 : 0);
                    if (shown > 1)
                    {
                        const std::string text = std::to_string(shown);
                        p.text(HudFont::Text, x1 - p.width(HudFont::Text, text) - 3.f, y1 - line - 1.f, ink, text);
                    }
                }
                // the key that takes it in hand: 3..9, 0
                if (i < 8)
                {
                    const char key[2] = {static_cast<char>(i == 7 ? '0' : '3' + i), 0};
                    p.text(HudFont::Hint, x0 + 3.f, y0 + 1.f, IM_COL32(255, 255, 255, 140), key, false);
                }
            }

            // the weight limit (inventory_mod.toml, or the settings file): a bar under the grid, red once nothing more fits.  On a
            // dedicated server the weight is the server's count (its rules may weigh the items differently).
            const float max_weight = inventory.limits().max_weight;
            if (max_weight > 0.f && n > 0)
            {
                const float weight = session.remote_weight >= 0.f ? session.remote_weight : inventory.weight();
                const int rows = grid_cell(n - 1).row + 1;
                const float x0 = std::floor(right - 3.f * pitch_f + (pitch_f - size_f)), x1 = std::floor(right);
                const float y0 = std::floor(first_bottom + (rows - 1) * pitch_f + (pitch_f - size_f));
                const float y1 = y0 + std::ceil(p.line(HudFont::Hint)) + 4.f;
                const float fraction = std::clamp(weight / max_weight, 0.f, 1.f);
                const bool full = weight >= max_weight - 1e-3f;
                p.rect(x0, y0, x1, y1, IM_COL32(0, 0, 0, 120));
                p.rect(x0, y0, x0 + std::floor((x1 - x0) * fraction), y1, full ? IM_COL32(200, 50, 40, 170) : IM_COL32(255, 255, 255, 60));
                p.outline(x0, y0, x1, y1, full ? IM_COL32(230, 70, 60, 230) : IM_COL32(255, 255, 255, 120), thickness);
                char text[32];
                std::snprintf(text, sizeof(text), "%.1f / %.1f", weight, max_weight);
                p.text(HudFont::Hint, (x0 + x1) * 0.5f - p.width(HudFont::Hint, text) * 0.5f, y0 + 2.f, IM_COL32(255, 255, 255, 230), text);
            }
            // no item name or status line here: the boxes say it (the messages still go to inventory_mod.log)
        }

        // ---- the screen effects' fallback ------------------------------------------------------------------------------
        //
        // The infection and the stamina are first of all postfx.cpp's shaders, which read the game's frame and so stay
        // with Direct3D (the call queue under the game's HUD, Present over it).  Without the shader the HUD tints and
        // darkens the screen itself: worked out each frame in draw() (where the shader's values are set too, and the
        // heartbeat is counted) and painted by paint_hud, under the session lock both.
        struct ScreenTint
        {
            ImU32 tint = 0; // over the whole screen
            float depth = 0.f; // the vignette, how deep (pixels) ...
            ImU32 edge = 0; // ... and its colour at the edge
            ImU32 flash = 0; // over the whole screen, over the rest
        };
        ScreenTint g_infection_tint;
        ScreenTint g_stamina_tint;

        void paint_tint(Painter& p, ImVec2 display, const ScreenTint& t)
        {
            p.rect(0.f, 0.f, display.x, display.y, t.tint);
            vignette(p, display, t.depth, t.edge);
            p.rect(0.f, 0.f, display.x, display.y, t.flash);
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

        void update_infection(const game::Session& session, ImVec2 display)
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
                const postfx::Layer layer = !cfg.infection_cover_game_hud ? postfx::Layer::UnderGameHud
                                            : !cfg.infection_cover_mod_hud ? postfx::Layer::UnderModHud
                                                                           : postfx::Layer::OverAll;
                postfx::set_infection(severity, std::min(pulse, 1.f), last, layer);
                return;
            }

            // the tint; the vignette, deeper and darker as it gets worse; the last seconds: red flashes on the beat
            ScreenTint& t = g_infection_tint;
            t.tint = IM_COL32(90, 125, 25, static_cast<int>(std::clamp(18.f + 70.f * severity + 25.f * severity * pulse, 0.f, 140.f)));
            t.depth = (0.10f + 0.30f * severity + 0.05f * severity * pulse) * std::min(display.x, display.y);
            t.edge = IM_COL32(10, 30, 5, static_cast<int>(std::clamp(90.f + 150.f * severity + 30.f * pulse, 0.f, 245.f)));
            if (remaining < 10.f)
                t.flash = IM_COL32(150, 20, 10, static_cast<int>(60.f * pulse * last));
        }

        // ---- stamina ------------------------------------------------------------------------------------------------
        //
        // Session::stamina (the server's [stamina]): a thin bar under the crosshair while it is not full (it fades a
        // moment after it is full again; orange when low, red and pulsing while exhausted), and the vignette of
        // postfx.cpp (the edges darken from half down; while exhausted they close in and pulse with each gasp).
        // Without the shader the HUD darkens the edges itself.  The settings' [stamina] turns either off.  The values
        // ease in draw(), each frame; paint_hud draws the bar from them.
        bool g_stamina_preview = false; // the Debug tab's preview
        float g_stamina_preview_value = 0.3f;
        bool g_stamina_preview_exhausted = false;
        float g_stamina_shown = 1.f; // eased toward the server's value (it comes ten times a second)
        float g_stamina_alpha = 0.f; // the bar's
        float g_stamina_exhausted = 0.f; // eased 0 .. 1
        bool g_stamina_exhausted_now = false;
        float g_stamina_breath = 0.f;
        bool g_stamina_live = false; // updated this frame: the bar may show
        ULONGLONG g_stamina_tick = 0;
        ULONGLONG g_stamina_full_since = 0;

        void update_stamina(const game::Session& session, ImVec2 display)
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
            g_stamina_live = true;
            g_stamina_exhausted_now = exhausted;
            g_stamina_shown += (value - g_stamina_shown) * std::min(1.f, dt * 12.f);
            g_stamina_exhausted = exhausted ? std::min(1.f, g_stamina_exhausted + dt / 0.4f) : std::max(0.f, g_stamina_exhausted - dt / 0.8f);
            if (value < 0.999f)
                g_stamina_full_since = 0;
            else if (g_stamina_full_since == 0)
                g_stamina_full_since = now;
            const bool show = g_stamina_full_since == 0 || now - g_stamina_full_since < 1200;
            g_stamina_alpha = show ? std::min(1.f, g_stamina_alpha + dt * 6.f) : std::max(0.f, g_stamina_alpha - dt * 2.f);

            const core::Config& cfg = core::config();
            // the gasps for air: about 50 a minute
            const float t = static_cast<float>(now % 1000000ULL) / 1000.f;
            const float breath = 0.5f - 0.5f * std::cos(t * 6.2831853f * 0.85f);
            g_stamina_breath = breath;
            const float tired = std::clamp((0.5f - g_stamina_shown) / 0.5f, 0.f, 1.f);
            if (cfg.stamina_vignette && (tired > 0.f || g_stamina_exhausted > 0.f))
            {
                if (postfx::stamina_ready())
                {
                    const postfx::Layer layer = !cfg.stamina_cover_game_hud ? postfx::Layer::UnderGameHud
                                                : !cfg.stamina_cover_mod_hud ? postfx::Layer::UnderModHud
                                                                             : postfx::Layer::OverAll;
                    postfx::set_stamina(tired, g_stamina_exhausted, breath, layer);
                }
                else
                {
                    g_stamina_tint.depth = (0.12f + 0.16f * tired + 0.06f * g_stamina_exhausted * breath) * std::min(display.x, display.y);
                    g_stamina_tint.edge =
                        IM_COL32(0, 0, 0, static_cast<int>(std::clamp(120.f * tired + 70.f * g_stamina_exhausted * (0.6f + 0.4f * breath), 0.f, 220.f)));
                }
            }
        }

        void draw_stamina_bar(Painter& p, ImVec2 display)
        {
            if (!g_stamina_live || !core::config().stamina_bar || g_stamina_alpha <= 0.01f)
                return;
            const float scale = core::config().hud_scale * std::max(1.f, display.y / 900.f);
            const float w = std::round(150.f * scale), h = std::max(3.f, std::round(5.f * scale));
            const float x0 = std::round((display.x - w) * 0.5f), y0 = std::round(display.y * 0.5f + 70.f * scale);
            const float a = g_stamina_alpha;
            capsule(p, x0 - 1.f, y0 - 1.f, x0 + w + 1.f, y0 + h + 1.f, IM_COL32(0, 0, 0, static_cast<int>(120.f * a)));
            ImU32 color = IM_COL32(235, 235, 225, static_cast<int>(210.f * a));
            if (g_stamina_exhausted_now)
                color = IM_COL32(225, 45, 30, static_cast<int>(255.f * a * (0.55f + 0.45f * g_stamina_breath)));
            else if (g_stamina_shown < 0.3f)
                color = IM_COL32(240, 150, 40, static_cast<int>(230.f * a));
            const float fill = std::clamp(g_stamina_shown, 0.f, 1.f) * w;
            if (fill >= 1.f)
                capsule(p, x0, y0, x0 + fill, y0 + h, color);
        }

        // ---- the melee wind-up --------------------------------------------------------------------------------------
        //
        // Session::melee_winding_since (the client's input, [melee]): a ring round the crosshair fills while the attack
        // is held, over the server's heavy_hold; full, the swing let go is a heavy one (the ring turns orange and
        // pulses), and letting go flashes it outward.  A tap shows nothing (it appears after a moment of holding).
        // While exhausted it stays grey: the swing will be light.  The settings' [melee] ring turns it off.
        void draw_melee(const game::Session& session, Painter& p, ImVec2 display)
        {
            if (!core::config().melee_ring || session.melee_hold <= 0.f)
                return;
            const ULONGLONG now = GetTickCount64();
            const float scale = core::config().hud_scale * std::max(1.f, display.y / 900.f);
            const ImVec2 center(std::round(display.x * 0.5f), std::round(display.y * 0.5f));
            const float radius = 26.f * scale, thick = std::max(2.f, 3.f * scale);
            constexpr float k_top = -1.5707963f, k_turn = 6.2831853f;
            // a stroke `width` wide along the circle of `r`, from a0 over `span`
            const auto arc = [&](float r, float a0, float span, float width, ImU32 color) {
                ring_segment(p, center, r - width * 0.5f, r + width * 0.5f, a0, a0 + span, color);
            };
            if (session.melee_winding_since != 0 && now >= session.melee_winding_since)
            {
                const float held = static_cast<float>(now - session.melee_winding_since) / 1000.f;
                const float charge = std::clamp(held / session.melee_hold, 0.f, 1.f);
                const float alpha = std::clamp((held - 0.12f) / 0.1f, 0.f, 1.f);
                if (alpha > 0.f)
                {
                    const bool full = charge >= 1.f && session.melee_can_heavy;
                    const float pulse = full ? 0.5f + 0.5f * std::sin(static_cast<float>(now % 100000ULL) / 1000.f * 12.f) : 0.f;
                    arc(radius, 0.f, k_turn, thick + 2.f, IM_COL32(0, 0, 0, static_cast<int>(110.f * alpha)));
                    ImU32 color = IM_COL32(235, 235, 225, static_cast<int>(220.f * alpha));
                    if (!session.melee_can_heavy)
                        color = IM_COL32(150, 150, 150, static_cast<int>(150.f * alpha));
                    else if (full)
                        color = IM_COL32(240, 150, 40, static_cast<int>((200.f + 55.f * pulse) * alpha));
                    if (charge > 0.f)
                        arc(radius, k_top, k_turn * charge, full ? thick + 1.f * pulse : thick, color);
                }
            }
            // the melee weapon in the hands, once half worn: what is left of it under the ring, red when nearly broken
            for (const game::NativeWeapon& w : session.native)
                if (w.active && !w.melee.empty() && w.wear >= inv::k_wear_broken / 2)
                {
                    const float left = 1.f - std::clamp(static_cast<float>(w.wear) / static_cast<float>(inv::k_wear_broken), 0.f, 1.f);
                    const float wide = 40.f * scale, high = std::max(2.f, 3.f * scale);
                    const ImVec2 p0(center.x - wide * 0.5f, center.y + radius + 8.f * scale);
                    p.rect(p0.x, p0.y, p0.x + wide, p0.y + high, IM_COL32(0, 0, 0, 120));
                    p.rect(p0.x, p0.y, p0.x + wide * left, p0.y + high, left > 0.2f ? IM_COL32(240, 200, 60, 220) : IM_COL32(230, 60, 50, 230));
                }
            // a heavy swing let go: the ring flashes outward
            if (session.melee_heavy_at != 0 && now >= session.melee_heavy_at && now - session.melee_heavy_at < 300)
            {
                const float t = static_cast<float>(now - session.melee_heavy_at) / 300.f;
                arc(radius * (1.f + 0.6f * t), 0.f, k_turn, thick * (1.f - 0.5f * t), IM_COL32(240, 150, 40, static_cast<int>(230.f * (1.f - t))));
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
            const float fade = t > k_screamer_seconds - 0.45f ? (k_screamer_seconds - t) / 0.45f : 1.f;
            const float flash = t < 0.08f ? 220.f / 255.f * (1.f - t / 0.08f) : 0.f; // the first instant: a white flash
            float aspect = 1.f;
            void* texture = postfx::screamer_texture(&aspect);
            const float pulse = std::sin(t * 40.f);
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
            // a red sting that pulses (without a picture, a stronger red over the black)
            const float red = texture != nullptr ? 70.f / 255.f * (0.25f + 0.2f * pulse) * fade : 200.f / 255.f * fade * (0.6f + 0.4f * pulse);

            // Drawn by postfx.cpp whenever it has the device: into the game's own frame (after the mod's VGUI HUD) where
            // a stream's capture sees it, else at Present; with the screen effect, under the infection shader at its
            // worst.  ImGui only without the device.
            if (postfx::available())
            {
                const float rect[4] = {at.x, at.y, at.x + size.x, at.y + size.y};
                const unsigned tint = (static_cast<unsigned>(255.f * fade) << 24) | (static_cast<unsigned>(light) * 0x010101u);
                postfx::set_screamer(texture, rect, tint, fade, red, flash);
                if (core::config().infection_effect && postfx::ready())
                    postfx::set_infection(0.92f, 0.5f + 0.5f * pulse, 0.7f); // the effect at its worst, throbbing with the shake
                return;
            }
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            dl->AddRectFilled(ImVec2(0.f, 0.f), display, IM_COL32(0, 0, 0, static_cast<int>(255.f * fade)));
            if (texture != nullptr)
                dl->AddImage(reinterpret_cast<ImTextureID>(texture), at, ImVec2(at.x + size.x, at.y + size.y), ImVec2(0.f, 0.f),
                             ImVec2(1.f, 1.f), IM_COL32(light, light, light, static_cast<int>(255.f * fade)));
            dl->AddRectFilled(ImVec2(0.f, 0.f), display, IM_COL32(150, 0, 0, static_cast<int>(255.f * red)));
            if (flash > 0.f)
                dl->AddRectFilled(ImVec2(0.f, 0.f), display, IM_COL32(255, 255, 255, static_cast<int>(255.f * flash)));
        }

        // ---- the radial wheel (hold the middle mouse button) ---------------------------------------------------------
        //
        // Like No More Room in Hell's inventory wheel: everything the player carries around the middle of the screen,
        // weapons first, then items, each in the place it first had (a swap does not move them); the segment the cursor
        // points at (by its direction from the middle) is taken in hand when the button is let go or on a left click,
        // dropped on a right click.  After them the reserve rounds, one segment per type in its colour: a right click
        // drops a magazine's worth, the right button held half a second all of them (they cannot be taken in hand).
        // Each segment's share of the circle grows with what it weighs (a part of the circle is shared out evenly, so
        // the light ones stay easy to point at).  The outer ring is the weight: what is carried (green when light, red
        // at the limit), grey for what is left.
        //
        // Like the rest of the HUD it is drawn through a Painter (VGUI, else ImGui: see "drawing" above), and while VGUI
        // draws it, it draws the pointer too (draw_pointer).  The pointed-at segment follows the cursor in the window procedure itself (wheel_mouse_moved,
        // from the layout the last drawing left), not a frame later; the wheel opens with a quick grow, and the segment
        // under the cursor slides out, lights up and ticks (the game's menu focus sound).

        struct WheelEntry
        {
            GridEntry entry;
            std::string classname; // the weapon / item: what taking it asks the game for
            std::string icon; // the icon's key: the classname, or a melee weapon's kind ("katana")
            std::string label; // the name in the middle; a melee weapon's kind
            std::string detail; // rounds, or how many
            bool active = false; // in the hands
            std::string ammo; // a round type: the reserve's segment
            unsigned color = 0; // its colour (0xRRGGBB)
            float weight = 0.f; // what it weighs: its share of the circle
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
                    w.weight = inv::weapon_weight(weapon.classname);
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
                    w.weight = stack.def->weight * static_cast<float>(stack.count + (stack.held ? 1 : 0));
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
            // Each weapon and item keeps its place: in the grid's order a gun taken out of a box goes first (the hands)
            // and the one it replaces into that box, so the two traded places.  The places are the order they were first
            // seen in (by kind, a second one of the same kind after the first), weapons before items.
            static std::vector<std::string> order;
            std::map<std::string, int> seen;
            std::vector<std::pair<int, std::size_t>> rank; // (weapon 0 / item 1) * big + place, index
            for (std::size_t i = 0; i < out.size(); ++i)
            {
                const WheelEntry& w = out[i];
                const bool item = !w.entry.native && !session.inventory.slot(w.entry.index).def->weapon();
                const std::string key = w.classname + "|" + w.icon + "#" + std::to_string(seen[w.classname + "|" + w.icon]++);
                auto at = std::find(order.begin(), order.end(), key);
                if (at == order.end())
                    at = order.insert(order.end(), key);
                rank.emplace_back((item ? 1 << 20 : 0) + static_cast<int>(at - order.begin()), i);
            }
            std::stable_sort(rank.begin(), rank.end());
            std::vector<WheelEntry> placed;
            placed.reserve(out.size());
            for (const auto& [place, i] : rank)
                placed.push_back(std::move(out[i]));
            out = std::move(placed);
            // the reserve rounds, one segment per type carried
            for (const char* kind : inv::ammo_kinds())
            {
                const int rounds = session.ammo_pool[inv::ammo_index(kind)];
                if (rounds <= 0)
                    continue;
                WheelEntry w{};
                w.ammo = kind;
                w.label = inv::ammo_title(kind);
                w.color = inv::ammo_color(kind);
                w.weight = static_cast<float>(rounds) * inv::round_weight(kind);
                char text[48];
                std::snprintf(text, sizeof(text), "%d (%.1f)", rounds, w.weight);
                w.detail = text;
                out.push_back(std::move(w));
            }
            return out;
        }

        // Each entry's share of the circle (radians, adding up to 2 pi): 40 % shared out evenly, the rest by weight
        // (evenly when nothing weighs anything).
        std::vector<float> wheel_spans(const std::vector<WheelEntry>& entries)
        {
            constexpr float k_two_pi = 6.2831853f, k_even = 0.4f;
            const std::size_t n = entries.size();
            std::vector<float> spans(n, n > 0 ? k_two_pi / static_cast<float>(n) : 0.f);
            float total = 0.f;
            for (const WheelEntry& w : entries)
                total += std::max(0.f, w.weight);
            if (n == 0 || total <= 0.f)
                return spans;
            for (std::size_t i = 0; i < n; ++i)
                spans[i] = k_two_pi * (k_even / static_cast<float>(n) + (1.f - k_even) * std::max(0.f, entries[i].weight) / total);
            return spans;
        }

        // Where the last drawing put the segments, for the window procedure (under g_wheel_mutex): the pick follows
        // the cursor as the messages come, and the release takes what is under the cursor then.
        struct WheelLayout
        {
            bool valid = false;
            float cx = 0.f, cy = 0.f;
            float dead = 0.f; // within this distance of the middle nothing is pointed at (letting go there cancels)
            std::vector<float> ends; // each segment's end (radians from the top, clockwise)
            std::vector<WheelPick> picks;
        };
        WheelLayout g_wheel_layout;
        int g_wheel_ticked = -1; // (main thread) the segment the last tick was for
        std::atomic<int> g_cursor_x{-1}, g_cursor_y{-1}; // the last WM_MOUSEMOVE, client coordinates
        std::atomic<ULONGLONG> g_wheel_opened{0};

        int wheel_hover(const WheelLayout& layout, float x, float y)
        {
            const float dx = x - layout.cx, dy = y - layout.cy;
            if (!layout.valid || layout.ends.empty() || dx * dx + dy * dy <= layout.dead * layout.dead)
                return -1;
            float angle = std::atan2(dy, dx) + k_pi * 0.5f; // 0 at the top, growing clockwise
            if (angle < 0.f)
                angle += 2.f * k_pi;
            for (std::size_t i = 0; i < layout.ends.size(); ++i)
                if (angle < layout.ends[i])
                    return static_cast<int>(i);
            return static_cast<int>(layout.ends.size()) - 1;
        }

        // Main thread (the window procedure): the cursor moved while the wheel is open.
        void wheel_mouse_moved(int x, int y)
        {
            int hover = -1;
            {
                std::lock_guard lock(g_wheel_mutex);
                if (!g_wheel_layout.valid)
                    return;
                hover = wheel_hover(g_wheel_layout, static_cast<float>(x), static_cast<float>(y));
                g_wheel_pick = hover >= 0 ? g_wheel_layout.picks[static_cast<std::size_t>(hover)] : WheelPick{};
            }
            if (hover != g_wheel_ticked)
            {
                g_wheel_ticked = hover;
                if (hover >= 0 && core::config().wheel_sounds)
                    ui::vgui::play_sound("ui/menu_focus.wav");
            }
        }


        // What moves on the wheel, from frame to frame (whichever thread draws it: one at a time).
        struct WheelMotion
        {
            ULONGLONG opened = 0; // the wheel these are for
            ULONGLONG last = 0;
            std::vector<float> lit; // each segment's hover, eased 0 .. 1
            float pointer = 0.f; // the pointer's angle, eased
            bool pointer_set = false;
        };
        WheelMotion g_wheel_motion;

        void draw_wheel(const game::Session& session, Painter& p, ImVec2 display)
        {
            const ImU32 green = IM_COL32(142, 214, 57, 255);
            const ULONGLONG now = GetTickCount64();

            // the motion: a quick grow and fade in when it opens, then each segment eases toward lit or not
            WheelMotion& m = g_wheel_motion;
            const ULONGLONG opened = g_wheel_opened.load();
            if (m.opened != opened)
                m = WheelMotion{opened, now};
            const float dt = std::clamp(static_cast<float>(now - m.last) / 1000.f, 0.f, 0.1f);
            m.last = now;
            const float open_t = std::clamp(static_cast<float>(now - opened) / 140.f, 0.f, 1.f);
            const float ease = 1.f - (1.f - open_t) * (1.f - open_t) * (1.f - open_t);
            const float alpha = ease, grow = 0.86f + 0.14f * ease;

            const ImVec2 c(display.x * 0.5f, display.y * 0.5f);
            const float r_out = display.y * 0.27f * grow, r_in = display.y * 0.11f * grow;
            const float unit = display.y / 1080.f; // a pixel at 1080 lines

            const std::vector<WheelEntry> entries = wheel_entries(session);
            const int n = static_cast<int>(entries.size());
            const std::vector<float> spans = wheel_spans(entries);
            std::vector<float> starts(spans.size(), 0.f); // from the top, clockwise
            for (std::size_t i = 1; i < spans.size(); ++i)
                starts[i] = starts[i - 1] + spans[i - 1];

            // the layout for the window procedure, and what the cursor points at now
            int hover = -1;
            {
                WheelLayout layout;
                layout.valid = true;
                layout.cx = c.x;
                layout.cy = c.y;
                layout.dead = display.y * 0.11f * 0.3f; // (the full size's: the same while it grows)
                for (std::size_t i = 0; i < spans.size(); ++i)
                {
                    layout.ends.push_back(starts[i] + spans[i]);
                    const WheelEntry& w = entries[i];
                    layout.picks.push_back(WheelPick{true, w.entry.native, w.entry.index, w.classname, w.ammo});
                }
                const int mx = g_cursor_x.load(), my = g_cursor_y.load();
                if (mx >= 0)
                    hover = wheel_hover(layout, static_cast<float>(mx), static_cast<float>(my));
                std::lock_guard lock(g_wheel_mutex);
                g_wheel_pick = hover >= 0 ? layout.picks[static_cast<std::size_t>(hover)] : WheelPick{};
                g_wheel_layout = std::move(layout);
            }

            if (m.lit.size() != static_cast<std::size_t>(n))
                m.lit.assign(static_cast<std::size_t>(n), 0.f);
            const float follow = 1.f - std::exp(-dt * 22.f);
            for (int i = 0; i < n; ++i)
            {
                float& lit = m.lit[static_cast<std::size_t>(i)];
                lit += ((i == hover ? 1.f : 0.f) - lit) * follow;
            }
            if (hover >= 0)
            {
                const int mx = g_cursor_x.load(), my = g_cursor_y.load();
                const float target = std::atan2(static_cast<float>(my) - c.y, static_cast<float>(mx) - c.x);
                if (!m.pointer_set)
                    m.pointer = target;
                float delta = std::remainder(target - m.pointer, 2.f * k_pi); // the short way round
                m.pointer += delta * (1.f - std::exp(-dt * 30.f));
                m.pointer_set = true;
            }

            // the segments
            disc(p, c, r_out + display.y * 0.004f, faded(IM_COL32(0, 0, 0, 90), alpha));
            const float gap = n > 1 ? 0.012f : 0.f;
            for (int i = 0; i < n; ++i)
            {
                const WheelEntry& w = entries[static_cast<std::size_t>(i)];
                const float lit = m.lit[static_cast<std::size_t>(i)];
                const float span = spans[static_cast<std::size_t>(i)];
                const float a0 = -k_pi * 0.5f + starts[static_cast<std::size_t>(i)] + gap, a1 = a0 + span - 2.f * gap;
                const float mid = (a0 + a1) * 0.5f;
                // the lit segment slides out from the middle and reaches further
                const ImVec2 sc(c.x + std::cos(mid) * 14.f * unit * lit, c.y + std::sin(mid) * 14.f * unit * lit);
                const float r1 = r_out + 10.f * unit * lit;
                ring_segment(p, sc, r_in, r1, a0, a1, faded(mixed(IM_COL32(0, 0, 0, 150), IM_COL32(60, 92, 26, 200), lit), alpha));
                ring_segment(p, sc, r1 - 4.f * unit, r1, a0, a1, faded(green, alpha * lit)); // its outer edge
                if (w.active)
                    ring_segment(p, sc, r_in, r_in + 4.f * unit, a0, a1, faded(green, alpha * 0.8f)); // in the hands
                // the icon (or a melee weapon's kind) in the middle of the segment, a little larger when lit
                const float rm = (r_in + r1) * 0.5f;
                const ImVec2 at(sc.x + std::cos(mid) * rm, sc.y + std::sin(mid) * rm);
                const float chord = std::min((r1 - r_in) * 0.8f, 2.f * rm * std::sin(std::min(span, k_pi) * 0.5f) * 0.8f);
                const float zoom = 1.f + 0.15f * lit;
                const float half_w = chord * 0.5f * zoom, half_h = std::min(chord, (r1 - r_in) * 0.5f) * 0.45f * zoom;
                const ImU32 ink = faded(w.active ? green : mixed(IM_COL32(215, 215, 215, 255), IM_COL32_WHITE, lit), alpha);
                if (!w.ammo.empty())
                {
                    // the rounds: a box in their colour (what a dropped pile of them is tinted), with a dark edge
                    const float bw = std::min(half_w, half_h * 1.6f), bh = std::min(half_h, bw * 0.7f), e = 2.f * unit + 1.f;
                    const ImVec2 edge[4] = {ImVec2(at.x - bw - e, at.y - bh - e), ImVec2(at.x + bw + e, at.y - bh - e),
                                            ImVec2(at.x + bw + e, at.y + bh + e), ImVec2(at.x - bw - e, at.y + bh + e)};
                    const ImVec2 box[4] = {ImVec2(at.x - bw, at.y - bh), ImVec2(at.x + bw, at.y - bh), ImVec2(at.x + bw, at.y + bh),
                                           ImVec2(at.x - bw, at.y + bh)};
                    p.polygon(edge, 4, faded(IM_COL32(0, 0, 0, 200), alpha));
                    p.polygon(box, 4, faded(IM_COL32((w.color >> 16) & 255u, (w.color >> 8) & 255u, w.color & 255u, 230), alpha));
                }
                else if (!p.icon(w.icon, at.x - half_w, at.y - half_h, at.x + half_w, at.y + half_h, ink))
                {
                    const float tw = p.width(HudFont::Text, w.label);
                    p.text(HudFont::Text, at.x - tw * 0.5f, at.y - p.line(HudFont::Text) * 0.5f, ink, w.label);
                }
                if (!w.detail.empty())
                {
                    const float tw = p.width(HudFont::Ammo, w.detail);
                    p.text(HudFont::Ammo, at.x - tw * 0.5f, at.y + half_h + 2.f * unit,
                           faded(mixed(IM_COL32(200, 200, 200, 220), IM_COL32(255, 255, 255, 255), lit), alpha), w.detail);
                }
            }

            // the weight: a ring outside, what is carried (green .. red), grey for what is left
            const float max_weight = session.inventory.limits().max_weight;
            const float weight = session.remote_weight >= 0.f ? session.remote_weight : session.inventory.weight();
            if (max_weight > 0.f)
            {
                const float w0 = r_out + display.y * 0.006f, w1 = w0 + display.y * 0.005f;
                const float fraction = std::clamp(weight / max_weight, 0.f, 1.f);
                ring_segment(p, c, w0, w1, -k_pi * 0.5f, -k_pi * 0.5f + 2.f * k_pi, faded(IM_COL32(110, 110, 110, 170), alpha));
                // what is carried: green when light, yellow half way, red at the limit
                const float t = fraction;
                const float r = t < 0.5f ? 70.f + (230.f - 70.f) * t / 0.5f : 230.f - 30.f * (t - 0.5f) / 0.5f;
                const float gch = t < 0.5f ? 200.f + 10.f * t / 0.5f : 210.f - 170.f * (t - 0.5f) / 0.5f;
                const float b = t < 0.5f ? 60.f - 20.f * t / 0.5f : 40.f - 5.f * (t - 0.5f) / 0.5f;
                if (fraction > 0.f)
                    ring_segment(p, c, w0, w1, -k_pi * 0.5f, -k_pi * 0.5f + 2.f * k_pi * fraction,
                                 faded(IM_COL32(static_cast<int>(r), static_cast<int>(gch), static_cast<int>(b), 230), alpha));
            }

            // the middle: what is pointed at, and the weight; a notch on its edge points where the cursor does
            disc(p, c, r_in * 0.95f, faded(IM_COL32(0, 0, 0, 170), alpha));
            if (hover >= 0 && m.pointer_set)
            {
                const float a = m.pointer, spread = 0.09f;
                const ImVec2 tip(c.x + std::cos(a) * (r_in + 1.f * unit), c.y + std::sin(a) * (r_in + 1.f * unit));
                const float rb = r_in * 0.95f - 9.f * unit;
                const ImVec2 notch[3] = {ImVec2(c.x + std::cos(a - spread) * rb, c.y + std::sin(a - spread) * rb), tip,
                                         ImVec2(c.x + std::cos(a + spread) * rb, c.y + std::sin(a + spread) * rb)};
                p.polygon(notch, 3, faded(green, alpha));
            }
            // a right button held on rounds: a ring fills, then all of them drop
            std::string drop_all;
            float held = 0.f;
            {
                std::lock_guard lock(g_wheel_mutex);
                if (!g_rounds_hold.kind.empty())
                {
                    held = static_cast<float>(now - g_rounds_hold.since) / static_cast<float>(k_rounds_hold_ms);
                    if (held >= 1.f)
                    {
                        drop_all = g_rounds_hold.kind;
                        g_rounds_hold = RoundsHold{};
                    }
                }
            }
            if (!drop_all.empty())
            {
                INV_LOG("hud: wheel right button held: drops all %s rounds", drop_all.c_str());
                game::backend().drop_ammo(drop_all, 0); // (only posts a request: fine with the session locked)
            }
            else if (held > 0.f)
                ring_segment(p, c, r_in * 0.95f - 5.f * unit, r_in * 0.95f, -k_pi * 0.5f,
                             -k_pi * 0.5f + 2.f * k_pi * std::min(held, 1.f), IM_COL32(240, 90, 75, 230));

            // the lines in the middle, centred as a block; a name too wide for the middle is set smaller
            struct Line
            {
                HudFont font;
                std::string text;
                ImU32 colour;
            };
            std::vector<Line> lines;
            if (hover >= 0)
            {
                const WheelEntry& w = entries[static_cast<std::size_t>(hover)];
                lines.push_back({p.width(HudFont::Title, w.label) > r_in * 1.8f ? HudFont::Text : HudFont::Title, w.label,
                                 IM_COL32_WHITE});
                if (!w.detail.empty())
                    lines.push_back({HudFont::Ammo, w.detail, IM_COL32(230, 230, 230, 255)});
            }
            else
                lines.push_back({HudFont::Title, n == 0 ? std::string("Nothing carried") : std::string("Inventory"), IM_COL32_WHITE});
            if (max_weight > 0.f)
            {
                char text[32];
                std::snprintf(text, sizeof(text), "%.1f / %.1f", weight, max_weight);
                lines.push_back({HudFont::Text, text,
                                 weight >= max_weight - 1e-3f ? IM_COL32(240, 90, 75, 255) : IM_COL32(190, 190, 190, 255)});
            }
            if (hover >= 0 && !entries[static_cast<std::size_t>(hover)].ammo.empty())
            {
                const std::string& kind = entries[static_cast<std::size_t>(hover)].ammo;
                const int magazine = std::min(inv::ammo_magazine(kind), session.ammo_pool[inv::ammo_index(kind)]);
                lines.push_back({HudFont::Hint, "right click: drop " + std::to_string(magazine) + "   hold: all", IM_COL32(150, 150, 150, 255)});
            }
            else if (hover >= 0)
                lines.push_back({HudFont::Hint, "release: take   right click: drop", IM_COL32(150, 150, 150, 255)});
            else if (n > 0)
                lines.push_back({HudFont::Hint, "point at an item", IM_COL32(150, 150, 150, 255)});
            const float spacing = 3.f * unit;
            float total = 0.f;
            for (const Line& l : lines)
                total += p.line(l.font) + spacing;
            float y = c.y - (total - spacing) * 0.5f;
            for (const Line& l : lines)
            {
                p.text(l.font, c.x - p.width(l.font, l.text) * 0.5f, y, faded(l.colour, alpha), l.text);
                y += p.line(l.font) + spacing;
            }
        }

        // The wheel's pointer where the window procedure last saw the cursor, when VGUI draws the wheel (with ImGui
        // drawing it, ImGui's own cursor is on): an arrow like the system's, white with a dark edge, in convex pieces.
        void draw_pointer(Painter& p, ImVec2 display)
        {
            const int cx = g_cursor_x.load(), cy = g_cursor_y.load();
            if (cx < 0)
                return;
            const float s = std::max(1.f, display.y / 900.f), x = static_cast<float>(cx), y = static_cast<float>(cy);
            const auto at = [&](float ax, float ay, float dx, float dy) { return ImVec2(x + ax * s + dx, y + ay * s + dy); };
            const auto arrow = [&](float dx, float dy, ImU32 colour) {
                const ImVec2 left[3] = {at(0.f, 0.f, dx, dy), at(0.f, 16.f, dx, dy), at(4.f, 12.f, dx, dy)};
                const ImVec2 right[3] = {at(0.f, 0.f, dx, dy), at(4.f, 12.f, dx, dy), at(11.f, 11.f, dx, dy)};
                const ImVec2 tail[4] = {at(4.f, 11.f, dx, dy), at(6.5f, 11.f, dx, dy), at(9.5f, 17.f, dx, dy), at(7.f, 18.f, dx, dy)};
                p.polygon(left, 3, colour);
                p.polygon(right, 3, colour);
                p.polygon(tail, 4, colour);
            };
            for (const ImVec2 d : {ImVec2(-1.f, 0.f), ImVec2(1.f, 0.f), ImVec2(0.f, -1.f), ImVec2(0.f, 1.f)})
                arrow(d.x, d.y, IM_COL32(0, 0, 0, 230));
            arrow(0.f, 0.f, IM_COL32_WHITE);
        }

        // ---- pings --------------------------------------------------------------------------------------------------
        //
        // The server told this client where its team pinged (game/ping.cpp); each ping is a diamond at that point of the
        // world, with its name and the distance in metres, through walls.  Off the screen (or behind the camera) it
        // sticks to the edge nearest the way it lies, with an arrow.  It pops in, and fades out over its last moments.
        void draw_pings(Painter& p, ImVec2 display)
        {
            static unsigned seen = 0;
            const unsigned serial = game::ping_serial();
            if (serial != seen)
            {
                if (serial > seen)
                    ui::vgui::play_sound("ui/menu_focus.wav");
                seen = serial;
            }
            const std::vector<game::PingMark> marks = game::ping_marks();
            if (marks.empty())
                return;
            float camera[3] = {};
            const bool have_camera = game::ping_camera(camera[0], camera[1], camera[2]);
            const float scale = core::config().hud_scale * std::max(1.f, display.y / 900.f);
            const float margin = 40.f * scale;
            const ULONGLONG now = GetTickCount64();
            const ImVec2 middle(display.x * 0.5f, display.y * 0.5f);
            for (const game::PingMark& mark : marks)
            {
                float ndc_x = 0.f, ndc_y = 0.f;
                bool in_front = false;
                if (!game::ping_project(mark.x, mark.y, mark.z, ndc_x, ndc_y, in_front))
                    return;
                const float age = static_cast<float>(now - mark.born) / 1000.f;
                const float left = static_cast<float>(mark.life_ms) / 1000.f - age;
                const float alpha = std::clamp(left / 0.8f, 0.f, 1.f) * std::clamp(age / 0.06f, 0.2f, 1.f);
                if (alpha <= 0.f)
                    continue;
                const float pop = 1.f + 0.7f * std::max(0.f, 1.f - age / 0.25f);
                float x = (1.f + ndc_x) * 0.5f * display.x, y = (1.f - ndc_y) * 0.5f * display.y;
                bool on_edge = !in_front || x < margin || x > display.x - margin || y < margin || y > display.y - margin;
                ImVec2 direction(0.f, 0.f);
                if (on_edge)
                {
                    float dx = x - middle.x, dy = y - middle.y;
                    if (std::fabs(dx) < 1.f && std::fabs(dy) < 1.f)
                        dy = 1.f; // straight behind: the bottom
                    const float length = std::sqrt(dx * dx + dy * dy);
                    direction = ImVec2(dx / length, dy / length);
                    const float reach_x = std::fabs(dx) > 0.001f ? (middle.x - margin) / std::fabs(dx) : 1e9f;
                    const float reach_y = std::fabs(dy) > 0.001f ? (middle.y - margin) / std::fabs(dy) : 1e9f;
                    const float t = std::min(reach_x, reach_y);
                    x = middle.x + dx * t;
                    y = middle.y + dy * t;
                }
                ImU32 colour = IM_COL32(85, 205, 240, 255);
                if (mark.kind == game::PingKind::Enemy)
                    colour = IM_COL32(235, 70, 60, 255);
                else if (mark.kind == game::PingKind::Item)
                    colour = IM_COL32(250, 200, 70, 255);
                const float r = (on_edge ? 9.f : 11.f) * scale * pop;
                const auto diamond = [&](float radius, ImU32 c) {
                    const ImVec2 points[4] = {ImVec2(x, y - radius), ImVec2(x + radius, y), ImVec2(x, y + radius), ImVec2(x - radius, y)};
                    p.polygon(points, 4, c);
                };
                diamond(r + 2.f * scale, faded(IM_COL32(0, 0, 0, 200), alpha));
                diamond(r, faded(colour, alpha));
                diamond(r * 0.42f, faded(IM_COL32(0, 0, 0, 220), alpha));
                if (on_edge)
                {
                    const ImVec2 tip(x + direction.x * (r + 11.f * scale), y + direction.y * (r + 11.f * scale));
                    const ImVec2 base(x + direction.x * (r + 3.f * scale), y + direction.y * (r + 3.f * scale));
                    const ImVec2 side(-direction.y * 5.f * scale, direction.x * 5.f * scale);
                    const ImVec2 arrow[3] = {tip, ImVec2(base.x + side.x, base.y + side.y), ImVec2(base.x - side.x, base.y - side.y)};
                    p.polygon(arrow, 3, faded(colour, alpha));
                }
                std::string distance;
                if (have_camera)
                {
                    const float dx = mark.x - camera[0], dy = mark.y - camera[1], dz = mark.z - camera[2];
                    char text[24];
                    std::snprintf(text, sizeof(text), "%d m", static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz) * 0.01905f + 0.5f));
                    distance = text;
                }
                float line_y = y + r + 4.f * scale;
                if (!distance.empty())
                {
                    p.text(HudFont::Hint, x - p.width(HudFont::Hint, distance) * 0.5f, line_y, faded(IM_COL32_WHITE, alpha), distance);
                    line_y += p.line(HudFont::Hint);
                }
                if (!mark.label.empty())
                    p.text(HudFont::Text, x - p.width(HudFont::Text, mark.label) * 0.5f, line_y, faded(colour, alpha), mark.label);
            }
        }

        // Everything but the inventory window, under the session lock: through VGUI from paint_vgui, or with ImGui from
        // draw() when the game's surface is not painting.
        void paint_hud(const game::Session& session, Painter& p, ImVec2 display, bool pointer)
        {
            paint_tint(p, display, g_infection_tint);
            paint_tint(p, display, g_stamina_tint);
            if (core::config().show_hotbar && session.playing && core::shows_slots())
            {
                if (core::config().replace_native)
                    draw_hotbar(session, p, display);
                else
                    draw_column(session, p, display);
            }
            draw_stamina_bar(p, display);
            if (session.playing)
            {
                draw_melee(session, p, display);
                draw_pings(p, display);
            }
            if (g.wheel && session.playing)
            {
                draw_wheel(session, p, display);
                if (pointer)
                    draw_pointer(p, display);
            }
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

                ImGuiPainter painter(ImGui::GetWindowDrawList(), 1.f); // (the window's own scale: the toolkit's)
                draw_slot(painter, pos.x, pos.y, static_cast<float>(size), stack, i == inventory.selected(), hovered, 0);
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

        // The reserve rounds (see game/rounds.cpp): each ammo type the player carries, what it weighs, and its drops
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
                // the colour its dropped piles are tinted
                const unsigned color = inv::ammo_color(kind);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float side = ImGui::GetTextLineHeight() - 4.f;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, at.y + 3.f), ImVec2(at.x + side, at.y + 3.f + side),
                                                          IM_COL32((color >> 16) & 255u, (color >> 8) & 255u, color & 255u, 255));
                ImGui::Dummy(ImVec2(side, side));
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
            ImGui::SameLine();
            if (nc::Button("Unload gun", ImVec2(90.f, 0.f)))
                g.intents.push_back({Intent::Unload, 0, 0});
            nc::SetItemTooltip("Empties the gun in your hands (or the one on the ground you look at) into your reserve.");
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
                changed |= nc::Checkbox("Wheel sounds (a tick on each item pointed at)", &cfg.wheel_sounds);
                changed |= nc::Checkbox("Store items on use", &cfg.auto_store);
                changed |= nc::Checkbox("Replace the game's weapon panel (1-0 keys, wheel); off: a grid under it, keys 3-0", &cfg.replace_native);
                changed |= nc::SliderFloat("Scale:", &cfg.hud_scale, 0.5f, 3.f, "%.2f");
            }
            nc::EndGroupBox();

            if (nc::BeginGroupBox("Infection", ImVec2(0, 0)))
            {
                changed |= nc::Checkbox("Screen effect", &cfg.infection_effect);
                if (cfg.infection_effect)
                {
                    changed |= nc::Checkbox("Effect covers the game's HUD", &cfg.infection_cover_game_hud);
                    nc::SetItemTooltip("Off: the effect is on the game's picture only; the game's HUD (health, teammates,\n"
                                       "crosshair) and the mod's boxes and wheel (drawn with it) stay clear over it.");
                    if (cfg.infection_cover_game_hud)
                    {
                        changed |= nc::Checkbox("Effect covers the mod's window", &cfg.infection_cover_mod_hud);
                        nc::SetItemTooltip("This window. Off: it stays clear over the effect.");
                    }
                }
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
                if (cfg.stamina_vignette)
                {
                    changed |= nc::Checkbox("Vignette covers the game's HUD", &cfg.stamina_cover_game_hud);
                    nc::SetItemTooltip("Off: the vignette is on the game's picture only; the game's HUD (health, teammates,\n"
                                       "crosshair) and the mod's boxes and wheel (drawn with it) stay clear over it.");
                    if (cfg.stamina_cover_game_hud)
                    {
                        changed |= nc::Checkbox("Vignette covers the mod's window", &cfg.stamina_cover_mod_hud);
                        nc::SetItemTooltip("This window. Off: it stays clear over the vignette.");
                    }
                }
                nc::TextDisabled("Whether there is stamina at all is [stamina] in the host's inventory_mod.toml.");
            }
            nc::EndGroupBox();

            if (nc::BeginGroupBox("Melee", ImVec2(0, 0)))
            {
                changed |= nc::Checkbox("Wind-up ring at the crosshair", &cfg.melee_ring);
                nc::SetItemTooltip("Fills while the attack is held with a melee weapon; full: the swing is a heavy one.");
                nc::TextDisabled("Light and heavy swings are [melee] in the host's inventory_mod.toml.");
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

        std::atomic<bool> g_open_settings{false}; // open_settings was called (another thread)
        std::atomic<bool> g_open_window{false}; // open_window was called (another thread)
        std::atomic<bool> g_settings_page{false}; // the game's menu has the settings (set_settings_page)
        bool g_select_settings = false; // the next frame selects the Settings tab

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
                    const ImGuiTabItemFlags settings_flags = g_select_settings ? ImGuiTabItemFlags_SetSelected : 0;
                    g_select_settings = false;
                    // (the settings are in the game's menu, Options > Inventory Mod, when it has the page)
                    if (!g_settings_page.load() && nc::BeginTabItem("Settings", nullptr, settings_flags))
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

    namespace
    {
        void apply_queued_settings(); // (below: the game's Options > Inventory Mod page)
    } // namespace

    void draw()
    {
        if (g_open_settings.exchange(false))
        {
            g.open = true;
            g_select_settings = true;
        }
        if (g_open_window.exchange(false))
            g.open = true;
        apply_queued_settings();
        // the game paints the HUD itself (paint_vgui), and its pointer on the wheel; else the overlay draws them
        const bool vgui_paints = ui::vgui::painting();
        ImGui::GetIO().MouseDrawCursor = g.open || (g.wheel && !vgui_paints);
        const ImVec2 display = ImGui::GetIO().DisplaySize;

        std::string to_save;
        {
            game::Session& session = game::local_session();
            std::lock_guard lock(session.mutex);
            g_infection_tint = ScreenTint{};
            g_stamina_tint = ScreenTint{};
            g_stamina_live = false;
            if (session.playing || g_stamina_preview)
                update_stamina(session, display);
            if (session.playing || g_infection_preview)
                update_infection(session, display);
            draw_screamer(session);
            if (g.wheel && !session.playing)
                g.wheel = false; // died, a menu opened ...
            if (!vgui_paints)
            {
                // under the inventory window; the text grows with the screen as the game's proportional fonts do
                ImGuiPainter painter(ImGui::GetBackgroundDrawList(), std::max(1.f, display.y / 720.f));
                paint_hud(session, painter, display, false);
            }
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

    void paint_vgui()
    {
        game::Session& session = game::local_session();
        // (try: should the game ever paint from inside something of the mod's that holds the lock, a frame is skipped)
        std::unique_lock lock(session.mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        int w = 0, h = 0;
        ui::vgui::screen_size(w, h);
        if (w <= 0 || h <= 0)
            return;
        VguiPainter painter;
        paint_hud(session, painter, ImVec2(static_cast<float>(w), static_cast<float>(h)), true);
    }

    bool wants_input()
    {
        return g.open || g.wheel;
    }

    bool wants_keyboard()
    {
        return g.open;
    }

    bool window_open()
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

    void open_settings()
    {
        g_open_settings = true;
    }

    namespace
    {
        std::mutex g_settings_mutex;
        std::vector<std::pair<std::string, std::string>> g_settings_queue; // apply_setting, for the next frame

        void apply_queued_settings()
        {
            std::vector<std::pair<std::string, std::string>> queue;
            {
                std::lock_guard lock(g_settings_mutex);
                queue.swap(g_settings_queue);
            }
            if (queue.empty())
                return;
            core::Config& cfg = core::config();
            for (const auto& [key, value] : queue)
            {
                const bool on = value == "1";
                const float number = static_cast<float>(std::atof(value.c_str()));
                if (key == "show_hotbar") cfg.show_hotbar = on;
                else if (key == "inventory_view") cfg.inventory_view = std::clamp(std::atoi(value.c_str()), 0, 2);
                else if (key == "auto_store") cfg.auto_store = on;
                else if (key == "replace_native") cfg.replace_native = on;
                else if (key == "hud_scale") cfg.hud_scale = std::clamp(number, 0.5f, 3.f);
                else if (key == "infection_effect") cfg.infection_effect = on;
                else if (key == "infection_cover_game_hud") cfg.infection_cover_game_hud = on;
                else if (key == "infection_cover_mod_hud") cfg.infection_cover_mod_hud = on;
                else if (key == "infection_sound") cfg.infection_sound = on;
                else if (key == "infection_screamer") cfg.infection_screamer = on;
                else if (key == "screamer_chance") cfg.screamer_chance = std::clamp(number, 0.f, 1.f);
                else if (key == "stamina_bar") cfg.stamina_bar = on;
                else if (key == "stamina_vignette") cfg.stamina_vignette = on;
                else if (key == "stamina_cover_game_hud") cfg.stamina_cover_game_hud = on;
                else if (key == "stamina_cover_mod_hud") cfg.stamina_cover_mod_hud = on;
                else if (key == "melee_ring") cfg.melee_ring = on;
                else if (key == "key_toggle") cfg.key_toggle = std::atoi(value.c_str());
                else if (key == "key_prev") cfg.key_prev = std::atoi(value.c_str());
                else if (key == "key_next") cfg.key_next = std::atoi(value.c_str());
                else if (key == "key_use") cfg.key_use = std::atoi(value.c_str());
                else if (key == "key_drop") cfg.key_drop = std::atoi(value.c_str());
                else if (key == "key_store") cfg.key_store = std::atoi(value.c_str());
                else if (key == "key_pickup") cfg.key_pickup = std::atoi(value.c_str());
                else if (key == "key_unload") cfg.key_unload = std::atoi(value.c_str());
                else if (key == "slots") cfg.slots = std::clamp(std::atoi(value.c_str()), 1, 24);
                else if (key == "weapon_slots") cfg.weapon_slots = std::clamp(std::atoi(value.c_str()), 0, 8);
                else if (key == "max_weight") cfg.max_weight = std::max(0.f, number);
                else
                {
                    INV_LOG("settings page: unknown setting %s", key.c_str());
                    continue;
                }
                INV_LOG("settings page: %s = %s", key.c_str(), value.c_str());
            }
            core::config_save();
        }
    } // namespace

    void open_window()
    {
        g_open_window = true;
    }

    void set_settings_page(bool available)
    {
        g_settings_page = available;
    }

    void apply_setting(const char* key, const char* value)
    {
        std::lock_guard lock(g_settings_mutex);
        g_settings_queue.emplace_back(key, value);
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
                g_wheel_layout = WheelLayout{}; // (the next drawing lays it out again)
            }
            g_wheel_ticked = -1;
            if (HWND window = GetActiveWindow())
            {
                RECT rc{};
                if (GetClientRect(window, &rc))
                {
                    POINT centre{(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
                    g_cursor_x = centre.x;
                    g_cursor_y = centre.y;
                    ClientToScreen(window, &centre);
                    SetCursorPos(centre.x, centre.y);
                }
            }
            g_wheel_opened = GetTickCount64();
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
            {
                std::lock_guard lock(g_wheel_mutex);
                g_rounds_hold = RoundsHold{};
                g_wheel_layout.valid = false;
            }
            const WheelPick pick = take_wheel_pick();
            if (!take || !pick.valid || !pick.ammo.empty()) // (rounds are not taken in hand)
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
            if (!pick.ammo.empty())
                return; // (rounds go through the hold: see on_wnd_message)
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
        if (msg == WM_MOUSEMOVE)
        {
            const int x = static_cast<int>(static_cast<short>(LOWORD(lparam))), y = static_cast<int>(static_cast<short>(HIWORD(lparam)));
            g_cursor_x = x;
            g_cursor_y = y;
            if (g.wheel)
                wheel_mouse_moved(x, y); // (and on to ImGui: the cursor is drawn there)
        }
        if (g.wheel)
        {
            if (msg == WM_MBUTTONUP || msg == WM_LBUTTONDOWN)
            {
                // what is under the cursor now, even when no drawing has caught up with it yet
                wheel_mouse_moved(static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam)));
                close_wheel(true);
                return true;
            }
            if (msg == WM_RBUTTONDOWN)
            {
                // rounds: a magazine's worth when the button is let go, all of them when it is held (draw_wheel)
                {
                    std::lock_guard lock(g_wheel_mutex);
                    if (g_wheel_pick.valid && !g_wheel_pick.ammo.empty())
                    {
                        g_rounds_hold = RoundsHold{g_wheel_pick.ammo, GetTickCount64()};
                        return true;
                    }
                }
                drop_wheel_pick();
                return true;
            }
            if (msg == WM_RBUTTONUP)
            {
                std::string kind;
                {
                    std::lock_guard lock(g_wheel_mutex);
                    kind = g_rounds_hold.kind; // (empty when the hold already dropped all)
                    g_rounds_hold = RoundsHold{};
                }
                if (!kind.empty())
                {
                    INV_LOG("hud: wheel right click drops %s rounds", kind.c_str());
                    game::backend().drop_ammo(kind, inv::ammo_magazine(kind));
                }
                return true;
            }
            if ((msg == WM_KEYDOWN && wparam == VK_ESCAPE) || msg == WM_KILLFOCUS)
            {
                close_wheel(false);
                return msg != WM_KILLFOCUS;
            }
            if (msg == WM_MOUSEWHEEL || msg == WM_LBUTTONUP || msg == WM_MBUTTONDOWN)
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
        if (vk == cfg.key_unload)
        {
            run({Intent::Unload, 0, 0});
            return true;
        }
        if (vk == cfg.key_ping)
        {
            run({Intent::Ping, 0, 0});
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
