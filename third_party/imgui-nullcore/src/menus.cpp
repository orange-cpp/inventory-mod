// UIContextMenu (libs/nclib/source/ui/desktop/task_bar.cpp) on imgui_nullcore: an ImGui popup per menu level, sized
// by the toolkit's rule (the widest row, the icon and arrow columns), painted once its rows are known, the submenu of
// the hovered row opened at the menu's right edge.

#include "internal.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace imgui_nullcore
{

    namespace
    {

        using namespace detail;

        constexpr int k_menu_row = 16;

        struct MenuRow
        {
            std::string text;
            Icon icon = Icon::None;
            bool sub = false;
        };

        // what a menu keeps from frame to frame
        struct MenuMemory
        {
            int w = 0, h = 0; // the size of its rows (the window's size)
            int open_sub = -1; // the row whose submenu is open
            std::vector<bool> subs; // which rows open a submenu
        };

        struct Menu
        {
            ImGuiID id = 0;
            ImGuiWindow* window = nullptr;
            int level = 0; // in the popup stack
            int root_level = 0; // the top menu's
            std::vector<MenuRow> rows;
            int w = 0; // this frame's width, the toolkit's resize() row by row
            bool has_icons = false, has_sub = false;
            int hover = -1;
            bool clicked = false; // a press on a row this frame
        };
        // the current context's menus (PART_MENUS)
        struct MenusState
        {
            std::unordered_map<ImGuiID, MenuMemory> memory;
            ImVector<ImGuiID> windows; // the menus' windows (they unfocus every other window)
            std::vector<Menu> menus;

            void end_frame()
            {
                IM_ASSERT_USER_ERROR(menus.empty(), "BeginPopup() / BeginMenu() without EndPopup() / EndMenu()");
                menus.clear();
            }
        };
        MenusState& menus_state()
        {
            return context_state<MenusState>(PART_MENUS);
        }

        int popup_level(ImGuiID id)
        {
            const ImGuiContext& g = *GImGui;
            for (int i = 0; i < g.OpenPopupStack.Size; ++i)
                if (g.OpenPopupStack[i].PopupId == id)
                    return i;
            return -1;
        }

        void close_menus(const Menu& m)
        {
            ImGui::ClosePopupToLevel(m.root_level, false);
            ImGui::FocusWindow(nullptr); // (the toolkit's active window was the menu: nothing is active after it)
        }

        // The popup of a menu at (x, y); pushes the menu.
        bool begin_menu_window(ImGuiID id, int x, int y, int root_level)
        {
            ImGuiContext& g = *GImGui;
            const int level = popup_level(id);
            if (level < 0)
            {
                g.NextWindowData.ClearFlags();
                return false;
            }
            MenuMemory& mem = menus_state().memory[id];
            if (g.OpenPopupStack[level].OpenFrameCount == g.FrameCount) // (re)opened: a new menu
                mem = MenuMemory{};
            ImGui::SetNextWindowPos(ImVec2(static_cast<float>(x), static_cast<float>(y)));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                           | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                                           | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground
                                           | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
            const bool open = ImGui::BeginPopupEx(id, flags);
            ImGui::PopStyleVar(3);
            if (!open)
                return false;
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.f); // (the active window: not its window's unfocused alpha)
            Menu m;
            m.id = id;
            m.window = g.CurrentWindow;
            m.level = level;
            m.root_level = root_level < 0 ? level : root_level;
            if (!menus_state().windows.contains(m.window->ID))
                menus_state().windows.push_back(m.window->ID);
            // UIContextMenu::on_mouse_move: the row under the cursor (rows clamped to the menu)
            const int rows = mem.h / k_menu_row;
            if (rows > 0 && g.HoveredWindow == m.window) // (its submenu, focused, does not stop it)
            {
                const int my = static_cast<int>(std::floor(g.IO.MousePos.y));
                m.hover = std::clamp((my - static_cast<int>(m.window->Pos.y)) / k_menu_row, 0, rows - 1);
                m.clicked = g.IO.MouseClicked[ImGuiMouseButton_Left];
                // the hovered row opens its submenu now (or closes the open one), as the toolkit's mouse move
                if (m.hover < static_cast<int>(mem.subs.size()))
                    mem.open_sub = mem.subs[static_cast<std::size_t>(m.hover)] ? m.hover : -1;
            }
            menus_state().menus.push_back(std::move(m));
            return true;
        }

        // UIContextMenu::resize after a row was added: the widest of (the width so far [+ 16 for the first icon or
        // submenu row], text + 8 [+ 16 icons] [+ 16 arrows])
        int add_row(Menu& m, const char* label, Icon icon, bool sub)
        {
            MenuRow row;
            row.text.assign(label, ImGui::FindRenderedTextEnd(label));
            row.icon = icon;
            row.sub = sub;
            int base = m.w;
            if (!sub && !m.has_icons && icon != Icon::None)
            {
                base += 16;
                m.has_icons = true;
            }
            if (sub && !m.has_sub) // (a submenu row's icon does not open the icon column)
            {
                base += 16;
                m.has_sub = true;
            }
            int need = CalcTextWidth(normal_font(), row.text.c_str(), row.text.c_str() + row.text.size()) + 8;
            if (m.has_icons)
                need += 16;
            if (m.has_sub)
                need += 16;
            m.w = std::max(base, need);
            m.rows.push_back(std::move(row));
            return static_cast<int>(m.rows.size()) - 1;
        }

        // UIContextMenu::paint, then the menu's size and submenu for the next frame
        void end_menu_window()
        {
            IM_ASSERT_USER_ERROR_RET(!menus_state().menus.empty(),
                                     "EndPopup() / EndMenu() without BeginPopup() / BeginMenu()");
            Menu m = std::move(menus_state().menus.back());
            menus_state().menus.pop_back();
            MenuMemory& mem = menus_state().memory[m.id];
            ImGuiWindow* window = m.window;
            ImDrawList* dl = window->DrawList;
            const int n = static_cast<int>(m.rows.size());
            const int x = static_cast<int>(window->Pos.x), y0 = static_cast<int>(window->Pos.y);
            const int w = m.w, h = k_menu_row * n;
            // (the clip rect is the window's: the size of the last frame)
            ImGui::PushClipRect(ImVec2(static_cast<float>(x), static_cast<float>(y0)),
                                ImVec2(static_cast<float>(x + w), static_cast<float>(y0 + h)), false);
            FillRect(dl, x, y0, x + w, y0 + h, tc(TC_BG));
            OutlineRect(dl, x, y0, x + w, y0 + h, tc(TC_BORDER));
            for (int i = 0, y = y0; i < n; ++i, y += k_menu_row)
            {
                const MenuRow& row = m.rows[static_cast<std::size_t>(i)];
                const bool hot = i == m.hover;
                const ImVec4& c = tc(hot ? TC_BORDER_ALT : TC_BORDER);
                if (row.icon != Icon::None)
                    AddIcon(dl, row.icon, x, y, x + 16, y + 16, c);
                if (row.sub)
                    AddIcon(dl, Icon::ArrowRight, x + w - 16, y, x + w, y + 16, c);
                const int tx = x + 4 + (m.has_icons ? 16 : 0);
                AddText(dl, normal_font(), tx, center_y(k_menu_row, text_h(), y), tc(hot ? TC_TEXT_ALT : TC_TEXT),
                        row.text.c_str(), row.text.c_str() + row.text.size());
                if (mem.open_sub == i)
                    OutlineRect(dl, x, y, x + w, y + k_menu_row, c);
                if (hot)
                    OutlineRect(dl, x, y, x + w, y + k_menu_row, c);
            }
            ImGui::PopClipRect();
            // the window fits the rows (the next frame)
            window->DC.CursorPos = window->Pos;
            ImGui::Dummy(ImVec2(static_cast<float>(w), static_cast<float>(std::max(h, 1))));
            if (window->Appearing) // (ImGui hides an auto-sized window's first frame: the toolkit's menu shows at once)
            {
                window->Size = window->SizeFull = ImVec2(static_cast<float>(w), static_cast<float>(std::max(h, 1)));
                window->HiddenFramesCannotSkipItems = 0;
                window->Hidden = false;
            }
            mem.w = w;
            mem.h = h;
            mem.subs.resize(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i)
                mem.subs[static_cast<std::size_t>(i)] = m.rows[static_cast<std::size_t>(i)].sub;
            if (m.hover != -1) // (the rows of this frame)
                mem.open_sub = m.rows[static_cast<std::size_t>(m.hover)].sub ? m.hover : -1;
            if (mem.open_sub == -1 && GImGui->OpenPopupStack.Size > m.level + 1 && popup_level(m.id) == m.level)
                ImGui::ClosePopupToLevel(m.level + 1, false);
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

    } // namespace

    namespace detail
    {

        bool is_menu_window(const ImGuiWindow* window)
        {
            return window && (window->Flags & ImGuiWindowFlags_Popup) && menus_state().windows.contains(window->ID);
        }

    } // namespace detail

    bool BeginPopup(const char* str_id, ImGuiWindowFlags flags)
    {
        (void)flags;
        ImGuiContext& g = *GImGui;
        const ImGuiID id = g.CurrentWindow->GetID(str_id);
        const int level = popup_level(id);
        if (level < 0)
        {
            g.NextWindowData.ClearFlags();
            return false;
        }
        // at the cursor where it was opened; upwards when its bottom would reach the screen's
        const MenuMemory& mem = menus_state().memory[id];
        const ImGuiPopupData& data = g.OpenPopupStack[level];
        const int x0 = static_cast<int>(std::floor(data.OpenMousePos.x)),
                  y0 = static_cast<int>(std::floor(data.OpenMousePos.y));
        const int h = data.OpenFrameCount == g.FrameCount ? 0 : mem.h;
        const int y = y0 + h >= static_cast<int>(g.IO.DisplaySize.y) ? y0 - h : y0;
        return begin_menu_window(id, x0, y, -1);
    }

    void EndPopup()
    {
        end_menu_window();
    }

    bool MenuItem(const char* label, Icon icon)
    {
        IM_ASSERT_USER_ERROR_RETV(!menus_state().menus.empty(), false, "MenuItem() outside BeginPopup()");
        Menu& m = menus_state().menus.back();
        const int i = add_row(m, label, icon, false);
        if (m.clicked && m.hover == i) // UIContextMenu::on_mouse_down: the press runs the row and closes the menu
        {
            m.clicked = false;
            close_menus(m);
            return true;
        }
        return false;
    }

    bool BeginMenu(const char* label, Icon icon)
    {
        IM_ASSERT_USER_ERROR_RETV(!menus_state().menus.empty(), false, "BeginMenu() outside BeginPopup()");
        Menu& m = menus_state().menus.back();
        const int i = add_row(m, label, icon, true);
        if (m.clicked && m.hover == i) // (a pressed submenu row runs nothing, but the menu closes)
        {
            m.clicked = false;
            close_menus(m);
            return false;
        }
        const MenuMemory& mem = menus_state().memory[m.id];
        if (mem.open_sub != i)
            return false;
        ImGuiContext& g = *GImGui;
        const ImGuiID sub_id = m.window->GetID(label);
        if (popup_level(sub_id) != m.level + 1)
            ImGui::OpenPopupEx(sub_id, ImGuiPopupFlags_None);
        // the child sits at the menu's right edge, level with its row (UIContextMenu::add_submenu)
        const int x = static_cast<int>(m.window->Pos.x) + mem.w;
        const int y = static_cast<int>(m.window->Pos.y) + k_menu_row * i;
        const int root_level = m.root_level;
        (void)g;
        return begin_menu_window(sub_id, x, y, root_level);
    }

    void EndMenu()
    {
        end_menu_window();
    }

    bool IsContextMenuFocused()
    {
        return is_menu_window(GImGui->NavWindow);
    }

} // namespace imgui_nullcore
