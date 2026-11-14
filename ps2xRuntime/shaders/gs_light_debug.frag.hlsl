// Fragment shader of the debug views of the match lights (task lt2-L0b; PS2X_LIGHT_DEBUG=position|normal).
//
// It draws a scene triangle with a colour made from the world position or the normal the renderer recovered for it
// (gpu/gpu_lighting.h), in place of the game's colour, so that the recovery can be judged by eye:
//   position  the colour is the height (y): the mat, at y 100, is one flat colour, the floor another, the post tops a third;
//             darker lines every 100 units of x and z make a grid (the ring is 800 x 800: eight squares each way).
//   normal    the colour is the normal: red = x, green = y (up is green), blue = z, each from -1 to 1 as 0 to 1.
// The alpha, the alpha test and so the cut-out shapes are the game's: the texture is sampled as gs.frag.hlsl does for
// S, T, Q coordinates, with the nearest texel only (a debug picture needs no filtering), for its alpha alone.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 1
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 2

#ifdef __spirv__
#define COMBINED [[vk::combinedImageSampler]]
#define LOCATION(n) [[vk::location(n)]]
#else
#define COMBINED
#define LOCATION(n)
#endif

COMBINED Texture2D<float4> Tex : register(t0, space2);
COMBINED SamplerState Samp : register(s0, space2);

// Per draw: the same block as gs.frag.hlsl.
cbuffer Draw : register(b0, space3)
{
    float4 TexInfo;
    uint4 Flags;
    uint4 Wrap;
    uint4 Test;
};

cbuffer Light : register(b1, space3)
{
    uint4 Mode; // x: 1 position, 2 normal
};

struct PSInput
{
    LOCATION(0) float4 Color : TEXCOORD0;
    LOCATION(1) float3 UVQ : TEXCOORD1;
    LOCATION(2) float3 WorldQ : TEXCOORD2;
    LOCATION(3) float3 Normal : TEXCOORD3;
    float4 Position : SV_Position;
};

int wrapCoord(int c, int size, uint mode, uint lo, uint hi)
{
    if (mode == 0u)
        return c & (size - 1);
    if (mode == 1u)
        return clamp(c, 0, size - 1);
    if (mode == 2u)
        return min(max(c, (int)lo), (int)hi);
    return (c & (int)lo) | (int)hi;
}

// Height to colour: a ramp through five colours from y = -50 to y = 350.
float3 heightColour(float y)
{
    const float3 stops[5] = {float3(0.10f, 0.15f, 0.55f), float3(0.10f, 0.60f, 0.65f), float3(0.25f, 0.75f, 0.25f),
                             float3(0.90f, 0.80f, 0.20f), float3(0.85f, 0.25f, 0.20f)};
    float t = clamp((y + 50.0f) / 100.0f, 0.0f, 3.999f);
    int i = (int)t;
    return lerp(stops[i], stops[i + 1], t - (float)i);
}

float4 main(PSInput input) : SV_Target0
{
    float4 vtx = floor(input.Color * 255.0f + 0.01f);
    float alpha = vtx.a;
    float invQ = 1.0f / (abs(input.UVQ.z) > 1.0e-8f ? input.UVQ.z : 1.0f);

    if ((Flags.x & 1u) != 0u)
    {
        int texW = (int)TexInfo.x;
        int texH = (int)TexInfo.y;
        float u = input.UVQ.x * invQ * TexInfo.x;
        float v = input.UVQ.y * invQ * TexInfo.y;
        int pu = wrapCoord((int)u, texW, Wrap.x, Wrap.z & 0xFFFFu, Wrap.z >> 16);
        int pv = wrapCoord((int)v, texH, Wrap.y, Wrap.w & 0xFFFFu, Wrap.w >> 16);
        float ta = round(Tex.SampleLevel(Samp, (float2(pu, pv) + 0.5f) * TexInfo.zw, 0.0f).a * 255.0f);
        bool tcc = (Flags.x & 8u) != 0u;
        uint tfx = (Flags.x >> 4u) & 3u;
        if (tfx == 0u)
            alpha = tcc ? min(floor(ta * vtx.a / 128.0f), 255.0f) : vtx.a;
        else if (tfx == 1u)
            alpha = tcc ? ta : vtx.a;
        else if (tfx == 2u)
            alpha = tcc ? min(ta + vtx.a, 255.0f) : vtx.a;
        else
            alpha = tcc ? ta : vtx.a;
    }

    if ((Flags.x & 64u) != 0u)
    {
        uint a = (uint)alpha;
        uint aref = Test.y;
        bool pass;
        switch (Test.x)
        {
        case 0u: pass = false; break;
        case 1u: pass = true; break;
        case 2u: pass = a < aref; break;
        case 3u: pass = a <= aref; break;
        case 4u: pass = a == aref; break;
        case 5u: pass = a >= aref; break;
        case 6u: pass = a > aref; break;
        default: pass = a != aref; break;
        }
        if (!pass)
            discard;
    }

    float3 world = input.WorldQ * invQ;
    float3 rgb;
    if (Mode.x == 2u)
    {
        rgb = normalize(input.Normal) * 0.5f + 0.5f;
    }
    else
    {
        rgb = heightColour(world.y);
        // The grid: a line 3 units wide at every 100 units of x and of z.
        float2 cell = abs(frac(world.xz / 100.0f + 0.5f) - 0.5f) * 100.0f;
        if (min(cell.x, cell.y) < 1.5f)
            rgb *= 0.45f;
    }

    float coefficient = (Test.z == 0u) ? alpha : (float)Test.w;
    return float4(rgb, saturate(coefficient / 128.0f));
}
