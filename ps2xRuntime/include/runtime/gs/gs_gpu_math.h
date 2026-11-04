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
// Nothing here includes SDL, and no header that generated game code includes refers to this one.

#include "runtime/gs/gs_types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

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

    // TRIANGLE / TRISTRIP / TRIFAN: the batch holds one triangle. Returns false for a triangle with no
    // area (the CPU renderer draws nothing for it either).
    inline bool BuildTriangle(const GSPrimitiveBatch &batch, uint32_t scale, Setup &out)
    {
        const GSDrawState &state = batch.state;
        const GSContext &ctx = state.context;
        int64_t ix[3], iy[3];
        for (int i = 0; i < 3; ++i)
        {
            ix[i] = std::lround(batch.vertices[static_cast<size_t>(i)].x * 16.0f) - ctx.xyoffset.ofx;
            iy[i] = std::lround(batch.vertices[static_cast<size_t>(i)].y * 16.0f) - ctx.xyoffset.ofy;
        }
        const int64_t denom = (iy[1] - iy[2]) * (ix[0] - ix[2]) + (ix[2] - ix[1]) * (iy[0] - iy[2]);
        if (denom == 0)
            return false;

        const float fscale = static_cast<float>(scale);
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
            // The GS samples at integer positions; the GPU at pixel centres, half a pixel further.
            dst.x = (fx[i] + 0.5f) * fscale;
            dst.y = (fy[i] + 0.5f) * fscale;
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

    // SPRITE: two corners, drawn as two triangles. Coordinates are truncated to whole pixels like the CPU
    // renderer; a zero-sized side counts as one pixel. The colour and Z of the second vertex apply to
    // the whole sprite. The texture coordinates run from the first vertex (left/top edge) to the second
    // (right/bottom edge) even when the corners are given the other way round, as in the CPU renderer.
    inline bool BuildSprite(const GSPrimitiveBatch &batch, uint32_t scale, Setup &out)
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

        const float fscale = static_cast<float>(scale);
        const float left = static_cast<float>(x0) * fscale;
        const float top = static_cast<float>(y0) * fscale;
        const float right = static_cast<float>(x0 + spanX) * fscale;
        const float bottom = static_cast<float>(y0 + spanY) * fscale;
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

    inline FragUniforms BuildFragUniforms(const GSDrawState &state, bool texelCoords, bool blendUsesFix)
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
}
