// Bloom, going down (task fx1, gpu_postfx.cpp): halves the picture with the 13-tap filter of Jimenez ("Next Generation Post
// Processing in Call of Duty: Advanced Warfare", 2014), whose taps overlap so a bright pixel does not flicker when it moves.
// With Mode.x = 1 (the first step) every tap is first run through the bloom threshold with a soft knee: only what is brighter
// than the threshold goes on to glow.
//
// It draws a texture half the size of the one it reads. Src.xy is where the picture starts in Tex (only the first step reads from
// inside a bigger texture); taps clamp to the picture.
//
// The first step reads the 8-bit picture in the display encoding and makes LINEAR light of it (fx2): a tap is the mean of the linear
// values of the 2 x 2 texels around it (not the linear value of their mean, which is darker at an edge between bright and dark), and the
// threshold (Bloom.x) is a linear value. The later steps read the half-float levels this pass wrote before.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 1
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1

#include "postfx_common.hlsli"

COMBINED Texture2D<float4> Tex : register(t0, space2);
COMBINED SamplerState Samp : register(s0, space2);

float3 Prefilter(float3 c)
{
    const float brightness = max(c.r, max(c.g, c.b));
    const float knee = Bloom.x * Bloom.y + 1e-4f;
    float soft = clamp(brightness - Bloom.x + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee);
    const float contribution = max(soft, brightness - Bloom.x) / max(brightness, 1e-4f);
    return c * contribution;
}

// The bilinear sample at q (texel coordinates) of the 8-bit picture, mixed in linear light: four loads, clamped to the picture.
float3 LinearBilinear(float2 q)
{
    const float2 f = q - 0.5f;
    const float2 fl = floor(f);
    const float2 w = f - fl;
    const int2 lo = int2(Src.xy);
    const int2 hi = lo + int2(Src.zw) - 1;
    const int2 p0 = clamp(int2(fl), lo, hi);
    const int2 p1 = clamp(int2(fl) + 1, lo, hi);
    const float3 a = SrgbToLinear(Tex.Load(int3(p0.x, p0.y, 0)).rgb);
    const float3 b = SrgbToLinear(Tex.Load(int3(p1.x, p0.y, 0)).rgb);
    const float3 c = SrgbToLinear(Tex.Load(int3(p0.x, p1.y, 0)).rgb);
    const float3 d = SrgbToLinear(Tex.Load(int3(p1.x, p1.y, 0)).rgb);
    return lerp(lerp(a, b, w.x), lerp(c, d, w.x), w.y);
}

float3 Tap(float2 centre, float2 offset, float2 texSize)
{
    const float2 lo = Src.xy + 0.5f;
    const float2 hi = Src.xy + Src.zw - 0.5f;
    const float2 q = clamp(centre + offset, lo, hi);
    if (Mode.x > 0.5f)
        return Prefilter(LinearBilinear(q));
    return Tex.SampleLevel(Samp, q / texSize, 0.0f).rgb;
}

float4 main(PSInput input) : SV_Target0
{
    uint tw, th;
    Tex.GetDimensions(tw, th);
    const float2 texSize = float2((float)tw, (float)th);
    // The centre of this pixel in the source: the corner shared by the 2 x 2 block it stands for.
    const float2 c = Src.xy + input.Position.xy * 2.0f;
    const float3 a = Tap(c, float2(-2.0f, -2.0f), texSize);
    const float3 b = Tap(c, float2(0.0f, -2.0f), texSize);
    const float3 cc = Tap(c, float2(2.0f, -2.0f), texSize);
    const float3 d = Tap(c, float2(-1.0f, -1.0f), texSize);
    const float3 e = Tap(c, float2(1.0f, -1.0f), texSize);
    const float3 f = Tap(c, float2(-2.0f, 0.0f), texSize);
    const float3 g = Tap(c, float2(0.0f, 0.0f), texSize);
    const float3 h = Tap(c, float2(2.0f, 0.0f), texSize);
    const float3 i = Tap(c, float2(-1.0f, 1.0f), texSize);
    const float3 j = Tap(c, float2(1.0f, 1.0f), texSize);
    const float3 k = Tap(c, float2(-2.0f, 2.0f), texSize);
    const float3 l = Tap(c, float2(0.0f, 2.0f), texSize);
    const float3 m = Tap(c, float2(2.0f, 2.0f), texSize);
    const float3 sum = (d + e + i + j) * 0.125f + (a + b + f + g) * 0.03125f + (b + cc + g + h) * 0.03125f +
                       (f + g + k + l) * 0.03125f + (g + h + l + m) * 0.03125f;
    return float4(sum, 1.0f);
}
