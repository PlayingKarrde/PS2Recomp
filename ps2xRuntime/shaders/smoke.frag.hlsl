// Smoke-test fragment shader for the SDL_GPU toolchain check (ps2x_gpu_smoke).
//
// Samples one texture and multiplies it by the interpolated vertex colour. Blending and the depth
// test are fixed-function state set up by the program, not by this shader.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 1
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 0
//
// Register spaces follow SDL_gpu.h, comment on SDL_CreateGPUShader (lines 2711-2741): fragment
// shaders use space2 for sampled textures, samplers and storage, and space3 for uniform buffers.
//
// SDL's Vulkan backend binds one COMBINED_IMAGE_SAMPLER per sampler slot (SDL_gpu_vulkan.c
// lines 3896 and 5225), but dxc normally writes a separate image and sampler. The
// [[vk::combinedImageSampler]] attribute on both declarations, at the same set and binding, makes
// dxc emit one combined object. DXIL does not need it, so it is only used when building SPIR-V.

#ifdef __spirv__
#define COMBINED [[vk::combinedImageSampler]]
#define LOCATION(n) [[vk::location(n)]]
#else
#define COMBINED
#define LOCATION(n)
#endif

COMBINED Texture2D<float4> Tex : register(t0, space2);
COMBINED SamplerState Samp : register(s0, space2);

struct PSInput
{
    LOCATION(0) float4 Color : TEXCOORD0;
    LOCATION(1) float2 UV : TEXCOORD1;
};

float4 main(PSInput input) : SV_Target0
{
    return Tex.Sample(Samp, input.UV) * input.Color;
}
