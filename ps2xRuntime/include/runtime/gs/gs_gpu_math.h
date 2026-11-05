#pragma once

// FPWR: the pure arithmetic of the GS GPU renderer (GSGpuBackend), kept apart from SDL so it can be
// unit-tested: turning GS primitives into GPU vertices, the blend equation into fixed-function blend
// factors, the per-draw uniform block, and which 8 KiB GS memory pages a texture or transfer touches.
//
// Where the CPU renderer (gs_cpu_backend.cpp) decides which pixels a primitive covers, this file
// reproduces its decisions in the terms a GPU rasteriser uses:
//   - GS positions are 12.4 fixed point minus the XYOFFSET; the GS samples a triangle at integer pixel
//     positions, a GPU at pixel centres, so triangle vertices are moved by half a pixel. The top-left
//     fill rule is the same on both. Sprites cover whole pixels [x0, x0+w) x [y0, y0+h) after the
//     coordinates are truncated to integers, so their corners are used as they are.
//   - Z is the 24-bit GS value divided by 2^24, which a 32-bit float holds exactly.
//   - The target can be larger than the GS frame by a factor per axis (TargetMapping); in progressive field mode
//     (GPU renderer M4a) it is twice as tall and the half line by which the game offsets alternate fields is removed.
// Nothing here includes SDL, and no header that generated game code includes refers to this one.

#include "runtime/gs/gs_types.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace GSGpu
{
    // One vertex as the GPU receives it. Layout: pos float2 @0, depth float @8, colour UBYTE4_NORM @12,
    // uvq float3 @16 (see gs.vert.hlsl).
    struct Vertex
    {
        float x, y;     // target pixels (GS pixel + 0.5 for triangles), origin top-left
        float z;        // GS Z / 2^24
        uint32_t rgba;  // r | g << 8 | b << 16 | a << 24
        float u, v, q;  // texel coordinates (q = 1), or the GS's S, T, Q
    };
    static_assert(sizeof(Vertex) == 28, "the vertex layout is described to SDL_GPU by offset");

    constexpr float kDepthScale = 1.0f / 16777216.0f;

    inline uint32_t PackRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
    }

    // Result of converting one primitive. `count` vertices (3 for a triangle, 6 for a sprite quad) are
    // in `v`. The bounding box is in GS pixel coordinates (the pixels the primitive can touch before
    // the scissor), for culling.
    struct Setup
    {
        Vertex v[6];
        uint32_t count = 0;
        int32_t minX = 0, minY = 0, maxX = -1, maxY = -1;
        bool texelCoords = false; // v.u/v.v are texel coordinates (UV and every sprite) rather than S, T, Q
    };

    inline float SafeQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f; // the CPU renderer's fabsQ (it keeps the sign)
    }

    inline bool BoxOutsideScissor(const Setup &s, const GSScissorReg &sc)
    {
        return s.maxX < static_cast<int32_t>(sc.x0) || s.minX > static_cast<int32_t>(sc.x1) ||
               s.maxY < static_cast<int32_t>(sc.y0) || s.minY > static_cast<int32_t>(sc.y1);
    }

    // How GS pixels map to target pixels: `sx` and `sy` target pixels per GS pixel along each axis.
    //
    // Weave mode (progressive == false) draws each of the game's 640 x 224 fields as it is, at `scale` times that size
    // (sx == sy == scale), and a later pass weaves the fields into 448 lines.
    //
    // Progressive mode draws every field as a full-height picture: the target is twice as tall (sy == 2 * scale) and the
    // two kinds of field of the game come out identical, which needs one thing here. The game draws alternate fields with
    // the XYOFFSET Y offset (OFY) half a line apart (1936.0 and 1936.5; bringup.md "GPU renderer", M4a) and sends the same
    // vertices in both. The fraction of OFY is the half line, so it is dropped: a field then has the geometry of the
    // first kind of field, and the second kind's half-line lower sampling grid becomes the odd lines of the target.
    // That is the 448-line grid a TV lays the two fields on, so the sample of target row j is at GS position j / sy
    // (rows 2k and 2k+1 sample the field at y = k and y = k + 1/2), where weave mode samples target pixel centres.
    // Sprites need no such step: their coordinates are truncated to whole pixels (BuildSprite), which already ignores
    // the fraction of OFY.
    struct TargetMapping
    {
        uint32_t sx = 1, sy = 1;
        bool progressive = false;
    };

    // TRIANGLE / TRISTRIP / TRIFAN: the batch holds one triangle. Returns false for a triangle with no
    // area (the CPU renderer draws nothing for it either).
    inline bool BuildTriangle(const GSPrimitiveBatch &batch, const TargetMapping &map, Setup &out)
    {
        const GSDrawState &state = batch.state;
        const GSContext &ctx = state.context;
        // Progressive mode: the same geometry in both kinds of field (see TargetMapping).
        const int64_t ofy = map.progressive ? static_cast<int64_t>(ctx.xyoffset.ofy & ~15u) : static_cast<int64_t>(ctx.xyoffset.ofy);
        int64_t ix[3], iy[3];
        for (int i = 0; i < 3; ++i)
        {
            ix[i] = std::lround(batch.vertices[static_cast<size_t>(i)].x * 16.0f) - ctx.xyoffset.ofx;
            iy[i] = std::lround(batch.vertices[static_cast<size_t>(i)].y * 16.0f) - ofy;
        }
        const int64_t denom = (iy[1] - iy[2]) * (ix[0] - ix[2]) + (ix[2] - ix[1]) * (iy[0] - iy[2]);
        if (denom == 0)
            return false;

        const float fscaleX = static_cast<float>(map.sx);
        const float fscaleY = static_cast<float>(map.sy);
        float fx[3], fy[3];
        for (int i = 0; i < 3; ++i)
        {
            fx[i] = static_cast<float>(ix[i]) / 16.0f;
            fy[i] = static_cast<float>(iy[i]) / 16.0f;
        }
        out.minX = static_cast<int32_t>(std::floor(std::min({fx[0], fx[1], fx[2]})));
        out.maxX = static_cast<int32_t>(std::ceil(std::max({fx[0], fx[1], fx[2]})));
        out.minY = static_cast<int32_t>(std::floor(std::min({fy[0], fy[1], fy[2]})));
        out.maxY = static_cast<int32_t>(std::ceil(std::max({fy[0], fy[1], fy[2]})));

        const GSVertex &last = batch.vertices[2];
        out.texelCoords = state.prim.fst;
        for (int i = 0; i < 3; ++i)
        {
            const GSVertex &src = batch.vertices[static_cast<size_t>(i)];
            Vertex &dst = out.v[i];
            // The GS samples at integer positions; the GPU at pixel centres, half a pixel further. In progressive
            // mode the rows of the target are the lines of the 448-line grid, which sample at j / sy (TargetMapping).
            dst.x = (fx[i] + 0.5f) * fscaleX;
            dst.y = map.progressive ? fy[i] * fscaleY + 0.5f : (fy[i] + 0.5f) * fscaleY;
            dst.z = static_cast<float>(src.z * static_cast<double>(kDepthScale));
            const GSVertex &colour = state.prim.iip ? src : last; // flat shading takes the last vertex
            dst.rgba = PackRgba(colour.r, colour.g, colour.b, colour.a);
            if (state.prim.fst)
            {
                dst.u = static_cast<float>(src.u) / 16.0f;
                dst.v = static_cast<float>(src.v) / 16.0f;
                dst.q = 1.0f;
            }
            else
            {
                dst.u = src.s;
                dst.v = src.t;
                dst.q = src.q;
            }
        }
        out.count = 3;
        return true;
    }

    inline bool BuildTriangle(const GSPrimitiveBatch &batch, uint32_t scale, Setup &out)
    {
        return BuildTriangle(batch, TargetMapping{scale, scale, false}, out);
    }

    // SPRITE: two corners, drawn as two triangles. Coordinates are truncated to whole pixels like the CPU
    // renderer; a zero-sized side counts as one pixel. The colour and Z of the second vertex apply to
    // the whole sprite. The texture coordinates run from the first vertex (left/top edge) to the second
    // (right/bottom edge) even when the corners are given the other way round, as in the CPU renderer.
    inline bool BuildSprite(const GSPrimitiveBatch &batch, const TargetMapping &map, Setup &out)
    {
        const GSDrawState &state = batch.state;
        const GSContext &ctx = state.context;
        const GSVertex &v0 = batch.vertices[0];
        const GSVertex &v1 = batch.vertices[1];

        const int ofx = ctx.xyoffset.ofx >> 4;
        const int ofy = ctx.xyoffset.ofy >> 4;
        int x0 = static_cast<int>(v0.x) - ofx;
        int y0 = static_cast<int>(v0.y) - ofy;
        int x1 = static_cast<int>(v1.x) - ofx;
        int y1 = static_cast<int>(v1.y) - ofy;
        if (x0 > x1)
            std::swap(x0, x1);
        if (y0 > y1)
            std::swap(y0, y1);
        const int spanX = std::max(1, x1 - x0);
        const int spanY = std::max(1, y1 - y0);
        out.minX = x0;
        out.maxX = x0 + spanX - 1;
        out.minY = y0;
        out.maxY = y0 + spanY - 1;

        float u0 = 0.0f, tv0 = 0.0f, u1 = 0.0f, tv1 = 0.0f;
        if (state.prim.tme)
        {
            if (state.prim.fst)
            {
                u0 = static_cast<float>(v0.u >> 4);
                tv0 = static_cast<float>(v0.v >> 4);
                u1 = static_cast<float>(v1.u >> 4);
                tv1 = static_cast<float>(v1.v >> 4);
            }
            else
            {
                const float q0 = SafeQ(v0.q);
                const float q1 = SafeQ(v1.q);
                const float w = static_cast<float>(state.textureWidth);
                const float h = static_cast<float>(state.textureHeight);
                u0 = (v0.s / q0) * w;
                tv0 = (v0.t / q0) * h;
                u1 = (v1.s / q1) * w;
                tv1 = (v1.t / q1) * h;
            }
        }
        out.texelCoords = true; // sprites interpolate texel coordinates, never S/T/Q

        const float fscaleX = static_cast<float>(map.sx);
        const float fscaleY = static_cast<float>(map.sy);
        const float left = static_cast<float>(x0) * fscaleX;
        const float top = static_cast<float>(y0) * fscaleY;
        const float right = static_cast<float>(x0 + spanX) * fscaleX;
        const float bottom = static_cast<float>(y0 + spanY) * fscaleY;
        const float z = static_cast<float>(v1.z * static_cast<double>(kDepthScale));
        const uint32_t rgba = PackRgba(v1.r, v1.g, v1.b, v1.a);
        auto make = [&](float x, float y, float u, float v)
        {
            Vertex r;
            r.x = x;
            r.y = y;
            r.z = z;
            r.rgba = rgba;
            r.u = u;
            r.v = v;
            r.q = 1.0f;
            return r;
        };
        const Vertex tl = make(left, top, u0, tv0);
        const Vertex tr = make(right, top, u1, tv0);
        const Vertex br = make(right, bottom, u1, tv1);
        const Vertex bl = make(left, bottom, u0, tv1);
        out.v[0] = tl;
        out.v[1] = tr;
        out.v[2] = br;
        out.v[3] = tl;
        out.v[4] = br;
        out.v[5] = bl;
        out.count = 6;
        return true;
    }

    inline bool BuildSprite(const GSPrimitiveBatch &batch, uint32_t scale, Setup &out)
    {
        return BuildSprite(batch, TargetMapping{scale, scale, false}, out);
    }

    // ------------------------------------------------------------------------------------------
    // Blending. The GS computes (A - B) * C + D with A, B, D from {Cs, Cd, 0} and C from {As, Ad, FIX}
    // (0x80 = 1.0). A GPU blender computes src * sf op dst * df. Every GS combination whose Cs and Cd
    // coefficients are 0, 1, C, 1-C or -C fits; the others (such as (Cd - Cs) * C + Cd) do not.
    enum class BlendFactor : uint8_t { Zero, One, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha };
    enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract };

    struct BlendState
    {
        bool enable = false;
        BlendFactor src = BlendFactor::One;
        BlendFactor dst = BlendFactor::Zero;
        BlendOp op = BlendOp::Add;
        bool supported = true;      // false: no exact fixed-function form; `src`/`dst` hold the usual alpha blend
        bool coefficientFromFix = false; // C is FIX (the shader outputs FIX/128 as its alpha)
        bool operator==(const BlendState &o) const
        {
            return enable == o.enable && src == o.src && dst == o.dst && op == o.op;
        }
    };

    inline BlendState BlendFromAlpha(bool abe, uint64_t alphaReg)
    {
        BlendState out;
        if (!abe)
            return out;
        out.enable = true;
        const unsigned a = static_cast<unsigned>(alphaReg & 3u);        // 0 Cs, 1 Cd, 2 zero
        const unsigned b = static_cast<unsigned>((alphaReg >> 2) & 3u);
        const unsigned c = static_cast<unsigned>((alphaReg >> 4) & 3u); // 0 As, 1 Ad, 2 FIX
        const unsigned d = static_cast<unsigned>((alphaReg >> 6) & 3u);
        out.coefficientFromFix = c == 2u;

        // Coefficient of Cs and of Cd as k0 + k1 * C.
        int ks0 = (d == 0u) ? 1 : 0, ks1 = (a == 0u ? 1 : 0) - (b == 0u ? 1 : 0);
        int kd0 = (d == 1u) ? 1 : 0, kd1 = (a == 1u ? 1 : 0) - (b == 1u ? 1 : 0);

        const bool alphaIsDst = c == 1u;
        auto factor = [&](int k0, int k1, bool &ok) -> BlendFactor
        {
            if (k0 == 0 && k1 == 0) return BlendFactor::Zero;
            if (k0 == 1 && k1 == 0) return BlendFactor::One;
            if (k0 == 0 && (k1 == 1 || k1 == -1)) return alphaIsDst ? BlendFactor::DstAlpha : BlendFactor::SrcAlpha;
            if (k0 == 1 && k1 == -1) return alphaIsDst ? BlendFactor::OneMinusDstAlpha : BlendFactor::OneMinusSrcAlpha;
            ok = false;
            return BlendFactor::One;
        };
        bool ok = true;
        const BlendFactor sf = factor(ks0, ks1, ok);
        const BlendFactor df = factor(kd0, kd1, ok);
        const bool srcNegative = ks0 == 0 && ks1 == -1;
        const bool dstNegative = kd0 == 0 && kd1 == -1;
        if (ok && !(srcNegative && dstNegative))
        {
            out.src = sf;
            out.dst = df;
            out.op = dstNegative ? BlendOp::Subtract : (srcNegative ? BlendOp::ReverseSubtract : BlendOp::Add);
            out.supported = true;
        }
        else
        {
            out.src = alphaIsDst ? BlendFactor::DstAlpha : BlendFactor::SrcAlpha;
            out.dst = alphaIsDst ? BlendFactor::OneMinusDstAlpha : BlendFactor::OneMinusSrcAlpha;
            out.op = BlendOp::Add;
            out.supported = false;
        }
        return out;
    }

    // ------------------------------------------------------------------------------------------
    // The per-draw fragment uniform block (see gs.frag.hlsl: four-component members only).
    struct FragUniforms
    {
        float texInfo[4];
        uint32_t flags[4];
        uint32_t wrap[4];
        uint32_t test[4];
        bool operator==(const FragUniforms &o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
    };
    static_assert(sizeof(FragUniforms) == 64, "four 16-byte members");

    constexpr uint32_t kFlagTextured = 1u << 0;
    constexpr uint32_t kFlagTexelCoords = 1u << 1;
    constexpr uint32_t kFlagBilinear = 1u << 2;
    constexpr uint32_t kFlagTcc = 1u << 3;
    constexpr uint32_t kFlagAlphaTest = 1u << 6;
    constexpr uint32_t kFlagQuantizeRound = 1u << 7; // texel coordinates rounded to 1/16 (a sprite with UV, as the CPU renderer does)
    constexpr uint32_t kFlagQuantizeFloor = 1u << 8; // truncated to 1/16 (a triangle with UV: the GS interpolates 12.4 fixed point)
    // A sprite on a target larger than the GS frame (flags[1] target rows and flags[2] target columns per GS pixel): the shader
    // samples the texture at the centre of the GS pixel the target pixel belongs to, like the GS does, instead of at the
    // target pixel's own centre. See gs.frag.hlsl.
    constexpr uint32_t kFlagSnapToGsPixel = 1u << 9;

    // `snapX` and `snapY` are the target pixels per GS pixel along each axis for a sprite (0 or 1: no snapping, see
    // kFlagSnapToGsPixel); other primitives pass 0.
    inline FragUniforms BuildFragUniforms(const GSDrawState &state, bool texelCoords, bool blendUsesFix, uint32_t snapX = 0u, uint32_t snapY = 0u)
    {
        FragUniforms u;
        std::memset(&u, 0, sizeof(u));
        const GSContext &ctx = state.context;
        const float w = static_cast<float>(state.textureWidth);
        const float h = static_cast<float>(state.textureHeight);
        if (state.prim.tme)
        {
            u.texInfo[0] = w;
            u.texInfo[1] = h;
            u.texInfo[2] = 1.0f / w;
            u.texInfo[3] = 1.0f / h;
            u.flags[0] |= kFlagTextured;
            if (texelCoords)
                u.flags[0] |= kFlagTexelCoords;
            if (state.prim.fst)
                u.flags[0] |= (state.prim.type == GS_PRIM_SPRITE) ? kFlagQuantizeRound : kFlagQuantizeFloor;
            if (texelCoords && (snapX > 1u || snapY > 1u))
            {
                u.flags[0] |= kFlagSnapToGsPixel;
                u.flags[1] = snapY;
                u.flags[2] = snapX;
            }
            if (state.linearFilter)
                u.flags[0] |= kFlagBilinear;
            if (ctx.tex0.tcc != 0u)
                u.flags[0] |= kFlagTcc;
            u.flags[0] |= static_cast<uint32_t>(ctx.tex0.tfx & 3u) << 4;
            const uint64_t clamp = ctx.clamp;
            u.wrap[0] = static_cast<uint32_t>(clamp & 3u);
            u.wrap[1] = static_cast<uint32_t>((clamp >> 2) & 3u);
            u.wrap[2] = static_cast<uint32_t>((clamp >> 4) & 0x3FFu) | (static_cast<uint32_t>((clamp >> 14) & 0x3FFu) << 16);
            u.wrap[3] = static_cast<uint32_t>((clamp >> 24) & 0x3FFu) | (static_cast<uint32_t>((clamp >> 34) & 0x3FFu) << 16);
        }
        else
        {
            u.texInfo[0] = u.texInfo[1] = u.texInfo[2] = u.texInfo[3] = 1.0f;
        }
        const uint64_t test = ctx.test;
        const uint32_t atst = static_cast<uint32_t>((test >> 1) & 7u);
        if ((test & 1u) != 0u && atst != 1u) // ALWAYS passes everything
            u.flags[0] |= kFlagAlphaTest;
        u.test[0] = atst;
        u.test[1] = static_cast<uint32_t>((test >> 4) & 0xFFu);
        u.test[2] = blendUsesFix ? 1u : 0u;
        u.test[3] = static_cast<uint32_t>((ctx.alpha >> 32) & 0xFFu);
        return u;
    }

    // Depth test of a draw: 0 never, 1 always, 2 greater or equal, 3 greater (the GS ZTST values). With
    // ZTE clear the CPU renderer treats the test as ALWAYS.
    inline uint32_t DepthCompare(uint64_t testReg)
    {
        const bool enabled = ((testReg >> 16) & 1u) != 0u;
        return enabled ? static_cast<uint32_t>((testReg >> 17) & 3u) : 1u;
    }

    // ------------------------------------------------------------------------------------------
    // GS memory pages (8 KiB, 512 in 4 MiB). Pages of a buffer or texture follow the GS address rule:
    // page = base/32 + (y / pageH) * (bw * 64 / pageW) + x / pageW, and a base that is not at the start
    // of a page spills into the next one.
    inline void PageSize(uint32_t psm, uint32_t &w, uint32_t &h)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            w = 64u;
            h = 64u;
            return;
        case GS_PSM_T8:
            w = 128u;
            h = 64u;
            return;
        case GS_PSM_T4:
            w = 128u;
            h = 128u;
            return;
        default: // 32-bit layouts, including T8H/T4HL/T4HH
            w = 64u;
            h = 32u;
            return;
        }
    }

    struct PageSpan
    {
        uint32_t first = 0; // first page, 0..511
        uint32_t count = 0; // pages, at most 512; the span wraps around the end of GS memory
    };

    // Pages that pixels [x0, x0+w) x [y0, y0+h) of a buffer at block pointer `base` (256-byte blocks)
    // with width `bw` (in 64 pixel units) can live in. A conservative contiguous range.
    inline PageSpan BufferPageSpan(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
    {
        PageSpan span;
        if (w == 0u || h == 0u)
            return span;
        uint32_t pw, ph;
        PageSize(psm, pw, ph);
        const uint32_t perRow = (bw * 64u) / pw;
        const uint32_t basePage = base / 32u;
        const uint32_t firstPage = basePage + (y0 / ph) * perRow + x0 / pw;
        uint32_t lastPage = basePage + ((y0 + h - 1u) / ph) * perRow + (x0 + w - 1u) / pw;
        if ((base % 32u) != 0u)
            ++lastPage;
        span.first = firstPage & 511u;
        const uint64_t count = static_cast<uint64_t>(lastPage) - firstPage + 1u;
        span.count = static_cast<uint32_t>(std::min<uint64_t>(count, 512u));
        return span;
    }

    // ------------------------------------------------------------------------------------------
    // Which texels of a texture a draw can sample. The GPU renderer decodes a texture from GS memory lazily, only
    // the 16 x 16 texel tiles that some draw can reach: a wrestler's portrait is declared 512 x 512 texels but a draw
    // uses about a tenth of that, and decoding the rest is what made the random pick on the wrestler select slow. The
    // bound has to be safe, because a texel the shader fetches that was never decoded would show garbage.
    struct TexelRect
    {
        int32_t x0 = 0, y0 = 0, x1 = -1, y1 = -1; // inclusive
        bool Empty() const { return x1 < x0 || y1 < y0; }
        bool operator==(const TexelRect &o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
    };

    // What gs.frag.hlsl can fetch from a width x height texture for the primitive in `setup`. The shader fetches texel
    // floor(u) (nearest), or floor(u - 0.5) and the one after it (bilinear), after rounding the coordinate to 1/16 and
    // applying the wrap mode. A texel coordinate stays between the smallest and the largest value at the vertices
    // (for S, T, Q too, as long as every Q is positive: the interpolated S/Q is a weighted mean of the vertex ratios),
    // so that range widened by one texel on each side covers every case, and the wrap mode then narrows or widens it:
    //   CLAMP         the range clamped to the texture
    //   REGION_CLAMP  the range clamped into [MINU, MAXU] (or V), then to the texture (the sampler clamps what is left)
    //   REPEAT        the range itself when it lies inside the texture, otherwise the whole axis
    //   REGION_REPEAT the whole axis
    // A Q that is not positive, a coordinate that is not finite or absurdly large, or no vertices: the whole texture.
    inline TexelRect TextureReach(const Setup &setup, uint64_t clampReg, uint32_t width, uint32_t height)
    {
        TexelRect whole;
        whole.x0 = 0;
        whole.y0 = 0;
        whole.x1 = static_cast<int32_t>(width) - 1;
        whole.y1 = static_cast<int32_t>(height) - 1;
        if (setup.count == 0 || width == 0u || height == 0u)
            return whole;
        const float size[2] = {static_cast<float>(width), static_cast<float>(height)};
        float lo[2] = {1.0e30f, 1.0e30f}, hi[2] = {-1.0e30f, -1.0e30f};
        for (uint32_t i = 0; i < setup.count; ++i)
        {
            const Vertex &v = setup.v[i];
            float c[2];
            if (setup.texelCoords)
            {
                c[0] = v.u;
                c[1] = v.v;
            }
            else
            {
                if (!(v.q > 0.0f))
                    return whole;
                c[0] = v.u / v.q * size[0];
                c[1] = v.v / v.q * size[1];
            }
            for (int k = 0; k < 2; ++k)
            {
                if (!(std::fabs(c[k]) < 1.0e6f))
                    return whole;
                lo[k] = std::min(lo[k], c[k]);
                hi[k] = std::max(hi[k], c[k]);
            }
        }
        const int32_t extent[2] = {static_cast<int32_t>(width), static_cast<int32_t>(height)};
        const uint32_t mode[2] = {static_cast<uint32_t>(clampReg & 3u), static_cast<uint32_t>((clampReg >> 2) & 3u)};
        const int32_t regionMin[2] = {static_cast<int32_t>((clampReg >> 4) & 0x3FFu), static_cast<int32_t>((clampReg >> 24) & 0x3FFu)};
        const int32_t regionMax[2] = {static_cast<int32_t>((clampReg >> 14) & 0x3FFu), static_cast<int32_t>((clampReg >> 34) & 0x3FFu)};
        int32_t first[2], last[2];
        for (int k = 0; k < 2; ++k)
        {
            const int32_t l = static_cast<int32_t>(std::floor(lo[k])) - 1;
            const int32_t h = static_cast<int32_t>(std::floor(hi[k])) + 1;
            first[k] = 0;
            last[k] = extent[k] - 1;
            switch (mode[k])
            {
            case 0u: // REPEAT
                if (l >= 0 && h <= extent[k] - 1)
                {
                    first[k] = l;
                    last[k] = h;
                }
                break;
            case 1u: // CLAMP
                first[k] = std::clamp(l, 0, extent[k] - 1);
                last[k] = std::clamp(h, 0, extent[k] - 1);
                break;
            case 2u: // REGION_CLAMP: min(max(c, MIN), MAX) as in the shader, which is monotonic in c
                first[k] = std::clamp(std::min(std::max(l, regionMin[k]), regionMax[k]), 0, extent[k] - 1);
                last[k] = std::clamp(std::min(std::max(h, regionMin[k]), regionMax[k]), 0, extent[k] - 1);
                break;
            default: // REGION_REPEAT is a bitwise operation, not a range
                break;
            }
        }
        TexelRect r;
        r.x0 = first[0];
        r.y0 = first[1];
        r.x1 = last[0];
        r.y1 = last[1];
        return r;
    }

    // Which 16 x 16 texel tiles of a texture hold decoded texels. A texture is at most 1024 x 1024 (TW, TH <= 10).
    constexpr uint32_t kTileShift = 4u;
    constexpr uint32_t kTileTexels = 1u << kTileShift;
    constexpr uint32_t kTilesPerSide = 64u;

    struct TileMap
    {
        std::bitset<kTilesPerSide * kTilesPerSide> decoded; // tile (tx, ty) is bit ty * 64 + tx
    };

    // Marks the tiles that `rect` (inclusive, inside the texture) touches as decoded, and appends to `out` the
    // tile-aligned rectangles, clipped to the width x height texture, that were not decoded before: runs of missing tiles
    // along a tile row, joined with the run above when the two line up. A rectangle that is already decoded adds nothing.
    inline void TakeMissingTiles(TileMap &map, const TexelRect &rect, uint32_t width, uint32_t height, std::vector<TexelRect> &out)
    {
        if (rect.Empty() || rect.x0 < 0 || rect.y0 < 0)
            return;
        const uint32_t tx0 = static_cast<uint32_t>(rect.x0) >> kTileShift, tx1 = std::min(static_cast<uint32_t>(rect.x1) >> kTileShift, kTilesPerSide - 1u);
        const uint32_t ty0 = static_cast<uint32_t>(rect.y0) >> kTileShift, ty1 = std::min(static_cast<uint32_t>(rect.y1) >> kTileShift, kTilesPerSide - 1u);
        const size_t firstNew = out.size();
        for (uint32_t ty = ty0; ty <= ty1; ++ty)
        {
            uint32_t tx = tx0;
            while (tx <= tx1)
            {
                if (map.decoded.test(ty * kTilesPerSide + tx))
                {
                    ++tx;
                    continue;
                }
                uint32_t run = tx;
                while (run + 1u <= tx1 && !map.decoded.test(ty * kTilesPerSide + run + 1u))
                    ++run;
                for (uint32_t i = tx; i <= run; ++i)
                    map.decoded.set(ty * kTilesPerSide + i);
                TexelRect r;
                r.x0 = static_cast<int32_t>(tx << kTileShift);
                r.y0 = static_cast<int32_t>(ty << kTileShift);
                r.x1 = static_cast<int32_t>(std::min((run + 1u) << kTileShift, width)) - 1;
                r.y1 = static_cast<int32_t>(std::min((ty + 1u) << kTileShift, height)) - 1;
                if (out.size() > firstNew && out.back().x0 == r.x0 && out.back().x1 == r.x1 && out.back().y1 + 1 == r.y0)
                    out.back().y1 = r.y1;
                else
                    out.push_back(r);
                tx = run + 1u;
            }
        }
    }
}
