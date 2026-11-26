#pragma once
// imgui_nullcore: the nullcore menu look for Dear ImGui (1.92+), pixel for pixel, behind Dear ImGui's own API.
//
// The functions are named and called like ImGui's (Begin / End, Checkbox, Button, InputText, InputInt, Combo, ListBox,
// BeginTabBar / BeginTabItem, BeginTable ..., BeginPopup / MenuItem, SetItemTooltip ...) and follow its rules (ids from
// labels, "##" hides a label, SetNextItemWidth / PushItemWidth, ImVec2(0, 0) sizes fit the content, negative sizes
// leave that much room), but each one paints what its nullcore toolkit twin paints
// (libs/nclib/source/ui/widgets/*.cpp):
//
//  * fonts     the toolkit's Verdana made the way its D3D9 renderer makes it: GDI glyphs, hard pixels (no
//              antialiasing), a black 1 px outline (vgui's FONTFLAG_OUTLINE).  An ImGui font size is the line height:
//              the toolkit's 12 px fonts are size 14.  The atlas must stay RGBA32 (ImGui's default) for the outline.
//  * style     StyleColorsNullcore() sets every ImGuiStyle colour / metric: 16 px rows 2 px apart, 16 px title bars.
//  * widgets   fill the row by default (nullcore's rows do; Begin sets the item width to -FLT_MIN); a labelled one
//              puts its label right-aligned in a label column first (112 px, PushLabelWidth), as the toolkit's
//              settings rows do, instead of ImGui's label on the right.
//
// The extras ImGui has no twin of: BeginGroupBox / EndGroupBox, KeyBind, IconButton, ImageTextButton, Label,
// ListBoxEx / StringListEditor, the colour picker, PushLabelWidth, and the primitives (FillRect ...) for custom
// controls in the same look.  ImGui's widgets the toolkit has no twin of (DragFloat, Selectable, RadioButton,
// Separator, trees ...) still work beside these: ImGui's shapes in StyleColorsNullcore's colours and metrics.
//
// The state (options, palette, fonts, open pickers and menus, textures) is the current ImGui context's, made on first
// use and freed with the context: call these with a context current, as ImGui's own.  An unbalanced call (End
// without Begin, a group box, tab bar, table, menu or label width left open at the end of the frame) is one of
// ImGui's recoverable user errors (io.ConfigErrorRecovery*).
//
// Independent of the rest of nullcore: it needs Dear ImGui only.  The fonts are GDI's on Windows and the same glyphs
// baked from GDI elsewhere (GlyphSource).

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <string>
#include <vector>

namespace imgui_nullcore
{

    // r | g << 8 | b << 16 | a << 24, the dword nullcore's theme stores.
    constexpr ImVec4 ColorFromDword(std::uint32_t v)
    {
        return ImVec4(static_cast<float>(v & 0xFF) / 255.f, static_cast<float>((v >> 8) & 0xFF) / 255.f,
                      static_cast<float>((v >> 16) & 0xFF) / 255.f, static_cast<float>(v >> 24) / 255.f);
    }

    // The window-chrome slots of nullcore's theme, in the order of nc::ui::ThemeSlot.
    struct Palette
    {
        static constexpr int SlotCount = 14;

        ImVec4 Bg;
        ImVec4 BgAlt;
        ImVec4 BgFocused;
        ImVec4 Border;
        ImVec4 BorderAlt;
        ImVec4 BorderFocused;
        ImVec4 BorderSelected;
        ImVec4 Text;
        ImVec4 TextAlt;
        ImVec4 TextFocused; // the window title
        ImVec4 TextSelected;
        ImVec4 ExitFocused; // close button, hovered
        ImVec4 ExitPressed; // close button, pressed
        ImVec4 Inactive; // disabled text

        // nullcore's factory preset.
        static Palette Factory();
        // The first SlotCount dwords of a nullcore preset row (fewer: the factory colour for the rest).
        static Palette FromDwords(const std::uint32_t* dwords, int count);
    };

    // ---- fonts ------------------------------------------------------------------------------------------------------

    // A vgui-style font spec (the toolkit's FontSpec): family, cell height, weight, effects.
    struct FontSpec
    {
        const char* Family = "Verdana";
        int Tall = 12; // the GDI cell height
        int Weight = 400; // 700: bold
        bool Outline = true; // FONTFLAG_OUTLINE: 1 px black around the glyph
        bool DropShadow = false; // FONTFLAG_DROPSHADOW
        bool Antialias = false; // FONTFLAG_ANTIALIAS (the toolkit's fonts have none)

        // The line height (the ImGui size): the cell + 2 * outline + shadow.
        constexpr float LineHeight() const
        {
            return static_cast<float>(Tall + (Outline ? 2 : 0) + (DropShadow ? 1 : 0));
        }
    };

    struct Fonts
    {
        ImFont* Normal = nullptr; // Verdana regular (UIFONT_NORMAL): the default font
        ImFont* Header = nullptr; // Verdana bold (UIFONT_HEADER): titles, tabs, group boxes
        float Size = 14.f; // their line height (12 px cells)
    };

    // Where the glyphs of AddFont / LoadFonts come from (GetOptions().Glyphs, read when the font is added).
    enum class GlyphSource : int
    {
        Auto, // GDI on Windows, the baked glyphs elsewhere
        Gdi, // GDI (where there is none: as Auto)
        Baked, // the glyphs baked from GDI (tools/bake_fonts.cpp): the same pixels on every platform, for the toolkit's
               // fonts (Verdana regular and bold, outlined, sizes 14 .. 22) and the code points baked
    };

    // Adds a font made like the toolkit's: GDI's glyphs, or the baked ones (GlyphSource); a font neither has (another
    // family or style, off Windows) is ImGui's own at the same line height.
    ImFont* AddFont(ImFontAtlas* atlas, const FontSpec& spec);
    // Adds the toolkit's regular (first, so it becomes the default font) and bold Verdana.  Call before the first
    // frame.
    Fonts LoadFonts(ImGuiIO& io, int tall = 12);
    // The fonts of the last LoadFonts().
    const Fonts& GetFonts();

    // ---- style ------------------------------------------------------------------------------------------------------

    // Like ImGui::StyleColorsDark: the colours and metrics of `dst` (the current style when null), and `palette`
    // becomes the one the widgets paint with.
    void StyleColorsNullcore(ImGuiStyle* dst = nullptr, const Palette& palette = Palette::Factory());
    // The palette of the last StyleColorsNullcore().
    const Palette& GetPalette();

    // How the colours reach the screen (GetOptions().Colors, applied by ApplyColorMode).
    enum class ColorMode : int
    {
        Plain, // as they are: what nc_preview and most D3D9 games' UIs show
        SourceGamma, // the way nullcore draws in a Source game: vgui's shader takes a vertex colour as gamma 2.2 and
                     // writes it through the sRGB curve, which darkens the dark ones (the window background 0x233342
                     // shows as 0x1D2F40).  Done on the vertex colours, like nullcore's renderer without its pixel
                     // shader: a flat colour matches the game within a level; a gradient or a blend between colours is
                     // still mixed in gamma space.
    };

    struct Options
    {
        // The toolkit's "Unfocused Transparency" (on by default): windows other than the focused one at half alpha.
        bool UnfocusedTransparency = true;
        // Called by a window's minimise button with the window's name instead of collapsing it (the toolkit hides the
        // window into its task bar).
        void (*Minimize)(const char* name) = nullptr;
        // Begin keeps a window on the screen, dragged or not (the toolkit's windows can cover the task bar, not leave
        // the screen).
        bool ClampToScreen = true;
        // Where the fonts' glyphs come from (set before LoadFonts / AddFont).
        GlyphSource Glyphs = GlyphSource::Auto;
        // How the colours reach the screen (ApplyColorMode).
        ColorMode Colors = ColorMode::Plain;
    };
    Options& GetOptions();

    // Puts the draw data's vertex colours through GetOptions().Colors: call it once a frame, after ImGui::Render() and
    // before the renderer backend's RenderDrawData (nothing to do in ColorMode::Plain).
    void ApplyColorMode(ImDrawData* draw_data);

    // Frees the textures (icons, the colour wheel) and closes the colour pickers: after the renderer backend's
    // Shutdown, before ImGui::DestroyContext.  (ImGui::DestroyContext frees them too.)
    void Shutdown();

    // ---- mouse cursor -----------------------------------------------------------------------------------------------

    // The toolkit's cursors (the vgui dc_* codes its widgets ask for): SizeAll on a window's title bar and while a
    // window is dragged, Hand on a tab or a link, IBeam on an editable text box, SizeNS on a reorderable list's row
    // (the toolkit's code 11, vgui's dc_sizens), SizeWE on a grid's column edge, Crosshair on the colour wheel, else
    // Arrow.
    enum class Cursor : int
    {
        Arrow,
        IBeam,
        Hand,
        SizeAll,
        SizeWE,
        SizeNS,
        Crosshair,
    };
    // Sets this frame's cursor (for custom widgets) and ImGui's nearest one (ImGui has no crosshair: Arrow).
    void SetMouseCursor(Cursor cursor);
    // This frame's cursor (Arrow unless a widget set one), for a host that shows the toolkit's own: set
    // ImGuiConfigFlags_NoMouseCursorChange and show this one after the frame.
    Cursor GetMouseCursor();

    // ---- windows ----------------------------------------------------------------------------------------------------

    // ImGui::Begin with nullcore's chrome: frame, a 16 px title bar (line on its bottom row, bold centred title), the
    // square minimise (collapses the window; not with NoCollapse) and close (with p_open) buttons, nullcore's scroll
    // bar when the content is taller than the window, half alpha when unfocused.  Dragged by the title bar only.
    // Like ImGui::Begin, End() is called whatever Begin() returned.
    bool Begin(const char* name, bool* p_open = nullptr, ImGuiWindowFlags flags = 0);
    void End();

    // ---- text -------------------------------------------------------------------------------------------------------

    // A 16 px row of text in TC_TEXT, 2 px in (a left-aligned UILabel without the hover colours).
    void TextUnformatted(const char* text, const char* text_end = nullptr);
    void Text(const char* fmt, ...) IM_FMTARGS(1);
    void TextV(const char* fmt, va_list args) IM_FMTLIST(1);
    // The same in TC_INACTIVE.
    void TextDisabled(const char* fmt, ...) IM_FMTARGS(1);
    // UILinkLabel: TC_TEXT, TC_TEXT_SELECTED hovered, TC_TEXT_FOCUSED pressed, the hand cursor; true when clicked.
    // width 0: as wide as the text; else `width` px (negative: the room left less that much), placed by align_x.
    bool TextLink(const char* label, float align_x = 0.f, float width = 0.f);
    // UILabel: the link's colours, `width` px wide (0: the item width), the text placed by `align_x` (0 left, 0.5
    // centred, 1 right, 2 px from the edges).  True when clicked.
    bool Label(const char* text, float align_x = 0.f, float width = 0.f, bool disabled = false);

    // ---- widgets ----------------------------------------------------------------------------------------------------

    // UIButton: fill and border follow idle / hovered / pressed, the text centred.  A colour pushed for
    // ImGuiCol_Button (PushStyleColor) is a fixed fill, like a UIButton's fill colour.
    bool Button(const char* label, const ImVec2& size = ImVec2(0, 0));
    // UIButton with an image left of its text (the start menu's buttons): the image inset by half the height
    // difference, the text in the rest placed by align_x (0 left, 0.5 centred, 1 right), the image tinted with the
    // border colour when `tint`.
    bool ImageTextButton(const char* label, ImTextureRef image, const ImVec2& image_size,
                         const ImVec2& size = ImVec2(0, 0), float align_x = 0.f, bool tint = false);
    // The height of the next text box, label or button (decl's .height): the toolkit's rows are 16 px.  A labelled
    // widget's box takes it, not its label; another widget next keeps its own height and drops it.
    void SetNextItemHeight(float height);
    // UIButton with a 16 x 16 icon tinted with the border colour (the preset buttons).
    enum class Icon : int
    {
        None = -1, // (MenuItem / BeginMenu: no icon)
        ArrowDown,
        Plus,
        Minus,
        Square,
        Cross,
        ArrowRight,
        Refresh,
        Import,
        Export,
        Ellipsis,
        UpDown,
        Circle,
        Dot,
        Count
    };
    bool IconButton(const char* str_id, Icon icon);
    // A swatch (a UIButton filled with a colour var), 16 x 16 by default.
    bool ColorButton(const char* desc_id, const ImVec4& col, ImGuiColorEditFlags flags = 0,
                     const ImVec2& size = ImVec2(0, 0));

    // UICheckbox: a 16 px box with an inner square when set, the label 4 px right of it.
    bool Checkbox(const char* label, bool* v);
    enum NcCheckboxFlags_ : int
    {
        NcCheckboxFlags_None = 0,
        NcCheckboxFlags_BoxRight = 1 << 0, // the toolkit's alt style: as wide as the item, the box on the right, the
                                           // label right-aligned beside it
    };
    using NcCheckboxFlags = int;
    bool Checkbox(const char* label, bool* v, NcCheckboxFlags flags);
    // A check box after a label column (decl's Check with a .label): `label` in the column, `text` beside the box.
    bool LabeledCheckbox(const char* label, const char* text, bool* v, NcCheckboxFlags flags = 0);
    // UIRadio: toggles `flags_value` in `flags`.
    bool CheckboxFlags(const char* label, int* flags, int flags_value);

    // UITextBox: the press starts the edit with the caret under the cursor; the hint in TC_INACTIVE while empty.
    bool InputText(const char* label, char* buf, std::size_t buf_size, ImGuiInputTextFlags flags = 0);
    bool InputTextWithHint(const char* label, const char* hint, char* buf, std::size_t buf_size,
                           ImGuiInputTextFlags flags = 0);
    // The same on a std::string, grown as it is typed into (imgui_stdlib's InputText).
    bool InputText(const char* label, std::string* str, ImGuiInputTextFlags flags = 0);
    bool InputTextWithHint(const char* label, const char* hint, std::string* str, ImGuiInputTextFlags flags = 0);
    // UINumberBox: the value and the up / down spinner (a release steps, holding repeats; Ctrl: step_fast).  A float
    // step of 0 steps by the last decimal of `format`, as the toolkit's precisions do.
    bool InputInt(const char* label, int* v, int step = 1, int step_fast = 100, ImGuiInputTextFlags flags = 0);
    bool InputFloat(const char* label, float* v, float step = 0.f, float step_fast = 0.f, const char* format = "%.3f",
                    ImGuiInputTextFlags flags = 0);

    // A slider in the toolkit's parts (it has no slider of its own): a text box's frame, a bar like the check box's inner
    // square up to the value, the value centred.  A press sets the value under the cursor and drags it; ImGui's
    // SliderBehavior does the rest (the arrow keys, ImGuiSliderFlags_Logarithmic / AlwaysClamp ...).
    bool SliderFloat(const char* label, float* v, float v_min, float v_max, const char* format = "%.3f",
                     ImGuiSliderFlags flags = 0);
    bool SliderInt(const char* label, int* v, int v_min, int v_max, const char* format = "%d",
                   ImGuiSliderFlags flags = 0);

    // UIComboBox: the release opens the list, which drops down from the box over what is below.
    // popup_max_height_in_items -1: every item.
    bool Combo(const char* label, int* current_item, const char* const items[], int items_count,
               int popup_max_height_in_items = -1);
    // UIListBox: 16 px rows 2 px apart, nullcore's scroll bar on the right; the press selects.
    // height_in_items -1: up to 7.
    bool ListBox(const char* label, int* current_item, const char* const items[], int items_count,
                 int height_in_items = -1);
    // UIListBox in full: rows from `getter`, filled by `color_getter` (null: TC_BG), `height` px (<= 0: the room left
    // below less that much).  NcListBoxFlags_Removable shows the delete icon on the hovered row (a click on it reports
    // the row in event->removed and clears the selection), NcListBoxFlags_Reorderable lets the selected row be dragged
    // (each step reports the two rows to swap in event->swap_a / swap_b; *current_item follows).  True on a change.
    enum NcListBoxFlags_ : int
    {
        NcListBoxFlags_None = 0,
        NcListBoxFlags_Removable = 1 << 0,
        NcListBoxFlags_Reorderable = 1 << 1,
    };
    using NcListBoxFlags = int;
    struct NcListBoxEvent
    {
        int removed = -1;
        int swap_a = -1, swap_b = -1;
        int right_clicked = -1; // the row under a right click (it does not select it): open a context menu for it
    };
    bool ListBoxEx(const char* label, int* current_item, const char* (*getter)(void* user_data, int idx),
                   ImU32 (*color_getter)(void* user_data, int idx), void* user_data, int items_count, float height,
                   NcListBoxFlags flags = 0, NcListBoxEvent* event = nullptr);
    // UIStringListEditor: a "<+ New Line>" box over a removable, reorderable list of `*items`.  Enter appends the
    // line or replaces the selected row; picking a row loads it into the box.  size.y <= 0: the room left.  True when
    // `*items` changed.
    bool StringListEditor(const char* str_id, std::vector<std::string>* items, const ImVec2& size = ImVec2(0, 0),
                          const char* hint = nullptr);
    // UIKeyBind: `key_name` centred, the ellipsis button on the right; true when clicked.
    bool KeyBind(const char* label, const char* key_name);

    // UIImage: the image at its natural size, centred in a `size` box (ImGui's item sizes: 0 fits the image, negative
    // leaves that much room).
    void Image(ImTextureRef tex_ref, const ImVec2& image_size, const ImVec2& size = ImVec2(0, 0));

    // ---- tables -----------------------------------------------------------------------------------------------------

    // UIListBoxEx, the grid of the Player Manager: a header row in the bold font, then 16 px rows 15 px apart (each
    // cell filled, its text 4 px in, a border line right and below), nullcore's scroll bar on the right (1 px outside
    // the box, under the cells), the mouse wheel scrolls by a page.  The columns keep their widths (no fitting): a drag
    // on a header's right edge resizes that column and the last one takes the difference.  Rows past the box are cut;
    // the scroll range follows the previous frame's row count.
    //   outer_size: ImGui's item size (0 or negative: the room left).  flags / inner_width: ignored.
    //   Cells: TableNextColumn(), then TableSetBgColor(ImGuiTableBgTarget_CellBg, ...) and TextUnformatted / Text, in
    //   any order (the fill defaults to TC_BG, TC_BG_ALT hovered; the text is TC_TEXT, TC_TEXT_FOCUSED hovered).
    bool BeginTable(const char* str_id, int columns, ImGuiTableFlags flags = 0, const ImVec2& outer_size = ImVec2(0, 0),
                    float inner_width = 0.f);
    void EndTable();
    // A column `init_width` px wide (0: 64), its width kept by the table.
    void TableSetupColumn(const char* label, ImGuiTableColumnFlags flags = 0, float init_width = 0.f,
                          ImGuiID user_id = 0);
    // A column whose width is `*width` (the toolkit's width vars), resized within [min_width, max_width].
    void TableSetupColumn(const char* label, int* width, int min_width, int max_width);
    void TableHeadersRow();
    void TableNextRow(ImGuiTableRowFlags row_flags = 0, float min_row_height = 0.f);
    bool TableNextColumn();
    // ImGuiTableBgTarget_CellBg: the current cell's fill (the other targets are ignored).
    void TableSetBgColor(ImGuiTableBgTarget target, ImU32 color, int column_n = -1);
    // The hovered cell: column (-1 none) and row (-1 the header, -2 none or a column edge under the cursor; a row
    // counts once it was given, so ask after the rows).  A right click on a cell is
    // ImGui::IsMouseClicked(ImGuiMouseButton_Right) while TableGetHoveredRow() != -2.
    int TableGetHoveredColumn();
    int TableGetHoveredRow();

    // ---- context menus ----------------------------------------------------------------------------------------------

    // UIContextMenu: a popup opened with ImGui::OpenPopup(str_id) at the cursor (upwards when it would leave the
    // screen), as wide as its widest row; 16 px rows outlined when hovered, an icon column once a row has an icon, a
    // ">" column once a row opens a submenu.  A press on a row runs it and closes the menu; a press elsewhere closes
    // it. While a menu is open every window is unfocused (the toolkit makes the menu the active window), and none is
    // focused after it closed.  The rows are in the current font.  Call EndPopup() only when BeginPopup() returned
    // true.
    bool BeginPopup(const char* str_id, ImGuiWindowFlags flags = 0);
    void EndPopup();
    // A row: true when pressed.
    bool MenuItem(const char* label, Icon icon = Icon::None);
    // A row that opens a submenu (to its right) while hovered: true while open; then EndMenu().
    bool BeginMenu(const char* label, Icon icon = Icon::None);
    void EndMenu();
    // A context menu has the focus (the toolkit's active window is the menu: no other window is active).
    bool IsContextMenuFocused();

    // The desktop's hint box for the last item while it (or a labelled widget's label) is hovered: 256 px wide at the
    // cursor, wrapped on spaces and newlines, TC_TEXT_ALT on TC_BG, over everything.
    void SetItemTooltip(const char* fmt, ...) IM_FMTARGS(1);
    void SetItemTooltipV(const char* fmt, va_list args) IM_FMTLIST(1);

    // UITabStrip: a 15 px row of bold tabs.  First in a window it spans the client area flush with the title bar, as
    // the toolkit's; elsewhere it starts at the cursor.  BeginTabItem returns true for the selected tab, whose page
    // follows it; call EndTabItem() only then.  A click selects a tab on the release (ImGuiTabItemFlags_SetSelected
    // selects it from code).
    bool BeginTabBar(const char* str_id, ImGuiTabBarFlags flags = 0);
    void EndTabBar();
    bool BeginTabItem(const char* label, bool* p_open = nullptr, ImGuiTabItemFlags flags = 0);
    void EndTabItem();

    // UIGroupBox: the frame starts 8 px in from the top-left, the bold title cuts its top edge, the rows start 11 px
    // in.  size.x 0: the item width; size.y 0: fits the rows.  Nested in another group it moves 8 px left, like the
    // toolkit's.  `label_width`: the label column inside.  The title is an ID scope (equal labels in two groups are two
    // widgets).  Like BeginChild, EndGroupBox() is called whatever BeginGroupBox() returned.
    bool BeginGroupBox(const char* title, const ImVec2& size = ImVec2(0, 0), float label_width = 112.f);
    void EndGroupBox();

    // A colour swatch in the next labelled widget's label column (its last 18 px, decl's .swatch); a click opens the
    // colour picker `picker_title` on `*col` (r | g << 8 | b << 16 | a << 24, ImGui's ImU32 layout).
    void SetNextItemSwatch(ImU32* col, const char* picker_title, bool alpha = true);

    // nullcore's colour picker (UIColorPicker): a 375 x 195 window centred on the screen with the hue / saturation
    // wheel, the R G B H S V (A) sliders and boxes, the hex box, Reset (to `default_col`), the palette and Submit.
    // Submit writes the edited colour to `*col` and closes it; the close button keeps the original.  Opening a title
    // that is open points it at `col` again; opening one of a non-zero `group` closes the others of that group.  The
    // pickers are drawn by ShowColorPickers(), once per frame (`*col` must live while the picker is open).
    void OpenColorPicker(const char* title, ImU32* col, ImU32 default_col, bool alpha = true, int group = 0);
    void ShowColorPickers();
    bool IsColorPickerOpen(const char* title);
    // The open pickers in the order they were opened (for a task bar), minimising one (open, not drawn) and closing
    // one as its close button does.
    int GetColorPickerCount();
    const char* GetColorPickerTitle(int index);
    void SetColorPickerHidden(const char* title, bool hidden);
    bool IsColorPickerHidden(const char* title);
    void CloseColorPicker(const char* title);

    // The label column of the labelled widgets (default 112 px).
    void PushLabelWidth(float width);
    void PopLabelWidth();

    // ---- primitives -------------------------------------------------------------------------------------------------

    // The toolkit's IUIRender primitives, in pixels (x1 / y1 exclusive), with style.Alpha applied: what the widgets are
    // made of, for custom controls in the same look.
    void FillRect(ImDrawList* dl, int x0, int y0, int x1, int y1, const ImVec4& col);
    void OutlineRect(ImDrawList* dl, int x0, int y0, int x1, int y1, const ImVec4& col);
    void Line(ImDrawList* dl, int x0, int y0, int x1, int y1, const ImVec4& col); // both ends included
    void OutlineCircle(ImDrawList* dl, int x, int y, int radius, int segments, const ImVec4& col);
    void AddIcon(ImDrawList* dl, Icon icon, int x0, int y0, int x1, int y1, const ImVec4& tint, bool flip_v = false);
    void AddText(ImDrawList* dl, ImFont* font, int x, int y, const ImVec4& col, const char* text,
                 const char* text_end = nullptr);
    int CalcTextWidth(ImFont* font, const char* text, const char* text_end = nullptr);

} // namespace imgui_nullcore
