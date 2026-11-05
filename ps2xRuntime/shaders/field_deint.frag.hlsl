// Fragment shader that builds the game's picture from its field pictures (the GS GPU renderer's direct
// presentation path, src/lib/gs/gs_gpu_present.cpp). It is a port of the CPU deinterlacer, GSFieldDeinterlacer
// in include/runtime/gs/gs_field_deinterlacer.h, and keeps its rules exactly, in whole 0..255 steps:
//
//   - The game draws one 224-line field per VBlank, every other field half a line lower. The picture is
//     448 lines. The lines of the newest field (Cur) stay: output row 2y + 1 when the newest field is the
//     lower one (State.r above half), else 2y.
//   - The lines between come from the previous field (Prev, row y) where nothing moved, and are the
//     average of the two newest-field lines around them, (a + b + 1) / 2, where something did.
//   - "Nothing moved" at x, y: the largest colour-channel difference between Cur and Older (the field
//     before Prev: the same line parity as Cur) over the 3 x 2 pixels around the output pixel (columns
//     x - 1 .. x + 1, rows `above` and `below`, clamped to the field) is at most 16. With no Older it
//     is always "moved" (255). With no Prev the lines are always interpolated.
//   - Mode 1 shows a progressive picture as it is (the CPU path copies it).
// Which way the newest field is offset ("lower") is measured by field_lower1/field_lower2.frag.hlsl and
// arrives in State. The alpha channel of the fields is the GS blend coefficient, not a colour, and the CPU
// picture has alpha 255 everywhere, so this shader ignores it and writes 1.
//
// Every fetch is a nearest-sampler read at a texel centre, so it is exact; the arithmetic is integer.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 4
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1
//
// Register spaces follow SDL_gpu.h (SDL_CreateGPUShader): fragment textures and samplers in space2,
// uniforms in space3. Texture and sampler carry [[vk::combinedImageSampler]] under __spirv__ as in
// present.frag.hlsl. The vertex shader is present.vert.hlsl (a rectangle in target pixels).

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

COMBINED Texture2D<float4> Cur : register(t0, space2);
COMBINED SamplerState CurSamp : register(s0, space2);
COMBINED Texture2D<float4> Prev : register(t1, space2);
COMBINED SamplerState PrevSamp : register(s1, space2);
COMBINED Texture2D<float4> Older : register(t2, space2);
COMBINED SamplerState OlderSamp : register(s2, space2);
COMBINED Texture2D<float4> State : register(t3, space2);
COMBINED SamplerState StateSamp : register(s3, space2);

cbuffer Params : register(b0, space3)
{
    uint4 Info; // x: field width, y: field height, z: has Prev, w: has Older
    uint4 Mode; // x: 0 deinterlace, 1 copy the field as a progressive picture
};

static const float2 kInvSize = float2(1.0f / 640.0f, 1.0f / 512.0f); // the field textures are 640 x 512

uint3 fetchCur(int2 p)
{
    return (uint3)round(Cur.SampleLevel(CurSamp, (float2(p) + 0.5f) * kInvSize, 0.0f).rgb * 255.0f);
}

uint3 fetchPrev(int2 p)
{
    return (uint3)round(Prev.SampleLevel(PrevSamp, (float2(p) + 0.5f) * kInvSize, 0.0f).rgb * 255.0f);
}

uint3 fetchOlder(int2 p)
{
    return (uint3)round(Older.SampleLevel(OlderSamp, (float2(p) + 0.5f) * kInvSize, 0.0f).rgb * 255.0f);
}

// The largest colour-channel difference between the newest field and the older one at this pixel.
uint motionAt(int2 p)
{
    const int3 d = abs(int3(fetchCur(p)) - int3(fetchOlder(p)));
    return (uint)max(d.x, max(d.y, d.z));
}

float4 main(PSInput input) : SV_Target0
{
    const float4 position = input.Position;
    const int x = (int)position.x;
    const int row = (int)position.y;
    const int width = (int)Info.x;
    const int height = (int)Info.y;

    if (Mode.x == 1u)
        return float4((float3)fetchCur(int2(x, row)) / 255.0f, 1.0f);

    const bool lower = State.SampleLevel(StateSamp, float2(0.5f, 0.5f), 0.0f).r > 0.5f;
    const int y = row >> 1;
    if ((row & 1) == (lower ? 1 : 0))
        return float4((float3)fetchCur(int2(x, y)) / 255.0f, 1.0f); // a line of the newest field

    const int above = lower ? max(y - 1, 0) : y;
    const int below = lower ? y : min(y + 1, height - 1);

    uint motion = 255u;
    if (Info.w != 0u)
    {
        const int left = max(x - 1, 0);
        const int right = min(x + 1, width - 1);
        motion = max(max(motionAt(int2(left, above)), max(motionAt(int2(x, above)), motionAt(int2(right, above)))),
                     max(motionAt(int2(left, below)), max(motionAt(int2(x, below)), motionAt(int2(right, below)))));
    }
    if (Info.z != 0u && motion <= 16u)
        return float4((float3)fetchPrev(int2(x, y)) / 255.0f, 1.0f); // nothing moved: the previous field's line

    const uint3 a = fetchCur(int2(x, above));
    const uint3 b = fetchCur(int2(x, below));
    return float4((float3)((a + b + 1u) / 2u) / 255.0f, 1.0f);
}
