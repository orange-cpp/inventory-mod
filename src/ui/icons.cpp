#include "ui/icons.hpp"

#include "core/log.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace icons
{

    namespace
    {
        // ---- VPK (version 1 and 2) --------------------------------------------------------------------------------
        //
        // The _dir.vpk holds the tree (extension, directory, name, then crc, preload bytes, archive index, offset,
        // length) and the files' data is in pak01_NNN.vpk next to it; archive 0x7FFF is the _dir file itself, after
        // the tree.  Only the paths the icons need are kept.

        struct VpkEntry
        {
            std::wstring dir_file;
            std::uint16_t archive = 0;
            std::uint32_t offset = 0;
            std::uint32_t length = 0;
            std::uint32_t data_start = 0; // where archive 0x7FFF's data begins in the _dir file
            std::string preload;
        };

        bool wanted(const std::string& path)
        {
            return path.rfind("scripts/weapon_", 0) == 0 || path.rfind("scripts/melee/", 0) == 0 ||
                   path.rfind("materials/vgui/hud/", 0) == 0;
        }

        // Adds the archive's wanted files that no earlier archive has (the first archive given wins, like the game's
        // search path: update before the DLCs before the base game).
        void index_vpk(const std::wstring& path, std::map<std::string, VpkEntry>& out)
        {
            FILE* f = _wfopen(path.c_str(), L"rb");
            if (f == nullptr)
                return;
            std::uint32_t header[3] = {};
            if (std::fread(header, 4, 3, f) != 3 || header[0] != 0x55AA1234u || (header[1] != 1 && header[1] != 2))
            {
                std::fclose(f);
                return;
            }
            const std::uint32_t header_size = header[1] == 2 ? 28 : 12;
            std::fseek(f, static_cast<long>(header_size), SEEK_SET);
            std::vector<char> tree(header[2]);
            const bool ok = std::fread(tree.data(), 1, tree.size(), f) == tree.size();
            std::fclose(f);
            if (!ok)
                return;

            std::size_t at = 0;
            const auto cstring = [&]() -> std::string {
                const std::size_t start = at;
                while (at < tree.size() && tree[at] != '\0')
                    ++at;
                std::string text(tree.data() + start, at - start);
                ++at;
                return text;
            };
            while (at < tree.size())
            {
                const std::string ext = cstring();
                if (ext.empty())
                    break;
                while (at < tree.size())
                {
                    const std::string directory = cstring();
                    if (directory.empty())
                        break;
                    while (at < tree.size())
                    {
                        const std::string name = cstring();
                        if (name.empty() || at + 18 > tree.size())
                            break;
                        std::uint32_t crc, offset, length;
                        std::uint16_t preload, archive;
                        std::memcpy(&crc, &tree[at], 4);
                        std::memcpy(&preload, &tree[at + 4], 2);
                        std::memcpy(&archive, &tree[at + 6], 2);
                        std::memcpy(&offset, &tree[at + 8], 4);
                        std::memcpy(&length, &tree[at + 12], 4);
                        at += 18;
                        std::string full = (directory == " " ? std::string() : directory + "/") + name + "." + ext;
                        for (char& c : full)
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (wanted(full) && out.find(full) == out.end() && at + preload <= tree.size())
                            out[full] = VpkEntry{path, archive, offset, length, header_size + header[2],
                                                 std::string(tree.data() + at, preload)};
                        at += preload;
                    }
                }
            }
        }

        bool read_entry(const VpkEntry& e, std::string& out)
        {
            out = e.preload;
            if (e.length == 0)
                return true;
            std::wstring file = e.dir_file;
            std::uint32_t offset = e.offset;
            if (e.archive == 0x7FFF)
                offset += e.data_start;
            else
            {
                wchar_t suffix[16];
                std::swprintf(suffix, 16, L"_%03u.vpk", e.archive);
                file.replace(file.size() - 8, 8, suffix); // "_dir.vpk"
            }
            FILE* f = _wfopen(file.c_str(), L"rb");
            if (f == nullptr)
                return false;
            std::fseek(f, static_cast<long>(offset), SEEK_SET);
            const std::size_t start = out.size();
            out.resize(start + e.length);
            const bool ok = std::fread(out.data() + start, 1, e.length, f) == e.length;
            std::fclose(f);
            return ok;
        }

        // ---- weapon scripts ------------------------------------------------------------------------------------------

        struct Rect
        {
            std::string sheet; // "vgui/hud/iconsheet"
            int x = 0, y = 0, w = 0, h = 0;
        };

        // KeyValues text as tokens: quoted strings, bare words, braces; // comments skipped.
        std::vector<std::string> tokens(const std::string& text)
        {
            std::vector<std::string> out;
            for (std::size_t i = 0; i < text.size();)
            {
                const char c = text[i];
                if (std::isspace(static_cast<unsigned char>(c)))
                    ++i;
                else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
                {
                    while (i < text.size() && text[i] != '\n')
                        ++i;
                }
                else if (c == '{' || c == '}')
                {
                    out.emplace_back(1, c);
                    ++i;
                }
                else if (c == '"')
                {
                    const std::size_t end = text.find('"', i + 1);
                    out.push_back(text.substr(i + 1, (end == std::string::npos ? text.size() : end) - i - 1));
                    i = end == std::string::npos ? text.size() : end + 1;
                }
                else
                {
                    const std::size_t start = i;
                    while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '{' &&
                           text[i] != '}' && text[i] != '"')
                        ++i;
                    out.push_back(text.substr(start, i - start));
                }
            }
            return out;
        }

        bool iequals(const std::string& a, const char* b)
        {
            return _stricmp(a.c_str(), b) == 0;
        }

        // TextureData { "<block>" { "file" .. "x" .. "y" .. "width" .. "height" .. } } of a weapon script ("weapon") or
        // of a melee script in the update archive ("sprite_active").
        bool texture_rect(const std::string& script, Rect& out, const char* block = "weapon")
        {
            const std::vector<std::string> t = tokens(script);
            for (std::size_t i = 0; i + 2 < t.size(); ++i)
            {
                if (!iequals(t[i], "TextureData") || t[i + 1] != "{")
                    continue;
                int depth = 0;
                for (std::size_t j = i + 1; j < t.size(); ++j)
                {
                    if (t[j] == "{")
                        ++depth;
                    else if (t[j] == "}")
                    {
                        if (--depth == 0)
                            return false;
                    }
                    else if (depth == 1 && iequals(t[j], block) && j + 1 < t.size() && t[j + 1] == "{")
                    {
                        for (std::size_t k = j + 2; k + 1 < t.size() && t[k] != "}"; k += 2)
                        {
                            if (iequals(t[k], "file"))
                                out.sheet = t[k + 1];
                            else if (iequals(t[k], "x"))
                                out.x = std::atoi(t[k + 1].c_str());
                            else if (iequals(t[k], "y"))
                                out.y = std::atoi(t[k + 1].c_str());
                            else if (iequals(t[k], "width"))
                                out.w = std::atoi(t[k + 1].c_str());
                            else if (iequals(t[k], "height"))
                                out.h = std::atoi(t[k + 1].c_str());
                        }
                        return !out.sheet.empty() && out.w > 0 && out.h > 0;
                    }
                }
            }
            return false;
        }

        // A melee script's (scripts/melee/<name>.txt) icon.  The base game's says "sprite_active" "icon_fireaxe": a
        // texture of its own, materials/vgui/hud/icon_fireaxe.vtf, the whole of it the icon (w / h 0: the image's
        // size).  The update archive's has TextureData { "sprite_active" { "file" "vgui/hud/icon_fireaxe" "x" ... } }.
        bool melee_rect(const std::string& script, Rect& out)
        {
            if (texture_rect(script, out, "sprite_active"))
                return true;
            const std::vector<std::string> t = tokens(script);
            for (std::size_t i = 0; i + 1 < t.size(); ++i)
                if (iequals(t[i], "sprite_active") && t[i + 1] != "{")
                {
                    out = Rect{"vgui/hud/" + t[i + 1], 0, 0, 0, 0};
                    return true;
                }
            return false;
        }

        // ---- VTF -------------------------------------------------------------------------------------------------
        //
        // Header (7.x): width +16, height +18, flags +20, frames +24, high-res format +52, mip count +56, low-res format
        // +57, low-res width/height +61/+62, header size +12; 7.3+ has a resource list at +80 (tag 0x30 = the high-res
        // data).  Mips are stored smallest first, so the full-size image is the last.

        struct Image
        {
            int width = 0, height = 0;
            std::vector<std::uint32_t> bgra; // A8R8G8B8 as D3D wants it
        };

        std::uint32_t pack(int r, int g, int b, int a)
        {
            return static_cast<std::uint32_t>(a) << 24 | static_cast<std::uint32_t>(r) << 16 |
                   static_cast<std::uint32_t>(g) << 8 | static_cast<std::uint32_t>(b);
        }

        void color565(std::uint16_t c, int& r, int& g, int& b)
        {
            r = ((c >> 11) & 31) * 255 / 31;
            g = ((c >> 5) & 63) * 255 / 63;
            b = (c & 31) * 255 / 31;
        }

        // One 4x4 block's colours (DXT1 style); `opaque`: always four colours (DXT3/5).
        void color_block(const std::uint8_t* p, bool opaque, std::uint32_t out[16], const std::uint8_t alpha[16])
        {
            const std::uint16_t c0 = static_cast<std::uint16_t>(p[0] | p[1] << 8);
            const std::uint16_t c1 = static_cast<std::uint16_t>(p[2] | p[3] << 8);
            int r[4], g[4], b[4], a[4] = {255, 255, 255, 255};
            color565(c0, r[0], g[0], b[0]);
            color565(c1, r[1], g[1], b[1]);
            if (opaque || c0 > c1)
            {
                r[2] = (2 * r[0] + r[1]) / 3, g[2] = (2 * g[0] + g[1]) / 3, b[2] = (2 * b[0] + b[1]) / 3;
                r[3] = (r[0] + 2 * r[1]) / 3, g[3] = (g[0] + 2 * g[1]) / 3, b[3] = (b[0] + 2 * b[1]) / 3;
            }
            else
            {
                r[2] = (r[0] + r[1]) / 2, g[2] = (g[0] + g[1]) / 2, b[2] = (b[0] + b[1]) / 2;
                r[3] = g[3] = b[3] = 0, a[3] = 0;
            }
            const std::uint32_t bits = static_cast<std::uint32_t>(p[4] | p[5] << 8 | p[6] << 16 | p[7] << 24);
            for (int i = 0; i < 16; ++i)
            {
                const int k = (bits >> (2 * i)) & 3;
                out[i] = pack(r[k], g[k], b[k], alpha != nullptr ? alpha[i] : a[k]);
            }
        }

        bool decode(const std::uint8_t* data, std::size_t size, int format, int w, int h, Image& out)
        {
            out.width = w;
            out.height = h;
            out.bgra.assign(static_cast<std::size_t>(w) * h, 0);
            const int bw = (w + 3) / 4, bh = (h + 3) / 4;
            const auto put_block = [&](int bx, int by, const std::uint32_t px[16]) {
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x)
                        if (bx * 4 + x < w && by * 4 + y < h)
                            out.bgra[static_cast<std::size_t>(by * 4 + y) * w + bx * 4 + x] = px[y * 4 + x];
            };
            switch (format)
            {
            case 13: // DXT1
            case 20: // DXT1 with one-bit alpha
                if (size < static_cast<std::size_t>(bw) * bh * 8)
                    return false;
                for (int by = 0; by < bh; ++by)
                    for (int bx = 0; bx < bw; ++bx)
                    {
                        std::uint32_t px[16];
                        color_block(data + (static_cast<std::size_t>(by) * bw + bx) * 8, false, px, nullptr);
                        put_block(bx, by, px);
                    }
                return true;
            case 14: // DXT3
            case 15: // DXT5
                if (size < static_cast<std::size_t>(bw) * bh * 16)
                    return false;
                for (int by = 0; by < bh; ++by)
                    for (int bx = 0; bx < bw; ++bx)
                    {
                        const std::uint8_t* p = data + (static_cast<std::size_t>(by) * bw + bx) * 16;
                        std::uint8_t alpha[16];
                        if (format == 14)
                            for (int i = 0; i < 16; ++i)
                                alpha[i] = static_cast<std::uint8_t>(((p[i / 2] >> (4 * (i & 1))) & 15) * 17);
                        else
                        {
                            int a[8] = {p[0], p[1]};
                            if (a[0] > a[1])
                                for (int i = 1; i < 7; ++i)
                                    a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
                            else
                            {
                                for (int i = 1; i < 5; ++i)
                                    a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
                                a[6] = 0;
                                a[7] = 255;
                            }
                            std::uint64_t bits = 0;
                            for (int i = 0; i < 6; ++i)
                                bits |= static_cast<std::uint64_t>(p[2 + i]) << (8 * i);
                            for (int i = 0; i < 16; ++i)
                                alpha[i] = static_cast<std::uint8_t>(a[(bits >> (3 * i)) & 7]);
                        }
                        std::uint32_t px[16];
                        color_block(p + 8, true, px, alpha);
                        put_block(bx, by, px);
                    }
                return true;
            case 0:  // RGBA8888
            case 12: // BGRA8888
                if (size < static_cast<std::size_t>(w) * h * 4)
                    return false;
                for (std::size_t i = 0; i < out.bgra.size(); ++i)
                {
                    const std::uint8_t* p = data + i * 4;
                    out.bgra[i] = format == 0 ? pack(p[0], p[1], p[2], p[3]) : pack(p[2], p[1], p[0], p[3]);
                }
                return true;
            default:
                return false;
            }
        }

        std::size_t mip_size(int format, int w, int h)
        {
            w = w > 0 ? w : 1;
            h = h > 0 ? h : 1;
            switch (format)
            {
            case 13:
            case 20:
                return static_cast<std::size_t>((w + 3) / 4) * ((h + 3) / 4) * 8;
            case 14:
            case 15:
                return static_cast<std::size_t>((w + 3) / 4) * ((h + 3) / 4) * 16;
            case 0:
            case 12:
                return static_cast<std::size_t>(w) * h * 4;
            default:
                return 0;
            }
        }

        bool load_vtf(const std::string& file, Image& out)
        {
            const auto* d = reinterpret_cast<const std::uint8_t*>(file.data());
            if (file.size() < 80 || std::memcmp(d, "VTF", 4) != 0)
                return false;
            const auto u32 = [&](std::size_t at) {
                std::uint32_t v;
                std::memcpy(&v, d + at, 4);
                return v;
            };
            const auto u16 = [&](std::size_t at) {
                std::uint16_t v;
                std::memcpy(&v, d + at, 2);
                return v;
            };
            const std::uint32_t minor = u32(8);
            const std::uint32_t header_size = u32(12);
            const int w = u16(16), h = u16(18);
            const std::uint32_t flags = u32(20);
            const int frames = u16(24) > 0 ? u16(24) : 1;
            const int format = static_cast<int>(u32(52));
            const int mips = d[56];
            const int low_format = static_cast<int>(u32(57));
            const int low_w = d[61], low_h = d[62];
            const int faces = (flags & 0x4000) != 0 ? 6 : 1; // an environment map
            if (mip_size(format, w, h) == 0)
                return false;

            std::size_t high = 0;
            if (minor >= 3)
            {
                const std::uint32_t count = u32(68);
                for (std::uint32_t i = 0; i < count && 80 + 8 * i + 8 <= file.size(); ++i)
                    if (d[80 + 8 * i] == 0x30 && d[81 + 8 * i] == 0 && d[82 + 8 * i] == 0)
                        high = u32(80 + 8 * i + 4);
            }
            else
                high = header_size + (low_format >= 0 ? mip_size(low_format, low_w, low_h) : 0);
            if (high == 0)
                return false;
            std::size_t at = high;
            for (int mip = mips - 1; mip > 0; --mip)
                at += mip_size(format, w >> mip, h >> mip) * frames * faces;
            const std::size_t size = mip_size(format, w, h);
            if (at + size > file.size())
                return false;
            return decode(d + at, size, format, w, h, out);
        }

        // ---- the loaded set ----------------------------------------------------------------------------------------

        struct Sheet
        {
            Image image;
            IDirect3DTexture9* texture = nullptr;
            bool failed = false;
        };

        std::mutex g_mutex;
        std::map<std::string, Rect> g_rects; // classname -> rectangle
        std::map<std::string, Sheet> g_sheets; // "vgui/hud/iconsheet" -> image / texture
        std::map<std::string, Icon> g_icons; // classname -> resolved icon (render thread)
        // The loader runs detached: a std::thread still joinable when the game exits (the DLL's statics destroyed at
        // process detach, with no unload before) calls std::terminate, and the game's exit became a crash
        std::atomic<bool> g_started{false};
        std::atomic<bool> g_loading{false};
        std::atomic<bool> g_ready{false};
        IDirect3DDevice9* g_device = nullptr;

        std::wstring game_dir()
        {
            wchar_t path[MAX_PATH];
            const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring dir(path, n);
            return dir.substr(0, dir.find_last_of(L'\\') + 1);
        }

        void load()
        {
            const std::wstring dir = game_dir();
            std::map<std::string, VpkEntry> files;
            for (const wchar_t* sub : {L"update", L"left4dead2_dlc3", L"left4dead2_dlc2", L"left4dead2_dlc1", L"left4dead2"})
                index_vpk(dir + sub + L"\\pak01_dir.vpk", files);

            std::map<std::string, Rect> rects;
            std::map<std::string, Sheet> sheets;
            for (const auto& [path, entry] : files)
            {
                if (path.rfind("scripts/weapon_", 0) != 0 || path.size() < 4 || path.compare(path.size() - 4, 4, ".txt") != 0)
                    continue;
                std::string script;
                Rect rect;
                if (!read_entry(entry, script) || !texture_rect(script, rect))
                    continue;
                for (char& c : rect.sheet)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const std::string classname = path.substr(8, path.size() - 8 - 4); // "scripts/" ... ".txt"
                if (sheets.find(rect.sheet) == sheets.end())
                {
                    Sheet sheet;
                    std::string vtf;
                    const auto it = files.find("materials/" + rect.sheet + ".vtf");
                    sheet.failed = it == files.end() || !read_entry(it->second, vtf) || !load_vtf(vtf, sheet.image);
                    sheets.emplace(rect.sheet, std::move(sheet));
                }
                rects[classname] = rect;
            }
            // the melee weapons: one texture each, keyed by the script name ("fireaxe", "katana" ...)
            int melee = 0;
            for (const auto& [path, entry] : files)
            {
                if (path.rfind("scripts/melee/", 0) != 0 || path.size() < 4 || path.compare(path.size() - 4, 4, ".txt") != 0)
                    continue;
                std::string script;
                Rect rect;
                if (!read_entry(entry, script) || !melee_rect(script, rect))
                    continue;
                for (char& c : rect.sheet)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const std::string sheet_name = rect.sheet;
                if (sheets.find(sheet_name) == sheets.end())
                {
                    Sheet sheet;
                    std::string vtf;
                    const auto it = files.find("materials/" + sheet_name + ".vtf");
                    sheet.failed = it == files.end() || !read_entry(it->second, vtf) || !load_vtf(vtf, sheet.image);
                    sheets.emplace(sheet_name, std::move(sheet));
                }
                const Sheet& sheet = sheets[sheet_name];
                if (sheet.failed)
                    continue;
                const std::string name = path.substr(14, path.size() - 14 - 4); // "scripts/melee/" ... ".txt"
                if (rect.w <= 0 || rect.h <= 0)
                    rect = Rect{sheet_name, 0, 0, sheet.image.width, sheet.image.height};
                rects[name] = rect;
                ++melee;
            }
            INV_LOG("icons: %d melee icons", melee);
            int good = 0;
            for (const auto& [name, sheet] : sheets)
                good += sheet.failed ? 0 : 1;
            INV_LOG("icons: %d weapon icons on %d of %d sheets (%d files indexed)", static_cast<int>(rects.size()), good,
                    static_cast<int>(sheets.size()), static_cast<int>(files.size()));
            std::lock_guard lock(g_mutex);
            g_rects = std::move(rects);
            g_sheets = std::move(sheets);
            g_ready = true;
        }

        void release_textures()
        {
            std::lock_guard lock(g_mutex);
            for (auto& [name, sheet] : g_sheets)
                if (sheet.texture != nullptr)
                {
                    sheet.texture->Release();
                    sheet.texture = nullptr;
                }
            g_icons.clear();
        }

        // (under g_mutex) The icon's rectangle trimmed to its visible pixels: some icons sit in a corner of a much
        // larger rectangle.  Sets the sheet; false when there is none.
        bool trimmed_rect(const char* classname, Rect& r, const Sheet*& sheet_out)
        {
            const auto rect = g_rects.find(classname);
            if (rect == g_rects.end())
                return false;
            const auto sheet = g_sheets.find(rect->second.sheet);
            if (sheet == g_sheets.end() || sheet->second.image.width <= 0)
                return false;
            const Image& image = sheet->second.image;
            r = rect->second;
            int left = r.x + r.w, top = r.y + r.h, right = r.x - 1, bottom = r.y - 1;
            for (int y = std::max(r.y, 0); y < std::min(r.y + r.h, image.height); ++y)
                for (int x = std::max(r.x, 0); x < std::min(r.x + r.w, image.width); ++x)
                    if ((image.bgra[static_cast<std::size_t>(y) * image.width + x] >> 24) > 24)
                    {
                        left = std::min(left, x);
                        right = std::max(right, x);
                        top = std::min(top, y);
                        bottom = std::max(bottom, y);
                    }
            if (right >= left && bottom >= top)
                r = Rect{r.sheet, left, top, right - left + 1, bottom - top + 1};
            sheet_out = &sheet->second;
            return true;
        }

        std::map<std::string, Sprite> g_sprites; // classname -> resolved sprite (an empty material: none)
    } // namespace

    void start()
    {
        if (g_started.exchange(true))
            return;
        g_loading = true;
        std::thread([] {
            load();
            g_loading = false;
        }).detach();
    }

    void set_device(IDirect3DDevice9* device)
    {
        if (device != g_device)
            release_textures();
        g_device = device;
    }

    const Icon* find(const char* classname)
    {
        if (!g_ready || g_device == nullptr || classname == nullptr)
            return nullptr;
        std::lock_guard lock(g_mutex);
        if (const auto known = g_icons.find(classname); known != g_icons.end())
            return known->second.texture != nullptr ? &known->second : nullptr;

        Icon& icon = g_icons[classname]; // remembered, found or not
        const auto rect = g_rects.find(classname);
        if (rect == g_rects.end())
            return nullptr;
        Sheet& sheet = g_sheets[rect->second.sheet];
        if (sheet.failed)
            return nullptr;
        if (sheet.texture == nullptr)
        {
            const Image& image = sheet.image;
            IDirect3DTexture9* texture = nullptr;
            D3DLOCKED_RECT locked{};
            if (FAILED(g_device->CreateTexture(image.width, image.height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                               &texture, nullptr)) ||
                FAILED(texture->LockRect(0, &locked, nullptr, 0)))
            {
                if (texture != nullptr)
                    texture->Release();
                sheet.failed = true;
                return nullptr;
            }
            for (int y = 0; y < image.height; ++y)
                std::memcpy(static_cast<std::uint8_t*>(locked.pBits) + static_cast<std::size_t>(y) * locked.Pitch,
                            image.bgra.data() + static_cast<std::size_t>(y) * image.width,
                            static_cast<std::size_t>(image.width) * 4);
            texture->UnlockRect(0);
            sheet.texture = texture;
        }
        Rect r;
        const Sheet* trimmed_on = nullptr;
        if (!trimmed_rect(classname, r, trimmed_on))
            return nullptr;
        const float iw = static_cast<float>(sheet.image.width), ih = static_cast<float>(sheet.image.height);
        icon.texture = sheet.texture;
        icon.u0 = static_cast<float>(r.x) / iw;
        icon.v0 = static_cast<float>(r.y) / ih;
        icon.u1 = static_cast<float>(r.x + r.w) / iw;
        icon.v1 = static_cast<float>(r.y + r.h) / ih;
        icon.aspect = static_cast<float>(r.w) / static_cast<float>(r.h);
        return &icon;
    }

    bool find_sprite(const char* classname, Sprite& out)
    {
        if (!g_ready || classname == nullptr)
            return false;
        std::lock_guard lock(g_mutex);
        auto known = g_sprites.find(classname);
        if (known == g_sprites.end())
        {
            Sprite& sprite = g_sprites[classname]; // remembered, found or not
            Rect r;
            const Sheet* sheet = nullptr;
            if (trimmed_rect(classname, r, sheet) && !sheet->failed && r.w > 0 && r.h > 0)
            {
                const float iw = static_cast<float>(sheet->image.width), ih = static_cast<float>(sheet->image.height);
                sprite.material = r.sheet;
                sprite.u0 = static_cast<float>(r.x) / iw;
                sprite.v0 = static_cast<float>(r.y) / ih;
                sprite.u1 = static_cast<float>(r.x + r.w) / iw;
                sprite.v1 = static_cast<float>(r.y + r.h) / ih;
                sprite.aspect = static_cast<float>(r.w) / static_cast<float>(r.h);
            }
            known = g_sprites.find(classname);
        }
        if (known->second.material.empty())
            return false;
        out = known->second;
        return true;
    }

    void shutdown()
    {
        for (int i = 0; i < 400 && g_loading.load(); ++i)
            Sleep(25); // (the loader reads the game's files: it is done within seconds)
        release_textures();
        g_device = nullptr;
    }

} // namespace icons
