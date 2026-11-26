#pragma once
// The pieces the widgets of imgui_nullcore share (imgui_nullcore.cpp defines them): the toolkit's colours, metrics,
// layout helpers and painters.  Not part of the public API.

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_nullcore/imgui_nullcore.hpp>

namespace imgui_nullcore::detail
{

    // nc::ui::ThemeSlot
    enum Slot : int
    {
        TC_BG,
        TC_BG_ALT,
        TC_BG_FOCUSED,
        TC_BORDER,
        TC_BORDER_ALT,
        TC_BORDER_FOCUSED,
        TC_BORDER_SELECTED,
        TC_TEXT,
        TC_TEXT_ALT,
        TC_TEXT_FOCUSED,
        TC_TEXT_SELECTED,
        TC_EXIT_FOCUSED,
        TC_EXIT_PRESSED,
        TC_INACTIVE,
    };

    // the toolkit's layout constants (ui/widgets/decl.hpp, widgets_*.cpp)
    constexpr int k_row = 16; // every control
    constexpr int k_gap = 2; // between items and the label column and its control
    constexpr int k_title = 16; // title bar rows, its line on the last one
    constexpr int k_tab_row = 15; // tab strip header
    constexpr float k_repeat_delay = 0.2f; // first auto-repeat of a held arrow
    constexpr float k_repeat_interval = 0.01f;
    constexpr float k_min_thumb = 12.f;
    constexpr int k_icon_cell = 18; // the icon texture: each 16 x 16 icon in an 18 x 18 cell

    // ---- the state of the current ImGui context
    // Each source file keeps its state in one part, made on its first use in a context and freed when ImGui
    // destroys that context (a Shutdown hook), so two contexts never share windows, menus, pickers or textures.
    enum Part : int
    {
        PART_CORE,
        PART_PICKERS,
        PART_MENUS,
        PART_TABLES,
        PART_LISTS,
        PART_TABS,
        PART_COUNT,
    };
    struct PartSlot
    {
        void* data = nullptr;
        void (*destroy)(void* data) = nullptr;
        void (*end_frame)(void* data) = nullptr; // the part's checks at the end of a frame (T::end_frame)
    };
    PartSlot& context_part(int part); // the current context's (one must be current)
    template <class T>
    T& context_state(int part)
    {
        PartSlot& slot = context_part(part);
        if (!slot.data)
        {
            slot.data = IM_NEW(T)();
            slot.destroy = [](void* data)
            {
                IM_DELETE(static_cast<T*>(data));
            };
            if constexpr (requires(T& t) { t.end_frame(); })
                slot.end_frame = [](void* data)
                {
                    static_cast<T*>(data)->end_frame();
                };
        }
        return *static_cast<T*>(slot.data);
    }

    // Unregisters (from the current context) and frees a texture of ours, then nulls it.
    void free_texture(ImTextureData*& tex);

    // ---- colours and fonts
    const ImVec4& tc(int slot); // the palette's slot
    ImU32 u32(const ImVec4& c); // with style.Alpha
    ImFont* normal_font();
    ImFont* header_font();
    float font_size();
    int text_h();
    int center_y(int h, int th, int y); // (h - text_h) * 0.5 + y, truncated
    // The end of the characters of `text` whose advances fit in max_w.
    const char* fit_end(ImFont* font, const char* text, const char* text_end, int max_w);
    int state_slot(int base, bool hovered, bool held); // idle / hovered / pressed: base, base + 1, base + 2
    double now();
    ImTextureData* icon_texture();

    // ---- layout
    float label_width();
    bool has_label(const char* label);
    float resolve_width(float w); // > 0 as is, 0 the item width, < 0 that much less than the rest of the row
    bool add_item(ImGuiID id, float w, float h, ImRect& bb); // a w x h item at the cursor; false when clipped
    float label_column(const char* label); // the label column of a labelled widget; returns the widget's width
    void finish_item(ImGuiID id, const ImRect& bb, bool hovered); // for SetItemTooltip
    bool is_focused(const ImGuiWindow* window);
    bool is_menu_window(const ImGuiWindow* window); // menus.cpp: an open context menu's window
    bool table_cell_text(const char* text, const char* end); // tables.cpp: inside a cell, its text (true: taken)

    // ---- painters
    // UIScrollBar; `pos` / `size` are fractions.  True when `pos` changed.
    bool scrollbar(ImGuiID id, int x, int y, int w, int h, float* pos, float size);
    void paint_field_frame(ImDrawList* dl, int x, int y, int w, int h, bool hovered, bool held, bool editing,
                           bool split);
    void paint_field_text(ImDrawList* dl, int x, int y, int client_w, int h, const char* text, int slot);
    // Edits `buf` over a field's client area (ImGui's input, the toolkit's painting); `id_label` gives `id`.
    bool edit_field(ImGuiID id, const char* id_label, const ImRect& text_bb, char* buf, int buf_size,
                    ImGuiInputTextFlags flags, ImDrawList* dl, const char* hint,
                    ImGuiInputTextCallback callback = nullptr, void* user_data = nullptr);

    void shutdown_color_pickers(); // color_picker.cpp

} // namespace imgui_nullcore::detail
