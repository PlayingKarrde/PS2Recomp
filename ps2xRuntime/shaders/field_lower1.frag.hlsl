// First pass of the "which way is the newest field offset" measurement (the GS GPU renderer's direct
// presentation path). It ports the sums of GSFieldDeinterlacer::currentIsLower (include/runtime/gs/
// gs_field_deinterlacer.h) and the next pass (field_lower2.frag.hlsl) compares them.
//
// The CPU code walks every line pair (y, y + 1) of the field and every fourth pixel x, reads the green
// channel, and adds up two errors against the previous field's two lines:
//     between = Prev.g[y] + Prev.g[y + 1]
//     lower  += | 2 * Cur.g[y]     - between |
//     upper  += | 2 * Cur.g[y + 1] - between |
// This pass makes one output pixel per sampled column c (x = 4c) that holds that column's two sums down
// the whole field, as floats (R32G32_FLOAT; the sums are whole numbers below 2^17, which a float holds
// exactly). The next pass adds the columns up in integers.
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

COMBINED Texture2D<float4> Cur : register(t0, space2);
COMBINED SamplerState CurSamp : register(s0, space2);
COMBINED Texture2D<float4> Prev : register(t1, space2);
COMBINED SamplerState PrevSamp : register(s1, space2);

cbuffer Params : register(b0, space3)
{
    uint4 Info; // x: field width, y: field height, z: number of sampled columns (ceil(width / 4))
};

static const float2 kInvSize = float2(1.0f / 640.0f, 1.0f / 512.0f);

int greenCur(int x, int y)
{
    return (int)round(Cur.SampleLevel(CurSamp, (float2(x, y) + 0.5f) * kInvSize, 0.0f).g * 255.0f);
}

int greenPrev(int x, int y)
{
    return (int)round(Prev.SampleLevel(PrevSamp, (float2(x, y) + 0.5f) * kInvSize, 0.0f).g * 255.0f);
}

float2 main(PSInput input) : SV_Target0
{
    const float4 position = input.Position;
    const uint column = (uint)position.x;
    if (column >= Info.z)
        return float2(0.0f, 0.0f);
    const int x = (int)column * 4;
    const int height = (int)Info.y;

    int lowerSum = 0;
    int upperSum = 0;
    int cur0 = greenCur(x, 0);
    int prev0 = greenPrev(x, 0);
    for (int y = 0; y + 1 < height; ++y)
    {
        const int cur1 = greenCur(x, y + 1);
        const int prev1 = greenPrev(x, y + 1);
        const int between = prev0 + prev1;
        lowerSum += abs(2 * cur0 - between);
        upperSum += abs(2 * cur1 - between);
        cur0 = cur1;
        prev0 = prev1;
    }
    return float2((float)lowerSum, (float)upperSum);
}
