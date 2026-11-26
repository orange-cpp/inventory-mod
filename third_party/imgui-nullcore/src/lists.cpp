// UIListBox (libs/nclib/source/ui/widgets/widgets_input.cpp) on imgui_nullcore: rows 2 px apart in an outline with
// nullcore's scroll bar, the selection on the press, the hovered row's delete icon, the selected row dragged to
// reorder; and UIStringListEditor, the "<+ New Line>" box over such a list.

#include "internal.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace imgui_nullcore
{

    namespace
    {

        using namespace detail;

        struct ListState
        {
            float pos = 0.f; // the scroll position (0..1)
            bool reorder = false; // dragging the selected row
            bool remove_pending = false; // pressed on the delete icon
            bool no_hover =
                    false; // a drag ended: no hovered row until the mouse moves (the toolkit's hover follows moves)
            int drag_dy = 0; // where in its row the dragged row was taken
        };

        // the current context's string list editors' lines (PART_LISTS)
        struct ListsState
        {
            std::unordered_map<ImGuiID, std::string> lines;
        };

        ImGuiStorage* storage()
        {
            return GImGui->CurrentWindow->DC.StateStorage;
        }

    } // namespace

    bool ListBoxEx(const char* label, int* current_item, const char* (*getter)(void* user_data, int idx),
                   ImU32 (*color_getter)(void* user_data, int idx), void* user_data, int items_count, float height,
                   NcListBoxFlags flags, NcListBoxEvent* event)
    {
        if (event)
            *event = NcListBoxEvent{};
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;
        ImGuiContext& g = *GImGui;
        const int count = items_count;
        const int row_h = k_row, pitch = row_h + 2;
        const float w = label_column(label);
        const ImGuiID id = window->GetID(label);
        const float h = height > 0.f ? std::floor(height) : std::floor(ImGui::GetContentRegionAvail().y + height);
        ImRect bb;
        if (!add_item(id, w, h, bb))
            return false;
        const int x = static_cast<int>(bb.Min.x), y = static_cast<int>(bb.Min.y);
        const int iw = static_cast<int>(w), ih = static_cast<int>(h);
        const bool can_remove = (flags & NcListBoxFlags_Removable) != 0;
        const bool can_reorder = (flags & NcListBoxFlags_Reorderable) != 0;

        ImGuiStorage* st = storage();
        const ImGuiID pos_key = ImHashStr("##nc_pos", 0, id), flags_key = ImHashStr("##nc_drag", 0, id);
        ListState ls;
        ls.pos = st->GetFloat(pos_key, 0.f);
        const int packed = st->GetInt(flags_key, 0);
        ls.reorder = (packed & 1) != 0;
        ls.remove_pending = (packed & 2) != 0;
        ls.no_hover = (packed & 4) != 0 && g.IO.MouseDelta.x == 0.f && g.IO.MouseDelta.y == 0.f;
        ls.drag_dy = packed >> 3;

        const float frac =
                count > 0 ? std::min(1.f, static_cast<float>(ih - 4) / static_cast<float>(pitch * count)) : 1.f;
        const int visible = std::min(count, (ih - 4) / pitch);
        auto first_row = [&]
        {
            const float f = ls.pos * static_cast<float>(count - visible);
            return f > 0.f ? static_cast<int>(f) : 0;
        };

        const ImRect rows_bb(bb.Min, ImVec2(bb.Max.x - 16.f, bb.Max.y));
        bool hovered = false, held = false;
        const bool pressed = ImGui::ButtonBehavior(
                rows_bb, id, &hovered, &held, ImGuiButtonFlags_PressedOnClickRelease | ImGuiButtonFlags_PressedOnClick);
        if (hovered && g.IO.MouseWheel != 0.f)
            ls.pos = std::clamp(ls.pos - g.IO.MouseWheel * frac, 0.f, 1.f);

        // UIListBox::on_mouse_move: the row under the cursor, the delete icon's column, the dragged row's place
        const int mx = static_cast<int>(std::floor(g.IO.MousePos.x)),
                  my = static_cast<int>(std::floor(g.IO.MousePos.y));
        const int total = count * pitch;
        const int rel_y = std::clamp(my - y - 2, 0, std::max(total, 0));
        const int first = first_row();
        const int end = first + visible;
        int hover_row = -1;
        bool over_remove = false;
        bool changed = false;
        if (ls.reorder && held)
        {
            const int idx = std::clamp(first + (rel_y + pitch / 2 - ls.drag_dy) / pitch, first, end - 1);
            if (*current_item != -1 && idx != *current_item && event)
            {
                event->swap_a = *current_item;
                event->swap_b = idx;
                *current_item = idx;
                changed = true;
            }
            hover_row = -1; // (UIListBox::on_mouse_move while reordering)
        }
        else if (hovered && !ls.reorder && !ls.no_hover)
        {
            hover_row = rel_y / pitch + first;
            if (hover_row < first || hover_row > end - 1)
                hover_row = -1;
            over_remove = can_remove && mx - x >= iw - 34;
            if (can_reorder && hover_row != -1 && !over_remove) // (the toolkit's code 11: vgui's dc_sizens)
                SetMouseCursor(Cursor::SizeNS);
        }

        if (event && hover_row != -1 && g.IO.MouseClicked[ImGuiMouseButton_Right]) // UIListBox::on_right_click
            event->right_clicked = hover_row;

        const bool press = pressed && g.IO.MouseClicked[0];
        const bool release = pressed && !press;
        if (press) // on_mouse_down
        {
            if (*current_item != hover_row)
            {
                *current_item = hover_row;
                changed = true;
            }
            if (hover_row != -1)
            {
                if (over_remove)
                    ls.remove_pending = true;
                else if (can_reorder)
                {
                    ls.reorder = true;
                    ls.drag_dy = (my - y - 2) % pitch;
                }
            }
        }
        if (!held || release) // on_mouse_up
        {
            if (release && hover_row != -1 && ls.remove_pending && over_remove && event)
            {
                event->removed = hover_row;
                *current_item = -1;
                changed = true;
                hover_row = -1;
            }
            if (ls.reorder)
                ls.no_hover = true;
            ls.reorder = false;
            ls.remove_pending = false;
        }
        if (!over_remove)
            ls.remove_pending = false;

        // UIListBox::paint: the outline, the scroll bar, the rows
        ImDrawList* dl = window->DrawList;
        OutlineRect(dl, x, y, x + iw, y + ih, tc(TC_BORDER));
        scrollbar(ImHashStr("##nc_bar", 0, id), x + iw - 16, y, 16, ih, &ls.pos, frac);
        const int first_painted = first_row();
        int seq_y = y + 3;
        for (int i = first_painted; i < first_painted + visible && i < count; ++i)
        {
            const bool hover = hover_row == i;
            const bool sel = *current_item == i;
            int row_y = seq_y;
            if (sel && ls.reorder)
            {
                const int lo = y + 3, hi = ih - row_h + y - 3;
                row_y = hi >= lo ? std::clamp(my - ls.drag_dy, lo, hi) : hi;
            }
            const int x0 = x + 3, x1 = x + iw - 18;
            const ImVec4 fill = color_getter ? ImGui::ColorConvertU32ToFloat4(color_getter(user_data, i)) : tc(TC_BG);
            FillRect(dl, x0, row_y, x1, row_y + row_h, fill);
            OutlineRect(dl, x0, row_y, x1, row_y + row_h, tc(hover ? 4 : 3 + 2 * (sel ? 1 : 0)));
            const char* text = getter(user_data, i);
            const char* fit = fit_end(normal_font(), text, nullptr, iw - 30);
            AddText(dl, normal_font(), x + 7, center_y(row_h, text_h(), row_y), tc(TC_TEXT + (hover ? 2 : 0)), text,
                    fit);
            if (hover && can_remove)
                AddIcon(dl, Icon::Cross, x1 - 16, row_y, x1, row_y + 16,
                        tc(over_remove ? (ls.remove_pending ? 5 : 4) : 3));
            seq_y += pitch;
        }

        st->SetFloat(pos_key, ls.pos);
        st->SetInt(flags_key,
                   (ls.reorder ? 1 : 0) | (ls.remove_pending ? 2 : 0) | (ls.no_hover ? 4 : 0) | (ls.drag_dy << 3));
        if (changed)
            ImGui::MarkItemEdited(id);
        finish_item(id, bb, hovered);
        return changed;
    }

    bool ListBox(const char* label, int* current_item, const char* const items[], int items_count, int height_in_items)
    {
        const int rows = height_in_items < 0 ? std::min(items_count, 7) : height_in_items;
        return ListBoxEx(
                label, current_item,
                [](void* data, int idx)
                {
                    return static_cast<const char* const*>(data)[idx];
                },
                nullptr, const_cast<void*>(static_cast<const void*>(items)), items_count,
                static_cast<float>(rows * (k_row + 2) + 4));
    }

    bool StringListEditor(const char* str_id, std::vector<std::string>* items, const ImVec2& size, const char* hint)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;
        ImGui::PushID(str_id);
        ImGuiStorage* st = storage();
        const ImGuiID sel_key = ImGui::GetID("##sel");
        int sel = st->GetInt(sel_key, -1);
        if (sel >= static_cast<int>(items->size()))
            sel = -1;
        // each editor's own line: two editors on the screen show and commit their own text
        std::string& line = context_state<ListsState>(PART_LISTS).lines[ImGui::GetID("##buf")];
        bool changed = false;
        const float w = size.x > 0.f ? size.x : resolve_width(size.x);
        // the editor is a plain Widget: its frame (BG, border) under the box and the list shows in the gap between them
        {
            const ImVec2 at = window->DC.CursorPos;
            const float h = size.y > 0.f ? size.y : std::floor(ImGui::GetContentRegionAvail().y + size.y);
            const int x0 = static_cast<int>(at.x), y0 = static_cast<int>(at.y);
            FillRect(window->DrawList, x0, y0, x0 + static_cast<int>(w), y0 + static_cast<int>(h), tc(TC_BG));
            OutlineRect(window->DrawList, x0, y0, x0 + static_cast<int>(w), y0 + static_cast<int>(h), tc(TC_BORDER));
        }
        // the box over the list, both edge to edge, no padding
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.f));
        ImGui::SetNextItemWidth(w);
        if (InputTextWithHint("##line", hint ? hint : "<+ New Line>", &line, ImGuiInputTextFlags_EnterReturnsTrue)
            && !line.empty())
        {
            // Enter: append the text, or replace the selected row
            if (sel == -1)
                items->push_back(line);
            else
            {
                (*items)[static_cast<std::size_t>(sel)] = line;
                sel = -1;
            }
            line.clear();
            changed = true;
        }
        const float list_h = size.y > 0.f ? size.y - 18.f : size.y;
        ImGui::SetNextItemWidth(w);
        NcListBoxEvent ev;
        const int before = sel;
        ListBoxEx(
                "##list", &sel,
                [](void* data, int idx)
                {
                    return (*static_cast<std::vector<std::string>*>(data))[static_cast<std::size_t>(idx)].c_str();
                },
                nullptr, items, static_cast<int>(items->size()), list_h,
                NcListBoxFlags_Removable | NcListBoxFlags_Reorderable, &ev);
        ImGui::PopStyleVar();
        if (ev.swap_a >= 0)
        {
            std::swap((*items)[static_cast<std::size_t>(ev.swap_a)], (*items)[static_cast<std::size_t>(ev.swap_b)]);
            changed = true;
        }
        if (ev.removed >= 0)
        {
            items->erase(items->begin() + ev.removed);
            changed = true;
        }
        if (sel != before && ev.swap_a < 0) // a row picked: its text into the box (none: the box empties)
        {
            line = sel >= 0 ? (*items)[static_cast<std::size_t>(sel)] : std::string();
        }
        st->SetInt(sel_key, sel);
        ImGui::PopID();
        return changed;
    }

} // namespace imgui_nullcore
