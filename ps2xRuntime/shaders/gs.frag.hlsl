// Fragment shader of the GS GPU renderer (GSGpuBackend, src/lib/gs/gs_gpu_backend.cpp).
//
// It does what the GS does to one pixel before the blender: look up the texture (wrap modes, nearest
// or bilinear filtering, with the same integer rounding as the CPU renderer), combine it with the
// vertex colour (TFX), and apply the alpha test. Blending and the depth test are fixed-function
// state of the pipeline. All the colour arithmetic is done in whole 0..255 steps like the GS (a
// colour of 0x80 is 1.0 when modulating), then divided by 255 for the render target.
//
// The alpha that leaves this shader is the blend coefficient C of the GS blend equation
// (Cs-Cd)*C+Cd as a fraction where 0x80 = 1.0: the source alpha As, or FIX, clamped to 1 because the
// GPU blend factors are clamped (the clips show As above 0x80 only as 0x81, which does not show).
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 1
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1
//
// Register spaces follow SDL_gpu.h: fragment textures and samplers in space2, uniforms in space3.
// SDL's Vulkan backend binds one COMBINED_IMAGE_SAMPLER per sampler slot, so (as in smoke.frag.hlsl)
// both declarations carry [[vk::combinedImageSampler]] when building SPIR-V.

#include "gs_frag_body.hlsli"

float4 main(PSInput input) : SV_Target0
{
    return GsColour(input);
}
