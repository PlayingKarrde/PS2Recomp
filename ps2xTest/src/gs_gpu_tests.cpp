#include "MiniTest.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_field_deinterlacer.h"
#include "runtime/gs/gs_gpu_math.h"
#include "runtime/gs/gs_gpu_present.h"
#include "runtime/gs/gs_presentation.h"
#include "runtime/gs/gs_texel.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/ps2_gs_transfer.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

// Tests for the GS GPU renderer (gs_gpu_math.h: the pure arithmetic; gs_gpu_backend: a small scene
// drawn by the CPU and GPU renderers, skipped when no GPU device can be created) and for the code
// shared by the two renderers (gs_texel.h, ps2_gs_transfer.h).

namespace
{
    using namespace GSGpu;

    GSPrimitiveBatch triangleBatch(float x0, float y0, float x1, float y1, float x2, float y2)
    {
        GSPrimitiveBatch b;
        b.vertexCount = 3;
        b.state.prim.type = GS_PRIM_TRIANGLE;
        b.state.prim.iip = true;
        b.state.context.xyoffset = {static_cast<uint16_t>(1728u * 16u), static_cast<uint16_t>(1936u * 16u)};
        const float xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
        for (int i = 0; i < 3; ++i)
        {
            b.vertices[static_cast<size_t>(i)].x = 1728.0f + xs[i];
            b.vertices[static_cast<size_t>(i)].y = 1936.0f + ys[i];
            b.vertices[static_cast<size_t>(i)].z = 1000.0 * (i + 1);
            b.vertices[static_cast<size_t>(i)].r = static_cast<uint8_t>(10 * (i + 1));
            b.vertices[static_cast<size_t>(i)].g = static_cast<uint8_t>(20 * (i + 1));
            b.vertices[static_cast<size_t>(i)].b = static_cast<uint8_t>(30 * (i + 1));
            b.vertices[static_cast<size_t>(i)].a = static_cast<uint8_t>(0x80);
        }
        return b;
    }

    GSPrimitiveBatch spriteBatch(float x0, float y0, float x1, float y1)
    {
        GSPrimitiveBatch b;
        b.vertexCount = 2;
        b.state.prim.type = GS_PRIM_SPRITE;
        b.state.context.xyoffset = {static_cast<uint16_t>(1728u * 16u), static_cast<uint16_t>(1936u * 16u)};
        b.vertices[0].x = 1728.0f + x0;
        b.vertices[0].y = 1936.0f + y0;
        b.vertices[1].x = 1728.0f + x1;
        b.vertices[1].y = 1936.0f + y1;
        b.vertices[1].z = 4096.0;
        b.vertices[1].r = 1;
        b.vertices[1].g = 2;
        b.vertices[1].b = 3;
        b.vertices[1].a = 4;
        return b;
    }

    uint64_t alphaReg(unsigned a, unsigned b, unsigned c, unsigned d, unsigned fix = 0)
    {
        return a | (b << 2) | (c << 4) | (d << 6) | (static_cast<uint64_t>(fix) << 32);
    }

    // ---- a small scene drawn by both renderers
    struct SceneResult
    {
        std::vector<uint8_t> pixels; // 640x224 RGBA
        bool ok = false;
    };

    GSPresentationRequest makeRequest(uint32_t fbp)
    {
        GSPresentationRequest r{};
        r.pmode = 0x8007;
        r.smode2 = 0x3;
        const uint64_t dispfb = static_cast<uint64_t>(fbp) | (10ull << 9);
        const uint64_t display = (636ull) | (32ull << 12) | (639ull << 32) | (223ull << 44);
        r.dispfb1 = r.dispfb2 = dispfb;
        r.display1 = r.display2 = display;
        return r;
    }

    // Uploads a 16x16 CT32 texture at block 8000 (a gradient with an alpha ramp), then draws, in order:
    // a gouraud triangle with alpha blending over a flat background sprite, a textured sprite with nearest
    // filtering, a textured sprite with bilinear filtering and an alpha test, and a sprite behind them
    // that the depth test must reject.
    void drawScene(GSRasterBackend &b)
    {
        GSContext ctx{};
        ctx.frame = {0, 10, GS_PSM_CT32, 0};
        ctx.zbuf = {140, GS_PSM_Z24, false};
        ctx.scissor = {0, 639, 0, 223};
        ctx.xyoffset = {static_cast<uint16_t>(1728u * 16u), static_cast<uint16_t>(1936u * 16u)};
        ctx.test = (1ull << 16) | (2ull << 17); // ZTE on, GEQUAL

        // texture data
        std::vector<uint32_t> tex(16 * 16);
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x)
                tex[y * 16 + x] = (x * 16u) | ((y * 16u) << 8) | ((200u - x * 8u) << 16) | ((0x40u + y * 4u) << 24);
        GSTransferCommand cmd{};
        cmd.bitbltbuf.dbp = 8000;
        cmd.bitbltbuf.dbw = 1;
        cmd.bitbltbuf.dpsm = GS_PSM_CT32;
        cmd.trxreg = {16, 16};
        cmd.direction = 0;
        b.BeginTransfer(cmd);
        b.UploadImage(reinterpret_cast<const uint8_t *>(tex.data()), static_cast<uint32_t>(tex.size() * 4));
        b.TextureFlush();

        auto sprite = [&](GSDrawState state, float x0, float y0, float x1, float y1, double z, uint8_t r, uint8_t g, uint8_t bl, uint8_t a,
                          uint16_t u0, uint16_t v0, uint16_t u1, uint16_t v1)
        {
            GSPrimitiveBatch p;
            p.vertexCount = 2;
            p.state = state;
            p.state.prim.type = GS_PRIM_SPRITE;
            p.vertices[0].x = 1728.0f + x0;
            p.vertices[0].y = 1936.0f + y0;
            p.vertices[0].u = u0;
            p.vertices[0].v = v0;
            p.vertices[1].x = 1728.0f + x1;
            p.vertices[1].y = 1936.0f + y1;
            p.vertices[1].u = u1;
            p.vertices[1].v = v1;
            p.vertices[1].z = z;
            p.vertices[1].r = r;
            p.vertices[1].g = g;
            p.vertices[1].b = bl;
            p.vertices[1].a = a;
            b.Submit(p);
        };

        GSDrawState flat{};
        flat.context = ctx;
        flat.context.test = 0;                    // ZTE off: always passes, writes Z
        sprite(flat, 0, 0, 640, 224, 100.0, 40, 60, 90, 0x80, 0, 0, 0, 0);

        GSDrawState gouraud{};
        gouraud.context = ctx;
        gouraud.context.alpha = alphaReg(0, 1, 0, 1); // (Cs-Cd)*As+Cd
        gouraud.prim.abe = true;
        gouraud.prim.iip = true;
        {
            GSPrimitiveBatch p;
            p.vertexCount = 3;
            p.state = gouraud;
            p.state.prim.type = GS_PRIM_TRIANGLE;
            const float xs[3] = {20.0f, 220.5f, 60.25f}, ys[3] = {10.0f, 30.75f, 150.0f};
            const uint8_t rs[3] = {255, 0, 0}, gs[3] = {0, 255, 0}, bs[3] = {0, 0, 255}, as[3] = {0x60, 0x80, 0x40};
            for (int i = 0; i < 3; ++i)
            {
                p.vertices[static_cast<size_t>(i)].x = 1728.0f + xs[i];
                p.vertices[static_cast<size_t>(i)].y = 1936.0f + ys[i];
                p.vertices[static_cast<size_t>(i)].z = 2000.0 + 500.0 * i;
                p.vertices[static_cast<size_t>(i)].r = rs[i];
                p.vertices[static_cast<size_t>(i)].g = gs[i];
                p.vertices[static_cast<size_t>(i)].b = bs[i];
                p.vertices[static_cast<size_t>(i)].a = as[i];
            }
            b.Submit(p);
        }

        GSDrawState textured{};
        textured.context = ctx;
        textured.context.tex0 = {8000, 1, GS_PSM_CT32, 4, 4, 1, 0, 0, 0, 0, 0, 0}; // 16x16 CT32, TCC, MODULATE
        textured.context.clamp = 0;                                                // REPEAT
        textured.prim.tme = true;
        textured.prim.fst = true;
        textured.textureWidth = 16;
        textured.textureHeight = 16;
        textured.linearFilter = false;
        sprite(textured, 300, 20, 364, 84, 5000.0, 0x80, 0x80, 0x80, 0x80, 0, 0, 16 * 16, 16 * 16);

        GSDrawState filtered = textured;
        filtered.linearFilter = true;
        filtered.context.alpha = alphaReg(0, 1, 0, 1);
        filtered.prim.abe = true;
        filtered.context.test |= 1ull | (5ull << 1) | (0x60ull << 4); // alpha test GEQUAL 0x60, KEEP
        sprite(filtered, 400, 20, 500, 120, 6000.0, 0x80, 0x80, 0x80, 0x80, 0, 0, 16 * 16, 16 * 16);

        GSDrawState hidden = flat;
        hidden.context.test = (1ull << 16) | (2ull << 17);
        sprite(hidden, 320, 40, 340, 60, 10.0, 255, 255, 255, 0x80, 0, 0, 0, 0); // behind the textured sprite: rejected
        b.Flush();
    }

    SceneResult runScene(GSRasterBackend &b, std::vector<uint8_t> &vram, GSCpuBackend *cpu, GSGpuBackend *gpu)
    {
        b.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
        drawScene(b);
        const GSPresentationRequest request = makeRequest(0);
        if (cpu)
            cpu->MarkPresentation(request);
        else
            gpu->MarkPresentation(request);
        b.Sync(GSSyncReason::Presentation);
        PresentationFrame frame = b.Present(request);
        SceneResult r;
        if (frame && frame.width == 640 && frame.height == 224)
        {
            r.pixels = std::move(frame.pixels);
            r.ok = true;
        }
        return r;
    }
}

void register_gs_gpu_tests()
{
    MiniTest::Case("GSGpu", [](TestCase &tc)
    {
        tc.Run("a triangle is moved half a pixel and flat shading takes the last vertex", [](TestCase &t)
        {
            GSPrimitiveBatch b = triangleBatch(10.0f, 20.0f, 50.0f, 20.0f, 10.0f, 60.0f);
            b.state.prim.iip = false;
            Setup s;
            t.IsTrue(BuildTriangle(b, 1, s), "a triangle with area converts");
            t.Equals(s.count, 3u, "three vertices");
            t.Equals(s.v[0].x, 10.5f, "x of vertex 0: GS integer position plus half a pixel");
            t.Equals(s.v[0].y, 20.5f, "y of vertex 0");
            t.Equals(s.v[1].x, 50.5f, "x of vertex 1");
            t.Equals(s.v[2].y, 60.5f, "y of vertex 2");
            t.Equals(s.minX, 10, "bounding box min x");
            t.Equals(s.maxX, 50, "bounding box max x");
            t.Equals(s.minY, 20, "bounding box min y");
            t.Equals(s.maxY, 60, "bounding box max y");
            t.Equals(s.v[0].rgba, PackRgba(30, 60, 90, 0x80), "flat shading: every vertex has the last vertex colour");
            t.Equals(s.v[1].rgba, s.v[0].rgba, "flat shading, vertex 1");
            t.Equals(s.v[0].z, 1000.0f * kDepthScale, "Z is the 24-bit value over 2^24");
            // scale 2: twice the GS position, plus the half target pixel that makes the GPU's pixel-centre sample the corner
            Setup s2;
            t.IsTrue(BuildTriangle(b, 2, s2), "scale 2");
            t.Equals(s2.v[1].x, 100.5f, "scale 2 x of vertex 1");
        });

        tc.Run("a fractional 12.4 position is kept and a degenerate triangle is dropped", [](TestCase &t)
        {
            GSPrimitiveBatch b = triangleBatch(10.0625f, 20.9375f, 50.5f, 20.0f, 10.0f, 60.0f);
            Setup s;
            t.IsTrue(BuildTriangle(b, 1, s), "converts");
            t.Equals(s.v[0].x, 10.0625f + 0.5f, "1/16 pixel steps survive");
            t.Equals(s.v[0].y, 20.9375f + 0.5f, "1/16 pixel steps survive in y");
            GSPrimitiveBatch flat = triangleBatch(10.0f, 10.0f, 20.0f, 20.0f, 30.0f, 30.0f);
            t.IsFalse(BuildTriangle(flat, 1, s), "three points on a line have no area");
        });

        tc.Run("UV and STQ texture coordinates", [](TestCase &t)
        {
            GSPrimitiveBatch b = triangleBatch(0, 0, 64, 0, 0, 64);
            b.state.prim.tme = true;
            b.state.prim.fst = true;
            b.vertices[1].u = 64 * 16;
            b.vertices[2].v = 48 * 16 + 8;
            Setup s;
            t.IsTrue(BuildTriangle(b, 1, s), "UV triangle");
            t.IsTrue(s.texelCoords, "UV coordinates are texel coordinates");
            t.Equals(s.v[1].u, 64.0f, "u in texels (12.4 divided by 16)");
            t.Equals(s.v[2].v, 48.5f, "v keeps its fraction");
            t.Equals(s.v[0].q, 1.0f, "q is 1 for UV");
            b.state.prim.fst = false;
            b.vertices[1].s = 0.5f;
            b.vertices[1].q = 0.25f;
            t.IsTrue(BuildTriangle(b, 1, s), "STQ triangle");
            t.IsFalse(s.texelCoords, "STQ stays S, T, Q for the shader to divide");
            t.Equals(s.v[1].u, 0.5f, "s passed through");
            t.Equals(s.v[1].q, 0.25f, "q passed through");
        });

        tc.Run("a sprite covers whole pixels, takes colour and Z from its second vertex and keeps its UV order", [](TestCase &t)
        {
            GSPrimitiveBatch b = spriteBatch(10.9f, 20.2f, 30.1f, 50.7f);
            b.state.prim.tme = true;
            b.state.prim.fst = true;
            b.vertices[0].u = 3 * 16 + 7;
            b.vertices[0].v = 5 * 16;
            b.vertices[1].u = 19 * 16;
            b.vertices[1].v = 35 * 16;
            Setup s;
            t.IsTrue(BuildSprite(b, 1, s), "sprite");
            t.Equals(s.count, 6u, "two triangles");
            t.Equals(s.v[0].x, 10.0f, "left edge: the coordinate truncated");
            t.Equals(s.v[0].y, 20.0f, "top edge");
            t.Equals(s.v[2].x, 30.0f, "right edge");
            t.Equals(s.v[2].y, 50.0f, "bottom edge");
            t.Equals(s.minX, 10, "box min x");
            t.Equals(s.maxX, 29, "box max x is the last covered pixel");
            t.Equals(s.maxY, 49, "box max y");
            t.Equals(s.v[0].u, 3.0f, "left u is the integer part of vertex 0 u");
            t.Equals(s.v[2].u, 19.0f, "right u is vertex 1 u");
            t.Equals(s.v[2].v, 35.0f, "bottom v");
            t.Equals(s.v[0].rgba, PackRgba(1, 2, 3, 4), "colour of the second vertex");
            t.Equals(s.v[0].z, 4096.0f * kDepthScale, "Z of the second vertex");
            // corners given the other way round: the rectangle is the same, the UV order is not flipped
            GSPrimitiveBatch r = spriteBatch(30.9f, 50.2f, 10.1f, 20.7f);
            r.state.prim.tme = true;
            r.state.prim.fst = true;
            r.vertices[0].u = 100 * 16;
            r.vertices[1].u = 0;
            t.IsTrue(BuildSprite(r, 1, s), "reversed sprite");
            t.Equals(s.v[0].x, 10.0f, "reversed: same left edge");
            t.Equals(s.v[0].u, 100.0f, "reversed: the left edge still gets vertex 0 u (as in the CPU renderer)");
            t.Equals(s.v[2].u, 0.0f, "reversed: the right edge gets vertex 1 u");
            // an empty side is one pixel wide
            GSPrimitiveBatch e = spriteBatch(5.0f, 5.0f, 5.0f, 9.0f);
            t.IsTrue(BuildSprite(e, 1, s), "zero width sprite");
            t.Equals(s.v[2].x - s.v[0].x, 1.0f, "one pixel wide");
            t.Equals(s.maxX, 5, "covers the one pixel");
        });

        tc.Run("progressive mapping removes the half line between fields and samples rows at j / sy", [](TestCase &t)
        {
            // The game sends the same vertices in both kinds of field and offsets alternate fields by half a line in OFY.
            GSPrimitiveBatch a = triangleBatch(10.0f, 20.75f, 50.0f, 20.75f, 10.0f, 60.75f);
            GSPrimitiveBatch b = a;
            b.state.context.xyoffset.ofy = static_cast<uint16_t>(1936u * 16u + 8u); // OFY 1936.5
            const TargetMapping progressive{1, 2, true}, weave{1, 1, false};
            Setup sa, sb;
            t.IsTrue(BuildTriangle(a, progressive, sa) && BuildTriangle(b, progressive, sb), "progressive triangles convert");
            for (int i = 0; i < 3; ++i)
            {
                t.Equals(sa.v[i].y, sb.v[i].y, "the two kinds of field have the same geometry");
                t.Equals(sa.v[i].x, sb.v[i].x, "... in x too");
            }
            t.Equals(sa.v[0].x, 10.5f, "x keeps the pixel-centre shift at scale 1");
            t.Equals(sa.v[0].y, 20.75f * 2.0f + 0.5f, "row j of the target samples the field at y = j / 2");
            t.Equals(sa.v[2].y, 60.75f * 2.0f + 0.5f, "vertex 2");
            t.Equals(sa.minY, sb.minY, "same bounding box (min y)");
            t.Equals(sa.maxY, sb.maxY, "same bounding box (max y)");
            // weave mode keeps the half line
            Setup wa, wb;
            t.IsTrue(BuildTriangle(a, weave, wa) && BuildTriangle(b, weave, wb), "weave triangles convert");
            t.Equals(wa.v[0].y, 20.75f + 0.5f, "weave: the first kind of field");
            t.Equals(wb.v[0].y, 20.75f + 0.5f - 0.5f, "weave: the second kind of field is half a line higher on the target");
            // the existing scale parameter still means the same in both axes
            Setup w2;
            t.IsTrue(BuildTriangle(a, 2, w2), "scale 2");
            t.Equals(w2.v[0].y, 20.75f * 2.0f + 0.5f, "scale 2: the GS position times 2, plus half a target pixel");
            // sprites: the rectangle is the same in both kinds of field and twice as tall
            GSPrimitiveBatch sprite = spriteBatch(10.0f, 20.0f, 30.0f, 50.0f);
            GSPrimitiveBatch spriteB = sprite;
            spriteB.state.context.xyoffset.ofy = static_cast<uint16_t>(1936u * 16u + 8u);
            Setup pa, pb;
            t.IsTrue(BuildSprite(sprite, progressive, pa) && BuildSprite(spriteB, progressive, pb), "progressive sprites convert");
            t.Equals(pa.v[0].y, 40.0f, "top edge: twice the GS row");
            t.Equals(pa.v[2].y, 100.0f, "bottom edge");
            t.Equals(pa.v[0].x, 10.0f, "x is not scaled");
            t.Equals(pb.v[0].y, pa.v[0].y, "the second kind of field puts the sprite in the same place");
            t.Equals(pb.v[2].y, pa.v[2].y, "bottom edge too");
            t.Equals(pa.minY, 20, "the box stays in GS pixels (min y)");
            t.Equals(pa.maxY, 49, "the box stays in GS pixels (max y)");
            // a textured sprite on a larger target is sampled at the centre of its GS pixel
            GSDrawState textured{};
            textured.prim.type = GS_PRIM_SPRITE;
            textured.prim.tme = true;
            textured.prim.fst = true;
            textured.textureWidth = 64;
            textured.textureHeight = 32;
            FragUniforms snapped = BuildFragUniforms(textured, true, false, 1u, 2u);
            t.IsTrue((snapped.flags[0] & kFlagSnapToGsPixel) != 0u, "the snap flag is set for a sprite with a target scale above 1");
            t.Equals(snapped.flags[1], 2u, "target rows per GS pixel");
            t.Equals(snapped.flags[2], 1u, "target columns per GS pixel");
            t.IsTrue((BuildFragUniforms(textured, true, false).flags[0] & kFlagSnapToGsPixel) == 0u, "no snapping at scale 1 (the M3 shader path)");
            t.IsTrue((BuildFragUniforms(textured, false, false, 1u, 2u).flags[0] & kFlagSnapToGsPixel) == 0u, "S, T, Q coordinates are never snapped");
        });

        tc.Run("a higher scale samples on the corner grid, so triangle and sprite edges meet and nearest texels are repeated", [](TestCase &t)
        {
            // A triangle with its left edge at GS x = 10 and a sprite starting at GS x = 10 cover from the same target pixel on.
            for (uint32_t n : {2u, 3u, 4u})
            {
                const TargetMapping map{n, 2u * n, true};
                GSPrimitiveBatch tri = triangleBatch(10.0f, 20.0f, 50.0f, 20.0f, 10.0f, 60.0f);
                GSPrimitiveBatch spr = spriteBatch(10.0f, 20.0f, 50.0f, 60.0f);
                Setup st, ss;
                t.IsTrue(BuildTriangle(tri, map, st) && BuildSprite(spr, map, ss), "scale converts");
                const float fn = static_cast<float>(n);
                // vertex + 0.5: the pixel whose centre is first at or after the edge is the one that starts the sprite
                t.Equals(st.v[0].x, 10.0f * fn + 0.5f, "triangle x: GS position times the scale, plus half a target pixel");
                t.Equals(ss.v[0].x, 10.0f * fn, "sprite x: GS position times the scale");
                t.Equals(st.v[0].y, 20.0f * 2.0f * fn + 0.5f, "triangle y: rows are 2 * scale per GS row");
                t.Equals(ss.v[0].y, 20.0f * 2.0f * fn, "sprite y");
                t.Equals(ss.v[2].x, 50.0f * fn, "sprite right edge");
                t.Equals(ss.v[2].y, 60.0f * 2.0f * fn, "sprite bottom edge");
                // target pixel X samples GS position X / n: the centre of pixel X is X + 0.5, the vertex is at 10 n + 0.5
                for (uint32_t k = 0; k < n; ++k)
                {
                    const float position = (static_cast<float>(10u * n + k) + 0.5f - st.v[0].x) / fn + 10.0f;
                    t.Equals(position, 10.0f + static_cast<float>(k) / fn, "target pixel X samples the GS position X / n");
                }
                t.Equals(st.minX, 10, "the box stays in GS pixels");
                t.Equals(ss.maxY, 59, "the sprite box stays in GS pixels");
            }
            // the centred grid (measurement only) puts a block's samples around the scale-1 sample
            {
                const TargetMapping centred{2, 4, true, true};
                GSPrimitiveBatch tri = triangleBatch(10.0f, 20.0f, 50.0f, 20.0f, 10.0f, 60.0f);
                Setup sc;
                t.IsTrue(BuildTriangle(tri, centred, sc), "centred converts");
                t.Equals(sc.v[0].x, 10.0f * 2.0f + 1.0f, "centred: half a block, not half a target pixel (x)");
                t.Equals(sc.v[0].y, 20.0f * 4.0f + 1.0f, "centred: a block row is 2 target rows at scale 2 (y)");
                const TargetMapping centred1{1, 2, true, true};
                const TargetMapping corner1{1, 2, true, false};
                Setup a, b;
                t.IsTrue(BuildTriangle(tri, centred1, a) && BuildTriangle(tri, corner1, b), "scale 1 converts");
                t.Equals(a.v[0].x, b.v[0].x, "at scale 1 both grids are the same (x)");
                t.Equals(a.v[0].y, b.v[0].y, "at scale 1 both grids are the same (y)");
            }
            // sprite sampling flags: at the target pixel keeps the snap data and adds its own flag
            GSDrawState textured{};
            textured.prim.type = GS_PRIM_SPRITE;
            textured.prim.tme = true;
            textured.prim.fst = true;
            textured.textureWidth = 64;
            textured.textureHeight = 32;
            textured.linearFilter = true;
            const FragUniforms atTarget = BuildFragUniforms(textured, true, false, 2u, 4u, true);
            t.IsTrue((atTarget.flags[0] & kFlagSnapToGsPixel) != 0u && (atTarget.flags[0] & kFlagSpriteAtTarget) != 0u, "snap data and the at-target flag");
            t.Equals(atTarget.flags[1], 4u, "target rows per GS pixel");
            t.Equals(atTarget.flags[2], 2u, "target columns per GS pixel");
            const FragUniforms atGs = BuildFragUniforms(textured, true, false, 2u, 4u, false);
            t.IsTrue((atGs.flags[0] & kFlagSpriteAtTarget) == 0u, "no at-target flag when sampling at the GS pixel");
            t.IsTrue((BuildFragUniforms(textured, true, false, 0u, 0u, true).flags[0] & kFlagSpriteAtTarget) == 0u, "no flag without snapping (scale 1, weave)");
        });

        tc.Run("blend equations map to fixed-function blending", [](TestCase &t)
        {
            BlendState a = BlendFromAlpha(true, alphaReg(0, 1, 0, 1)); // (Cs-Cd)*As+Cd
            t.IsTrue(a.enable && a.supported, "alpha blend");
            t.IsTrue(a.src == BlendFactor::SrcAlpha && a.dst == BlendFactor::OneMinusSrcAlpha && a.op == BlendOp::Add, "Cs*As + Cd*(1-As)");
            BlendState add = BlendFromAlpha(true, alphaReg(0, 2, 0, 1)); // (Cs-0)*As+Cd
            t.IsTrue(add.src == BlendFactor::SrcAlpha && add.dst == BlendFactor::One && add.op == BlendOp::Add, "Cs*As + Cd");
            BlendState fix = BlendFromAlpha(true, alphaReg(0, 2, 2, 1, 0x80)); // (Cs-0)*FIX+Cd
            t.IsTrue(fix.coefficientFromFix && fix.src == BlendFactor::SrcAlpha && fix.dst == BlendFactor::One, "Cs*FIX + Cd");
            BlendState sub = BlendFromAlpha(true, alphaReg(0, 1, 0, 2)); // (Cs-Cd)*As+0
            t.IsTrue(sub.supported && sub.op == BlendOp::Subtract && sub.src == BlendFactor::SrcAlpha && sub.dst == BlendFactor::SrcAlpha,
                     "Cs*As - Cd*As");
            BlendState dstAlpha = BlendFromAlpha(true, alphaReg(0, 1, 1, 1)); // (Cs-Cd)*Ad+Cd
            t.IsTrue(dstAlpha.src == BlendFactor::DstAlpha && dstAlpha.dst == BlendFactor::OneMinusDstAlpha, "destination alpha as the coefficient");
            BlendState bad = BlendFromAlpha(true, alphaReg(1, 0, 0, 1)); // (Cd-Cs)*As+Cd = Cd*(1+As) - Cs*As
            t.IsFalse(bad.supported, "a coefficient of 1+C has no fixed-function form");
            t.IsFalse(BlendFromAlpha(false, alphaReg(0, 1, 0, 1)).enable, "ABE off: no blending");
        });

        tc.Run("depth test selection and exact Z values", [](TestCase &t)
        {
            t.Equals(DepthCompare((1ull << 16) | (2ull << 17)), 2u, "ZTE on, GEQUAL");
            t.Equals(DepthCompare((1ull << 16) | (3ull << 17)), 3u, "ZTE on, GREATER");
            t.Equals(DepthCompare((1ull << 16) | (1ull << 17)), 1u, "ZTE on, ALWAYS");
            t.Equals(DepthCompare(2ull << 17), 1u, "ZTE off: always passes, like the CPU renderer");
            // Every 24-bit Z value is exact in a float after dividing by 2^24, and equal values stay equal.
            bool exact = true, ordered = true;
            for (uint32_t z = 0; z < (1u << 24); z += 4099u)
            {
                const float d = static_cast<float>(static_cast<double>(z) * static_cast<double>(kDepthScale));
                exact = exact && static_cast<uint32_t>(d * 16777216.0f) == z && d < 1.0f;
                const float next = static_cast<float>(static_cast<double>(z + 1u) * static_cast<double>(kDepthScale));
                ordered = ordered && next > d;
            }
            exact = exact && static_cast<uint32_t>(static_cast<float>(16777215.0 * static_cast<double>(kDepthScale)) * 16777216.0f) == 16777215u;
            t.IsTrue(exact, "z / 2^24 is exact in a float for every 24-bit Z value tried, and below 1");
            t.IsTrue(ordered, "neighbouring Z values stay distinct and ordered");
        });

        tc.Run("the fragment uniform block", [](TestCase &t)
        {
            GSDrawState s{};
            s.prim.tme = true;
            s.linearFilter = true;
            s.textureWidth = 256;
            s.textureHeight = 128;
            s.context.tex0.tcc = 1;
            s.context.tex0.tfx = 0;
            s.context.clamp = 1ull | (2ull << 2) | (5ull << 4) | (47ull << 14) | (7ull << 24) | (55ull << 34);
            s.context.test = 1ull | (5ull << 1) | (16ull << 4);
            s.context.alpha = (static_cast<uint64_t>(0x80) << 32);
            const FragUniforms u = BuildFragUniforms(s, false, true);
            t.Equals(u.texInfo[0], 256.0f, "width");
            t.Equals(u.texInfo[3], 1.0f / 128.0f, "1/height");
            t.IsTrue((u.flags[0] & kFlagTextured) && (u.flags[0] & kFlagBilinear) && (u.flags[0] & kFlagTcc) && !(u.flags[0] & kFlagTexelCoords),
                     "textured, bilinear, TCC, S/T/Q");
            t.IsTrue((u.flags[0] & kFlagAlphaTest) != 0, "alpha test on");
            t.Equals(u.wrap[0], 1u, "WMS CLAMP");
            t.Equals(u.wrap[1], 2u, "WMT REGION_CLAMP");
            t.Equals(u.wrap[2], 5u | (47u << 16), "U region");
            t.Equals(u.wrap[3], 7u | (55u << 16), "V region");
            t.Equals(u.test[0], 5u, "ATST GEQUAL");
            t.Equals(u.test[1], 16u, "AREF");
            t.Equals(u.test[2], 1u, "blend coefficient is FIX");
            t.Equals(u.test[3], 0x80u, "FIX");
            s.context.test = 1ull | (1ull << 1); // ATST ALWAYS
            t.IsFalse((BuildFragUniforms(s, false, false).flags[0] & kFlagAlphaTest) != 0, "ALWAYS needs no test in the shader");
        });

        tc.Run("texture and transfer page spans follow the GS address rule", [](TestCase &t)
        {
            // a 640x224 CT32 frame at page 0: 10 pages per row, 7 rows
            PageSpan f = BufferPageSpan(GS_PSM_CT32, 0, 10, 0, 0, 640, 224);
            t.Equals(f.first, 0u, "frame starts at page 0");
            t.Equals(f.count, 70u, "10 x 7 pages");
            // the Z buffer at page 140
            PageSpan z = BufferPageSpan(GS_PSM_Z24, 140u * 32u, 10, 0, 0, 640, 224);
            t.Equals(z.first, 140u, "Z buffer at page 140");
            t.Equals(z.count, 70u, "same size");
            // a texture that does not start at a page boundary reaches one page further
            PageSpan aligned = BufferPageSpan(GS_PSM_CT32, 64, 1, 0, 0, 64, 32);
            PageSpan shifted = BufferPageSpan(GS_PSM_CT32, 64 + 4, 1, 0, 0, 64, 32);
            t.Equals(aligned.first, 2u, "block 64 is page 2");
            t.Equals(aligned.count, 1u, "one page when aligned");
            t.Equals(shifted.count, 2u, "one more when the base is inside a page");
            // T4: 128x128 pages
            PageSpan t4 = BufferPageSpan(GS_PSM_T4, 100u * 32u, 8, 0, 0, 512, 512);
            t.Equals(t4.first, 100u, "T4 texture page");
            t.Equals(t4.count, 16u, "4 x 4 pages of 128 x 128");
            // the span wraps around the end of GS memory
            PageSpan w = BufferPageSpan(GS_PSM_CT32, 510u * 32u, 1, 0, 0, 64, 96);
            t.Equals(w.first, 510u, "wrapping span starts at page 510");
            t.Equals(w.count, 3u, "three pages");
        });

        tc.Run("TextureReach bounds the texels a draw can sample, for every wrap mode", [](TestCase &t)
        {
            const auto clampReg = [](uint64_t wms, uint64_t wmt, uint64_t minu, uint64_t maxu, uint64_t minv, uint64_t maxv)
            { return wms | (wmt << 2) | (minu << 4) | (maxu << 14) | (minv << 24) | (maxv << 34); };
            const auto sprite = [](float u0, float v0, float u1, float v1)
            {
                Setup s;
                s.count = 6;
                s.texelCoords = true;
                const float us[6] = {u0, u1, u1, u0, u1, u0}, vs[6] = {v0, v0, v1, v0, v1, v1};
                for (int i = 0; i < 6; ++i)
                {
                    s.v[i].u = us[i];
                    s.v[i].v = vs[i];
                    s.v[i].q = 1.0f;
                }
                return s;
            };
            // CLAMP: the range plus one texel on each side, limited to the texture
            TexelRect r = TextureReach(sprite(10.0f, 20.0f, 50.5f, 30.0f), clampReg(1, 1, 0, 0, 0, 0), 256, 256);
            t.IsTrue(r.x0 == 9 && r.x1 == 51 && r.y0 == 19 && r.y1 == 31, "CLAMP: 9..51 x 19..31");
            r = TextureReach(sprite(-5.0f, -2.0f, 300.0f, 1000.0f), clampReg(1, 1, 0, 0, 0, 0), 256, 128);
            t.IsTrue(r.x0 == 0 && r.x1 == 255 && r.y0 == 0 && r.y1 == 127, "CLAMP: limited to the texture");
            r = TextureReach(sprite(400.0f, 400.0f, 500.0f, 500.0f), clampReg(1, 1, 0, 0, 0, 0), 256, 256);
            t.IsTrue(r.x0 == 255 && r.x1 == 255 && r.y0 == 255 && r.y1 == 255, "CLAMP: a draw entirely past the edge reads the edge texel");
            // REGION_CLAMP: clamped into the region (the portraits of the wrestler select use it)
            r = TextureReach(sprite(0.0f, 4.0f, 100.0f, 90.0f), clampReg(2, 2, 16, 31, 8, 63), 512, 512);
            t.IsTrue(r.x0 == 16 && r.x1 == 31 && r.y0 == 8 && r.y1 == 63, "REGION_CLAMP: 16..31 x 8..63");
            r = TextureReach(sprite(20.0f, 20.0f, 40.0f, 40.0f), clampReg(2, 2, 16, 600, 8, 700), 512, 512);
            t.IsTrue(r.x0 == 19 && r.x1 == 41 && r.y0 == 19 && r.y1 == 41, "REGION_CLAMP: a region wider than the texture still follows the draw");
            // REPEAT: the range when it lies inside the texture, otherwise the whole axis
            r = TextureReach(sprite(10.0f, 20.0f, 50.0f, 30.0f), clampReg(0, 0, 0, 0, 0, 0), 256, 256);
            t.IsTrue(r.x0 == 9 && r.x1 == 51 && r.y0 == 19 && r.y1 == 31, "REPEAT inside the texture");
            r = TextureReach(sprite(10.0f, 20.0f, 300.0f, 30.0f), clampReg(0, 0, 0, 0, 0, 0), 256, 256);
            t.IsTrue(r.x0 == 0 && r.x1 == 255 && r.y0 == 19 && r.y1 == 31, "REPEAT past the edge: the whole axis");
            r = TextureReach(sprite(0.0f, 20.0f, 50.0f, 30.0f), clampReg(0, 0, 0, 0, 0, 0), 256, 256);
            t.IsTrue(r.x0 == 0 && r.x1 == 255, "REPEAT touching the edge: bilinear wraps to the far side, so the whole axis");
            r = TextureReach(sprite(10.0f, 20.0f, 50.0f, 30.0f), clampReg(3, 3, 7, 16, 7, 16), 256, 256);
            t.IsTrue(r.x0 == 0 && r.x1 == 255 && r.y0 == 0 && r.y1 == 255, "REGION_REPEAT: everything");
            // S, T, Q: the ratio times the declared size; a Q that is not positive, NaN, or no vertices: everything
            Setup stq;
            stq.count = 3;
            stq.texelCoords = false;
            const float ss[3] = {0.25f, 0.5f, 0.25f}, ts[3] = {0.25f, 0.25f, 0.5f}, qs[3] = {1.0f, 2.0f, 4.0f};
            for (int i = 0; i < 3; ++i)
            {
                stq.v[i].u = ss[i] * qs[i];
                stq.v[i].v = ts[i] * qs[i];
                stq.v[i].q = qs[i];
            }
            r = TextureReach(stq, clampReg(1, 1, 0, 0, 0, 0), 512, 512);
            t.IsTrue(r.x0 == 127 && r.x1 == 257 && r.y0 == 127 && r.y1 == 257, "STQ: s / q x 512 between 128 and 256, plus a texel each side");
            stq.v[1].q = 0.0f;
            r = TextureReach(stq, clampReg(1, 1, 0, 0, 0, 0), 512, 512);
            t.IsTrue(r.x0 == 0 && r.x1 == 511 && r.y1 == 511, "Q = 0: everything");
            stq.v[1].q = -1.0f;
            r = TextureReach(stq, clampReg(1, 1, 0, 0, 0, 0), 512, 512);
            t.IsTrue(r.x1 == 511 && r.y1 == 511, "negative Q: everything");
            stq.v[1].q = std::nanf("");
            r = TextureReach(stq, clampReg(1, 1, 0, 0, 0, 0), 512, 512);
            t.IsTrue(r.x1 == 511 && r.y1 == 511, "NaN Q: everything");
            Setup none;
            r = TextureReach(none, clampReg(1, 1, 0, 0, 0, 0), 64, 32);
            t.IsTrue(r.x0 == 0 && r.x1 == 63 && r.y0 == 0 && r.y1 == 31, "no vertices: the whole texture");

            // Every texel the shader would fetch must lie inside the rectangle: random draws, all wrap modes, nearest and
            // bilinear, the 1/16 rounding of the coordinate, UV and STQ with perspective-correct interpolation.
            const auto wrapCoord = [](int c, int size, uint32_t mode, int lo, int hi)
            {
                int v;
                if (mode == 0u)
                    v = c & (size - 1);
                else if (mode == 1u)
                    v = std::clamp(c, 0, size - 1);
                else if (mode == 2u)
                    v = std::min(std::max(c, lo), hi);
                else
                    v = (c & lo) | hi;
                return std::clamp(v, 0, size - 1); // the sampler clamps what is left
            };
            uint32_t rng = 12345u;
            const auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0f; };
            uint32_t checked = 0, outside = 0, narrower = 0;
            for (int trial = 0; trial < 3000; ++trial)
            {
                const int sizes[2] = {16 << static_cast<int>(next() * 6.0f), 16 << static_cast<int>(next() * 6.0f)};
                const uint32_t mode[2] = {static_cast<uint32_t>(next() * 4.0f), static_cast<uint32_t>(next() * 4.0f)};
                const int rmin[2] = {static_cast<int>(next() * sizes[0]), static_cast<int>(next() * sizes[1])};
                const int rmax[2] = {static_cast<int>(next() * sizes[0] * 1.2f), static_cast<int>(next() * sizes[1] * 1.2f)};
                const uint64_t reg = clampReg(mode[0], mode[1], static_cast<uint64_t>(rmin[0]), static_cast<uint64_t>(rmax[0]),
                                              static_cast<uint64_t>(rmin[1]), static_cast<uint64_t>(rmax[1]));
                const bool stqMode = next() < 0.4f;
                const int vertexCount = next() < 0.5f ? 3 : 6;
                Setup s;
                s.count = static_cast<uint32_t>(vertexCount);
                s.texelCoords = !stqMode;
                for (int i = 0; i < vertexCount; ++i)
                {
                    const float u = next() * (sizes[0] + 80.0f) - 40.0f, v = next() * (sizes[1] + 80.0f) - 40.0f;
                    const float q = 0.2f + next() * 4.0f;
                    s.v[i].q = stqMode ? q : 1.0f;
                    s.v[i].u = stqMode ? u / sizes[0] * q : u;
                    s.v[i].v = stqMode ? v / sizes[1] * q : v;
                }
                const TexelRect reach = TextureReach(s, reg, static_cast<uint32_t>(sizes[0]), static_cast<uint32_t>(sizes[1]));
                if (reach.x1 - reach.x0 + 1 < sizes[0] || reach.y1 - reach.y0 + 1 < sizes[1])
                    ++narrower;
                for (int sample = 0; sample < 40; ++sample)
                {
                    // a point of the polygon: a convex combination of the first three vertices (or of all for the sprite quad)
                    float w[6] = {};
                    float sum = 0.0f;
                    for (int i = 0; i < 3; ++i)
                    {
                        w[i] = next();
                        sum += w[i];
                    }
                    float us = 0.0f, vs = 0.0f, qs2 = 0.0f;
                    for (int i = 0; i < 3; ++i)
                    {
                        us += w[i] / sum * s.v[i].u;
                        vs += w[i] / sum * s.v[i].v;
                        qs2 += w[i] / sum * s.v[i].q;
                    }
                    float u = stqMode ? us / qs2 * sizes[0] : us;
                    float v = stqMode ? vs / qs2 * sizes[1] : vs;
                    u = std::floor(u * 16.0f + 0.5f) / 16.0f;
                    v = std::floor(v * 16.0f + 0.5f) / 16.0f;
                    const bool bilinear = (sample & 1) != 0;
                    int px[2], py[2], count = 1;
                    if (!bilinear)
                    {
                        px[0] = wrapCoord(static_cast<int>(u), sizes[0], mode[0], rmin[0], rmax[0]);
                        py[0] = wrapCoord(static_cast<int>(v), sizes[1], mode[1], rmin[1], rmax[1]);
                    }
                    else
                    {
                        const int u0 = static_cast<int>(std::floor(u - 0.5f)), v0 = static_cast<int>(std::floor(v - 0.5f));
                        px[0] = wrapCoord(u0, sizes[0], mode[0], rmin[0], rmax[0]);
                        px[1] = wrapCoord(u0 + 1, sizes[0], mode[0], rmin[0], rmax[0]);
                        py[0] = wrapCoord(v0, sizes[1], mode[1], rmin[1], rmax[1]);
                        py[1] = wrapCoord(v0 + 1, sizes[1], mode[1], rmin[1], rmax[1]);
                        count = 2;
                    }
                    for (int a = 0; a < count; ++a)
                        for (int b = 0; b < count; ++b)
                        {
                            ++checked;
                            if (px[a] < reach.x0 || px[a] > reach.x1 || py[b] < reach.y0 || py[b] > reach.y1)
                                ++outside;
                        }
                }
            }
            t.Equals(outside, 0u, "no fetched texel lies outside the reach (" + std::to_string(checked) + " fetches checked)");
            t.IsTrue(narrower > 500u, "the bound is usually narrower than the texture (" + std::to_string(narrower) + " of 3000 draws)");
        });

        tc.Run("TakeMissingTiles hands out each 16 x 16 tile once, in merged rectangles clipped to the texture", [](TestCase &t)
        {
            TileMap map;
            std::vector<TexelRect> out;
            TexelRect want;
            want.x0 = 0;
            want.y0 = 0;
            want.x1 = 103;
            want.y1 = 103;
            TakeMissingTiles(map, want, 512, 512, out);
            t.Equals(out.size(), 1u, "a fresh 104 x 104 reach is one rectangle");
            t.IsTrue(out.size() == 1 && out[0] == TexelRect{0, 0, 111, 111}, "of 7 x 7 whole tiles: 0..111");
            out.clear();
            TakeMissingTiles(map, want, 512, 512, out);
            t.IsTrue(out.empty(), "asking again hands out nothing");
            want.x0 = 100;
            want.y0 = 0;
            want.x1 = 130;
            want.y1 = 40;
            TakeMissingTiles(map, want, 512, 512, out);
            t.IsTrue(out.size() == 1 && out[0] == TexelRect{112, 0, 143, 47}, "an overlapping reach hands out only the new tiles, as one rectangle");
            // a hole in a row splits it in two
            TileMap holed;
            holed.decoded.set(0u * kTilesPerSide + 1u);
            out.clear();
            want.x0 = 0;
            want.y0 = 0;
            want.x1 = 47;
            want.y1 = 15;
            TakeMissingTiles(holed, want, 512, 512, out);
            t.IsTrue(out.size() == 2 && out[0] == TexelRect{0, 0, 15, 15} && out[1] == TexelRect{32, 0, 47, 15}, "a decoded tile in the middle splits the run");
            // rectangles are clipped to a texture that is not a whole number of tiles
            TileMap small;
            out.clear();
            want.x0 = 0;
            want.y0 = 0;
            want.x1 = 39;
            want.y1 = 7;
            TakeMissingTiles(small, want, 40, 8, out);
            t.IsTrue(out.size() == 1 && out[0] == TexelRect{0, 0, 39, 7}, "40 x 8 texture: 40 x 8 texels");
            out.clear();
            TakeMissingTiles(small, want, 40, 8, out);
            t.IsTrue(out.empty(), "and nothing the second time");
            // an empty reach adds nothing
            TexelRect none;
            TakeMissingTiles(small, none, 40, 8, out);
            t.IsTrue(out.empty(), "an empty rectangle hands out nothing");
        });

        tc.Run("shared texel rules: TEXA, CLUT selection, CLD", [](TestCase &t)
        {
            using namespace GSTexel;
            GSTexaReg texa{127, true, 129};
            t.Equals(applyTexa(texa, GS_PSM_CT32, 0x11223344u), 0x11223344u, "CT32 keeps its alpha");
            t.Equals(applyTexa(texa, GS_PSM_CT24, 0x00FFFFFFu), 0x7FFFFFFFu, "CT24 alpha is TA0");
            t.Equals(applyTexa(texa, GS_PSM_CT24, 0x00000000u), 0x00000000u, "AEM: black is transparent");
            t.Equals(applyTexa(texa, GS_PSM_CT16, Rgba5551ToRgba8888(0x8000)), 0x81000000u, "CT16 alpha bit set: TA1");
            t.Equals(Rgba5551ToRgba8888(0x7FFF) & 0x00FFFFFFu, 0x00F8F8F8u, "5551 channels are shifted up");
            t.Equals(swizzleClutIndexCSM1(0x08), 0x10u, "CSM1 swaps bits 3 and 4");
            t.Equals(swizzleClutIndexCSM1(0x10), 0x08u, "and back");
            std::array<uint16_t, 512> clut{};
            clut[5] = 0x1234;
            clut[256 + 5] = 0x5678;
            t.Equals(LookupClutEntry(clut.data(), texa, 5, GS_PSM_CT32, 1, 0, GS_PSM_T8), 0x56781234u, "T8 index 5 of a CT32 CLUT");
            std::array<uint32_t, 2> cbp{};
            GSTex0Reg tex0{};
            tex0.cld = 4;
            tex0.cbp = 77;
            t.IsTrue(ClutLoadWanted(tex0, cbp), "CLD 4 loads when the pointer changed");
            t.Equals(cbp[0], 77u, "and remembers it");
            t.IsFalse(ClutLoadWanted(tex0, cbp), "CLD 4 does not load again for the same pointer");
            tex0.cld = 0;
            t.IsFalse(ClutLoadWanted(tex0, cbp), "CLD 0 never loads");
        });

        tc.Run("the shared transfer code writes and reads back every format it handles", [](TestCase &t)
        {
            GSMem::InitLookupTables();
            std::vector<uint8_t> vram(GSMem::MEMORY_SIZE, 0);
            GSTransferCommand cmd{};
            cmd.bitbltbuf.dbp = 100;
            cmd.bitbltbuf.dbw = 2;
            cmd.bitbltbuf.dpsm = GS_PSM_CT32;
            cmd.trxreg = {70, 3};
            cmd.direction = 0;
            GSTransferSnapshot st{};
            GSTransfer::Begin(cmd, st);
            t.Equals(st.totalPixels, 210u, "70 x 3 pixels");
            std::vector<uint32_t> px(210);
            for (uint32_t i = 0; i < px.size(); ++i)
                px[i] = 0x01010101u * (i & 0xFFu) + i;
            // in two chunks that split a row
            GSTransfer::UploadImage(vram.data(), cmd, st, reinterpret_cast<const uint8_t *>(px.data()), 100 * 4);
            t.Equals(st.copiedPixels, 100u, "first chunk");
            GSTransfer::UploadImage(vram.data(), cmd, st, reinterpret_cast<const uint8_t *>(px.data()) + 100 * 4, 110 * 4);
            t.Equals(st.direction, 3u, "transfer done");
            bool same = true;
            for (uint32_t y = 0; y < 3; ++y)
                for (uint32_t x = 0; x < 70; ++x)
                    same = same && GSTransfer::ReadPixel(vram.data(), GS_PSM_CT32, 100, 2, x, y) == px[y * 70 + x];
            t.IsTrue(same, "every pixel reads back");
            // local to host returns the same bytes
            GSTransferCommand rd = cmd;
            rd.direction = 1;
            rd.bitbltbuf.sbp = 100;
            rd.bitbltbuf.sbw = 2;
            rd.bitbltbuf.spsm = GS_PSM_CT32;
            GSTransferSnapshot rs{};
            GSTransfer::Begin(rd, rs);
            std::vector<uint8_t> host;
            size_t pos = 99;
            GSTransfer::LocalToHost(vram.data(), rd, rs, host, pos);
            t.Equals(host.size(), static_cast<size_t>(210 * 4), "bytes read");
            t.Equals(pos, static_cast<size_t>(0), "read position reset");
            t.IsTrue(std::memcmp(host.data(), px.data(), host.size()) == 0, "the same bytes come back");
        });

        tc.Run("GSPresent::MakePlan: which presentation requests are a plain copy of a frame region", [](TestCase &t)
        {
            using Kind = GSPresent::Plan::Kind;
            const GSPresentationRequest usual = makeRequest(70);
            GSPresent::Plan plan = GSPresent::MakePlan(usual);
            t.IsTrue(plan.kind == Kind::DisplayFrame && plan.dualCircuit, "two circuits on the same frame");
            t.Equals(plan.frame.fbp, 70u, "frame");
            t.Equals(plan.frame.fbw, 10u, "buffer width");
            t.Equals(plan.width, 640u, "width");
            t.Equals(plan.height, 224u, "height");
            t.IsTrue(plan.interlaced, "SMODE2.INT");

            GSPresentationRequest r = usual;
            r.dispfb2 = static_cast<uint64_t>(0) | (10ull << 9);
            t.IsTrue(GSPresent::MakePlan(r).kind == Kind::Complex, "circuits on different frames");
            r = usual;
            r.pmode |= 0x80; // SLBG: the background colour shows through
            t.IsTrue(GSPresent::MakePlan(r).kind == Kind::Complex, "SLBG");
            r = usual;
            r.display2 = (636ull) | (32ull << 12) | (639ull << 32) | (199ull << 44);
            t.IsTrue(GSPresent::MakePlan(r).kind == Kind::Complex, "circuits of different sizes");
            r = usual;
            r.smode2 = 0x1; // INT on, FFMD off: field mode repeats lines
            t.IsTrue(GSPresent::MakePlan(r).kind == Kind::Complex, "field mode");
            r = usual;
            r.pmode = 0x1; // one circuit
            t.IsTrue(GSPresent::MakePlan(r).kind == Kind::DisplayFrame && !GSPresent::MakePlan(r).dualCircuit, "one circuit");
            r.hasPreferredSource = true;
            r.preferredDestFbp = 70;
            r.preferredSource = {200, 10, GS_PSM_CT32, 0};
            t.IsTrue(GSPresent::MakePlan(r).kind == Kind::Complex, "one circuit with a preferred source");
            r.preferredDestFbp = 0; // not this frame
            t.IsTrue(GSPresent::MakePlan(r).kind == Kind::DisplayFrame, "a preferred source for another frame does not apply");
            r = usual;
            r.pmode = 0;
            t.IsTrue(GSPresent::MakePlan(r).kind == Kind::None, "no circuit");
        });

        tc.Run("the GPU renderer publishes a field per marker and Present shows the marker's field", [](TestCase &t)
        {
            if (!SDL_Init(SDL_INIT_VIDEO))
            {
                SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
                if (!SDL_Init(SDL_INIT_VIDEO))
                {
                    std::cout << "    (skipped: no SDL video driver)" << std::endl;
                    return;
                }
            }
            const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
            SDL_GPUDevice *device = SDL_CreateGPUDevice(formats, false, nullptr);
            if (!device)
            {
                std::cout << "    (skipped: no GPU device: " << SDL_GetError() << ")" << std::endl;
                return;
            }
            {
                GSMem::InitLookupTables();
                std::vector<uint8_t> vram(GSMem::MEMORY_SIZE, 0);
                GSGpuBackend gpu(device, 1);
                t.IsTrue(gpu.Ready() && gpu.SetFieldPublishing(true), "the GPU renderer sets up and publishes fields");
                if (gpu.Ready() && gpu.FieldPublishing())
                {
                    gpu.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
                    drawScene(gpu); // draws into frame 0 (fbp 0, 10 wide): flat background (40, 60, 90) and more
                    const GSPresentationRequest request = makeRequest(0);
                    gpu.MarkPresentation(request);

                    GSGpuBackend::FieldInfo first;
                    t.IsTrue(gpu.TakeNewestField(0, first), "a field was published");
                    t.IsTrue(first.valid && first.slot >= 0 && first.width == 640 && first.height == 224 && first.seq == 1, "its size and slot");

                    // The game draws the next field into the same buffer after the marker: the picture of the
                    // marker must not change.
                    GSContext ctx{};
                    ctx.frame = {0, 10, GS_PSM_CT32, 0};
                    ctx.zbuf = {140, GS_PSM_Z24, false};
                    ctx.scissor = {0, 639, 0, 223};
                    ctx.xyoffset = {static_cast<uint16_t>(1728u * 16u), static_cast<uint16_t>(1936u * 16u)};
                    GSPrimitiveBatch overwrite = spriteBatch(0, 0, 640, 224);
                    overwrite.state.context = ctx;
                    overwrite.vertices[0].x = 1728.0f;
                    overwrite.vertices[0].y = 1936.0f;
                    overwrite.vertices[1].x = 1728.0f + 640.0f;
                    overwrite.vertices[1].y = 1936.0f + 224.0f;
                    overwrite.vertices[1].z = 90000.0;
                    overwrite.vertices[1].r = 200;
                    overwrite.vertices[1].g = 10;
                    overwrite.vertices[1].b = 10;
                    overwrite.vertices[1].a = 0x80;
                    gpu.Submit(overwrite);
                    gpu.Flush();
                    PresentationFrame frame = gpu.Present(request);
                    t.IsTrue(static_cast<bool>(frame) && frame.width == 640 && frame.height == 224, "Present returns the picture");
                    if (frame)
                    {
                        const uint8_t *px = &frame.pixels[(200u * 640u + 600u) * 4u];
                        t.IsTrue(std::abs(px[0] - 40) <= 2 && std::abs(px[1] - 60) <= 2 && std::abs(px[2] - 90) <= 2 && px[3] == 255,
                                 "the pixel still has the marker's colour, not the colour drawn after it");
                    }

                    // The ring: a pinned slot is never handed to the GS thread again, and the newest field wins.
                    std::vector<int> others;
                    uint64_t seq = first.seq;
                    bool pinnedReused = false;
                    for (int i = 0; i < 30; ++i)
                    {
                        gpu.MarkPresentation(makeRequest(static_cast<uint32_t>(i % 2 ? 70 : 0)));
                        GSGpuBackend::FieldInfo f;
                        if (gpu.TakeNewestField(seq, f))
                        {
                            seq = f.seq;
                            if (f.slot == first.slot)
                                pinnedReused = true;
                            gpu.UnpinField(f.slot);
                            for (int c = 0; c < f.candidateCount; ++c)
                                gpu.UnpinField(f.candidates[c]);
                        }
                    }
                    t.IsFalse(pinnedReused, "a pinned slot is not overwritten");
                    t.Equals(seq, 31ull, "every marker published a field");
                    t.Equals(gpu.GetStats().fieldsDropped, 0ull, "no field was dropped");
                    gpu.UnpinField(first.slot);
                    for (int c = 0; c < first.candidateCount; ++c)
                        gpu.UnpinField(first.candidates[c]);
                    GSGpuBackend::FieldInfo nothing;
                    t.IsFalse(gpu.TakeNewestField(seq, nothing), "nothing newer than the newest field");

                    // The presenter: the placeholder before any field, then the deinterlaced picture of one.
                    GSGpuPresenter presenter(gpu);
                    t.IsTrue(presenter.Ready(), "the presenter sets up");
                    if (presenter.Ready())
                    {
                        SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(device);
                        presenter.Record(cb);
                        SDL_SubmitGPUCommandBuffer(cb);
                        presenter.Submitted();
                        std::vector<uint8_t> shown;
                        t.IsTrue(presenter.Download(shown), "the picture can be read");
                        if (shown.size() == 640u * 512u * 4u)
                            t.IsTrue(shown[0] == 255 && shown[1] == 0 && shown[2] == 255 && shown[3] == 255, "no field yet: the magenta placeholder");
                        const GSGpuPresenter::Size size = presenter.Pull();
                        t.IsTrue(size.width == 640 && size.height == 448, "an interlaced 640x224 field makes a 640x448 picture");
                        cb = SDL_AcquireGPUCommandBuffer(device);
                        presenter.Record(cb);
                        SDL_SubmitGPUCommandBuffer(cb);
                        presenter.Submitted();
                        if (presenter.Download(shown) && shown.size() == 640u * 512u * 4u)
                        {
                            t.IsTrue(shown[3] == 255, "alpha is 255 inside the picture");
                            t.IsTrue(shown[(460u * 640u + 10u) * 4u + 3u] == 0, "and 0 below it");
                        }
                    }
                }
            }
            SDL_DestroyGPUDevice(device);
        });

        tc.Run("progressive fields: a 640x224 field is drawn as a 640x448 picture, shown as it is, with identical row pairs", [](TestCase &t)
        {
            if (!SDL_Init(SDL_INIT_VIDEO))
            {
                SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
                if (!SDL_Init(SDL_INIT_VIDEO))
                {
                    std::cout << "    (skipped: no SDL video driver)" << std::endl;
                    return;
                }
            }
            const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
            SDL_GPUDevice *device = SDL_CreateGPUDevice(formats, false, nullptr);
            if (!device)
            {
                std::cout << "    (skipped: no GPU device: " << SDL_GetError() << ")" << std::endl;
                return;
            }
            {
                GSMem::InitLookupTables();
                std::vector<uint8_t> vram(GSMem::MEMORY_SIZE, 0);
                GSGpuBackend gpu(device, 1, GSGpuBackend::FieldMode::Progressive);
                t.IsTrue(gpu.Ready() && gpu.SetFieldPublishing(true), "the progressive GPU renderer sets up and publishes fields");
                if (gpu.Ready() && gpu.FieldPublishing())
                {
                    t.Equals(gpu.TargetScaleX(), 1u, "X scale 1");
                    t.Equals(gpu.TargetScaleY(), 2u, "Y scale 2");
                    gpu.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
                    drawScene(gpu); // flat background (40, 60, 90) and more, as in the test above
                    const GSPresentationRequest request = makeRequest(0);
                    gpu.MarkPresentation(request);

                    GSGpuBackend::FieldInfo field;
                    t.IsTrue(gpu.TakeNewestField(0, field), "a field was published");
                    t.IsTrue(field.valid && field.width == 640 && field.height == 448 && field.scaleY == 2 && field.progressive,
                             "the field is 640 x 448 texels");
                    t.IsFalse(field.interlaced, "and nothing for the presenter to weave");

                    // Path A: the picture of the marker, 448 lines.
                    PresentationFrame frame = gpu.Present(request);
                    t.IsTrue(static_cast<bool>(frame) && frame.width == 640 && frame.height == 448, "Present returns a 640x448 picture");
                    if (frame && frame.height == 448)
                    {
                        auto px = [&](uint32_t x, uint32_t y) { return &frame.pixels[(static_cast<size_t>(y) * 640u + x) * 4u]; };
                        const auto nearTo = [](const uint8_t *p, int r, int g, int b) { return std::abs(p[0] - r) <= 2 && std::abs(p[1] - g) <= 2 && std::abs(p[2] - b) <= 2; };
                        t.IsTrue(nearTo(px(600, 400), 40, 60, 90) && px(600, 400)[3] == 255, "the flat background, alpha 255");
                        // the textured sprite (300, 20)-(364, 84): rows 40 .. 167 of the picture. Texel (2, 0) is (32, 0, 184).
                        t.IsTrue(nearTo(px(310, 39), 40, 60, 90), "the row above the sprite's first row is background");
                        t.IsTrue(nearTo(px(310, 40), 32, 0, 184), "the sprite's first row is row 2 * 20");
                        t.IsTrue(nearTo(px(310, 41), 32, 0, 184), "and its second row has the same texel");
                        t.IsTrue(nearTo(px(310, 167), px(310, 166)[0], px(310, 166)[1], px(310, 166)[2]), "the last row pair is one texel");
                        t.IsTrue(nearTo(px(310, 168), 40, 60, 90), "the row below the sprite is background");
                    }

                    // The presenter shows the field as it is.
                    GSGpuPresenter presenter(gpu);
                    t.IsTrue(presenter.Ready(), "the presenter sets up");
                    if (presenter.Ready())
                    {
                        const GSGpuPresenter::Size size = presenter.Pull();
                        t.IsTrue(size.width == 640 && size.height == 448, "a progressive field makes a 640x448 picture");
                        SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(device);
                        presenter.Record(cb);
                        SDL_SubmitGPUCommandBuffer(cb);
                        presenter.Submitted();
                        std::vector<uint8_t> shown;
                        t.IsTrue(presenter.Download(shown) && shown.size() == 640u * 512u * 4u, "the picture can be read");
                        if (shown.size() == 640u * 512u * 4u && frame && frame.height == 448)
                        {
                            bool same = true;
                            for (uint32_t y = 0; y < 448 && same; ++y)
                                for (uint32_t x = 0; x < 640; ++x)
                                    if (std::memcmp(&shown[(static_cast<size_t>(y) * 640u + x) * 4u], &frame.pixels[(static_cast<size_t>(y) * 640u + x) * 4u], 4) != 0)
                                    {
                                        same = false;
                                        break;
                                    }
                            t.IsTrue(same, "the direct picture is exactly what Present returns (448 lines, no weaving)");
                            t.IsTrue(shown[(460u * 640u + 10u) * 4u + 3u] == 0, "and nothing below the picture");
                        }
                    }
                }
            }
            SDL_DestroyGPUDevice(device);
        });

        tc.Run("a black display frame at fbp 0 shows the other frame (Compose's fallback), and the GPU picture agrees", [](TestCase &t)
        {
            if (!SDL_Init(SDL_INIT_VIDEO))
            {
                SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
                if (!SDL_Init(SDL_INIT_VIDEO))
                {
                    std::cout << "    (skipped: no SDL video driver)" << std::endl;
                    return;
                }
            }
            const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
            SDL_GPUDevice *device = SDL_CreateGPUDevice(formats, false, nullptr);
            if (!device)
            {
                std::cout << "    (skipped: no GPU device: " << SDL_GetError() << ")" << std::endl;
                return;
            }
            {
                GSMem::InitLookupTables();
                std::vector<uint8_t> vram(GSMem::MEMORY_SIZE, 0);
                GSGpuBackend gpu(device, 1);
                t.IsTrue(gpu.Ready() && gpu.SetFieldPublishing(true), "the GPU renderer sets up and publishes fields");
                if (gpu.Ready() && gpu.FieldPublishing())
                {
                    gpu.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
                    GSPresentationRequest request = makeRequest(0);
                    request.contextFrames[0] = {0, 10, GS_PSM_CT32, 0};
                    request.contextFrames[1] = {70, 10, GS_PSM_CT32, 0};
                    // a full-screen flat sprite into the frame buffer at `fbp`
                    auto fill = [&](uint32_t fbp, uint8_t r, uint8_t g, uint8_t b)
                    {
                        GSContext ctx{};
                        ctx.frame = {fbp, 10, GS_PSM_CT32, 0};
                        ctx.zbuf = {140, GS_PSM_Z24, false};
                        ctx.scissor = {0, 639, 0, 223};
                        ctx.xyoffset = {static_cast<uint16_t>(1728u * 16u), static_cast<uint16_t>(1936u * 16u)};
                        GSPrimitiveBatch p = spriteBatch(0, 0, 640, 224);
                        p.state.context = ctx;
                        p.vertices[0].x = 1728.0f;
                        p.vertices[0].y = 1936.0f;
                        p.vertices[1].x = 1728.0f + 640.0f;
                        p.vertices[1].y = 1936.0f + 224.0f;
                        p.vertices[1].z = 100.0;
                        p.vertices[1].r = r;
                        p.vertices[1].g = g;
                        p.vertices[1].b = b;
                        p.vertices[1].a = 0x80;
                        gpu.Submit(p);
                    };
                    GSFieldDeinterlacer readback;
                    GSGpuPresenter presenter(gpu);
                    t.IsTrue(presenter.Ready(), "the presenter sets up");
                    auto both = [&](const char *what, int expectR, int expectG, int expectB)
                    {
                        gpu.MarkPresentation(request);
                        PresentationFrame frame = gpu.Present(request);
                        t.IsTrue(static_cast<bool>(frame), std::string(what) + ": Present returns a picture");
                        if (!frame || !presenter.Ready())
                            return;
                        const uint8_t *px = &frame.pixels[(100u * 640u + 300u) * 4u];
                        t.IsTrue(std::abs(px[0] - expectR) <= 2 && std::abs(px[1] - expectG) <= 2 && std::abs(px[2] - expectB) <= 2,
                                 std::string(what) + ": Present shows the expected colour");
                        // the same through the runtime's CPU deinterlacer, and through the GPU presenter
                        std::vector<uint8_t> packed(640u * 224u * 4u), expected(640u * 512u * 4u, 0u);
                        for (uint32_t y = 0; y < 224u; ++y)
                            std::memcpy(&packed[y * 640u * 4u], &frame.pixels[y * 640u * 4u], 640u * 4u);
                        readback.process(packed.data(), 640u, 224u, expected.data(), 640u * 4u);
                        presenter.Pull();
                        SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(device);
                        presenter.Record(cb);
                        SDL_SubmitGPUCommandBuffer(cb);
                        presenter.Submitted();
                        std::vector<uint8_t> got;
                        t.IsTrue(presenter.Download(got) && got.size() == expected.size(), std::string(what) + ": the GPU picture can be read");
                        if (got.size() == expected.size())
                            t.IsTrue(std::memcmp(got.data(), expected.data(), expected.size()) == 0,
                                     std::string(what) + ": the GPU picture equals the CPU deinterlacer's on Present's pixels");
                    };
                    fill(70, 40, 60, 90);
                    fill(0, 0, 0, 0);       // the displayed frame is black
                    both("black display frame", 40, 60, 90);
                    fill(70, 40, 60, 90);
                    fill(0, 200, 10, 10);   // not black any more: no substitution
                    both("non-black display frame", 200, 10, 10);
                    fill(0, 0, 0, 0);
                    fill(70, 0, 0, 0);      // both black: stays black
                    both("both frames black", 0, 0, 0);
                }
            }
            SDL_DestroyGPUDevice(device);
        });

        tc.Run("the GPU deinterlacer equals the CPU one on noise, on repeated fields (ties) and on black fields", [](TestCase &t)
        {
            if (!SDL_Init(SDL_INIT_VIDEO))
            {
                SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
                if (!SDL_Init(SDL_INIT_VIDEO))
                {
                    std::cout << "    (skipped: no SDL video driver)" << std::endl;
                    return;
                }
            }
            const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
            SDL_GPUDevice *device = SDL_CreateGPUDevice(formats, false, nullptr);
            if (!device)
            {
                std::cout << "    (skipped: no GPU device: " << SDL_GetError() << ")" << std::endl;
                return;
            }
            {
                GSMem::InitLookupTables();
                std::vector<uint8_t> vram(GSMem::MEMORY_SIZE, 0);
                GSGpuBackend gpu(device, 1);
                t.IsTrue(gpu.Ready() && gpu.SetFieldPublishing(true), "the GPU renderer sets up and publishes fields");
                if (gpu.Ready() && gpu.FieldPublishing())
                {
                    gpu.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
                    GSFieldDeinterlacer readback;
                    GSGpuPresenter presenter(gpu);
                    t.IsTrue(presenter.Ready(), "the presenter sets up");

                    // A 640 x 224 field of `pattern` drawn 1:1 into the frame buffer at `fbp`: uploaded as a CT32
                    // texture and drawn as a nearest-filtered, modulated sprite, which reproduces the texels exactly.
                    // pattern 0: noise (seeded), 1: black, 2: the same noise as the last call.
                    uint32_t seed = 12345u, lastSeed = seed;
                    auto field = [&](uint32_t fbp, int pattern, uint32_t newSeed)
                    {
                        std::vector<uint32_t> texels(640u * 224u);
                        if (pattern == 0)
                            lastSeed = newSeed;
                        uint32_t x = lastSeed * 2654435761u + 1u;
                        for (uint32_t &px : texels)
                        {
                            x ^= x << 13;
                            x ^= x >> 17;
                            x ^= x << 5;
                            // noise, with half of the picture smooth so that "nothing moved" and "moved" both occur
                            px = pattern == 1 ? 0xFF000000u : ((x & 0x00FFFFFFu) | 0x80000000u);
                        }
                        if (pattern != 1)
                            for (uint32_t i = 0; i < texels.size(); ++i)
                                if ((i % 640u) < 320u)
                                    texels[i] = (texels[i] & 0xFFF0F0F0u) | 0x80000000u; // coarser values on the left half
                        GSTransferCommand cmd{};
                        cmd.bitbltbuf.dbp = 8000;
                        cmd.bitbltbuf.dbw = 10;
                        cmd.bitbltbuf.dpsm = GS_PSM_CT32;
                        cmd.trxreg = {640, 224};
                        cmd.direction = 0;
                        gpu.BeginTransfer(cmd);
                        gpu.UploadImage(reinterpret_cast<const uint8_t *>(texels.data()), static_cast<uint32_t>(texels.size() * 4));
                        gpu.TextureFlush();
                        GSContext ctx{};
                        ctx.frame = {fbp, 10, GS_PSM_CT32, 0};
                        ctx.zbuf = {140, GS_PSM_Z24, false};
                        ctx.scissor = {0, 639, 0, 223};
                        ctx.xyoffset = {static_cast<uint16_t>(1728u * 16u), static_cast<uint16_t>(1936u * 16u)};
                        ctx.tex0 = {8000, 10, GS_PSM_CT32, 10, 8, 1, 0, 0, 0, 0, 0, 0}; // 1024 x 256 declared, 640 wide in memory
                        GSPrimitiveBatch p;
                        p.vertexCount = 2;
                        p.state.context = ctx;
                        p.state.prim.type = GS_PRIM_SPRITE;
                        p.state.prim.tme = true;
                        p.state.prim.fst = true;
                        p.state.textureWidth = 1024;
                        p.state.textureHeight = 256;
                        p.vertices[0].x = 1728.0f;
                        p.vertices[0].y = 1936.0f;
                        p.vertices[0].u = 0;
                        p.vertices[0].v = 0;
                        p.vertices[1].x = 1728.0f + 640.0f;
                        p.vertices[1].y = 1936.0f + 224.0f;
                        p.vertices[1].u = 640 * 16;
                        p.vertices[1].v = 224 * 16;
                        p.vertices[1].z = 100.0;
                        p.vertices[1].r = p.vertices[1].g = p.vertices[1].b = p.vertices[1].a = 0x80;
                        gpu.Submit(p);
                    };

                    uint64_t compared = 0, differing = 0, ties = 0;
                    auto step = [&](const char *what, uint32_t fbp, int pattern, uint32_t newSeed)
                    {
                        field(fbp, pattern, newSeed);
                        GSPresentationRequest request = makeRequest(fbp);
                        gpu.MarkPresentation(request);
                        PresentationFrame frame = gpu.Present(request);
                        t.IsTrue(static_cast<bool>(frame) && frame.width == 640 && frame.height == 224, std::string(what) + ": Present returns the field");
                        if (!frame)
                            return;
                        std::vector<uint8_t> packed(640u * 224u * 4u), expected(640u * 512u * 4u, 0u);
                        for (uint32_t y = 0; y < 224u; ++y)
                            std::memcpy(&packed[y * 640u * 4u], &frame.pixels[y * 640u * 4u], 640u * 4u);
                        readback.process(packed.data(), 640u, 224u, expected.data(), 640u * 4u);
                        if (readback.lastDecision().measured && readback.lastDecision().lowerError == readback.lastDecision().upperError)
                            ++ties;
                        presenter.Pull();
                        SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(device);
                        presenter.Record(cb);
                        SDL_SubmitGPUCommandBuffer(cb);
                        presenter.Submitted();
                        std::vector<uint8_t> got;
                        if (!presenter.Download(got) || got.size() != expected.size())
                        {
                            t.Fail(std::string(what) + ": the GPU picture could not be read");
                            return;
                        }
                        ++compared;
                        if (std::memcmp(got.data(), expected.data(), expected.size()) != 0)
                        {
                            ++differing;
                            t.Fail(std::string(what) + ": the GPU picture differs from the CPU deinterlacer's");
                        }
                    };

                    // alternating buffers like the game; noise, a repeated field (a tie), black, noise again
                    uint32_t fbp = 0;
                    for (int i = 0; i < 6; ++i)
                    {
                        step("noise", fbp, 0, 100u + static_cast<uint32_t>(i));
                        fbp = fbp == 0 ? 70 : 0;
                    }
                    step("repeat", fbp, 2, 0);   // the same noise as the last field, drawn into the other buffer: identical fields
                    fbp = fbp == 0 ? 70 : 0;
                    step("repeat", fbp, 2, 0);
                    fbp = fbp == 0 ? 70 : 0;
                    step("black", fbp, 1, 0);
                    fbp = fbp == 0 ? 70 : 0;
                    step("black", fbp, 1, 0);    // black after black: another tie
                    fbp = fbp == 0 ? 70 : 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        step("noise again", fbp, 0, 200u + static_cast<uint32_t>(i));
                        fbp = fbp == 0 ? 70 : 0;
                    }
                    t.Equals(differing, 0ull, "no picture differs");
                    t.IsTrue(compared == 14u, "14 pictures were compared (" + std::to_string(compared) + ")");
                    t.IsTrue(ties >= 2u, "the sequence contained ties (" + std::to_string(ties) + ")");
                }
            }
            SDL_DestroyGPUDevice(device);
        });

        tc.Run("the GPU renderer draws a small scene like the CPU renderer", [](TestCase &t)
        {
            if (!SDL_Init(SDL_INIT_VIDEO))
            {
                SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
                if (!SDL_Init(SDL_INIT_VIDEO))
                {
                    std::cout << "    (skipped: no SDL video driver)" << std::endl;
                    return;
                }
            }
            const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
            SDL_GPUDevice *device = SDL_CreateGPUDevice(formats, false, nullptr);
            if (!device)
            {
                std::cout << "    (skipped: no GPU device: " << SDL_GetError() << ")" << std::endl;
                return;
            }
            {
                GSMem::InitLookupTables();
                std::vector<uint8_t> vramCpu(GSMem::MEMORY_SIZE, 0), vramGpu(GSMem::MEMORY_SIZE, 0);
                GSCpuBackend cpu;
                GSGpuBackend gpu(device, 1);
                t.IsTrue(gpu.Ready(), "the GPU renderer sets up");
                if (gpu.Ready())
                {
                    const SceneResult a = runScene(cpu, vramCpu, &cpu, nullptr);
                    const SceneResult b = runScene(gpu, vramGpu, nullptr, &gpu);
                    t.IsTrue(a.ok && b.ok, "both renderers returned a 640x224 picture");
                    if (a.ok && b.ok)
                    {
                        uint64_t differing = 0;
                        int maxDiff = 0;
                        for (uint32_t y = 0; y < 224; ++y)
                            for (uint32_t x = 0; x < 640; ++x)
                            {
                                int worst = 0;
                                for (int c = 0; c < 3; ++c)
                                    worst = std::max(worst, std::abs(static_cast<int>(a.pixels[(y * 640 + x) * 4 + c]) - static_cast<int>(b.pixels[(y * 640 + x) * 4 + c])));
                                maxDiff = std::max(maxDiff, worst);
                                differing += worst > 2 ? 1u : 0u;
                            }
                        // flat parts, the blend, the nearest and bilinear textures and the depth test must agree
                        t.IsTrue(differing < 640u * 224u / 200u, "fewer than 0.5% of the pixels differ by more than 2 (" + std::to_string(differing) + " do, largest difference " + std::to_string(maxDiff) + ")");
                        // the background, a covered point, and a point the hidden sprite must not change
                        auto px = [&](const SceneResult &r, uint32_t x, uint32_t y) { return std::vector<uint8_t>(&r.pixels[(y * 640 + x) * 4], &r.pixels[(y * 640 + x) * 4] + 3); };
                        t.IsTrue(px(a, 600, 200) == px(b, 600, 200), "the flat background is identical");
                        t.IsTrue(px(a, 330, 50) == px(b, 330, 50), "the textured sprite shows where the hidden sprite lies behind it");
                    }
                }
            }
            SDL_DestroyGPUDevice(device);
        });
    });
}
