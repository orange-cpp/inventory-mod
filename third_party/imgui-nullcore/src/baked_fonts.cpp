// The baked glyphs as an ImGui font loader (see gdi_fonts.hpp): the data of baked_fonts.inc, which the bake tool
// (tools/bake_fonts.cpp) makes on Windows from the GDI loader's own glyph images.
//
// The data, little-endian:
//   "NCBF", u8 version (1), u8 palette size, the palette (RGBA, 4 bytes each), u8 font count, then per font:
//     char family[16], u16 weight, u8 size (the line height), u8 outline, u8 shadow, u8 antialias, i16 ascent,
//     i16 descent, u16 glyph count, then per glyph: u16 code point, i16 advance, i8 x0, i8 y0, u8 w, u8 h
//   then u8 bits per pixel (2 or 8), u32 byte count and the glyphs' pixels (every font's glyphs in order, w * h palette
//   indices each), packed from the low bits of each byte.

#include "gdi_fonts.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <imgui_internal.h>

namespace imgui_nullcore::detail
{

    namespace
    {

#include "baked_fonts.inc"

        // reads the data, stops (empty) at anything out of place
        struct Reader
        {
            const std::uint8_t* at;
            const std::uint8_t* end;
            bool ok = true;

            std::uint32_t uint(int bytes)
            {
                if (end - at < bytes)
                {
                    ok = false;
                    return 0;
                }
                std::uint32_t v = 0;
                for (int i = 0; i < bytes; ++i)
                    v |= static_cast<std::uint32_t>(at[i]) << (8 * i);
                at += bytes;
                return v;
            }
            std::int32_t sint(int bytes)
            {
                const std::uint32_t v = uint(bytes);
                const std::uint32_t sign = 1u << (8 * bytes - 1);
                return static_cast<std::int32_t>(v ^ sign) - static_cast<std::int32_t>(sign);
            }
        };

        BakedFonts decode(const std::uint8_t* data, std::size_t size)
        {
            BakedFonts out;
            Reader r{data, data + size};
            if (size < 4 || std::memcmp(data, "NCBF", 4) != 0)
                return out;
            r.at += 4;
            if (r.uint(1) != 1)
                return out;
            const std::uint32_t colours = r.uint(1);
            for (std::uint32_t i = 0; i < colours; ++i)
                out.palette.push_back(r.uint(4));
            const std::uint32_t fonts = r.uint(1);
            std::uint32_t pixels = 0;
            for (std::uint32_t f = 0; f < fonts && r.ok; ++f)
            {
                BakedFont font;
                for (char& c : font.family)
                    c = static_cast<char>(r.uint(1));
                font.family[sizeof(font.family) - 1] = 0;
                font.weight = static_cast<int>(r.uint(2));
                font.size = static_cast<int>(r.uint(1));
                font.outline = static_cast<int>(r.uint(1));
                font.shadow = static_cast<int>(r.uint(1));
                font.antialias = r.uint(1) != 0;
                font.metrics.ascent = r.sint(2);
                font.metrics.descent = r.sint(2);
                const std::uint32_t glyphs = r.uint(2);
                for (std::uint32_t i = 0; i < glyphs && r.ok; ++i)
                {
                    BakedGlyph g;
                    g.codepoint = static_cast<std::uint16_t>(r.uint(2));
                    g.advance = static_cast<std::int16_t>(r.sint(2));
                    g.x0 = static_cast<std::int8_t>(r.sint(1));
                    g.y0 = static_cast<std::int8_t>(r.sint(1));
                    g.w = static_cast<std::uint8_t>(r.uint(1));
                    g.h = static_cast<std::uint8_t>(r.uint(1));
                    g.pixels = pixels;
                    pixels += static_cast<std::uint32_t>(g.w) * g.h;
                    font.glyphs.push_back(g);
                }
                std::sort(font.glyphs.begin(), font.glyphs.end(),
                          [](const BakedGlyph& a, const BakedGlyph& b)
                          {
                              return a.codepoint < b.codepoint;
                          });
                out.fonts.push_back(std::move(font));
            }
            const std::uint32_t bits = r.uint(1);
            const std::uint32_t packed = r.uint(4);
            if (!r.ok || (bits != 2 && bits != 8) || static_cast<std::size_t>(r.end - r.at) < packed
                || static_cast<std::size_t>(packed) * 8u < static_cast<std::size_t>(pixels) * bits)
                return {};
            out.pixels.resize(pixels);
            const std::uint32_t mask = (1u << bits) - 1u;
            for (std::size_t i = 0; i < pixels; ++i)
            {
                const std::size_t bit = i * bits;
                out.pixels[i] = static_cast<std::uint8_t>((r.at[bit / 8] >> (bit % 8)) & mask);
            }
            for (const std::uint8_t index : out.pixels)
                if (index >= out.palette.size())
                    return {};
            return out;
        }

        bool same_family(const char* baked, const wchar_t* family)
        {
            for (; *baked || *family; ++baked, ++family)
            {
                const auto a = static_cast<unsigned char>(*baked);
                const wchar_t b = *family;
                if (b > 0x7F || std::tolower(a) != std::tolower(static_cast<int>(b)))
                    return false;
            }
            return true;
        }

        const GdiFontData& spec_of(const ImFontConfig* src)
        {
            return *static_cast<const GdiFontData*>(src->FontData);
        }

        const BakedFont*& font_of(void* loader_data)
        {
            return *static_cast<const BakedFont**>(loader_data);
        }

        bool src_init(ImFontAtlas*, ImFontConfig* src)
        {
            return src->FontData && src->FontDataSize >= static_cast<int>(sizeof(GdiFontData));
        }

        // every code point: its glyph, or the replacement glyph (as the GDI loader's missing-glyph box)
        bool src_contains_glyph(ImFontAtlas*, ImFontConfig*, ImWchar)
        {
            return true;
        }

        bool baked_init(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void* loader_data)
        {
            const BakedFont* font = find_baked_font(spec_of(src), static_cast<int>(std::lround(baked->Size)));
            font_of(loader_data) = font;
            if (font && !src->MergeMode)
            {
                baked->Ascent = static_cast<float>(font->metrics.ascent);
                baked->Descent = -static_cast<float>(font->metrics.descent);
            }
            return font != nullptr;
        }

        bool baked_load_glyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loader_data,
                              ImWchar codepoint, ImFontGlyph* out_glyph, float* out_advance_x)
        {
            const BakedFont* font = font_of(loader_data);
            if (!font)
                return false;
            const BakedGlyph* g = font->find(static_cast<char32_t>(codepoint));
            if (!g)
                g = font->find(U'\uFFFD');
            if (!g)
                return false;
            if (out_advance_x)
            {
                *out_advance_x = static_cast<float>(g->advance);
                return true;
            }
            return add_glyph_image(atlas, src, baked, out_glyph, codepoint, baked_fonts().image(*g));
        }

    } // namespace

    const BakedGlyph* BakedFont::find(char32_t codepoint) const
    {
        const auto it = std::lower_bound(glyphs.begin(), glyphs.end(), codepoint,
                                         [](const BakedGlyph& g, char32_t cp)
                                         {
                                             return g.codepoint < cp;
                                         });
        return it != glyphs.end() && it->codepoint == codepoint ? &*it : nullptr;
    }

    GlyphImage BakedFonts::image(const BakedGlyph& g) const
    {
        GlyphImage out;
        out.advance = g.advance;
        if (g.w == 0 || g.h == 0)
            return out;
        out.x0 = g.x0;
        out.y0 = g.y0;
        out.w = g.w;
        out.h = g.h;
        const std::size_t n = static_cast<std::size_t>(g.w) * g.h;
        out.rgba.resize(n * 4u);
        for (std::size_t i = 0; i < n; ++i)
        {
            const std::uint32_t c = palette[pixels[g.pixels + i]];
            for (int k = 0; k < 4; ++k)
                out.rgba[i * 4u + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(c >> (8 * k));
        }
        return out;
    }

    const BakedFonts& baked_fonts()
    {
        static const BakedFonts s_fonts = decode(k_baked_fonts, sizeof(k_baked_fonts));
        return s_fonts;
    }

    const BakedFont* find_baked_font(const GdiFontData& spec, int size)
    {
        const BakedFont* best = nullptr;
        for (const BakedFont& f : baked_fonts().fonts)
        {
            if (!same_family(f.family, spec.family) || f.weight != spec.weight || f.outline != spec.outline
                || f.shadow != spec.shadow || f.antialias != spec.antialias)
                continue;
            if (!best || std::abs(f.size - size) < std::abs(best->size - size))
                best = &f;
        }
        return best;
    }

    const ImFontLoader* baked_font_loader()
    {
        static const ImFontLoader loader = []
        {
            ImFontLoader l;
            l.Name = "nullcore baked";
            l.FontSrcInit = src_init;
            l.FontSrcContainsGlyph = src_contains_glyph;
            l.FontBakedInit = baked_init;
            l.FontBakedLoadGlyph = baked_load_glyph;
            l.FontBakedSrcLoaderDataSize = sizeof(const BakedFont*);
            return l;
        }();
        return &loader;
    }

    bool add_glyph_image(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, ImFontGlyph* glyph,
                         ImWchar codepoint, const GlyphImage& image)
    {
        glyph->Codepoint = codepoint;
        glyph->AdvanceX = static_cast<float>(image.advance);
        if (image.w <= 0 || image.h <= 0)
            return true; // a space
        const ImFontAtlasRectId pack_id = ImFontAtlasPackAddRect(atlas, image.w, image.h);
        if (pack_id == ImFontAtlasRectId_Invalid)
            return false;
        ImTextureRect* r = ImFontAtlasPackGetRect(atlas, pack_id);
        glyph->X0 = static_cast<float>(image.x0);
        glyph->Y0 = static_cast<float>(image.y0);
        glyph->X1 = glyph->X0 + static_cast<float>(image.w);
        glyph->Y1 = glyph->Y0 + static_cast<float>(image.h);
        glyph->Visible = true;
        glyph->PackId = pack_id;
        ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, glyph, r, image.rgba.data(), ImTextureFormat_RGBA32,
                                           image.w * 4);
        return true;
    }

} // namespace imgui_nullcore::detail
