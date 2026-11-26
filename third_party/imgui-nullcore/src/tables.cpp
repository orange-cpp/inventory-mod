// UIListBoxEx (libs/nclib/source/ui/widgets/widgets_input.cpp) on imgui_nullcore, behind ImGui's table calls: the
// header row, the cells painted one after the other (each over the text that ran out of the one before), the border
// lines, the column edge drag, nullcore's scroll bar under the cells.

#include "internal.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace imgui_nullcore
{

    namespace
    {

        using namespace detail;

        constexpr int k_default_column = 64; // (UIGridModel's fallback width)

        struct Column
        {
            std::string label;
            int* width = nullptr;
            int min_w = 0, max_w = 0x7FFF;
        };

        struct Table
        {
            ImGuiWindow* window = nullptr;
            ImGuiID id = 0;
            int x = 0, y = 0, w = 0, h = 0;
            int row_h = k_row;
            int columns = 0;
            std::vector<Column> cols;
            std::vector<int> own_widths; // TableSetupColumn without a width pointer (kept in the window's storage)
            int prev_rows = 0; // the previous frame's row count (the scroll range)
            float scroll = 0.f; // px
            bool hovered = false, held = false, pressed = false;
            bool resizing = false;
            int resize_col = -1; // the column whose right edge is hot / dragged
            int hover_col = -1, hover_row = -2;
            bool input_done = false;
            // the row and cell being built
            int row = -1, col = -1, ry = 0, cx = 0;
            bool cell_open = false, clip_pushed = false;
            ImVec4 cell_bg;
            std::string cell_text;
        };
        struct TablesState
        {
            std::vector<Table> tables;

            void end_frame()
            {
                IM_ASSERT_USER_ERROR(tables.empty(), "BeginTable() without EndTable()");
                tables.clear();
            }
        };
        std::vector<Table>& tables()
        {
            return context_state<TablesState>(PART_TABLES).tables;
        }

        ImGuiID key(const Table& t, const char* what)
        {
            return ImHashStr(what, 0, t.id);
        }

        ImGuiID width_key(const Table& t, int column)
        {
            return ImHashStr("##nc_w", 0, t.id + static_cast<ImGuiID>(column));
        }

        int column_width(const Table& t, int c)
        {
            return c < static_cast<int>(t.cols.size()) && t.cols[static_cast<std::size_t>(c)].width
                           ? *t.cols[static_cast<std::size_t>(c)].width
                           : k_default_column;
        }

        int content_height(const Table& t)
        {
            return (t.row_h - 1) * (t.prev_rows + 1);
        }

        // UIGridModel::resize_column of the Player Manager: the column takes `width` (in its range), the last one the
        // difference (in its range), their sum kept
        void resize_column(Table& t, int c, int width)
        {
            const int n = static_cast<int>(t.cols.size());
            if (c < 0 || c >= n - 1)
                return;
            Column& col = t.cols[static_cast<std::size_t>(c)];
            Column& last = t.cols[static_cast<std::size_t>(n - 1)];
            const int sum = *col.width + *last.width;
            const int new_c = std::clamp(width, col.min_w, col.max_w);
            const int new_last = std::clamp(sum - new_c, last.min_w, last.max_w);
            *col.width = sum - new_last;
            *last.width = new_last;
        }

        // UIListBoxEx::on_mouse_move / on_mouse_down: the hovered cell and the column edge, or the edge being dragged
        void update_input(Table& t)
        {
            if (t.input_done)
                return;
            t.input_done = true;
            while (static_cast<int>(t.cols.size()) < t.columns) // (columns nobody set up)
                t.cols.push_back(Column{});
            ImGuiContext& g = *GImGui;
            const int mx = static_cast<int>(std::floor(g.IO.MousePos.x)),
                      my = static_cast<int>(std::floor(g.IO.MousePos.y));
            int rel_x = std::clamp(mx - t.x, 0, t.w - 1);
            if (t.resizing && t.held)
            {
                for (int i = 0; i < t.resize_col; ++i)
                    rel_x -= column_width(t, i);
                resize_column(t, t.resize_col, rel_x);
                SetMouseCursor(Cursor::SizeWE);
                return;
            }
            t.resizing = false;
            t.resize_col = -1;
            ImGuiStorage* st = t.window->DC.StateStorage;
            if (!t.hovered)
            {
                // under a context menu the grid keeps its hover (the toolkit's list gets no mouse moves then)
                if (is_menu_window(g.HoveredWindow))
                {
                    t.hover_col = st->GetInt(key(t, "##nc_hcol"), -1);
                    t.hover_row = st->GetInt(key(t, "##nc_hrow"), -2);
                }
                st->SetInt(key(t, "##nc_hcol"), t.hover_col);
                st->SetInt(key(t, "##nc_hrow"), t.hover_row);
                return;
            }
            const int content = content_height(t);
            const int yy = std::clamp(my - t.y - 1, 0, std::max(content, 0));
            const int abs_y = yy + static_cast<int>(t.scroll);
            int cw = 0;
            for (int c = 0; c < t.columns; ++c)
            {
                cw = column_width(t, c);
                if (rel_x < cw)
                {
                    t.hover_col = c;
                    break;
                }
                rel_x -= cw;
            }
            struct Keep // (the hover kept for a context menu over the grid)
            {
                Table& t;
                ImGuiStorage* st;
                ~Keep()
                {
                    st->SetInt(key(t, "##nc_hcol"), t.hover_col);
                    st->SetInt(key(t, "##nc_hrow"), t.hover_row);
                }
            } keep{t, st};
            if (t.hover_col == -1)
                return;
            // (the rows are counted as they come: TableGetHoveredRow drops a row past them)
            t.hover_row = abs_y / (t.row_h - 1) - 1;
            if (t.hover_row < -1)
                t.hover_row = -2;
            if (t.hover_row == -1 && cw - rel_x <= 2)
            {
                t.resize_col = t.hover_col;
                SetMouseCursor(Cursor::SizeWE);
                if (t.pressed)
                {
                    t.resizing = true;
                    t.hover_col = -1;
                    t.hover_row = -2;
                }
            }
        }

        // a cell: its fill, its text, the border line on its right
        void finish_cell(Table& t)
        {
            if (!t.cell_open)
                return;
            t.cell_open = false;
            ImDrawList* dl = t.window->DrawList;
            const int cw = column_width(t, t.col);
            const bool hot = t.hover_row == t.row && t.hover_col == t.col;
            FillRect(dl, t.cx, t.ry, t.cx + cw, t.ry + t.row_h - 2, t.cell_bg);
            if (!t.cell_text.empty())
                AddText(dl, normal_font(), t.cx + 4, t.ry, tc(TC_TEXT + (hot ? 2 : 0)), t.cell_text.c_str(),
                        t.cell_text.c_str() + t.cell_text.size());
            Line(dl, t.cx - 1 + cw, t.ry, t.cx - 1 + cw, t.ry + t.row_h - 1, tc(TC_BORDER));
            t.cx += cw;
        }

        void start_cell(Table& t)
        {
            const bool hot = t.hover_row == t.row && t.hover_col == t.col;
            t.cell_open = true;
            t.cell_bg = tc(hot ? TC_BG_ALT : TC_BG);
            t.cell_text.clear();
        }

        // the row's missing cells, then the line under it
        void finish_row(Table& t)
        {
            if (t.row < 0)
                return;
            finish_cell(t);
            while (t.col < t.columns - 1)
            {
                ++t.col;
                start_cell(t);
                finish_cell(t);
            }
            Line(t.window->DrawList, t.x, t.ry - 2 + t.row_h, t.cx, t.ry - 2 + t.row_h, tc(TC_BORDER));
        }

        Table* current()
        {
            return tables().empty() ? nullptr : &tables().back();
        }

    } // namespace

    namespace detail
    {

        bool table_cell_text(const char* text, const char* end)
        {
            Table* t = current();
            if (!t || !t->cell_open || t->window != GImGui->CurrentWindow)
                return false;
            t->cell_text.append(text, end ? end : text + std::strlen(text));
            return true;
        }

    } // namespace detail

    bool BeginTable(const char* str_id, int columns, ImGuiTableFlags flags, const ImVec2& outer_size, float inner_width)
    {
        (void)flags;
        (void)inner_width;
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems || columns <= 0)
            return false;
        Table t;
        t.window = window;
        t.id = window->GetID(str_id);
        t.columns = columns;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float w = outer_size.x > 0.f ? outer_size.x : std::max(1.f, avail.x + outer_size.x);
        const float h = outer_size.y > 0.f ? outer_size.y : std::max(1.f, avail.y + outer_size.y);
        ImRect bb;
        if (!add_item(t.id, std::floor(w), std::floor(h), bb))
            return false;
        t.x = static_cast<int>(bb.Min.x);
        t.y = static_cast<int>(bb.Min.y);
        t.w = static_cast<int>(bb.GetWidth());
        t.h = static_cast<int>(bb.GetHeight());
        ImGuiStorage* st = window->DC.StateStorage;
        t.prev_rows = st->GetInt(key(t, "##nc_rows"), 0);
        t.resizing = st->GetBool(key(t, "##nc_resizing"), false);
        t.resize_col = st->GetInt(key(t, "##nc_resize_col"), -1);
        t.cols.reserve(static_cast<std::size_t>(columns));
        t.own_widths.reserve(static_cast<std::size_t>(columns)); // (the columns point into it)

        // the rows take the mouse; the scroll bar its 16 px (from w - 15)
        const ImRect rows_bb(bb.Min, ImVec2(bb.Min.x + static_cast<float>(t.w - 15), bb.Max.y));
        t.pressed = ImGui::ButtonBehavior(rows_bb, t.id, &t.hovered, &t.held, ImGuiButtonFlags_PressedOnClick);
        if (t.pressed && !GImGui->IO.MouseClicked[ImGuiMouseButton_Left])
            t.pressed = false;
        // (ImGui blocks the windows behind a focused popup: the toolkit's list still follows the mouse under its menu)
        if (!t.hovered && GImGui->HoveredWindow == window && is_menu_window(GImGui->NavWindow)
            && rows_bb.Contains(GImGui->IO.MousePos))
            t.hovered = true;

        // UIListBoxEx::paint: the scroll bar (a child: painted first), the visible fraction from the content height
        float pos = st->GetFloat(key(t, "##nc_pos"), 0.f);
        const int content = content_height(t);
        const float frac =
                content != 0 ? std::min(1.f, static_cast<float>(t.h + 1) / static_cast<float>(content)) : 1.f;
        if (t.hovered && GImGui->IO.MouseWheel != 0.f && frac != 1.f)
            pos = std::clamp(pos - GImGui->IO.MouseWheel * frac, 0.f, 1.f);
        scrollbar(key(t, "##nc_bar"), t.x + t.w - 15, t.y - 1, 16, t.h + 2, &pos, frac);
        st->SetFloat(key(t, "##nc_pos"), pos);
        const float scroll = pos * static_cast<float>(content - t.h - 1);
        t.scroll = scroll > 0.f ? scroll : 0.f;
        ImGui::PushID(static_cast<int>(t.id));
        tables().push_back(std::move(t));
        return true;
    }

    void TableSetupColumn(const char* label, int* width, int min_width, int max_width)
    {
        Table* t = current();
        IM_ASSERT_USER_ERROR_RET(t, "TableSetupColumn() outside BeginTable()");
        if (!t)
            return;
        if (static_cast<int>(t->cols.size()) >= t->columns)
            return;
        Column c;
        c.label = label ? std::string(label, ImGui::FindRenderedTextEnd(label)) : std::string();
        c.width = width;
        c.min_w = min_width;
        c.max_w = max_width;
        t->cols.push_back(std::move(c));
    }

    void TableSetupColumn(const char* label, ImGuiTableColumnFlags flags, float init_width, ImGuiID user_id)
    {
        (void)flags;
        (void)user_id;
        Table* t = current();
        IM_ASSERT_USER_ERROR_RET(t, "TableSetupColumn() outside BeginTable()");
        if (!t)
            return;
        if (static_cast<int>(t->cols.size()) >= t->columns)
            return;
        const int init = init_width > 0.f ? static_cast<int>(init_width) : k_default_column;
        t->own_widths.push_back(
                t->window->DC.StateStorage->GetInt(width_key(*t, static_cast<int>(t->cols.size())), init));
        TableSetupColumn(label, &t->own_widths.back(), 0, 0x7FFF);
    }

    // the header row: each column's fill (TC_BG_ALT when the column is hovered), bold text, the line on its right, then
    // the line under the row
    void TableHeadersRow()
    {
        Table* t = current();
        IM_ASSERT_USER_ERROR_RET(t, "TableHeadersRow() outside BeginTable()");
        if (!t)
            return;
        update_input(*t);
        ImDrawList* dl = t->window->DrawList;
        int cx = t->x;
        for (int c = 0; c < t->columns; ++c)
        {
            const int cw = column_width(*t, c);
            const bool hot = t->hover_col == c; // (the header of the hovered column, whatever the row)
            FillRect(dl, cx, t->y, cx + cw, t->y + t->row_h - 2, tc(hot ? TC_BG_ALT : TC_BG));
            const std::string& label = t->cols[static_cast<std::size_t>(c)].label;
            if (!label.empty())
                AddText(dl, header_font(), cx + 4, t->y, tc(TC_TEXT + (hot ? 2 : 0)), label.c_str(),
                        label.c_str() + label.size());
            Line(dl, cx - 1 + cw, t->y, cx - 1 + cw, t->y + t->row_h - 1, tc(TC_BORDER));
            cx += cw;
        }
        Line(dl, t->x, t->y - 2 + t->row_h, cx, t->y - 2 + t->row_h, tc(TC_BORDER));
    }

    void TableNextRow(ImGuiTableRowFlags row_flags, float min_row_height)
    {
        (void)row_flags;
        (void)min_row_height;
        Table* t = current();
        IM_ASSERT_USER_ERROR_RET(t, "TableNextRow() outside BeginTable()");
        if (!t)
            return;
        update_input(*t);
        if (t->row < 0)
        {
            // the rows are cut below the header row
            ImGui::PushClipRect(ImVec2(static_cast<float>(t->x), static_cast<float>(t->y + t->row_h - 1)),
                                ImVec2(static_cast<float>(t->x + t->w), static_cast<float>(t->y + t->h)), true);
            t->clip_pushed = true;
            t->ry = t->y - static_cast<int>(t->scroll) + t->row_h - 1;
        }
        else
        {
            finish_row(*t);
            t->ry += t->row_h - 1;
        }
        ++t->row;
        t->col = -1;
        t->cx = t->x;
    }

    bool TableNextColumn()
    {
        Table* t = current();
        IM_ASSERT_USER_ERROR_RETV(t, false, "TableNextColumn() outside BeginTable()");
        if (!t)
            return false;
        if (t->row < 0 || t->col >= t->columns - 1)
            TableNextRow();
        finish_cell(*t);
        ++t->col;
        start_cell(*t);
        return t->ry + t->row_h - 1 > t->y + t->row_h - 1 && t->ry < t->y + t->h;
    }

    void TableSetBgColor(ImGuiTableBgTarget target, ImU32 color, int column_n)
    {
        Table* t = current();
        IM_ASSERT_USER_ERROR_RET(t, "TableSetBgColor() outside BeginTable()");
        if (!t)
            return;
        if (target == ImGuiTableBgTarget_CellBg && t->cell_open && (column_n == -1 || column_n == t->col))
            t->cell_bg = ImGui::ColorConvertU32ToFloat4(color);
    }

    int TableGetHoveredColumn()
    {
        Table* t = current();
        if (!t)
            return -1;
        update_input(*t);
        return t->hover_col;
    }

    int TableGetHoveredRow()
    {
        Table* t = current();
        if (!t)
            return -2;
        update_input(*t);
        if (t->resize_col != -1 || t->hover_row > t->row) // (past the rows given so far)
            return -2;
        return t->hover_row;
    }

    void EndTable()
    {
        Table* t = current();
        IM_ASSERT_USER_ERROR_RET(t, "EndTable() without BeginTable()");
        if (!t)
            return;
        update_input(*t);
        finish_row(*t);
        if (t->clip_pushed)
            ImGui::PopClipRect();
        ImGuiStorage* st = t->window->DC.StateStorage;
        st->SetInt(key(*t, "##nc_rows"), t->row + 1);
        st->SetBool(key(*t, "##nc_resizing"), t->resizing && t->held);
        st->SetInt(key(*t, "##nc_resize_col"), t->resize_col);
        for (std::size_t i = 0, own = 0; i < t->cols.size(); ++i) // the widths the table keeps
            if (own < t->own_widths.size() && t->cols[i].width == &t->own_widths[own])
                st->SetInt(width_key(*t, static_cast<int>(i)), t->own_widths[own++]);
        ImGui::PopID();
        tables().pop_back();
    }

} // namespace imgui_nullcore
