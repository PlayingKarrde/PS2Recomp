// First pass of the "is this picture all black" test (the GS GPU renderer's direct presentation path).
//
// Compose (src/lib/gs/gs_presentation.cpp) replaces a black display frame at frame buffer 0 by the first
// non-black one of two other frames. "Black" there means: every pixel of the displayed width x height region
// has red, green and blue all zero (alpha is ignored). The host has to decide that on the GPU, so this pass
// makes one output pixel per column x that counts, down the whole height, the pixels of each of three
// pictures (the displayed region and the two fallback frames' regions) that are not black. The next pass
// (field_black2.frag.hlsl) adds the columns up. The counts are whole numbers below 2^10, exact in floats.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 3
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

COMBINED Texture2D<float4> Pic0 : register(t0, space2);
COMBINED SamplerState Samp0 : register(s0, space2);
COMBINED Texture2D<float4> Pic1 : register(t1, space2);
COMBINED SamplerState Samp1 : register(s1, space2);
COMBINED Texture2D<float4> Pic2 : register(t2, space2);
COMBINED SamplerState Samp2 : register(s2, space2);

cbuffer Params : register(b0, space3)
{
    uint4 Info; // x: width, y: height, z: number of fallback pictures (0, 1 or 2)
};

static const float2 kInvSize = float2(1.0f / 640.0f, 1.0f / 512.0f);

bool nonBlack0(int x, int y)
{
    return any(Pic0.SampleLevel(Samp0, (float2(x, y) + 0.5f) * kInvSize, 0.0f).rgb > 0.0f);
}

bool nonBlack1(int x, int y)
{
    return any(Pic1.SampleLevel(Samp1, (float2(x, y) + 0.5f) * kInvSize, 0.0f).rgb > 0.0f);
}

bool nonBlack2(int x, int y)
{
    return any(Pic2.SampleLevel(Samp2, (float2(x, y) + 0.5f) * kInvSize, 0.0f).rgb > 0.0f);
}

float4 main(PSInput input) : SV_Target0
{
    const float4 position = input.Position;
    const int x = (int)position.x;
    if (x >= (int)Info.x)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    int count0 = 0;
    int count1 = 0;
    int count2 = 0;
    for (int y = 0; y < (int)Info.y; ++y)
    {
        count0 += nonBlack0(x, y) ? 1 : 0;
        if (Info.z >= 1u)
            count1 += nonBlack1(x, y) ? 1 : 0;
        if (Info.z >= 2u)
            count2 += nonBlack2(x, y) ? 1 : 0;
    }
    return float4((float)count0, (float)count1, (float)count2, 0.0f);
}
