// The body of postfx_down.frag.hlsl (task fx1, fx2) and, with DOWN_WRESTLER defined, of postfx_down_w.frag.hlsl (task fx3). See
// those files for what the pass does and what it reads. Without DOWN_WRESTLER this compiles to exactly what postfx_down.frag.hlsl
// compiled to before the body moved here.

#include "postfx_common.hlsli"

COMBINED Texture2D<float4> Tex : register(t0, space2);
COMBINED SamplerState Samp : register(s0, space2);
#ifdef DOWN_WRESTLER
COMBINED Texture2D<float4> Mask : register(t1, space2);
COMBINED SamplerState MaskSamp : register(s1, space2);
#endif

float3 Prefilter(float3 c)
{
#ifdef DOWN_WRESTLER
    // Aux.x: brightness from the largest channel (0) to the Rec.709 luminance (1); the colour that passes is the pixel's own either way
    const float brightness = lerp(max(c.r, max(c.g, c.b)), dot(c, float3(0.2126f, 0.7152f, 0.0722f)), Aux.x);
#else
    const float brightness = max(c.r, max(c.g, c.b));
#endif
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

#ifdef DOWN_WRESTLER
// How much of the bloom source a wrestler pixel keeps: 1 off the wrestlers, Aux.y on them. The wrestler mask (gs_mask.frag.hlsl: 1 in
// the channel of a wrestler's card pixels, no depth test) is read at the four texels LinearBilinear mixes, with the same weights, so the
// edge of a wrestler is as soft as the tap. The mask is the frame target's size and the picture is Aux.zw into it; Tex holds the
// picture from Src.xy.
float WrestlerHold(float2 q)
{
    const float2 f = q - 0.5f;
    const float2 fl = floor(f);
    const float2 w = f - fl;
    const int2 lo = int2(Src.xy);
    const int2 hi = lo + int2(Src.zw) - 1;
    const int2 p0 = clamp(int2(fl), lo, hi) + int2(Aux.zw) - lo;
    const int2 p1 = clamp(int2(fl) + 1, lo, hi) + int2(Aux.zw) - lo;
    const float4 ma = Mask.Load(int3(p0.x, p0.y, 0));
    const float4 mb = Mask.Load(int3(p1.x, p0.y, 0));
    const float4 mc = Mask.Load(int3(p0.x, p1.y, 0));
    const float4 md = Mask.Load(int3(p1.x, p1.y, 0));
    const float a = step(0.5f, max(max(ma.r, ma.g), max(ma.b, ma.a)));
    const float b = step(0.5f, max(max(mb.r, mb.g), max(mb.b, mb.a)));
    const float c = step(0.5f, max(max(mc.r, mc.g), max(mc.b, mc.a)));
    const float d = step(0.5f, max(max(md.r, md.g), max(md.b, md.a)));
    const float on = lerp(lerp(a, b, w.x), lerp(c, d, w.x), w.y);
    return lerp(1.0f, Aux.y, on);
}
#endif

float3 Tap(float2 centre, float2 offset, float2 texSize)
{
    const float2 lo = Src.xy + 0.5f;
    const float2 hi = Src.xy + Src.zw - 0.5f;
    const float2 q = clamp(centre + offset, lo, hi);
    if (Mode.x > 0.5f)
    {
#ifdef DOWN_WRESTLER
        const float3 source = Prefilter(LinearBilinear(q));
        return Aux.y < 0.999f ? source * WrestlerHold(q) : source;
#else
        return Prefilter(LinearBilinear(q));
#endif
    }
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
