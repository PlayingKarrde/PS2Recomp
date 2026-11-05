// Chooses the picture of a field and copies it into the presenter's own texture (the GS GPU renderer's
// direct presentation path). It does what Compose's fallback does (src/lib/gs/gs_presentation.cpp,
// copySource): the displayed region is the picture, unless it is black and a fallback frame is not, in which
// case the first non-black fallback frame is. With no fallback frames the pass is a plain copy. The flags
// (field_black2.frag.hlsl) say, in red, green and blue, whether the displayed picture and the first and
// second fallback picture are NOT black.
//
// The output alpha is written as 1: the alpha channel of the GS frame is the blend coefficient, not a
// colour, and the CPU picture has alpha 255.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 4
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
COMBINED Texture2D<float4> Flags : register(t3, space2);
COMBINED SamplerState FlagsSamp : register(s3, space2);

cbuffer Params : register(b0, space3)
{
    uint4 Info; // x: width, y: height, z: number of fallback pictures (0, 1 or 2)
};

static const float2 kInvSize = float2(1.0f / 640.0f, 1.0f / 512.0f);

float4 main(PSInput input) : SV_Target0
{
    const float4 position = input.Position;
    const float2 uv = (floor(position.xy) + 0.5f) * kInvSize;
    float3 rgb = Pic0.SampleLevel(Samp0, uv, 0.0f).rgb;
    if (Info.z != 0u)
    {
        const float3 nonBlack = Flags.SampleLevel(FlagsSamp, float2(0.5f, 0.5f), 0.0f).rgb;
        if (nonBlack.x < 0.5f)
        {
            if (Info.z >= 1u && nonBlack.y > 0.5f)
                rgb = Pic1.SampleLevel(Samp1, uv, 0.0f).rgb;
            else if (Info.z >= 2u && nonBlack.z > 0.5f)
                rgb = Pic2.SampleLevel(Samp2, uv, 0.0f).rgb;
        }
    }
    return float4(rgb, 1.0f);
}
