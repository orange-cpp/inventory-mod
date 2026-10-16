#include "ui/postfx.hpp"

#include "core/config.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstring>
#include <d3d9.h>
#include <d3dcommon.h>
#include <string>
#include <vector>
#include <wincodec.h>

namespace postfx
{

    namespace
    {
        // ---- the shaders ---------------------------------------------------------------------------------------------
        //
        // One full-screen pass over a copy of the frame.  What grows with the severity s (0 .. 1):
        //   - the heartbeat: the picture throbs in on every beat (faster as it gets worse, the HUD's clock);
        //   - the world sways: slow waves, then quicker small ones;
        //   - the edges blur and split into red / blue, wider on the beat;
        //   - double vision past half way;
        //   - the colour drains to a sick yellow green, the contrast hardens;
        //   - dark veins creep in from the edges, glowing red on the beat;
        //   - the vignette closes in; grain; near the end the sight dims now and then;
        //   - the last seconds: a red beat.

        constexpr const char* k_vertex_shader = R"(
float4 half_pixel : register(c0);
struct Vertex { float4 pos : POSITION; float2 uv : TEXCOORD0; };
Vertex main(Vertex v) { v.pos.xy += half_pixel.xy; return v; }
)";

        constexpr const char* k_pixel_shader = R"(
sampler2D frame : register(s0);
float4 p0 : register(c0); // time (s), severity, beat, last
float4 p1 : register(c1); // 1 / width, 1 / height, width / height, 0

float hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float noise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), f.x), lerp(hash(i + float2(0, 1)), hash(i + 1.0), f.x), f.y);
}
float fbm(float2 p) { return noise(p) * 0.5 + noise(p * 2.03 + 7.1) * 0.3 + noise(p * 4.1 + 3.7) * 0.2; }

// the frame with its red and blue pulled apart along dir
float3 tap(float2 uv, float2 dir, float split)
{
    return float3(tex2Dlod(frame, float4(uv + dir * split, 0, 0)).r,
                  tex2Dlod(frame, float4(uv, 0, 0)).g,
                  tex2Dlod(frame, float4(uv - dir * split, 0, 0)).b);
}

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float t = p0.x, s = p0.y, beat = p0.z, last = p0.w, aspect = p1.z;
    float s2 = s * s;
    float2 c = (uv - 0.5) * float2(aspect, 1.0); // from the middle, square units
    float r = length(c);
    float e = length(uv - 0.5) * 2.0; // 0 in the middle, 1 at the middle of an edge, 1.41 in a corner

    // the throb and the sway
    float2 suv = 0.5 + (uv - 0.5) * (1.0 - 0.014 * beat * (0.25 + s));
    suv += float2(sin(t * 0.9 + uv.y * 5.0), cos(t * 0.7 + uv.x * 4.0)) * 0.004 * s;
    suv += float2(sin(t * 2.3 + uv.y * 17.0), sin(t * 1.9 + uv.x * 13.0)) * 0.0018 * s2;

    // blurred, split edges
    float edge = saturate((e - 0.35 + 0.25 * s) * 1.6);
    float2 dir = c / max(r, 0.0001);
    dir.x /= aspect;
    float split = (0.0012 + 0.009 * s * (0.6 + 0.7 * beat)) * (0.25 + edge);
    float blur = (0.0015 + 0.011 * s) * edge;
    float3 col = tap(suv, dir, split) * 2.0;
    [unroll] for (int k = 0; k < 8; ++k)
    {
        float a = k * 0.785398 + t * 0.4;
        col += tap(suv + float2(cos(a) / aspect, sin(a)) * blur, dir, split);
    }
    col /= 10.0;

    // double vision
    float dv = saturate((s - 0.5) / 0.5);
    float2 ghost = float2(sin(t * 0.8), cos(t * 0.6) * 0.4) * 0.016 * dv * (0.7 + 0.5 * beat);
    col = lerp(col, tap(suv + ghost, dir, split * 1.5), 0.42 * dv);

    // the sick colour
    float lum = dot(col, float3(0.299, 0.587, 0.114));
    float3 sick = lum * float3(0.80, 1.0, 0.42) + float3(0.015, 0.035, 0.0);
    col = lerp(col, sick, saturate(0.18 + 0.62 * s));
    col = saturate((col - 0.5) * (1.0 + 0.35 * s) + 0.5 - 0.04 * s);

    // the veins: ridges of warped noise, from the edges inward
    float2 vp = c * 6.0;
    float n = fbm(vp + fbm(vp * 0.5 + float2(t * 0.03, 0.0)) * 1.8);
    float width = 0.035 + 0.025 * beat * s;
    float vein = 1.0 - smoothstep(0.0, width, abs(n - 0.5));
    float reach = smoothstep(1.2 - 0.8 * s, 1.45 - 0.65 * s, e + (fbm(c * 3.0) - 0.5) * 0.3);
    vein *= reach;
    col = col * (1.0 - 0.8 * vein) + vein * float3(0.20, 0.01, 0.03) * (0.3 + 0.9 * beat);

    // the vignette
    float vig = smoothstep(0.55 - 0.4 * s, 1.5 - 0.6 * s - 0.1 * beat * s, e);
    col = lerp(col, col * float3(0.05, 0.09, 0.02), vig * (0.6 + 0.4 * s));

    // grain
    float2 px = uv / p1.xy + frac(t * 7.13) * float2(97.0, 57.0);
    float grain = frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
    col += (grain - 0.5) * (0.02 + 0.025 * s);

    // the sight dims now and then near the end
    col *= 1.0 - 0.55 * s2 * s * smoothstep(0.62, 0.9, noise(float2(t * 1.3, 5.0)));

    // the beat, and the last seconds in red
    col += float3(0.06, 0.0, 0.0) * beat * s;
    col = lerp(col, float3(col.r * 1.45 + 0.07, col.g * 0.3, col.b * 0.25), last * (0.25 + 0.6 * beat));
    return float4(saturate(col), 1.0);
}
)";

        using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                              LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

        struct Vertex
        {
            float x, y, z, w;
            float u, v;
        };

        IDirect3DVertexShader9* g_vs = nullptr;
        IDirect3DPixelShader9* g_ps = nullptr;
        IDirect3DVertexDeclaration9* g_decl = nullptr;
        IDirect3DDevice9* g_device = nullptr;
        IDirect3DTexture9* g_screamer = nullptr; // managed pool: survives a Reset
        float g_screamer_aspect = 1.f;
        std::string g_screamer_path; // what g_screamer was read from (also when that failed)
        IDirect3DTexture9* g_copy = nullptr; // the frame
        UINT g_copy_w = 0, g_copy_h = 0;
        D3DFORMAT g_copy_format = D3DFMT_UNKNOWN;
        bool g_ready = false;
        bool g_failed_copy = false; // logged once
        bool g_wanted = false; // set_infection this frame
        struct Screamer
        {
            bool wanted = false;
            IDirect3DTexture9* texture = nullptr;
            float rect[4] = {};
            D3DCOLOR color = 0;
            float backdrop = 0.f, red = 0.f;
        };
        Screamer g_screamer_frame; // set_screamer this frame
        float g_severity = 0.f, g_beat = 0.f, g_last = 0.f;

        template <class T> void release(T*& p)
        {
            if (p != nullptr)
            {
                p->Release();
                p = nullptr;
            }
        }

        ID3DBlob* compile(D3DCompileFn d3d_compile, const char* source, const char* target)
        {
            ID3DBlob* code = nullptr;
            ID3DBlob* errors = nullptr;
            const HRESULT hr = d3d_compile(source, std::strlen(source), "inventory_mod_infection", nullptr, nullptr, "main",
                                           target, 1u << 15 /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, &code, &errors);
            if (FAILED(hr))
            {
                INV_LOG("postfx: the %s shader did not compile (0x%08lX): %s", target, static_cast<unsigned long>(hr),
                        errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                release(code);
            }
            release(errors);
            return code;
        }

        // A picture file into BGRA pixels (at most 2048 on a side).
        bool decode(const std::wstring& path, std::vector<unsigned char>& pixels, UINT& width, UINT& height)
        {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            bool ok = false;
            IWICImagingFactory* factory = nullptr;
            IWICBitmapDecoder* decoder = nullptr;
            IWICBitmapFrameDecode* frame = nullptr;
            IWICBitmapScaler* scaler = nullptr;
            IWICFormatConverter* converter = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                             WICDecodeMetadataCacheOnDemand, &decoder)) &&
                SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) && width > 0 &&
                height > 0)
            {
                IWICBitmapSource* source = frame;
                const UINT longest = std::max(width, height);
                if (longest > 2048 && SUCCEEDED(factory->CreateBitmapScaler(&scaler)))
                {
                    width = std::max(1u, width * 2048 / longest);
                    height = std::max(1u, height * 2048 / longest);
                    if (SUCCEEDED(scaler->Initialize(frame, width, height, WICBitmapInterpolationModeFant)))
                        source = scaler;
                }
                if (SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
                    SUCCEEDED(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                                    0.0, WICBitmapPaletteTypeCustom)))
                {
                    pixels.resize(static_cast<std::size_t>(width) * height * 4);
                    ok = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
                }
            }
            release(converter);
            release(scaler);
            release(frame);
            release(decoder);
            release(factory);
            if (SUCCEEDED(co))
                CoUninitialize();
            return ok;
        }

        void load_screamer()
        {
            const std::string& name = core::config().screamer_image;
            if (name == g_screamer_path)
                return;
            g_screamer_path = name;
            release(g_screamer);
            if (g_device == nullptr || name.empty())
                return;
            std::wstring wide(name.size(), L'\0');
            wide.resize(static_cast<std::size_t>(std::max(0, MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()),
                                                                                   wide.data(), static_cast<int>(wide.size())))));
            std::replace(wide.begin(), wide.end(), L'/', L'\\');
            if (wide.find(L':') == std::wstring::npos && wide.rfind(L"\\\\", 0) != 0)
                wide = std::wstring(core::module_dir()) + wide;
            std::vector<unsigned char> pixels;
            UINT width = 0, height = 0;
            if (!decode(wide, pixels, width, height))
            {
                INV_LOG("postfx: no screamer picture: %s cannot be read (PNG, JPEG, BMP)", name.c_str());
                return;
            }
            D3DLOCKED_RECT locked{};
            if (FAILED(g_device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_screamer, nullptr)) ||
                FAILED(g_screamer->LockRect(0, &locked, nullptr, 0)))
            {
                release(g_screamer);
                INV_LOG("postfx: no %ux%u texture for the screamer", width, height);
                return;
            }
            for (UINT y = 0; y < height; ++y)
                std::memcpy(static_cast<unsigned char*>(locked.pBits) + static_cast<std::size_t>(y) * locked.Pitch,
                            pixels.data() + static_cast<std::size_t>(y) * width * 4, static_cast<std::size_t>(width) * 4);
            g_screamer->UnlockRect(0);
            g_screamer_aspect = static_cast<float>(width) / static_cast<float>(height);
            INV_LOG("postfx: the screamer is %s (%ux%u)", name.c_str(), width, height);
        }

        // A screen-space quad with the fixed-function pipeline (pre-transformed vertices, blended).
        struct QuadVertex
        {
            float x, y, z, rhw;
            D3DCOLOR color;
            float u, v;
        };

        void draw_quad(IDirect3DDevice9* device, IDirect3DTexture9* texture, float x0, float y0, float x1, float y1, D3DCOLOR color)
        {
            device->SetTexture(0, texture);
            device->SetTextureStageState(0, D3DTSS_COLOROP, texture != nullptr ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, texture != nullptr ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            x0 -= 0.5f, y0 -= 0.5f, x1 -= 0.5f, y1 -= 0.5f; // D3D9 pixel centres
            const QuadVertex quad[4] = {
                {x0, y0, 0.f, 1.f, color, 0.f, 0.f},
                {x1, y0, 0.f, 1.f, color, 1.f, 0.f},
                {x0, y1, 0.f, 1.f, color, 0.f, 1.f},
                {x1, y1, 0.f, 1.f, color, 1.f, 1.f},
            };
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex));
        }

        void draw_screamer(IDirect3DDevice9* device, const Screamer& s, float w, float h)
        {
            device->SetVertexShader(nullptr);
            device->SetPixelShader(nullptr);
            device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            device->SetRenderState(D3DRS_LIGHTING, FALSE);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
            device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            const auto alpha = [](float a) { return static_cast<D3DCOLOR>(std::clamp(a, 0.f, 1.f) * 255.f) << 24; };
            draw_quad(device, nullptr, 0.f, 0.f, w, h, alpha(s.backdrop));
            if (s.texture != nullptr)
                draw_quad(device, s.texture, s.rect[0], s.rect[1], s.rect[2], s.rect[3], s.color);
            draw_quad(device, nullptr, 0.f, 0.f, w, h, alpha(s.red) | 0x00960000u);
            device->SetTexture(0, nullptr);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        }

        bool make_copy(IDirect3DDevice9* device, IDirect3DSurface9* back_buffer)
        {
            D3DSURFACE_DESC desc{};
            if (FAILED(back_buffer->GetDesc(&desc)))
                return false;
            if (g_copy != nullptr && desc.Width == g_copy_w && desc.Height == g_copy_h && desc.Format == g_copy_format)
                return true;
            release(g_copy);
            const HRESULT hr = device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                                                     D3DPOOL_DEFAULT, &g_copy, nullptr);
            if (FAILED(hr))
            {
                if (!g_failed_copy)
                    INV_LOG("postfx: no %ux%u copy of the frame (format %d): 0x%08lX", desc.Width, desc.Height,
                            static_cast<int>(desc.Format), static_cast<unsigned long>(hr));
                g_failed_copy = true;
                g_copy = nullptr;
                return false;
            }
            g_copy_w = desc.Width;
            g_copy_h = desc.Height;
            g_copy_format = desc.Format;
            return true;
        }
    } // namespace

    bool init(IDirect3DDevice9* device)
    {
        shutdown();
        g_device = device;
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        const auto d3d_compile =
            compiler != nullptr ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (d3d_compile == nullptr)
        {
            INV_LOG("postfx: no d3dcompiler_47.dll, the infection gets the plain overlay");
            if (compiler != nullptr)
                FreeLibrary(compiler);
            return false;
        }
        ID3DBlob* vs = compile(d3d_compile, k_vertex_shader, "vs_3_0");
        ID3DBlob* ps = vs != nullptr ? compile(d3d_compile, k_pixel_shader, "ps_3_0") : nullptr;
        bool ok = vs != nullptr && ps != nullptr &&
                  SUCCEEDED(device->CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()), &g_vs)) &&
                  SUCCEEDED(device->CreatePixelShader(static_cast<const DWORD*>(ps->GetBufferPointer()), &g_ps));
        if (ok)
        {
            const D3DVERTEXELEMENT9 elements[] = {
                {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                D3DDECL_END(),
            };
            ok = SUCCEEDED(device->CreateVertexDeclaration(elements, &g_decl));
        }
        if (ps != nullptr)
            INV_LOG("postfx: infection shader ready (%u bytes)", static_cast<unsigned>(ps->GetBufferSize()));
        release(vs);
        release(ps);
        FreeLibrary(compiler);
        if (!ok)
        {
            shutdown();
            return false;
        }
        g_ready = true;
        return true;
    }

    bool ready() { return g_ready; }

    void set_infection(float severity, float beat, float last)
    {
        g_wanted = true;
        g_severity = severity;
        g_beat = beat;
        g_last = last;
    }

    void set_screamer(void* texture, const float rect[4], unsigned color, float backdrop, float red)
    {
        g_screamer_frame.wanted = true;
        g_screamer_frame.texture = static_cast<IDirect3DTexture9*>(texture);
        std::memcpy(g_screamer_frame.rect, rect, sizeof(g_screamer_frame.rect));
        g_screamer_frame.color = color;
        g_screamer_frame.backdrop = backdrop;
        g_screamer_frame.red = red;
    }

    void render(IDirect3DDevice9* device, IDirect3DSurface9* back_buffer)
    {
        const bool wanted = g_wanted;
        g_wanted = false;
        const Screamer screamer = g_screamer_frame;
        g_screamer_frame.wanted = false;
        if (!g_ready || back_buffer == nullptr || (!wanted && !screamer.wanted) || !make_copy(device, back_buffer))
            return;
        if (screamer.wanted)
        {
            // (the overlay's state block puts the game's state back afterwards)
            D3DVIEWPORT9 full{0, 0, g_copy_w, g_copy_h, 0.f, 1.f};
            device->SetViewport(&full);
            device->SetRenderState(D3DRS_ZENABLE, FALSE);
            device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            device->SetRenderState(D3DRS_FOGENABLE, FALSE);
            device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
            device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            draw_screamer(device, screamer, static_cast<float>(g_copy_w), static_cast<float>(g_copy_h));
        }
        if (!wanted)
            return;
        IDirect3DSurface9* copy = nullptr;
        if (FAILED(g_copy->GetSurfaceLevel(0, &copy)))
            return;
        const HRESULT hr = device->StretchRect(back_buffer, nullptr, copy, nullptr, D3DTEXF_NONE);
        copy->Release();
        if (FAILED(hr))
            return;

        // (the overlay's state block puts the game's state back afterwards)
        const float w = static_cast<float>(g_copy_w), h = static_cast<float>(g_copy_h);
        D3DVIEWPORT9 viewport{0, 0, g_copy_w, g_copy_h, 0.f, 1.f};
        device->SetViewport(&viewport);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        device->SetTexture(0, g_copy);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        device->SetVertexDeclaration(g_decl);
        device->SetVertexShader(g_vs);
        device->SetPixelShader(g_ps);
        const float half_pixel[4] = {-1.f / w, 1.f / h, 0.f, 0.f};
        device->SetVertexShaderConstantF(0, half_pixel, 1);
        const float time = static_cast<float>(GetTickCount64() % 1000000ULL) / 1000.f;
        const float constants[8] = {time, g_severity, g_beat, g_last, 1.f / w, 1.f / h, w / h, 0.f};
        device->SetPixelShaderConstantF(0, constants, 2);
        const Vertex quad[4] = {
            {-1.f, 1.f, 0.5f, 1.f, 0.f, 0.f},
            {1.f, 1.f, 0.5f, 1.f, 1.f, 0.f},
            {-1.f, -1.f, 0.5f, 1.f, 0.f, 1.f},
            {1.f, -1.f, 0.5f, 1.f, 1.f, 1.f},
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
        device->SetTexture(0, nullptr);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
    }

    void* screamer_texture(float* aspect)
    {
        load_screamer();
        if (aspect != nullptr)
            *aspect = g_screamer_aspect;
        return g_screamer;
    }

    void invalidate()
    {
        release(g_copy);
        g_copy_w = g_copy_h = 0;
    }

    void shutdown()
    {
        invalidate();
        release(g_vs);
        release(g_ps);
        release(g_decl);
        release(g_screamer);
        g_screamer_path.clear();
        g_device = nullptr;
        g_ready = false;
        g_wanted = false;
    }

} // namespace postfx
