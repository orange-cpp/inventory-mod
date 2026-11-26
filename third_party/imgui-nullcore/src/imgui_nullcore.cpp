// The nullcore look for Dear ImGui (see imgui_nullcore.hpp).  Every widget paints what its toolkit counterpart paints
// (libs/nclib/source/ui/widgets/*.cpp), with the same primitives (fill_rect / outline_rect / line are whole-pixel
// rectangles, as the toolkit's ShapeTessellator makes them), the same fonts (gdi_fonts.cpp) and the same icons.

#include "gdi_fonts.hpp"
#include "internal.hpp"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <imgui_internal.h>
#include <imgui_nullcore/imgui_nullcore.hpp>
#include <memory>
#include <string>
#include <vector>

namespace imgui_nullcore
{

    namespace detail
    {

        // nullcore's factory preset (source/app/theme.cpp, slots 0 .. 13), r | g << 8 | b << 16 | a << 24.
        constexpr std::array<std::uint32_t, Palette::SlotCount> k_factory = {
                0xFF423323u, // BG
                0xFF4C3B29u, // BG Alt
                0xFF775836u, // BG Focused
                0xFF96960Fu, // Border
                0xFF8EB13Bu, // Border Alt
                0xFF8FE138u, // Border Focused
                0xFF0F43C6u, // Border Selected
                0xFFE0D2C3u, // Text
                0xFFB4E1FFu, // Text Alt
                0xFFFFFFFFu, // Text Focused
                0xFF007FFFu, // Text Selected
                0xFF0000BAu, // Exit Focused
                0xFF0000FFu, // Exit Pressed
                0xFF7F7F7Fu, // Inactive
        };

        static_assert(sizeof(Palette) == sizeof(ImVec4) * Palette::SlotCount, "Palette is the slots in order");

        // ---- the state of the current context ---------------------------------------------------------------------

        // the parts of one context, freed by its Shutdown hook (the hook's UserData)
        struct Parts
        {
            PartSlot slots[PART_COUNT];
        };
        constexpr ImGuiID k_hook_owner = 0x6E636F72; // "ncor"

        static void free_parts(ImGuiContext*, ImGuiContextHook* hook)
        {
            auto* parts = static_cast<Parts*>(hook->UserData);
            hook->UserData = nullptr;
            for (int i = PART_COUNT - 1; i >= 0; --i)
                if (parts->slots[i].data)
                    parts->slots[i].destroy(parts->slots[i].data);
            IM_DELETE(parts);
        }

        // every Begin ended, every Push popped: a frame that leaves one open is a user error (recoverable, like
        // ImGui's own: the stack is dropped so the next frame starts clean)
        static void end_frame_parts(ImGuiContext*, ImGuiContextHook* hook)
        {
            auto* parts = static_cast<Parts*>(hook->UserData);
            for (PartSlot& slot : parts->slots)
                if (slot.data && slot.end_frame)
                    slot.end_frame(slot.data);
        }

        constexpr float k_unfocused_alpha = 127.f / 255.f; // the toolkit's 0.5 multiplier, truncated


        // the icons: white, alpha only (a transparent pixel around each in the texture)
        constexpr std::uint8_t k_icon_alpha[static_cast<int>(Icon::Count)][16 * 16] = {
#include "icons.inc"
        };

        struct WindowState
        {
            bool dimmed = false;
            bool scrollbar = false;
        };

        struct GroupState
        {
            ImGuiWindow* window = nullptr;
            std::string title;
            ImVec2 pos; // the cursor at begin: the box's top-left (+ the nesting shift)
            int box_x = 0, box_y = 0, w = 0, fixed_h = 0;
            ImVec2 content;
            ImVec2 backup_indent, backup_curr_line_size, backup_cursor_max;
            float backup_curr_line_base = 0.f;
            ImVec2 backup_prev_line_pos, backup_prev_line_size;
            float backup_prev_line_base = 0.f;
            bool backup_same_line = false;
            float backup_work_max_x = 0.f, backup_content_max_x = 0.f;
            float backup_work_max_y = 0.f, backup_content_max_y = 0.f;
            ImDrawListSplitter splitter;
        };

        // The last nullcore widget and whether it is hovered (anywhere: a number box's spinner too), for
        // SetItemTooltip(); the label column of a labelled control counts as the control.
        struct LastItem
        {
            ImGuiID id = 0;
            bool hovered = false;
        };

        // SetNextItemSwatch's, for the next labelled widget
        struct NextSwatch
        {
            ImU32* col = nullptr;
            const char* title = nullptr;
            bool alpha = true;
        };

        // the current context's state of the widgets here (PART_CORE)
        struct CoreState
        {
            Palette palette = Palette::Factory();
            Fonts fonts;
            Options options;
            ImTextureData* icons = nullptr;
            ImVector<float> label_widths;
            ImVector<WindowState> windows;
            std::vector<std::unique_ptr<GroupState>> groups;
            LastItem last_item;
            NextSwatch next_swatch;
            ImGuiID label_owner = 0;
            bool label_hovered = false;
            float next_height = 0.f; // SetNextItemHeight's
            Cursor cursor = Cursor::Arrow;
            int cursor_frame = -1;

            CoreState() = default;
            CoreState(const CoreState&) = delete;
            CoreState& operator=(const CoreState&) = delete;
            ~CoreState()
            {
                free_texture(icons);
            }
            void end_frame()
            {
                IM_ASSERT_USER_ERROR(windows.empty(), "imgui_nullcore::Begin() without End()");
                IM_ASSERT_USER_ERROR(groups.empty(), "BeginGroupBox() without EndGroupBox()");
                IM_ASSERT_USER_ERROR(label_widths.empty(), "PushLabelWidth() without PopLabelWidth()");
                windows.clear();
                for (auto gs = groups.rbegin(); gs != groups.rend(); ++gs) // (the innermost's split first)
                    if ((*gs)->splitter._Count > 1)
                        (*gs)->splitter.Merge((*gs)->window->DrawList);
                groups.clear();
                label_widths.clear();
            }
        };
        static CoreState& core()
        {
            return context_state<CoreState>(PART_CORE);
        }

        PartSlot& context_part(int part)
        {
            ImGuiContext* ctx = ImGui::GetCurrentContext();
            IM_ASSERT(ctx && "imgui_nullcore needs a current ImGui context (ImGui::CreateContext first)");
            for (ImGuiContextHook& hook : ctx->Hooks)
                if (hook.Owner == k_hook_owner && hook.Type == ImGuiContextHookType_Shutdown && hook.UserData)
                    return static_cast<Parts*>(hook.UserData)->slots[part];
            ImGuiContextHook hook;
            hook.Type = ImGuiContextHookType_Shutdown;
            hook.Owner = k_hook_owner;
            hook.Callback = free_parts;
            auto* parts = IM_NEW(Parts)();
            hook.UserData = parts;
            ImGui::AddContextHook(ctx, &hook);
            hook.Type = ImGuiContextHookType_EndFramePre;
            hook.Callback = end_frame_parts;
            ImGui::AddContextHook(ctx, &hook);
            return parts->slots[part];
        }

        void free_texture(ImTextureData*& tex)
        {
            if (!tex)
                return;
            if (ImGuiContext* ctx = ImGui::GetCurrentContext(); ctx && ctx->UserTextures.contains(tex))
                ImGui::UnregisterUserTexture(tex);
            IM_DELETE(tex);
            tex = nullptr;
        }

        void finish_item(ImGuiID id, const ImRect& bb, bool hovered)
        {
            ImGuiContext& g = *GImGui;
            ImGui::SetLastItemData(id, g.LastItemData.ItemFlags, g.LastItemData.StatusFlags, bb);
            core().last_item = {id, hovered};
        }

        const ImVec4& tc(int slot)
        {
            return (&core().palette.Bg)[std::clamp(slot, 0, Palette::SlotCount - 1)];
        }

        // with style.Alpha, rounded the way the toolkit truncates its multiplied alpha (see k_unfocused_alpha)
        ImU32 u32(const ImVec4& c)
        {
            return ImGui::GetColorU32(c);
        }

        ImFont* normal_font()
        {
            return ImGui::GetFont(); // the current font: LoadFonts makes the regular Verdana the default
        }
        ImFont* header_font()
        {
            return core().fonts.Header ? core().fonts.Header : normal_font();
        }
        float font_size()
        {
            return ImGui::GetFontSize();
        }

        // SetNextItemHeight's, else the toolkit's 16 px row
        static int row_height()
        {
            const float h = core().next_height;
            core().next_height = 0.f;
            return h > 0.f ? static_cast<int>(h) : k_row;
        }
        int text_h()
        {
            return static_cast<int>(font_size());
        }

        // (h - text_h) * 0.5 + y, truncated (widgets_input.cpp's center_y)
        int center_y(int h, int th, int y)
        {
            return static_cast<int>(static_cast<float>(h - th) * 0.5f + static_cast<float>(y));
        }

        float label_width()
        {
            return core().label_widths.empty() ? 112.f : core().label_widths.back();
        }

        bool has_label(const char* label)
        {
            return label && ImGui::FindRenderedTextEnd(label) != label;
        }

        // The number of characters (UTF-8 sequences) of `text` whose advances fit in max_w (an overflowing one is not
        // counted); returns the end of them.
        const char* fit_end(ImFont* font, const char* text, const char* text_end, int max_w)
        {
            if (!text_end)
                text_end = text + std::strlen(text);
            ImFontBaked* baked = font->GetFontBaked(font_size());
            int acc = 0;
            const char* p = text;
            while (p < text_end)
            {
                unsigned int c = 0;
                const int n = ImTextCharFromUtf8(&c, p, text_end);
                acc += static_cast<int>(baked->GetCharAdvance(static_cast<ImWchar>(c)));
                if (acc > max_w)
                    break;
                p += n > 0 ? n : 1;
            }
            return p;
        }

        // A width as ImGui reads one: > 0 as is, 0 the item width (SetNextItemWidth / PushItemWidth, -FLT_MIN in our
        // windows: the rest of the row), < 0 that much less than the rest of the row.
        float resolve_width(float w)
        {
            if (w > 0.f)
                return std::floor(w);
            if (w == 0.f)
                return std::max(std::floor(ImGui::CalcItemWidth()), 1.f);
            return std::max(std::floor(ImGui::GetContentRegionAvail().x + w), 1.f);
        }

        // Reserves a w x h item at the cursor; false when it is clipped.  A SetNextItemHeight the widget did not
        // take (row_height) is spent here: it is the next item's, not the one after's.
        bool add_item(ImGuiID id, float w, float h, ImRect& bb)
        {
            core().next_height = 0.f;
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            const ImVec2 pos = window->DC.CursorPos;
            bb = ImRect(pos, ImVec2(pos.x + w, pos.y + h));
            ImGui::ItemSize(bb, ImGui::GetStyle().FramePadding.y);
            return ImGui::ItemAdd(bb, id);
        }

        // The label column in front of a labelled widget (a visible label: right-aligned, label_width() wide, 2 px
        // before the widget); returns the widget's width, the item width from where the widget starts.
        float label_column(const char* label)
        {
            ImGuiContext& g = *GImGui;
            const bool has_next_w = (g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth) != 0;
            const float next_w = g.NextItemData.Width;
            core().label_owner = 0;
            const NextSwatch swatch = core().next_swatch;
            core().next_swatch = {};
            if (has_label(label))
            {
                g.NextItemData.HasFlags &= ~ImGuiNextItemDataFlags_HasWidth; // (the widget's, not the label's)
                const float next_height = core().next_height; // (SetNextItemHeight's too)
                core().next_height = 0.f;
                ImGui::PushID(label);
                // with a swatch the label gives it its last 18 px (decl's place_label)
                Label(label, 1.f, label_width() - (swatch.col ? 18.f : 0.f));
                const bool label_hovered = core().last_item.hovered;
                if (swatch.col)
                {
                    ImGui::SameLine(0.f, static_cast<float>(k_gap));
                    if (ColorButton("##swatch", ImGui::ColorConvertU32ToFloat4(*swatch.col)))
                        OpenColorPicker(swatch.title, swatch.col, IM_COL32_WHITE, swatch.alpha, 1);
                }
                core().last_item.hovered = label_hovered;
                ImGui::PopID();
                core().label_owner = ImGui::GetCurrentWindow()->GetID(label);
                core().label_hovered = core().last_item.hovered;
                ImGui::SameLine(0.f, static_cast<float>(k_gap));
                if (has_next_w)
                    ImGui::SetNextItemWidth(next_w);
                core().next_height = next_height;
            }
            return resolve_width(0.f);
        }

        bool is_focused(const ImGuiWindow* window)
        {
            if (!window)
                return false;
            for (const ImGuiWindow* w = GImGui->NavWindow; w; w = w->ParentWindow)
            {
                if (is_menu_window(w)) // (an open context menu is the active window: none of the others is)
                    return false;
                if (w->RootWindow == window->RootWindow)
                    return true;
            }
            return false;
        }

        double now()
        {
            return ImGui::GetTime();
        }

        // ---- icons --------------------------------------------------------------------------------------------------

        ImTextureData* icon_texture()
        {
            ImTextureData*& g_icons = core().icons;
            if (g_icons)
                return g_icons;
            const int count = static_cast<int>(Icon::Count);
            g_icons = IM_NEW(ImTextureData)();
            g_icons->Create(ImTextureFormat_RGBA32, count * k_icon_cell, k_icon_cell);
            std::memset(g_icons->Pixels, 0, static_cast<std::size_t>(g_icons->GetSizeInBytes()));
            for (int i = 0; i < count; ++i)
                for (int y = 0; y < 16; ++y)
                    for (int x = 0; x < 16; ++x)
                    {
                        auto* p = static_cast<unsigned char*>(g_icons->GetPixelsAt(i * k_icon_cell + 1 + x, 1 + y));
                        p[0] = p[1] = p[2] = 255;
                        p[3] = k_icon_alpha[i][y * 16 + x];
                    }
            g_icons->UseColors = false;
            ImGui::RegisterUserTexture(g_icons);
            return g_icons;
        }

        // ---- the toolkit's widgets' parts ---------------------------------------------------------------------------

        // idle / hovered / pressed (TC_X, TC_X_ALT, TC_X_FOCUSED)
        int state_slot(int base, bool hovered, bool held)
        {
            return base + (hovered ? (held ? 2 : 1) : 0);
        }

        // UIScrollBar: the outline, the arrow boxes at both ends, the thumb with three grip lines.  `pos` and `size`
        // are fractions (the scroll position, the visible part).  True when `pos` changed.
        bool scrollbar(ImGuiID id, int x, int y, int w, int h, float* pos, float size)
        {
            ImGuiContext& g = *GImGui;
            ImGuiWindow* window = g.CurrentWindow;
            ImDrawList* dl = window->DrawList;
            const ImRect bb(ImVec2(static_cast<float>(x), static_cast<float>(y)),
                            ImVec2(static_cast<float>(x + w), static_cast<float>(y + h)));
            ImGui::ItemAdd(bb, id, nullptr, ImGuiItemFlags_NoNav);
            bool hovered = false, held = false;
            const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick);

            ImGuiStorage* st = window->DC.StateStorage;
            const ImGuiID part_key = id + 1, grab_key = id + 2, repeat_key = id + 3;
            int part = st->GetInt(part_key, 0); // 1 up, 2 down, 3 thumb
            const float before = *pos;
            const float track = static_cast<float>(h - 34);
            auto thumb_size = [&]
            {
                return static_cast<int>(std::max(size * track, k_min_thumb));
            };
            auto thumb_pos = [&]
            {
                return static_cast<int>(*pos * (track - static_cast<float>(thumb_size())));
            };
            const int my = static_cast<int>(std::floor(g.IO.MousePos.y));
            bool up_hot = false, down_hot = false, thumb_hot = part == 3;
            if (part != 3 && hovered)
            {
                up_hot = my < y + 16;
                down_hot = my >= y + h - 16;
                thumb_hot = my >= y + thumb_pos() + 18 && my < y + thumb_pos() + thumb_size() + 18;
            }
            if (!held)
                part = 0;
            if (part == 1 && !up_hot)
                part = 0;
            if (part == 2 && !down_hot)
                part = 0;
            if (pressed)
            {
                part = up_hot ? 1 : (down_hot ? 2 : (thumb_hot ? 3 : 0));
                st->SetFloat(repeat_key, static_cast<float>(now()) + k_repeat_delay);
                if (part == 3)
                    st->SetInt(grab_key, my - thumb_pos() - y - 17);
            }
            if (part == 1 || part == 2)
            {
                if (now() > static_cast<double>(st->GetFloat(repeat_key, 0.f)))
                {
                    *pos = std::clamp(*pos + (part == 1 ? -size : size), 0.f, 1.f);
                    st->SetFloat(repeat_key, static_cast<float>(now()) + k_repeat_interval);
                }
            }
            else if (part == 3)
            {
                const float lo = static_cast<float>(y + 17);
                const float hi = static_cast<float>(y + 17 + (h - 34) - thumb_size());
                const float cur = static_cast<float>(my - st->GetInt(grab_key, 0));
                *pos = lo == hi ? (cur < hi ? 0.f : 1.f) : std::clamp((cur - lo) / (hi - lo), 0.f, 1.f);
            }
            st->SetInt(part_key, part);

            const int up_slot = up_hot ? (part == 1 ? 5 : 4) : 3;
            const int dn_slot = down_hot ? (part == 2 ? 5 : 4) : 3;
            const int th_slot = thumb_hot ? (part == 3 ? 5 : 4) : 3;
            OutlineRect(dl, x, y, x + w, y + h, tc(TC_BORDER));
            AddIcon(dl, Icon::ArrowDown, x, y, x + 16, y + 16, tc(up_slot), true);
            OutlineRect(dl, x, y, x + 16, y + 16, tc(up_slot));
            const int by = y + h;
            AddIcon(dl, Icon::ArrowDown, x, by - 16, x + 16, by, tc(dn_slot));
            OutlineRect(dl, x, by - 16, x + 16, by, tc(dn_slot));
            const int ts = thumb_size();
            const int ty = y + thumb_pos() + 17;
            FillRect(dl, x + 2, ty, x + 14, ty + ts, tc(th_slot));
            const int gy = static_cast<int>(static_cast<float>(ts) * 0.5f + static_cast<float>(ty) - 2.f);
            for (int i = 0; i < 3; ++i)
                Line(dl, x + 4, gy + 2 * i, x + 12, gy + 2 * i, tc(TC_BG));
            return *pos != before;
        }

        // The minimise / close button of the title bar (window.cpp's TitleButton).
        struct TitleButton
        {
            ImRect bb;
            bool hovered = false, held = false, pressed = false;
        };

        static void paint_title_button(ImDrawList* dl, const TitleButton& b, bool close, bool shared_focused)
        {
            const int x = static_cast<int>(b.bb.Min.x), y = static_cast<int>(b.bb.Min.y);
            int fill;
            if (close)
                fill = b.held ? TC_EXIT_PRESSED : (b.hovered ? TC_EXIT_FOCUSED : TC_BG);
            else
                fill = b.held ? TC_BG_FOCUSED : (b.hovered ? TC_BG_ALT : TC_BG);
            FillRect(dl, x, y, x + 16, y + 16, tc(fill));
            AddIcon(dl, close ? Icon::Cross : Icon::Minus, x, y, x + 16, y + 16, ImVec4(1, 1, 1, 1));
            OutlineRect(dl, x, y, x + 16, y + 16, tc(b.hovered ? TC_BORDER_FOCUSED : TC_BORDER));
            if (shared_focused) // the column the two share: one colour whichever is painted last
            {
                const int shared_x = close ? x : x + 15;
                FillRect(dl, shared_x, y, shared_x + 1, y + 16, tc(TC_BORDER_FOCUSED));
            }
        }

        // The frame and value of a text field (UITextBox / UINumberBox / UIComboBox share it): fill and border by
        // state; `split` draws the line before the right-hand h x h square.
        void paint_field_frame(ImDrawList* dl, int x, int y, int w, int h, bool hovered, bool held, bool editing,
                               bool split)
        {
            const int fill = (editing || !held) ? (hovered ? 1 : 0) : 2;
            const int border = held ? 5 : (hovered ? 4 : (editing ? 6 : 3));
            FillRect(dl, x, y, x + w, y + h, tc(TC_BG + fill));
            OutlineRect(dl, x, y, x + w, y + h, tc(border));
            if (split)
                Line(dl, x + w - h, y + 1, x + w - h, y + h - 1, tc(border));
        }

        // The text of a field that is not being edited: what fits in the client width with 4 px margins.
        void paint_field_text(ImDrawList* dl, int x, int y, int client_w, int h, const char* text, int slot)
        {
            if (!text || !*text)
                return;
            const char* end = fit_end(normal_font(), text, nullptr, client_w - 8);
            AddText(dl, normal_font(), x + 4, center_y(h, text_h(), y), tc(slot), text, end);
        }

        // A field being edited, painted as UITextBox::paint_text_only paints it: the text from a scroll position kept
        // in characters (4 of them around the caret stay in view), the selection box, the selected text in
        // TC_TEXT_SELECTED, the caret line (on for 1 s, then blinking every 0.5 s).  The text, caret and selection
        // are ImGui's input state's.
        static void paint_edit(ImDrawList* dl, ImGuiID id, int x, int y, int client_w, int h, const char* hint)
        {
            ImGuiInputTextState* state = ImGui::GetInputTextState(id);
            if (!state)
                return;
            ImGuiStorage* st = GImGui->CurrentWindow->DC.StateStorage;
            const ImGuiID scroll_key = ImHashStr("##nc_scroll", 0, id), start_key = ImHashStr("##nc_start", 0, id);
            const char* text = state->GetText();
            const int bytes = state->TextLen;
            ImVector<int> at; // the characters' byte offsets, and the end
            for (int b = 0; b < bytes;)
            {
                at.push_back(b);
                unsigned int c = 0;
                b += std::max(ImTextCharFromUtf8(&c, text + b, text + bytes), 1);
            }
            at.push_back(bytes);
            const int len = at.Size - 1;
            auto index_of = [&](int byte)
            {
                int i = 0;
                while (i < len && at[i] < byte)
                    ++i;
                return i;
            };
            ImFont* font = normal_font();
            ImFontBaked* baked = font->GetFontBaked(font_size());
            auto char_w = [&](int i)
            {
                unsigned int c = 0;
                ImTextCharFromUtf8(&c, text + at[i], text + bytes);
                return static_cast<int>(baked->GetCharAdvance(static_cast<ImWchar>(c)));
            };
            auto slice = [&](int a, int b)
            {
                int w = 0;
                for (int i = a; i < b; ++i)
                    w += char_w(i);
                return w;
            };
            auto layout = [&](int from) // update_text_layout: the characters that fit with 4 px margins
            {
                int visible = 0, acc = 4;
                for (int i = from; i < len; ++i)
                {
                    acc += char_w(i);
                    if (acc > client_w - 4)
                        break;
                    ++visible;
                }
                return visible;
            };

            // the toolkit keeps a caret (the selection's anchor) and a signed selection length to the moving end
            const int cursor = index_of(state->GetCursorPos());
            int caret = cursor, sel = 0;
            if (state->HasSelection())
            {
                const int s0 = index_of(state->GetSelectionStart()), s1 = index_of(state->GetSelectionEnd());
                caret = cursor == s1 ? s0 : s1;
                sel = cursor - caret;
            }
            // set_selection
            int scroll = std::clamp(st->GetInt(scroll_key, 0), 0, len);
            int visible = layout(scroll);
            const int endp = caret + sel;
            int lo = caret, hi = caret;
            if (sel < 0)
                lo += sel;
            else if (sel > 0)
                hi = caret + sel;
            const int keep_lo = std::max(0, endp - 4), keep_hi = std::min(len, endp + 4);
            if (scroll > keep_lo)
            {
                scroll = keep_lo;
                visible = layout(scroll);
            }
            else if (keep_hi > visible + scroll)
            {
                scroll = std::max(0, keep_hi - visible);
                visible = layout(scroll);
            }
            st->SetInt(scroll_key, scroll);
            if (lo < scroll)
                lo = scroll;
            const int sel_x = slice(scroll, lo);
            const int sel_w = slice(lo, std::min(hi, visible + scroll));

            // paint_text_only
            if (sel != 0)
            {
                const int sx = x + sel_x + 4;
                FillRect(dl, sx, y + 2, sx + sel_w, y + h - 2, tc(TC_BG_FOCUSED));
            }
            const int ty = center_y(h, text_h(), y);
            auto str = [&](int px, int slot, int a, int b)
            {
                if (b > a)
                    AddText(dl, font, px, ty, tc(slot), text + at[a], text + at[b]);
            };
            if (len > 0)
            {
                const int end_idx = scroll + std::min(len - scroll, visible);
                if (sel != 0)
                {
                    int l = caret, r = caret;
                    if (sel < 0)
                        l += sel;
                    else
                        r += sel;
                    l = std::max(l, scroll);
                    r = std::min(r, end_idx);
                    if (l > scroll)
                        str(x + 4, TC_TEXT, scroll, l);
                    str(x + 4 + sel_x, TC_TEXT_SELECTED, l, r);
                    if (r < scroll + visible)
                        str(x + 4 + sel_x + sel_w, TC_TEXT, r, end_idx);
                }
                else
                    str(x + 4, TC_TEXT, scroll, end_idx);
            }
            else if (hint && *hint)
                AddText(dl, font, x + 4, ty, tc(TC_INACTIVE), hint);
            const double t = now() - static_cast<double>(st->GetFloat(start_key, 0.f));
            if (t < 1.0 || static_cast<int>((t - 1.0) / 0.5) % 2 == 1)
            {
                const int cx = (sel < 0 ? sel_x : sel_x + sel_w) + x + 4;
                Line(dl, cx, y + 3, cx, y + h - 3, tc(TC_BORDER));
            }
        }

        // Edits `buf` over a field's client area: ImGui's text input (TempInputText without its select-all, so the
        // press that starts the edit puts the caret under the cursor, as UITextBox::begin_edit) does the keys, the
        // mouse and the clipboard and paints nothing; paint_edit paints.  `id_label` must give `id` (the widget's).
        bool edit_field(ImGuiID id, const char* id_label, const ImRect& text_bb, char* buf, int buf_size,
                        ImGuiInputTextFlags flags, ImDrawList* dl, const char* hint, ImGuiInputTextCallback callback,
                        void* user_data)
        {
            ImGuiContext& g = *GImGui;
            ImGuiWindow* window = g.CurrentWindow;
            const bool init = g.TempInputId != id;
            if (init)
                ImGui::ClearActiveID();
            const ImVec4 clear(0, 0, 0, 0);
            for (const ImGuiCol col :
                 {ImGuiCol_Text, ImGuiCol_TextSelectedBg, ImGuiCol_InputTextCursor, ImGuiCol_FrameBg,
                  ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_NavCursor})
                ImGui::PushStyleColor(col, clear);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
            ImGui::PushStyleVar(
                    ImGuiStyleVar_FramePadding,
                    ImVec2(4.f, static_cast<float>(center_y(static_cast<int>(text_bb.GetHeight()), text_h(), 0))));
            // (InputTextEx lays out text_bb as an item of its own: the widget's own layout, over its whole box, is
            // put back after it, so a SameLine() that follows starts after the box, not the text part)
            const ImVec2 backup = window->DC.CursorPos, backup_prev_line = window->DC.CursorPosPrevLine;
            const ImVec2 backup_prev_size = window->DC.PrevLineSize;
            const float backup_prev_base = window->DC.PrevLineTextBaseOffset;
            window->DC.CursorPos = text_bb.Min;
            g.LastItemData.ItemFlags |= ImGuiItemFlags_AllowDuplicateId;
            const bool changed = ImGui::InputTextEx(id_label, nullptr, buf, buf_size, text_bb.GetSize(),
                                                    flags | ImGuiInputTextFlags_TempInput, callback, user_data);
            ImGui::KeepAliveID(id);
            if (init && g.ActiveId == id)
            {
                g.TempInputId = id;
                window->DC.StateStorage->SetFloat(ImHashStr("##nc_start", 0, id), static_cast<float>(now()));
                window->DC.StateStorage->SetInt(ImHashStr("##nc_scroll", 0, id), 0);
            }
            if (g.ActiveId != id)
                g.TempInputId = 0;
            window->DC.CursorPos = backup;
            window->DC.CursorPosPrevLine = backup_prev_line;
            window->DC.PrevLineSize = backup_prev_size;
            window->DC.PrevLineTextBaseOffset = backup_prev_base;
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(7);
            if (g.TempInputId == id)
                paint_edit(dl, id, static_cast<int>(text_bb.Min.x), static_cast<int>(text_bb.Min.y),
                           static_cast<int>(text_bb.GetWidth()), static_cast<int>(text_bb.GetHeight()), hint);
            return changed;
        }

    } // namespace detail

    using namespace detail;

    // ---- palette / fonts / style ------------------------------------------------------------------------------------

    Palette Palette::Factory()
    {
        return FromDwords(k_factory.data(), static_cast<int>(k_factory.size()));
    }

    Palette Palette::FromDwords(const std::uint32_t* dwords, int count)
    {
        Palette p;
        for (int i = 0; i < SlotCount; ++i)
            (&p.Bg)[i] = ColorFromDword(dwords && i < count ? dwords[i] : k_factory[static_cast<std::size_t>(i)]);
        return p;
    }

    ImFont* AddFont(ImFontAtlas* atlas, const FontSpec& spec)
    {
        ImFontConfig cfg;
        cfg.SizePixels = spec.LineHeight();
        std::snprintf(cfg.Name, sizeof(cfg.Name), "%s %d%s", spec.Family, spec.Tall, spec.Weight >= 700 ? " bold" : "");
        detail::GdiFontData font{};
        for (int i = 0; spec.Family[i] && i < static_cast<int>(std::size(font.family)) - 1; ++i)
            font.family[i] = static_cast<wchar_t>(static_cast<unsigned char>(spec.Family[i]));
        font.weight = spec.Weight;
        font.outline = spec.Outline ? 1 : 0;
        font.shadow = spec.DropShadow ? 1 : 0;
        font.antialias = spec.Antialias;
        // GDI where there is one (unless the baked glyphs are asked for), else the baked glyphs of this style
        const ImFontLoader* loader = core().options.Glyphs != GlyphSource::Baked ? detail::gdi_font_loader() : nullptr;
        if (!loader && detail::find_baked_font(font, static_cast<int>(spec.LineHeight())))
            loader = detail::baked_font_loader();
        if (loader)
        {
            auto* data = static_cast<detail::GdiFontData*>(IM_ALLOC(sizeof(detail::GdiFontData)));
            *data = font;
            cfg.FontLoader = loader;
            cfg.FontData = data;
            cfg.FontDataSize = static_cast<int>(sizeof(detail::GdiFontData));
            cfg.FontDataOwnedByAtlas = true; // freed by the atlas
            return atlas->AddFont(&cfg);
        }
        return atlas->AddFontDefault(&cfg); // neither: ImGui's own font at the same line height
    }

    Fonts LoadFonts(ImGuiIO& io, int tall)
    {
        Fonts f;
        FontSpec spec;
        spec.Tall = tall;
        f.Normal = AddFont(io.Fonts, spec);
        spec.Weight = 700;
        f.Header = AddFont(io.Fonts, spec);
        f.Size = spec.LineHeight();
        io.FontDefault = f.Normal;
        core().fonts = f;
        return f;
    }

    const Fonts& GetFonts()
    {
        return core().fonts;
    }

    const Palette& GetPalette()
    {
        return core().palette;
    }

    Options& GetOptions()
    {
        return core().options;
    }

    void ApplyColorMode(ImDrawData* draw_data)
    {
        if (!draw_data || core().options.Colors != ColorMode::SourceGamma)
            return;
        // vgui's curve for a flat colour (nclib's d3d9 backend, source_flat_color): gamma 2.2 to linear, then the sRGB
        // curve, which is not quite its inverse (0x23 -> 0x1E, 0x42 -> 0x40, 0xC3 -> 0xC4)
        static const std::array<ImU32, 256> k_curve = []
        {
            std::array<ImU32, 256> t{};
            for (std::size_t i = 0; i < t.size(); ++i)
            {
                const double lin = std::pow(static_cast<double>(i) / 255.0, 2.2);
                const double s = lin <= 0.0031308 ? lin * 12.92 : 1.055 * std::pow(lin, 1.0 / 2.4) - 0.055;
                t[i] = static_cast<ImU32>(std::clamp(s * 255.0 + 0.5, 0.0, 255.0));
            }
            return t;
        }();
        for (ImDrawList* list : draw_data->CmdLists)
            for (ImDrawVert& v : list->VtxBuffer)
                v.col = (v.col & IM_COL32_A_MASK) | k_curve[(v.col >> IM_COL32_R_SHIFT) & 0xFFu] << IM_COL32_R_SHIFT
                        | k_curve[(v.col >> IM_COL32_G_SHIFT) & 0xFFu] << IM_COL32_G_SHIFT
                        | k_curve[(v.col >> IM_COL32_B_SHIFT) & 0xFFu] << IM_COL32_B_SHIFT;
    }

    void SetMouseCursor(Cursor cursor)
    {
        core().cursor = cursor;
        core().cursor_frame = GImGui->FrameCount;
        static constexpr ImGuiMouseCursor k_imgui[] = {ImGuiMouseCursor_Arrow,    ImGuiMouseCursor_TextInput,
                                                       ImGuiMouseCursor_Hand,     ImGuiMouseCursor_ResizeAll,
                                                       ImGuiMouseCursor_ResizeEW, ImGuiMouseCursor_ResizeNS,
                                                       ImGuiMouseCursor_Arrow};
        ImGui::SetMouseCursor(k_imgui[static_cast<int>(cursor)]);
    }

    Cursor GetMouseCursor()
    {
        return core().cursor_frame == GImGui->FrameCount ? core().cursor : Cursor::Arrow;
    }

    void Shutdown()
    {
        shutdown_color_pickers();
        free_texture(core().icons);
    }

    void StyleColorsNullcore(ImGuiStyle* dst, const Palette& p)
    {
        core().palette = p;
        ImGuiStyle& s = dst ? *dst : ImGui::GetStyle();

        // ---- metrics: the toolkit's 16 px rows 2 px apart, square, 1 px lines
        s.WindowPadding = ImVec2(3.f, 2.f); // the 1 px frame + the 2 px content padding; the title bar is 16 px
        s.FramePadding = ImVec2(4.f, 1.f); // 14 px lines + 2 = the 16 px rows and title bar
        s.ItemSpacing = ImVec2(static_cast<float>(k_gap), static_cast<float>(k_gap));
        s.ItemInnerSpacing = ImVec2(4.f, 4.f);
        s.CellPadding = ImVec2(4.f, 1.f);
        s.IndentSpacing = 12.f;
        s.ScrollbarSize = 16.f;
        s.GrabMinSize = k_min_thumb;
        s.WindowBorderSize = 0.f; // Begin paints the frame
        s.ChildBorderSize = 1.f;
        s.PopupBorderSize = 1.f;
        s.FrameBorderSize = 1.f;
        s.TabBorderSize = 0.f;
        s.WindowRounding = 0.f;
        s.ChildRounding = 0.f;
        s.FrameRounding = 0.f;
        s.PopupRounding = 0.f;
        s.ScrollbarRounding = 0.f;
        s.GrabRounding = 0.f;
        s.TabRounding = 0.f;
        s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
        s.WindowMenuButtonPosition = ImGuiDir_None; // the title bar carries nullcore's own buttons
        s.ButtonTextAlign = ImVec2(0.5f, 0.5f);
        s.SelectableTextAlign = ImVec2(0.f, 0.5f);
        s.AntiAliasedLines = false; // hard pixels
        s.AntiAliasedLinesUseTex = false;
        s.AntiAliasedFill = false;
        if (core().fonts.Normal)
            s.FontSizeBase = core().fonts.Size;

        // ---- colours
        ImVec4* c = s.Colors;
        const ImVec4 clear(0.f, 0.f, 0.f, 0.f);
        c[ImGuiCol_Text] = p.Text;
        c[ImGuiCol_TextDisabled] = p.Inactive;
        c[ImGuiCol_WindowBg] = p.Bg;
        c[ImGuiCol_ChildBg] = clear;
        c[ImGuiCol_PopupBg] = p.Bg;
        c[ImGuiCol_Border] = p.Border;
        c[ImGuiCol_BorderShadow] = clear;
        c[ImGuiCol_FrameBg] = p.Bg;
        c[ImGuiCol_FrameBgHovered] = p.BgAlt;
        c[ImGuiCol_FrameBgActive] = p.BgFocused;
        c[ImGuiCol_TitleBg] = p.Bg;
        c[ImGuiCol_TitleBgActive] = p.Bg;
        c[ImGuiCol_TitleBgCollapsed] = p.Bg;
        c[ImGuiCol_MenuBarBg] = p.BgAlt;
        c[ImGuiCol_ScrollbarBg] = p.Bg;
        c[ImGuiCol_ScrollbarGrab] = p.Border;
        c[ImGuiCol_ScrollbarGrabHovered] = p.BorderAlt;
        c[ImGuiCol_ScrollbarGrabActive] = p.BorderFocused;
        c[ImGuiCol_CheckMark] = p.BorderAlt;
        c[ImGuiCol_SliderGrab] = p.BorderAlt;
        c[ImGuiCol_SliderGrabActive] = p.BorderFocused;
        c[ImGuiCol_Button] = p.Bg;
        c[ImGuiCol_ButtonHovered] = p.BgAlt;
        c[ImGuiCol_ButtonActive] = p.BgFocused;
        c[ImGuiCol_Header] = p.BgAlt;
        c[ImGuiCol_HeaderHovered] = p.BgFocused;
        c[ImGuiCol_HeaderActive] = p.BorderSelected;
        c[ImGuiCol_Separator] = p.Border;
        c[ImGuiCol_SeparatorHovered] = p.BorderAlt;
        c[ImGuiCol_SeparatorActive] = p.BorderFocused;
        c[ImGuiCol_ResizeGrip] = clear; // the toolkit's windows have none
        c[ImGuiCol_ResizeGripHovered] = clear;
        c[ImGuiCol_ResizeGripActive] = clear;
        c[ImGuiCol_InputTextCursor] = p.Border; // the text box's caret
        c[ImGuiCol_Tab] = p.BgAlt;
        c[ImGuiCol_TabHovered] = p.BgFocused;
        c[ImGuiCol_TabSelected] = p.Border;
        c[ImGuiCol_TabSelectedOverline] = p.BorderFocused;
        c[ImGuiCol_TabDimmed] = p.Bg;
        c[ImGuiCol_TabDimmedSelected] = p.Border;
        c[ImGuiCol_TabDimmedSelectedOverline] = p.BorderAlt;
        c[ImGuiCol_PlotLines] = p.TextAlt;
        c[ImGuiCol_PlotLinesHovered] = p.TextSelected;
        c[ImGuiCol_PlotHistogram] = p.BorderAlt;
        c[ImGuiCol_PlotHistogramHovered] = p.BorderFocused;
        c[ImGuiCol_TableHeaderBg] = p.BgAlt;
        c[ImGuiCol_TableBorderStrong] = p.Border;
        c[ImGuiCol_TableBorderLight] = p.Border;
        c[ImGuiCol_TableRowBg] = clear;
        c[ImGuiCol_TableRowBgAlt] = clear;
        c[ImGuiCol_TextLink] = p.TextSelected;
        c[ImGuiCol_TextSelectedBg] = p.BgFocused; // the text box's selection
        c[ImGuiCol_TreeLines] = p.Border;
        c[ImGuiCol_DragDropTarget] = p.TextSelected;
        c[ImGuiCol_UnsavedMarker] = p.TextSelected;
        c[ImGuiCol_NavCursor] = p.BorderFocused;
        c[ImGuiCol_NavWindowingHighlight] = p.TextFocused;
        c[ImGuiCol_NavWindowingDimBg] = ImVec4(p.Bg.x, p.Bg.y, p.Bg.z, 0.5f);
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(16.f / 255.f, 16.f / 255.f, 16.f / 255.f, 150.f / 255.f);

        if (!dst && ImGui::GetCurrentContext())
            ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true; // the toolkit drags by the title bar only
    }

    // ---- primitives -------------------------------------------------------------------------------------------------

    void FillRect(ImDrawList* dl, int x0, int y0, int x1, int y1, const ImVec4& c)
    {
        if (x1 <= x0 || y1 <= y0 || c.w <= 0.f)
            return;
        dl->AddRectFilled(ImVec2(static_cast<float>(x0), static_cast<float>(y0)),
                          ImVec2(static_cast<float>(x1), static_cast<float>(y1)), u32(c));
    }

    // the four 1 px edges, no pixel twice (ShapeTessellator::outline_rect)
    void OutlineRect(ImDrawList* dl, int x0, int y0, int x1, int y1, const ImVec4& c)
    {
        if (x1 <= x0 || y1 <= y0 || c.w <= 0.f)
            return;
        FillRect(dl, x0, y0, x1, y0 + 1, c);
        if (y1 - y0 > 1)
            FillRect(dl, x0, y1 - 1, x1, y1, c);
        if (y1 - y0 > 2)
        {
            FillRect(dl, x0, y0 + 1, x0 + 1, y1 - 1, c);
            if (x1 - x0 > 1)
                FillRect(dl, x1 - 1, y0 + 1, x1, y1 - 1, c);
        }
    }

    void Line(ImDrawList* dl, int x0, int y0, int x1, int y1, const ImVec4& c)
    {
        if (x0 == x1 || y0 == y1) // the pixels from one end to the other
        {
            FillRect(dl, std::min(x0, x1), std::min(y0, y1), std::max(x0, x1) + 1, std::max(y0, y1) + 1, c);
            return;
        }
        // a 1 px quad over the line's pixel centres, half a pixel past each end (ShapeTessellator::segment)
        const float fx0 = static_cast<float>(x0) + 0.5f, fy0 = static_cast<float>(y0) + 0.5f;
        const float fx1 = static_cast<float>(x1) + 0.5f, fy1 = static_cast<float>(y1) + 0.5f;
        const float dx = fx1 - fx0, dy = fy1 - fy0, len = std::sqrt(dx * dx + dy * dy);
        const float ux = dx / len * 0.5f, uy = dy / len * 0.5f, nx = uy, ny = -ux;
        dl->AddQuadFilled(ImVec2(fx0 - ux + nx, fy0 - uy + ny), ImVec2(fx1 + ux + nx, fy1 + uy + ny),
                          ImVec2(fx1 + ux - nx, fy1 + uy - ny), ImVec2(fx0 - ux - nx, fy0 - uy - ny), u32(c));
    }

    void AddIcon(ImDrawList* dl, Icon icon, int x0, int y0, int x1, int y1, const ImVec4& tint, bool flip_v)
    {
        const int i = static_cast<int>(icon);
        if (i < 0 || i >= static_cast<int>(Icon::Count) || tint.w <= 0.f)
            return;
        ImTextureData* tex = icon_texture();
        const float tw = static_cast<float>(tex->Width), th = static_cast<float>(tex->Height);
        ImVec2 uv0(static_cast<float>(i * k_icon_cell + 1) / tw, 1.f / th);
        ImVec2 uv1(static_cast<float>(i * k_icon_cell + 17) / tw, 17.f / th);
        if (flip_v)
            std::swap(uv0.y, uv1.y);
        dl->AddImage(tex->GetTexRef(), ImVec2(static_cast<float>(x0), static_cast<float>(y0)),
                     ImVec2(static_cast<float>(x1), static_cast<float>(y1)), uv0, uv1, u32(tint));
    }

    void AddText(ImDrawList* dl, ImFont* font, int x, int y, const ImVec4& c, const char* text, const char* text_end)
    {
        if (!text || text == text_end)
            return;
        dl->AddText(font ? font : normal_font(), font_size(), ImVec2(static_cast<float>(x), static_cast<float>(y)),
                    u32(c), text, text_end);
    }

    int CalcTextWidth(ImFont* font, const char* text, const char* text_end)
    {
        if (!text)
            return 0;
        font = font ? font : normal_font();
        return static_cast<int>(font->CalcTextSizeA(font_size(), FLT_MAX, 0.f, text, text_end).x);
    }

    // ---- window -----------------------------------------------------------------------------------------------------

    bool Begin(const char* name, bool* p_open, ImGuiWindowFlags flags)
    {
        ImGuiContext& g = *GImGui;
        // the title is painted here: ImGui gets an empty one ("##name")
        const std::string id = std::string("##") + name;
        ImGuiWindow* existing = ImGui::FindWindowByName(id.c_str());
        WindowState state;
        state.dimmed = core().options.UnfocusedTransparency && existing && !is_focused(existing);
        if (state.dimmed)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, k_unfocused_alpha);

        // Widget::clamp_to_screen: a window stays on the screen (dragged or not), unless its place is set this frame
        // (SetNextWindowPos with ImGuiCond_Always)
        const bool placed = (g.NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasPos)
                            && (g.NextWindowData.PosCond == 0 || (g.NextWindowData.PosCond & ImGuiCond_Always));
        if (core().options.ClampToScreen && existing && !placed)
        {
            const ImVec2 display = g.IO.DisplaySize;
            const ImVec2 clamped(std::fmin(std::fmax(existing->Pos.x, 0.f), display.x - existing->Size.x),
                                 std::fmin(std::fmax(existing->Pos.y, 0.f), display.y - existing->Size.y));
            if (clamped.x != existing->Pos.x || clamped.y != existing->Pos.y)
                ImGui::SetNextWindowPos(clamped);
        }

        // (no ImGui border: its line under the title bar would be painted twice with ours)
        const float frame_border = g.Style.FrameBorderSize;
        g.Style.FrameBorderSize = 0.f;
        const bool open = ImGui::Begin(id.c_str(), nullptr, flags | ImGuiWindowFlags_NoScrollbar);
        g.Style.FrameBorderSize = frame_border;
        ImGuiWindow* window = g.CurrentWindow;
        ImDrawList* dl = window->DrawList;
        const int x = static_cast<int>(window->Pos.x), y = static_cast<int>(window->Pos.y);
        const int w = static_cast<int>(window->Size.x);
        const int h = window->Collapsed ? k_title : static_cast<int>(window->Size.y);

        ImGui::PushClipRect(window->Pos, ImVec2(window->Pos.x + window->Size.x, window->Pos.y + static_cast<float>(h)),
                            false);
        if (!(flags & ImGuiWindowFlags_NoTitleBar))
        {
            // Widget::paint: the frame, the line under the title, the title
            OutlineRect(dl, x, y, x + w, y + h, tc(TC_BORDER));
            Line(dl, x, y + 15, x + w - 1, y + 15, tc(TC_BORDER));
            const char* title_end = ImGui::FindRenderedTextEnd(name);
            if (title_end != name)
            {
                const int tw = CalcTextWidth(header_font(), name, title_end);
                AddText(dl, header_font(), x + (w - tw) / 2, y + 1, tc(TC_TEXT_FOCUSED), name, title_end);
            }

            // the buttons: hit-tested minimise first (it wins the shared column, as in the toolkit), painted close
            // first (the window paints its children back to front)
            TitleButton min_b, close_b;
            const bool has_min = !(flags & ImGuiWindowFlags_NoCollapse);
            const bool has_close = p_open != nullptr;
            min_b.bb = ImRect(ImVec2(static_cast<float>(x + w - 31), static_cast<float>(y)),
                              ImVec2(static_cast<float>(x + w - 15), static_cast<float>(y + 16)));
            close_b.bb = ImRect(ImVec2(static_cast<float>(x + w - 16), static_cast<float>(y)),
                                ImVec2(static_cast<float>(x + w), static_cast<float>(y + 16)));
            auto behave = [&](TitleButton& b, const char* str_id)
            {
                const ImGuiID bid = window->GetID(str_id);
                ImGui::KeepAliveID(bid);
                b.pressed = ImGui::ButtonBehavior(b.bb, bid, &b.hovered, &b.held);
            };
            if (has_min)
                behave(min_b, "#NC_MIN");
            if (has_close)
                behave(close_b, "#NC_CLOSE");
            const bool shared = (has_min && min_b.hovered) || (has_close && close_b.hovered);
            // Widget::on_mouse_move: the title bar's row (its buttons are children: they keep the arrow)
            if (g.HoveredWindow == window && !shared
                && ImGui::IsMouseHoveringRect(window->Pos, ImVec2(window->Pos.x + window->Size.x, window->Pos.y + 16.f),
                                              false))
                SetMouseCursor(Cursor::SizeAll);
            if (has_close)
                paint_title_button(dl, close_b, true, shared && has_min);
            if (has_min)
                paint_title_button(dl, min_b, false, shared && has_close);
            if (has_close && close_b.pressed)
                *p_open = false;
            if (has_min && min_b.pressed)
            {
                if (core().options.Minimize)
                    core().options.Minimize(name);
                else
                    ImGui::SetWindowCollapsed(!window->Collapsed);
            }
        }
        else
            OutlineRect(dl, x, y, x + w, y + h, tc(TC_BORDER));
        ImGui::PopClipRect();

        // nullcore's scroll bar instead of ImGui's: docked on the right over the frame, the content 16 px narrower
        state.scrollbar =
                open && !window->Collapsed && window->ScrollMax.y > 0.f && !(flags & ImGuiWindowFlags_NoScrollbar);
        if (state.scrollbar)
        {
            window->WorkRect.Max.x -= 16.f;
            window->ContentRegionRect.Max.x -= 16.f;
            ImGui::PushClipRect(window->InnerClipRect.Min,
                                ImVec2(window->Pos.x + window->Size.x - 16.f, window->InnerClipRect.Max.y), true);
        }
        window->DC.ItemWidth = -FLT_MIN; // nullcore's rows fill the window (PushItemWidth still narrows them)
        if (g.MovingWindow == window) // (a dragged window follows the cursor: dc_sizeall)
            SetMouseCursor(Cursor::SizeAll);
        core().windows.push_back(state);
        return open;
    }

    void End()
    {
        IM_ASSERT_USER_ERROR_RET(!core().windows.empty(), "End() without Begin()");
        const WindowState state = core().windows.back();
        core().windows.pop_back();
        if (state.scrollbar)
        {
            ImGui::PopClipRect();
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            const int x = static_cast<int>(window->Pos.x), y = static_cast<int>(window->Pos.y);
            const int w = static_cast<int>(window->Size.x), h = static_cast<int>(window->Size.y);
            const float max = window->ScrollMax.y;
            float pos = max > 0.f ? window->Scroll.y / max : 0.f;
            const float view = window->InnerRect.GetHeight();
            const float size = view / std::max(view + max, 1.f);
            ImGui::PushClipRect(window->Pos, ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y),
                                false);
            if (scrollbar(window->GetID("#NC_SCROLL"), x + w - 16, y + 15, 16, h - 15, &pos, size))
                ImGui::SetScrollY(window, std::round(pos * max));
            ImGui::PopClipRect();
        }
        ImGui::End();
        if (state.dimmed)
            ImGui::PopStyleVar();
    }

    // ---- simple widgets ---------------------------------------------------------------------------------------------

    void SetNextItemSwatch(ImU32* col, const char* picker_title, bool alpha)
    {
        core().next_swatch = {col, picker_title, alpha};
    }

    void PushLabelWidth(float width)
    {
        core().label_widths.push_back(width);
    }

    void PopLabelWidth()
    {
        IM_ASSERT_USER_ERROR_RET(!core().label_widths.empty(), "PopLabelWidth() without PushLabelWidth()");
        core().label_widths.pop_back();
    }

    namespace
    {

        // UITextWidget::paint_text: `text` in a w x 16 row, placed by align_x (2 px from the edges), in `slot`.
        void paint_row_text(ImDrawList* dl, const ImRect& bb, const char* text, const char* end, float align_x,
                            int slot)
        {
            const int x = static_cast<int>(bb.Min.x), y = static_cast<int>(bb.Min.y),
                      iw = static_cast<int>(bb.GetWidth());
            const int tw = CalcTextWidth(normal_font(), text, end);
            const int ty = y + (static_cast<int>(bb.GetHeight()) - text_h()) / 2;
            int tx = x + 2;
            if (align_x >= 1.f)
                tx = iw - tw + x - 2;
            else if (align_x > 0.f)
                tx = x + (iw - tw) / 2;
            AddText(dl, normal_font(), tx, ty, tc(slot), text, end);
        }

        // UILabel / UILinkLabel: hoverable text, TC_TEXT_SELECTED hovered, TC_TEXT_FOCUSED pressed.
        // `fit`: as wide as the text (+ 2 px each side), else `width` (0: the item width).
        bool label_ex(const char* text, const char* end, float align_x, float width, bool disabled, bool fit,
                      bool link = false)
        {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            if (window->SkipItems)
                return false;
            const ImGuiID id = window->GetID(text); // (the whole label: "##" and "###" tell the ids apart)
            const float w =
                    fit ? static_cast<float>(CalcTextWidth(normal_font(), text, end) + 4) : resolve_width(width);
            ImRect bb;
            if (!add_item(id, w, static_cast<float>(row_height()), bb))
                return false;
            bool hovered = false, held = false;
            const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
            if (link && hovered) // UILinkLabel::on_mouse_move: dc_hand
                SetMouseCursor(Cursor::Hand);
            const int slot = disabled ? TC_INACTIVE : (hovered ? (held ? TC_TEXT_FOCUSED : TC_TEXT_SELECTED) : TC_TEXT);
            paint_row_text(window->DrawList, bb, text, end, align_x, slot);
            finish_item(id, bb, hovered);
            return pressed;
        }

        // Static text: a 16 px row, 2 px in.
        void text_ex(const char* text, const char* end, int slot)
        {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            if (window->SkipItems)
                return;
            if (!end)
                end = text + std::strlen(text);
            if (table_cell_text(text, end)) // (a table cell's text: painted with the cell)
                return;
            const ImVec2 pos = window->DC.CursorPos;
            const ImRect bb(pos, ImVec2(pos.x + static_cast<float>(CalcTextWidth(normal_font(), text, end) + 4),
                                        pos.y + static_cast<float>(row_height())));
            ImGui::ItemSize(bb, ImGui::GetStyle().FramePadding.y);
            if (!ImGui::ItemAdd(bb, 0))
                return;
            paint_row_text(window->DrawList, bb, text, end, 0.f, slot);
        }

    } // namespace

    bool Label(const char* text, float align_x, float width, bool disabled)
    {
        return label_ex(text, ImGui::FindRenderedTextEnd(text), align_x, width, disabled, false);
    }

    void TextUnformatted(const char* text, const char* text_end)
    {
        text_ex(text, text_end, TC_TEXT);
    }

    void TextV(const char* fmt, va_list args)
    {
        const char* text = nullptr;
        const char* end = nullptr;
        ImFormatStringToTempBufferV(&text, &end, fmt, args);
        text_ex(text, end, TC_TEXT);
    }

    void Text(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        TextV(fmt, args);
        va_end(args);
    }

    void TextDisabled(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        const char* text = nullptr;
        const char* end = nullptr;
        ImFormatStringToTempBufferV(&text, &end, fmt, args);
        va_end(args);
        text_ex(text, end, TC_INACTIVE);
    }

    void Image(ImTextureRef tex_ref, const ImVec2& image_size, const ImVec2& size)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return;
        const ImVec2 item = ImGui::CalcItemSize(size, image_size.x, image_size.y);
        ImRect bb;
        if (!add_item(0, std::floor(item.x), std::floor(item.y), bb))
            return;
        // UIImage::paint: natural size, centred, untinted
        const int tw = static_cast<int>(image_size.x), th = static_cast<int>(image_size.y);
        const int x = static_cast<int>(bb.Min.x)
                      + static_cast<int>(static_cast<float>(static_cast<int>(bb.GetWidth()) - tw) * 0.5f);
        const int y = static_cast<int>(bb.Min.y)
                      + static_cast<int>(static_cast<float>(static_cast<int>(bb.GetHeight()) - th) * 0.5f);
        window->DrawList->AddImage(tex_ref, ImVec2(static_cast<float>(x), static_cast<float>(y)),
                                   ImVec2(static_cast<float>(x + tw), static_cast<float>(y + th)), ImVec2(0, 0),
                                   ImVec2(1, 1), u32(ImVec4(1, 1, 1, 1)));
    }

    bool TextLink(const char* label, float align_x, float width)
    {
        return label_ex(label, ImGui::FindRenderedTextEnd(label), align_x, width, false, width == 0.f, true);
    }

    namespace
    {

        // UICheckbox::paint.  `width` < 0: as wide as the box and the text (ImGui's); else the row's width (the box
        // on its right with NcCheckboxFlags_BoxRight, the text right-aligned beside it).
        bool checkbox_ex(ImGuiID id, const char* text, const char* end, bool* v, NcCheckboxFlags flags, float width)
        {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            const int side = k_row;
            const bool box_right = (flags & NcCheckboxFlags_BoxRight) != 0;
            const int tw = end != text ? CalcTextWidth(normal_font(), text, end) : 0;
            const float w = width >= 0.f ? width : static_cast<float>(side + (end != text ? 4 + tw : 0));
            ImRect bb;
            if (!add_item(id, w, static_cast<float>(k_row), bb))
                return false;
            bool hovered = false, held = false;
            const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
            if (pressed)
            {
                *v = !*v;
                ImGui::MarkItemEdited(id);
            }
            ImDrawList* dl = window->DrawList;
            const int bx = static_cast<int>(bb.Min.x), y = static_cast<int>(bb.Min.y), iw = static_cast<int>(w);
            const int x = box_right ? bx + iw - side : bx;
            FillRect(dl, x, y, x + side, y + side, tc(state_slot(TC_BG, hovered, held)));
            OutlineRect(dl, x, y, x + side, y + side, tc(state_slot(TC_BORDER, hovered, held)));
            if (*v)
                FillRect(dl, x + 3, y + 3, x + side - 3, y + side - 3, tc(TC_BORDER_ALT));
            if (end != text)
            {
                const int ty = y + (k_row - text_h()) / 2;
                AddText(dl, normal_font(), box_right ? iw - tw - side + bx - 4 : side + bx + 4, ty, tc(TC_TEXT), text,
                        end);
            }
            finish_item(id, bb, hovered);
            return pressed;
        }

    } // namespace

    bool Checkbox(const char* label, bool* v)
    {
        return Checkbox(label, v, NcCheckboxFlags_None);
    }

    bool Checkbox(const char* label, bool* v, NcCheckboxFlags flags)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;
        const float width = (flags & NcCheckboxFlags_BoxRight) ? resolve_width(0.f) : -1.f;
        return checkbox_ex(window->GetID(label), label, ImGui::FindRenderedTextEnd(label), v, flags, width);
    }

    bool LabeledCheckbox(const char* label, const char* text, bool* v, NcCheckboxFlags flags)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;
        const float width = label_column(label);
        return checkbox_ex(window->GetID(label), text, text + std::strlen(text), v, flags, width);
    }

    bool CheckboxFlags(const char* label, int* flags, int flags_value)
    {
        bool on = (*flags & flags_value) != 0;
        const bool pressed = Checkbox(label, &on);
        if (pressed)
            *flags = on ? (*flags | flags_value) : (*flags & ~flags_value);
        return pressed;
    }

    namespace
    {

        struct ButtonIcon
        {
            ImTextureRef tex;
            ImVec2 uv0, uv1;
            int w = 16, h = 16;
        };

        ButtonIcon toolkit_icon(Icon icon)
        {
            ImTextureData* tex = icon_texture();
            const float tw = static_cast<float>(tex->Width), th = static_cast<float>(tex->Height);
            const int i = static_cast<int>(icon);
            return {tex->GetTexRef(), ImVec2(static_cast<float>(i * k_icon_cell + 1) / tw, 1.f / th),
                    ImVec2(static_cast<float>(i * k_icon_cell + 17) / tw, 17.f / th), 16, 16};
        }

        // UIButton::paint: fill, icon (inset by half the height difference), the text in its client area (right of
        // the icon) placed by align_x, border last.  `size` as ImGui's: 0 fits `fit`, negative leaves that much room.
        bool button_ex(const char* id_str, const char* text, ImVec2 size, ImVec2 fit, const ButtonIcon* icon,
                       bool tint_icon, const ImVec4* fill_color, float align_x = 0.5f)
        {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            if (window->SkipItems)
                return false;
            const ImGuiID id = window->GetID(id_str);
            if (size.y == 0.f && core().next_height > 0.f)
                size.y = static_cast<float>(row_height());
            const ImVec2 item = ImGui::CalcItemSize(size, fit.x, fit.y);
            const float w = std::floor(item.x), h = std::floor(item.y);
            ImRect bb;
            if (!add_item(id, w, h, bb))
                return false;
            bool hovered = false, held = false;
            const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
            ImDrawList* dl = window->DrawList;
            const int x = static_cast<int>(bb.Min.x), y = static_cast<int>(bb.Min.y);
            const int iw = static_cast<int>(w), ih = static_cast<int>(h);
            const ImVec4& border = tc(state_slot(TC_BORDER, hovered, held));
            // a colour pushed for ImGuiCol_Button is a fixed fill (UIButton's fill colour / theme slot)
            const ImVec4& pushed = GImGui->Style.Colors[ImGuiCol_Button];
            const bool fixed = pushed.x != core().palette.Bg.x || pushed.y != core().palette.Bg.y || pushed.z != core().palette.Bg.z
                               || pushed.w != core().palette.Bg.w;
            FillRect(dl, x, y, x + iw, y + ih,
                     fill_color ? *fill_color : (fixed ? pushed : tc(state_slot(TC_BG, hovered, held))));
            int ox = 0, cw = iw;
            if (icon)
            {
                const int pad = (ih - icon->h) / 2;
                dl->AddImage(icon->tex, ImVec2(static_cast<float>(x + pad), static_cast<float>(y + pad)),
                             ImVec2(static_cast<float>(x + pad + icon->w), static_cast<float>(y + pad + icon->h)),
                             icon->uv0, icon->uv1, u32(tint_icon ? border : ImVec4(1, 1, 1, 1)));
                ox = ih + icon->w - icon->h; // the text area starts h + (icon w - icon h) to the right
                cw = iw + icon->h - icon->w - ih;
            }
            const char* end = text ? ImGui::FindRenderedTextEnd(text) : nullptr;
            if (text && end != text)
            {
                const int tw = CalcTextWidth(normal_font(), text, end);
                int tx = x + ox + 2;
                if (align_x >= 1.f)
                    tx = cw - tw + x + ox - 2;
                else if (align_x > 0.f)
                    tx = x + ox + (cw - tw) / 2;
                AddText(dl, normal_font(), tx, y + (ih - text_h()) / 2, tc(TC_TEXT), text, end);
            }
            OutlineRect(dl, x, y, x + iw, y + ih, border);
            finish_item(id, bb, hovered);
            return pressed;
        }

    } // namespace

    bool Button(const char* label, const ImVec2& size)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float tw = static_cast<float>(CalcTextWidth(normal_font(), label, ImGui::FindRenderedTextEnd(label)));
        return button_ex(label, label, size,
                         ImVec2(tw + style.FramePadding.x * 2.f, font_size() + style.FramePadding.y * 2.f), nullptr,
                         false, nullptr);
    }

    bool ImageTextButton(const char* label, ImTextureRef image, const ImVec2& image_size, const ImVec2& size,
                         float align_x, bool tint)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const char* end = ImGui::FindRenderedTextEnd(label);
        const float tw = static_cast<float>(CalcTextWidth(normal_font(), label, end));
        const ButtonIcon icon{image, ImVec2(0, 0), ImVec2(1, 1), static_cast<int>(image_size.x),
                              static_cast<int>(image_size.y)};
        const float fit_h = std::max(font_size() + style.FramePadding.y * 2.f, image_size.y);
        return button_ex(label, label, size, ImVec2(fit_h + tw + style.FramePadding.x * 2.f, fit_h), &icon, tint,
                         nullptr, align_x);
    }

    void SetNextItemHeight(float height)
    {
        core().next_height = height;
    }

    bool IconButton(const char* str_id, Icon icon)
    {
        const ButtonIcon tex = toolkit_icon(icon);
        return button_ex(str_id, nullptr, ImVec2(16.f, 16.f), ImVec2(16.f, 16.f), &tex, true, nullptr);
    }

    bool ColorButton(const char* desc_id, const ImVec4& col, ImGuiColorEditFlags flags, const ImVec2& size)
    {
        (void)flags;
        return button_ex(desc_id, nullptr, size, ImVec2(16.f, 16.f), nullptr, false, &col);
    }

    // ---- fields -----------------------------------------------------------------------------------------------------

    namespace
    {

        // ImGuiInputTextFlags_CallbackResize on a std::string (imgui_stdlib's)
        int resize_string(ImGuiInputTextCallbackData* data)
        {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                auto* str = static_cast<std::string*>(data->UserData);
                str->resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = str->data();
            }
            return 0;
        }

        // `str`: the std::string `buf` is the data of (the resize callback may move it while editing)
        bool input_text_ex(const char* label, const char* hint, char* buf, std::size_t buf_size,
                           ImGuiInputTextFlags flags, std::string* str = nullptr)
        {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            if (window->SkipItems)
                return false;
            const float w = label_column(label);
            const ImGuiID id = window->GetID(label);
            const int h = row_height();
            ImRect bb;
            if (!add_item(id, w, static_cast<float>(h), bb))
                return false;
            ImGuiContext& g = *GImGui;
            bool hovered = false, held = false;
            bool editing = ImGui::TempInputIsActive(id);
            if (!editing) // UITextBox::begin_edit on the press
                editing = ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
            else
            {
                hovered = g.HoveredWindow == window && ImGui::IsMouseHoveringRect(bb.Min, bb.Max);
                held = g.ActiveId == id && g.IO.MouseDown[0];
            }
            ImDrawList* dl = window->DrawList;
            const int x = static_cast<int>(bb.Min.x), y = static_cast<int>(bb.Min.y), iw = static_cast<int>(w);
            if (hovered || (g.HoveredWindow == window && ImGui::IsMouseHoveringRect(bb.Min, bb.Max)))
                SetMouseCursor(Cursor::IBeam); // UITextBox::on_mouse_move
            paint_field_frame(dl, x, y, iw, h, hovered, held, editing, false);
            bool changed = false;
            if (editing && str)
            {
                changed = edit_field(id, label, bb, buf, static_cast<int>(buf_size),
                                     flags | ImGuiInputTextFlags_CallbackResize, dl, hint, resize_string, str);
                buf = str->data();
            }
            else if (editing)
                changed = edit_field(id, label, bb, buf, static_cast<int>(buf_size), flags, dl, hint);
            if (g.TempInputId != id)
            {
                if (buf[0])
                    paint_field_text(dl, x, y, iw, h, buf, TC_TEXT);
                else if (hint)
                    AddText(dl, normal_font(), x + 4, center_y(h, text_h(), y), tc(TC_INACTIVE), hint);
            }
            finish_item(id, bb, hovered);
            return changed;
        }

    } // namespace

    bool InputText(const char* label, char* buf, std::size_t buf_size, ImGuiInputTextFlags flags)
    {
        return input_text_ex(label, nullptr, buf, buf_size, flags);
    }

    bool InputTextWithHint(const char* label, const char* hint, char* buf, std::size_t buf_size,
                           ImGuiInputTextFlags flags)
    {
        return input_text_ex(label, hint, buf, buf_size, flags);
    }

    bool InputText(const char* label, std::string* str, ImGuiInputTextFlags flags)
    {
        return input_text_ex(label, nullptr, str->data(), str->capacity() + 1, flags, str);
    }

    bool InputTextWithHint(const char* label, const char* hint, std::string* str, ImGuiInputTextFlags flags)
    {
        return input_text_ex(label, hint, str->data(), str->capacity() + 1, flags, str);
    }

    namespace
    {

        // UINumberBox: the text field + the spinner square (upper half +, lower half -).  `step_value` applies a step.
        template<typename Step>
        bool number_box_ex(const char* label, ImGuiDataType type, void* v, const char* format,
                           ImGuiInputTextFlags flags, const Step& step_value)
        {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            if (window->SkipItems)
                return false;
            const float w = label_column(label);
            const ImGuiID id = window->GetID(label);
            ImRect bb;
            if (!add_item(id, w, static_cast<float>(k_row), bb))
                return false;
            ImGuiContext& g = *GImGui;
            ImDrawList* dl = window->DrawList;
            const int x = static_cast<int>(bb.Min.x), y = static_cast<int>(bb.Min.y), iw = static_cast<int>(w);
            const int h = k_row, mid = y + h / 2;
            const ImRect text_bb(bb.Min, ImVec2(bb.Max.x - static_cast<float>(h), bb.Max.y));
            const ImRect up_bb(ImVec2(bb.Max.x - static_cast<float>(h), bb.Min.y),
                               ImVec2(bb.Max.x, static_cast<float>(mid)));
            const ImRect dn_bb(ImVec2(bb.Max.x - static_cast<float>(h), static_cast<float>(mid)), bb.Max);
            const ImGuiID uid = ImHashStr("#up", 0, id), did = ImHashStr("#down", 0, id);

            // the text first (a click on the spinner ends its edit in the same frame), its frame under it
            ImDrawListSplitter splitter;
            splitter.Split(dl, 2);
            splitter.SetCurrentChannel(dl, 1);
            bool changed = false;
            bool hovered = false, held = false;
            bool editing = ImGui::TempInputIsActive(id);
            if (!editing)
                editing = ImGui::ButtonBehavior(text_bb, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
            else
                held = g.ActiveId == id && g.IO.MouseDown[0];
            if (hovered || (g.HoveredWindow == window && ImGui::IsMouseHoveringRect(text_bb.Min, text_bb.Max)))
                SetMouseCursor(Cursor::IBeam); // (the text part: the spinner keeps the arrow)
            char text[64];
            ImGui::DataTypeFormatString(text, IM_ARRAYSIZE(text), type, v, format);
            if (editing
                && edit_field(id, label, text_bb, text, IM_ARRAYSIZE(text),
                              flags
                                      | (type == ImGuiDataType_Float ? ImGuiInputTextFlags_CharsScientific
                                                                     : ImGuiInputTextFlags_CharsDecimal),
                              dl, nullptr)
                && ImGui::DataTypeApplyFromText(text, type, v, format))
                changed = true;
            editing = g.TempInputId == id;
            if (!editing)
            {
                ImGui::DataTypeFormatString(text, IM_ARRAYSIZE(text), type, v, format);
                paint_field_text(dl, x, y, iw - h, h, text, TC_TEXT);
            }

            // spinner halves: a release applies one step (ending an edit), holding repeats after k_repeat_delay
            auto spin = [&](ImGuiID sid, const ImRect& sbb, int sign, bool& hot, bool& down)
            {
                ImGui::ItemAdd(sbb, sid, nullptr, ImGuiItemFlags_NoNav);
                bool sheld = false;
                const bool released = ImGui::ButtonBehavior(sbb, sid, &hot, &sheld);
                down = sheld && hot;
                ImGuiStorage* st = window->DC.StateStorage;
                if (ImGui::IsItemActivated())
                    st->SetFloat(sid, static_cast<float>(now()) + k_repeat_delay);
                if (down && now() > static_cast<double>(st->GetFloat(sid, 0.f)))
                {
                    step_value(sign);
                    changed = true;
                    st->SetFloat(sid, static_cast<float>(now()) + k_repeat_interval);
                }
                if (released)
                {
                    step_value(sign);
                    changed = true;
                }
            };
            bool up_hot = false, up_down = false, dn_hot = false, dn_down = false;
            spin(uid, up_bb, 1, up_hot, up_down);
            spin(did, dn_bb, -1, dn_hot, dn_down);
            hovered = hovered || up_hot || dn_hot
                      || (g.HoveredWindow == window && ImGui::IsMouseHoveringRect(bb.Min, bb.Max));
            held = held || up_down || dn_down;

            const int up_slot = up_hot ? (up_down ? 5 : 4) : 3;
            const int dn_slot = dn_hot ? (dn_down ? 5 : 4) : 3;
            // the up / down halves of the 16 x 16 icon, over the text
            ImTextureData* tex = icon_texture();
            const float tw = static_cast<float>(tex->Width), th = static_cast<float>(tex->Height);
            const float u0 = static_cast<float>(static_cast<int>(Icon::UpDown) * k_icon_cell + 1) / tw;
            const float u1 = u0 + 16.f / tw;
            const float v0 = 1.f / th, vm = 9.f / th, v1 = 17.f / th;
            dl->AddImage(tex->GetTexRef(), ImVec2(static_cast<float>(x + iw - h), static_cast<float>(y)),
                         ImVec2(static_cast<float>(x + iw), static_cast<float>(mid)), ImVec2(u0, v0), ImVec2(u1, vm),
                         u32(tc(up_slot)));
            dl->AddImage(tex->GetTexRef(), ImVec2(static_cast<float>(x + iw - h), static_cast<float>(mid)),
                         ImVec2(static_cast<float>(x + iw), static_cast<float>(y + h)), ImVec2(u0, vm), ImVec2(u1, v1),
                         u32(tc(dn_slot)));
            splitter.SetCurrentChannel(dl, 0);
            paint_field_frame(dl, x, y, iw, h, hovered, held, editing, true);
            splitter.Merge(dl);
            if (changed)
                ImGui::MarkItemEdited(id);
            finish_item(id, bb, hovered);
            return changed;
        }

    } // namespace

    bool InputInt(const char* label, int* v, int step, int step_fast, ImGuiInputTextFlags flags)
    {
        return number_box_ex(label, ImGuiDataType_S32, v, "%d", flags,
                             [&](int sign)
                             {
                                 int by = GImGui->IO.KeyCtrl && step_fast > 0 ? step_fast : (step > 0 ? step : 1);
                                 by *= sign;
                                 ImGui::DataTypeApplyOp(ImGuiDataType_S32, '+', v, v, &by); // (stops at INT_MIN / MAX)
                             });
    }

    bool InputFloat(const char* label, float* v, float step, float step_fast, const char* format,
                    ImGuiInputTextFlags flags)
    {
        if (step <= 0.f) // the toolkit's: one unit of the last decimal shown (precision 2 / 3 / 4: 0.1 / 0.01 / 0.001)
        {
            int decimals = 0;
            if (const char* dot = std::strstr(format, "%."))
                decimals = std::atoi(dot + 2);
            step = std::pow(10.f, -static_cast<float>(decimals));
        }
        return number_box_ex(label, ImGuiDataType_Float, v, format, flags,
                             [&](int sign)
                             {
                                 float by = GImGui->IO.KeyCtrl && step_fast > 0.f ? step_fast : step;
                                 by *= static_cast<float>(sign);
                                 ImGui::DataTypeApplyOp(ImGuiDataType_Float, '+', v, v, &by);
                             });
    }

    namespace
    {

        // A slider (the toolkit has none): a field frame, a bar inset 3 px like the check box's inner square in
        // TC_BORDER_ALT up to the value, the value centred over it.  ImGui's SliderBehavior does the mouse, the keys and
        // the clamping; a press anywhere sets the value under the cursor and drags it.
        template<typename T>
        bool slider_ex(const char* label, ImGuiDataType type, T* v, T v_min, T v_max, const char* format,
                       ImGuiSliderFlags flags)
        {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            if (window->SkipItems)
                return false;
            const float w = label_column(label);
            const ImGuiID id = window->GetID(label);
            const int h = k_row;
            ImRect bb;
            if (!add_item(id, w, static_cast<float>(h), bb))
                return false;
            ImGuiContext& g = *GImGui;
            const bool hovered = ImGui::ItemHoverable(bb, id, g.LastItemData.ItemFlags);
            if (hovered && g.IO.MouseClicked[0])
            {
                ImGui::SetActiveID(id, window);
                ImGui::SetFocusID(id, window);
                ImGui::FocusWindow(window);
                g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
            }
            // the bar's span is the value's range (no grab to leave room for)
            const ImRect bar(bb.Min.x + 3.f, bb.Min.y + 3.f, bb.Max.x - 3.f, bb.Max.y - 3.f);
            ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 0.f);
            ImRect grab;
            const bool changed = ImGui::SliderBehavior(bar, id, type, v, &v_min, &v_max, format,
                                                       flags | ImGuiSliderFlags_NoRoundToFormat, &grab);
            ImGui::PopStyleVar();
            if (changed)
                ImGui::MarkItemEdited(id);
            const bool held = g.ActiveId == id;

            ImDrawList* dl = window->DrawList;
            const int x = static_cast<int>(bb.Min.x), y = static_cast<int>(bb.Min.y), iw = static_cast<int>(w);
            paint_field_frame(dl, x, y, iw, h, hovered, held, false, false);
            // the value's place on the slider's own scale (linear or logarithmic): where SliderBehavior put the
            // grab's centre, between the ends of its usable span (the bar less its 2 px grab padding and half a grab)
            const float half_grab = grab.GetWidth() * 0.5f;
            const float lo = bar.Min.x + 2.f + half_grab, hi = bar.Max.x - 2.f - half_grab;
            const float t = hi > lo ? std::clamp((grab.GetCenter().x - lo) / (hi - lo), 0.f, 1.f) : 0.f;
            const int bar_w = static_cast<int>(t * static_cast<float>(iw - 6) + 0.5f);
            if (bar_w > 0)
                FillRect(dl, x + 3, y + 3, x + 3 + bar_w, y + h - 3, tc(TC_BORDER_ALT));
            char text[64];
            ImGui::DataTypeFormatString(text, IM_ARRAYSIZE(text), type, v, format);
            const char* end = fit_end(normal_font(), text, nullptr, iw - 8);
            const int tw = CalcTextWidth(normal_font(), text, end);
            AddText(dl, normal_font(), x + (iw - tw) / 2, center_y(h, text_h(), y), tc(TC_TEXT), text, end);
            finish_item(id, bb, hovered);
            return changed;
        }

    } // namespace

    bool SliderFloat(const char* label, float* v, float v_min, float v_max, const char* format, ImGuiSliderFlags flags)
    {
        return slider_ex(label, ImGuiDataType_Float, v, v_min, v_max, format, flags);
    }

    bool SliderInt(const char* label, int* v, int v_min, int v_max, const char* format, ImGuiSliderFlags flags)
    {
        return slider_ex(label, ImGuiDataType_S32, v, v_min, v_max, format, flags);
    }

    bool Combo(const char* label, int* current_item, const char* const items[], int items_count,
               int popup_max_height_in_items)
    {
        int* const current = current_item;
        const int count = items_count, max_rows = popup_max_height_in_items;
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;
        const float w = label_column(label);
        const ImGuiID id = window->GetID(label);
        ImRect bb;
        if (!add_item(id, w, static_cast<float>(k_row), bb))
            return false;
        ImGuiContext& g = *GImGui;
        const int x = static_cast<int>(bb.Min.x), y = static_cast<int>(bb.Min.y), iw = static_cast<int>(w);
        const int h = k_row;
        const ImGuiID popup_id = ImHashStr("##nc_combo", 0, id);
        const bool open = ImGui::IsPopupOpen(popup_id, ImGuiPopupFlags_None);
        // UIComboBox::refresh_from_var: a value without an option shows OUT_OF_RANGE
        const char* shown = (count > 0 && *current >= 0 && *current < count) ? items[*current] : "OUT_OF_RANGE";

        // UIComboBox::on_mouse_up: the release over the box opens the list (the selected option in view)
        const int rows = (max_rows > 0 && count > max_rows) ? max_rows : count;
        ImGuiStorage* st = window->DC.StateStorage;
        const ImGuiID first_key = ImHashStr("##nc_first", 0, id);
        bool hovered = false, held = false;
        const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
        const ImRect arrow_bb(ImVec2(bb.Max.x - static_cast<float>(h), bb.Min.y), bb.Max);
        const bool arrow_hot = hovered && arrow_bb.Contains(g.IO.MousePos);
        if (pressed && !open)
        {
            st->SetInt(first_key, std::clamp(*current - rows / 2, 0, std::max(0, count - rows)));
            ImGui::OpenPopupEx(popup_id, ImGuiPopupFlags_None);
        }

        // closed: UIComboBox::draw_frame + the text + the arrow
        ImDrawList* dl = window->DrawList;
        paint_field_frame(dl, x, y, iw, h, hovered, held, false, true);
        paint_field_text(dl, x, y, iw - h, h, shown, TC_TEXT);
        AddIcon(dl, Icon::ArrowDown, x + iw - h, y, x + iw, y + h, tc(arrow_hot ? (held ? 5 : 4) : 3));

        bool changed = false;
        if (!ImGui::IsPopupOpen(popup_id, ImGuiPopupFlags_None))
        {
            finish_item(id, bb, hovered);
            return false;
        }

        // open: the box grows down over its rows (pitch h - 1), painted in a popup so it covers what is below
        const int ch = h + (h - 1) * rows + 1;
        ImGui::SetNextWindowPos(bb.Min);
        ImGui::SetNextWindowSize(ImVec2(w, static_cast<float>(ch)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.f);
        const ImGuiWindowFlags pflags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                                        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground
                                        | ImGuiWindowFlags_NoScrollWithMouse;
        const bool visible = ImGui::BeginPopupEx(popup_id, pflags);
        ImGui::PopStyleVar(2);
        if (!visible)
        {
            finish_item(id, bb, hovered);
            return false;
        }
        ImGuiWindow* popup = g.CurrentWindow;
        ImDrawList* pdl = popup->DrawList;
        int first = std::clamp(st->GetInt(first_key, 0), 0, std::max(0, count - rows));
        const ImRect all(bb.Min, ImVec2(bb.Max.x, bb.Min.y + static_cast<float>(ch)));
        const bool popup_hovered = g.HoveredWindow == popup && all.Contains(g.IO.MousePos);
        if (popup_hovered && g.IO.MouseWheel != 0.f && rows < count)
            first = std::clamp(first - static_cast<int>(g.IO.MouseWheel), 0, count - rows);
        st->SetInt(first_key, first);
        int hover_item = -1;
        const int my = static_cast<int>(std::floor(g.IO.MousePos.y));
        if (popup_hovered && count > 0 && my >= y + h && my < y + ch)
            hover_item = std::clamp(first + (my - y - h) / (h - 1), 0, count - 1);
        const bool down = popup_hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const bool arrow_over = popup_hovered && arrow_bb.Contains(g.IO.MousePos);
        if (popup_hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (hover_item >= 0)
            {
                changed = *current != hover_item;
                *current = hover_item;
            }
            ImGui::CloseCurrentPopup();
        }

        const int bottom = y + ch;
        const ImVec4& edge = tc(TC_BORDER_ALT + (down ? 1 : 0));
        FillRect(pdl, x, y, x + iw, bottom, tc(TC_BG));
        FillRect(pdl, x, y, x + iw, y + h, tc(TC_BG_ALT));
        OutlineRect(pdl, x, y, x + iw, bottom, edge);
        const int sep_y = y + h - 1;
        Line(pdl, x, sep_y, x + iw - 1, sep_y, edge);
        Line(pdl, x + iw - h, y + 1, x + iw - h, sep_y, edge);
        const int row = hover_item - first;
        if (hover_item != -1 && row >= 0 && row < rows)
        {
            const int y0 = y + h + row * (h - 1), y1 = y + h + (h - 1) * (row + 1);
            FillRect(pdl, x + 1, y0, x + iw - 1, y1, tc(TC_BG_ALT + (down ? 1 : 0)));
        }
        if (rows > 0 && rows < count) // where the shown rows are
        {
            const int top = y + h, span = bottom - 1 - top;
            const int t0 = top + span * first / count, t1 = top + span * (first + rows) / count;
            FillRect(pdl, x + iw - 4, t0, x + iw - 1, std::max(t1, t0 + 2), edge);
        }
        paint_field_text(pdl, x, y, iw - h, h, shown, TC_TEXT);
        AddIcon(pdl, Icon::ArrowDown, x + iw - h, y, x + iw, y + h, tc(arrow_over ? (down ? 5 : 4) : 3), true);
        int ly = y + h - 1;
        for (int i = first; i < std::min(count, first + rows); ++i)
        {
            const char* t = items[i];
            const char* end = fit_end(normal_font(), t, nullptr, iw - 8);
            AddText(pdl, normal_font(), x + 4, center_y(h, text_h(), ly), tc(TC_TEXT + (hover_item == i ? 2 : 0)), t,
                    end);
            ly += h - 1;
        }
        ImGui::EndPopup();
        if (changed)
            ImGui::MarkItemEdited(id);
        finish_item(id, bb, false);
        return changed;
    }

    bool KeyBind(const char* label, const char* key_name)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;
        const float w = label_column(label);
        const ImGuiID id = window->GetID(label);
        ImRect bb;
        if (!add_item(id, w, static_cast<float>(k_row), bb))
            return false;
        bool hovered = false, held = false;
        const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
        // UIKeyBind::paint
        ImDrawList* dl = window->DrawList;
        const int x = static_cast<int>(bb.Min.x), y = static_cast<int>(bb.Min.y), iw = static_cast<int>(w);
        const int h = k_row;
        const int border = state_slot(TC_BORDER, hovered, held);
        FillRect(dl, x, y, x + iw, y + h, tc(state_slot(TC_BG, hovered, held)));
        OutlineRect(dl, x, y, x + iw, y + h, tc(border));
        const int sx = x + iw - h;
        Line(dl, sx, y, sx, y + h - 1, tc(border));
        AddIcon(dl, Icon::Ellipsis, x + iw - h, y, x + iw, y + h, tc(border));
        if (key_name && *key_name)
        {
            const int cw = iw - h;
            const int tw = CalcTextWidth(normal_font(), key_name);
            AddText(dl, normal_font(), x + (cw - tw) / 2, y + (h - text_h()) / 2, tc(TC_TEXT), key_name);
        }
        finish_item(id, bb, hovered);
        return pressed;
    }

    // ---- tooltip ----------------------------------------------------------------------------------------------------

    namespace
    {

        // The end of the first line of `s` in a box max_w wide (the desktop's wrap_line): after a newline, after the
        // last space before the overflowing character, else one character before it; null: all of it.
        const char* wrap_line(ImFont* font, const char* s, const char* end, int max_w)
        {
            ImFontBaked* baked = font->GetFontBaked(font_size());
            int acc = 0;
            const char* last_space = nullptr;
            const char* prev = nullptr; // the character before the current one
            for (const char* p = s; p < end;)
            {
                unsigned int c = 0;
                const int n = std::max(ImTextCharFromUtf8(&c, p, end), 1);
                if (c == '\n')
                    return p + n;
                if (c == ' ')
                    last_space = p;
                acc += static_cast<int>(baked->GetCharAdvance(static_cast<ImWchar>(c)));
                if (acc > max_w)
                    return last_space ? last_space + 1 : prev;
                prev = p;
                p += n;
            }
            return nullptr;
        }

    } // namespace

    void SetItemTooltipV(const char* fmt, va_list args)
    {
        ImGuiContext& g = *GImGui;
        const bool ours = g.LastItemData.ID != 0 && g.LastItemData.ID == core().last_item.id;
        const bool hovered = ours ? core().last_item.hovered || (core().label_owner == core().last_item.id && core().label_hovered)
                                  : ImGui::IsItemHovered();
        if (!hovered)
            return;
        const char* text = nullptr;
        const char* text_end = nullptr;
        ImFormatStringToTempBufferV(&text, &text_end, fmt, args);
        if (!text || text == text_end)
            return;
        // UIDesktop::paint_tooltip: at the cursor (+8, +20), 256 px wide, one (tall + 1) px row per line, full alpha
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float alpha = g.Style.Alpha;
        g.Style.Alpha = 1.f;
        const int x0 = static_cast<int>(std::floor(g.IO.MousePos.x)) + 8;
        const int y0 = static_cast<int>(std::floor(g.IO.MousePos.y)) + 20;
        const int tall = text_h();
        const char* end = text_end;
        int total = 0;
        for (const char* rest = text;;)
        {
            const char* line_end = wrap_line(normal_font(), rest, end, 256);
            FillRect(dl, x0, y0 + total, x0 + 256, y0 + total + tall + 1, tc(TC_BG));
            const char* shown_end = line_end ? line_end : end;
            if (shown_end > rest && shown_end[-1] == '\n') // (the toolkit's newline glyph is blank)
                --shown_end;
            AddText(dl, normal_font(), x0 + 2, y0 + total + 1, tc(TC_TEXT_ALT), rest, shown_end);
            total += tall + 1;
            if (!line_end || line_end <= rest)
                break;
            rest = line_end;
        }
        OutlineRect(dl, x0, y0, x0 + 256, y0 + total, tc(TC_BORDER));
        g.Style.Alpha = alpha;
    }

    void SetItemTooltip(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        SetItemTooltipV(fmt, args);
        va_end(args);
    }

    // ---- containers -------------------------------------------------------------------------------------------------

    namespace
    {

        // A tab bar being submitted: its header row and the next tab's place in it.
        struct TabBarState
        {
            ImGuiWindow* window = nullptr;
            ImGuiID id = 0;
            int px = 0, py = 0, w = 0;
            int next_x = 0; // the next tab's offset in the row
            int index = 0; // the next tab's index
            int selected = 0; // this frame's selection
            int select_next = -1; // a click's, applied by EndTabBar
            ImVec2 page; // where its pages start
            bool pinned = false; // flush with the window's title bar: it stays put while the pages scroll under it
        };
        struct TabsState
        {
            ImVector<TabBarState> bars;

            void end_frame()
            {
                IM_ASSERT_USER_ERROR(bars.empty(), "BeginTabBar() without EndTabBar()");
                bars.clear();
            }
        };
        ImVector<TabBarState>& tab_bars()
        {
            return context_state<TabsState>(PART_TABS).bars;
        }

        ImGuiID selected_key(ImGuiID bar)
        {
            return ImHashStr("##nc_selected", 0, bar);
        }

    } // namespace

    bool BeginTabBar(const char* str_id, ImGuiTabBarFlags flags)
    {
        (void)flags;
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;
        TabBarState bar;
        bar.window = window;
        bar.id = window->GetID(str_id);
        // first in the window: the strip fills the client area, flush with the frame and the title bar; first on a tab
        // page: it fills the page (the toolkit's strips fill their parent)
        const bool flush = window->DC.CursorPos.x == window->DC.CursorStartPos.x
                           && window->DC.CursorPos.y == window->DC.CursorStartPos.y && !window->ParentWindow;
        const TabBarState* outer = tab_bars().empty() ? nullptr : &tab_bars().back();
        const bool on_page = outer && outer->window == window && window->DC.CursorPos.x == outer->page.x
                             && window->DC.CursorPos.y == outer->page.y;
        if (on_page)
        {
            bar.px = outer->px;
            bar.py = outer->py + k_tab_row;
            bar.w = outer->w;
            bar.pinned = outer->pinned;
            window->DC.CursorPos = ImVec2(static_cast<float>(bar.px), static_cast<float>(bar.py));
        }
        else if (flush)
        {
            bar.px = static_cast<int>(window->Pos.x) + 1;
            bar.py = static_cast<int>(window->Pos.y + window->TitleBarHeight);
            bar.w = static_cast<int>(window->Size.x) - 2;
            bar.pinned = true;
            window->DC.CursorPos = ImVec2(static_cast<float>(bar.px), static_cast<float>(bar.py));
        }
        else
        {
            bar.px = static_cast<int>(window->DC.CursorPos.x);
            bar.py = static_cast<int>(window->DC.CursorPos.y);
            bar.w = static_cast<int>(resolve_width(0.f));
        }
        bar.selected = window->DC.StateStorage->GetInt(selected_key(bar.id), 0);
        // UITabStrip::paint: the header's baseline first, the tabs over it
        ImGui::PushClipRect(window->Pos, ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y), false);
        Line(window->DrawList, bar.px, bar.py + 14, bar.px + bar.w, bar.py + 14, tc(TC_BORDER));
        ImGui::PopClipRect();
        const ImRect bb(ImVec2(static_cast<float>(bar.px), static_cast<float>(bar.py)),
                        ImVec2(static_cast<float>(bar.px + bar.w), static_cast<float>(bar.py + k_tab_row)));
        ImGui::ItemSize(bb.GetSize()); // the page starts under the row
        if (bar.pinned)
        {
            // the strip stays under the title bar, the page scrolls: it starts where the window's scroll puts it and
            // is clipped to below the strip (what scrolls up goes under it)
            window->DC.CursorPos.y -= window->Scroll.y;
            window->DC.CursorPosPrevLine.y -= window->Scroll.y;
        }
        bar.page = window->DC.CursorPos;
        ImGui::ItemAdd(bb, bar.id);
        if (bar.pinned)
            ImGui::PushClipRect(ImVec2(window->InnerClipRect.Min.x, static_cast<float>(bar.py + k_tab_row)),
                                window->InnerClipRect.Max, true);
        ImGui::PushOverrideID(bar.id);
        tab_bars().push_back(bar);
        return true;
    }

    void EndTabBar()
    {
        IM_ASSERT_USER_ERROR_RET(!tab_bars().empty(), "EndTabBar() without BeginTabBar()");
        const TabBarState bar = tab_bars().back();
        tab_bars().pop_back();
        ImGui::PopID();
        if (bar.pinned)
            ImGui::PopClipRect();
        int selected = bar.select_next >= 0 ? bar.select_next : bar.selected;
        if (selected >= bar.index) // the selected tab went away: the first one
            selected = 0;
        bar.window->DC.StateStorage->SetInt(selected_key(bar.id), selected);
    }

    bool BeginTabItem(const char* label, bool* p_open, ImGuiTabItemFlags flags)
    {
        (void)p_open; // (the toolkit's tabs have no close button)
        IM_ASSERT_USER_ERROR_RETV(!tab_bars().empty(), false, "BeginTabItem() outside BeginTabBar()");
        TabBarState& bar = tab_bars().back();
        ImGuiContext& g = *GImGui;
        ImGuiWindow* window = bar.window;
        const int i = bar.index++;
        if (flags & ImGuiTabItemFlags_SetSelected)
            bar.select_next = i;

        ImFont* font = header_font();
        const char* end = ImGui::FindRenderedTextEnd(label);
        const int hw = CalcTextWidth(font, label, end) + 8;
        const int x = bar.px + bar.next_x, py = bar.py;
        bar.next_x += hw;
        const ImGuiID id = window->GetID(label);
        const ImRect bb(ImVec2(static_cast<float>(x), static_cast<float>(py)),
                        ImVec2(static_cast<float>(x + hw), static_cast<float>(py + k_tab_row)));
        ImGui::PushClipRect(window->Pos, ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y), false);
        const ImVec2 backup = window->DC.CursorPos; // (placed in the row, not laid out)
        ImGui::ItemAdd(bb, id, nullptr, ImGuiItemFlags_NoNav);
        window->DC.CursorPos = backup;
        bool hovered = false, held = false;
        if (ImGui::ButtonBehavior(bb, id, &hovered, &held)) // UITab::on_mouse_up: the release over it selects it
            bar.select_next = i;
        if (hovered) // UITabStrip::on_mouse_move: dc_hand over a tab's label
            SetMouseCursor(Cursor::Hand);

        // UITabStrip::paint for this tab: the toolkit's "active" tab is the active widget, the one pressed
        ImDrawList* dl = window->DrawList;
        const bool selected = bar.selected == i;
        if (selected)
        {
            FillRect(dl, x, py, x + hw, py + 15, tc(TC_BORDER));
            AddText(dl, font, x + 4, py, tc(TC_TEXT_FOCUSED), label, end);
        }
        else
        {
            const int slot = held ? (hovered ? TC_BG_FOCUSED : TC_BG_ALT) : TC_BG;
            FillRect(dl, x, py, x + hw, py + 14, tc(slot));
            Line(dl, x + hw - 1, py, x + hw - 1, py + 14, tc(TC_BORDER));
            AddText(dl, font, x + 4, py, tc(TC_TEXT), label, end);
        }
        if (selected || hovered)
            Line(dl, x + 4, py + 12, x + hw - 5, py + 12, tc(selected ? TC_BG : TC_BORDER_FOCUSED));
        ImGui::PopClipRect();
        (void)g;
        finish_item(id, bb, hovered);
        return selected;
    }

    void EndTabItem()
    {
        IM_ASSERT_USER_ERROR_RET(!tab_bars().empty(), "EndTabItem() outside BeginTabBar()");
    }

    bool BeginGroupBox(const char* title, const ImVec2& size, float label_width_arg)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        auto gs = std::make_unique<GroupState>();
        gs->window = window;
        gs->title = title ? std::string(title, ImGui::FindRenderedTextEnd(title)) : std::string();
        const bool nested = !core().groups.empty() && core().groups.back()->window == window;
        const int shift = nested ? 8 : 0; // a nested group's frame lines up with its parent's rows
        gs->pos = window->DC.CursorPos;
        const int w = static_cast<int>(resolve_width(size.x)) + shift;
        gs->box_x = static_cast<int>(gs->pos.x) - shift;
        gs->box_y = static_cast<int>(gs->pos.y);
        gs->w = w;
        gs->fixed_h = size.y > 0.f ? static_cast<int>(size.y) : 0;
        // the client area is (9, 9) in, (w - 10) wide; the rows 2 px further in
        gs->content = ImVec2(static_cast<float>(gs->box_x + 11), static_cast<float>(gs->box_y + 11));

        gs->backup_indent = ImVec2(window->DC.Indent.x, 0.f);
        gs->backup_curr_line_size = window->DC.CurrLineSize;
        gs->backup_curr_line_base = window->DC.CurrLineTextBaseOffset;
        gs->backup_prev_line_pos = window->DC.CursorPosPrevLine;
        gs->backup_prev_line_size = window->DC.PrevLineSize;
        gs->backup_prev_line_base = window->DC.PrevLineTextBaseOffset;
        gs->backup_cursor_max = window->DC.CursorMaxPos;
        gs->backup_same_line = window->DC.IsSameLine;
        gs->backup_work_max_x = window->WorkRect.Max.x;
        gs->backup_content_max_x = window->ContentRegionRect.Max.x;

        window->DC.Indent.x = gs->content.x - window->Pos.x - window->DC.ColumnsOffset.x;
        window->DC.CursorPos = gs->content;
        window->DC.CursorMaxPos = gs->content;
        window->DC.CurrLineSize = ImVec2(0, 0);
        window->DC.CurrLineTextBaseOffset = 0.f;
        window->DC.IsSameLine = false;
        const float right = static_cast<float>(gs->box_x + w - 3);
        window->WorkRect.Max.x = right;
        window->ContentRegionRect.Max.x = right;
        gs->backup_work_max_y = window->WorkRect.Max.y;
        gs->backup_content_max_y = window->ContentRegionRect.Max.y;
        if (gs->fixed_h > 0) // the rows' room ends 3 px above the box's bottom (the frame + the padding)
        {
            const float bottom = static_cast<float>(gs->box_y + gs->fixed_h - 3);
            window->WorkRect.Max.y = bottom;
            window->ContentRegionRect.Max.y = bottom;
        }

        // the frame goes under the rows: channel 0, filled in at the end when the height is known
        gs->splitter.Split(window->DrawList, 2);
        gs->splitter.SetCurrentChannel(window->DrawList, 1);
        PushLabelWidth(label_width_arg);
        ImGui::PushItemWidth(-FLT_MIN); // the rows fill the box
        ImGui::PushID(title ? title : "##group"); // the rows' ids are the group's: "Mode:" in two groups is two ids
        core().groups.push_back(std::move(gs));
        return true;
    }

    void EndGroupBox()
    {
        IM_ASSERT_USER_ERROR_RET(!core().groups.empty(), "EndGroupBox() without BeginGroupBox()");
        std::unique_ptr<GroupState> gs = std::move(core().groups.back());
        core().groups.pop_back();
        ImGui::PopID();
        ImGui::PopItemWidth();
        PopLabelWidth();
        ImGuiWindow* window = gs->window;
        ImDrawList* dl = window->DrawList;
        const float rows_h = std::max(0.f, window->DC.CursorMaxPos.y - gs->content.y);
        const int h =
                gs->fixed_h > 0 ? gs->fixed_h : static_cast<int>(rows_h) + 14; // gap + rows + gap + the frame's 10

        window->DC.Indent.x = gs->backup_indent.x;
        window->DC.CurrLineSize = gs->backup_curr_line_size;
        window->DC.CurrLineTextBaseOffset = gs->backup_curr_line_base;
        window->DC.CursorPosPrevLine = gs->backup_prev_line_pos;
        window->DC.PrevLineSize = gs->backup_prev_line_size;
        window->DC.PrevLineTextBaseOffset = gs->backup_prev_line_base;
        window->DC.CursorMaxPos = ImMax(gs->backup_cursor_max, window->DC.CursorMaxPos);
        window->DC.IsSameLine = gs->backup_same_line;
        window->WorkRect.Max.x = gs->backup_work_max_x;
        window->ContentRegionRect.Max.x = gs->backup_content_max_x;
        window->WorkRect.Max.y = gs->backup_work_max_y;
        window->ContentRegionRect.Max.y = gs->backup_content_max_y;

        // UIGroupBox::draw_frame under the rows, the title over them
        const int x = gs->box_x, y = gs->box_y, w = gs->w;
        gs->splitter.SetCurrentChannel(dl, 0);
        OutlineRect(dl, x + 8, y + 8, x + w, y + h, tc(TC_BORDER));
        const int tw = CalcTextWidth(header_font(), gs->title.c_str());
        const int th = text_h();
        const int half = (w - tw) / 2;
        FillRect(dl, half + x + 6, y, half + tw + x + 10, y + th, tc(TC_BG));
        gs->splitter.Merge(dl);
        if (!gs->title.empty())
            AddText(dl, header_font(), x + 8 + (w - tw) / 2, y - 1, tc(TC_TEXT), gs->title.c_str());

        window->DC.CursorPos = gs->pos;
        const ImRect bb(gs->pos, ImVec2(static_cast<float>(x + w), static_cast<float>(y + h)));
        ImGui::ItemSize(bb.GetSize());
        ImGui::ItemAdd(bb, 0);
    }

} // namespace imgui_nullcore
