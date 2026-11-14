// Tilt-shift blur (task fx1, gpu_postfx.cpp): one direction of a separable blur whose radius depends on the screen row only.
// Rows near the focus centre stay sharp; the picture blurs above and below, which is what makes a scene look like a miniature.
// It never looks at the depth buffer: the game's depth is layer-biased and is no distance (docs/formats/fpr-model.md).
//
// The pass draws a picture-sized texture. It reads Tex from (Src.xy) on and clamps to the picture, so the rows below the field
// and the margins of the frame target never leak in. 13 taps spread over +-radius, Gaussian weights, linear filtering.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 1
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1

#include "postfx_common.hlsli"

COMBINED Texture2D<float4> Tex : register(t0, space2);
COMBINED SamplerState Samp : register(s0, space2);

float4 main(PSInput input) : SV_Target0
{
    uint tw, th;
    Tex.GetDimensions(tw, th);
    const float2 texSize = float2((float)tw, (float)th);
    const float2 p = input.Position.xy;
    const float radius = TiltAmount(p.y / Src.w) * Tilt.w;
    const float2 base = Src.xy + p;
    if (radius < 0.05f)
        return Tex.Load(int3(int2(base), 0));
    const float2 lo = Src.xy + 0.5f;
    const float2 hi = Src.xy + Src.zw - 0.5f;
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float total = 0.0f;
    [unroll]
    for (int i = -6; i <= 6; ++i)
    {
        const float f = (float)i / 6.0f;
        const float w = exp(-4.5f * f * f);
        const float2 q = clamp(base + Aux.xy * (f * radius), lo, hi);
        sum += Tex.SampleLevel(Samp, q / texSize, 0.0f) * w;
        total += w;
    }
    return sum / total;
}
