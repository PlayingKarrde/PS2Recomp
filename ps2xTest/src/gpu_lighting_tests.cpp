#include "MiniTest.h"
#include "gpu/gpu_lighting.h"
#include "gpu/gpu_postfx.h"
#include "ps2_settings.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_presentation.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// Tests for the first part of the match lights (task lt2-L0a): undoing the projection, face normals, the camera channel that
// keeps a camera with its frame, and the light arithmetic. No GPU and no game data at test time.
//
// The numbers below come from two frames of GS recordings of a match in arena 0 (the design study, docs/hd-lighting-tier2.md
// in the game's repository; scripts lt1_unproject.py and lt2_consts.py). For each frame: the camera matrix fitted from the hall,
// a few vertices of objects that were NOT used for the fit (screen x, y and Q as the renderer receives them, and the position
// the model file gives them), and a few triangles with the normal they should have.

namespace
{
    using namespace ps2x_lighting;

    struct Sample
    {
        float x, y, q;
        float world[3];
    };

    struct Triangle
    {
        float xyq[9]; // three corners: x, y, q
        float normal[3];
    };

    // ring.gsrec marker 300 (the match camera): the camera matrix fitted from the hall (lt1_unproject.py); the Z column is not used and left 0
    const float kRingMatrix[16] = {3267.94971f, 655.305847f, 0.0f, 0.593029678f, -1115.39014f, -2332.95898f, 0.0f, -0.544638157f, -838.93042f, 655.309021f, 0.0f, 0.593029678f, 5326387.0f, 5569804.0f, 0.0f, 2576.92407f};
    const Sample kRingSamples[] = {
        {2071.875f, 2349.6875f, 0.000488272577f, {-400.0f, 100.0f, -400.0f}}, // apron
        {2346.6875f, 2250.1875f, 0.000449247338f, {-100.0f, 100.0f, -400.0f}}, // apron
        {2580.875f, 2165.4375f, 0.000415998598f, {200.0f, 100.0f, -400.0f}}, // apron
        {1416.125f, 2115.5625f, 0.00039643838f, {-400.0f, 100.000015f, 400.0f}}, // apron
        {1630.9375f, 2192.25f, 0.000426520855f, {-400.0f, 100.0f, 100.0f}}, // apron
        {1881.0f, 2281.5625f, 0.000461543648f, {-400.0f, 99.9999924f, -200.0f}}, // apron
        {2091.5625f, 1797.25f, 0.000260151079f, {1113.3938f, 35.4595184f, 1055.65271f}}, // crowd
        {3262.8125f, 2090.8125f, 0.000366631313f, {930.797424f, 19.3805752f, -659.028748f}}, // crowd
        {2969.125f, 1913.625f, 0.000318995997f, {1211.29346f, 108.227959f, -171.115128f}}, // crowd
        {3243.1875f, 1917.375f, 0.000317945582f, {1426.9364f, 96.6350021f, -379.940643f}}, // crowd
        {3069.4375f, 1980.75f, 0.000345682696f, {1024.0636f, 109.163925f, -391.115143f}}, // crowd
        {3091.4375f, 1974.625f, 0.000342105428f, {1070.48193f, 104.166969f, -391.115143f}}, // crowd
    };
    // eye of Ring: -1462.483 1572.539 -1438.652
    const Triangle kRingTriangles[] = {
        {{2071.875f, 2349.6875f, 0.000488272577f, 2168.625f, 2314.6875f, 0.000474532018f, 2071.25f, 2399.8125f, 0.000475624198f}, {0.0f, 0.0f, -1.0f}}, // apron, normal from the file
        {{2569.0625f, 2212.375f, 0.000406782172f, 2496.3125f, 2239.75f, 0.00041683772f, 2580.875f, 2165.4375f, 0.000415998598f}, {0.0f, 0.0f, -1.0f}}, // apron, normal from the file
        {{1630.9375f, 2192.25f, 0.000426520855f, 1709.9375f, 2220.5f, 0.000437589217f, 1640.375f, 2239.75f, 0.00041683772f}, {-1.0f, -0.0f, -0.0f}}, // apron, normal from the file
        {{1416.125f, 2115.5625f, 0.00039643838f, 1510.1875f, 2092.375f, 0.000387332198f, 1484.25f, 2139.9375f, 0.00040598301f}, {0.0f, 1.0f, 0.0f}}, // mat grid: faces up
        {{2294.1875f, 2049.0f, 0.000370320806f, 2305.5f, 2092.375f, 0.000387334178f, 2377.5f, 2070.1875f, 0.000378635974f}, {0.0f, 1.0f, 0.0f}}, // mat grid: faces up
        {{2029.625f, 1948.75f, 0.000370502763f, 1976.25f, 1951.75f, 0.000370031135f, 2035.375f, 1976.4375f, 0.000366114778f}, {-0.707106769f, 0.0f, -0.707106769f}}, // wrestler part: an upright card that faces the camera side
        {{1976.25f, 1951.75f, 0.000370031135f, 2035.375f, 1976.4375f, 0.000366114778f, 1982.625f, 1979.25f, 0.000365668122f}, {-0.707106769f, 0.0f, -0.707106769f}}, // the next triangle of the strip (opposite winding)
        {{2017.5f, 1993.1875f, 0.000363455358f, 1964.75f, 1993.25f, 0.000363448722f, 2017.8125f, 2019.9375f, 0.000359208119f}, {-0.707106769f, 0.0f, -0.707106769f}}, // wrestler part: an upright card that faces the camera side
        {{1964.75f, 1993.25f, 0.000363448722f, 2017.8125f, 2019.9375f, 0.000359208119f, 1965.75f, 2019.9375f, 0.000359210826f}, {-0.707106769f, 0.0f, -0.707106769f}}, // the next triangle of the strip (opposite winding)
        {{1903.75f, 2033.375f, 0.000364797219f, 2045.0f, 2033.375f, 0.000364797219f, 1908.0f, 2005.8125f, 0.000353967742f}, {0.0f, 1.0f, 0.0f}}, // wrestler shadow: flat on the mat
    };
    // fx1_end.gsrec marker 945 (the far, time-over camera): the camera matrix fitted from the hall (lt1_unproject.py); the Z column is not used and left 0
    const float kFarMatrix[16] = {3267.94922f, 655.303894f, 0.0f, 0.593029559f, -1115.85181f, -2333.30151f, 0.0f, -0.544635296f, -838.911926f, 655.300903f, 0.0f, 0.593029499f, 19583672.0f, 19715006.0f, 0.0f, 9561.09766f};
    const Sample kFarSamples[] = {
        {2048.3125f, 2098.875f, 0.000110714878f, {-400.0f, 100.0f, -400.0f}}, // apron
        {2115.1875f, 2079.6875f, 0.000108576227f, {-100.0f, 100.0f, -400.0f}}, // apron
        {2179.5f, 2061.1875f, 0.000106518652f, {200.0f, 100.0f, -400.0f}}, // apron
        {1875.4375f, 2049.25f, 0.000105189705f, {-400.0f, 100.000015f, 400.0f}}, // apron
        {1938.1875f, 2067.3125f, 0.000107195789f, {-400.0f, 100.0f, 100.0f}}, // apron
        {2003.375f, 2086.0f, 0.00010927987f, {-400.0f, 99.9999924f, -200.0f}}, // apron
        {2519.125f, 2106.375f, 0.000112471869f, {519.955811f, 142.531967f, -1518.8219f}}, // crowd
        {1577.4375f, 2106.375f, 0.000112471869f, {-1518.8219f, 142.531967f, 519.955811f}}, // crowd
        {2059.1875f, 1944.375f, 9.23523621e-05f, {1113.3938f, 35.4595184f, 1055.65271f}}, // crowd
        {2377.8125f, 2022.8125f, 0.000102144179f, {1022.29346f, 95.0359497f, -548.884766f}}, // crowd
        {2491.0f, 1992.625f, 9.86871964e-05f, {1615.9364f, 90.0389938f, -568.8255f}}, // crowd
        {2382.4375f, 2000.8125f, 9.99690819e-05f, {1236.6886f, 109.163925f, -391.115143f}}, // crowd
    };
    // eye of Far: -5634.691 5284.463 -5634.549
    const Triangle kFarTriangles[] = {
        {{2048.3125f, 2098.875f, 0.000110714878f, 2070.875f, 2092.375f, 0.000109992696f, 2048.3125f, 2112.0f, 0.000110051267f}, {0.0f, 0.0f, -1.0f}}, // apron, normal from the file
        {{2178.75f, 2074.0f, 0.000105904248f, 2157.6875f, 2080.125f, 0.000106573578f, 2179.5f, 2061.1875f, 0.000106518652f}, {0.0f, 0.0f, -1.0f}}, // apron, normal from the file
        {{1938.1875f, 2067.3125f, 0.000107195789f, 1959.6875f, 2073.4375f, 0.000107881591f, 1938.875f, 2080.125f, 0.000106573578f}, {-1.0f, -0.0f, -0.0f}}, // apron, normal from the file
        {{1875.4375f, 2049.25f, 0.000105189705f, 1898.0f, 2043.4375f, 0.000104537598f, 1896.125f, 2055.1875f, 0.000105849998f}, {0.0f, 1.0f, 0.0f}}, // mat grid: faces up
        {{2111.875f, 2031.9375f, 0.000103257364f, 2112.6875f, 2043.4375f, 0.000104537656f, 2133.625f, 2037.625f, 0.000103893552f}, {0.0f, 1.0f, 0.0f}}, // mat grid: faces up
        {{2061.25f, 2004.9375f, 0.000103254381f, 2075.875f, 2006.5f, 0.000103181934f, 2058.125f, 2012.375f, 0.000102916281f}, {-0.707106769f, 0.0f, -0.707106769f}}, // wrestler part: an upright card that faces the camera side
        {{2075.875f, 2006.5f, 0.000103181934f, 2058.125f, 2012.375f, 0.000102916281f, 2072.6875f, 2013.9375f, 0.000102845392f}, {-0.707106769f, 0.0f, -0.707106769f}}, // the next triangle of the strip (opposite winding)
        {{2062.6875f, 2016.8125f, 0.000102714905f, 2077.5625f, 2016.3125f, 0.000102738377f, 2063.6875f, 2024.375f, 0.000102373815f}, {-0.707106769f, 0.0f, -0.707106769f}}, // wrestler part: an upright card that faces the camera side
        {{2077.5625f, 2016.3125f, 0.000102738377f, 2063.6875f, 2024.375f, 0.000102373815f, 2078.5f, 2023.8125f, 0.000102397622f}, {-0.707106769f, 0.0f, -0.707106769f}}, // the next triangle of the strip (opposite winding)
        {{2056.375f, 2027.75f, 0.000102841419f, 2096.1875f, 2027.75f, 0.000102841419f, 2056.3125f, 2019.875f, 0.000101961989f}, {0.0f, 1.0f, 0.0f}}, // wrestler shadow: flat on the mat
    };

    double distance(const float a[3], const float b[3])
    {
        return std::sqrt((double(a[0]) - b[0]) * (double(a[0]) - b[0]) + (double(a[1]) - b[1]) * (double(a[1]) - b[1]) +
                         (double(a[2]) - b[2]) * (double(a[2]) - b[2]));
    }

    double degreesBetween(const float a[3], const float b[3])
    {
        const double dot = double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
        return std::acos(std::fmax(-1.0, std::fmin(1.0, dot))) * 180.0 / 3.14159265358979;
    }

    template <size_t N>
    double worstPosition(const float matrix[16], const Sample (&samples)[N])
    {
        const Unprojector u = Unprojector::from(matrix);
        double worst = u.valid ? 0.0 : 1e9;
        for (const Sample &s : samples)
        {
            float p[3];
            if (!u.worldAt(s.x, s.y, s.q, p))
                return 1e9;
            worst = std::fmax(worst, distance(p, s.world));
        }
        return worst;
    }

    template <size_t N>
    double worstNormal(const float matrix[16], const Triangle (&triangles)[N])
    {
        const Unprojector u = Unprojector::from(matrix);
        double worst = u.valid ? 0.0 : 1e9;
        for (const Triangle &t : triangles)
        {
            float p[3][3], n[3];
            for (int i = 0; i < 3; ++i)
                if (!u.worldAt(t.xyq[i * 3], t.xyq[i * 3 + 1], t.xyq[i * 3 + 2], p[i]))
                    return 1e9;
            if (!faceNormal(p[0], p[1], p[2], u.eye, n))
                return 1e9;
            worst = std::fmax(worst, degreesBetween(n, t.normal));
        }
        return worst;
    }

    std::string number(double v)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "%.4f", v);
        return text;
    }

    // A camera in the game's convention, for the made-up cases: looks along +z from (0, 0, -1000), screen distance 500, centre 2048.
    void simpleCamera(float m[16])
    {
        const float rows[16] = {500.0f, 0.0f, 0.0f, 0.0f, 0.0f, -500.0f, 0.0f, 0.0f, 2048.0f, 2048.0f, 1.0f, 1.0f, 2048000.0f, 2048000.0f, 0.0f, 1000.0f};
        std::memcpy(m, rows, sizeof(rows));
    }

    void project(const float m[16], const double p[3], double &x, double &y, double &w)
    {
        const double X = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
        const double Y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
        w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
        x = X / w;
        y = Y / w;
    }

    // ---- the debug views on the GPU renderer (task lt2-L0b)
    void setEnvVar(const char *name, const char *value)
    {
#if defined(_WIN32)
        _putenv_s(name, value ? value : "");
#else
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
#endif
    }

    GSContext sceneContext()
    {
        GSContext ctx{};
        ctx.frame = {0, 10, GS_PSM_CT32, 0};
        ctx.zbuf = {140, GS_PSM_Z24, false};
        ctx.scissor = {0, 639, 0, 223};
        ctx.xyoffset = {static_cast<uint16_t>(1728u * 16u), static_cast<uint16_t>(1936u * 16u)};
        ctx.test = (1ull << 16) | (2ull << 17); // ZTE on, GEQUAL
        return ctx;
    }

    // A flat triangle given by three world points, projected with `m` the way the game does: x and y cut to 1/16 pixel, Q = 1 / w.
    void worldTriangle(GSRasterBackend &b, const float m[16], const double points[3][3], double z, uint8_t r, uint8_t g, uint8_t bl, bool depthWrites = true)
    {
        GSPrimitiveBatch p;
        p.vertexCount = 3;
        p.state.context = sceneContext();
        p.state.context.zbuf.zmask = !depthWrites; // (the game draws its effect billboards without depth writes)
        p.state.prim.type = GS_PRIM_TRIANGLE;
        p.state.prim.iip = false;
        p.state.prim.fst = false;
        for (size_t i = 0; i < 3; ++i)
        {
            double x, y, w;
            project(m, points[i], x, y, w);
            p.vertices[i].x = static_cast<float>(std::floor(x * 16.0) / 16.0);
            p.vertices[i].y = static_cast<float>(std::floor(y * 16.0) / 16.0);
            p.vertices[i].z = z;
            p.vertices[i].q = static_cast<float>(1.0 / w);
            p.vertices[i].r = r;
            p.vertices[i].g = g;
            p.vertices[i].b = bl;
            p.vertices[i].a = 0x80;
        }
        b.Submit(p);
    }

    // One field: a clear, a floor triangle 200 units below the eye (it faces up) and a wall triangle that faces the eye (-z).
    void drawWorldField(GSRasterBackend &b, const float m[16])
    {
        GSPrimitiveBatch clear;
        clear.vertexCount = 2;
        clear.state.context = sceneContext();
        clear.state.context.test = (1ull << 16) | (1ull << 17);
        clear.state.prim.type = GS_PRIM_SPRITE;
        clear.vertices[0].x = 1728.0f;
        clear.vertices[0].y = 1936.0f;
        clear.vertices[1].x = 1728.0f + 640.0f;
        clear.vertices[1].y = 1936.0f + 224.0f;
        clear.vertices[1].r = 30;
        clear.vertices[1].g = 40;
        clear.vertices[1].b = 60;
        clear.vertices[1].a = 0x80;
        b.Submit(clear);
        const double floorPoints[3][3] = {{-150.0, -200.0, 0.0}, {150.0, -200.0, 0.0}, {0.0, -200.0, 1000.0}};
        const double wallPoints[3][3] = {{-100.0, -100.0, 50.0}, {100.0, -100.0, 50.0}, {0.0, 100.0, 50.0}};
        worldTriangle(b, m, floorPoints, 1000.0, 200, 60, 20);
        worldTriangle(b, m, wallPoints, 2000.0, 20, 60, 200);
    }

    struct Field
    {
        std::vector<uint8_t> px; // 640 x 448 RGBA
        bool ok = false;
        const uint8_t *at(uint32_t x, uint32_t y) const { return &px[(static_cast<size_t>(y) * 640u + x) * 4u]; }
    };

    // Makes a renderer with PS2X_LIGHT_DEBUG set to `view` (nullptr: unset), draws the field twice (with a camera for the frame when
    // `camera`), and reads the second field.
    Field renderField(SDL_GPUDevice *device, const char *view, bool camera)
    {
        Field out;
        setEnvVar("PS2X_LIGHT_DEBUG", view);
        std::vector<uint8_t> vram(GSMem::MEMORY_SIZE, 0);
        GSGpuBackend gpu(device, 1, GSGpuBackend::FieldMode::Progressive);
        setEnvVar("PS2X_LIGHT_DEBUG", nullptr);
        if (!gpu.Ready() || !gpu.SetFieldPublishing(true))
            return out;
        gpu.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
        GSPresentationRequest request{};
        request.pmode = 0x8007;
        request.smode2 = 0x3;
        request.dispfb1 = request.dispfb2 = 0ull | (10ull << 9);
        request.display1 = request.display2 = (636ull) | (32ull << 12) | (639ull << 32) | (223ull << 44);
        float m[16];
        simpleCamera(m);
        for (uint32_t frame = 0; frame < 2; ++frame)
        {
            if (camera)
                channel().post(m, 1000u + frame, frame & 1u);
            channel().kickBegin(1000u + frame, frame & 1u);
            drawWorldField(gpu, m);
            channel().kickEnd(1u);
            gpu.MarkPresentation(request);
        }
        PresentationFrame frame = gpu.Present(request);
        if (frame && frame.width == 640 && frame.height == 448)
        {
            out.px = frame.pixels;
            out.ok = true;
        }
        return out;
    }

    bool near3(const uint8_t *p, int r, int g, int b, int tolerance)
    {
        return std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance;
    }

    std::string rgbText(const uint8_t *p)
    {
        return "(" + std::to_string(p[0]) + ", " + std::to_string(p[1]) + ", " + std::to_string(p[2]) + ")";
    }

    // ---- the lights on the GPU renderer (task lt2-L1)
    // A match field in the made-up camera: the clear, a floor of 16 x 16 squares (512 triangle-list triangles, which makes it a match
    // field for the screen effects' tracking) 200 units below the eye, and optionally an opaque overlay box (the clock's Z).
    void drawLitField(GSRasterBackend &b, const float m[16], bool overlay, const std::function<void()> &beforeOverlay = {})
    {
        GSPrimitiveBatch clear;
        clear.vertexCount = 2;
        clear.state.context = sceneContext();
        clear.state.context.test = (1ull << 16) | (1ull << 17);
        clear.state.prim.type = GS_PRIM_SPRITE;
        clear.vertices[0].x = 1728.0f;
        clear.vertices[0].y = 1936.0f;
        clear.vertices[1].x = 1728.0f + 640.0f;
        clear.vertices[1].y = 1936.0f + 224.0f;
        clear.vertices[1].r = 30;
        clear.vertices[1].g = 40;
        clear.vertices[1].b = 60;
        clear.vertices[1].a = 0x80;
        b.Submit(clear);
        for (int row = 0; row < 16; ++row)
            for (int col = 0; col < 16; ++col)
            {
                const double x0 = -300.0 + 37.5 * col, x1 = x0 + 37.5, z0 = 75.0 * row, z1 = z0 + 75.0;
                const double a[3][3] = {{x0, -200.0, z0}, {x1, -200.0, z0}, {x0, -200.0, z1}};
                const double c[3][3] = {{x1, -200.0, z0}, {x1, -200.0, z1}, {x0, -200.0, z1}};
                worldTriangle(b, m, a, 1000.0, 200, 60, 20);
                worldTriangle(b, m, c, 1000.0, 200, 60, 20);
            }
        {
            // An effect: a small green square just above the floor, straight under the test spotlight, drawn without depth writes.
            const double a[3][3] = {{-20.0, -199.0, 40.0}, {20.0, -199.0, 40.0}, {-20.0, -199.0, 80.0}};
            const double c[3][3] = {{20.0, -199.0, 40.0}, {20.0, -199.0, 80.0}, {-20.0, -199.0, 80.0}};
            worldTriangle(b, m, a, 1001.0, 40, 220, 80, false);
            worldTriangle(b, m, c, 1001.0, 40, 220, 80, false);
        }
        if (beforeOverlay)
            beforeOverlay(); // (more of the scene: the wrestler card of the shape test)
        if (overlay)
        {
            GSPrimitiveBatch box;
            box.vertexCount = 2;
            box.state.context = sceneContext();
            box.state.prim.type = GS_PRIM_SPRITE;
            box.vertices[0].x = 1728.0f + 500.0f;
            box.vertices[0].y = 1936.0f + 20.0f;
            box.vertices[1].x = 1728.0f + 560.0f;
            box.vertices[1].y = 1936.0f + 45.0f;
            box.vertices[1].z = 65536.0;
            box.vertices[1].r = 0xFF;
            box.vertices[1].g = 0x10;
            box.vertices[1].b = 0x10;
            box.vertices[1].a = 0x80;
            b.Submit(box);
        }
    }

    // The look of the lit tests: the hall at 0.5 and a white spotlight 600 high over (0, z), pointing straight down, cone 10 to 20.
    Look testLook(float z = 0.0f)
    {
        Look look;
        look.ambient[0] = look.ambient[1] = look.ambient[2] = 0.5f;
        Light spot;
        spot.type = LightType::Spot;
        spot.position[0] = 0.0f; spot.position[1] = 600.0f; spot.position[2] = z;
        spot.target[0] = 0.0f; spot.target[1] = 100.0f; spot.target[2] = z;
        spot.strength = 2.0f;
        spot.innerDegrees = 10.0f;
        spot.outerDegrees = 20.0f;
        spot.radius = 2000.0f;
        look.lights.push_back(spot);
        return look;
    }

    // A renderer with the lights on or off (PS2X_LIGHT, which is the switch when there is no settings store) and `look` as the
    // current look; the field is drawn three times and the last one read.
    // ---- wrestler shape (task lt3-T1)
    // One triangle of a strip, as the game draws a wrestler part: an upright card in the plane z = 0, which faces the made-up camera.
    void cardTriangle(GSRasterBackend &b, const float m[16], const double points[3][3], double z, uint8_t r, uint8_t g, uint8_t bl)
    {
        GSPrimitiveBatch p;
        p.vertexCount = 3;
        p.state.context = sceneContext();
        p.state.prim.type = GS_PRIM_TRISTRIP;
        p.state.prim.iip = false;
        p.state.prim.fst = false;
        for (size_t i = 0; i < 3; ++i)
        {
            double x, y, w;
            project(m, points[i], x, y, w);
            p.vertices[i].x = static_cast<float>(std::floor(x * 16.0) / 16.0);
            p.vertices[i].y = static_cast<float>(std::floor(y * 16.0) / 16.0);
            p.vertices[i].z = z;
            p.vertices[i].q = static_cast<float>(1.0 / w);
            p.vertices[i].r = r;
            p.vertices[i].g = g;
            p.vertices[i].b = bl;
            p.vertices[i].a = 0x80;
        }
        b.Submit(p);
    }

    // The scene of the shape test, drawn after the floor: optionally a thin post in front (a triangle list, as ropes and posts are,
    // drawn before the wrestlers with a higher Z), then a grey card 120 wide and 120 high standing on x -60..60, y -150..-30 at z 0:
    // picture x 290 to 350, y 254 to 374. The post covers picture x 316 to 324 over the card's whole height.
    void drawCard(GSRasterBackend &b, const float m[16], bool post)
    {
        if (post)
        {
            const double a[3][3] = {{-8.0, -160.0, 0.0}, {8.0, -160.0, 0.0}, {-8.0, -20.0, 0.0}};
            const double c[3][3] = {{8.0, -160.0, 0.0}, {8.0, -20.0, 0.0}, {-8.0, -20.0, 0.0}};
            worldTriangle(b, m, a, 3000.0, 20, 20, 120);
            worldTriangle(b, m, c, 3000.0, 20, 20, 120);
        }
        const double a[3][3] = {{-60.0, -150.0, 0.0}, {60.0, -150.0, 0.0}, {-60.0, -30.0, 0.0}};
        const double c[3][3] = {{60.0, -150.0, 0.0}, {-60.0, -30.0, 0.0}, {60.0, -30.0, 0.0}};
        cardTriangle(b, m, a, 2000.0, 120, 120, 120);
        cardTriangle(b, m, c, 2000.0, 120, 120, 120);
    }

    // The look of the shape test: the hall at 0.5 and one white point light to the left of the card and in front of it.
    Look shapeLook(float shape, float rim)
    {
        Look look;
        look.ambient[0] = look.ambient[1] = look.ambient[2] = 0.5f;
        Light side;
        side.type = LightType::Point;
        side.position[0] = -500.0f; side.position[1] = -90.0f; side.position[2] = -400.0f;
        side.strength = 1.0f;
        side.radius = 3000.0f;
        look.lights.push_back(side);
        look.wrestlerShape = shape;
        look.wrestlerRim = rim;
        look.wrestlerOutline = 4.0f;
        return look;
    }

    Field renderLitField(SDL_GPUDevice *device, bool lights, bool camera, bool overlay, const Look &look = testLook(), bool show = false, int card = 0)
    {
        Field out;
        setEnvVar("PS2X_LIGHT", lights ? "1" : nullptr);
        setCurrentLook(look);
        setShowLights(show);
        struct Restore
        {
            ~Restore()
            {
                setEnvVar("PS2X_LIGHT", nullptr);
                setShowLights(false);
            }
        } restore;
        std::vector<uint8_t> vram(GSMem::MEMORY_SIZE, 0);
        GSGpuBackend gpu(device, 1, GSGpuBackend::FieldMode::Progressive);
        if (!gpu.Ready() || !gpu.SetFieldPublishing(true))
            return out;
        gpu.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
        GSPresentationRequest request{};
        request.pmode = 0x8007;
        request.smode2 = 0x3;
        request.dispfb1 = request.dispfb2 = 0ull | (10ull << 9);
        request.display1 = request.display2 = (636ull) | (32ull << 12) | (639ull << 32) | (223ull << 44);
        float m[16];
        simpleCamera(m);
        // Three fields: the renderer reads its switches at a VBlank, so the first field is drawn before the lights are live.
        for (uint32_t frame = 0; frame < 3; ++frame)
        {
            if (camera)
                channel().post(m, 2000u + frame, frame & 1u);
            channel().kickBegin(2000u + frame, frame & 1u);
            if (card != 0)
                drawLitField(gpu, m, overlay, [&]() { drawCard(gpu, m, card == 2); }); // 1: the card, 2: a post in front of it too
            else
                drawLitField(gpu, m, overlay);
            channel().kickEnd(1u);
            gpu.MarkPresentation(request);
        }
        PresentationFrame frame = gpu.Present(request);
        if (frame && frame.width == 640 && frame.height == 448)
        {
            out.px = frame.pixels;
            out.ok = true;
        }
        return out;
    }

    // How many pixels of the box differ (any channel) between two fields.
    uint32_t differing(const Field &a, const Field &b, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
    {
        uint32_t n = 0;
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = x0; x < x1; ++x)
                if (std::memcmp(a.at(x, y), b.at(x, y), 4) != 0)
                    ++n;
        return n;
    }

    double srgbToLinear(double c)
    {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    }

    int linearToSrgb8(double c)
    {
        c = std::fmin(1.0, std::fmax(0.0, c));
        const double e = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
        return static_cast<int>(e * 255.0 + 0.5);
    }
}

void register_gpu_lighting_tests()
{
    MiniTest::Case("MatchLights", [](TestCase &tc)
    {

    tc.Run("undoing the projection: recorded vertices of the apron and crowd come back where the model file has them", [](TestCase &t)
    {
        const double ring = worstPosition(kRingMatrix, kRingSamples);
        const double distant = worstPosition(kFarMatrix, kFarSamples);
        t.IsTrue(ring < 0.10, "match camera: every sample within 0.10 units (worst " + number(ring) + ")");
        t.IsTrue(distant < 0.35, "distant camera: every sample within 0.35 units (worst " + number(distant) + ")");
        // The 1/16 pixel rounding is the whole error, so it grows with the camera's distance.
        t.IsTrue(distant > ring, "the error grows with the camera's distance");
    });

    tc.Run("undoing the projection: a made-up camera, exact without rounding and close with the 1/16 pixel of the GS", [](TestCase &t)
    {
        float m[16];
        simpleCamera(m);
        const Unprojector u = Unprojector::from(m);
        t.IsTrue(u.valid, "the camera can be inverted");
        t.IsTrue(std::fabs(u.eye[0]) < 1e-6 && std::fabs(u.eye[1]) < 1e-6 && std::fabs(u.eye[2] + 1000.0) < 1e-6, "the eye is at (0, 0, -1000)");
        const double points[4][3] = {{0.0, 0.0, 0.0}, {123.5, -40.25, 310.0}, {-400.0, 100.0, 400.0}, {37.0, 255.0, -399.0}};
        double worstExact = 0.0, worstRounded = 0.0;
        for (const auto &p : points)
        {
            double x, y, w;
            project(m, p, x, y, w);
            double back[3];
            t.IsTrue(u.worldAt(static_cast<float>(x), static_cast<float>(y), static_cast<float>(1.0 / w), back), "a point in front of the camera");
            for (int i = 0; i < 3; ++i)
                worstExact = std::fmax(worstExact, std::fabs(back[i] - p[i]));
            const float rx = static_cast<float>(std::floor(x * 16.0) / 16.0), ry = static_cast<float>(std::floor(y * 16.0) / 16.0);
            u.worldAt(rx, ry, static_cast<float>(1.0 / w), back);
            for (int i = 0; i < 3; ++i)
                worstRounded = std::fmax(worstRounded, std::fabs(back[i] - p[i]));
        }
        t.IsTrue(worstExact < 0.02, "float x, y, q: within 0.02 units (worst " + number(worstExact) + ")");
        t.IsTrue(worstRounded < 0.25, "x, y cut to 1/16 pixel: within 0.25 units (worst " + number(worstRounded) + ")");
    });

    tc.Run("undoing the projection: what cannot be done says so", [](TestCase &t)
    {
        float zero[16] = {};
        t.IsFalse(Unprojector::from(zero).valid, "an all-zero matrix");
        float flat[16];
        simpleCamera(flat);
        flat[0] = flat[4] = flat[8] = 0.0f; // X no longer depends on the position
        t.IsFalse(Unprojector::from(flat).valid, "a matrix whose three equations are not independent");
        float m[16];
        simpleCamera(m);
        const Unprojector u = Unprojector::from(m);
        float p[3];
        t.IsFalse(u.worldAt(2048.0f, 2048.0f, 0.0f, p), "a Q of 0");
        t.IsFalse(Unprojector{}.worldAt(2048.0f, 2048.0f, 0.001f, p), "an unprojector that was never set");
    });

    tc.Run("face normals: apron walls, the mat, wrestler cards and shadows from recorded triangles", [](TestCase &t)
    {
        const double ring = worstNormal(kRingMatrix, kRingTriangles);
        const double distant = worstNormal(kFarMatrix, kFarTriangles);
        t.IsTrue(ring < 0.5, "match camera: every normal within 0.5 degrees (worst " + number(ring) + ")");
        t.IsTrue(distant < 2.0, "distant camera: every normal within 2 degrees (worst " + number(distant) + ")");
    });

    tc.Run("face normals: turned to the eye whatever the winding, and none for a triangle with no area", [](TestCase &t)
    {
        const float a[3] = {0.0f, 100.0f, 0.0f}, b[3] = {10.0f, 100.0f, 0.0f}, c[3] = {0.0f, 100.0f, 10.0f};
        const double above[3] = {50.0, 900.0, -300.0}, below[3] = {50.0, -900.0, -300.0};
        float n1[3], n2[3], n3[3];
        t.IsTrue(faceNormal(a, b, c, above, n1) && faceNormal(a, c, b, above, n2), "both windings give a normal");
        t.IsTrue(n1[1] > 0.999f && n2[1] > 0.999f, "seen from above, both face up");
        t.IsTrue(faceNormal(a, b, c, below, n3) && n3[1] < -0.999f, "seen from below, it faces down");
        const float d[3] = {20.0f, 100.0f, 0.0f};
        t.IsFalse(faceNormal(a, b, d, above, n1), "three corners in a line");
        t.IsFalse(faceNormal(a, a, a, above, n1), "three equal corners");
    });

    tc.Run("the camera channel: a camera is active only for the kick of its own frame, on the kicking thread", [](TestCase &t)
    {
        CameraChannel ch;
        Camera got;
        float m[16];
        simpleCamera(m);
        t.IsFalse(ch.activeCamera(got), "nothing before any camera");

        ch.post(m, 100u, 1u);
        t.IsFalse(ch.activeCamera(got), "a posted camera is not active before the kick");
        ch.kickBegin(100u, 1u);
        t.IsTrue(ch.activeCamera(got) && got.gameFrame == 100u && got.buffer == 1u && got.serial == 1u && got.m[15] == 1000.0f,
                 "active during the kick of its frame, with its matrix");
        bool otherThread = true;
        std::thread([&] { Camera c; otherThread = ch.activeCamera(c); }).join();
        t.IsFalse(otherThread, "not active for another thread");
        ch.kickEnd(7u);
        t.IsFalse(ch.activeCamera(got), "gone after the kick");

        ch.kickBegin(101u, 0u);
        t.IsFalse(ch.activeCamera(got), "the next frame has no camera of its own (a menu): none is active");
        ch.kickEnd(3u);

        const ChannelStats s = ch.stats();
        t.IsTrue(s.posted == 1u && s.kicks == 2u && s.kicksWithCamera == 1u && s.kicksWithPackets == 2u && s.stale == 0u && s.replaced == 0u,
                 "the counts: 1 camera, 2 kicks, 1 with a camera");
    });

    tc.Run("the camera channel: the frame counter may be one ahead at the kick; the last of several cameras wins; a wrong one is dropped", [](TestCase &t)
    {
        CameraChannel ch;
        Camera got;
        float m[16];
        simpleCamera(m);

        ch.post(m, 200u, 0u);
        ch.kickBegin(201u, 0u); // the game counted the frame before it kicked
        t.IsTrue(ch.activeCamera(got) && got.gameFrame == 200u, "counter one ahead: still the camera of this frame");
        ch.kickEnd(1u);

        m[15] = 1111.0f;
        ch.post(m, 202u, 1u);
        m[15] = 2222.0f;
        ch.post(m, 202u, 1u);
        ch.kickBegin(202u, 1u);
        t.IsTrue(ch.activeCamera(got) && got.m[15] == 2222.0f && got.serial == 3u, "two cameras in one frame: the later one");
        ch.kickEnd(1u);

        ch.post(m, 203u, 1u);
        ch.kickBegin(204u, 0u); // the other frame buffer: not the frame this camera drew
        t.IsFalse(ch.activeCamera(got), "a camera for the other frame buffer is not used");
        ch.kickEnd(1u);
        ch.kickBegin(205u, 1u);
        t.IsFalse(ch.activeCamera(got), "and it does not wait for a later kick either");
        ch.kickEnd(0u);

        ch.post(m, 300u, 0u);
        ch.kickBegin(304u, 0u); // right buffer, four frames late
        t.IsFalse(ch.activeCamera(got), "a camera from frames ago is not used");
        ch.kickEnd(0u);

        const ChannelStats s = ch.stats();
        t.IsTrue(s.posted == 5u && s.replaced == 1u && s.stale == 2u && s.kicksWithCamera == 2u && s.kicks == 5u && s.kicksWithPackets == 3u,
                 "the counts: 5 cameras, 1 replaced, 2 dropped, 2 used");
    });

    tc.Run("the camera channel: the trace has one row per camera and per kick", [](TestCase &t)
    {
        namespace fs = std::filesystem;
        const fs::path path = fs::temp_directory_path() / "ps2x_light_trace_test.csv";
        {
            CameraChannel ch;
            float m[16];
            simpleCamera(m);
            t.IsTrue(ch.traceTo(path.string()), "the trace file opens");
            ch.post(m, 10u, 0u);
            ch.kickBegin(10u, 0u);
            ch.kickEnd(42u);
            ch.kickBegin(11u, 1u);
            ch.kickEnd(5u);
        }
        std::ifstream in(path);
        std::string line;
        int cameras = 0, kicksWith = 0, kicksWithout = 0, header = 0;
        while (std::getline(in, line))
        {
            if (line.rfind("event,", 0) == 0) ++header;
            else if (line.rfind("camera,", 0) == 0) ++cameras;
            else if (line.rfind("kick,", 0) == 0 && line.find(",10,0,1,1,42,") != std::string::npos) ++kicksWith;
            else if (line.rfind("kick,", 0) == 0 && line.find(",11,1,0,0,5,") != std::string::npos) ++kicksWithout;
        }
        in.close();
        std::error_code ec;
        fs::remove(path, ec);
        t.IsTrue(header == 1 && cameras == 1 && kicksWith == 1 && kicksWithout == 1, "a header, one camera row, a kick with it and a kick without");
    });

    tc.Run("lights: ambient alone is the picture of the game; a spotlight lights its cone and nothing else", [](TestCase &t)
    {
        const float mat[3] = {0.0f, 100.0f, 0.0f}, up[3] = {0.0f, 1.0f, 0.0f};
        Look look;
        float out[3];
        lookAt(look, mat, up, Facing::Lambert, out);
        t.IsTrue(out[0] == 1.0f && out[1] == 1.0f && out[2] == 1.0f, "ambient 1 and no lights: exactly 1");

        Light spot;
        spot.type = LightType::Spot;
        spot.position[1] = 900.0f;
        spot.target[1] = 100.0f;
        spot.colour[0] = 1.0f; spot.colour[1] = 0.5f; spot.colour[2] = 0.25f;
        spot.strength = 2.0f;
        spot.radius = 1600.0f;
        spot.innerDegrees = 18.0f;
        spot.outerDegrees = 30.0f;
        lightAt(spot, mat, up, Facing::Lambert, out);
        const float fall = (1.0f - 0.25f) * (1.0f - 0.25f); // 800 of 1600 units
        t.IsTrue(std::fabs(out[0] - 2.0f * fall) < 1e-4f && std::fabs(out[1] - 1.0f * fall) < 1e-4f && std::fabs(out[2] - 0.5f * fall) < 1e-4f,
                 "under the spot: colour x strength x falloff");

        const float inside[3] = {200.0f, 100.0f, 0.0f};  // 14 degrees off the axis
        const float edge[3] = {360.0f, 100.0f, 0.0f};    // 24 degrees: between inner and outer
        const float outside[3] = {600.0f, 100.0f, 0.0f}; // 37 degrees
        float a[3], b[3], c[3];
        lightAt(spot, inside, up, Facing::PositionOnly, a);
        lightAt(spot, edge, up, Facing::PositionOnly, b);
        lightAt(spot, outside, up, Facing::PositionOnly, c);
        t.IsTrue(a[0] > b[0] && b[0] > 0.0f && c[0] == 0.0f, "full inside the inner cone, partial between, nothing outside");

        look.ambient[0] = look.ambient[1] = look.ambient[2] = 0.4f;
        look.lights.push_back(spot);
        lookAt(look, mat, up, Facing::Lambert, out);
        t.IsTrue(std::fabs(out[0] - (0.4f + 2.0f * fall)) < 1e-4f, "a look is ambient plus its lights");
    });

    tc.Run("lights: falloff ends at the radius; facing rules for surfaces, cloth and cards", [](TestCase &t)
    {
        Light point;
        point.position[1] = 500.0f;
        point.radius = 400.0f;
        const float up[3] = {0.0f, 1.0f, 0.0f}, down[3] = {0.0f, -1.0f, 0.0f}, side[3] = {1.0f, 0.0f, 0.0f};
        const float at[3] = {0.0f, 300.0f, 0.0f}, beyond[3] = {0.0f, 100.0f, 0.0f}, nearRadius[3] = {0.0f, 100.01f, 0.0f};
        float out[3];
        lightAt(point, beyond, up, Facing::Lambert, out);
        t.IsTrue(out[0] == 0.0f, "nothing at or beyond the radius");
        lightAt(point, nearRadius, up, Facing::Lambert, out);
        t.IsTrue(out[0] >= 0.0f && out[0] < 1e-4f, "and it reaches zero smoothly");

        const float fall = (1.0f - 0.25f) * (1.0f - 0.25f);
        lightAt(point, at, up, Facing::Lambert, out);
        t.IsTrue(std::fabs(out[0] - fall) < 1e-5f, "a surface that faces the light: all of it");
        lightAt(point, at, down, Facing::Lambert, out);
        t.IsTrue(out[0] == 0.0f, "a surface that faces away: none");
        lightAt(point, at, side, Facing::Lambert, out);
        t.IsTrue(out[0] == 0.0f, "an upright card under a light from above gets nothing with the surface rule");
        lightAt(point, at, side, Facing::PositionOnly, out);
        t.IsTrue(std::fabs(out[0] - fall) < 1e-5f, "the same card by position only: all of it");
        lightAt(point, at, side, Facing::Half, out);
        t.IsTrue(std::fabs(out[0] - fall * 0.25f) < 1e-5f, "the soft rule: a quarter for a surface at a right angle");
        lightAt(point, at, down, Facing::Half, out);
        t.IsTrue(out[0] == 0.0f, "the soft rule: none for a surface that faces straight away");
    });

    tc.Run("the camera side file of a recording: written at each change, read back, and played around the same Submit numbers", [](TestCase &t)
    {
        namespace fs = std::filesystem;
        const fs::path path = fs::temp_directory_path() / "ps2x_light_side_test.camera";
        float m[16];
        simpleCamera(m);
        {
            // The game side: three frames of 4 Submits each; the first and third have a camera, the second (a menu) has none.
            CameraChannel live;
            CameraFileWriter writer;
            t.IsTrue(writer.open(path.string()), "the side file opens");
            uint64_t submit = 0;
            for (uint32_t frame = 0; frame < 3; ++frame)
            {
                if (frame != 1u)
                {
                    m[15] = 1000.0f + static_cast<float>(frame);
                    live.post(m, 50u + frame, frame & 1u);
                }
                live.kickBegin(50u + frame, frame & 1u);
                for (int i = 0; i < 4; ++i)
                    writer.note(submit++, live);
                live.kickEnd(4u);
            }
            writer.note(submit, live); // a Submit outside any kick
        }
        std::vector<CameraFileRow> rows;
        std::string error;
        t.IsTrue(loadCameraFile(path.string(), rows, &error), "the side file reads back" + (error.empty() ? std::string() : ": " + error));
        t.IsTrue(rows.size() == 4u, "four rows: on, off, on, off");
        if (rows.size() == 4u)
        {
            t.IsTrue(rows[0].on && rows[0].submit == 0u && rows[0].gameFrame == 50u && rows[0].buffer == 0u && rows[0].m[15] == 1000.0f && rows[0].m[12] == 2048000.0f,
                     "row 1: on at Submit 0 with the matrix, bit for bit");
            t.IsTrue(!rows[1].on && rows[1].submit == 4u, "row 2: off at Submit 4");
            t.IsTrue(rows[2].on && rows[2].submit == 8u && rows[2].gameFrame == 52u && rows[2].m[15] == 1002.0f, "row 3: on at Submit 8");
            t.IsTrue(!rows[3].on && rows[3].submit == 12u, "row 4: off at Submit 12");
        }

        // The replay side: the same cameras are active for the same Submit numbers.
        CameraChannel replay;
        CameraFilePlayer player(rows);
        bool same = true;
        for (uint64_t submit = 0; submit < 14u; ++submit)
        {
            player.beforeSubmit(submit, replay);
            Camera got;
            const bool active = replay.activeCamera(got);
            const bool expected = submit < 4u || (submit >= 8u && submit < 12u);
            same = same && active == expected && (!active || got.m[15] == (submit < 4u ? 1000.0f : 1002.0f));
        }
        player.finish(replay);
        Camera got;
        t.IsTrue(same, "Submits 0-3 and 8-11 have their cameras, the others none");
        t.IsTrue(player.camerasPlayed() == 2u && !replay.activeCamera(got), "two cameras played, none left active");

        // What is not a side file is refused.
        {
            std::ofstream bad(path);
            bad << "PS2XCAM 1\non 5 1 0 1 2 3\n";
        }
        t.IsFalse(loadCameraFile(path.string(), rows, &error) || !rows.empty(), "a row with too few numbers is refused");
        {
            std::ofstream bad(path);
            bad << "something else\n";
        }
        t.IsFalse(loadCameraFile(path.string(), rows, &error), "a file without the first line is refused");
        std::error_code ec;
        fs::remove(path, ec);
        t.IsFalse(loadCameraFile(path.string(), rows, &error), "a missing file is reported");
    });

    tc.Run("object rules: shadows are left alone, cards and ropes are lit by position, surfaces by their normal", [](TestCase &t)
    {
        PrimitiveKind shadow;
        shadow.strip = true;
        shadow.textured = true;
        shadow.texW = shadow.texH = 256;
        shadow.texPsm = 0x13;
        shadow.flatBlack = true;
        t.IsTrue(!ruleFor(shadow).lit && std::string(ruleFor(shadow).name) == "shadow", "a flat black strip is a shadow: not lit");
        PrimitiveKind part = shadow;
        part.flatBlack = false;
        part.texW = part.texH = 64;
        t.IsTrue(ruleFor(part).lit && ruleFor(part).facing == Facing::PositionOnly && std::string(ruleFor(part).name) == "card", "any other strip is a card: by position only");
        PrimitiveKind rope;
        rope.textured = true;
        rope.texW = rope.texH = 32;
        rope.texPsm = 0;
        t.IsTrue(ruleFor(rope).lit && ruleFor(rope).facing == Facing::PositionOnly && std::string(ruleFor(rope).name) == "rope", "a triangle list with a 32 x 32 32-bit texture is a rope: by position only");
        PrimitiveKind mat;
        mat.textured = true;
        mat.texW = mat.texH = 256;
        mat.texPsm = 0x13;
        t.IsTrue(ruleFor(mat).lit && ruleFor(mat).facing == Facing::Lambert && std::string(ruleFor(mat).name) == "surface", "other triangle lists are surfaces: by the face normal");
        PrimitiveKind blackModel = mat;
        blackModel.flatBlack = true;
        t.IsTrue(ruleFor(blackModel).lit, "a black triangle of a model is still a surface (only strips can be shadows)");
        PrimitiveKind small = rope;
        small.texPsm = 0x13;
        t.IsTrue(ruleFor(small).facing == Facing::Lambert, "a 32 x 32 texture with a palette is not a rope");
    });

    tc.Run("the look from the environment: defaults, values, clamps; and the shader's uniform block agrees with lightAt", [](TestCase &t)
    {
        for (const char *name : {"PS2X_LIGHT", "PS2X_LIGHT_AMBIENT", "PS2X_LIGHT_SPOT_INTENSITY", "PS2X_LIGHT_SPOT_COLOUR", "PS2X_LIGHT_SPOT_CONE",
                                 "PS2X_LIGHT_SPOT_HEIGHT", "PS2X_LIGHT_SPOT_RADIUS"})
            setEnvVar(name, nullptr);
        Look look;
        t.IsFalse(lookFromEnvironment(look), "off unless PS2X_LIGHT=1");
        t.IsTrue(look.lights.size() == 1u && look.ambient[0] == 0.6f && look.lights[0].type == LightType::Spot && look.lights[0].position[1] == 900.0f &&
                     look.lights[0].target[1] == 100.0f && look.lights[0].strength == 1.5f && look.lights[0].innerDegrees == 22.0f && look.lights[0].outerDegrees == 34.0f,
                 "the defaults: ambient 0.6, one spotlight 900 high over the ring centre aimed at the mat, 1.5, cone 22 to 34");
        setEnvVar("PS2X_LIGHT", "1");
        setEnvVar("PS2X_LIGHT_AMBIENT", "0.2,0.3,0.4");
        setEnvVar("PS2X_LIGHT_SPOT_INTENSITY", "3");
        setEnvVar("PS2X_LIGHT_SPOT_COLOUR", "1,0.5,0.25");
        setEnvVar("PS2X_LIGHT_SPOT_CONE", "40,30"); // the outer angle below the inner one
        setEnvVar("PS2X_LIGHT_SPOT_HEIGHT", "50");  // below the mat
        setEnvVar("PS2X_LIGHT_SPOT_RADIUS", "1500");
        t.IsTrue(lookFromEnvironment(look), "PS2X_LIGHT=1 turns them on");
        t.IsTrue(look.ambient[0] == 0.2f && look.ambient[1] == 0.3f && look.ambient[2] == 0.4f, "ambient as three numbers");
        const Light &spot = look.lights[0];
        t.IsTrue(spot.strength == 3.0f && spot.colour[1] == 0.5f && spot.radius == 1500.0f, "intensity, colour and radius");
        t.IsTrue(spot.innerDegrees == 40.0f && spot.outerDegrees == 40.0f, "an outer angle below the inner one is raised to it");
        t.IsTrue(spot.position[1] == 110.0f, "a height below the mat is raised above it");
        setEnvVar("PS2X_LIGHT", "yes");
        setEnvVar("PS2X_LIGHT_AMBIENT", "0.8");
        setEnvVar("PS2X_LIGHT_SPOT_CONE", "18,30");
        setEnvVar("PS2X_LIGHT_SPOT_HEIGHT", "900");
        t.IsFalse(lookFromEnvironment(look), "anything but 1 is off");
        t.IsTrue(look.ambient[0] == 0.8f && look.ambient[2] == 0.8f, "ambient as one number");
        for (const char *name : {"PS2X_LIGHT", "PS2X_LIGHT_AMBIENT", "PS2X_LIGHT_SPOT_INTENSITY", "PS2X_LIGHT_SPOT_COLOUR", "PS2X_LIGHT_SPOT_CONE",
                                 "PS2X_LIGHT_SPOT_HEIGHT", "PS2X_LIGHT_SPOT_RADIUS"})
            setEnvVar(name, nullptr);

        // The uniform block: the arithmetic of shaders/gs_light.hlsli, done here on the block's numbers, gives what lookAt gives.
        const LightUniforms u = lightUniforms(look, Facing::Lambert);
        t.IsTrue(u.position[0][1] == 900.0f && u.position[0][3] == 1500.0f && u.axis[0][1] == -1.0f && u.axis[0][3] == 3.0f && u.rule[0] == 0.0f && u.ambient[3] == 1.0f && u.colour[0][3] == 1.0f &&
                     std::fabs(u.cone[0][0] - std::cos(30.0f * 3.14159265f / 180.0f)) < 1e-6f && std::fabs(u.cone[0][1] - std::cos(18.0f * 3.14159265f / 180.0f)) < 1e-6f,
                 "position and radius, axis and strength, the cosines of the cone, the rule");
        t.IsTrue(lightUniforms(look, Facing::Half).rule[0] == 1.0f && lightUniforms(look, Facing::PositionOnly).rule[0] == 2.0f, "the three rules as numbers");
        const float up[3] = {0.0f, 1.0f, 0.0f};
        double worst = 0.0;
        for (const float x : {0.0f, 150.0f, 280.0f, 330.0f, 420.0f, 600.0f})
        {
            const float at[3] = {x, 100.0f, 40.0f};
            float expected[3];
            lookAt(look, at, up, Facing::Lambert, expected);
            const float toLight[3] = {u.position[0][0] - at[0], u.position[0][1] - at[1], u.position[0][2] - at[2]};
            const float distance = std::sqrt(toLight[0] * toLight[0] + toLight[1] * toLight[1] + toLight[2] * toLight[2]);
            float got[3] = {u.ambient[0], u.ambient[1], u.ambient[2]};
            if (distance < u.position[0][3])
            {
                const float ratio = distance / u.position[0][3];
                const float fall = (1.0f - ratio * ratio) * (1.0f - ratio * ratio);
                const float l[3] = {toLight[0] / distance, toLight[1] / distance, toLight[2] / distance};
                const float cosAngle = -(u.axis[0][0] * l[0] + u.axis[0][1] * l[1] + u.axis[0][2] * l[2]);
                const float e = std::fmin(1.0f, std::fmax(0.0f, (cosAngle - u.cone[0][0]) / (u.cone[0][1] - u.cone[0][0])));
                const float cone = e * e * (3.0f - 2.0f * e);
                const float face = std::fmax(up[0] * l[0] + up[1] * l[1] + up[2] * l[2], 0.0f);
                for (int i = 0; i < 3; ++i)
                    got[i] += u.colour[0][i] * (u.axis[0][3] * fall * cone * face);
            }
            for (int i = 0; i < 3; ++i)
                worst = std::fmax(worst, std::fabs(static_cast<double>(got[i]) - expected[i]));
        }
        t.IsTrue(worst < 1e-4, "six points across the cone: the block's arithmetic and lookAt agree (worst " + number(worst) + ")");
    });

    tc.Run("the look file: what is written reads back the same; comments, commas and missing fields are fine; numbers are clamped", [](TestCase &t)
    {
        for (const std::string &name : presetNames())
        {
            const Look look = presetLook(name);
            Look back;
            std::string error;
            const std::string text = formatLook(look);
            t.IsTrue(parseLook(text, back, &error), name + " reads back" + (error.empty() ? std::string() : ": " + error));
            t.IsTrue(look.lights.size() <= static_cast<size_t>(kMaxLights) && formatLook(back) == text, name + ": the same text again, 8 lights at most");
        }
        t.IsTrue(formatLook(presetLook("centre spot")) == formatLook(defaultLook()) && defaultLook().lights.size() == 1u, "\"centre spot\" is the built-in look, one light");
        t.IsTrue(presetLook("four corner spots").lights.size() == 4u && presetLook("house lights").lights.empty() && presetLook("house lights").ambient[0] == 1.0f,
                 "four corner spots has four; house lights is the game's own picture");

        Look look;
        std::string error;
        const char *text = "# a look written by hand\n"
                           "look 1\n"
                           "ambient 0.4   # one number\n"
                           "\n"
                           "light spot\n"
                           "  position 100, 700, -50\n"
                           "  aim 0 100 0\n"
                           "  colour 1 0.5 0.25\n"
                           "  intensity 99\n"
                           "  cone 50 20\n"
                           "light point off\n"
                           "  range 0\n";
        t.IsTrue(parseLook(text, look, &error), "a hand-written look parses" + (error.empty() ? std::string() : ": " + error));
        t.IsTrue(look.ambient[0] == 0.4f && look.ambient[2] == 0.4f && look.lights.size() == 2u, "ambient as one number; two lights");
        if (look.lights.size() == 2u)
        {
            const Light &a = look.lights[0], &b = look.lights[1];
            t.IsTrue(a.type == LightType::Spot && a.on && a.position[0] == 100.0f && a.position[1] == 700.0f && a.position[2] == -50.0f && a.colour[1] == 0.5f,
                     "the spotlight: on by default, position with commas, colour");
            t.IsTrue(a.strength == 64.0f && a.innerDegrees == 50.0f && a.outerDegrees == 50.0f, "intensity clamped to 64; an outer angle below the inner one raised to it");
            t.IsTrue(b.type == LightType::Point && !b.on && b.radius == 1.0f && b.position[1] == 900.0f, "the point light: off, range clamped to 1, the other fields at their defaults");
        }
    });

    tc.Run("the look file: bad files are refused with a reason and leave the look alone", [](TestCase &t)
    {
        const char *bad[] = {"",
                             "ambient 0.5\n",
                             "look 2\nambient 0.5\n",
                             "look 1\nambient\n",
                             "look 1\nambient 0.5 0.5\n",
                             "look 1\nambient nan\n",
                             "look 1\nposition 0 0 0\n",
                             "look 1\nlight laser\n",
                             "look 1\nlight spot maybe\n",
                             "look 1\nlight spot\n  position 1 2\n",
                             "look 1\nlight spot\n  cone 20\n",
                             "look 1\nlight spot\n  intensity 1x\n",
                             "look 1\nlight spot\n  wattage 100\n",
                             "look 1\nlight spot\nlight spot\nlight spot\nlight spot\nlight spot\nlight spot\nlight spot\nlight spot\nlight spot\n",
                             "\x01\x02\xff\xfe binary rubbish \x00",
                             "look 1\nlight spot\n  colour 1 1 inf\n"};
        const Look before = presetLook("four corner spots");
        int refused = 0, untouched = 0, reasons = 0;
        for (const char *text : bad)
        {
            Look look = before;
            std::string error;
            refused += parseLook(text, look, &error) ? 0 : 1;
            untouched += formatLook(look) == formatLook(before) ? 1 : 0;
            reasons += error.rfind("line ", 0) == 0 ? 1 : 0;
        }
        const int count = static_cast<int>(sizeof(bad) / sizeof(bad[0]));
        t.IsTrue(refused == count, "all " + std::to_string(count) + " bad texts are refused (" + std::to_string(refused) + ")");
        t.IsTrue(untouched == count && reasons == count, "each leaves the look as it was and names a line");
        Look eight;
        std::string text = "look 1\n";
        for (int i = 0; i < 8; ++i)
            text += "light point\n";
        t.IsTrue(parseLook(text, eight) && eight.lights.size() == 8u, "exactly 8 lights is fine");
    });

    tc.Run("the current look: an arena's saved look comes back for that arena; another arena or a bad file gets the built-in one", [](TestCase &t)
    {
        namespace fs = std::filesystem;
        const fs::path folder = fs::temp_directory_path() / "ps2x_light_looks_test";
        std::error_code ec;
        fs::remove_all(folder, ec);
        setLookFolder(folder.string());
        const std::string builtIn = formatLook(defaultLook());

        setArena(3);
        t.IsTrue(currentArena() == 3 && formatLook(currentLook()) == builtIn && lookSource() == "built-in", "arena 3 with no file: the built-in look");
        const uint64_t before = lookGeneration();
        Look edited = presetLook("two colours");
        edited.lights[1].position[0] = -777.0f;
        setCurrentLook(edited);
        t.IsTrue(lookGeneration() != before && lookSource() == "edited", "an edit changes the generation");
        std::string message;
        t.IsTrue(saveUserLook(&message) && fs::exists(folder / "arena03.look"), "Save writes lights/arena03.look: " + message);
        t.IsTrue(userLookPath(3) == (folder / "arena03.look").string(), "the path the panel shows is that file");

        setArena(4);
        t.IsTrue(formatLook(currentLook()) == builtIn, "arena 4 has no file: the built-in look");
        setArena(3);
        t.IsTrue(formatLook(currentLook()) == formatLook(edited) && currentLook().lights[1].position[0] == -777.0f, "back in arena 3: the saved look, number for number");

        resetLook();
        t.IsTrue(formatLook(currentLook()) == builtIn && fs::exists(folder / "arena03.look"), "Reset gives the built-in look and leaves the file");
        t.IsTrue(reloadLook(&message) && formatLook(currentLook()) == formatLook(edited), "Reload takes the file again");

        {
            std::ofstream bad(folder / "arena05.look");
            bad << "look 1\nlight spot\n  wattage 100\n";
        }
        setArena(5);
        t.IsTrue(formatLook(currentLook()) == builtIn && lookSource().find("bad") != std::string::npos, "a bad file: the built-in look, and the source says so");

        Look many = defaultLook();
        for (int i = 0; i < 12; ++i)
            many.lights.push_back(many.lights[0]);
        setCurrentLook(many);
        t.IsTrue(currentLook().lights.size() == 8u, "a look with more than 8 lights is cut to 8");
        setShowLights(true);
        t.IsTrue(showLights(), "show lights is kept");
        setShowLights(false);
        resetLook();
        fs::remove_all(folder, ec);
    });

    tc.Run("the uniform block: lights that are off or have no strength are left out; point lights; the markers' switch; lookAt agrees for many lights", [](TestCase &t)
    {
        Look look = presetLook("two colours");
        Light point;
        point.type = LightType::Point;
        point.position[0] = 300.0f; point.position[1] = 300.0f; point.position[2] = -200.0f;
        point.colour[0] = 0.2f; point.colour[1] = 1.0f; point.colour[2] = 0.3f;
        point.strength = 1.5f;
        point.radius = 900.0f;
        look.lights.push_back(point);
        look.lights[1].on = false;      // the blue one
        Light none = point;
        none.strength = 0.0f;
        look.lights.push_back(none);
        const LightUniforms u = lightUniforms(look, Facing::PositionOnly, true);
        t.IsTrue(u.ambient[3] == 3.0f && u.rule[0] == 2.0f && u.rule[1] == 1.0f, "three of five lights are sent; the rule and the markers' switch");
        t.IsTrue(u.colour[0][3] == 1.0f && u.colour[1][3] == 1.0f && u.colour[2][3] == 0.0f && u.cone[0][3] == 0.0f && u.cone[1][3] == 2.0f && u.cone[2][3] == 3.0f,
                 "two spotlights and the point light, each with its number in the look");
        t.IsTrue(lightUniforms(look, Facing::Lambert).rule[1] == 0.0f, "no markers unless asked");

        // The shader's loop (gs_light.hlsli), done here on the block, against lookAt: surfaces facing up, at points across the ring.
        const LightUniforms l = lightUniforms(look, Facing::Lambert);
        const float up[3] = {0.0f, 1.0f, 0.0f};
        double worst = 0.0;
        for (const float x : {-380.0f, -120.0f, 0.0f, 210.0f, 390.0f})
            for (const float z : {-300.0f, 0.0f, 250.0f})
            {
                const float at[3] = {x, 100.0f, z};
                float expected[3];
                lookAt(look, at, up, Facing::Lambert, expected);
                float got[3] = {l.ambient[0], l.ambient[1], l.ambient[2]};
                for (int k = 0; k < static_cast<int>(l.ambient[3]); ++k)
                {
                    const float toLight[3] = {l.position[k][0] - at[0], l.position[k][1] - at[1], l.position[k][2] - at[2]};
                    const float distance = std::sqrt(toLight[0] * toLight[0] + toLight[1] * toLight[1] + toLight[2] * toLight[2]);
                    if (distance >= l.position[k][3])
                        continue;
                    const float ratio = distance / l.position[k][3];
                    const float fall = (1.0f - ratio * ratio) * (1.0f - ratio * ratio);
                    const float dir[3] = {toLight[0] / distance, toLight[1] / distance, toLight[2] / distance};
                    float cone = 1.0f;
                    if (l.colour[k][3] > 0.5f)
                    {
                        const float cosAngle = -(l.axis[k][0] * dir[0] + l.axis[k][1] * dir[1] + l.axis[k][2] * dir[2]);
                        const float e = std::fmin(1.0f, std::fmax(0.0f, (cosAngle - l.cone[k][0]) / (l.cone[k][1] - l.cone[k][0])));
                        cone = e * e * (3.0f - 2.0f * e);
                    }
                    const float face = std::fmax(up[0] * dir[0] + up[1] * dir[1] + up[2] * dir[2], 0.0f);
                    for (int i = 0; i < 3; ++i)
                        got[i] += l.colour[k][i] * (l.axis[k][3] * fall * cone * face);
                }
                for (int i = 0; i < 3; ++i)
                    worst = std::fmax(worst, std::fabs(static_cast<double>(got[i]) - expected[i]));
            }
        t.IsTrue(worst < 1e-4, "15 points on the mat, 3 lights: the block's arithmetic and lookAt agree (worst " + number(worst) + ")");
        float out[3];
        Light off = point;
        off.on = false;
        const float closeBy[3] = {300.0f, 100.0f, -200.0f};
        lightAt(off, closeBy, up, Facing::Lambert, out);
        t.IsTrue(out[0] == 0.0f && out[1] == 0.0f, "a light that is off adds nothing");
        float a[3], b[3];
        markerColour(0, a);
        markerColour(7, b);
        t.IsTrue(a[0] == 1.0f && a[1] == 0.25f && b[0] == 0.75f, "the markers' colours: light 1 red, light 8 grey");
    });

    tc.Run("materials: a texture's material wins over the object rule; each kind gives its mode, strength and glow; effects are shown as drawn", [](TestCase &t)
    {
        PrimitiveKind surface;
        surface.textured = true;
        surface.texW = surface.texH = 128;
        surface.texPsm = 0x13;
        PrimitiveKind card = surface;
        card.strip = true;
        PrimitiveKind shadow = card;
        shadow.flatBlack = true;
        PrimitiveKind effect = card;
        effect.depthWriteOff = true;

        LightChoice c = chooseLight(surface, nullptr);
        t.IsTrue(c.lit && c.mode == 0 && c.strength == 1.0f && c.glow == 0.0f && std::string(c.name) == "surface", "no material, a surface: lit by its normal");
        c = chooseLight(card, nullptr);
        t.IsTrue(c.lit && c.mode == 2 && std::string(c.name) == "card", "no material, a card: by position only");
        c = chooseLight(shadow, nullptr);
        t.IsTrue(!c.lit && std::string(c.name) == "shadow", "no material, a shadow: not lit");
        c = chooseLight(effect, nullptr);
        t.IsTrue(c.lit && c.mode == 3 && c.glow == 0.0f && std::string(c.name) == "effect", "no material, drawn without depth writes: an effect, shown as the game drew it");
        PrimitiveKind blackEffect = effect;
        blackEffect.flatBlack = true;
        t.IsTrue(std::string(chooseLight(blackEffect, nullptr).name) == "effect", "the effect rule comes before the shadow rule");

        Material m;
        m.kind = MaterialKind::Keep;
        t.IsTrue(!chooseLight(surface, &m).lit, "keep: not lit");
        m.kind = MaterialKind::Unlit;
        c = chooseLight(card, &m);
        t.IsTrue(c.lit && c.mode == 3 && c.glow == 0.0f, "unlit: a fixed light of 1");
        m.kind = MaterialKind::Glow;
        m.glow = 1.5f;
        c = chooseLight(surface, &m);
        t.IsTrue(c.lit && c.mode == 3 && c.glow == 1.5f, "glow 1.5: a fixed light of 2.5");
        m.glow = 99.0f;
        t.IsTrue(chooseLight(surface, &m).glow == 16.0f, "glow is clamped to 16");
        m = Material{};
        m.kind = MaterialKind::PositionOnly;
        m.strength = 0.4f;
        c = chooseLight(surface, &m);
        t.IsTrue(c.lit && c.mode == 2 && c.strength == 0.4f, "position with strength 0.4, on a surface");
        m.kind = MaterialKind::FaceNormal;
        m.strength = 1.0f;
        c = chooseLight(card, &m);
        t.IsTrue(c.lit && c.mode == 0, "normal, on a card");
        m.kind = MaterialKind::Soft;
        t.IsTrue(chooseLight(shadow, &m).lit && chooseLight(shadow, &m).mode == 1, "soft, even on what the rules call a shadow");

        // The table: a look's entry wins over a built-in one for the same key; key 0 (no key) never has a material.
        Look look;
        t.IsNull(materialFor(look, 0u), "no key, no material");
        t.IsNull(materialFor(look, 0x1234567890abcdefull), "a key nobody lists has none");
        MaterialEntry mine;
        mine.key = 0x1234567890abcdefull;
        mine.material.kind = MaterialKind::Glow;
        mine.material.glow = 2.0f;
        look.materials.push_back(mine);
        const Material *found = materialFor(look, 0x1234567890abcdefull);
        t.IsTrue(found && found->kind == MaterialKind::Glow && found->glow == 2.0f, "the look's entry is found");
        for (const MaterialEntry &builtIn : builtInMaterials())
        {
            t.IsTrue(builtIn.key != 0u && !builtIn.note.empty(), "every built-in material has a key and says what it is");
            Look other;
            const Material *b = materialFor(other, builtIn.key);
            t.IsTrue(b && b->kind == builtIn.material.kind, "a built-in material is found without any look entry");
            MaterialEntry over;
            over.key = builtIn.key;
            over.material.kind = MaterialKind::Unlit;
            other.materials.push_back(over);
            t.IsTrue(materialFor(other, builtIn.key)->kind == MaterialKind::Unlit, "and a look's entry for the same texture wins");
        }
        const LightUniforms u = lightUniforms(defaultLook(), Facing::Lambert);
        t.IsTrue(u.rule[2] == 1.0f && u.rule[3] == 0.0f, "the uniform block starts at full strength and no glow");
    });

    tc.Run("the look file: material lines read, write and are refused when wrong", [](TestCase &t)
    {
        Look look;
        std::string error;
        const char *text = "look 1\n"
                           "ambient 0.5\n"
                           "material 00000000000000aa unlit        # a sign\n"
                           "material 00000000000000bb normal strength 0.4\n"
                           "material 00000000000000cc glow 1.5   # a screen\n"
                           "material 00000000000000dd keep\n"
                           "material 00000000000000ee position\n"
                           "material 00000000000000ff soft strength 9\n"
                           "material 00000000000000aa glow 0.25\n"
                           "light spot\n";
        t.IsTrue(parseLook(text, look, &error), "a look with materials parses" + (error.empty() ? std::string() : ": " + error));
        t.IsTrue(look.materials.size() == 6u && look.lights.size() == 1u, "six textures (a later line for the same key replaces the earlier one), one light");
        if (look.materials.size() == 6u)
        {
            t.IsTrue(look.materials[0].key == 0xaaull && look.materials[0].material.kind == MaterialKind::Glow && look.materials[0].material.glow == 0.25f,
                     "the second line for ...aa won: glow 0.25");
            t.IsTrue(look.materials[1].material.kind == MaterialKind::FaceNormal && look.materials[1].material.strength == 0.4f, "normal strength 0.4");
            t.IsTrue(look.materials[2].material.kind == MaterialKind::Glow && look.materials[2].material.glow == 1.5f && look.materials[2].note == "a screen",
                     "glow 1.5, and the comment is kept as the note");
            t.IsTrue(look.materials[3].material.kind == MaterialKind::Keep && look.materials[4].material.kind == MaterialKind::PositionOnly, "keep and position");
            t.IsTrue(look.materials[5].material.kind == MaterialKind::Soft && look.materials[5].material.strength == 4.0f, "soft, strength clamped to 4");
        }
        Look back;
        const std::string written = formatLook(look);
        t.IsTrue(parseLook(written, back, &error) && formatLook(back) == written, "what is written reads back as the same text");
        t.IsTrue(written.find("material 00000000000000cc glow 1.5   # a screen") != std::string::npos, "the line as written: key, kind, number, note");

        const char *bad[] = {"look 1\nmaterial\n",
                             "look 1\nmaterial 00aa unlit\n",
                             "look 1\nmaterial 00000000000000zz unlit\n",
                             "look 1\nmaterial 0000000000000000 unlit\n",
                             "look 1\nmaterial 00000000000000aa shiny\n",
                             "look 1\nmaterial 00000000000000aa glow\n",
                             "look 1\nmaterial 00000000000000aa glow much\n",
                             "look 1\nmaterial 00000000000000aa unlit strength 0.5\n",
                             "look 1\nmaterial 00000000000000aa normal 0.5\n",
                             "look 1\nmaterial 00000000000000aa normal strength\n",
                             "look 1\nmaterial 00000000000000aa normal strength 0.5 extra\n",
                             "look 1\nmaterial 00000000000000aa glow 1 strength 2\n"};
        int refused = 0;
        for (const char *one : bad)
        {
            Look l;
            refused += parseLook(one, l, &error) ? 0 : 1;
        }
        t.IsTrue(refused == static_cast<int>(sizeof(bad) / sizeof(bad[0])), "12 wrong material lines are each refused (" + std::to_string(refused) + ")");
    });

    tc.Run("the lights on the GPU renderer: off or without a camera the picture is the same bytes; on, the floor is lit under the spotlight and dimmed outside it, the overlay and unlit pixels are untouched", [](TestCase &t)
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
            ps2x_settings::resetForTest();
            ps2x_postfx::Params off;
            off.enabled = false;
            ps2x_postfx::setLive(off);
            GSMem::InitLookupTables();
            const Field plain = renderLitField(device, false, true, true);
            const Field noCamera = renderLitField(device, true, false, true);
            const Field lit = renderLitField(device, true, true, true);
            const Field litNoOverlay = renderLitField(device, true, true, false);
            t.IsTrue(plain.ok && noCamera.ok && lit.ok && litNoOverlay.ok, "four fields were read back");
            if (plain.ok && noCamera.ok && lit.ok && litNoOverlay.ok)
            {
                // The floor is 200 below the eye. (0, -200, 60) is under the spotlight (4 degrees off its axis): picture (320, 412).
                // (0, -200, 900) is 48 degrees off, outside the 20 degree cone: picture (320, 329). The clear shows at (40, 40).
                const uint8_t *under = lit.at(345, 412), *outside = lit.at(320, 329);
                t.IsTrue(near3(plain.at(345, 412), 200, 60, 20, 1) && near3(plain.at(320, 329), 200, 60, 20, 1), "lights off: the floor in its own colour");
                // The effect square (drawn without depth writes) covers picture (320, 412): under the lights it is the game's own pixel.
                t.IsTrue(near3(plain.at(320, 412), 40, 220, 80, 1), "lights off: the effect square in its own colour, " + rgbText(plain.at(320, 412)));
                t.IsTrue(std::memcmp(lit.at(320, 412), plain.at(320, 412), 4) == 0, "lights on: an effect in the middle of the cone is shown as the game drew it, " + rgbText(lit.at(320, 412)));
                t.IsTrue(plain.px == noCamera.px, "lights on but no camera: the same bytes as lights off");
                const int r = linearToSrgb8(srgbToLinear(200 / 255.0) * 0.5), g = linearToSrgb8(srgbToLinear(60 / 255.0) * 0.5), b = linearToSrgb8(srgbToLinear(20 / 255.0) * 0.5);
                t.IsTrue(near3(outside, r, g, b, 2), "outside the cone: the colour times the ambient 0.5 in linear light, " + rgbText(outside) + " for (" + std::to_string(r) + ", " +
                                                         std::to_string(g) + ", " + std::to_string(b) + ")");
                // Under the spot the light is 0.5 + 2 x falloff x facing, about 1.9: brighter than the game's colour, the hue kept.
                t.IsTrue(under[0] > 225 && under[0] > under[1] && under[1] > under[2] && under[1] > 60, "under the spotlight: brighter than the game drew it, " + rgbText(under));
                const int red = linearToSrgb8(srgbToLinear(200 / 255.0) * 1.9);
                t.IsTrue(under[0] <= 255 && red == 255, "(the light would take the red past white: the shoulder holds it in range)");
                t.IsTrue(std::memcmp(lit.at(40, 40), plain.at(40, 40), 4) == 0, "the clear, where nothing lit was drawn: the same bytes");
                t.IsTrue(near3(lit.at(530, 64), 255, 16, 16, 0) && std::memcmp(lit.at(530, 64), plain.at(530, 64), 4) == 0, "the overlay box: the game's own pixels");
                t.IsTrue(differing(lit, litNoOverlay, 0, 100, 640, 448) == 0u, "a field without an overlay is lit the same (the chain runs at the field copy)");
                t.IsTrue(near3(litNoOverlay.at(530, 64), 30, 40, 60, 0), "and has the clear where the box was");

                // "Show lights": the spotlight over (0, 300). The floor straight under it is at picture (320, 378): a dot in light 1's colour.
                const Field marked = renderLitField(device, true, true, true, testLook(300.0f), true);
                const Field unmarked = renderLitField(device, true, true, true, testLook(300.0f), false);
                t.IsTrue(marked.ok && unmarked.ok, "two more fields were read back");
                if (marked.ok && unmarked.ok)
                {
                    const uint8_t *dot = marked.at(320, 378);
                    t.IsTrue(near3(dot, 247, 63, 46, 8), "show lights: a red dot under light 1, " + rgbText(dot));
                    t.IsFalse(near3(unmarked.at(320, 378), 247, 63, 46, 8), "and none without the switch, " + rgbText(unmarked.at(320, 378)));
                    const uint32_t changed = differing(marked, unmarked, 0, 300, 640, 448);
                    t.IsTrue(changed > 200u && changed < 20000u, "the markers are a dot and thin outlines, not the whole floor (" + std::to_string(changed) + " pixels)");
                    t.IsTrue(std::memcmp(marked.at(530, 64), plain.at(530, 64), 4) == 0, "the overlay box is still the game's");
                }
            }
        }
        SDL_DestroyGPUDevice(device);
    });

    tc.Run("wrestler shape: the look file's three lines, the built-in values for a file without them, clamps and refusals; PS2X_LIGHT_SHAPE", [](TestCase &t)
    {
        Look look = defaultLook();
        t.IsTrue(look.wrestlerShape == kWrestlerShape && look.wrestlerRim == kWrestlerRim && look.wrestlerOutline == kWrestlerOutline && wrestlerShapeWanted(look),
                 "the built-in look has the built-in shape values, and they ask for shape");
        look.wrestlerShape = 0.25f;
        look.wrestlerRim = 1.5f;
        look.wrestlerOutline = 3.5f;
        Look back;
        std::string error;
        const std::string text = formatLook(look);
        t.IsTrue(text.find("wrestler_shape 0.25\n") != std::string::npos && text.find("wrestler_rim 1.5\n") != std::string::npos && text.find("wrestler_outline 3.5\n") != std::string::npos,
                 "the three lines are written");
        t.IsTrue(parseLook(text, back, &error) && back.wrestlerShape == 0.25f && back.wrestlerRim == 1.5f && back.wrestlerOutline == 3.5f && formatLook(back) == text,
                 "and read back the same" + (error.empty() ? std::string() : ": " + error));
        // A file written before these lines existed (the L2 format): everything else as it says, the shape at the built-in values.
        Look old;
        old.wrestlerShape = old.wrestlerRim = 0.0f;
        t.IsTrue(parseLook("look 1\nambient 0.3\nlight point\n  position 1 2 3\n", old, &error) && old.ambient[0] == 0.3f && old.lights.size() == 1u &&
                     old.wrestlerShape == kWrestlerShape && old.wrestlerRim == kWrestlerRim && old.wrestlerOutline == kWrestlerOutline,
                 "a file without the lines gets the built-in values");
        Look clamped;
        t.IsTrue(parseLook("look 1\nwrestler_shape 7\nwrestler_rim -1\nwrestler_outline 100\n", clamped) && clamped.wrestlerShape == 1.0f && clamped.wrestlerRim == 0.0f &&
                     clamped.wrestlerOutline == 8.0f,
                 "numbers out of range are clamped (shape 0 to 1, rim 0 to 4, outline 0.5 to 8)");
        Look none;
        t.IsTrue(parseLook("look 1\nwrestler_shape 0\nwrestler_rim 0\n", none) && !wrestlerShapeWanted(none), "both strengths at 0: no shape is wanted");
        const Look before = presetLook("two colours");
        int refused = 0, untouched = 0;
        for (const char *bad : {"look 1\nwrestler_shape\n", "look 1\nwrestler_rim 1 2\n", "look 1\nwrestler_outline wide\n", "look 1\nwrestler_glow 1\n"})
        {
            Look l = before;
            refused += parseLook(bad, l, &error) ? 0 : 1;
            untouched += formatLook(l) == formatLook(before) ? 1 : 0;
        }
        t.IsTrue(refused == 4 && untouched == 4, "four wrong lines are refused and leave the look alone");

        Look env = defaultLook();
        setEnvVar("PS2X_LIGHT_SHAPE", nullptr);
        t.IsFalse(wrestlerShapeFromEnvironment(env), "PS2X_LIGHT_SHAPE unset: the look's values stay");
        setEnvVar("PS2X_LIGHT_SHAPE", "0,0");
        t.IsTrue(wrestlerShapeFromEnvironment(env) && !wrestlerShapeWanted(env) && env.wrestlerOutline == kWrestlerOutline, "PS2X_LIGHT_SHAPE=0,0 turns the shape off and keeps the outline");
        setEnvVar("PS2X_LIGHT_SHAPE", "0.5,2,3");
        t.IsTrue(wrestlerShapeFromEnvironment(env) && env.wrestlerShape == 0.5f && env.wrestlerRim == 2.0f && env.wrestlerOutline == 3.0f, "three numbers set all three");
        setEnvVar("PS2X_LIGHT_SHAPE", nullptr);
    });

    tc.Run("generated shape: the look file's four lines, the built-in values for a file without them, clamps; PS2X_LIGHT_NORMALS; when it is wanted", [](TestCase &t)
    {
        Look look = defaultLook();
        t.IsTrue(look.wrestlerNormals == kWrestlerNormals && look.wrestlerPaint == kWrestlerPaint && look.wrestlerCrease == kWrestlerCrease && look.wrestlerSmooth == kWrestlerSmooth &&
                     wrestlerNormalsWanted(look),
                 "the built-in look has the built-in values, and they ask for generated shape");
        look.wrestlerNormals = 0.25f;
        look.wrestlerPaint = 1.5f;
        look.wrestlerCrease = 0.5f;
        look.wrestlerSmooth = true;
        Look back;
        std::string error;
        const std::string text = formatLook(look);
        t.IsTrue(text.find("wrestler_normals 0.25\n") != std::string::npos && text.find("wrestler_paint 1.5\n") != std::string::npos &&
                     text.find("wrestler_crease 0.5\n") != std::string::npos && text.find("wrestler_smooth 1\n") != std::string::npos,
                 "the four lines are written");
        t.IsTrue(parseLook(text, back, &error) && back.wrestlerNormals == 0.25f && back.wrestlerPaint == 1.5f && back.wrestlerCrease == 0.5f && back.wrestlerSmooth &&
                     formatLook(back) == text,
                 "and read back the same" + (error.empty() ? std::string() : ": " + error));
        // A file written before these lines existed (L2 or T1): the generated shape at the built-in values.
        Look old;
        old.wrestlerNormals = 0.0f;
        old.wrestlerSmooth = true;
        t.IsTrue(parseLook("look 1\nambient 0.3\nwrestler_shape 1\nwrestler_rim 0.8\n", old, &error) && old.wrestlerNormals == kWrestlerNormals && old.wrestlerPaint == kWrestlerPaint &&
                     old.wrestlerCrease == kWrestlerCrease && old.wrestlerSmooth == kWrestlerSmooth,
                 "a file without the lines gets the built-in values");
        Look clamped;
        t.IsTrue(parseLook("look 1\nwrestler_normals 9\nwrestler_paint -1\nwrestler_crease 3\nwrestler_smooth 0\n", clamped) && clamped.wrestlerNormals == 1.5f &&
                     clamped.wrestlerPaint == 0.0f && clamped.wrestlerCrease == 1.0f && !clamped.wrestlerSmooth,
                 "numbers out of range are clamped (weight 0 to 1.5, paint 0 to 2, crease 0 to 1)");
        Look none;
        t.IsTrue(parseLook("look 1\nwrestler_normals 0\n", none) && !wrestlerNormalsWanted(none) && wrestlerShapeWanted(none), "weight 0: no generated shape, the outline's stays");
        Look flat;
        t.IsTrue(parseLook("look 1\nwrestler_shape 0\n", flat) && !wrestlerNormalsWanted(flat), "shape 0: the maps would change nothing, so they are not wanted");
        Look bad = defaultLook();
        t.IsTrue(!parseLook("look 1\nwrestler_normals\n", bad, &error) && !parseLook("look 1\nwrestler_paint a\n", bad, &error) && formatLook(bad) == formatLook(defaultLook()),
                 "wrong lines are refused and leave the look alone");

        Look env = defaultLook();
        setEnvVar("PS2X_LIGHT_NORMALS", nullptr);
        t.IsFalse(wrestlerNormalsFromEnvironment(env), "PS2X_LIGHT_NORMALS unset: the look's values stay");
        setEnvVar("PS2X_LIGHT_NORMALS", "0");
        t.IsTrue(wrestlerNormalsFromEnvironment(env) && !wrestlerNormalsWanted(env) && env.wrestlerPaint == kWrestlerPaint, "PS2X_LIGHT_NORMALS=0 turns it off and keeps the rest");
        setEnvVar("PS2X_LIGHT_NORMALS", "1,0.5,0.25,1");
        t.IsTrue(wrestlerNormalsFromEnvironment(env) && env.wrestlerNormals == 1.0f && env.wrestlerPaint == 0.5f && env.wrestlerCrease == 0.25f && env.wrestlerSmooth,
                 "four numbers set all four");
        setEnvVar("PS2X_LIGHT_NORMALS", nullptr);
    });

    tc.Run("generated shape: a tile's normal map leans outward at its cut-out and is flat in the middle; paint leans away from the bright side; a mirrored tile gives the mirrored map", [](TestCase &t)
    {
        // A 32 x 32 tile: a disc of radius 13 texels, opaque (alpha 128), one grey. Around it transparent.
        const uint32_t W = 32, H = 32;
        const auto disc = [&](bool ramp, bool mirror) {
            std::vector<uint32_t> px(W * H, 0u);
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                {
                    const float dx = static_cast<float>(x) - 15.5f, dy = static_cast<float>(y) - 15.5f;
                    if (dx * dx + dy * dy > 13.0f * 13.0f)
                        continue;
                    const uint32_t sx = mirror ? W - 1u - x : x;
                    const uint32_t grey = ramp ? 40u + sx * 6u : 128u; // brighter to the right (before mirroring)
                    px[y * W + x] = grey | (grey << 8) | (grey << 16) | (128u << 24);
                }
            return px;
        };
        const auto value = [](const std::vector<uint8_t> &m, uint32_t x, uint32_t y, int c) { return (static_cast<int>(m[(y * 32u + x) * 4u + static_cast<uint32_t>(c)]) - 128) / 127.0f; };
        std::vector<uint8_t> plain(W * H * 4u), ramp(W * H * 4u), mirrored(W * H * 4u);
        generateTileNormals(disc(false, false).data(), W, H, 16u, plain.data());
        generateTileNormals(disc(true, false).data(), W, H, 16u, ramp.data());
        generateTileNormals(disc(true, true).data(), W, H, 16u, mirrored.data());
        t.IsTrue(std::fabs(value(plain, 15, 15, 0)) < 0.01f && std::fabs(value(plain, 16, 16, 1)) < 0.01f, "the middle of the disc (more than 5 texels inside) is flat");
        t.IsTrue(value(plain, 3, 15, 0) < -0.5f && value(plain, 28, 15, 0) > 0.5f && std::fabs(value(plain, 3, 15, 1)) < 0.2f, "the left edge leans left, the right edge right");
        t.IsTrue(value(plain, 15, 3, 1) < -0.5f && value(plain, 15, 28, 1) > 0.5f, "the top edge leans up (y is down in a texture), the bottom edge down");
        t.IsTrue(value(plain, 6, 15, 0) < value(plain, 8, 15, 0) && value(plain, 8, 15, 0) < -0.02f, "the lean falls off inward");
        bool noPaint = true;
        for (size_t i = 0; i < plain.size(); i += 4)
            noPaint = noPaint && plain[i + 2] == 128 && plain[i + 3] == 128;
        t.IsTrue(noPaint, "one flat colour: the paint part is 0 everywhere, also along the cut-out (no cliff)");
        t.IsTrue(value(ramp, 15, 15, 2) < -0.05f && std::fabs(value(ramp, 15, 15, 3)) < 0.02f, "brighter to the right counts as rising to the right: the surface leans left there");
        t.IsTrue(std::fabs(value(ramp, 3, 15, 2)) < 0.6f && ramp[(15u * W + 1u) * 4u + 2u] == 128, "the paint part stays small at the cut-out and is 0 outside it");
        bool same = true;
        for (uint32_t y = 0; y < H && same; ++y)
            for (uint32_t x = 0; x < W && same; ++x)
                for (int c = 0; c < 4; ++c)
                {
                    const int a = ramp[(y * W + x) * 4u + static_cast<uint32_t>(c)] - 128, b = mirrored[(y * W + (W - 1u - x)) * 4u + static_cast<uint32_t>(c)] - 128;
                    same = same && std::abs(((c & 1) == 0 ? -a : a) - b) <= 1;
                }
        t.IsTrue(same, "the mirrored tile's map is the mirror image with x turned round");
        // A tile cut by the border of its texture: the border counts as empty, so the map leans out there too.
        std::vector<uint32_t> full(W * H, 0x80808080u);
        std::vector<uint8_t> whole(W * H * 4u);
        generateTileNormals(full.data(), W, H, 16u, whole.data());
        t.IsTrue(value(whole, 0, 15, 0) < -0.5f && value(whole, 31, 15, 0) > 0.5f && std::fabs(value(whole, 15, 15, 0)) < 0.01f, "a full tile: its border is its cut-out");
        // The alpha test: texels below the reference are not part of the figure.
        std::vector<uint32_t> faint(W * H, 0x08808080u);
        generateTileNormals(faint.data(), W, H, 16u, whole.data());
        bool allFlat = true;
        for (uint8_t v : whole)
            allFlat = allFlat && v == 128;
        t.IsTrue(allFlat, "texels that fail the card's alpha test give a flat map");
    });

    tc.Run("generated shape: the bulge is kept at the wrestler's outline and faded where the tile's edge lies inside him; paint is not faded; the weight scales; with none the shape pass is T1's", [](TestCase &t)
    {
        Look look = defaultLook();
        look.wrestlerNormals = 1.0f;
        look.wrestlerPaint = 0.0f;
        const float edge[2] = {0.8f, 0.0f}, none[2] = {0.0f, 0.0f}, paint[2] = {0.0f, 0.5f};
        float atOutline[2], inside[2], unfaded[2], painted[2], half[2];
        wrestlerTileLean(look, edge, none, 0.55f, atOutline); // one texel from the tile's edge, and the wrestler's outline is as near
        wrestlerTileLean(look, edge, none, 1.0f, inside);     // the same texel of the tile, deep inside the wrestler
        t.IsTrue(std::fabs(atOutline[0] - 0.8f) < 0.01f && atOutline[1] == 0.0f, "at the outline the bulge comes through as it is");
        t.IsTrue(std::fabs(inside[0]) < 1.0e-4f, "deep inside the wrestler the same bulge is faded out: no crease at a joint");
        look.wrestlerCrease = 0.0f;
        wrestlerTileLean(look, edge, none, 1.0f, unfaded);
        t.IsTrue(std::fabs(unfaded[0] - 0.8f) < 0.01f, "with the crease fade at 0 it is kept");
        look.wrestlerCrease = 1.0f;
        look.wrestlerPaint = 1.0f;
        wrestlerTileLean(look, edge, paint, 1.0f, painted);
        t.IsTrue(std::fabs(painted[0]) < 1.0e-4f && std::fabs(painted[1] - 0.5f / std::sqrt(1.25f)) < 1.0e-4f, "the painted part is not faded");
        look.wrestlerNormals = 0.5f;
        wrestlerTileLean(look, edge, paint, 1.0f, half);
        t.IsTrue(std::fabs(half[1] - painted[1] * 0.5f) < 1.0e-5f, "the weight scales the result");
        // A point halfway: the fade is partial and monotonic in how far inside the wrestler the pixel is.
        look.wrestlerNormals = 1.0f;
        look.wrestlerPaint = 0.0f;
        float last = 1.0f;
        bool falling = true;
        for (float b = 0.5f; b <= 1.0f; b += 0.05f)
        {
            float v[2];
            wrestlerTileLean(look, edge, none, b, v);
            falling = falling && v[0] <= last + 1.0e-6f;
            last = v[0];
        }
        t.IsTrue(falling, "the further inside the wrestler, the less of a tile edge's bulge is left");

        // The shape pass: no tile lean, or (0, 0), is T1's result; a lean towards the light gains, away from it loses.
        look = defaultLook();
        look.lights.clear();
        Light l;
        l.type = LightType::Point;
        l.position[0] = -600.0f; l.position[1] = 300.0f; l.position[2] = 0.0f;
        l.radius = 3000.0f;
        l.strength = 2.0f;
        look.lights.push_back(l);
        look.wrestlerRim = 0.0f;
        const float at[3] = {0.0f, 150.0f, 0.0f};
        const double eye[3] = {0.0, 400.0, -900.0};
        float t1[3], zero[3], towards[3], away[3];
        wrestlerShapeAt(look, at, eye, 0.0f, 0.0f, 0.0f, 1.0f, t1);
        wrestlerShapeAt(look, at, eye, 0.0f, 0.0f, 0.0f, 1.0f, zero, none);
        t.IsTrue(t1[0] == zero[0] && t1[1] == zero[1] && t1[2] == zero[2], "a tile lean of (0, 0) changes nothing");
        // Which way is "towards the light" on the screen depends on the camera's handedness: take the side that gains.
        const float leftLean[2] = {-0.6f, 0.0f}, rightLean[2] = {0.6f, 0.0f};
        wrestlerShapeAt(look, at, eye, 0.0f, 0.0f, 0.0f, 1.0f, towards, leftLean);
        wrestlerShapeAt(look, at, eye, 0.0f, 0.0f, 0.0f, 1.0f, away, rightLean);
        float outlineLeft[3];
        wrestlerShapeAt(look, at, eye, -1.0f, 0.0f, 0.6f, 1.0f, outlineLeft);
        t.IsTrue((towards[0] - zero[0]) * (away[0] - zero[0]) < 0.0f, "leaning one way gains light and the other way loses it");
        t.IsTrue(std::fabs(towards[0] - outlineLeft[0]) < 1.0e-5f, "a tile lean tips the surface exactly as the same lean of the outline does");
        const float huge[2] = {-5.0f, 0.0f};
        float held[3], most[3];
        wrestlerShapeAt(look, at, eye, 0.0f, 0.0f, 0.0f, 1.0f, held, huge);
        const float mostLean[2] = {-kWrestlerLeanMost, 0.0f};
        wrestlerShapeAt(look, at, eye, 0.0f, 0.0f, 0.0f, 1.0f, most, mostLean);
        t.IsTrue(std::fabs(held[0] - most[0]) < 1.0e-5f, "the tip is held to kWrestlerLeanMost");
    });

    tc.Run("hand-made normal maps: the picture's axes (+x right, +y up, z out) become the generated maps' (+y down); blue is not read; the mark is 0 0 and never the generated maps' bytes", [](TestCase &t)
    {
        const auto texel = [](int r, int g, int b) {
            const uint8_t rgb[3] = {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
            std::array<uint8_t, 4> out{};
            handNormalTexel(rgb, out.data());
            return out;
        };
        const auto value = [](uint8_t byte) { return (static_cast<int>(byte) - 128) / 127.0f; };
        const auto flat = texel(128, 128, 255);
        t.IsTrue(flat[0] == 128 && flat[1] == 128, "(128, 128, 255) is flat");
        t.IsTrue(flat[2] == 0 && flat[3] == 0, "and carries the mark: 0 in both of the last two bytes");
        // a surface that faces to the right of the picture (x = +0.6, z = 0.8)
        const auto right = texel(static_cast<int>(std::lround((0.6f * 0.5f + 0.5f) * 255.0f)), 128, static_cast<int>(std::lround((0.8f * 0.5f + 0.5f) * 255.0f)));
        t.IsTrue(std::fabs(value(right[0]) - 0.6f) < 0.02f && right[1] == 128, "facing right: x is +0.6");
        // a surface that faces UP in the picture (y = +0.6 with +y up): the generated maps count y downward, so it is negative there
        const auto up = texel(128, static_cast<int>(std::lround((0.6f * 0.5f + 0.5f) * 255.0f)), 230);
        t.IsTrue(std::fabs(value(up[1]) + 0.6f) < 0.02f && up[0] == 128, "facing up: y is -0.6 in the generated maps' axes (+y down)");
        const auto down = texel(128, static_cast<int>(std::lround((-0.6f * 0.5f + 0.5f) * 255.0f)), 230);
        t.IsTrue(value(down[1]) > 0.55f, "facing down: +y in the generated maps' axes");
        // only x and y are read
        const auto noBlue = texel(200, 70, 0), fullBlue = texel(200, 70, 255);
        t.IsTrue(noBlue == fullBlue, "the blue channel is not read (a map saved with two channels works)");
        // an un-normalized x, y is shortened to length 1
        const auto big = texel(255, 255, 255);
        t.IsTrue(std::fabs(value(big[0]) - 0.7071f) < 0.02f && std::fabs(value(big[1]) + 0.7071f) < 0.02f, "(1, 1) is shortened to (0.707, 0.707)");
        // the bytes 0 and 1 are never 0 (a texel of a hand-made map is told apart by the last two alone, and these stay like the generated maps')
        bool never0 = true;
        for (int r = 0; r < 256; r += 5)
            for (int g = 0; g < 256; g += 5)
            {
                const auto o = texel(r, g, 128);
                never0 = never0 && o[0] >= 1 && o[1] >= 1 && o[2] == 0 && o[3] == 0;
            }
        t.IsTrue(never0, "bytes 0 and 1 are 1 to 255 and 2 and 3 are 0 for every colour");
        // generated maps never write 0 into either of the last two bytes, whatever the tile: the pair 0 0 is only a hand-made map's
        bool markFree = true;
        uint32_t seed = 12345u;
        for (int tile = 0; tile < 12; ++tile)
        {
            std::vector<uint32_t> px(32u * 32u);
            for (uint32_t &p : px)
            {
                seed = seed * 1664525u + 1013904223u;
                // random colours; a quarter of the texels (more for some tiles) are cut out
                p = (seed & 0x00FFFFFFu) | (((seed >> 24) & static_cast<uint32_t>(tile % 4 + 1)) != 0u ? 0x80000000u : 0u);
            }
            std::vector<uint8_t> map(32u * 32u * 4u);
            generateTileNormals(px.data(), 32u, 32u, 16u, map.data());
            for (size_t i = 0; i < map.size(); i += 4)
                markFree = markFree && map[i + 2] >= 1 && map[i + 3] >= 1;
        }
        t.IsTrue(markFree, "12 noisy tiles: no generated texel has a 0 in the last two bytes");
    });

    tc.Run("hand-made normal maps: a generated map written as a picture and read back is the same lean; at the paint weight it is the shape pass's own lean with the fade off; the weight scales", [](TestCase &t)
    {
        const uint32_t W = 32, H = 32;
        std::vector<uint32_t> px(W * H, 0u);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                const float dx = static_cast<float>(x) - 15.5f, dy = static_cast<float>(y) - 15.5f;
                if (dx * dx + dy * dy > 13.0f * 13.0f)
                    continue;
                const uint32_t grey = 30u + x * 5u + y * 2u; // a ramp, so that there is paint as well as bulge
                px[y * W + x] = grey | (grey << 8) | (grey << 16) | (128u << 24);
            }
        std::vector<uint8_t> generated(W * H * 4u), picture(W * H * 3u);
        generateTileNormals(px.data(), W, H, 16u, generated.data());
        // paint weight 0: the unit normal of the bulge alone, which is what the map stores
        generatedMapToPicture(generated.data(), W, H, 0.0f, picture.data());
        int worst = 0;
        for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i)
        {
            uint8_t back[4];
            handNormalTexel(&picture[i * 3u], back);
            worst = std::max({worst, std::abs(static_cast<int>(back[0]) - generated[i * 4u + 0]), std::abs(static_cast<int>(back[1]) - generated[i * 4u + 1])});
        }
        t.IsTrue(worst <= 2, "bulge only: the picture read back leans as the generated map does (largest difference " + std::to_string(worst) + " of 255)");
        // a flat texel is the usual flat colour, and the picture is a unit normal pointing out of the picture (blue above half)
        t.IsTrue(picture[(15u * W + 15u) * 3u + 2u] >= 250 && std::abs(static_cast<int>(picture[(15u * W + 15u) * 3u]) - 128) <= 2, "the middle of the disc is (128, 128, 255) or nearly");
        // with the paint: the shape pass's arithmetic (crease fade off, whole weight, deep inside the wrestler)
        Look look = defaultLook();
        look.wrestlerNormals = 1.0f;
        look.wrestlerPaint = 0.35f;
        look.wrestlerCrease = 0.0f;
        generatedMapToPicture(generated.data(), W, H, look.wrestlerPaint, picture.data());
        float worstLean = 0.0f;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                const uint8_t *g = &generated[(y * W + x) * 4u];
                const float bulge[2] = {(static_cast<float>(g[0]) - 128.0f) / 127.0f, (static_cast<float>(g[1]) - 128.0f) / 127.0f};
                const float paint[2] = {(static_cast<float>(g[2]) - 128.0f) / 127.0f * kTilePaintRange, (static_cast<float>(g[3]) - 128.0f) / 127.0f * kTilePaintRange};
                float lean[2];
                wrestlerTileLean(look, bulge, paint, 1.0f, lean);
                uint8_t back[4];
                handNormalTexel(&picture[(y * W + x) * 3u], back);
                const float hand[2] = {(static_cast<float>(back[0]) - 128.0f) / 127.0f, (static_cast<float>(back[1]) - 128.0f) / 127.0f};
                worstLean = std::max({worstLean, std::fabs(hand[0] - lean[0]), std::fabs(hand[1] - lean[1])});
            }
        t.IsTrue(worstLean < 0.02f, "with the paint: the same lean as wrestlerTileLean with the fade off, within a few steps of 255 (largest " + std::to_string(worstLean) + ")");
        // the weight: a hand map's lean is the map's x and y times wrestler_normals, nothing else
        const float hand[2] = {0.5f, -0.25f};
        float lean[2];
        look.wrestlerNormals = 0.6f;
        look.wrestlerPaint = 2.0f;   // (no effect on a hand map)
        look.wrestlerCrease = 1.0f;  // (neither)
        wrestlerHandLean(look, hand, lean);
        t.IsTrue(std::fabs(lean[0] - 0.3f) < 1.0e-6f && std::fabs(lean[1] + 0.15f) < 1.0e-6f, "weight 0.6 gives 0.6 of the map");
        look.wrestlerNormals = 0.0f;
        wrestlerHandLean(look, hand, lean);
        t.IsTrue(lean[0] == 0.0f && lean[1] == 0.0f, "weight 0 gives nothing");
    });

    tc.Run("the part tag: every tag reads back from both triangles of its quad whatever bytes it replaces; the game's own bytes never read as a tag", [](TestCase &t)
    {
        // The game's own byte for a corner is the low byte of the whole part of w + 1; the renderer has Q = 1 / w.
        const auto qOf = [](float w) { return 1.0f / w; };
        const auto ownByte = [](float w) { return static_cast<uint8_t>(static_cast<uint32_t>(std::floor(w + 1.0f)) & 0xFFu); };
        // Every payload (wrestler 0-15, class 0-15, order 0-31, the referee flag), each against a few sets of bytes it may replace:
        // corners at nearly the same depth (as a quad's are), and bytes that happen to equal the tag's own.
        uint64_t tags = 0, refused = 0, wrongFirst = 0, wrongSecond = 0;
        uint32_t seed = 12345u;
        const auto next = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        for (uint32_t wrestler = 0; wrestler < 16u; ++wrestler)
            for (uint32_t partClass = 0; partClass < 16u; ++partClass)
                for (uint32_t order = 0; order < 32u; ++order)
                    for (uint32_t referee = 0; referee < 2u; ++referee)
                        for (int trial = 0; trial < 6; ++trial)
                        {
                            PartTag tag;
                            tag.wrestler = static_cast<uint8_t>(wrestler);
                            tag.partClass = static_cast<uint8_t>(partClass);
                            tag.order = static_cast<uint8_t>(order);
                            tag.referee = referee != 0u;
                            float w[4];
                            const float base = 300.0f + static_cast<float>(next() % 3000u);
                            for (float &value : w)
                                value = base + static_cast<float>(next() % 6000u) / 100.0f;
                            uint8_t original[4], bytes[4];
                            for (int i = 0; i < 4; ++i)
                                original[i] = ownByte(w[i]);
                            if (trial >= 4)
                            {
                                // The worst case for the game's side: the corners' own bytes are the very bytes one salt would write.
                                const uint8_t data1 = static_cast<uint8_t>(wrestler | (partClass << 4));
                                const uint8_t data2 = static_cast<uint8_t>(order | (referee << 5) | ((trial - 4u) << 6));
                                const uint8_t same[4] = {partTagCheckFirst(data1, data2), data1, data2, partTagCheckLast(data1, data2)};
                                for (int i = 0; i < 4; ++i)
                                {
                                    // a w whose own byte is that byte
                                    w[i] = std::floor(base / 256.0f) * 256.0f + static_cast<float>(same[i]) - 1.0f + 0.5f;
                                    original[i] = ownByte(w[i]);
                                }
                            }
                            ++tags;
                            if (!encodePartTag(tag, original, bytes))
                            {
                                ++refused;
                                continue;
                            }
                            const float q[4] = {qOf(w[0]), qOf(w[1]), qOf(w[2]), qOf(w[3])};
                            PartTag first, second;
                            const bool a = decodePartTag(bytes, q, first), b = decodePartTag(bytes + 1, q + 1, second);
                            const auto same = [&](const PartTag &x) {
                                return x.wrestler == tag.wrestler && x.partClass == tag.partClass && x.order == tag.order && x.referee == tag.referee;
                            };
                            wrongFirst += (a && same(first)) ? 0u : 1u;
                            wrongSecond += (b && same(second)) ? 0u : 1u;
                        }
        t.IsTrue(tags == 16u * 16u * 32u * 2u * 6u && refused == 0u, "every tag can be written (" + std::to_string(refused) + " of " + std::to_string(tags) + " refused)");
        t.IsTrue(wrongFirst == 0u && wrongSecond == 0u,
                 "every tag reads back from its first and its second triangle (" + std::to_string(wrongFirst) + " and " + std::to_string(wrongSecond) + " wrong)");
        // For every pair of data bytes at least two of the four salts leave the second triangle unlike a first one, so that one
        // of them is also clear of the bytes it replaces.
        uint32_t fewest = 4;
        for (uint32_t data1 = 0; data1 < 256u; ++data1)
            for (uint32_t plain2 = 0; plain2 < 64u; ++plain2)
            {
                uint32_t free = 0;
                for (uint32_t salt = 0; salt < 4u; ++salt)
                {
                    const uint8_t data2 = static_cast<uint8_t>(plain2 | (salt << 6));
                    free += static_cast<uint8_t>(data1) != partTagCheckFirst(data2, partTagCheckLast(static_cast<uint8_t>(data1), data2)) ? 1u : 0u;
                }
                fewest = std::min(fewest, free);
            }
        t.IsTrue(fewest >= 2u, "at least two salts are free for every tag (fewest: " + std::to_string(fewest) + ")");
        // The game's own bytes, at any depth and with the renderer's Q rounded as a float: never a tag.
        uint64_t misread = 0, triangles = 0;
        for (int i = 0; i < 400000; ++i)
        {
            float w[3];
            const float base = 50.0f + static_cast<float>(next() % 600000u) / 100.0f;
            uint8_t fog[3];
            float q[3];
            for (int k = 0; k < 3; ++k)
            {
                w[k] = base + static_cast<float>(next() % 8000u) / 100.0f;
                fog[k] = ownByte(w[k]);
                q[k] = qOf(w[k]);
            }
            PartTag tag;
            ++triangles;
            misread += decodePartTag(fog, q, tag) ? 1u : 0u;
        }
        t.IsTrue(misread == 0u, "none of " + std::to_string(triangles) + " untagged triangles reads as a tag (" + std::to_string(misread) + ")");
        // The other emitter of the game (0x18D160) leaves w, not w + 1: still the game's own.
        {
            const float w[3] = {812.3f, 830.9f, 799.99f};
            const uint8_t fog[3] = {static_cast<uint8_t>(static_cast<uint32_t>(w[0]) & 0xFFu), static_cast<uint8_t>(static_cast<uint32_t>(w[1]) & 0xFFu),
                                    static_cast<uint8_t>(static_cast<uint32_t>(w[2]) & 0xFFu)};
            const float q[3] = {qOf(w[0]), qOf(w[1]), qOf(w[2])};
            PartTag tag;
            t.IsTrue(!decodePartTag(fog, q, tag), "bytes made from w alone are not a tag either");
        }
    });

    tc.Run("the part tag: which part classes are joined; a kept edge is not faded, and without one the fade is what it was", [](TestCase &t)
    {
        bool symmetric = true, selves = true;
        int joins = 0;
        for (uint32_t a = 0; a < kPartClassCount; ++a)
        {
            selves = selves && partsJoined(a, a);
            for (uint32_t b = 0; b < kPartClassCount; ++b)
            {
                symmetric = symmetric && partsJoined(a, b) == partsJoined(b, a);
                joins += (a < b && partsJoined(a, b)) ? 1 : 0;
            }
        }
        t.IsTrue(symmetric && selves, "the table is the same both ways and a class is joined to itself");
        t.IsTrue(joins == 8, "eight joints: neck, waist, two shoulders' worth (chest-upper arm), elbow, wrist, hip, knee, ankle (" + std::to_string(joins) + ")");
        t.IsTrue(partsJoined(6, 2) && partsJoined(3, 4) && partsJoined(5, 8) && !partsJoined(5, 2) && !partsJoined(4, 1) && !partsJoined(7, 2) && !partsJoined(5, 1),
                 "a thigh and the waist are joined, an upper arm and a forearm, a shin and a foot are; a shin and the waist, a forearm and the chest, a hand and the waist, a shin and the chest are not");
        t.IsTrue(partsJoined(kPartObject, 1) && partsJoined(1, 12), "an object or an unknown class counts as joined: the fade stays as it was");
        Look look = defaultLook();
        look.wrestlerNormals = 1.0f;
        look.wrestlerPaint = 0.0f;
        const float edge[2] = {0.8f, 0.0f}, none[2] = {0.0f, 0.0f};
        float faded[2], same[2], kept[2], atOutline[2];
        wrestlerTileLean(look, edge, none, 1.0f, faded);
        wrestlerTileLean(look, edge, none, 1.0f, same, 0.0f);
        wrestlerTileLean(look, edge, none, 1.0f, kept, 1.0f);
        wrestlerTileLean(look, edge, none, 0.55f, atOutline);
        t.IsTrue(std::memcmp(faded, same, sizeof(faded)) == 0, "with no kept edge the result is bit for bit the one from before the tag");
        t.IsTrue(std::fabs(faded[0]) < 1.0e-4f && std::fabs(kept[0] - atOutline[0]) < 1.0e-6f,
                 "deep inside the wrestler a kept edge keeps the bulge it has on the outline");
    });

    tc.Run("wrestler shape: a flat-on pixel keeps today's light under the first light; the lit side of the outline gains and the far side loses; the rim only adds", [](TestCase &t)
    {
        // The made-up camera's place and a point on a card in front of it; one light to the left and in front.
        const double eye[3] = {0.0, 0.0, -1000.0};
        const float at[3] = {0.0f, -90.0f, 0.0f};
        Look look = shapeLook(1.0f, 0.0f);
        float flat[3], left[3], right[3], up[3];
        wrestlerShapeAt(look, at, eye, 0.0f, 0.0f, 0.0f, 1.0f, flat);
        t.IsTrue(std::fabs(flat[0]) < 1.0e-5f && std::fabs(flat[2]) < 1.0e-5f, "no lean: the change is 0 (" + number(flat[0]) + ")");
        wrestlerShapeAt(look, at, eye, -1.0f, 0.0f, kWrestlerLean, 1.0f, left);
        wrestlerShapeAt(look, at, eye, 1.0f, 0.0f, kWrestlerLean, 1.0f, right);
        wrestlerShapeAt(look, at, eye, 0.0f, -1.0f, kWrestlerLean, 1.0f, up);
        t.IsTrue(left[0] > 0.05f && right[0] < -0.05f, "the outline on the light's side gains (" + number(left[0]) + "), the other side loses (" + number(right[0]) + ")");
        t.IsTrue(up[0] < left[0] && up[0] > right[0], "an outline that faces up, with the light level with it, is in between (" + number(up[0]) + ")");
        // By hand for the left outline, with the eye level with the point (so "towards the eye" is exactly -z):
        // N = 0.92 x left + sqrt(1 - 0.92^2) x towards the eye; L = (-500, 0, -400) / |...|.
        {
            const double level[3] = {0.0, -90.0, -1000.0};
            wrestlerShapeAt(look, at, level, -1.0f, 0.0f, kWrestlerLean, 1.0f, left);
            const double lx = -500.0 / std::sqrt(500.0 * 500.0 + 400.0 * 400.0), lz = -400.0 / std::sqrt(500.0 * 500.0 + 400.0 * 400.0);
            const double depth = std::sqrt(1.0 - 0.92 * 0.92), length = std::sqrt(0.92 * 0.92 + depth * depth);
            const double nl = (-0.92 * lx - depth * lz) / length;
            const double wrapN = std::fmin(1.0, (nl + 0.5) / 1.5), wrapFlat = (-lz + 0.5) / 1.5;
            float today[3];
            const float anyNormal[3] = {0.0f, 1.0f, 0.0f};
            lookAt(look, at, anyNormal, Facing::PositionOnly, today);
            const double lightOnly = today[0] - 0.5;
            const double expected = lightOnly * (wrapN / wrapFlat) - lightOnly;
            t.IsTrue(std::fabs(left[0] - expected) < 1.0e-4, "the left outline by hand: " + number(expected) + ", got " + number(left[0]));
        }
        // The mirror camera (cross(up, view) to the left of the screen): the same lean on the screen is the other way in the world.
        float mirrored[3];
        wrestlerShapeAt(look, at, eye, -1.0f, 0.0f, kWrestlerLean, -1.0f, mirrored);
        t.IsTrue(std::fabs(mirrored[0] - right[0]) < 1.0e-5f, "the right-hand sign turns the lean round");
        // The rim alone: nothing well inside, light on the side that faces the light, none on the far side, never less than 0.
        look = shapeLook(0.0f, 1.0f);
        float rimFlat[3], rimLeft[3], rimRight[3];
        wrestlerShapeAt(look, at, eye, 0.0f, 0.0f, 0.0f, 1.0f, rimFlat);
        wrestlerShapeAt(look, at, eye, -1.0f, 0.0f, kWrestlerLean, 1.0f, rimLeft);
        wrestlerShapeAt(look, at, eye, 1.0f, 0.0f, kWrestlerLean, 1.0f, rimRight);
        t.IsTrue(rimFlat[0] == 0.0f && rimLeft[0] > 0.1f && rimRight[0] == 0.0f, "the rim: 0 inside, " + number(rimLeft[0]) + " towards the light, 0 away from it");
        look = shapeLook(0.0f, 0.0f);
        wrestlerShapeAt(look, at, eye, -1.0f, 0.0f, kWrestlerLean, 1.0f, left);
        t.IsTrue(left[0] == 0.0f && left[1] == 0.0f && left[2] == 0.0f, "both strengths at 0: no change anywhere");
        look = shapeLook(1.0f, 1.0f);
        look.lights.clear();
        wrestlerShapeAt(look, at, eye, -1.0f, 0.0f, kWrestlerLean, 1.0f, left);
        t.IsTrue(left[0] == 0.0f, "no lights: no change");
    });

    tc.Run("wrestler shape on the GPU renderer: at strength 0 the lit picture's bytes; with it, only the card changes, its lit edge gains and its far edge loses, a post in front cuts no edge", [](TestCase &t)
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
            ps2x_settings::resetForTest();
            ps2x_postfx::Params off;
            off.enabled = false;
            ps2x_postfx::setLive(off);
            GSMem::InitLookupTables();
            setEnvVar("PS2X_LIGHT_SHAPE", nullptr);
            const Field plain = renderLitField(device, false, true, true, shapeLook(1.0f, 1.0f), false, 1);
            const Field flat = renderLitField(device, true, true, true, shapeLook(0.0f, 0.0f), false, 1);
            const uint32_t passesFlat = ps2x_postfx::status().lastPasses.load();
            const Field shaped = renderLitField(device, true, true, true, shapeLook(1.0f, 0.0f), false, 1);
            const uint32_t passesShaped = ps2x_postfx::status().lastPasses.load();
            const Field rimmed = renderLitField(device, true, true, true, shapeLook(0.0f, 1.0f), false, 1);
            const Field noOverlay = renderLitField(device, true, true, false, shapeLook(1.0f, 0.0f), false, 1);
            const Field flatPost = renderLitField(device, true, true, true, shapeLook(0.0f, 0.0f), false, 2);
            const Field shapedPost = renderLitField(device, true, true, true, shapeLook(1.0f, 0.0f), false, 2);
            setEnvVar("PS2X_LIGHT_SHAPE", "0,0");
            const Field byEnvironment = renderLitField(device, true, true, true, shapeLook(1.0f, 1.0f), false, 1);
            setEnvVar("PS2X_LIGHT_SHAPE", nullptr);
            const bool ok = plain.ok && flat.ok && shaped.ok && rimmed.ok && noOverlay.ok && flatPost.ok && shapedPost.ok && byEnvironment.ok;
            t.IsTrue(ok, "eight fields were read back");
            if (ok)
            {
                // The card is picture x 290 to 350, y 254 to 374. Rows 300 to 330 are clear of its top and bottom edge.
                t.IsTrue(near3(plain.at(320, 314), 120, 120, 120, 1), "lights off: the card in its own grey, " + rgbText(plain.at(320, 314)));
                t.IsTrue(flat.px == byEnvironment.px, "PS2X_LIGHT_SHAPE=0,0 over a look that asks for shape: the bytes of the look with both strengths at 0");
                t.IsTrue(passesFlat == 1u && passesShaped == 4u, "the chain: 1 pass at strength 0, 4 with shape (two for the mask's blur, one to light the card again, the multiply): " +
                                                                    std::to_string(passesFlat) + ", " + std::to_string(passesShaped));
                // Outside the card nothing changes, to the byte.
                uint32_t outside = 0, inside = 0;
                for (uint32_t y = 0; y < 448; ++y)
                    for (uint32_t x = 0; x < 640; ++x)
                    {
                        const bool onCard = x >= 289 && x <= 351 && y >= 252 && y <= 376;
                        const bool differs = std::memcmp(shaped.at(x, y), flat.at(x, y), 4) != 0 || std::memcmp(rimmed.at(x, y), flat.at(x, y), 4) != 0;
                        (onCard ? inside : outside) += differs ? 1u : 0u;
                    }
                t.IsTrue(outside == 0u && inside > 500u, "shape and rim change pixels of the card (" + std::to_string(inside) + ") and none outside it (" + std::to_string(outside) + ")");
                t.IsTrue(std::memcmp(shaped.at(530, 64), plain.at(530, 64), 4) == 0, "the overlay box: the game's own pixels");
                // The middle of the card is flat-on: as at strength 0. The light is to the left: the left edge gains, the right edge loses.
                const int mid = shaped.at(320, 314)[0] - flat.at(320, 314)[0];
                const int left = shaped.at(291, 314)[0] - flat.at(291, 314)[0], right = shaped.at(348, 314)[0] - flat.at(348, 314)[0];
                t.IsTrue(std::abs(mid) <= 1, "the middle of the card keeps today's light (" + std::to_string(mid) + " levels)");
                t.IsTrue(left >= 8 && right <= -8, "the edge towards the light gains (" + std::to_string(left) + " levels), the far edge loses (" + std::to_string(right) + ")");
                // Against the reference arithmetic: at the left edge's outermost pixel the blurred mask is about 1/2, so the lean is its
                // full 0.92, straight to the left (the edge is upright and far from the corners).
                {
                    const double eye[3] = {0.0, 0.0, -1000.0};
                    const float at[3] = {-59.0f, -90.0f, 0.0f}; // picture (290, 314)
                    const Look look = shapeLook(1.0f, 0.0f);
                    float change[3], today[3];
                    const float anyNormal[3] = {0.0f, 1.0f, 0.0f};
                    wrestlerShapeAt(look, at, eye, -1.0f, 0.0f, kWrestlerLean, 1.0f, change);
                    lookAt(look, at, anyNormal, Facing::PositionOnly, today);
                    const int expected = linearToSrgb8(srgbToLinear(120 / 255.0) * (today[0] + change[0]));
                    const int got = shaped.at(290, 314)[0];
                    t.IsTrue(std::abs(got - expected) <= 6, "the outermost pixel of the lit edge: " + std::to_string(got) + " for the reference's " + std::to_string(expected));
                }
                // The rim alone: light on the left edge only.
                const int rimLeft = rimmed.at(290, 314)[0] - flat.at(290, 314)[0], rimMid = rimmed.at(320, 314)[0] - flat.at(320, 314)[0],
                          rimRight = rimmed.at(349, 314)[0] - flat.at(349, 314)[0];
                t.IsTrue(rimLeft >= 8 && rimMid == 0 && rimRight == 0, "the rim alone: " + std::to_string(rimLeft) + " levels on the lit edge, " + std::to_string(rimMid) +
                                                                           " in the middle, " + std::to_string(rimRight) + " on the far edge");
                t.IsTrue(differing(shaped, noOverlay, 0, 100, 640, 448) == 0u, "a field without an overlay is shaped the same (the chain runs at the field copy)");
                // A post in front of the card (picture x 316 to 324): the card's pixels beside it are as without the post, because the
                // mask is drawn with no depth test; the post's own pixels are not shaped.
                uint32_t beside = 0, onPost = 0;
                for (uint32_t y = 300; y < 330; ++y)
                {
                    for (uint32_t x = 292; x < 314; ++x)
                        beside += std::memcmp(shapedPost.at(x, y), shaped.at(x, y), 4) != 0 ? 1u : 0u;
                    for (uint32_t x = 327; x < 348; ++x)
                        beside += std::memcmp(shapedPost.at(x, y), shaped.at(x, y), 4) != 0 ? 1u : 0u;
                    for (uint32_t x = 318; x < 323; ++x)
                        onPost += std::memcmp(shapedPost.at(x, y), flatPost.at(x, y), 4) != 0 ? 1u : 0u;
                }
                t.IsTrue(beside == 0u, "a post in front cuts no edge into the card: the card beside it is as without the post (" + std::to_string(beside) + " pixels differ)");
                t.IsTrue(onPost == 0u, "and the post's own pixels are not shaped (" + std::to_string(onPost) + " differ)");
            }
        }
        SDL_DestroyGPUDevice(device);
    });

    tc.Run("the debug views on the GPU renderer: off or without a camera the picture is the same bytes; with both, normals and heights are drawn", [](TestCase &t)
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
            ps2x_settings::resetForTest();
            ps2x_postfx::Params off;
            off.enabled = false;
            ps2x_postfx::setLive(off);
            GSMem::InitLookupTables();
            const Field plain = renderField(device, nullptr, false);
            const Field cameraOnly = renderField(device, nullptr, true);
            const Field switchOnly = renderField(device, "normal", false);
            const Field normals = renderField(device, "normal", true);
            const Field heights = renderField(device, "position", true);
            t.IsTrue(plain.ok && cameraOnly.ok && switchOnly.ok && normals.ok && heights.ok, "five fields were read back");
            if (plain.ok && cameraOnly.ok && switchOnly.ok && normals.ok && heights.ok)
            {
                // The floor's centre is at GS (2048, 2131): picture (320, 390). The wall's is at GS (2048, 2065): picture (320, 258).
                t.IsTrue(near3(plain.at(320, 390), 200, 60, 20, 1) && near3(plain.at(320, 258), 20, 60, 200, 1),
                         "the plain picture has the two triangles in their own colours: floor " + rgbText(plain.at(320, 390)) + ", wall " + rgbText(plain.at(320, 258)));
                t.IsTrue(plain.px == cameraOnly.px, "a camera without the switch: the same bytes");
                t.IsTrue(plain.px == switchOnly.px, "the switch without a camera: the same bytes");
                t.IsTrue(near3(normals.at(320, 390), 128, 255, 128, 1), "normal view: the floor faces up, (128, 255, 128); got " + rgbText(normals.at(320, 390)));
                t.IsTrue(near3(normals.at(320, 258), 128, 128, 0, 1), "normal view: the wall faces the eye at -z, (128, 128, 0); got " + rgbText(normals.at(320, 258)));
                t.IsTrue(near3(normals.at(40, 40), 30, 40, 60, 1), "normal view: the clear is left alone");
                // Height view: y = -200 is below the ramp, its first colour (26, 38, 140), or that times 0.45 on a grid line; y = 0 on the
                // wall's centre line is halfway from the first colour to the second: (26, 96, 153).
                const uint8_t *floorPixel = heights.at(320, 390);
                t.IsTrue(near3(floorPixel, 26, 38, 140, 2) || near3(floorPixel, 11, 17, 63, 2), "height view: the floor, 200 below the eye; got " + rgbText(floorPixel));
                bool gridSeen = false, plainSeen = false;
                for (uint32_t x = 250; x < 390; ++x)
                {
                    gridSeen = gridSeen || near3(heights.at(x, 420), 11, 17, 63, 2);
                    plainSeen = plainSeen || near3(heights.at(x, 420), 26, 38, 140, 2);
                }
                t.IsTrue(gridSeen && plainSeen, "height view: the floor shows grid lines between plain squares");
                const uint8_t *wallPixel = heights.at(325, 248); // y = 0 is GS row 2048: picture row 224; a little below it, off the x = 0 grid line
                t.IsTrue(wallPixel[2] > wallPixel[0] && wallPixel[1] > 40 && wallPixel[1] < 110, "height view: the wall near y 0 is between the first two colours; got " + rgbText(wallPixel));
                t.IsFalse(heights.px == normals.px, "the two views differ");
            }
        }
        SDL_DestroyGPUDevice(device);
    });

    });
}
