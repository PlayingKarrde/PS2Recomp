// Second pass of the "is this picture all black" test (the GS GPU renderer's direct presentation path); see
// field_black1.frag.hlsl. It adds up the per-column counts as integers. The output is one pixel whose red,
// green and blue channels are 1 for "the picture has a pixel that is not black" and 0 for "all black", for
// the displayed picture and the two fallback pictures.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 1
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1
//
// Register spaces follow SDL_gpu.h; see field_deint.frag.hlsl for the rest. Vertex shader: present.vert.hlsl.

#ifdef __spirv__
#define COMBINED [[vk::combinedImageSampler]]
#define LOCATION(n) [[vk::location(n)]]
#else
#define COMBINED
#define LOCATION(n)
#endif

// The vertex shader (present.vert.hlsl) writes the texture coordinate first and SV_Position after it. Direct3D 12
// refuses a pipeline whose pixel shader leaves the first one out, because SV_Position would then be matched to a
// different register than the vertex shader wrote it to. The coordinate itself is not used here.
struct PSInput
{
    LOCATION(0) float2 UV : TEXCOORD0;
    float4 Position : SV_Position;
};

COMBINED Texture2D<float4> Counts : register(t0, space2);
COMBINED SamplerState CountsSamp : register(s0, space2);

cbuffer Params : register(b0, space3)
{
    uint4 Info; // x: width (the number of columns)
};

float4 main(PSInput input) : SV_Target0
{
    const float4 position = input.Position;
    uint total0 = 0u;
    uint total1 = 0u;
    uint total2 = 0u;
    for (uint c = 0u; c < Info.x; ++c)
    {
        const float3 counts = Counts.SampleLevel(CountsSamp, float2(((float)c + 0.5f) / 640.0f, 0.5f), 0.0f).rgb;
        total0 += (uint)(counts.x + 0.5f);
        total1 += (uint)(counts.y + 0.5f);
        total2 += (uint)(counts.z + 0.5f);
    }
    return float4(total0 != 0u ? 1.0f : 0.0f, total1 != 0u ? 1.0f : 0.0f, total2 != 0u ? 1.0f : 0.0f, 1.0f);
}
