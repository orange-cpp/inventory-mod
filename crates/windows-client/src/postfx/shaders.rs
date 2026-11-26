// Original shader programs shared with the C++ mod, compiled by Direct3D 9 at runtime.

pub const VERTEX: &str = r#"
float4 half_pixel : register(c0);
struct Vertex { float4 pos : POSITION; float2 uv : TEXCOORD0; };
Vertex main(Vertex v) { v.pos.xy += half_pixel.xy; return v; }
"#;

pub const PIXEL: &str = r#"
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
"#;

pub const STAMINA: &str = r#"
sampler2D frame : register(s0);
float4 p0 : register(c0); // time (s), tired (0 at half .. 1 empty), exhausted (0 .. 1, eased), breath (0 .. 1)
float4 p1 : register(c1); // 1 / width, 1 / height, width / height, 0

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float k = p0.y, ex = p0.z, breath = p0.w, aspect = p1.z;
    float e = length(uv - 0.5) * 2.0; // 0 in the middle, 1 at the middle of an edge
    float2 suv = 0.5 + (uv - 0.5) * (1.0 - 0.006 * breath * ex); // each gasp pulls the picture in a little

    float edge = saturate((e - 0.45) * 1.8);
    float blur = 0.0045 * edge * ex;
    float3 col = tex2Dlod(frame, float4(suv, 0, 0)).rgb * 2.0;
    [unroll] for (int i = 0; i < 6; ++i)
    {
        float a = i * 1.047198;
        col += tex2Dlod(frame, float4(suv + float2(cos(a) / aspect, sin(a)) * blur, 0, 0)).rgb;
    }
    col /= 8.0;

    float inner = 0.95 - 0.40 * k - 0.15 * ex - 0.10 * ex * breath;
    float vig = smoothstep(inner, inner + 0.8, e);
    float lum = dot(col, float3(0.299, 0.587, 0.114));
    col = lerp(col, lum.xxx, saturate(vig * (0.45 * k + 0.35 * ex)));
    col *= 1.0 - vig * saturate(0.5 * k + 0.35 * ex * (0.6 + 0.4 * breath));
    col += float3(0.045, 0.0, 0.0) * vig * ex * breath;
    return float4(saturate(col), 1.0);
}
"#;
