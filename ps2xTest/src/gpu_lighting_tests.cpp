#include "MiniTest.h"
#include "gpu/gpu_lighting.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

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

    });
}
