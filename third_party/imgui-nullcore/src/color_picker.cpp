// nullcore's colour picker window (libs/nclib/source/ui/widgets/color_picker.cpp) on imgui_nullcore: the same layout,
// maths (truncating HSV, the procedural wheel texture) and behaviour (Submit keeps, the close button reverts).

#include "internal.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace imgui_nullcore
{

    namespace
    {

        using namespace detail;

        // the 18 swatches (r | g<<8 | b<<16 | a<<24)
        constexpr std::array<ImU32, 18> k_palette = {
                0xFFFFFF7F, 0xFFFF7FFF, 0xFF7FFFFF, 0xFF7F7FFF, 0xFF7FFF7F, 0xFFFF7F7F,
                0xFF1F1F7F, 0xFF1F7F1F, 0xFF7F1F1F, 0xFF7F7F1F, 0xFF7F1F7F, 0xFF1F7F7F,
                0xFFFF7F1F, 0xFF7F1FFF, 0xFF1FFF7F, 0xFF7FFF1F, 0xFFFF1F7F, 0xFF1F7FFF,
        };

        constexpr float k_rad2_deg = 57.2957763671875f; // (the toolkit's constants, not the nearest floats)
        constexpr float k_deg2_rad = 0.01745329238474369f;
        constexpr int k_wheel_tex_size = 256;
        constexpr int k_width = 375, k_height = 195;

        struct Rgba
        {
            int r = 0, g = 0, b = 0, a = 255;
        };

        ImU32 to_u32(Rgba c)
        {
            return static_cast<ImU32>(c.r) | static_cast<ImU32>(c.g) << 8 | static_cast<ImU32>(c.b) << 16
                   | static_cast<ImU32>(c.a) << 24;
        }

        Rgba from_u32(ImU32 v)
        {
            return {static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF), static_cast<int>((v >> 16) & 0xFF),
                    static_cast<int>(v >> 24)};
        }

        ImVec4 to_vec4(Rgba c)
        {
            return ImGui::ColorConvertU32ToFloat4(to_u32(c));
        }

        float wrap_degrees(float a)
        {
            const float r = static_cast<float>(std::fmod(static_cast<double>(a), 360.0));
            return r < 0.0f ? r + 360.0f : r;
        }

        // HSV -> RGB (h degrees, s and v in 0..1), components truncated
        Rgba hsv_to_rgb(float h, float s, float v)
        {
            const float v4 = v * 255.0f;
            if (s == 0.0f || v4 == 0.0f)
                return {static_cast<int>(v4), static_cast<int>(v4), static_cast<int>(v4), 255};
            const float sector = h / 60.0f;
            const int i = static_cast<int>(sector);
            const float f = sector - static_cast<float>(i);
            const float p = (1.0f - s) * v4;
            const float q = (1.0f - f * s) * v4;
            const float t = (1.0f - (1.0f - f) * s) * v4;
            switch (i % 6)
            {
            case 0:
                return {static_cast<int>(v4), static_cast<int>(t), static_cast<int>(p), 255};
            case 1:
                return {static_cast<int>(q), static_cast<int>(v4), static_cast<int>(p), 255};
            case 2:
                return {static_cast<int>(p), static_cast<int>(v4), static_cast<int>(t), 255};
            case 3:
                return {static_cast<int>(p), static_cast<int>(q), static_cast<int>(v4), 255};
            case 4:
                return {static_cast<int>(t), static_cast<int>(p), static_cast<int>(v4), 255};
            default:
                return {static_cast<int>(v4), static_cast<int>(p), static_cast<int>(q), 255};
            }
        }

        // v always; h and s only for a colour that is neither black nor grey
        void rgb_to_hsv(Rgba c, float& h, float& s, float& v)
        {
            const int mx = std::max(c.r, std::max(c.g, c.b));
            const int mn = std::min(c.r, std::min(c.g, c.b));
            v = static_cast<float>(mx) / 255.0f;
            const float delta = static_cast<float>(mx - mn);
            if (mx != 0 && delta != 0.0f)
            {
                float hh;
                if (c.r == mx)
                    hh = static_cast<float>(c.g - c.b) / delta;
                else if (c.g == mx)
                    hh = static_cast<float>(c.b - c.r) / delta + 2.0f;
                else
                    hh = static_cast<float>(c.r - c.g) / delta + 4.0f;
                h = wrap_degrees(hh * 60.0f);
                s = delta / static_cast<float>(mx);
            }
        }

        bool wheel_to_hue_sat(int x, int y, int r, float& hue, float& sat)
        {
            hue = wrap_degrees(static_cast<float>(std::atan2(static_cast<double>(y), static_cast<double>(x)))
                               * k_rad2_deg);
            const float d = static_cast<float>(std::sqrt(static_cast<double>(x) * x + static_cast<double>(y) * y))
                            / static_cast<float>(r);
            sat = d >= 1.0f ? 1.0f : d;
            return sat < 1.0f;
        }

        ImTextureData*& wheel();

        ImTextureData* wheel_texture()
        {
            ImTextureData*& g_wheel = wheel();
            if (g_wheel)
                return g_wheel;
            g_wheel = IM_NEW(ImTextureData)();
            g_wheel->Create(ImTextureFormat_RGBA32, k_wheel_tex_size, k_wheel_tex_size);
            const int half = k_wheel_tex_size / 2;
            for (int row = 0; row < k_wheel_tex_size; ++row)
                for (int col = 0; col < k_wheel_tex_size; ++col)
                {
                    auto* p = static_cast<unsigned char*>(g_wheel->GetPixelsAt(col, row));
                    p[0] = p[1] = p[2] = p[3] = 0;
                    float hue, sat;
                    if (!wheel_to_hue_sat(col - half, row - half, half, hue, sat))
                        continue;
                    const Rgba c = hsv_to_rgb(hue, sat, 1.0f);
                    p[0] = static_cast<unsigned char>(c.r);
                    p[1] = static_cast<unsigned char>(c.g);
                    p[2] = static_cast<unsigned char>(c.b);
                    p[3] = 255;
                }
            g_wheel->UseColors = true; // (a backend may store it in another channel order: it converts coloured ones)
            ImGui::RegisterUserTexture(g_wheel);
            return g_wheel;
        }

        struct Picker
        {
            std::string title;
            ImU32* target = nullptr;
            int group = 0;
            bool alpha = true;
            bool open = true;
            Rgba original, def;
            int r = 0, g = 0, b = 0, a = 255, h = 0, s = 0, v = 0; // the seven editing vars
            float hue = 0.f, sat = 0.f, val = 0.f; // their fractions
            char hex[16] = {};
            bool center = true; // place it on the next frame
            bool hidden = false; // minimised (SetColorPickerHidden): open, not drawn

            Rgba color() const
            {
                return {r, g, b, alpha ? a : 255};
            }

            void update_hex()
            {
                const Rgba c = color();
                if (alpha)
                    std::snprintf(hex, sizeof(hex), "%02X%02X%02X%02X", static_cast<unsigned>(c.r), static_cast<unsigned>(c.g),
                                  static_cast<unsigned>(c.b), static_cast<unsigned>(c.a));
                else
                    std::snprintf(hex, sizeof(hex), "%02X%02X%02X", static_cast<unsigned>(c.r), static_cast<unsigned>(c.g),
                                  static_cast<unsigned>(c.b));
            }

            void set_color(Rgba c)
            {
                r = c.r;
                g = c.g;
                b = c.b;
                a = c.a;
                rgb_to_hsv(c, hue, sat, val);
                h = static_cast<int>(hue);
                s = static_cast<int>(sat * 100.0f);
                v = static_cast<int>(val * 100.0f);
                update_hex();
            }

            void set_hue_sat(float hue_deg, float sat01, float val01)
            {
                hue = hue_deg;
                sat = sat01;
                val = val01;
                h = static_cast<int>(hue_deg);
                s = static_cast<int>(sat01 * 100.0f);
                v = static_cast<int>(val01 * 100.0f);
                const Rgba c = hsv_to_rgb(hue_deg, sat01, val01);
                r = c.r;
                g = c.g;
                b = c.b;
                update_hex();
            }

            // slider position t of channel 0..6 (R G B H S V A) -> its var, then RGB <-> HSV as the var callbacks do
            void set_channel(int channel, float t)
            {
                switch (channel)
                {
                case 0:
                    r = static_cast<int>(t * 255.0f);
                    break;
                case 1:
                    g = static_cast<int>(t * 255.0f);
                    break;
                case 2:
                    b = static_cast<int>(t * 255.0f);
                    break;
                case 3:
                    h = std::min(static_cast<int>(t * 360.0f), 359);
                    break;
                case 4:
                    s = static_cast<int>(t * 100.0f);
                    break;
                case 5:
                    v = static_cast<int>(t * 100.0f);
                    break;
                default:
                    a = static_cast<int>(t * 255.0f);
                    break;
                }
                channel_changed(channel);
            }

            void channel_changed(int channel)
            {
                if (channel < 3 || channel == 6)
                    set_color(color());
                else
                    set_hue_sat(static_cast<float>(h), static_cast<float>(s) * 0.01f, static_cast<float>(v) * 0.01f);
            }

            float channel_value(int channel) const
            {
                const Rgba c = color();
                switch (channel)
                {
                case 0:
                    return static_cast<float>(c.r) / 255.0f;
                case 1:
                    return static_cast<float>(c.g) / 255.0f;
                case 2:
                    return static_cast<float>(c.b) / 255.0f;
                case 3:
                    return hue / 360.0f;
                case 4:
                    return sat;
                case 5:
                    return val;
                default:
                    return static_cast<float>(c.a) / 255.0f;
                }
            }

            Rgba channel_color(int channel, float t) const
            {
                Rgba c = color();
                switch (channel)
                {
                case 0:
                    c.r = static_cast<int>(t * 255.0f) & 0xFF;
                    break;
                case 1:
                    c.g = static_cast<int>(t * 255.0f) & 0xFF;
                    break;
                case 2:
                    c.b = static_cast<int>(t * 255.0f) & 0xFF;
                    break;
                case 3:
                    c = hsv_to_rgb(t * 360.0f, 1.0f, 1.0f);
                    break;
                case 4:
                    c = hsv_to_rgb(hue, t, val);
                    break;
                case 5:
                    c = hsv_to_rgb(hue, sat, t);
                    break;
                default:
                    c.a = static_cast<int>(t * 255.0f) & 0xFF;
                    break;
                }
                return c;
            }

            void commit_hex()
            {
                std::uint32_t x = static_cast<std::uint32_t>(std::strtoul(hex, nullptr, 16));
                if (!alpha)
                    x = (x << 8) | 0xFF;
                set_color({static_cast<int>(x >> 24), static_cast<int>((x >> 16) & 0xFF),
                           static_cast<int>((x >> 8) & 0xFF), static_cast<int>(x & 0xFF)});
            }
        };

        // the current context's pickers (PART_PICKERS)
        struct PickersState
        {
            std::vector<Picker> pickers;
            ImTextureData* wheel = nullptr;

            PickersState() = default;
            PickersState(const PickersState&) = delete;
            PickersState& operator=(const PickersState&) = delete;
            ~PickersState()
            {
                free_texture(wheel);
            }
        };
        PickersState& pickers_state()
        {
            return context_state<PickersState>(PART_PICKERS);
        }
        std::vector<Picker>& pickers()
        {
            return pickers_state().pickers;
        }
        ImTextureData*& wheel()
        {
            return pickers_state().wheel;
        }

        // ShapeTessellator::segment: a 1 px quad over the line's pixel centres, half a pixel past each end
        void segment(ImDrawList* dl, float x0, float y0, float x1, float y1, ImU32 col)
        {
            const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
            if (!(len > 0.0f))
                return;
            const float ux = dx / len * 0.5f, uy = dy / len * 0.5f, nx = uy, ny = -ux;
            dl->AddQuadFilled(ImVec2(x0 - ux + nx, y0 - uy + ny), ImVec2(x1 + ux + nx, y1 + uy + ny),
                              ImVec2(x1 + ux - nx, y1 + uy - ny), ImVec2(x0 - ux - nx, y0 - uy - ny), col);
        }

        // ColorWheel: the texture, then the marker circle at (cos h, sin h) * r * s from the centre
        void wheel(Picker& p, ImGuiWindow* window, int x, int y, int w)
        {
            ImGuiContext& g = *GImGui;
            const ImGuiID id = window->GetID("##wheel");
            const ImRect bb(ImVec2(static_cast<float>(x), static_cast<float>(y)),
                            ImVec2(static_cast<float>(x + w), static_cast<float>(y + w)));
            const int r = w / 2;
            // (the toolkit's wheel takes the mouse in its parent's client space less r: its own 2 px offset is left in)
            const int mx = static_cast<int>(std::floor(g.IO.MousePos.x)) - (x - 2) - r;
            const int my = static_cast<int>(std::floor(g.IO.MousePos.y)) - (y - 2) - r;
            ImGui::ItemAdd(bb, id, nullptr, ImGuiItemFlags_NoNav);
            bool hovered = false, held = false;
            // only the disc is hot; once pressed the position is followed outside it
            if (mx * mx + my * my < r * r || g.ActiveId == id)
                ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
            if (hovered || held) // dc_crosshair over the disc and while it is pressed
                SetMouseCursor(Cursor::Crosshair);
            if (held)
            {
                float hue, sat;
                wheel_to_hue_sat(mx, my, r, hue, sat);
                p.set_hue_sat(hue, sat, p.val);
            }
            ImDrawList* dl = window->DrawList;
            dl->AddImage(wheel_texture()->GetTexRef(), bb.Min, bb.Max, ImVec2(0, 0), ImVec2(1, 1),
                         u32(ImVec4(1, 1, 1, 1)));
            const float half = static_cast<float>(w / 2);
            const float ang = p.hue * k_deg2_rad;
            const int cx = x + w / 2 + static_cast<int>(std::cos(static_cast<double>(ang)) * static_cast<double>(half)
                                                            * static_cast<double>(p.sat));
            const int cy = y + w / 2 + static_cast<int>(std::sin(static_cast<double>(ang)) * static_cast<double>(half)
                                                            * static_cast<double>(p.sat));
            OutlineCircle(dl, cx, cy, 3, 360, ImVec4(0, 0, 0, 1));
        }

        // ColorSlider: the checkerboard, one column per pixel of the channel's colour, the outline, the marker
        void slider(Picker& p, ImGuiWindow* window, int channel, int x, int y, int w, int h)
        {
            ImGuiContext& g = *GImGui;
            const ImGuiID id = window->GetID(channel + 1000);
            const ImRect bb(ImVec2(static_cast<float>(x), static_cast<float>(y)),
                            ImVec2(static_cast<float>(x + w), static_cast<float>(y + h)));
            ImGui::ItemAdd(bb, id, nullptr, ImGuiItemFlags_NoNav);
            bool hovered = false, held = false;
            ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
            if (held)
                p.set_channel(channel,
                              std::clamp((std::floor(g.IO.MousePos.x) - static_cast<float>(x)) / static_cast<float>(w),
                                         0.f, 1.f));
            ImDrawList* dl = window->DrawList;
            for (int cy = 0; cy < h; cy += 8)
                for (int cx = 0; cx < w; cx += 8)
                {
                    const bool grey = (((cx / 8) ^ (cy / 8)) & 1) != 0;
                    const float k = grey ? 191.f / 255.f : 1.f;
                    FillRect(dl, x + cx, y + cy, x + std::min(cx + 8, w), y + std::min(cy + 8, h), ImVec4(k, k, k, 1));
                }
            for (int i = 0; i < w; ++i)
                FillRect(dl, x + i, y, x + i + 1, y + h,
                         to_vec4(p.channel_color(channel, static_cast<float>(i + 1) / static_cast<float>(w))));
            OutlineRect(dl, x, y, x + w, y + h, tc(hovered || held ? TC_BORDER_ALT : TC_BORDER));
            const Rgba cur = p.color();
            float mx = p.channel_value(channel) * static_cast<float>(w) + static_cast<float>(x);
            mx = std::fmin(std::fmax(mx, static_cast<float>(x + 1)), static_cast<float>(x + w - 1));
            const int lx = static_cast<int>(mx);
            Line(dl, lx, y + 2, lx, y + h - 2, to_vec4({255 - cur.r, 255 - cur.g, 255 - cur.b, 255}));
        }

        // false once closed (Submit or the close button)
        bool draw(Picker& p)
        {
            if (p.hidden)
                return true;
            ImGuiIO& io = ImGui::GetIO();
            if (p.center)
            {
                ImGui::SetNextWindowPos(ImVec2(std::floor((io.DisplaySize.x - k_width) / 2.f),
                                               std::floor((io.DisplaySize.y - k_height) / 2.f)));
                ImGui::SetNextWindowFocus();
                p.center = false;
            }
            ImGui::SetNextWindowSize(ImVec2(k_width, k_height));
            bool open = true;
            if (Begin(p.title.c_str(), &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
            {
                ImGuiWindow* window = ImGui::GetCurrentWindow();
                const int x = static_cast<int>(window->Pos.x), y = static_cast<int>(window->Pos.y);
                // decl's layout: the wheel at (3, 18), the controls column from x + 180, 192 px wide
                wheel(p, window, x + 3, y + 18, 175);
                const int cx = x + 180;
                static const char* const k_names[] = {"R:", "G:", "B:", "H:", "S:", "V:", "A:"};
                int* const values[] = {&p.r, &p.g, &p.b, &p.h, &p.s, &p.v, &p.a};
                static const int k_max[] = {255, 255, 255, 359, 100, 100, 255};
                const int rows = p.alpha ? 7 : 6;
                for (int i = 0; i < rows; ++i)
                {
                    const int ry = y + 18 + i * 18;
                    ImGui::SetCursorScreenPos(ImVec2(static_cast<float>(cx), static_cast<float>(ry)));
                    Label(k_names[i], 1.f, 16.f);
                    slider(p, window, i == 6 ? 6 : i, cx + 18, ry, 124, 16);
                    ImGui::SetCursorScreenPos(ImVec2(static_cast<float>(cx + 144), static_cast<float>(ry)));
                    ImGui::PushID(i);
                    ImGui::SetNextItemWidth(48.f);
                    if (InputInt("##value", values[i], 1, 100))
                    {
                        *values[i] = std::clamp(*values[i], 0, k_max[i]);
                        p.channel_changed(i);
                    }
                    ImGui::PopID();
                }
                const int hy = y + 18 + 7 * 18 - (p.alpha ? 0 : 18);
                ImGui::SetCursorScreenPos(ImVec2(static_cast<float>(cx), static_cast<float>(hy)));
                Label("HEX:", 1.f, 32.f);
                ImGui::SetCursorScreenPos(ImVec2(static_cast<float>(cx + 34), static_cast<float>(hy)));
                ImGui::SetNextItemWidth(108.f);
                InputText("##hex", p.hex, p.alpha ? 9 : 7, ImGuiInputTextFlags_CharsHexadecimal);
                if (ImGui::IsItemDeactivated())
                    p.commit_hex();
                ImGui::SetCursorScreenPos(ImVec2(static_cast<float>(cx + 144), static_cast<float>(hy)));
                if (Button("Reset", ImVec2(48.f, 0.f)))
                    p.set_color(p.def);
                // the palette (two rows of nine 14 x 14 swatches) and Submit filling the corner
                const int py = y + 18 + 8 * 18;
                for (int i = 0; i < 18; ++i)
                {
                    ImGui::SetCursorScreenPos(
                            ImVec2(static_cast<float>(cx + (i / 2) * 16), static_cast<float>(py + (i % 2) * 16)));
                    ImGui::PushID(i);
                    if (ColorButton("##palette", ImGui::ColorConvertU32ToFloat4(k_palette[static_cast<std::size_t>(i)]),
                                    0, ImVec2(14.f, 14.f)))
                        p.set_color(from_u32(k_palette[static_cast<std::size_t>(i)]));
                    ImGui::PopID();
                }
                ImGui::SetCursorScreenPos(ImVec2(static_cast<float>(cx + 144), static_cast<float>(py)));
                if (Button("Submit", ImVec2(48.f, static_cast<float>(y + 192 - py))))
                {
                    if (p.target)
                        *p.target = to_u32(p.color());
                    open = false;
                }
            }
            End();
            return open;
        }

    } // namespace

    void OutlineCircle(ImDrawList* dl, int x, int y, int radius, int segments, const ImVec4& col)
    {
        if (col.w <= 0.f || radius <= 0 || segments < 3)
            return;
        const ImU32 c = u32(col);
        const float cx = static_cast<float>(x) + 0.5f, cy = static_cast<float>(y) + 0.5f,
                    fr = static_cast<float>(radius);
        float px = cx + fr, py = cy;
        for (int i = 1; i <= segments; ++i)
        {
            const float ang = 6.28318531f * static_cast<float>(i) / static_cast<float>(segments);
            const float nx = cx + fr * std::cos(ang), ny = cy + fr * std::sin(ang);
            segment(dl, px, py, nx, ny, c);
            px = nx;
            py = ny;
        }
    }

    void OpenColorPicker(const char* title, ImU32* col, ImU32 default_col, bool alpha, int group)
    {
        if (group != 0) // one of a group at a time: the others close without their edits
            pickers().erase(std::remove_if(pickers().begin(), pickers().end(),
                                           [&](const Picker& p)
                                           {
                                               return p.group == group && p.title != title;
                                           }),
                            pickers().end());
        for (Picker& p : pickers())
            if (p.title == title)
            {
                p.target = col;
                p.original = from_u32(*col);
                p.def = from_u32(default_col);
                if (group != 0) // the toolkit opens it again on the colour
                    p.set_color(p.original);
                else // re-pointed: it shows the default colour (SettingsWindow::open_theme_color_picker)
                    p.set_color(p.def);
                ImGui::SetWindowFocus((std::string("##") + title).c_str());
                return;
            }
        Picker p;
        p.title = title;
        p.target = col;
        p.group = group;
        p.alpha = alpha;
        p.original = from_u32(*col);
        p.def = from_u32(default_col);
        p.set_color(alpha ? p.original : Rgba{p.original.r, p.original.g, p.original.b, 255});
        pickers().push_back(p);
    }

    void ShowColorPickers()
    {
        for (std::size_t i = 0; i < pickers().size();)
            if (draw(pickers()[i]))
                ++i;
            else
            {
                pickers().erase(pickers().begin() + static_cast<std::ptrdiff_t>(i));
                ImGui::FocusWindow(nullptr); // (the toolkit's closed picker leaves no active window)
            }
    }

    bool IsColorPickerOpen(const char* title)
    {
        return std::any_of(pickers().begin(), pickers().end(),
                           [&](const Picker& p)
                           {
                               return p.title == title;
                           });
    }

    int GetColorPickerCount()
    {
        return static_cast<int>(pickers().size());
    }

    const char* GetColorPickerTitle(int index)
    {
        return index >= 0 && index < GetColorPickerCount() ? pickers()[static_cast<std::size_t>(index)].title.c_str()
                                                           : nullptr;
    }

    void SetColorPickerHidden(const char* title, bool hidden)
    {
        for (Picker& p : pickers())
            if (p.title == title)
                p.hidden = hidden;
    }

    bool IsColorPickerHidden(const char* title)
    {
        return std::any_of(pickers().begin(), pickers().end(),
                           [&](const Picker& p)
                           {
                               return p.title == title && p.hidden;
                           });
    }

    void CloseColorPicker(const char* title)
    {
        std::erase_if(pickers(),
                      [&](const Picker& p)
                      {
                          return p.title == title;
                      }); // (as its close button: no edit)
    }

    namespace detail
    {
        void shutdown_color_pickers()
        {
            pickers().clear();
            free_texture(wheel());
        }
    } // namespace detail

} // namespace imgui_nullcore
