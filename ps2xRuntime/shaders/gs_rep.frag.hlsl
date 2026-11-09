// Fragment shader of the GS GPU renderer for textures that a texture pack replaces (task t1b; GSGpuBackend, src/lib/gs/gs_gpu_backend.cpp).
//
// It is gs.frag.hlsl with a different way to fetch the texture colour; the colour arithmetic after the fetch (TFX, alpha test,
// the blend coefficient) is the same code, and the two files must stay in step. The draw is the same draw: the primitive, the wrap
// modes, REGION_CLAMP, the 1/16 rounding of coordinates and STQ all work in the ORIGINAL texel units of the declared texture, exactly
// as in gs.frag.hlsl. Only the final fetch changes:
//   - Sampler 0 is the original texture (decoded from GS memory as before). Sampler 1 is the replacement picture: S times the size of
//     the REPLACED RECTANGLE of the texture (RepInfo), either RGBA8 in the GS's alpha scale (0x80 = opaque) or, for an index image,
//     R8 palette indices. An index picture is looked up in the texture's live palette, which comes in the uniform block (Pal).
//   - A tap is a texel of the picture. Its position in original texels is split into the original texel and the sub-texel inside it
//     (S x S per original texel). The wrap mode is applied to the original texel (so REPEAT, CLAMP and REGION_CLAMP behave as the game
//     wants, and a REGION_CLAMP part never reads its neighbour's pixels), and the sub-texel is kept, except that a clamped coordinate
//     takes the edge sub-texel. A tap whose original texel lies outside the replaced rectangle reads the original texture instead.
//   - The colour is filtered bilinearly AFTER the palette lookup (an index image is never filtered as numbers). The colours are
//     weighted by their alpha, so a transparent palette entry (colour black) does not darken the edge of a sprite.
//   - When the picture is minified (several of its texels fall in one target pixel: a high scale on a low internal resolution) the
//     bilinear tap is repeated on a grid across the pixel's footprint (up to 4 x 4), so the result is a smooth average rather than an
//     aliased single tap. The footprint comes from the derivatives of the texel coordinate.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 2
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1
//
// Register spaces follow SDL_gpu.h: fragment textures and samplers in space2, uniforms in space3. SDL's Vulkan backend binds one
// COMBINED_IMAGE_SAMPLER per sampler slot, so both declarations of a slot carry [[vk::combinedImageSampler]] when building SPIR-V.

#ifdef __spirv__
#define COMBINED [[vk::combinedImageSampler]]
#define LOCATION(n) [[vk::location(n)]]
#else
#define COMBINED
#define LOCATION(n)
#endif

COMBINED Texture2D<float4> Tex : register(t0, space2);
COMBINED SamplerState Samp : register(s0, space2);
COMBINED Texture2D<float4> RepTex : register(t1, space2);
COMBINED SamplerState RepSamp : register(s1, space2);

// Per draw. Four-component members only, so the SPIR-V and DXIL layouts agree.
cbuffer Draw : register(b0, space3)
{
    float4 TexInfo; // x: width, y: height, z: 1/width, w: 1/height (the declared texture size in texels)
    uint4 Flags;    // as in gs.frag.hlsl: x: bit0 textured, bit1 coordinates are texels, bit2 bilinear (ignored here: replaced textures are
                    //    always filtered), bit3 TCC, bits 4-5 TFX, bit6 alpha test, bit7/8 round/truncate to 1/16, bit9/10 sprite sampling
                    // y: target rows per GS pixel, z: target columns per GS pixel (used with bit9)
    uint4 Wrap;     // x: WMS, y: WMT, z: MINU | MAXU << 16, w: MINV | MAXV << 16
    uint4 Test;     // x: ATST, y: AREF, z: blend coefficient (0 = source alpha, 1 = FIX), w: FIX
    uint4 RepInfo;  // x: rectangle x0 | y0 << 16, y: rectangle width | height << 16 (original texels), z: S | kind << 8 (1 RGBA, 2 index),
                    // w: palette index mask (15 or 255)
    uint4 Pal[64];  // the live palette of an index picture: entry i is Pal[i >> 2][i & 3], r | g << 8 | b << 16 | a << 24
};

struct PSInput
{
    LOCATION(0) float4 Color : TEXCOORD0;
    LOCATION(1) float3 UVQ : TEXCOORD1;
    float4 Position : SV_Position; // last, as in the vertex shader's output
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

// One texel of the original texture, in 0..255 steps (an exact fetch: nearest sampler, texel centre).
float4 fetchOriginal(int2 p)
{
    return round(Tex.SampleLevel(Samp, (float2(p) + 0.5f) * TexInfo.zw, 0.0f) * 255.0f);
}

int floorDiv(int a, int b)
{
    int q = a / b;
    return ((a % b) != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// The tap at texel (hx, hy) of the replacement picture's grid, which has S x S texels per original texel. Returns r, g, b, a in 0..255
// steps (a in the GS's scale).
float4 tap(int hx, int hy, int texW, int texH, int S, uint kind, int2 rectO, int2 rectS, uint mask)
{
    const uint wms = Wrap.x, wmt = Wrap.y;
    const uint minU = Wrap.z & 0xFFFFu, maxU = Wrap.z >> 16;
    const uint minV = Wrap.w & 0xFFFFu, maxV = Wrap.w >> 16;
    int ox = floorDiv(hx, S);
    int oy = floorDiv(hy, S);
    int sx = hx - ox * S;
    int sy = hy - oy * S;
    const int wox = wrapCoord(ox, texW, wms, minU, maxU);
    const int woy = wrapCoord(oy, texH, wmt, minV, maxV);
    // A clamped coordinate lands on the edge texel: take that texel's edge sub-texel.
    if (wms == 1u || wms == 2u)
    {
        if (wox > ox)
            sx = 0;
        else if (wox < ox)
            sx = S - 1;
    }
    if (wmt == 1u || wmt == 2u)
    {
        if (woy > oy)
            sy = 0;
        else if (woy < oy)
            sy = S - 1;
    }
    const int2 rel = int2(wox, woy) - rectO;
    if (rel.x >= 0 && rel.y >= 0 && rel.x < rectS.x && rel.y < rectS.y)
    {
        const int2 hp = rel * S + int2(sx, sy);
        const float2 uv = (float2(hp) + 0.5f) / float2(rectS * S);
        const float4 texel = RepTex.SampleLevel(RepSamp, uv, 0.0f);
        if (kind == 1u)
            return round(texel * 255.0f);
        const uint index = (uint)round(texel.r * 255.0f) & mask;
        const uint packed = Pal[index >> 2u][index & 3u];
        return float4((float)(packed & 255u), (float)((packed >> 8u) & 255u), (float)((packed >> 16u) & 255u), (float)(packed >> 24u));
    }
    return fetchOriginal(int2(wox, woy));
}

// Adds one bilinear sample, centred on (pu, pv) in original texel units, with weight `w`: sum.rgb += w * a * rgb, sum.a += w * a (the
// alpha-weighted colour), and `total` += w * a / 128 style bookkeeping is done by the caller from sum.a.
void addBilinear(float pu, float pv, float w, int texW, int texH, int S, uint kind, int2 rectO, int2 rectS, uint mask, inout float4 sum)
{
    const float X = pu * (float)S - 0.5f;
    const float Y = pv * (float)S - 0.5f;
    const float fx0 = floor(X), fy0 = floor(Y);
    const float wx = X - fx0, wy = Y - fy0;
    const int hx = (int)fx0, hy = (int)fy0;
    const float4 c00 = tap(hx, hy, texW, texH, S, kind, rectO, rectS, mask);
    const float4 c10 = tap(hx + 1, hy, texW, texH, S, kind, rectO, rectS, mask);
    const float4 c01 = tap(hx, hy + 1, texW, texH, S, kind, rectO, rectS, mask);
    const float4 c11 = tap(hx + 1, hy + 1, texW, texH, S, kind, rectO, rectS, mask);
    const float w00 = (1.0f - wx) * (1.0f - wy) * w;
    const float w10 = wx * (1.0f - wy) * w;
    const float w01 = (1.0f - wx) * wy * w;
    const float w11 = wx * wy * w;
    sum.rgb += c00.rgb * (c00.a * w00) + c10.rgb * (c10.a * w10) + c01.rgb * (c01.a * w01) + c11.rgb * (c11.a * w11);
    sum.a += c00.a * w00 + c10.a * w10 + c01.a * w01 + c11.a * w11;
}

float4 main(PSInput input) : SV_Target0
{
    // Derivatives first, outside any branch (every pixel of a 2 x 2 block must run the same code).
    const bool texelCoords = (Flags.x & 2u) != 0u;
    float2 uvRaw;
    if (texelCoords)
    {
        uvRaw = input.UVQ.xy;
    }
    else
    {
        // the CPU renderer's order of operations: S * (1/Q) * size
        const float invQ = 1.0f / (abs(input.UVQ.z) > 1.0e-8f ? input.UVQ.z : 1.0f);
        uvRaw = float2(input.UVQ.x * invQ * TexInfo.x, input.UVQ.y * invQ * TexInfo.y);
    }
    const float2 dRdx = ddx(uvRaw);
    const float2 dRdy = ddy(uvRaw);
    const float dudx = ddx(input.UVQ.x);
    const float dvdy = ddy(input.UVQ.y);
    // the footprint of a target pixel, in original texels
    const float2 footprint = float2(max(abs(dRdx.x), abs(dRdy.x)), max(abs(dRdx.y), abs(dRdy.y)));

    float4 vtx = floor(input.Color * 255.0f + 0.01f);
    float u = uvRaw.x;
    float v = uvRaw.y;
    if (texelCoords && (Flags.x & 512u) != 0u)
    {
        // A sprite on a target larger than the GS frame: see gs.frag.hlsl.
        float uGs = u, vGs = v;
        if (Flags.y > 1u)
        {
            const float scaleY = (float)Flags.y;
            vGs += ((floor(input.Position.y / scaleY) + 0.5f) * scaleY - input.Position.y) * dvdy;
        }
        if (Flags.z > 1u)
        {
            const float scaleX = (float)Flags.z;
            uGs += ((floor(input.Position.x / scaleX) + 0.5f) * scaleX - input.Position.x) * dudx;
        }
        if ((Flags.x & 1024u) == 0u)
        {
            u = uGs;
            v = vGs;
        }
        else if ((Flags.x & 4u) != 0u)
        {
            const float fu = floor(uGs - 0.5f), fv = floor(vGs - 0.5f);
            u = clamp(u, fu + 0.5f, fu + 1.5f);
            v = clamp(v, fv + 0.5f, fv + 1.5f);
        }
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

    const int texW = (int)TexInfo.x;
    const int texH = (int)TexInfo.y;
    const int S = (int)(RepInfo.z & 255u);
    const uint kind = (RepInfo.z >> 8u) & 255u;
    const int2 rectO = int2((int)(RepInfo.x & 0xFFFFu), (int)(RepInfo.x >> 16u));
    const int2 rectS = int2((int)(RepInfo.y & 0xFFFFu), (int)(RepInfo.y >> 16u));
    const uint mask = RepInfo.w;

    // How many bilinear taps to spread over a pixel: one while the picture is magnified or about 1 : 1, more when it is minified.
    const int nx = clamp((int)ceil(footprint.x * (float)S * 0.5f), 1, 4);
    const int ny = clamp((int)ceil(footprint.y * (float)S * 0.5f), 1, 4);
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    const float sampleWeight = 1.0f / (float)(nx * ny);
    for (int iy = 0; iy < ny; ++iy)
    {
        const float pv = v + (((float)iy + 0.5f) / (float)ny - 0.5f) * footprint.y;
        for (int ix = 0; ix < nx; ++ix)
        {
            const float pu = u + (((float)ix + 0.5f) / (float)nx - 0.5f) * footprint.x;
            addBilinear(pu, pv, sampleWeight, texW, texH, S, kind, rectO, rectS, mask, sum);
        }
    }
    // sum.a is the average alpha; sum.rgb holds colour * alpha weights, so the colour is sum.rgb / sum.a
    float3 texRgb = float3(0.0f, 0.0f, 0.0f);
    if (sum.a > 1.0e-4f)
        texRgb = sum.rgb / sum.a;
    float4 texel = float4(floor(texRgb + 0.5f), floor(sum.a + 0.5f));

    bool tcc = (Flags.x & 8u) != 0u;
    uint tfx = (Flags.x >> 4u) & 3u;
    float3 rgb;
    float alpha;
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
