// Second pass of the "which way is the newest field offset" measurement (the GS GPU renderer's direct
// presentation path); see field_lower1.frag.hlsl. It adds up the per-column sums as integers and decides
// like GSFieldDeinterlacer::currentIsLower:
//     lower = lowerError < upperError, and on a tie the answer flips the previous one.
// The output is one pixel, 1 (lower) or 0, in the red channel of the next State texture; the previous
// State is read from Prev.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 2
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

COMBINED Texture2D<float4> Sums : register(t0, space2);
COMBINED SamplerState SumsSamp : register(s0, space2);
COMBINED Texture2D<float4> Prev : register(t1, space2);
COMBINED SamplerState PrevSamp : register(s1, space2);

cbuffer Params : register(b0, space3)
{
    uint4 Info; // x: field width, y: field height, z: number of sampled columns (the same block as field_lower1)
};

float4 main(PSInput input) : SV_Target0
{
    const float4 position = input.Position;
    uint lowerSum = 0u;
    uint upperSum = 0u;
    for (uint c = 0u; c < Info.z; ++c)
    {
        const float2 sums = Sums.SampleLevel(SumsSamp, float2(((float)c + 0.5f) / 160.0f, 0.5f), 0.0f).rg;
        lowerSum += (uint)(sums.x + 0.5f);
        upperSum += (uint)(sums.y + 0.5f);
    }
    const bool previousLower = Prev.SampleLevel(PrevSamp, float2(0.5f, 0.5f), 0.0f).r > 0.5f;
    const bool lower = (lowerSum == upperSum) ? !previousLower : (lowerSum < upperSum);
    return lower ? float4(1.0f, 1.0f, 1.0f, 1.0f) : float4(0.0f, 0.0f, 0.0f, 1.0f);
}
