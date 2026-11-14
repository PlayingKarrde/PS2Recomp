#include "MiniTest.h"
#include "gpu/gpu_lighting.h"
#include "gpu/gpu_postfx.h"
#include "ps2_settings.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_presentation.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    void worldTriangle(GSRasterBackend &b, const float m[16], const double points[3][3], double z, uint8_t r, uint8_t g, uint8_t bl)
    {
        GSPrimitiveBatch p;
        p.vertexCount = 3;
        p.state.context = sceneContext();
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
    void drawLitField(GSRasterBackend &b, const float m[16], bool overlay)
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

    // A renderer with the lights' environment as given (PS2X_LIGHT=1 when `lights`), the field drawn twice, the second field read.
    Field renderLitField(SDL_GPUDevice *device, bool lights, bool camera, bool overlay)
    {
        Field out;
        setEnvVar("PS2X_LIGHT", lights ? "1" : nullptr);
        setEnvVar("PS2X_LIGHT_AMBIENT", "0.5");
        setEnvVar("PS2X_LIGHT_SPOT_INTENSITY", "2");
        setEnvVar("PS2X_LIGHT_SPOT_COLOUR", "1,1,1");
        setEnvVar("PS2X_LIGHT_SPOT_CONE", "10,20");
        setEnvVar("PS2X_LIGHT_SPOT_HEIGHT", "600");
        setEnvVar("PS2X_LIGHT_SPOT_RADIUS", "2000");
        std::vector<uint8_t> vram(GSMem::MEMORY_SIZE, 0);
        GSGpuBackend gpu(device, 1, GSGpuBackend::FieldMode::Progressive);
        for (const char *name : {"PS2X_LIGHT", "PS2X_LIGHT_AMBIENT", "PS2X_LIGHT_SPOT_INTENSITY", "PS2X_LIGHT_SPOT_COLOUR", "PS2X_LIGHT_SPOT_CONE",
                                 "PS2X_LIGHT_SPOT_HEIGHT", "PS2X_LIGHT_SPOT_RADIUS"})
            setEnvVar(name, nullptr);
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
        t.IsTrue(u.spotPosition[1] == 900.0f && u.spotPosition[3] == 1500.0f && u.spotAxis[1] == -1.0f && u.spotAxis[3] == 3.0f && u.spotCone[2] == 0.0f &&
                     std::fabs(u.spotCone[0] - std::cos(30.0f * 3.14159265f / 180.0f)) < 1e-6f && std::fabs(u.spotCone[1] - std::cos(18.0f * 3.14159265f / 180.0f)) < 1e-6f,
                 "position and radius, axis and strength, the cosines of the cone, the rule");
        t.IsTrue(lightUniforms(look, Facing::Half).spotCone[2] == 1.0f && lightUniforms(look, Facing::PositionOnly).spotCone[2] == 2.0f, "the three rules as numbers");
        const float up[3] = {0.0f, 1.0f, 0.0f};
        double worst = 0.0;
        for (const float x : {0.0f, 150.0f, 280.0f, 330.0f, 420.0f, 600.0f})
        {
            const float at[3] = {x, 100.0f, 40.0f};
            float expected[3];
            lookAt(look, at, up, Facing::Lambert, expected);
            const float toLight[3] = {u.spotPosition[0] - at[0], u.spotPosition[1] - at[1], u.spotPosition[2] - at[2]};
            const float distance = std::sqrt(toLight[0] * toLight[0] + toLight[1] * toLight[1] + toLight[2] * toLight[2]);
            float got[3] = {u.ambient[0], u.ambient[1], u.ambient[2]};
            if (distance < u.spotPosition[3])
            {
                const float ratio = distance / u.spotPosition[3];
                const float fall = (1.0f - ratio * ratio) * (1.0f - ratio * ratio);
                const float l[3] = {toLight[0] / distance, toLight[1] / distance, toLight[2] / distance};
                const float cosAngle = -(u.spotAxis[0] * l[0] + u.spotAxis[1] * l[1] + u.spotAxis[2] * l[2]);
                const float e = std::fmin(1.0f, std::fmax(0.0f, (cosAngle - u.spotCone[0]) / (u.spotCone[1] - u.spotCone[0])));
                const float cone = e * e * (3.0f - 2.0f * e);
                const float face = std::fmax(up[0] * l[0] + up[1] * l[1] + up[2] * l[2], 0.0f);
                for (int i = 0; i < 3; ++i)
                    got[i] += u.spotColour[i] * (u.spotAxis[3] * fall * cone * face);
            }
            for (int i = 0; i < 3; ++i)
                worst = std::fmax(worst, std::fabs(static_cast<double>(got[i]) - expected[i]));
        }
        t.IsTrue(worst < 1e-4, "six points across the cone: the block's arithmetic and lookAt agree (worst " + number(worst) + ")");
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
                const uint8_t *under = lit.at(320, 412), *outside = lit.at(320, 329);
                t.IsTrue(near3(plain.at(320, 412), 200, 60, 20, 1) && near3(plain.at(320, 329), 200, 60, 20, 1), "lights off: the floor in its own colour");
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
