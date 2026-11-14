// Bloom, going up (task fx1, gpu_postfx.cpp): adds the next smaller level, widened with a 3 x 3 tent filter, to this level.
// Low is the smaller level (read with linear filtering), High the same-size level from the way down. Bloom.w is how much of
// the glow of the smaller level is added back.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 2
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1

#include "postfx_common.hlsli"

COMBINED Texture2D<float4> Low : register(t0, space2);
COMBINED SamplerState LowSamp : register(s0, space2);
COMBINED Texture2D<float4> High : register(t1, space2);
COMBINED SamplerState HighSamp : register(s1, space2);

float4 main(PSInput input) : SV_Target0
{
    uint hw, hh, lw, lh;
    High.GetDimensions(hw, hh);
    Low.GetDimensions(lw, lh);
    const float2 uv = input.Position.xy / float2((float)hw, (float)hh);
    const float2 d = 1.0f / float2((float)lw, (float)lh);
    float3 s = Low.SampleLevel(LowSamp, uv + float2(-d.x, -d.y), 0.0f).rgb;
    s += 2.0f * Low.SampleLevel(LowSamp, uv + float2(0.0f, -d.y), 0.0f).rgb;
    s += Low.SampleLevel(LowSamp, uv + float2(d.x, -d.y), 0.0f).rgb;
    s += 2.0f * Low.SampleLevel(LowSamp, uv + float2(-d.x, 0.0f), 0.0f).rgb;
    s += 4.0f * Low.SampleLevel(LowSamp, uv, 0.0f).rgb;
    s += 2.0f * Low.SampleLevel(LowSamp, uv + float2(d.x, 0.0f), 0.0f).rgb;
    s += Low.SampleLevel(LowSamp, uv + float2(-d.x, d.y), 0.0f).rgb;
    s += 2.0f * Low.SampleLevel(LowSamp, uv + float2(0.0f, d.y), 0.0f).rgb;
    s += Low.SampleLevel(LowSamp, uv + float2(d.x, d.y), 0.0f).rgb;
    const float3 high = High.SampleLevel(HighSamp, uv, 0.0f).rgb;
    return float4(high + s * (Bloom.w / 16.0f), 1.0f);
}
