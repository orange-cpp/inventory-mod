#pragma once
// The ImGui font loaders that make nullcore's glyphs.
//
//  * gdi_font_loader (Windows): GDI's glyphs, the way the toolkit's D3D9 renderer makes them
//    (libs/nclib/source/d3d9/gdi_glyphs.cpp, which follows vgui's CWin32Font::GetCharRGBA), with the effects of its
//    glyph atlas (render/glyph_atlas.cpp: a white glyph, a black drop shadow, a hard black 1 px outline).
//  * baked_font_loader (everywhere): the same glyphs recorded by the bake tool (tools/bake_fonts.cpp) on Windows into
//    baked_fonts.inc: the toolkit's fonts (Verdana regular and bold, outlined, line heights 14 .. 22) for the code
//    points it lists; the others show the replacement glyph.
//
// The glyphs go into the atlas as RGBA, so the text colour tints the glyph and leaves the outline black.  The ImGui
// size of a font is its line height: the GDI cell height (the vgui "tall") + 2 * outline + shadow.  A size is baked on
// first use, like any ImGui 1.92 font.

#include <cstdint>
#include <imgui.h>
#include <imgui_nullcore/imgui_nullcore.hpp>
#include <vector>

namespace imgui_nullcore::detail
{

    // The spec as the font source's data (copied by the atlas: plain bytes).
    struct GdiFontData
    {
        wchar_t family[32] = {};
        int weight = 400;
        int outline = 0;
        int shadow = 0;
        bool antialias = false;
    };

    // A glyph as it goes into the atlas: its advance, its box from the pen and the line's top, its RGBA pixels (none
    // for a space).
    struct GlyphImage
    {
        int advance = 0;
        int x0 = 0, y0 = 0, w = 0, h = 0;
        std::vector<std::uint8_t> rgba;
    };

    // A font size's metrics as ImFontBaked takes them.
    struct GlyphMetrics
    {
        int ascent = 0, descent = 0; // ImFontBaked::Ascent, -ImFontBaked::Descent
    };

    // The GDI loader, or null where GDI is not available.
    const ImFontLoader* gdi_font_loader();
    // What the GDI loader makes for `spec` at line height `size` (Windows; false elsewhere or when GDI cannot answer).
    bool gdi_glyph_image(const GdiFontData& spec, int size, char32_t codepoint, GlyphImage& out);
    bool gdi_metrics(const GdiFontData& spec, int size, GlyphMetrics& out);

    // The baked fonts: one per style and size.
    struct BakedGlyph
    {
        std::uint16_t codepoint = 0;
        std::int16_t advance = 0;
        std::int8_t x0 = 0, y0 = 0;
        std::uint8_t w = 0, h = 0;
        std::uint32_t pixels = 0; // its w * h palette indices in BakedFonts::pixels
    };
    struct BakedFont
    {
        char family[16] = {};
        int weight = 400, size = 14, outline = 1, shadow = 0;
        bool antialias = false;
        GlyphMetrics metrics;
        std::vector<BakedGlyph> glyphs; // by code point
        const BakedGlyph* find(char32_t codepoint) const;
    };
    struct BakedFonts
    {
        std::vector<std::uint32_t> palette; // RGBA, R in the low byte
        std::vector<BakedFont> fonts;
        std::vector<std::uint8_t> pixels;
        GlyphImage image(const BakedGlyph& g) const;
    };
    const BakedFonts& baked_fonts(); // baked_fonts.inc, decoded on first use
    // The baked font for `spec` at line height `size` (the nearest baked size of its style), or null.
    const BakedFont* find_baked_font(const GdiFontData& spec, int size);
    const ImFontLoader* baked_font_loader();

    // Puts `image` into the atlas as `glyph` (both loaders).
    bool add_glyph_image(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, ImFontGlyph* glyph,
                         ImWchar codepoint, const GlyphImage& image);

} // namespace imgui_nullcore::detail
