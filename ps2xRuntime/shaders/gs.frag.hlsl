// Fragment shader of the GS GPU renderer (GSGpuBackend, src/lib/gs/gs_gpu_backend.cpp).
//
// It does what the GS does to one pixel before the blender: look up the texture (wrap modes, nearest
// or bilinear filtering, with the same integer rounding as the CPU renderer), combine it with the
// vertex colour (TFX), and apply the alpha test. Blending and the depth test are fixed-function
// state of the pipeline. All the colour arithmetic is done in whole 0..255 steps like the GS (a
// colour of 0x80 is 1.0 when modulating), then divided by 255 for the render target.
//
// The alpha that leaves this shader is the blend coefficient C of the GS blend equation
// (Cs-Cd)*C+Cd as a fraction where 0x80 = 1.0: the source alpha As, or FIX, clamped to 1 because the
// GPU blend factors are clamped (the clips show As above 0x80 only as 0x81, which does not show).
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 1
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1
//
// Register spaces follow SDL_gpu.h: fragment textures and samplers in space2, uniforms in space3.
// SDL's Vulkan backend binds one COMBINED_IMAGE_SAMPLER per sampler slot, so (as in smoke.frag.hlsl)
// both declarations carry [[vk::combinedImageSampler]] when building SPIR-V.

#ifdef __spirv__
#define COMBINED [[vk::combinedImageSampler]]
#define LOCATION(n) [[vk::location(n)]]
#else
#define COMBINED
#define LOCATION(n)
#endif

COMBINED Texture2D<float4> Tex : register(t0, space2);
COMBINED SamplerState Samp : register(s0, space2);

// Per draw. Four-component members only, so the SPIR-V and DXIL layouts agree.
cbuffer Draw : register(b0, space3)
{
    float4 TexInfo; // x: width, y: height, z: 1/width, w: 1/height (the texture size in texels)
    uint4 Flags;    // x: bit0 textured, bit1 coordinates are texels (UV or sprite) rather than S,T,Q,
                    //    bit2 bilinear, bit3 TCC (texture alpha used), bits 4-5 TFX, bit6 alpha test on,
                    //    bit7 round texel coordinates to 1/16 (sprite with UV), bit8 truncate them (triangle with UV)
    uint4 Wrap;     // x: WMS, y: WMT, z: MINU | MAXU << 16, w: MINV | MAXV << 16 (REGION modes)
    uint4 Test;     // x: ATST, y: AREF, z: blend coefficient (0 = source alpha, 1 = FIX), w: FIX
};

struct PSInput
{
    LOCATION(0) float4 Color : TEXCOORD0;
    LOCATION(1) float3 UVQ : TEXCOORD1;
};

int wrapCoord(int c, int size, uint mode, uint lo, uint hi)
{
    if (mode == 0u)
        return c & (size - 1);                 // REPEAT
    if (mode == 1u)
        return clamp(c, 0, size - 1);          // CLAMP
    if (mode == 2u)
        return min(max(c, (int)lo), (int)hi);  // REGION_CLAMP
    return (c & (int)lo) | (int)hi;            // REGION_REPEAT
}

// One texel, in 0..255 steps. The sampler is nearest with clamp-to-edge, and the coordinate is the
// texel centre, so this is an exact fetch.
float4 fetchTexel(int2 p)
{
    return round(Tex.SampleLevel(Samp, (float2(p) + 0.5f) * TexInfo.zw, 0.0f) * 255.0f);
}

float4 bilerp8(float4 c00, float4 c10, float4 c01, float4 c11, float fx, float fy)
{
    // The CPU renderer integer bilinear filter: 8-bit weights, rounding at each of the three steps.
    float4 top = floor((c00 * (256.0f - fx) + c10 * fx + 128.0f) / 256.0f);
    float4 bottom = floor((c01 * (256.0f - fx) + c11 * fx + 128.0f) / 256.0f);
    return floor((top * (256.0f - fy) + bottom * fy + 128.0f) / 256.0f);
}

float4 main(PSInput input) : SV_Target0
{
    // The CPU renderer truncates interpolated colours to whole steps; the small offset keeps a flat colour
    // (all three vertices equal) from falling to the step below through float rounding.
    float4 vtx = floor(input.Color * 255.0f + 0.01f);
    float3 rgb = vtx.rgb;
    float alpha = vtx.a;

    if ((Flags.x & 1u) != 0u)
    {
        int texW = (int)TexInfo.x;
        int texH = (int)TexInfo.y;
        float u, v;
        if ((Flags.x & 2u) != 0u)
        {
            u = input.UVQ.x;
            v = input.UVQ.y;
        }
        else
        {
            // the CPU renderer's order of operations: S * (1/Q) * size
            float invQ = 1.0f / (abs(input.UVQ.z) > 1.0e-8f ? input.UVQ.z : 1.0f);
            u = input.UVQ.x * invQ * TexInfo.x;
            v = input.UVQ.y * invQ * TexInfo.y;
        }
        if ((Flags.x & 128u) != 0u)
        {
            // GS 12.4 fixed point: a sprite's coordinate is rounded to 1/16 of a texel, a triangle's truncated
            u = floor(u * 16.0f + 0.5f) / 16.0f;
            v = floor(v * 16.0f + 0.5f) / 16.0f;
        }
        else if ((Flags.x & 256u) != 0u)
        {
            u = floor(u * 16.0f + 0.001f) / 16.0f;
            v = floor(v * 16.0f + 0.001f) / 16.0f;
        }
        uint wms = Wrap.x, wmt = Wrap.y;
        uint minU = Wrap.z & 0xFFFFu, maxU = Wrap.z >> 16;
        uint minV = Wrap.w & 0xFFFFu, maxV = Wrap.w >> 16;

        float4 texel;
        if ((Flags.x & 4u) == 0u)
        {
            // nearest: the integer part, truncated toward zero like the CPU renderer
            int pu = wrapCoord((int)u, texW, wms, minU, maxU);
            int pv = wrapCoord((int)v, texH, wmt, minV, maxV);
            texel = fetchTexel(int2(pu, pv));
        }
        else
        {
            float su = u - 0.5f, sv = v - 0.5f;
            float fu = floor(su), fv = floor(sv);
            int u0 = (int)fu, v0 = (int)fv;
            float fx = min(floor((su - fu) * 256.0f), 256.0f);
            float fy = min(floor((sv - fv) * 256.0f), 256.0f);
            int wu0 = wrapCoord(u0, texW, wms, minU, maxU);
            int wu1 = wrapCoord(u0 + 1, texW, wms, minU, maxU);
            int wv0 = wrapCoord(v0, texH, wmt, minV, maxV);
            int wv1 = wrapCoord(v0 + 1, texH, wmt, minV, maxV);
            texel = bilerp8(fetchTexel(int2(wu0, wv0)), fetchTexel(int2(wu1, wv0)),
                            fetchTexel(int2(wu0, wv1)), fetchTexel(int2(wu1, wv1)), fx, fy);
        }

        bool tcc = (Flags.x & 8u) != 0u;
        uint tfx = (Flags.x >> 4u) & 3u;
        float3 tex = texel.rgb;
        float ta = texel.a;
        if (tfx == 0u) // MODULATE: (texel * vertex) >> 7
        {
            rgb = min(floor(tex * vtx.rgb / 128.0f), 255.0f);
            alpha = tcc ? min(floor(ta * vtx.a / 128.0f), 255.0f) : vtx.a;
        }
        else if (tfx == 1u) // DECAL
        {
            rgb = tex;
            alpha = tcc ? ta : vtx.a;
        }
        else if (tfx == 2u) // HIGHLIGHT
        {
            rgb = min(floor(tex * vtx.rgb / 128.0f) + vtx.a, 255.0f);
            alpha = tcc ? min(ta + vtx.a, 255.0f) : vtx.a;
        }
        else // HIGHLIGHT2
        {
            rgb = min(floor(tex * vtx.rgb / 128.0f) + vtx.a, 255.0f);
            alpha = tcc ? ta : vtx.a;
        }
    }

    if ((Flags.x & 64u) != 0u) // alpha test, AFAIL = KEEP: a failing pixel changes nothing
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

    float coefficient = (Test.z == 0u) ? alpha : (float)Test.w;
    return float4(rgb / 255.0f, saturate(coefficient / 128.0f));
}
