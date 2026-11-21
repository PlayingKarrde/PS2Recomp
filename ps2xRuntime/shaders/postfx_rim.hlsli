// The body of postfx_rim.frag.hlsl (task lt3-T1) and, with RIM_NORMALS defined, of postfx_rim_n.frag.hlsl (task lt3-T3). See those
// files for what the pass does and what it reads. Without RIM_NORMALS this compiles to exactly what T1 had.

#include "postfx_common.hlsli"

#define MAX_LIGHTS 8

cbuffer Light : register(b1, space3)
{
    float4 LightAmbient;                // rgb; w: how many lights follow
    float4 LightRule;                   // not used here
    float4 LightPosition[MAX_LIGHTS];   // xyz: where the light is (world units), w: its range (no light beyond it)
    float4 LightAxis[MAX_LIGHTS];       // xyz: the unit direction a spotlight points in, w: its intensity
    float4 LightColour[MAX_LIGHTS];     // rgb, w: 1 a spotlight, 0 a point light
    float4 LightCone[MAX_LIGHTS];       // x: cos(outer half angle), y: cos(inner half angle)
};

COMBINED Texture2D<float4> Light : register(t0, space2);
COMBINED SamplerState LightSamp : register(s0, space2);
COMBINED Texture2D<float4> Where : register(t1, space2);
COMBINED SamplerState WhereSamp : register(s1, space2);
COMBINED Texture2D<float4> Blur : register(t2, space2);
COMBINED SamplerState BlurSamp : register(s2, space2);
#ifdef RIM_NORMALS
COMBINED Texture2D<float4> Lean : register(t3, space2);
COMBINED SamplerState LeanSamp : register(s3, space2);

// The numbers below are ps2x_lighting's kTileBulgeRadius, kTilePaintRange, kCreaseStart, kCreaseEnd, kWrestlerLeanMost.
#define TILE_RADIUS 5.0f
#define PAINT_RANGE 2.0f
#define CREASE_START 1.0f
#define CREASE_END 3.0f
#define LEAN_MOST 0.97f

// How many sigmas into a blurred edge the value b lies (1/2 on the edge, 1 far inside): the inverse of the normal distribution,
// through Winitzki's approximation of the inverse error function (ps2x_lighting's sigmasInside).
float SigmasInside(float b)
{
    const float x = clamp(b * 2.0f - 1.0f, 0.0f, 0.9999f);
    const float a = 0.147f;
    const float ln = log(1.0f - x * x);
    const float t = 2.0f / (3.14159265f * a) + ln * 0.5f;
    return 1.41421356f * sqrt(max(sqrt(max(t * t - ln / a, 0.0f)) - t, 0.0f));
}

// What the tile's generated map adds to the lean at a pixel (ps2x_lighting::wrestlerTileLean). `lean` is the card shader's fourth
// output, `b` the blurred mask of the pixel's wrestler.
float2 TileLean(float4 lean, float b)
{
    const float4 m = lean * 255.0f;
    const float2 bulge = (m.xy - 128.0f) / 127.0f;
    const float2 paint = (m.zw - 128.0f) / 127.0f * PAINT_RANGE;
    const float l = min(length(bulge), 1.0f);
    const float dTile = TILE_RADIUS * (1.0f - l);
    const float dOut = Bloom.w * SigmasInside(b);
    const float x = saturate((dOut - dTile - CREASE_START) / (CREASE_END - CREASE_START));
    const float fade = 1.0f - Bloom.z * (x * x * (3.0f - 2.0f * x));
    const float2 slope = bulge * (fade / sqrt(max(1.0f - l * l, 0.05f))) + Bloom.y * paint;
    return slope * (Bloom.x / sqrt(1.0f + dot(slope, slope)));
}
#endif

#define WRAP 0.5f

float BlurAt(int2 p, float4 channel)
{
    p = clamp(p, int2(0, 0), int2(Src.zw) - 1);
    return dot(Blur.Load(int3(p, 0)), channel);
}

float4 main(PSInput input) : SV_Target0
{
    const int2 q = int2(input.Position.xy); // this pixel in the frame target
    const float4 light = Light.Load(int3(q, 0));
    const float4 where = Where.Load(int3(q, 0));
    if (where.a < 0.75f)
        return light; // not a wrestler's pixel
    const float group = floor(where.a);
    const float share = (where.a - group) * 2.0f;
    const float4 channel = float4(group == 1.0f ? 1.0f : 0.0f, group == 2.0f ? 1.0f : 0.0f, group == 3.0f ? 1.0f : 0.0f, group >= 4.0f ? 1.0f : 0.0f);

    // The outline: how far down the blurred mask this pixel is, and which way the mask falls (y grows down the screen).
    const int2 p = q - int2(Src.xy);
    const float b = BlurAt(p, channel);
    const float gx = (BlurAt(p + int2(1, 0), channel) - BlurAt(p - int2(1, 0), channel)) * 0.5f;
    const float gy = (BlurAt(p + int2(0, 1), channel) - BlurAt(p - int2(0, 1), channel)) * 0.5f;
    const float steep = sqrt(gx * gx + gy * gy);
    const float lean = saturate((1.0f - b) * 2.0f) * Tilt.w;
    const float2 out2 = steep > 1.0e-7f ? float2(-gx, -gy) / steep : float2(0.0f, 0.0f); // the way out, on the screen

    // The camera's axes at this pixel, in the world: V away from the eye, R to the right of the screen, U up the screen.
    const float3 world = where.rgb;
    const float3 V = normalize(world - Aux.xyz);
    float3 R = cross(float3(0.0f, 1.0f, 0.0f), V);
    const float rl = length(R);
    R = rl > 1.0e-5f ? R / rl : float3(1.0f, 0.0f, 0.0f);
    const float3 U = cross(V, R);
    R *= Aux.w;
    const float2 s = out2 * lean;
#ifdef RIM_NORMALS
    // The generated map's lean is added to the outline's; together they never tip the surface further than LEAN_MOST.
    const float2 tile = TileLean(Lean.Load(int3(q, 0)), b);
    float2 tip = s;
    float depth = sqrt(max(1.0f - lean * lean, 0.02f));
    if (tile.x != 0.0f || tile.y != 0.0f)
    {
        tip += tile;
        const float tl = length(tip);
        if (tl > LEAN_MOST)
            tip *= LEAN_MOST / tl;
        depth = sqrt(max(1.0f - dot(tip, tip), 0.02f));
    }
    const float3 N = normalize(tip.x * R - tip.y * U - depth * V);
#else
    const float3 N = normalize(s.x * R - s.y * U - sqrt(max(1.0f - lean * lean, 0.02f)) * V);
#endif
    const float3 N2 = normalize(s.x * R - s.y * U - 0.001f * V);

    float3 today = float3(0.0f, 0.0f, 0.0f), shaped = float3(0.0f, 0.0f, 0.0f), rim = float3(0.0f, 0.0f, 0.0f);
    float gain = 1.0f;
    const int count = (int)LightAmbient.w;
    for (int k = 0; k < MAX_LIGHTS; ++k)
    {
        if (k >= count)
            break;
        const float3 toLight = LightPosition[k].xyz - world;
        const float distance = length(toLight);
        const float3 l = distance > 0.0f ? toLight / distance : float3(0.0f, 1.0f, 0.0f);
        if (k == 0)
            gain = 1.0f / clamp((dot(-V, l) + WRAP) / (1.0f + WRAP), 0.3f, 1.0f);
        if (distance >= LightPosition[k].w)
            continue;
        const float ratio = distance / LightPosition[k].w;
        const float fall = (1.0f - ratio * ratio) * (1.0f - ratio * ratio);
        float cone = 1.0f;
        if (LightColour[k].w > 0.5f)
            cone = smoothstep(LightCone[k].x, LightCone[k].y, -dot(LightAxis[k].xyz, l));
        const float3 c = LightColour[k].rgb * (LightAxis[k].w * fall * cone);
        today += c;
        shaped += c * saturate((dot(N, l) + WRAP) / (1.0f + WRAP));
        rim += c * max(dot(N2, l), 0.0f);
    }
    const float3 change = Tilt.x * (shaped * gain - today) + Tilt.y * (lean * lean) * rim;
    return float4(max(light.rgb + change * share, 0.0f), light.a);
}
