// Fragment shader that puts GS memory contents into the GS GPU renderer depth target (GSGpuBackend).
//
// The game uploads CT32 movie frames into the pages of its Z buffer (entrance movie), and later Z tests
// read that data as if it were depth, as on the real GS. The depth target lives only on the GPU, so
// after such an upload the backend decodes the affected Z buffer pages from its GS memory image (Z24,
// swizzled like the GS does), packs each depth value into the R, G, B bytes of an RGBA8 texture, and
// draws the pages as quads with this shader. It writes that value, divided by 2^24, as the pixel depth
// (the depth test of the pipeline is ALWAYS, depth writes are on, colour writes are masked off).
//
// The texture coordinates are normalised and nearest-sampled, so a pixel gets exactly the texel under it.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 1
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 0
//
// Register spaces follow SDL_gpu.h: fragment textures and samplers in space2. The combined image sampler
// attributes are needed for SPIR-V only (see gs.frag.hlsl).

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
    LOCATION(1) float3 UVQ : TEXCOORD1;
};

struct PSOutput
{
    float4 Color : SV_Target0; // masked off by the pipeline, but the colour target is bound
    float Depth : SV_Depth;
};

PSOutput main(PSInput input)
{
    float4 t = round(Tex.SampleLevel(Samp, input.UVQ.xy, 0.0f) * 255.0f);
    PSOutput output;
    output.Color = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.Depth = (t.r + t.g * 256.0f + t.b * 65536.0f) / 16777216.0f;
    return output;
}
