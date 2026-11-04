// Fragment shader that puts the game's picture on the screen (the host's SDL_GPU presenter).
//
// It reads the picture texture and writes the colour unchanged, with the texture's own alpha; the
// pipeline blends that over a black clear exactly as SDL_BLENDMODE_BLEND did for the SDL_Renderer path.
// Which scale mode is in use (PS2X_SCALE) is decided by the sampler the host binds and by the Mode
// value below:
//   Mode 0: one texture sample. The host binds a nearest sampler for PS2X_SCALE=nearest and a linear
//           sampler for bilinear.
//   Mode 1: "sharp", SDL's SDL_SCALEMODE_PIXELART. The host binds a linear sampler.
//
// ---------------------------------------------------------------------------------------------
// GetPixelArtUV() and the SampleGrad() call below are a port of SDL's pixel-art scaling, SDL 3.4.18
// (tag release-3.4.18, 2026-10-02):
//   src/render/direct3d11/D3D11_PixelShader_Common.hlsli, lines 152-165 (GetPixelArtUV) and lines
//   175-177 (TEXTURETYPE_RGB_PIXELART: SampleGrad with the derivatives of the original coordinate).
//   The same algorithm in GLSL is in src/render/opengl/SDL_shaders_gl.c, lines 139-152.
//   The sampler it needs is linear (src/render/direct3d11/SDL_render_d3d11.c line 2446:
//   "case SDL_SCALEMODE_PIXELART: // Uses linear sampling"), and the constants come from
//   D3D11_SetupShaderConstants (same file, lines 2200-2222): texel size = 1 / texture size.
// SDL is under the zlib licence, whose notice is kept here:
//
//   Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
//
//   This software is provided 'as-is', without any express or implied warranty. In no event will
//   the authors be held liable for any damages arising from the use of this software.
//   Permission is granted to anyone to use this software for any purpose, including commercial
//   applications, and to alter it and redistribute it freely, subject to the following restrictions:
//   1. The origin of this software must not be misrepresented; you must not claim that you wrote the
//      original software. If you use this software in a product, an acknowledgment in the product
//      documentation would be appreciated but is not required.
//   2. Altered source versions must be plainly marked as such, and must not be misrepresented as
//      being the original software.
//   3. This notice may not be removed or altered from any source distribution.
//
// This file is an altered source version: the algorithm is SDL's, the surrounding code is ours.
// ---------------------------------------------------------------------------------------------
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 1
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1
//
// Register spaces follow SDL_gpu.h, comment on SDL_CreateGPUShader (lines 2711-2741): fragment
// shaders use space2 for sampled textures and samplers, and space3 for uniform buffers.
// SDL's Vulkan backend binds one COMBINED_IMAGE_SAMPLER per sampler slot (SDL_gpu_vulkan.c lines
// 3896 and 5225), so the texture and sampler carry [[vk::combinedImageSampler]] under __spirv__
// (see smoke.frag.hlsl).

#ifdef __spirv__
#define COMBINED [[vk::combinedImageSampler]]
#define LOCATION(n) [[vk::location(n)]]
#else
#define COMBINED
#define LOCATION(n)
#endif

COMBINED Texture2D<float4> Tex : register(t0, space2);
COMBINED SamplerState Samp : register(s0, space2);

cbuffer Params : register(b0, space3)
{
    float4 TexelSize; // 1/w, 1/h, w, h of the whole picture texture (SDL's texel_size)
    float4 Mode;      // x: 0 = plain sample, 1 = pixel art; yzw unused
};

struct PSInput
{
    LOCATION(0) float2 UV : TEXCOORD0;
};

// SDL D3D11_PixelShader_Common.hlsli lines 152-165. `uvWidth` is fwidth(uv), taken by the caller
// outside the branch below so every pixel of a quad computes it.
float2 GetPixelArtUV(float2 uv, float2 uvWidth)
{
    // box filter size in texel units
    float2 boxSize = clamp(uvWidth * TexelSize.zw, 1e-5, 1);

    // scale uv by texture size to get texel coordinate
    float2 tx = uv * TexelSize.zw - 0.5 * boxSize;

    // compute offset for pixel-sized box filter
    float2 txOffset = smoothstep(1 - boxSize, 1, frac(tx));

    // compute bilinear sample uv coordinates
    return (floor(tx) + 0.5 + txOffset) * TexelSize.xy;
}

float4 main(PSInput input) : SV_Target0
{
    const float2 uvWidth = fwidth(input.UV);
    const float2 ddxUV = ddx(input.UV);
    const float2 ddyUV = ddy(input.UV);
    if (Mode.x > 0.5f)
    {
        const float2 uv = GetPixelArtUV(input.UV, uvWidth);
        return Tex.SampleGrad(Samp, uv, ddxUV, ddyUV);
    }
    return Tex.Sample(Samp, input.UV);
}
