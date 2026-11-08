// Fragment shader of the UI layer for textured geometry: texel times vertex colour. Both are premultiplied by
// alpha (RmlUi's font atlases and generated textures already are; the renderer premultiplies the images it
// loads), and the product of two premultiplied colours is the premultiplied colour of the product.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 1
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 0
//
// Register spaces follow SDL_gpu.h, comment on SDL_CreateGPUShader (lines 2711-2741): fragment shaders use
// space2 for textures and samplers. SDL's Vulkan backend binds one combined image sampler per slot, so both
// declarations carry [[vk::combinedImageSampler]] when building SPIR-V (see smoke.frag.hlsl).

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
