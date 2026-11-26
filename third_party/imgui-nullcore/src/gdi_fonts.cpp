// nullcore's glyphs as an ImGui font loader from GDI (see gdi_fonts.hpp).  The rasterising is gdi_glyphs.cpp's and the
// effects glyph_atlas.cpp's, kept step for step so the pixels are the same.

#include "gdi_fonts.hpp"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <imgui_internal.h>
#include <memory>
#include <usp10.h>
#include <vector>
#include <windows.h>

namespace imgui_nullcore::detail
{

    namespace
    {

        // A font the glyphs come from: the font's own or a fallback family.
        struct Face
        {
            HFONT handle = nullptr;
            int ascent = 0, descent = 0; // TEXTMETRIC tmAscent / tmDescent
            SCRIPT_CACHE cache = nullptr; // Uniscribe's (code points past U+FFFF)
        };

        // One baked size of a font: the GDI font at its cell height.
        struct Baked
        {
            Face face;
            int tall = 0, weight = 0;
            int height = 0; // TEXTMETRIC tmHeight
            bool antialias = false;
            int outline = 0, shadow = 0;
            std::vector<Face> fallbacks; // made on the first code point the face lacks
            bool fallbacks_made = false;
        };

        // A glyph as GDI makes it: 8-bit coverage and its place from the pen and the cell's top.
        struct GlyphBitmap
        {
            int w = 0, h = 0;
            int x = 0, y = 0;
            int advance = 0;
            std::vector<std::uint8_t> coverage;
        };

        // What GDI is asked to draw: a UTF-16 unit, or (past U+FFFF) a glyph index of the face.
        struct GlyphCode
        {
            UINT code = 0;
            bool by_index = false;
        };

        // Tahoma first: vgui's own fallback (gdi_glyphs.cpp's list).
        constexpr std::array k_fallback_families = {
                L"Tahoma",       L"Segoe UI",         L"Segoe UI Symbol", L"Segoe UI Emoji",  L"Segoe UI Historic",
                L"Nirmala UI",   L"Microsoft YaHei",  L"Yu Gothic",       L"Malgun Gothic",   L"Microsoft JhengHei",
                L"Ebrima",       L"Gadugi",           L"Leelawadee UI",   L"Mongolian Baiti", L"Microsoft Himalaya",
                L"Cambria Math", L"Arial Unicode MS", L"MS Gothic",       L"SimSun",
        };

        HDC dc()
        {
            static HDC s_dc = CreateCompatibleDC(nullptr); // the process's, like the toolkit's one per rasteriser
            return s_dc;
        }

        bool zero_width(char32_t cp)
        {
            return (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064)
                   || cp == 0xFEFF || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0000 && cp <= 0xE01EF);
        }

        void read_metrics(HDC hdc, Face& face)
        {
            SelectObject(hdc, face.handle);
            TEXTMETRICW tm = {};
            if (GetTextMetricsW(hdc, &tm))
            {
                face.ascent = tm.tmAscent;
                face.descent = tm.tmDescent;
            }
        }

        void release(Face& face)
        {
            if (face.handle)
                DeleteObject(face.handle);
            if (face.cache)
                ScriptFreeCache(&face.cache);
            face = {};
        }

        HFONT create_font(int tall, int weight, bool antialias, const wchar_t* family)
        {
            return CreateFontW(tall, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, antialias ? ANTIALIASED_QUALITY : NONANTIALIASED_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, family);
        }

        bool face_glyph(HDC hdc, Face& face, char32_t cp, GlyphCode& out)
        {
            if (cp <= 0xFFFF)
            {
                const auto ch = static_cast<wchar_t>(cp);
                WORD index = 0;
                const DWORD r = GetGlyphIndicesW(hdc, &ch, 1, &index, GGI_MARK_NONEXISTING_GLYPHS);
                if (r != GDI_ERROR && index == 0xFFFF)
                    return false;
                out = {static_cast<UINT>(ch), false};
                return true;
            }
            const wchar_t pair[2] = {static_cast<wchar_t>(0xD800 + ((cp - 0x10000) >> 10)),
                                     static_cast<wchar_t>(0xDC00 + ((cp - 0x10000) & 0x3FF))};
            std::array<SCRIPT_ITEM, 3> items = {};
            int item_count = 0;
            if (ScriptItemize(pair, 2, static_cast<int>(items.size()) - 1, nullptr, nullptr, items.data(), &item_count)
                != S_OK)
                return false;
            std::array<WORD, 4> glyphs = {};
            std::array<WORD, 2> clusters = {};
            std::array<SCRIPT_VISATTR, 4> attrs = {};
            int glyph_count = 0;
            if (ScriptShape(hdc, &face.cache, pair, 2, static_cast<int>(glyphs.size()), &items[0].a, glyphs.data(),
                            clusters.data(), attrs.data(), &glyph_count)
                != S_OK)
                return false;
            SCRIPT_FONTPROPERTIES props = {};
            props.cBytes = sizeof(props);
            if (glyph_count != 1 || ScriptGetFontProperties(hdc, &face.cache, &props) != S_OK
                || glyphs[0] == props.wgDefault)
                return false;
            out = {glyphs[0], true};
            return true;
        }

        // CWin32Font::GetCharRGBA's antialiased branch (GGO_GRAY8_BITMAP).
        bool gray_glyph(HDC hdc, int ascent, const ABC& abc, const Baked& b, GlyphCode code, GlyphBitmap& out)
        {
            static constexpr MAT2 k_identity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
            const UINT format = GGO_GRAY8_BITMAP | (code.by_index ? GGO_GLYPH_INDEX : 0u);
            GLYPHMETRICS gm = {};
            const DWORD size = GetGlyphOutlineW(hdc, code.code, format, &gm, 0, nullptr, &k_identity);
            if (size == GDI_ERROR || size == 0)
                return false;
            std::vector<std::uint8_t> raw(size);
            if (GetGlyphOutlineW(hdc, code.code, format, &gm, size, raw.data(), &k_identity) == GDI_ERROR)
                return false;
            const int box_w = static_cast<int>(gm.gmBlackBoxX), box_h = static_cast<int>(gm.gmBlackBoxY);
            const std::size_t pitch = (static_cast<std::size_t>(box_w) + 3u) & ~std::size_t{3};
            const int cell_b = static_cast<int>(abc.abcB) + 2 * b.outline + b.shadow;
            const int x_start = box_w >= cell_b + 2 ? (box_w - cell_b) / 2 : 0;
            const int w = std::min(box_w - x_start, static_cast<int>(abc.abcB) + b.outline + b.shadow);
            if (w <= 0 || box_h <= 0)
                return false;
            out.w = w;
            out.h = box_h;
            out.x = abc.abcA;
            out.y = ascent - gm.gmptGlyphOrigin.y;
            out.coverage.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(box_h), 0);
            for (int y = 0; y < box_h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const std::size_t at = static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x + x_start);
                    const unsigned level = at < raw.size() ? raw[at] : 0u;
                    out.coverage[static_cast<std::size_t>(y) * static_cast<std::size_t>(w)
                                 + static_cast<std::size_t>(x)] =
                            static_cast<std::uint8_t>(std::min(level, 64u) * 255u / 64u);
                }
            return true;
        }

        // CWin32Font::GetCharRGBA's other branch: white on black with ExtTextOut, the alpha from the luminance, the
        // cell's first `outline` rows left out, trimmed to the drawn pixels.
        void drawn_glyph(HDC hdc, const Baked& b, const Face& face, const ABC& abc, GlyphCode code, GlyphBitmap& out)
        {
            const int ascent = b.face.ascent;
            const int top = std::min(0, ascent - face.ascent);
            const int bottom = std::max(b.height, ascent + face.descent);
            const int w = static_cast<int>(abc.abcB), h = bottom - top;
            if (w <= 0 || h <= 0)
                return;
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h; // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bmp || !bits)
            {
                if (bmp)
                    DeleteObject(bmp);
                return;
            }
            const HGDIOBJ old = SelectObject(hdc, bmp);
            SetTextAlign(hdc, TA_LEFT | TA_BASELINE | TA_NOUPDATECP);
            SetBkColor(hdc, RGB(0, 0, 0));
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkMode(hdc, OPAQUE);
            const RECT r = {0, 0, w, h};
            const auto unit = static_cast<wchar_t>(code.code);
            ExtTextOutW(hdc, -abc.abcA, ascent - top, ETO_OPAQUE | (code.by_index ? ETO_GLYPH_INDEX : 0u), &r, &unit, 1,
                        nullptr);
            GdiFlush();

            const auto* px = static_cast<const std::uint8_t*>(bits); // B, G, R, x
            std::vector<std::uint8_t> alpha(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
            int x0 = w, y0 = h, x1 = 0, y1 = 0;
            for (int y = std::max(b.outline - top, 0); y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const std::uint8_t* p =
                            px
                            + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x))
                                      * 4u;
                    const auto a = static_cast<std::uint8_t>(static_cast<float>(p[2]) * 0.34f
                                                             + static_cast<float>(p[1]) * 0.55f
                                                             + static_cast<float>(p[0]) * 0.11f);
                    if (!a)
                        continue;
                    alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = a;
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x + 1);
                    y1 = std::max(y1, y + 1);
                }
            SelectObject(hdc, old);
            DeleteObject(bmp);
            if (x1 <= x0 || y1 <= y0)
                return; // nothing drawn (a space)
            out.w = x1 - x0;
            out.h = y1 - y0;
            out.x = abc.abcA + x0;
            out.y = top + y0;
            out.coverage.resize(static_cast<std::size_t>(out.w) * static_cast<std::size_t>(out.h));
            for (int y = 0; y < out.h; ++y)
                std::copy_n(&alpha[static_cast<std::size_t>(y + y0) * static_cast<std::size_t>(w)
                                   + static_cast<std::size_t>(x0)],
                            out.w, &out.coverage[static_cast<std::size_t>(y) * static_cast<std::size_t>(out.w)]);
        }

        void make_fallbacks(Baked& b)
        {
            b.fallbacks_made = true;
            HDC hdc = dc();
            for (const wchar_t* family : k_fallback_families)
            {
                Face face;
                face.handle = create_font(b.tall, b.weight, b.antialias, family);
                if (!face.handle)
                    continue;
                read_metrics(hdc, face);
                std::array<wchar_t, LF_FACESIZE> name = {};
                if (!GetTextFaceW(hdc, static_cast<int>(name.size()), name.data())
                    || _wcsicmp(name.data(), family) != 0)
                {
                    release(face);
                    continue;
                }
                b.fallbacks.push_back(face);
            }
        }

        // gdi_glyphs.cpp's GdiGlyphs::glyph: false when GDI cannot answer at all.
        bool rasterise(Baked& b, char32_t cp, GlyphBitmap& out)
        {
            HDC hdc = dc();
            if (!b.face.handle || !hdc)
                return false;
            out = {};
            if (zero_width(cp))
                return true;
            Face* face = &b.face;
            GlyphCode code = {static_cast<UINT>(cp), false};
            SelectObject(hdc, b.face.handle);
            if (cp != U'\t' && !face_glyph(hdc, b.face, cp, code))
            {
                if (!b.fallbacks_made)
                    make_fallbacks(b);
                face = nullptr;
                for (Face& fallback : b.fallbacks)
                {
                    SelectObject(hdc, fallback.handle);
                    if (face_glyph(hdc, fallback, cp, code))
                    {
                        face = &fallback;
                        break;
                    }
                }
                if (!face)
                {
                    face = &b.face;
                    SelectObject(hdc, b.face.handle);
                    code = {static_cast<UINT>(cp <= 0xFFFF ? cp : 0xFFFD), false};
                }
            }
            ABC abc = {};
            const auto unit = static_cast<wchar_t>(code.code);
            if (!(code.by_index ? GetCharABCWidthsI(hdc, code.code, 1, nullptr, &abc)
                                : GetCharABCWidthsW(hdc, code.code, code.code, &abc)))
            {
                SIZE sz = {};
                WORD index = static_cast<WORD>(code.code);
                if (!(code.by_index ? GetTextExtentPointI(hdc, &index, 1, &sz)
                                    : GetTextExtentPoint32W(hdc, &unit, 1, &sz)))
                    return false;
                abc = {0, static_cast<UINT>(sz.cx), 0};
            }
            out.advance = abc.abcA + static_cast<int>(abc.abcB) + abc.abcC;
            if (cp == U'\t')
                return true;
            if (face == &b.face && b.antialias && cp <= 0xFF && gray_glyph(hdc, b.face.ascent, abc, b, code, out))
                return true;
            drawn_glyph(hdc, b, *face, abc, code, out);
            return true;
        }

        // glyph_atlas.cpp's compose(): white glyph, black drop shadow, then a black outline on the empty pixels next
        // to a glyph pixel; the first `top` rows left out.
        std::vector<std::uint8_t> compose(const GlyphBitmap& g, int o, int s, int top, int& out_w, int& out_h)
        {
            const int w = g.w + 2 * o + s, h = g.h + 2 * o + s;
            std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0);
            auto px = [&](int x, int y)
            {
                return &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x))
                             * 4u];
            };
            for (int y = 0; y < g.h; ++y)
                for (int x = 0; x < g.w; ++x)
                    if (const std::uint8_t c = g.coverage[static_cast<std::size_t>(y) * static_cast<std::size_t>(g.w)
                                                          + static_cast<std::size_t>(x)])
                    {
                        std::uint8_t* p = px(x + o, y + o);
                        p[0] = p[1] = p[2] = 255;
                        p[3] = c;
                    }
            if (s > 0)
                for (int y = h - 1; y >= s; --y)
                    for (int x = w - 1; x >= s; --x)
                        if (std::uint8_t* p = px(x, y); p[3] == 0)
                            p[3] = px(x - s, y - s)[3];
            if (o > 0)
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        std::uint8_t* p = px(x, y);
                        if (p[3] != 0)
                            continue;
                        bool touches = false;
                        for (int dy = -o; dy <= o && !touches; ++dy)
                            for (int dx = -o; dx <= o && !touches; ++dx)
                            {
                                const int tx = x + dx, ty = y + dy;
                                touches = tx >= 0 && ty >= 0 && tx < w && ty < h && px(tx, ty)[0] != 0;
                            }
                        if (touches)
                            p[3] = 255;
                    }
            top = std::clamp(top, 0, h);
            out_w = w;
            out_h = h - top;
            rgba.erase(rgba.begin(), rgba.begin() + static_cast<std::ptrdiff_t>(top) * w * 4);
            return rgba;
        }

        const GdiFontData& spec_of(const ImFontConfig* src)
        {
            return *static_cast<const GdiFontData*>(src->FontData);
        }

        Baked*& baked_of(void* loader_data)
        {
            return *static_cast<Baked**>(loader_data);
        }

        bool src_init(ImFontAtlas*, ImFontConfig* src)
        {
            return src->FontData && src->FontDataSize >= static_cast<int>(sizeof(GdiFontData));
        }

        // Every code point: the face's glyph, a fallback family's, or the face's missing-glyph box (as the toolkit).
        bool src_contains_glyph(ImFontAtlas*, ImFontConfig*, ImWchar)
        {
            return true;
        }

        // A size of the font: the GDI font at the cell height the line height leaves.
        Baked* make_baked(const GdiFontData& spec, int size)
        {
            auto* b = new Baked;
            b->weight = spec.weight;
            b->antialias = spec.antialias;
            b->outline = spec.outline;
            b->shadow = spec.shadow;
            // the ImGui size is the line height: the cell + the effects (glyph_atlas.cpp's line_height)
            b->tall = std::max(1, size - 2 * b->outline - b->shadow);
            b->face.handle = create_font(b->tall, b->weight, b->antialias, spec.family);
            if (b->face.handle && dc())
            {
                SelectObject(dc(), b->face.handle);
                TEXTMETRICW tm = {};
                if (GetTextMetricsW(dc(), &tm))
                {
                    b->height = tm.tmHeight;
                    b->face.ascent = tm.tmAscent;
                    b->face.descent = tm.tmDescent;
                }
            }
            if (b->height <= 0)
                b->height = b->tall;
            return b;
        }

        void destroy_baked(Baked* b)
        {
            release(b->face);
            for (Face& face : b->fallbacks)
                release(face);
            delete b;
        }

        GlyphMetrics metrics_of(const Baked& b)
        {
            return {b.face.ascent + b.outline, b.face.descent + b.outline + b.shadow};
        }

        // The glyph as it goes into the atlas.
        bool glyph_image(Baked& b, char32_t codepoint, GlyphImage& out)
        {
            out = {};
            GlyphBitmap bitmap;
            if (!rasterise(b, codepoint, bitmap))
                return false;
            out.advance = bitmap.advance;
            if (bitmap.w <= 0 || bitmap.h <= 0)
                return true; // a space
            const int o = b.outline;
            // the glyph keeps its row in the cell: an outline above the cell's top is cut (glyph_atlas.cpp)
            const int top = std::max(o - bitmap.y, 0);
            int w = 0, h = 0;
            out.rgba = compose(bitmap, o, b.shadow, top, w, h);
            if (w <= 0 || h <= 0)
            {
                out.rgba.clear();
                return true;
            }
            out.x0 = bitmap.x - o;
            out.y0 = bitmap.y - o + top;
            out.w = w;
            out.h = h;
            return true;
        }

        bool baked_init(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void* loader_data)
        {
            Baked* b = make_baked(spec_of(src), static_cast<int>(std::lround(baked->Size)));
            baked_of(loader_data) = b;
            if (!src->MergeMode)
            {
                const GlyphMetrics m = metrics_of(*b);
                baked->Ascent = static_cast<float>(m.ascent);
                baked->Descent = -static_cast<float>(m.descent);
            }
            return b->face.handle != nullptr;
        }

        void baked_destroy(ImFontAtlas*, ImFontConfig*, ImFontBaked*, void* loader_data)
        {
            Baked*& b = baked_of(loader_data);
            if (!b)
                return;
            destroy_baked(b);
            b = nullptr;
        }

        bool baked_load_glyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loader_data,
                              ImWchar codepoint, ImFontGlyph* out_glyph, float* out_advance_x)
        {
            Baked* b = baked_of(loader_data);
            GlyphImage image;
            if (!b || !glyph_image(*b, static_cast<char32_t>(codepoint), image))
                return false;
            if (out_advance_x)
            {
                *out_advance_x = static_cast<float>(image.advance);
                return true;
            }
            return add_glyph_image(atlas, src, baked, out_glyph, codepoint, image);
        }

        // the bake tool's and the tests' fonts: one per spec and size, kept
        struct Kept
        {
            GdiFontData spec;
            int size = 0;
            std::unique_ptr<Baked, void (*)(Baked*)> baked{nullptr, destroy_baked};
        };

        Baked* kept_baked(const GdiFontData& spec, int size)
        {
            static std::vector<Kept> s_kept;
            for (Kept& k : s_kept)
                if (k.size == size && std::memcmp(&k.spec, &spec, sizeof(spec)) == 0)
                    return k.baked.get();
            Kept k;
            k.spec = spec;
            k.size = size;
            k.baked.reset(make_baked(spec, size));
            s_kept.push_back(std::move(k));
            return s_kept.back().baked.get();
        }

    } // namespace

    const ImFontLoader* gdi_font_loader()
    {
        static const ImFontLoader loader = []
        {
            ImFontLoader l;
            l.Name = "nullcore GDI";
            l.FontSrcInit = src_init;
            l.FontSrcContainsGlyph = src_contains_glyph;
            l.FontBakedInit = baked_init;
            l.FontBakedDestroy = baked_destroy;
            l.FontBakedLoadGlyph = baked_load_glyph;
            l.FontBakedSrcLoaderDataSize = sizeof(Baked*);
            return l;
        }();
        return &loader;
    }

    bool gdi_glyph_image(const GdiFontData& spec, int size, char32_t codepoint, GlyphImage& out)
    {
        Baked* b = kept_baked(spec, size);
        return b->face.handle && glyph_image(*b, codepoint, out);
    }

    bool gdi_metrics(const GdiFontData& spec, int size, GlyphMetrics& out)
    {
        const Baked* b = kept_baked(spec, size);
        out = metrics_of(*b);
        return b->face.handle != nullptr;
    }

} // namespace imgui_nullcore::detail

#else

namespace imgui_nullcore::detail
{
    const ImFontLoader* gdi_font_loader()
    {
        return nullptr;
    }

    bool gdi_glyph_image(const GdiFontData&, int, char32_t, GlyphImage&)
    {
        return false;
    }

    bool gdi_metrics(const GdiFontData&, int, GlyphMetrics&)
    {
        return false;
    }
} // namespace imgui_nullcore::detail

#endif
