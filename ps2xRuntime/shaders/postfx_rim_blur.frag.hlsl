// Wrestler shape, the blur of the wrestler mask (task lt3-T1, gpu_postfx.cpp; docs/hd-lighting-tier3.md in the game's repository).
//
// The mask (gs_mask.frag.hlsl) is 1 where a wrestler is and 0 elsewhere, one channel per wrestler. Blurred, it becomes a soft hill
// over each wrestler: about 1 well inside him, 1/2 on his outline, 0 well outside. The pass after this one (postfx_rim.frag.hlsl)
// reads how far down the hill a pixel is and which way the hill falls there, which is "how close to the outline, and which way is
// out". This shader is one direction of that blur (Gaussian); it is run twice, across and then down.
//
// The pass draws into a texture of the picture's size. It reads Tex from Src.xy on (the picture's place in the frame target for
// the first direction, 0 for the second) and counts everything outside the picture as 0 (no wrestler).
//   Aux.xy   the direction: (1, 0) or (0, 1)
//   Tilt.z   the blur's sigma in texels of the target (the outline width in wrestler texels x the size of one such texel on screen)
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 1
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1

#include "postfx_common.hlsli"

COMBINED Texture2D<float4> Tex : register(t0, space2);
COMBINED SamplerState Samp : register(s0, space2);

#define MAX_TAPS 64

float4 main(PSInput input) : SV_Target0
{
    const int2 p = int2(input.Position.xy);
    const int2 size = int2(Src.zw);
    const int2 direction = int2(Aux.xy);
    const float sigma = max(Tilt.z, 0.25f);
    const int reach = (int)min(ceil(sigma * 2.5f), (float)MAX_TAPS);
    const float k = -0.5f / (sigma * sigma);
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float weights = 0.0f;
    for (int i = -MAX_TAPS; i <= MAX_TAPS; ++i)
    {
        if (i < -reach || i > reach)
            continue;
        const float w = exp(k * (float)(i * i));
        weights += w;
        const int2 q = p + direction * i;
        if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y)
            continue; // outside the picture: no wrestler
        sum += w * Tex.Load(int3(int2(Src.xy) + q, 0));
    }
    return sum / weights;
}
