// ps2x_gs_replay: plays a GS recording (see ps2xRuntime/include/runtime/gs/gs_recording.h and
// docs/formats/gs-recording.md) through a GS renderer without the game.
//
//   ps2x_gs_replay <file> [--backend cpu|gpu] [--compare] [--driver D] [--scale N] [--gpu-debug]
//                         [--png-dir D] [--png-every N] [--diff-dir D] [--diff-worst N] [--diff-every N]
//                         [--json F] [--threads N] [--defer 0|1] [--hashes F] [--max-markers N]
//                         [--drop-state LIST] [--info] [--presents] [--offsets] [--fields weave|progressive]
//                         [--direct [--direct-every N] [--direct-dir D]]
//
// How it works. The recording starts at a presentation marker with the whole GS memory and the
// backend's other state. The player loads that into a fresh backend, then makes every call the game
// made, in order. At each marker it waits for the renderer to finish, asks for the picture (Present)
// and hashes it. While the game ran, the presenter thread did the same for the markers it showed and
// the recording kept the hashes ("live" hashes); for every marker the recording also holds a hash of
// the displayed VRAM pages. With the CPU renderer the player compares both kinds.
//
// --backend gpu replays through GSGpuBackend (SDL_GPU, headless). Its pictures cannot equal the live
// hashes, so those are reported as "not comparable". --compare replays through both renderers on the
// same records and, for every picture, measures the share of pixels where any channel differs by more
// than 2 (and the largest difference); it reports mean, median, 95th percentile and the worst frame per
// clip, and writes the CPU, GPU and an amplified difference picture for the worst frames (--diff-dir).
//
// --fields weave|progressive (default weave) is the GPU renderer's field mode (GSGpuBackend::FieldMode). Weave draws each
// 224-line field as it is and weaves the last three into 448 lines, so its pictures compare with the CPU renderer's.
// Progressive draws every field as a 640 x 448 picture. With --compare the GPU's 448-line picture is first averaged
// down to the CPU picture's 224 lines (pairs of rows), which is only a rough check: the CPU renderer samples the
// field at integer positions and a progressive row pair at y and y + 1/2, so edges and textures differ by up to a
// quarter of a line.
// --offsets lists where the half-line offset between the game's alternate fields is applied (investigation, task g4a).
//
// --direct (with --backend gpu) also runs the GPU renderer's direct presentation path (path B, see
// gs_gpu_present.h) next to its readback path (path A): at each marker the picture the runtime would get
// from path A is rebuilt the way UploadFrame does it (the CPU GSFieldDeinterlacer), the picture path B
// builds on the GPU is downloaded, and the two are compared byte for byte. --direct-every N consumes only
// every Nth marker, as a host that cannot keep up with the game does. --direct-dir D writes the pictures of
// the first differing markers.
//
// Exit code: 0 when everything that could be checked matched (with --compare also: the pictures are
// within the thresholds below), 1 when something did not, 2 on usage errors or a file that cannot be read.

#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_field_deinterlacer.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_gpu_math.h"
#include "runtime/gs/gs_gpu_present.h"
#include "runtime/gs/gs_recording.h"
#include "../common/png_write.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using ps2x::test::writePng;

    // Provisional acceptance thresholds for --compare (to be reviewed): the mean share of pixels
    // over 2 per clip below 1%, and the worst frame below 5%.
    constexpr double kMeanThresholdPercent = 1.0;
    constexpr double kWorstThresholdPercent = 5.0;
    constexpr int kPixelTolerance = 2;
    constexpr uint32_t kStride = GSCpuBackend::kPresentationStride; // pixels per row of every picture

    void setEnv(const char *name, const char *value)
    {
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }

    struct Options
    {
        std::string file;
        std::string backend = "cpu";
        std::string fields = "weave";
        std::string compareRows = "parity";
        bool compare = false;
        std::string driver;
        uint32_t scale = 1;
        bool gpuDebug = false;
        std::string pngDir;
        uint32_t pngEvery = 30;
        std::string diffDir;
        uint32_t diffWorst = 8;
        uint32_t diffEvery = 60;
        std::string jsonFile;
        std::string hashesFile;
        int threads = -1;
        int defer = -1;
        uint64_t maxMarkers = ~0ull;
        std::string dropState; // debugging: start without part of the recorded backend state
        bool info = false;
        bool presents = false;
        bool direct = false;
        bool noFields = false;
        uint32_t directEvery = 1;
        std::string directDir;
        bool states = false;
        bool offsets = false;   // investigation: how the half-line field offset is applied (see reportOffsets)
        bool extent = false;    // investigation (task w1): how far the clip's primitives reach outside x 0..639 (see reportExtent)
        bool prims = false;     // investigation (task g4b): which primitive kinds the clip draws and how its sprites are scaled (see reportPrims)
        bool vsScale1 = false;  // task g4b: also run a scale-1 GPU renderer and compare the scaled pictures, averaged down, with its pictures
        int onlyState = -1;     // debugging: draw only primitives of this recorded state id
        int skipState = -1;     // debugging: skip primitives of this recorded state id
    };

    void usage()
    {
        std::fprintf(stderr,
                     "usage: ps2x_gs_replay <file> [--backend cpu|gpu] [--compare] [--driver D] [--scale N] [--gpu-debug]\n"
                     "                             [--png-dir D] [--png-every N] [--diff-dir D] [--diff-worst N] [--diff-every N]\n"
                     "                             [--json F] [--threads N] [--defer 0|1] [--hashes F] [--max-markers N]\n"
                     "                             [--drop-state LIST] [--info] [--presents] [--offsets] [--fields weave|progressive]\n"
                     "                             [--direct [--direct-every N] [--direct-dir D]] [--no-fields]\n"
                     "  --backend B     the renderer to replay through: cpu (default) or gpu (SDL_GPU, headless)\n"
                     "  --compare       replay through both and measure how the pictures differ\n"
                     "  --driver D      SDL_GPU driver for the gpu renderer: vulkan, direct3d12 or metal (default: SDL picks)\n"
                     "  --scale N       internal resolution of the gpu renderer, 1 to 4 (weave mode draws at 1 only); its pictures are 640N pixels wide\n"
                     "  --compare-rows R  with --compare --fields progressive: how the 448-line GPU picture is reduced to the cpu picture's 224 lines:\n"
                     "                  parity (default): the rows that sample the field the way the cpu renderer does (even rows for a field whose OFY is a\n"
                     "                  whole number, odd rows for the half-line one), or average: pairs of rows averaged\n"
                     "  --fields F      the gpu renderer's field mode: weave (default; the M3 picture, comparable with the cpu renderer) or\n"
                     "                  progressive (every field a 640x448 picture; --compare then averages row pairs down first)\n"
                     "  --gpu-debug     ask SDL_GPU for its validation layers\n"
                     "  --png-dir D     write the picture of every Nth marker to D/marker_NNNNN.png (with --compare: also D/gpu_marker_NNNNN.png)\n"
                     "  --png-every N   with --png-dir: every Nth marker (default 30)\n"
                     "  --diff-dir D    with --compare: write the CPU, GPU and an amplified difference picture of the worst --diff-worst\n"
                     "                  frames (default 8) and of every --diff-every th marker (default 60) to D\n"
                     "  --json F        with --compare or --backend gpu: write a JSON summary\n"
                     "  --threads N     CPU raster threads, including the caller (PS2X_GS_THREADS; 1 = no pool, nothing queued)\n"
                     "  --defer 0|1     0 draws every primitive at once on the calling thread (PS2X_GS_DEFER=0)\n"
                     "  --hashes F      write one line per marker with the replayed hashes, to compare two runs\n"
                     "  --max-markers N stop after N markers\n"
                     "  --drop-state L  debugging: start without part of the recorded backend state (comma list of\n"
                     "                  clut, pagecache, transfer) to see whether the replay still matches\n"
                     "  --direct        with --backend gpu: also run the direct presentation path and compare its picture with the\n"
                     "                  readback path's, byte for byte (see the top of this file)\n"
                     "  --no-fields     the gpu renderer reads the frame targets when Present is called, as in M2, instead of\n"
                     "                  using the field copies it takes at each marker\n"
                     "  --direct-every N  consume only every Nth marker, as a slow host would (default 1)\n"
                     "  --direct-dir D  write the pictures of the first differing markers to D\n"
                     "                  (with --direct, --png-dir also gets direct_marker_NNNNN.png: the picture path B builds, at its own size)\n"
                     "  --info          list the records in the file and stop\n"
                     "  --presents      list the distinct presentation requests in the file and stop\n"
                     "  --offsets       list how the half-line field offset is applied in the file and stop\n"
                     "  --prims         list which primitive kinds the file draws and how its sprites are scaled and filtered, and stop\n"
                     "  --wide W:H      widescreen prototype (task w1): the gpu renderer's targets and pictures are wider, as PS2X_GS_WIDE=W:H does in the game\n"
                     "  --extent        list the GS state the clip draws with (scissor, offsets, frame, Z) and how far its primitives reach outside\n"
                     "                  x 0..639 (widescreen investigation, task w1), and stop\n"
                     "  --vs-scale1     with --backend gpu --fields progressive --scale N (N > 1): also run a scale-1 GPU renderer and compare every\n"
                     "                  picture, averaged down by N in both directions, with its scale-1 picture (share of pixels differing by more than\n"
                     "                  8 in any colour channel); --diff-dir D writes the scale-1, averaged and difference pictures of the worst frames\n");
    }

    bool parse(int argc, char **argv, Options &o)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            auto value = [&](const char *name) -> const char *
            {
                if (i + 1 >= argc)
                {
                    std::fprintf(stderr, "%s needs a value\n", name);
                    return nullptr;
                }
                return argv[++i];
            };
            if (arg == "--png-dir")
            {
                const char *v = value("--png-dir");
                if (!v) return false;
                o.pngDir = v;
            }
            else if (arg == "--png-every")
            {
                const char *v = value("--png-every");
                if (!v) return false;
                o.pngEvery = static_cast<uint32_t>(std::max(1l, std::strtol(v, nullptr, 10)));
            }
            else if (arg == "--backend")
            {
                const char *v = value("--backend");
                if (!v) return false;
                o.backend = v;
                if (o.backend != "cpu" && o.backend != "gpu")
                {
                    std::fprintf(stderr, "--backend must be cpu or gpu\n");
                    return false;
                }
            }
            else if (arg == "--fields")
            {
                const char *v = value("--fields");
                if (!v) return false;
                o.fields = v;
                if (o.fields != "weave" && o.fields != "progressive")
                {
                    std::fprintf(stderr, "--fields must be weave or progressive\n");
                    return false;
                }
            }
            else if (arg == "--compare-rows")
            {
                const char *v = value("--compare-rows");
                if (!v) return false;
                o.compareRows = v;
                if (o.compareRows != "parity" && o.compareRows != "average")
                {
                    std::fprintf(stderr, "--compare-rows must be parity or average\n");
                    return false;
                }
            }
            else if (arg == "--compare")
            {
                o.compare = true;
            }
            else if (arg == "--driver")
            {
                const char *v = value("--driver");
                if (!v) return false;
                o.driver = v;
            }
            else if (arg == "--scale")
            {
                const char *v = value("--scale");
                if (!v) return false;
                o.scale = static_cast<uint32_t>(std::max(1l, std::strtol(v, nullptr, 10)));
            }
            else if (arg == "--gpu-debug")
            {
                o.gpuDebug = true;
            }
            else if (arg == "--diff-dir")
            {
                const char *v = value("--diff-dir");
                if (!v) return false;
                o.diffDir = v;
            }
            else if (arg == "--diff-worst")
            {
                const char *v = value("--diff-worst");
                if (!v) return false;
                o.diffWorst = static_cast<uint32_t>(std::max(0l, std::strtol(v, nullptr, 10)));
            }
            else if (arg == "--diff-every")
            {
                const char *v = value("--diff-every");
                if (!v) return false;
                o.diffEvery = static_cast<uint32_t>(std::max(0l, std::strtol(v, nullptr, 10)));
            }
            else if (arg == "--json")
            {
                const char *v = value("--json");
                if (!v) return false;
                o.jsonFile = v;
            }
            else if (arg == "--threads")
            {
                const char *v = value("--threads");
                if (!v) return false;
                o.threads = static_cast<int>(std::strtol(v, nullptr, 10));
            }
            else if (arg == "--defer")
            {
                const char *v = value("--defer");
                if (!v) return false;
                o.defer = static_cast<int>(std::strtol(v, nullptr, 10));
            }
            else if (arg == "--hashes")
            {
                const char *v = value("--hashes");
                if (!v) return false;
                o.hashesFile = v;
            }
            else if (arg == "--max-markers")
            {
                const char *v = value("--max-markers");
                if (!v) return false;
                o.maxMarkers = static_cast<uint64_t>(std::strtoull(v, nullptr, 10));
            }
            else if (arg == "--drop-state")
            {
                const char *v = value("--drop-state");
                if (!v) return false;
                o.dropState = v;
            }
            else if (arg == "--direct")
            {
                o.direct = true;
            }
            else if (arg == "--no-fields")
            {
                o.noFields = true;
            }
            else if (arg == "--direct-every")
            {
                const char *v = value("--direct-every");
                if (!v) return false;
                o.directEvery = static_cast<uint32_t>(std::max(1l, std::strtol(v, nullptr, 10)));
            }
            else if (arg == "--direct-dir")
            {
                const char *v = value("--direct-dir");
                if (!v) return false;
                o.directDir = v;
            }
            else if (arg == "--info")
            {
                o.info = true;
            }
            else if (arg == "--presents")
            {
                o.presents = true;
            }
            else if (arg == "--states")
            {
                o.states = true;
            }
            else if (arg == "--offsets")
            {
                o.offsets = true;
            }
            else if (arg == "--prims")
            {
                o.prims = true;
            }
            else if (arg == "--extent")
            {
                o.extent = true;
            }
            else if (arg == "--wide")
            {
                const char *v = value("--wide");
                if (!v) return false;
                setEnv("PS2X_GS_WIDE", v); // read by the GPU renderer when it is created (widescreen prototype, task w1)
            }
            else if (arg == "--vs-scale1")
            {
                o.vsScale1 = true;
            }
            else if (arg == "--only-state")
            {
                const char *v = value("--only-state");
                if (!v) return false;
                o.onlyState = static_cast<int>(std::strtol(v, nullptr, 10));
            }
            else if (arg == "--skip-state")
            {
                const char *v = value("--skip-state");
                if (!v) return false;
                o.skipState = static_cast<int>(std::strtol(v, nullptr, 10));
            }
            else if (!arg.empty() && arg[0] == '-')
            {
                std::fprintf(stderr, "unknown option %s\n", arg.c_str());
                return false;
            }
            else if (o.file.empty())
            {
                o.file = arg;
            }
            else
            {
                std::fprintf(stderr, "more than one file given\n");
                return false;
            }
        }
        return !o.file.empty();
    }

    // What the replay produced for one marker.
    struct Replayed
    {
        uint64_t frameHash = 0;
        uint32_t width = 0, height = 0;
        uint64_t displayHash = 0;
        bool done = false;
    };
    struct LiveFrame
    {
        uint32_t width = 0, height = 0;
        uint64_t hash = 0;
    };

    // One renderer being driven: its own copy of GS memory, the calls timed.
    struct Side
    {
        const char *name = "";
        std::vector<uint8_t> vram;
        GSRasterBackend *backend = nullptr;
        GSCpuBackend *cpu = nullptr;
        GSGpuBackend *gpu = nullptr;
        double seconds = 0.0; // time spent in calls (and the waits at markers)
        double markSeconds = 0.0; // of which MarkPresentation alone
        uint64_t callChecks = 0, callMismatches = 0;
        std::vector<std::string> callProblems;
    };

    // One picture of a marker, as RGBA rows `stride` pixels apart (kStride, except for the GPU renderer's pictures at a scale above 1).
    struct Picture
    {
        std::vector<uint8_t> pixels;
        uint32_t width = 0, height = 0;
        uint32_t stride = kStride;
    };

    // Plays one record into one side. Markers are handled by the caller.
    void applyRecord(Side &side, const GSRec::Record &rec)
    {
        GSRasterBackend &b = *side.backend;
        const auto t0 = Clock::now();
        switch (rec.type)
        {
        case GSRec::RT_Submit:
            b.Submit(rec.batch);
            break;
        case GSRec::RT_LoadClut:
            b.LoadClut(rec.tex0, rec.texclut);
            break;
        case GSRec::RT_BeginTransfer:
            b.BeginTransfer(rec.transfer);
            break;
        case GSRec::RT_UploadImage:
            b.UploadImage(rec.data, rec.size);
            break;
        case GSRec::RT_Flush:
            b.Flush();
            break;
        case GSRec::RT_TextureFlush:
            b.TextureFlush();
            break;
        case GSRec::RT_Sync:
            b.Sync(rec.syncReason);
            break;
        case GSRec::RT_ClearFramebuffer:
        {
            const bool got = b.ClearFramebuffer(rec.context, rec.rgba);
            ++side.callChecks;
            if (got != rec.result)
            {
                ++side.callMismatches;
                side.callProblems.push_back("ClearFramebuffer returned " + std::to_string(got) + ", the game saw " + std::to_string(rec.result));
            }
            break;
        }
        case GSRec::RT_WriteVram:
            b.WriteVram(rec.psm, rec.base, rec.bw, rec.x, rec.y, rec.value);
            break;
        case GSRec::RT_Reset:
            b.Reset();
            break;
        case GSRec::RT_ReadVram:
        {
            const uint32_t got = b.ReadVram(rec.psm, rec.base, rec.bw, rec.x, rec.y);
            ++side.callChecks;
            if (got != rec.value)
            {
                ++side.callMismatches;
                side.callProblems.push_back("ReadVram returned " + std::to_string(got) + ", the game saw " + std::to_string(rec.value));
            }
            break;
        }
        case GSRec::RT_ConsumeLocalToHost:
        {
            std::vector<uint8_t> buffer(std::max<uint32_t>(rec.maxBytes, 1u));
            const uint32_t got = b.ConsumeLocalToHostBytes(buffer.data(), rec.maxBytes);
            ++side.callChecks;
            if (got != rec.size || (got != 0 && std::memcmp(buffer.data(), rec.data, got) != 0))
            {
                ++side.callMismatches;
                side.callProblems.push_back("ConsumeLocalToHostBytes returned " + std::to_string(got) + " bytes, the game saw " + std::to_string(rec.size));
            }
            break;
        }
        default:
            break;
        }
        side.seconds += std::chrono::duration<double>(Clock::now() - t0).count();
    }

    // A marker: the renderer is told, waits until the picture of this moment exists, and Present returns it.
    PresentationFrame playMarker(Side &side, const GSRec::Record &rec)
    {
        const auto t0 = Clock::now();
        if (side.cpu)
            side.cpu->MarkPresentation(rec.request);
        else
            side.gpu->MarkPresentation(rec.request);
        side.markSeconds += std::chrono::duration<double>(Clock::now() - t0).count();
        side.backend->Sync(GSSyncReason::Presentation); // waits until the drawer has copied VRAM / the GPU is done
        PresentationFrame frame = side.backend->Present(rec.request);
        side.seconds += std::chrono::duration<double>(Clock::now() - t0).count();
        return frame;
    }

    struct DiffResult
    {
        double share = 0.0; // fraction of pixels where any channel differs by more than kPixelTolerance
        int maxDiff = 0;
        uint64_t differing = 0;
        uint64_t pixels = 0;
    };

    // `tolerance` and `channels` (4: alpha counts, 3: colour only) default to the --compare rule.
    DiffResult diffPictures(const Picture &a, const Picture &b, int tolerance = kPixelTolerance, int channels = 4)
    {
        DiffResult r;
        const uint32_t w = std::min(a.width, b.width), h = std::min(a.height, b.height);
        if (a.width != b.width || a.height != b.height)
        {
            r.share = 1.0;
            r.maxDiff = 255;
            r.pixels = static_cast<uint64_t>(std::max(a.width, b.width)) * std::max(a.height, b.height);
            r.differing = r.pixels;
            return r;
        }
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t *pa = a.pixels.data() + static_cast<size_t>(y) * a.stride * 4u;
            const uint8_t *pb = b.pixels.data() + static_cast<size_t>(y) * b.stride * 4u;
            for (uint32_t x = 0; x < w; ++x)
            {
                int worst = 0;
                for (int c = 0; c < channels; ++c)
                    worst = std::max(worst, std::abs(static_cast<int>(pa[x * 4u + c]) - static_cast<int>(pb[x * 4u + c])));
                r.maxDiff = std::max(r.maxDiff, worst);
                r.differing += worst > tolerance ? 1u : 0u;
            }
        }
        r.pixels = static_cast<uint64_t>(w) * h;
        r.share = r.pixels ? static_cast<double>(r.differing) / static_cast<double>(r.pixels) : 0.0;
        return r;
    }

    // Amplified absolute difference (8x per channel), with pixels over the tolerance forced to at least 160 in red.
    Picture makeDiffPicture(const Picture &a, const Picture &b)
    {
        Picture d;
        d.width = std::min(a.width, b.width);
        d.height = std::min(a.height, b.height);
        d.stride = d.width;
        d.pixels.assign(static_cast<size_t>(d.stride) * d.height * 4u, 255u);
        for (uint32_t y = 0; y < d.height; ++y)
            for (uint32_t x = 0; x < d.width; ++x)
            {
                const uint8_t *pa = a.pixels.data() + (static_cast<size_t>(y) * a.stride + x) * 4u;
                const uint8_t *pb = b.pixels.data() + (static_cast<size_t>(y) * b.stride + x) * 4u;
                uint8_t *pd = d.pixels.data() + (static_cast<size_t>(y) * d.stride + x) * 4u;
                int worst = 0;
                for (int c = 0; c < 3; ++c)
                {
                    const int diff = std::abs(static_cast<int>(pa[c]) - static_cast<int>(pb[c]));
                    worst = std::max(worst, diff);
                    pd[c] = static_cast<uint8_t>(std::min(255, diff * 8));
                }
                if (worst > kPixelTolerance)
                    pd[0] = std::max<uint8_t>(pd[0], 160);
                pd[3] = 255;
            }
        return d;
    }

    // The GPU renderer's progressive picture has twice the rows of the CPU renderer's field picture: average each pair of rows.
    Picture halveRows(const Picture &p)
    {
        Picture r;
        r.width = p.width;
        r.height = p.height / 2u;
        r.pixels.assign(p.pixels.size(), 0u);
        for (uint32_t y = 0; y < r.height; ++y)
        {
            const uint8_t *a = p.pixels.data() + static_cast<size_t>(2u * y) * kStride * 4u;
            const uint8_t *b = a + static_cast<size_t>(kStride) * 4u;
            uint8_t *d = r.pixels.data() + static_cast<size_t>(y) * kStride * 4u;
            for (uint32_t i = 0; i < p.width * 4u; ++i)
                d[i] = static_cast<uint8_t>((static_cast<unsigned>(a[i]) + b[i] + 1u) / 2u);
        }
        return r;
    }

    // The rows 2y + parity of a progressive picture: with parity 0 the rows that sample the field at whole GS lines (what the CPU
    // renderer does for a field whose OFY is a whole number), with parity 1 the half-line lower ones (the other kind of field).
    Picture pickRows(const Picture &p, unsigned parity)
    {
        Picture r;
        r.width = p.width;
        r.height = p.height / 2u;
        r.pixels.assign(p.pixels.size(), 0u);
        for (uint32_t y = 0; y < r.height; ++y)
            std::memcpy(r.pixels.data() + static_cast<size_t>(y) * kStride * 4u,
                        p.pixels.data() + static_cast<size_t>(2u * y + parity) * kStride * 4u, static_cast<size_t>(p.width) * 4u);
        return r;
    }

    Picture toPicture(const PresentationFrame &frame, uint32_t stride = kStride)
    {
        Picture p;
        p.width = frame.width;
        p.height = frame.height;
        p.stride = stride;
        if (frame)
            p.pixels = frame.pixels;
        return p;
    }

    bool writePicture(const std::string &path, const Picture &p)
    {
        if (p.pixels.empty() || p.width == 0 || p.height == 0)
            return false;
        return writePng(path, p.pixels.data(), p.width, p.height, p.stride * 4u);
    }

    // The average of every n x n block of pixels of a scaled picture, one pixel of the result per block, with the blocks'
    // top-left corners `shift` pixels up and to the left of multiples of n (the blocks at the border are clipped to the
    // picture). The result is a picture of width / n x height / n. All four channels are averaged, rounding to nearest.
    // With `pick` set the result takes the one pixel at each block's top-left corner (n x + 0, n y + 0 before the shift) instead of
    // the average: the target pixels that sample the same GS position as the scale-1 picture's pixels do (TargetMapping, corner grid).
    // `pickOffset` moves the picked pixel that many pixels right and down (n / 2 is half a scale-1 pixel off the scale-1 sample position:
    // how much the scale-1 picture changes when its samples are taken half a pixel away, to compare the averages with).
    Picture boxDown(const Picture &p, uint32_t n, uint32_t shift, bool pick = false, uint32_t pickOffset = 0)
    {
        Picture r;
        r.width = p.width / n;
        r.height = p.height / n;
        r.stride = r.width;
        r.pixels.assign(static_cast<size_t>(r.stride) * r.height * 4u, 0u);
        for (uint32_t y = 0; y < r.height; ++y)
            for (uint32_t x = 0; x < r.width; ++x)
            {
                uint32_t sum[4] = {};
                uint32_t count = 0;
                for (uint32_t ky = 0; ky < (pick ? 1u : n); ++ky)
                    for (uint32_t kx = 0; kx < (pick ? 1u : n); ++kx)
                    {
                        const int64_t sx = static_cast<int64_t>(x) * n + kx - shift + pickOffset, sy = static_cast<int64_t>(y) * n + ky - shift + pickOffset;
                        if (sx < 0 || sy < 0 || sx >= static_cast<int64_t>(p.width) || sy >= static_cast<int64_t>(p.height))
                            continue;
                        const uint8_t *src = p.pixels.data() + (static_cast<size_t>(sy) * p.stride + static_cast<size_t>(sx)) * 4u;
                        for (int c = 0; c < 4; ++c)
                            sum[c] += src[c];
                        ++count;
                    }
                uint8_t *dst = r.pixels.data() + (static_cast<size_t>(y) * r.stride + x) * 4u;
                for (int c = 0; c < 4; ++c)
                    dst[c] = count ? static_cast<uint8_t>((sum[c] + count / 2u) / count) : 0u;
            }
        return r;
    }

    std::string clipName(const std::string &path)
    {
        const size_t slash = path.find_last_of("/\\");
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);
        return name;
    }

    double percentile(std::vector<double> v, double p)
    {
        if (v.empty())
            return 0.0;
        std::sort(v.begin(), v.end());
        const double pos = p * static_cast<double>(v.size() - 1);
        const size_t lo = static_cast<size_t>(pos);
        const size_t hi = std::min(lo + 1, v.size() - 1);
        return v[lo] + (v[hi] - v[lo]) * (pos - static_cast<double>(lo));
    }

    struct WorstFrame
    {
        double share = 0.0;
        int maxDiff = 0;
        uint32_t marker = 0;
        Picture cpu, gpu;
    };

    // --offsets (investigation for the progressive-field mode, task g4a): where does the half-line difference between the
    // game's two fields live? The game draws one 224-line field per VBlank into fbp 0 or fbp 70, and every other field is
    // "half a line lower" (OFY 1936 against 1936.5, according to bringup.md). This reads the Submit records and reports
    //   1. the XYOFFSET of every primitive, by frame buffer, primitive kind and context (CTXT);
    //   2. the scissor boxes, by frame buffer;
    //   3. the fractional part (in 1/16) of every vertex Y before and after the offset is subtracted, by frame buffer;
    //   4. for the segments between markers (one segment = one field): the frame buffer and offsets they draw with, and which
    //      frame buffer the marker after them displays;
    //   5. for consecutive fields with the same number of primitives, which members of the draw state differ between the
    //      primitives at the same position, and how the geometry differs (identical after the offset is subtracted, shifted
    //      by exactly half a line, or something else).
    struct OffsetPrim
    {
        GSDrawState st;
        int kind = 0;              // 0 sprite, 1 triangle
        int32_t rawY16[3] = {};    // vertex Y in 1/16 pixel as the game sent it (includes the offset)
        int32_t effY16[3] = {};    // ... after the offset is subtracted
        int32_t effX16[3] = {};
        int count = 0;
        std::array<GSVertex, 3> vertices{};
    };

    // --prims (investigation for the internal-resolution work, task g4b): which primitive kinds the clip draws, and for the
    // sprites, whether they show their texels 1:1, magnified or minified, with nearest or bilinear filtering. Lines and points
    // stay one target pixel thin at a higher scale, so where they are used matters; so does the share of magnified bilinear
    // sprites, which a higher scale can sample more finely. A sprite is 1:1 when a texel is one GS pixel on both axes (within
    // 1/32), magnified when a texel covers more than one pixel on an axis and none less, minified when it covers less on an
    // axis and none more, and mixed otherwise. The pixel columns count the sprite's area (width x height in GS pixels).
    int reportPrims(GSRec::Reader &reader)
    {
        static const char *const kType[] = {"POINT", "LINE", "LINESTRIP", "TRIANGLE", "TRISTRIP", "TRIFAN", "SPRITE"};
        uint64_t perType[7] = {};
        uint64_t drawn = 0;
        struct Bucket
        {
            uint64_t prims = 0, pixels = 0;
        };
        // [textured: 0 no / 1 yes][filter: 0 nearest / 1 linear][class: 0 1:1, 1 magnified, 2 minified, 3 mixed]
        Bucket sprites[2][2][4] = {};
        Bucket triangles[2][2] = {};                // [textured][filter]
        std::map<std::string, uint64_t> lineStates; // where lines and points are drawn
        std::map<std::string, uint64_t> clampKinds; // wrap modes of the textured sprites: WMS/WMT
        GSRec::Record r;
        while (reader.next(r) == GSRec::Reader::Status::Ok)
        {
            if (r.type != GSRec::RT_Submit)
                continue;
            const GSDrawState &st = r.batch.state;
            const unsigned type = std::min<unsigned>(st.prim.type, 6);
            ++perType[type];
            ++drawn;
            const int textured = st.prim.tme ? 1 : 0;
            const int linear = st.linearFilter ? 1 : 0;
            if (type == GS_PRIM_POINT || type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP)
            {
                char key[160];
                std::snprintf(key, sizeof(key), "%-9s fbp %u tex %s abe %d", kType[type], st.context.frame.fbp, textured ? "on" : "off", st.prim.abe ? 1 : 0);
                ++lineStates[key];
            }
            else if (type == GS_PRIM_SPRITE)
            {
                GSGpu::Setup setup;
                if (r.batch.vertexCount < 2 || !GSGpu::BuildSprite(r.batch, 1u, setup))
                    continue;
                const float spanX = setup.v[1].x - setup.v[0].x, spanY = setup.v[2].y - setup.v[1].y;
                int cls = 0;
                if (textured)
                {
                    const float texX = std::fabs(setup.v[1].u - setup.v[0].u), texY = std::fabs(setup.v[2].v - setup.v[1].v);
                    const float rx = texX / std::max(spanX, 1.0f), ry = texY / std::max(spanY, 1.0f); // texels per pixel
                    const bool oneX = std::fabs(rx - 1.0f) < 1.0f / 32.0f, oneY = std::fabs(ry - 1.0f) < 1.0f / 32.0f;
                    const bool magX = rx < 1.0f - 1.0f / 32.0f, magY = ry < 1.0f - 1.0f / 32.0f;
                    const bool minX = rx > 1.0f + 1.0f / 32.0f, minY = ry > 1.0f + 1.0f / 32.0f;
                    if (oneX && oneY)
                        cls = 0;
                    else if ((magX || magY) && !(minX || minY))
                        cls = 1;
                    else if ((minX || minY) && !(magX || magY))
                        cls = 2;
                    else
                        cls = 3;
                    char key[96];
                    std::snprintf(key, sizeof(key), "WMS %llu WMT %llu", (unsigned long long)(st.context.clamp & 3), (unsigned long long)((st.context.clamp >> 2) & 3));
                    ++clampKinds[key];
                }
                Bucket &b = sprites[textured][linear][cls];
                ++b.prims;
                b.pixels += static_cast<uint64_t>(std::max(spanX, 1.0f) * std::max(spanY, 1.0f));
            }
            else
            {
                ++triangles[textured][linear].prims;
            }
        }
        std::printf("primitives drawn: %llu\n", static_cast<unsigned long long>(drawn));
        for (unsigned t = 0; t < 7; ++t)
            std::printf("  %-10s %llu\n", kType[t], static_cast<unsigned long long>(perType[t]));
        static const char *const kClass[] = {"1:1", "magnified", "minified", "mixed"};
        uint64_t spriteTotal = 0, spritePixels = 0;
        for (int t = 0; t < 2; ++t)
            for (int f = 0; f < 2; ++f)
                for (int c = 0; c < 4; ++c)
                {
                    spriteTotal += sprites[t][f][c].prims;
                    spritePixels += sprites[t][f][c].pixels;
                }
        std::printf("sprites: %llu (%llu GS pixels in all)\n", static_cast<unsigned long long>(spriteTotal), static_cast<unsigned long long>(spritePixels));
        for (int t = 1; t >= 0; --t)
            for (int f = 0; f < 2; ++f)
                for (int c = 0; c < 4; ++c)
                {
                    const Bucket &b = sprites[t][f][c];
                    if (!b.prims)
                        continue;
                    std::printf("  %-10s %-8s %-9s %9llu sprites (%5.1f%%) %12llu pixels (%5.1f%%)\n", t ? "textured" : "untextured", f ? "bilinear" : "nearest",
                                t ? kClass[c] : "-", static_cast<unsigned long long>(b.prims),
                                100.0 * static_cast<double>(b.prims) / std::max<double>(1.0, static_cast<double>(spriteTotal)),
                                static_cast<unsigned long long>(b.pixels),
                                100.0 * static_cast<double>(b.pixels) / std::max<double>(1.0, static_cast<double>(spritePixels)));
                }
        for (const auto &e : clampKinds)
            std::printf("  textured sprites with %s: %llu\n", e.first.c_str(), static_cast<unsigned long long>(e.second));
        std::printf("triangles: untextured %llu, textured nearest %llu, textured bilinear %llu\n",
                    static_cast<unsigned long long>(triangles[0][0].prims + triangles[0][1].prims), static_cast<unsigned long long>(triangles[1][0].prims),
                    static_cast<unsigned long long>(triangles[1][1].prims));
        if (lineStates.empty())
            std::printf("lines and points: none\n");
        for (const auto &e : lineStates)
            std::printf("lines and points: %s : %llu\n", e.first.c_str(), static_cast<unsigned long long>(e.second));
        return 0;
    }


    // --extent (widescreen investigation, task w1): what state the clip draws with, and how far its primitives reach outside
    // the 640-wide screen. For every primitive the screen-space bounding box is built the way the GPU renderer builds it
    // (gs_gpu_math.h; X is the GS position minus OFX, so the screen is x 0..639), and the primitive is put in one class:
    // inside, straddling the left edge, straddling the right edge, spanning both edges, entirely left, entirely right. The
    // same is repeated for the scissor, which is what the GS itself keeps: a primitive entirely outside the scissor draws
    // nothing. The report also lists the distinct SCISSOR, XYOFFSET, FRAME, ZBUF and PRIM attributes (primitives counted),
    // and how far out the primitives that touch the screen reach (the histogram bins are in GS pixels beyond the edge).
    int reportExtent(GSRec::Reader &reader)
    {
        static const char *const kType[] = {"POINT", "LINE", "LINESTRIP", "TRIANGLE", "TRISTRIP", "TRIFAN", "SPRITE"};
        static const char *const kClass[] = {"inside", "left edge", "right edge", "both edges", "all left", "all right"};
        struct Counts
        {
            uint64_t n[6] = {};
        };
        Counts byKind[2][2];                         // [sprite 0 / triangle 1][untextured 0 / textured 1]
        uint64_t total = 0, markers = 0, inScissor = 0, emptyScissor = 0;
        std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, uint64_t> scissors;                           // x0 x1 y0 y1
        std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, uint64_t> frames;                             // fbp fbw psm zbp
        std::map<std::tuple<uint32_t, uint32_t>, uint64_t> offsets;                                                // ofx ofy
        std::map<std::string, uint64_t> prims;
        const int32_t kBins[] = {1, 20, 40, 80, 160, 320, 640, 1280, 100000};
        uint64_t leftReach[9] = {}, rightReach[9] = {}; // by how far a straddling primitive's far corner is beyond the edge
        int32_t minOfAll = 0, maxOfAll = 639;
        uint64_t outsideThisMarker = 0, outsideMarkers = 0, maxOutsideInMarker = 0;
        std::map<uint32_t, uint64_t> widthHist; // bounding-box width of triangles, in 80 pixel bins: how large single prims are
        // The 2D elements of a 3D scene (the HUD): textured primitives with UV coordinates (FST) and flat shading, as opposed to the
        // perspective (STQ) prims of the 3D world. Counted by the screen x range they cover, in 40 pixel bins by their left edge.
        std::map<int32_t, uint64_t> hudByLeft;
        uint64_t hudPrims = 0;
        int32_t hudMin = 100000, hudMax = -100000;
        GSRec::Record r;
        while (reader.next(r) == GSRec::Reader::Status::Ok)
        {
            if (r.type == GSRec::RT_Marker)
            {
                ++markers;
                if (outsideThisMarker)
                    ++outsideMarkers;
                maxOutsideInMarker = std::max(maxOutsideInMarker, outsideThisMarker);
                outsideThisMarker = 0;
                continue;
            }
            if (r.type != GSRec::RT_Submit)
                continue;
            const GSDrawState &st = r.batch.state;
            const unsigned type = std::min<unsigned>(st.prim.type, 6);
            const GSContext &c = st.context;
            ++scissors[{c.scissor.x0, c.scissor.x1, c.scissor.y0, c.scissor.y1}];
            ++frames[{c.frame.fbp, c.frame.fbw, c.frame.psm, c.zbuf.zbp}];
            ++offsets[{c.xyoffset.ofx, c.xyoffset.ofy}];
            {
                char key[160];
                std::snprintf(key, sizeof(key), "%-9s iip %d tme %d fst %d abe %d fge %d aa1 %d ctxt %d", kType[type], st.prim.iip, st.prim.tme, st.prim.fst, st.prim.abe, st.prim.fge,
                              st.prim.aa1, st.prim.ctxt);
                ++prims[key];
            }
            if (type == GS_PRIM_POINT || type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP)
                continue;
            GSGpu::Setup setup;
            const bool sprite = type == GS_PRIM_SPRITE;
            if (sprite ? (r.batch.vertexCount < 2 || !GSGpu::BuildSprite(r.batch, GSGpu::TargetMapping{1, 1, false}, setup))
                       : !GSGpu::BuildTriangle(r.batch, GSGpu::TargetMapping{1, 1, false}, setup))
                continue;
            ++total;
            const int32_t minX = setup.minX, maxX = setup.maxX;
            int cls;
            if (maxX < 0)
                cls = 4;
            else if (minX > 639)
                cls = 5;
            else if (minX < 0 && maxX > 639)
                cls = 3;
            else if (minX < 0)
                cls = 1;
            else if (maxX > 639)
                cls = 2;
            else
                cls = 0;
            ++byKind[sprite ? 0 : 1][st.prim.tme ? 1 : 0].n[cls];
            if (!sprite && st.prim.tme && st.prim.fst && !st.prim.iip)
            {
                ++hudPrims;
                hudMin = std::min(hudMin, minX);
                hudMax = std::max(hudMax, maxX);
                ++hudByLeft[(minX >= 0 ? minX / 40 : -1) * 40];
            }
            if (cls != 0)
                ++outsideThisMarker;
            minOfAll = std::min(minOfAll, minX);
            maxOfAll = std::max(maxOfAll, maxX);
            if (!sprite)
                ++widthHist[static_cast<uint32_t>(std::max<int32_t>(0, std::min<int32_t>(maxX - minX, 4000)) / 80)];
            // scissor test, as the GPU renderer's BoxOutsideScissor
            if (GSGpu::BoxOutsideScissor(setup, c.scissor))
                ++emptyScissor;
            else
                ++inScissor;
            if (minX < 0 && maxX >= 0)
            {
                for (int b = 0; b < 9; ++b)
                    if (-minX <= kBins[b] || b == 8)
                    {
                        ++leftReach[b];
                        break;
                    }
            }
            if (maxX > 639 && minX <= 639)
            {
                for (int b = 0; b < 9; ++b)
                    if (maxX - 639 <= kBins[b] || b == 8)
                    {
                        ++rightReach[b];
                        break;
                    }
            }
        }
        std::printf("markers: %llu; triangles and sprites with area: %llu\n", static_cast<unsigned long long>(markers), static_cast<unsigned long long>(total));
        std::printf("SCISSOR x0 x1 y0 y1 (prims):\n");
        for (const auto &e : scissors)
            std::printf("  %u %u %u %u : %llu\n", std::get<0>(e.first), std::get<1>(e.first), std::get<2>(e.first), std::get<3>(e.first), static_cast<unsigned long long>(e.second));
        std::printf("XYOFFSET ofx ofy (1/16 pixel; prims):\n");
        for (const auto &e : offsets)
            std::printf("  %u (%.4f) %u (%.4f) : %llu\n", std::get<0>(e.first), std::get<0>(e.first) / 16.0, std::get<1>(e.first), std::get<1>(e.first) / 16.0, static_cast<unsigned long long>(e.second));
        std::printf("FRAME fbp fbw psm / ZBUF zbp (prims):\n");
        for (const auto &e : frames)
            std::printf("  fbp %u fbw %u psm 0x%02x zbp %u : %llu\n", std::get<0>(e.first), std::get<1>(e.first), std::get<2>(e.first), std::get<3>(e.first), static_cast<unsigned long long>(e.second));
        std::printf("PRIM attributes (prims):\n");
        for (const auto &e : prims)
            std::printf("  %s : %llu\n", e.first.c_str(), static_cast<unsigned long long>(e.second));
        std::printf("screen x extent of all primitives: %d .. %d (screen is 0..639)\n", minOfAll, maxOfAll);
        std::printf("class by screen position (x 0..639) of the bounding box:\n");
        std::printf("  %-22s", "");
        for (int cl = 0; cl < 6; ++cl)
            std::printf(" %10s", kClass[cl]);
        std::printf("\n");
        for (int k = 0; k < 2; ++k)
            for (int t = 0; t < 2; ++t)
            {
                std::printf("  %-9s %-12s", k ? "triangle" : "sprite", t ? "textured" : "untextured");
                for (int cl = 0; cl < 6; ++cl)
                    std::printf(" %10llu", static_cast<unsigned long long>(byKind[k][t].n[cl]));
                std::printf("\n");
            }
        uint64_t outside = 0, entirelyOut = 0;
        for (int k = 0; k < 2; ++k)
            for (int t = 0; t < 2; ++t)
                for (int cl = 1; cl < 6; ++cl)
                {
                    outside += byKind[k][t].n[cl];
                    if (cl >= 4)
                        entirelyOut += byKind[k][t].n[cl];
                }
        std::printf("primitives not entirely inside x 0..639: %llu of %llu (%.1f%%); entirely outside it: %llu (%.1f%%)\n", static_cast<unsigned long long>(outside),
                    static_cast<unsigned long long>(total), 100.0 * static_cast<double>(outside) / std::max<double>(1.0, static_cast<double>(total)),
                    static_cast<unsigned long long>(entirelyOut), 100.0 * static_cast<double>(entirelyOut) / std::max<double>(1.0, static_cast<double>(total)));
        std::printf("with the clip's scissor: %llu primitives have a box that reaches it, %llu are entirely outside it (the GS draws nothing for those)\n",
                    static_cast<unsigned long long>(inScissor), static_cast<unsigned long long>(emptyScissor));
        std::printf("fields (markers) with at least one primitive not inside x 0..639: %llu of %llu; most in one field: %llu\n", static_cast<unsigned long long>(outsideMarkers),
                    static_cast<unsigned long long>(markers), static_cast<unsigned long long>(maxOutsideInMarker));
        std::printf("how far straddling primitives reach beyond an edge (GS pixels), count by bin <=");
        for (int b = 0; b < 8; ++b)
            std::printf(" %d", kBins[b]);
        std::printf(" and more:\n  left : ");
        for (int b = 0; b < 9; ++b)
            std::printf(" %llu", static_cast<unsigned long long>(leftReach[b]));
        std::printf("\n  right: ");
        for (int b = 0; b < 9; ++b)
            std::printf(" %llu", static_cast<unsigned long long>(rightReach[b]));
        std::printf("\n");
        std::printf("2D elements (textured, UV, flat shaded triangle strips): %llu primitives, screen x %d .. %d; by left edge in 40 pixel bins (bin: count):", static_cast<unsigned long long>(hudPrims),
                    hudPrims ? hudMin : 0, hudPrims ? hudMax : 0);
        for (const auto &e : hudByLeft)
            std::printf(" %d:%llu", e.first, static_cast<unsigned long long>(e.second));
        std::printf("\n");
        std::printf("triangle bounding-box width in 80 pixel bins (bin: count):");
        for (const auto &e : widthHist)
            std::printf(" %u:%llu", e.first * 80, static_cast<unsigned long long>(e.second));
        std::printf("\n");
        return 0;
    }

    int reportOffsets(GSRec::Reader &reader)
    {
        using Key4 = std::tuple<uint32_t, uint32_t, uint32_t, int, int>;
        std::map<Key4, uint64_t> offsets; // fbp, ofx, ofy, kind, ctxt
        std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, uint64_t> scissors;
        std::map<std::tuple<uint32_t, int, int, int>, uint64_t> fracs; // fbp, kind, 0 raw / 1 effective, fraction in 1/16
        struct Segment
        {
            std::vector<OffsetPrim> prims;
            std::map<uint32_t, uint64_t> fbps;
            std::map<std::pair<uint32_t, uint32_t>, uint64_t> offs;
            int displayedFbp = -1; // the marker after the segment
        };
        std::vector<Segment> segments(1);
        std::map<std::string, uint64_t> memberDiffs;
        uint64_t pairsCompared = 0, pairsSkipped = 0, primPairs = 0, geomSame = 0, geomHalf = 0, geomOther = 0, geomHalfXs = 0, geomRawSame = 0;
        std::map<int, uint64_t> deltaHist; // effective-Y delta (1/16 units) of the first vertex, between consecutive fields, for prims whose X is unchanged
        uint64_t displayedIsPrevSegment = 0, displayedIsNextSegment = 0, displayedOther = 0;
        std::vector<std::string> tbpSamples;
        std::string pairFlags; // one character per pair of consecutive fields: '=' nothing but the frame buffer and OFY differs, '~' something else, '.' different primitive counts
        const auto sameVertex = [](const GSVertex &a, const GSVertex &b)
        {
            return a.x == b.x && a.y == b.y && a.z == b.z && a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a && a.q == b.q && a.s == b.s && a.t == b.t &&
                   a.u == b.u && a.v == b.v;
        };

        GSRec::Record r;
        while (reader.next(r) == GSRec::Reader::Status::Ok)
        {
            if (r.type == GSRec::RT_Marker)
            {
                segments.back().displayedFbp = static_cast<int>(r.request.dispfb1 & 0x1FFu);
                segments.emplace_back();
                continue;
            }
            if (r.type != GSRec::RT_Submit)
                continue;
            const GSDrawState &st = r.batch.state;
            const GSContext &c = st.context;
            OffsetPrim p;
            p.st = st;
            p.vertices = r.batch.vertices;
            p.kind = st.prim.type == GS_PRIM_SPRITE ? 0 : 1;
            p.count = p.kind == 0 ? 2 : 3;
            for (int i = 0; i < p.count; ++i)
            {
                p.rawY16[i] = static_cast<int32_t>(std::lround(r.batch.vertices[static_cast<size_t>(i)].y * 16.0f));
                p.effY16[i] = p.rawY16[i] - static_cast<int32_t>(c.xyoffset.ofy);
                p.effX16[i] = static_cast<int32_t>(std::lround(r.batch.vertices[static_cast<size_t>(i)].x * 16.0f)) - static_cast<int32_t>(c.xyoffset.ofx);
                ++fracs[{c.frame.fbp, p.kind, 0, p.rawY16[i] & 15}];
                ++fracs[{c.frame.fbp, p.kind, 1, p.effY16[i] & 15}];
            }
            ++offsets[{c.frame.fbp, c.xyoffset.ofx, c.xyoffset.ofy, p.kind, st.prim.ctxt ? 1 : 0}];
            ++scissors[{c.frame.fbp, c.scissor.x0, c.scissor.x1, c.scissor.y0, c.scissor.y1}];
            Segment &seg = segments.back();
            ++seg.fbps[c.frame.fbp];
            ++seg.offs[{c.xyoffset.ofx, c.xyoffset.ofy}];
            seg.prims.push_back(std::move(p));
        }

        std::printf("== XYOFFSET by frame buffer (OFX/OFY are 12.4 fixed point: value / 16 = pixels)\n");
        for (const auto &e : offsets)
        {
            const auto &[fbp, ofx, ofy, kind, ctxt] = e.first;
            std::printf("  fbp %3u  OFX %5u (%.4f)  OFY %5u (%.4f)  %-8s ctxt %d : %llu prims\n", fbp, ofx, ofx / 16.0, ofy, ofy / 16.0,
                        kind == 0 ? "sprite" : "triangle", ctxt, static_cast<unsigned long long>(e.second));
        }
        std::printf("== scissor by frame buffer (x0 x1 y0 y1)\n");
        for (const auto &e : scissors)
        {
            const auto &[fbp, x0, x1, y0, y1] = e.first;
            std::printf("  fbp %3u  scissor %u %u %u %u : %llu prims\n", fbp, x0, x1, y0, y1, static_cast<unsigned long long>(e.second));
        }
        std::printf("== fraction of vertex Y in 1/16 pixel (raw = as sent, eff = minus OFY): fbp kind -> {fraction: count}\n");
        {
            std::map<std::tuple<uint32_t, int, int>, std::string> lines;
            for (const auto &e : fracs)
            {
                const auto &[fbp, kind, eff, frac] = e.first;
                lines[{fbp, kind, eff}] += " " + std::to_string(frac) + ":" + std::to_string(e.second);
            }
            for (const auto &l : lines)
                std::printf("  fbp %3u %-8s %s %s\n", std::get<0>(l.first), std::get<1>(l.first) == 0 ? "sprite" : "triangle",
                            std::get<2>(l.first) ? "eff" : "raw", l.second.c_str());
        }

        std::printf("== the first 10 fields (segments between markers): frame buffers and offsets drawn with, then the frame buffer the next marker shows\n");
        for (size_t k = 0; k + 1 < segments.size() && k < 10; ++k)
        {
            const Segment &s = segments[k];
            std::printf("  segment %zu: %zu prims; fbp", k, s.prims.size());
            for (const auto &f : s.fbps)
                std::printf(" %u(%llu)", f.first, static_cast<unsigned long long>(f.second));
            std::printf("; (OFX,OFY)");
            for (const auto &o : s.offs)
                std::printf(" (%.4f,%.4f)x%llu", o.first.first / 16.0, o.first.second / 16.0, static_cast<unsigned long long>(o.second));
            std::printf("; marker shows fbp %d\n", s.displayedFbp);
        }
        for (size_t k = 0; k + 2 < segments.size(); ++k)
        {
            const Segment &s = segments[k];
            if (s.fbps.empty() || s.displayedFbp < 0)
                continue;
            const uint32_t drawn = s.fbps.begin()->first;
            const uint32_t drawnNext = segments[k + 1].fbps.empty() ? 0xFFFFFFFFu : segments[k + 1].fbps.begin()->first;
            if (static_cast<uint32_t>(s.displayedFbp) == drawn)
                ++displayedIsPrevSegment;
            else if (static_cast<uint32_t>(s.displayedFbp) == drawnNext)
                ++displayedIsNextSegment;
            else
                ++displayedOther;
        }
        std::printf("== marker k shows the frame buffer drawn (first fbp) in the field before it: %llu, in the field after it: %llu, neither: %llu\n",
                    static_cast<unsigned long long>(displayedIsPrevSegment), static_cast<unsigned long long>(displayedIsNextSegment),
                    static_cast<unsigned long long>(displayedOther));

        // 5. consecutive fields
        const auto diffState = [&](const GSDrawState &a, const GSDrawState &b)
        {
            const GSContext &x = a.context;
            const GSContext &y = b.context;
            const auto note = [&](bool differs, const char *name)
            {
                if (differs)
                    ++memberDiffs[name];
            };
            note(x.frame.fbp != y.frame.fbp, "frame.fbp");
            note(x.frame.fbw != y.frame.fbw, "frame.fbw");
            note(x.frame.psm != y.frame.psm, "frame.psm");
            note(x.frame.fbmsk != y.frame.fbmsk, "frame.fbmsk");
            note(x.zbuf.zbp != y.zbuf.zbp, "zbuf.zbp");
            note(x.zbuf.zmask != y.zbuf.zmask, "zbuf.zmask");
            note(x.scissor.x0 != y.scissor.x0 || x.scissor.x1 != y.scissor.x1, "scissor.x");
            note(x.scissor.y0 != y.scissor.y0 || x.scissor.y1 != y.scissor.y1, "scissor.y");
            note(x.xyoffset.ofx != y.xyoffset.ofx, "xyoffset.ofx");
            note(x.xyoffset.ofy != y.xyoffset.ofy, "xyoffset.ofy");
            note(x.tex0.tbp0 != y.tex0.tbp0, "tex0.tbp0");
            note(x.tex0.tbw != y.tex0.tbw || x.tex0.psm != y.tex0.psm, "tex0.tbw/psm");
            note(x.tex0.tw != y.tex0.tw || x.tex0.th != y.tex0.th, "tex0.tw/th");
            note(x.tex0.tcc != y.tex0.tcc || x.tex0.tfx != y.tex0.tfx, "tex0.tcc/tfx");
            note(x.tex0.cbp != y.tex0.cbp || x.tex0.cpsm != y.tex0.cpsm || x.tex0.csm != y.tex0.csm || x.tex0.csa != y.tex0.csa, "tex0.clut");
            note(x.tex1 != y.tex1, "tex1");
            note(x.clamp != y.clamp, "clamp");
            note(x.alpha != y.alpha, "alpha");
            note(x.test != y.test, "test");
            note(x.fba != y.fba, "fba");
            note(a.prim.type != b.prim.type || a.prim.iip != b.prim.iip || a.prim.tme != b.prim.tme || a.prim.abe != b.prim.abe || a.prim.fst != b.prim.fst,
                 "prim");
            note(a.texa.ta0 != b.texa.ta0 || a.texa.ta1 != b.texa.ta1 || a.texa.aem != b.texa.aem, "texa");
            note(a.textureWidth != b.textureWidth || a.textureHeight != b.textureHeight, "textureWidth/Height");
            note(a.linearFilter != b.linearFilter, "linearFilter");
        };
        for (size_t k = 0; k + 2 < segments.size(); ++k)
        {
            const Segment &a = segments[k];
            const Segment &b = segments[k + 1];
            if (a.prims.empty() || a.prims.size() != b.prims.size())
            {
                ++pairsSkipped;
                pairFlags.push_back('.');
                continue;
            }
            ++pairsCompared;
            bool pairStatic = true;
            for (size_t i = 0; i < a.prims.size(); ++i)
            {
                const OffsetPrim &p = a.prims[i];
                const OffsetPrim &q = b.prims[i];
                if (p.kind != q.kind)
                {
                    ++geomOther;
                    pairStatic = false;
                    continue;
                }
                ++primPairs;
                if (p.st.context.tex0.tbp0 != q.st.context.tex0.tbp0 && tbpSamples.size() < 16)
                {
                    char text[96];
                    std::snprintf(text, sizeof(text), "field pair %zu, primitive %zu: TBP0 %u -> %u", k, i, p.st.context.tex0.tbp0, q.st.context.tex0.tbp0);
                    tbpSamples.push_back(text);
                }
                const std::map<std::string, uint64_t> before = memberDiffs;
                diffState(p.st, q.st);
                for (const auto &m : memberDiffs)
                {
                    const auto was = before.find(m.first);
                    if (m.first != "frame.fbp" && m.first != "xyoffset.ofy" && (was == before.end() || was->second != m.second))
                        pairStatic = false;
                }
                for (int v = 0; v < p.count; ++v)
                    if (!sameVertex(p.vertices[static_cast<size_t>(v)], q.vertices[static_cast<size_t>(v)]))
                        pairStatic = false;
                bool rawSame = true, effSame = true, half = true, xSame = true;
                for (int v = 0; v < p.count; ++v)
                {
                    rawSame = rawSame && p.rawY16[v] == q.rawY16[v];
                    effSame = effSame && p.effY16[v] == q.effY16[v];
                    const int d = q.effY16[v] - p.effY16[v];
                    half = half && (d == 8 || d == -8);
                    xSame = xSame && p.effX16[v] == q.effX16[v];
                }
                geomRawSame += rawSame ? 1u : 0u;
                if (effSame)
                    ++geomSame;
                else if (half && xSame)
                    ++geomHalf;
                else
                    ++geomOther;
                if (xSame)
                    ++deltaHist[std::max(-64, std::min(64, q.effY16[0] - p.effY16[0]))];
            }
            pairFlags.push_back(pairStatic ? '=' : '~');
        }
        {
            // How the alternation of the two kinds of field runs: OFY and the frame buffer of each non-empty field.
            uint64_t fields = 0, ofyAlternates = 0, ofySame = 0, fbpAlternates = 0, fbpSame = 0, pairingChanges = 0;
            const OffsetPrim *prevFirst = nullptr;
            for (const Segment &seg : segments)
            {
                if (seg.prims.empty())
                    continue;
                const OffsetPrim &first = seg.prims.front();
                ++fields;
                if (prevFirst)
                {
                    (prevFirst->st.context.xyoffset.ofy != first.st.context.xyoffset.ofy ? ofyAlternates : ofySame)++;
                    (prevFirst->st.context.frame.fbp != first.st.context.frame.fbp ? fbpAlternates : fbpSame)++;
                    const bool prevLowerPairing = (prevFirst->st.context.xyoffset.ofy & 8u) != 0u;
                    const bool curLowerPairing = (first.st.context.xyoffset.ofy & 8u) != 0u;
                    const bool fbpChanged = prevFirst->st.context.frame.fbp != first.st.context.frame.fbp;
                    // the pairing (fbp, OFY) flips when one of the two alternates and the other does not
                    if ((prevLowerPairing != curLowerPairing) != fbpChanged)
                        ++pairingChanges;
                }
                prevFirst = &first;
            }
            std::printf("== alternation of consecutive non-empty fields (%llu fields): OFY differs %llu times, equal %llu; frame buffer differs %llu, equal %llu; "
                        "the (frame buffer, OFY) pairing flips %llu times\n",
                        static_cast<unsigned long long>(fields), static_cast<unsigned long long>(ofyAlternates), static_cast<unsigned long long>(ofySame),
                        static_cast<unsigned long long>(fbpAlternates), static_cast<unsigned long long>(fbpSame), static_cast<unsigned long long>(pairingChanges));
        }
        std::printf("== consecutive fields: %llu pairs compared (same primitive count), %llu skipped (different count)\n",
                    static_cast<unsigned long long>(pairsCompared), static_cast<unsigned long long>(pairsSkipped));
        std::printf("  primitive pairs at the same position: %llu\n", static_cast<unsigned long long>(primPairs));
        std::printf("    vertices identical as sent (raw Y): %llu; identical after the offset is subtracted: %llu; every vertex exactly half a line (+-8/16) apart with the same X: %llu; anything else: %llu\n",
                    static_cast<unsigned long long>(geomRawSame), static_cast<unsigned long long>(geomSame), static_cast<unsigned long long>(geomHalf),
                    static_cast<unsigned long long>(geomOther));
        std::printf("    draw-state members that differ between the two fields (pairs that differ, of %llu):\n", static_cast<unsigned long long>(primPairs));
        for (const auto &m : memberDiffs)
            std::printf("      %-22s %llu\n", m.first.c_str(), static_cast<unsigned long long>(m.second));
        {
            // Runs of pairs in which the game sends the same draw list twice (nothing but the frame buffer and OFY differs):
            // the scenes that do not move. Pair k is fields k and k + 1; the picture shown at marker m comes from field m - 1.
            std::printf("== runs of at least 20 consecutive field pairs with an identical draw list (pair k = fields k, k+1):");
            size_t runStart = 0;
            bool any = false;
            for (size_t k = 0; k <= pairFlags.size(); ++k)
                if (k == pairFlags.size() || pairFlags[k] != '=')
                {
                    if (k - runStart >= 20 && runStart < k && pairFlags[runStart] == '=')
                    {
                        std::printf(" [%zu..%zu]", runStart, k - 1);
                        any = true;
                    }
                    runStart = k + 1;
                }
            std::printf("%s\n", any ? "" : " none");
            uint64_t statics = 0;
            for (char c : pairFlags)
                statics += c == '=' ? 1u : 0u;
            std::printf("== pairs with an identical draw list: %llu of %zu\n", static_cast<unsigned long long>(statics), pairFlags.size());
            std::printf("== pair flags (= identical draw list, ~ it changed, . different primitive count), one per pair k = fields k, k+1:\n");
            for (size_t k = 0; k < pairFlags.size(); k += 100)
                std::printf("  [%zu] %s\n", k, pairFlags.substr(k, 100).c_str());
        }
        if (!tbpSamples.empty())
        {
            std::printf("    the first primitives whose texture address (TBP0) differs between two consecutive fields:\n");
            for (const std::string &t : tbpSamples)
                std::printf("      %s\n", t.c_str());
        }
        std::printf("    effective-Y change of the first vertex in 1/16 pixel (prims with unchanged X), most common:");
        {
            std::vector<std::pair<uint64_t, int>> sorted;
            for (const auto &h : deltaHist)
                sorted.emplace_back(h.second, h.first);
            std::sort(sorted.rbegin(), sorted.rend());
            for (size_t i = 0; i < sorted.size() && i < 10; ++i)
                std::printf(" %+d:%llu", sorted[i].second, static_cast<unsigned long long>(sorted[i].first));
            std::printf("\n");
        }
        return 0;
    }

    SDL_GPUDevice *makeGpuDevice(const Options &opt, std::string &driverName, std::string &deviceName)
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            // No display (a console session, WSL without WSLg): the offscreen video driver is enough for a headless device.
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
            if (!SDL_Init(SDL_INIT_VIDEO))
            {
                std::fprintf(stderr, "SDL_Init(VIDEO): %s\n", SDL_GetError());
                return nullptr;
            }
        }
        const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
        SDL_GPUDevice *device = SDL_CreateGPUDevice(formats, opt.gpuDebug, opt.driver.empty() ? nullptr : opt.driver.c_str());
        if (!device)
        {
            std::fprintf(stderr, "SDL_CreateGPUDevice(driver=%s): %s\n", opt.driver.empty() ? "(auto)" : opt.driver.c_str(), SDL_GetError());
            return nullptr;
        }
        driverName = SDL_GetGPUDeviceDriver(device);
        const SDL_PropertiesID props = SDL_GetGPUDeviceProperties(device);
        deviceName = SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_NAME_STRING, "(unknown)");
        return device;
    }
}

int main(int argc, char **argv)
{
    Options opt;
    if (!parse(argc, argv, opt))
    {
        usage();
        return 2;
    }
    const bool useCpu = opt.compare || opt.backend == "cpu";
    const bool useGpu = opt.compare || opt.backend == "gpu";
    if (opt.scale > 4)
    {
        std::fprintf(stderr, "--scale must be 1 to 4\n");
        return 2;
    }
    if (opt.scale > 1 && (opt.compare || opt.backend != "gpu" || opt.fields != "progressive"))
    {
        std::fprintf(stderr, "--scale above 1 needs --backend gpu --fields progressive (the CPU renderer draws at scale 1 only: use --vs-scale1 to compare)\n");
        return 2;
    }
    if (opt.vsScale1 && (opt.scale < 2 || opt.backend != "gpu" || opt.compare || opt.noFields))
    {
        std::fprintf(stderr, "--vs-scale1 needs --backend gpu --fields progressive --scale 2..4 (and not --compare or --no-fields)\n");
        return 2;
    }

    // These are read once, when the backend first needs them, so set them before it exists.
    if (opt.threads >= 0)
        setEnv("PS2X_GS_THREADS", std::to_string(opt.threads).c_str());
    if (opt.defer == 0)
        setEnv("PS2X_GS_DEFER", "0");
    else if (opt.defer == 1)
        setEnv("PS2X_GS_DEFER", "1");

    // The runtime prints a trace of its first primitives to std::cout, from several threads at once;
    // keep it out of the report. The sink holds no state, so it is safe to write to concurrently.
    struct DiscardBuffer : std::streambuf
    {
        int overflow(int c) override { return c; }
        std::streamsize xsputn(const char *, std::streamsize n) override { return n; }
    } discard;
    std::streambuf *const coutBuffer = std::cout.rdbuf(&discard);

    GSRec::Reader reader;
    std::string error;
    if (!reader.openFile(opt.file, &error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    std::vector<uint8_t> vram;
    GSRec::BackendState start;
    if (!reader.readStart(vram, start, &error))
    {
        std::fprintf(stderr, "%s: %s\n", opt.file.c_str(), error.c_str());
        return 2;
    }
    std::printf("%s: version %u, %zu bytes, VRAM %u bytes\n", opt.file.c_str(), reader.version(), reader.fileSize(), reader.vramSize());

    if (opt.presents)
    {
        // The distinct presentation requests of the clip: which display circuits are on, how they
        // blend, where they read, how big the picture is. This decides what a renderer's Present must do.
        struct Key
        {
            uint64_t pmode, smode2, dispfb1, display1, dispfb2, display2;
            bool hasPreferred;
            uint32_t prefFbp, prefDest;
            bool operator<(const Key &o) const
            {
                return std::tie(pmode, smode2, dispfb1, display1, dispfb2, display2, hasPreferred, prefFbp, prefDest) <
                       std::tie(o.pmode, o.smode2, o.dispfb1, o.display1, o.dispfb2, o.display2, o.hasPreferred, o.prefFbp, o.prefDest);
            }
        };
        std::map<Key, uint64_t> seen;
        GSRec::Record r;
        while (reader.next(r) == GSRec::Reader::Status::Ok)
            if (r.type == GSRec::RT_Marker)
            {
                const GSPresentationRequest &q = r.request;
                ++seen[{q.pmode, q.smode2, q.dispfb1, q.display1, q.dispfb2, q.display2, q.hasPreferredSource,
                        q.preferredSource.fbp, q.preferredDestFbp}];
            }
        for (const auto &e : seen)
        {
            const Key &k = e.first;
            auto fb = [](uint64_t d) {
                char b[96];
                std::snprintf(b, sizeof(b), "fbp %llu fbw %llu psm 0x%02llx dbx %llu dby %llu", (unsigned long long)(d & 0x1FF), (unsigned long long)((d >> 9) & 0x3F),
                              (unsigned long long)((d >> 15) & 0x1F), (unsigned long long)((d >> 32) & 0x7FF), (unsigned long long)((d >> 43) & 0x7FF));
                return std::string(b);
            };
            auto dp = [](uint64_t d) {
                char b[96];
                std::snprintf(b, sizeof(b), "dx %llu dy %llu magh %llu magv %llu dw %llu dh %llu", (unsigned long long)(d & 0xFFF), (unsigned long long)((d >> 12) & 0x7FF),
                              (unsigned long long)((d >> 23) & 0xF), (unsigned long long)((d >> 27) & 3), (unsigned long long)((d >> 32) & 0xFFF) + 1, (unsigned long long)((d >> 44) & 0x7FF) + 1);
                return std::string(b);
            };
            std::printf("%6llu markers: PMODE %#llx (en1 %d en2 %d mmod %d amod %d slbg %d alp %llu) SMODE2 %#llx (int %d ffmd %d)\n"
                        "               DISPFB1 %s | DISPLAY1 %s\n"
                        "               DISPFB2 %s | DISPLAY2 %s\n"
                        "               preferred source: %s fbp %u dest fbp %u\n",
                        (unsigned long long)e.second, (unsigned long long)k.pmode, (int)(k.pmode & 1), (int)((k.pmode >> 1) & 1), (int)((k.pmode >> 5) & 1),
                        (int)((k.pmode >> 6) & 1), (int)((k.pmode >> 7) & 1), (unsigned long long)((k.pmode >> 8) & 0xFF), (unsigned long long)k.smode2,
                        (int)(k.smode2 & 1), (int)((k.smode2 >> 1) & 1), fb(k.dispfb1).c_str(), dp(k.display1).c_str(), fb(k.dispfb2).c_str(),
                        dp(k.display2).c_str(), k.hasPreferred ? "yes" : "no", k.prefFbp, k.prefDest);
        }
        return 0;
    }

    if (opt.offsets)
        return reportOffsets(reader);
    if (opt.prims)
        return reportPrims(reader);
    if (opt.extent)
        return reportExtent(reader);

    if (opt.states)
    {
        // The distinct draw states of the clip as a GPU renderer sees them (what would pick a pipeline,
        // a shader path or a texture), with how many primitives and how much bounding-box area each has.
        struct Info
        {
            uint64_t prims = 0, area = 0;
            std::string text;
            std::vector<uint32_t> ids;
        };
        std::map<std::string, Info> table;
        GSRec::Record r;
        while (reader.next(r) == GSRec::Reader::Status::Ok)
            if (r.type == GSRec::RT_Submit)
            {
                const GSDrawState &st = r.batch.state;
                const GSContext &c = st.context;
                static const char *const kType[] = {"POINT", "LINE", "LSTRIP", "TRI", "TRISTRIP", "TRIFAN", "SPRITE"};
                char key[400];
                std::snprintf(key, sizeof(key),
                              "%-7s %s%s%s%s | tex %s %ux%u tbw %u psm 0x%02x tfx %u tcc %u %s clamp %llu/%llu | alpha %02llx fix %llu | test ate %llu atst %llu aref %llu ztst %llu zte %llu zmask %u | fbp %u zbp %u",
                              kType[std::min<unsigned>(st.prim.type, 6)], st.prim.iip ? "IIP " : "", st.prim.abe ? "ABE " : "", st.prim.fst ? "FST " : "STQ ",
                              st.pabe ? "PABE " : "", st.prim.tme ? "on" : "off", st.textureWidth, st.textureHeight, c.tex0.tbw, c.tex0.psm, c.tex0.tfx, c.tex0.tcc,
                              st.linearFilter ? "linear" : "nearest", (unsigned long long)(c.clamp & 3), (unsigned long long)((c.clamp >> 2) & 3),
                              (unsigned long long)(c.alpha & 0xFF), (unsigned long long)((c.alpha >> 32) & 0xFF), (unsigned long long)(c.test & 1),
                              (unsigned long long)((c.test >> 1) & 7), (unsigned long long)((c.test >> 4) & 0xFF), (unsigned long long)((c.test >> 17) & 3),
                              (unsigned long long)((c.test >> 16) & 1), c.zbuf.zmask ? 1u : 0u, c.frame.fbp, c.zbuf.zbp);
                Info &info = table[key];
                ++info.prims;
                if (std::find(info.ids.begin(), info.ids.end(), r.stateId) == info.ids.end())
                    info.ids.push_back(r.stateId);
                float x0 = r.batch.vertices[0].x, x1 = x0, y0 = r.batch.vertices[0].y, y1 = y0;
                for (int i = 1; i < std::min<int>(r.batch.vertexCount, st.prim.type == GS_PRIM_SPRITE ? 2 : 3); ++i)
                {
                    x0 = std::min(x0, r.batch.vertices[i].x);
                    x1 = std::max(x1, r.batch.vertices[i].x);
                    y0 = std::min(y0, r.batch.vertices[i].y);
                    y1 = std::max(y1, r.batch.vertices[i].y);
                }
                info.area += static_cast<uint64_t>(std::max(0.0f, x1 - x0) * std::max(0.0f, y1 - y0));
            }
        std::vector<std::pair<std::string, Info>> rows(table.begin(), table.end());
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second.area > b.second.area; });
        for (const auto &row : rows)
        {
            std::string ids;
            for (uint32_t id : row.second.ids)
                ids += (ids.empty() ? "" : ",") + std::to_string(id);
            std::printf("%9llu prims %12llu px state ids %s :: %s\n", (unsigned long long)row.second.prims, (unsigned long long)row.second.area, ids.c_str(), row.first.c_str());
        }
        return 0;
    }

    if (opt.info)
    {
        uint64_t counts[256] = {};
        GSRec::Record rec;
        GSRec::Reader::Status status;
        uint64_t lastHostNs = 0;
        while ((status = reader.next(rec)) == GSRec::Reader::Status::Ok)
        {
            ++counts[rec.type];
            if (rec.type == GSRec::RT_Marker)
                lastHostNs = rec.hostNs;
        }
        for (unsigned t = 0; t < 256; ++t)
            if (counts[t])
                std::printf("  0x%02X %-20s %llu\n", t, GSRec::RecordName(static_cast<uint8_t>(t)), static_cast<unsigned long long>(counts[t]));
        std::printf("  states %u, blobs %u, original duration %.2f s, %s\n", reader.statesDefined(), reader.blobsDefined(),
                    static_cast<double>(lastHostNs) / 1.0e9,
                    status == GSRec::Reader::Status::End ? "End record present" : "no End record (cut short)");
        return 0;
    }

    if (opt.dropState.find("clut") != std::string::npos)
    {
        start.clut.fill(0);
        start.clutCbp.fill(0);
    }
    if (opt.dropState.find("pagecache") != std::string::npos)
    {
        start.pageCacheBase = 0xFFFFFFFFu;
        start.pageCacheBytes.clear();
    }
    if (opt.dropState.find("transfer") != std::string::npos)
    {
        start.transfer = {};
        start.transferState = {};
        start.localToHost.clear();
        start.localToHostReadPos = 0;
    }

    // The renderers. Each gets its own copy of the starting GS memory.
    Side cpuSide, gpuSide, refSide;
    std::unique_ptr<GSCpuBackend> cpuBackend;
    std::unique_ptr<GSGpuBackend> gpuBackend;
    std::unique_ptr<GSGpuBackend> refBackend; // --vs-scale1: the same GPU renderer at scale 1
    SDL_GPUDevice *gpuDevice = nullptr;
    std::string gpuDriver, gpuDeviceName;
    if (useCpu)
    {
        cpuSide.name = "cpu";
        cpuSide.vram = vram;
        cpuBackend = std::make_unique<GSCpuBackend>();
        cpuSide.backend = cpuBackend.get();
        cpuSide.cpu = cpuBackend.get();
        cpuBackend->Initialize(cpuSide.vram.data(), static_cast<uint32_t>(cpuSide.vram.size()));
        cpuBackend->ImportRecordingState(start);
    }
    if (useGpu)
    {
        gpuDevice = makeGpuDevice(opt, gpuDriver, gpuDeviceName);
        if (!gpuDevice)
            return 2;
        std::printf("gpu: driver %s, device \"%s\", scale %u, fields %s\n", gpuDriver.c_str(), gpuDeviceName.c_str(), opt.scale, opt.fields.c_str());
        gpuSide.name = "gpu";
        gpuSide.vram = vram;
        gpuBackend = std::make_unique<GSGpuBackend>(gpuDevice, opt.scale,
                                                    opt.fields == "progressive" ? GSGpuBackend::FieldMode::Progressive : GSGpuBackend::FieldMode::Weave);
        if (!gpuBackend->Ready())
        {
            std::fprintf(stderr, "the GPU renderer could not be set up\n");
            return 2;
        }
        gpuSide.backend = gpuBackend.get();
        gpuSide.gpu = gpuBackend.get();
        gpuBackend->Initialize(gpuSide.vram.data(), static_cast<uint32_t>(gpuSide.vram.size()));
        gpuBackend->ImportRecordingState(start);
        // The game's mode: the picture of a marker is the field copy taken at that marker.
        if (!opt.noFields && !gpuBackend->SetFieldPublishing(true))
        {
            std::fprintf(stderr, "the GPU renderer could not publish field copies\n");
            return 2;
        }
        if (opt.vsScale1)
        {
            refSide.name = "gpu1x";
            refSide.vram = vram;
            refBackend = std::make_unique<GSGpuBackend>(gpuDevice, 1u, GSGpuBackend::FieldMode::Progressive);
            if (!refBackend->Ready() || !refBackend->SetFieldPublishing(true))
            {
                std::fprintf(stderr, "the scale-1 reference renderer could not be set up\n");
                return 2;
            }
            refSide.backend = refBackend.get();
            refSide.gpu = refBackend.get();
            refBackend->Initialize(refSide.vram.data(), static_cast<uint32_t>(refSide.vram.size()));
            refBackend->ImportRecordingState(start);
        }
    }
    std::unique_ptr<GSGpuPresenter> presenter;
    GSFieldDeinterlacer readbackDeinterlacer; // what the runtime does with path A's pictures
    uint64_t directCompared = 0, directDiffering = 0, directPixelsDiffering = 0, directMagenta = 0, directFieldPictures = 0;
    int directMaxDiff = 0;
    uint32_t directWritten = 0;
    double directPullSeconds = 0.0, directRecordSeconds = 0.0;
    if (opt.direct)
    {
        if (!useGpu)
        {
            std::fprintf(stderr, "--direct needs --backend gpu\n");
            return 2;
        }
        if (opt.noFields)
        {
            std::fprintf(stderr, "--direct needs the field copies (do not use --no-fields)\n");
            return 2;
        }
        presenter = std::make_unique<GSGpuPresenter>(*gpuBackend);
        if (!presenter->Ready())
        {
            std::fprintf(stderr, "the direct presenter could not be set up\n");
            return 2;
        }
    }

    // The rows of the GPU renderer's pictures are this many pixels apart (640 at scale 1; more at a higher scale with field publishing).
    const uint32_t gpuStride = (useGpu && !opt.noFields) ? gpuBackend->PictureStride() : kStride;
    const uint32_t gpuRows = (useGpu && !opt.noFields) ? gpuBackend->PictureRows() : 512u;
    std::map<uint32_t, uint32_t> ofyOfFbp; // the OFY of the latest primitive drawn into each frame buffer (--compare-rows parity)
    std::vector<Replayed> replayed; // the CPU side's when there is one, else the GPU's
    std::map<uint32_t, LiveFrame> liveFrames;
    std::map<uint32_t, uint64_t> liveDisplays;
    uint64_t records = 0, submits = 0, uploadBytes = 0;
    uint64_t firstHostNs = 0, lastHostNs = 0;
    double pngSeconds = 0.0;
    uint64_t pngCount = 0, diffPngCount = 0;
    std::string stopReason = "End record";
    const std::string clip = clipName(opt.file);

    // --vs-scale1 bookkeeping: the scaled picture averaged down by `opt.scale`, with the blocks' corners at multiples of the scale
    // (shift 0) and moved up and left by half the scale (shift scale / 2), against the scale-1 picture of the same marker.
    constexpr int kDownsampleTolerance = 8;
    struct DownsampleSeries
    {
        uint32_t shift = 0;
        std::vector<double> shares; // percent of pixels differing by more than kDownsampleTolerance in any colour channel
        std::vector<int> maxDiffs;
        std::vector<uint32_t> markers;
        std::vector<WorstFrame> worst;
    };
    // Series 2 is not an average: it takes the pixel at each block's top-left corner, which samples the same GS position as the
    // scale-1 picture's pixel does when the sample grid is the corner grid (the default).
    // Series 3 takes the pixel half a scale-1 pixel to the right and down of that corner: the scale-1 picture's sensitivity to a half pixel.
    constexpr int kSeries = 4;
    DownsampleSeries downsample[kSeries];
    downsample[0].shift = 0;
    downsample[1].shift = opt.scale / 2u;
    downsample[2].shift = 0;
    downsample[3].shift = 0;
    uint64_t downsampleUnscaled = 0; // markers whose picture was not at the scaled size (a picture composed from real pixels)

    // --compare bookkeeping
    std::vector<double> shares; // per compared marker, in percent
    std::vector<int> maxDiffs;
    std::vector<uint32_t> shareMarkers;
    uint64_t sizeMismatches = 0, framesWithoutPicture = 0;
    std::vector<WorstFrame> worst; // the opt.diffWorst frames with the largest share so far

    const auto t0 = Clock::now();
    GSRec::Record rec;
    for (;;)
    {
        const GSRec::Reader::Status status = reader.next(rec);
        if (status == GSRec::Reader::Status::End)
            break;
        if (status == GSRec::Reader::Status::Truncated)
        {
            stopReason = "the file ends without an End record (cut short)";
            break;
        }
        if (status == GSRec::Reader::Status::Corrupt)
        {
            std::fprintf(stderr, "corrupt record at byte %zu\n", reader.position());
            return 2;
        }
        ++records;
        if (rec.type == GSRec::RT_Submit && ((opt.onlyState >= 0 && static_cast<int>(rec.stateId) != opt.onlyState) ||
                                             (opt.skipState >= 0 && static_cast<int>(rec.stateId) == opt.skipState)))
            continue; // debugging filter
        if (rec.type == GSRec::RT_Submit)
        {
            ++submits;
            ofyOfFbp[rec.batch.state.context.frame.fbp] = rec.batch.state.context.xyoffset.ofy;
        }
        else if (rec.type == GSRec::RT_UploadImage)
            uploadBytes += rec.size;

        if (rec.type == GSRec::RT_Marker)
        {
            if (replayed.size() <= rec.marker)
                replayed.resize(static_cast<size_t>(rec.marker) + 1);
            if (replayed.size() == 1)
                firstHostNs = rec.hostNs;
            lastHostNs = rec.hostNs;

            Picture pictures[2]; // cpu, gpu
            if (useCpu)
            {
                const PresentationFrame frame = playMarker(cpuSide, rec);
                pictures[0] = toPicture(frame);
                // Nothing else has been submitted since the wait, so VRAM is as it was at the marker.
                replayed[rec.marker].displayHash = GSCpuBackend::HashPresentedPages(cpuSide.vram.data(), rec.request);
                if (frame)
                    replayed[rec.marker].frameHash = GSRec::HashFrame(frame.pixels.data(), frame.width, frame.height, kStride * 4u);
            }
            if (useGpu)
            {
                const PresentationFrame frame = playMarker(gpuSide, rec);
                pictures[1] = toPicture(frame, gpuStride);
                if (!useCpu && frame)
                    replayed[rec.marker].frameHash = GSRec::HashFrame(frame.pixels.data(), frame.width, frame.height, gpuStride * 4u);
            }
            if (presenter && rec.marker % opt.directEvery == opt.directEvery - 1u)
            {
                // Path A: the picture the runtime would have built from Present()'s pixels (UploadFrame in ps2_runtime.cpp).
                std::vector<uint8_t> expected(static_cast<size_t>(gpuStride) * gpuRows * 4u, 0u);
                const Picture &readback = pictures[1];
                uint32_t pictureWidth = 640, pictureHeight = 448;
                if (readback.pixels.empty())
                {
                    for (size_t i = 0; i < expected.size(); i += 4)
                    {
                        expected[i] = 255;
                        expected[i + 2] = 255;
                        expected[i + 3] = 255;
                    }
                    ++directMagenta;
                }
                else
                {
                    const uint32_t width = readback.width, height = readback.height;
                    std::vector<uint8_t> packed(static_cast<size_t>(width) * height * 4u);
                    for (uint32_t y = 0; y < height; ++y)
                        std::memcpy(packed.data() + static_cast<size_t>(y) * width * 4u,
                                    readback.pixels.data() + static_cast<size_t>(y) * readback.stride * 4u, static_cast<size_t>(width) * 4u);
                    const bool fieldPictures = (rec.request.smode2 & 1ull) != 0ull && height != 0u && height * 2u <= 512u && width != 0u && width <= 640u;
                    if (fieldPictures)
                    {
                        readbackDeinterlacer.process(packed.data(), width, height, expected.data(), static_cast<size_t>(gpuStride) * 4u);
                        pictureHeight = height * 2u;
                        ++directFieldPictures;
                    }
                    else
                    {
                        readbackDeinterlacer.reset();
                        for (uint32_t y = 0; y < std::min<uint32_t>(height, gpuRows); ++y)
                            std::memcpy(expected.data() + static_cast<size_t>(y) * gpuStride * 4u, packed.data() + static_cast<size_t>(y) * width * 4u,
                                        static_cast<size_t>(std::min<uint32_t>(width, gpuStride)) * 4u);
                        pictureHeight = height;
                    }
                    pictureWidth = width;
                }
                (void)pictureWidth;
                (void)pictureHeight;

                // Path B: the picture built on the GPU from the field the marker published.
                const auto d0 = Clock::now();
                const GSGpuPresenter::Size directSize = presenter->Pull();
                const auto d1 = Clock::now();
                SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpuDevice);
                presenter->Record(cmd);
                SDL_SubmitGPUCommandBuffer(cmd);
                presenter->Submitted();
                const auto d2 = Clock::now();
                directPullSeconds += std::chrono::duration<double>(d1 - d0).count();
                directRecordSeconds += std::chrono::duration<double>(d2 - d1).count();
                std::vector<uint8_t> got;
                if (!presenter->Download(got))
                {
                    std::fprintf(stderr, "marker %u: the direct picture could not be downloaded\n", rec.marker);
                    return 2;
                }
                ++directCompared;
                if (!opt.pngDir.empty() && rec.marker % opt.pngEvery == 0)
                {
                    // The picture the host would show (path B), at its own size: with --png-dir, direct_marker_NNNNN.png.
                    Picture shownDirect;
                    shownDirect.pixels = got;
                    shownDirect.stride = gpuStride;
                    shownDirect.width = directSize.width;
                    shownDirect.height = directSize.height;
                    char directName[64];
                    std::snprintf(directName, sizeof(directName), "direct_marker_%05u.png", rec.marker);
                    if (!writePicture(opt.pngDir + "/" + directName, shownDirect))
                        std::fprintf(stderr, "cannot write %s/%s (does the folder exist?)\n", opt.pngDir.c_str(), directName);
                }
                if (std::getenv("PS2X_REPLAY_TRACE_DECISION"))
                {
                    const GSFieldDeinterlacer::Decision &cpuDecision = readbackDeinterlacer.lastDecision();
                    uint64_t gpuLower = 0, gpuUpper = 0;
                    bool gpuIsLower = false;
                    presenter->DebugDecision(gpuLower, gpuUpper, gpuIsLower);
                    std::printf("trace marker %u: CPU measured %d %lld/%lld %s | GPU %llu/%llu %s\n", rec.marker, cpuDecision.measured ? 1 : 0,
                                static_cast<long long>(cpuDecision.lowerError), static_cast<long long>(cpuDecision.upperError),
                                cpuDecision.lower ? "lower" : "upper", static_cast<unsigned long long>(gpuLower),
                                static_cast<unsigned long long>(gpuUpper), gpuIsLower ? "lower" : "upper");
                }
                uint64_t different = 0;
                int largest = 0;
                for (size_t i = 0; i < expected.size(); i += 4)
                {
                    int worst = 0;
                    for (int c = 0; c < 4; ++c)
                        worst = std::max(worst, std::abs(static_cast<int>(expected[i + static_cast<size_t>(c)]) - static_cast<int>(got[i + static_cast<size_t>(c)])));
                    largest = std::max(largest, worst);
                    different += worst != 0 ? 1u : 0u;
                }
                if (different != 0)
                {
                    ++directDiffering;
                    directPixelsDiffering += different;
                    directMaxDiff = std::max(directMaxDiff, largest);
                    if (directWritten < 4 && !opt.directDir.empty())
                    {
                        ++directWritten;
                        Picture a, b, diff;
                        a.pixels = expected;
                        b.pixels = got;
                        a.stride = b.stride = gpuStride;
                        a.width = b.width = diff.width = gpuStride;
                        a.height = b.height = diff.height = gpuRows;
                        diff = makeDiffPicture(a, b);
                        char name[96];
                        std::snprintf(name, sizeof(name), "%s_m%05u_", clip.c_str(), rec.marker);
                        const std::string base = opt.directDir + "/" + name;
                        writePicture(base + "readback.png", a);
                        writePicture(base + "direct.png", b);
                        writePicture(base + "diff.png", diff);
                    }
                    if (directDiffering <= 5)
                    {
                        std::printf("  marker %u: direct picture differs from the readback path's in %llu pixels (largest channel difference %d)\n",
                                    rec.marker, static_cast<unsigned long long>(different), largest);
                        const GSFieldDeinterlacer::Decision &cpuDecision = readbackDeinterlacer.lastDecision();
                        uint64_t gpuLower = 0, gpuUpper = 0;
                        bool gpuIsLower = false;
                        const bool gpuKnown = presenter->DebugDecision(gpuLower, gpuUpper, gpuIsLower);
                        std::printf("    CPU deinterlacer: measured %d, lower error %lld, upper error %lld, answer %s | GPU: lower sum %llu, upper sum %llu, answer %s\n",
                                    cpuDecision.measured ? 1 : 0, static_cast<long long>(cpuDecision.lowerError),
                                    static_cast<long long>(cpuDecision.upperError), cpuDecision.lower ? "lower" : "upper",
                                    static_cast<unsigned long long>(gpuLower), static_cast<unsigned long long>(gpuUpper),
                                    gpuKnown ? (gpuIsLower ? "lower" : "upper") : "?");
                    }
                }
            }
            if (refBackend)
            {
                const Picture reference = toPicture(playMarker(refSide, rec), refBackend->PictureStride());
                const Picture &scaled = pictures[1];
                if (reference.pixels.empty() || scaled.pixels.empty() || scaled.width != reference.width * opt.scale ||
                    scaled.height != reference.height * opt.scale)
                {
                    ++downsampleUnscaled;
                }
                else
                {
                    for (int k = 0; k < kSeries; ++k)
                    {
                        DownsampleSeries &series = downsample[k];
                        const Picture averaged = boxDown(scaled, opt.scale, series.shift, k >= 2, k == 3 ? opt.scale / 2u : 0u);
                        const DiffResult d = diffPictures(reference, averaged, kDownsampleTolerance, 3);
                        series.shares.push_back(d.share * 100.0);
                        series.maxDiffs.push_back(d.maxDiff);
                        series.markers.push_back(rec.marker);
                        if (!opt.diffDir.empty() && opt.diffWorst != 0)
                        {
                            const bool room = series.worst.size() < opt.diffWorst;
                            auto smallest = std::min_element(series.worst.begin(), series.worst.end(),
                                                             [](const WorstFrame &a, const WorstFrame &b) { return a.share < b.share; });
                            if (room || d.share * 100.0 > smallest->share)
                            {
                                WorstFrame w;
                                w.share = d.share * 100.0;
                                w.maxDiff = d.maxDiff;
                                w.marker = rec.marker;
                                w.cpu = reference; // the scale-1 picture
                                w.gpu = averaged;  // the scaled picture, averaged down
                                if (room)
                                    series.worst.push_back(std::move(w));
                                else
                                    *smallest = std::move(w);
                            }
                        }
                    }
                }
            }
            const Picture &shown = useCpu ? pictures[0] : pictures[1];
            replayed[rec.marker].width = shown.width;
            replayed[rec.marker].height = shown.height;
            replayed[rec.marker].done = true;

            if (opt.compare)
            {
                if (pictures[0].pixels.empty() || pictures[1].pixels.empty())
                {
                    ++framesWithoutPicture;
                }
                else
                {
                    // A progressive GPU picture is compared at the CPU picture's size.
                    // (the field on display at this marker was drawn with the OFY last used for its frame buffer)
                    const uint32_t shownFbp = static_cast<uint32_t>(rec.request.dispfb1 & 0x1FFu);
                    const unsigned parity = (ofyOfFbp.count(shownFbp) && (ofyOfFbp[shownFbp] & 8u) != 0u) ? 1u : 0u;
                    const Picture reduced = pictures[1].height == pictures[0].height * 2u && pictures[1].width == pictures[0].width
                                                ? (opt.compareRows == "average" ? halveRows(pictures[1]) : pickRows(pictures[1], parity))
                                                : Picture{};
                    const Picture &gpuPicture = reduced.pixels.empty() ? pictures[1] : reduced;
                    if (pictures[0].width != gpuPicture.width || pictures[0].height != gpuPicture.height)
                        ++sizeMismatches;
                    const DiffResult d = diffPictures(pictures[0], gpuPicture);
                    shares.push_back(d.share * 100.0);
                    maxDiffs.push_back(d.maxDiff);
                    shareMarkers.push_back(rec.marker);
                    if (!opt.diffDir.empty())
                    {
                        if (opt.diffEvery != 0 && rec.marker % opt.diffEvery == 0)
                        {
                            char name[96];
                            std::snprintf(name, sizeof(name), "%s_m%05u_", clip.c_str(), rec.marker);
                            const std::string base = opt.diffDir + "/" + name;
                            const Picture diff = makeDiffPicture(pictures[0], gpuPicture);
                            if (writePicture(base + "cpu.png", pictures[0]) && writePicture(base + "gpu.png", pictures[1]) &&
                                writePicture(base + "diff.png", diff))
                                diffPngCount += 3;
                            else
                                std::fprintf(stderr, "cannot write pictures to %s (does the folder exist?)\n", opt.diffDir.c_str());
                        }
                        if (opt.diffWorst != 0)
                        {
                            const bool room = worst.size() < opt.diffWorst;
                            auto smallest = std::min_element(worst.begin(), worst.end(), [](const WorstFrame &a, const WorstFrame &b)
                                                             { return a.share < b.share; });
                            if (room || d.share * 100.0 > smallest->share)
                            {
                                WorstFrame w;
                                w.share = d.share * 100.0;
                                w.maxDiff = d.maxDiff;
                                w.marker = rec.marker;
                                w.cpu = pictures[0];
                                w.gpu = gpuPicture;
                                if (room)
                                    worst.push_back(std::move(w));
                                else
                                    *smallest = std::move(w);
                            }
                        }
                    }
                }
            }
            if (!opt.pngDir.empty() && !shown.pixels.empty() && rec.marker % opt.pngEvery == 0)
            {
                const auto p0 = Clock::now();
                char name[64];
                std::snprintf(name, sizeof(name), "marker_%05u.png", rec.marker);
                bool ok = writePicture(opt.pngDir + "/" + name, shown);
                if (ok && opt.compare)
                {
                    std::snprintf(name, sizeof(name), "gpu_marker_%05u.png", rec.marker);
                    ok = writePicture(opt.pngDir + "/" + name, pictures[1]);
                }
                if (ok)
                    ++pngCount;
                else
                    std::fprintf(stderr, "cannot write %s/%s (does the folder exist?)\n", opt.pngDir.c_str(), name);
                pngSeconds += std::chrono::duration<double>(Clock::now() - p0).count();
            }
            if (replayed.size() >= opt.maxMarkers)
            {
                stopReason = "--max-markers";
                break;
            }
            continue;
        }
        if (rec.type == GSRec::RT_LiveFrame)
        {
            liveFrames[rec.marker] = {rec.width, rec.height, rec.hash};
            continue;
        }
        if (rec.type == GSRec::RT_LiveDisplay)
        {
            liveDisplays[rec.marker] = rec.hash;
            continue;
        }
        if (useCpu)
            applyRecord(cpuSide, rec);
        if (useGpu)
            applyRecord(gpuSide, rec);
        if (refBackend)
            applyRecord(refSide, rec);
    }
    if (useCpu)
        cpuBackend->Sync(GSSyncReason::DebugReadback);
    if (useGpu)
        gpuBackend->Sync(GSSyncReason::DebugReadback);
    if (refBackend)
        refBackend->Sync(GSSyncReason::DebugReadback);
    std::cout.rdbuf(coutBuffer);
    const double seconds = std::chrono::duration<double>(Clock::now() - t0).count() - pngSeconds;

    // Compare with what the game saw. Only the CPU renderer can reproduce it.
    uint64_t frameChecked = 0, frameMatched = 0, frameUnchecked = 0, frameNoMarker = 0;
    uint64_t dispChecked = 0, dispMatched = 0, dispUnchecked = 0;
    std::vector<std::string> mismatches;
    for (size_t m = 0; useCpu && m < replayed.size(); ++m) // the game's hashes can only be reproduced by the CPU renderer
    {
        if (!replayed[m].done)
            continue;
        const auto lf = liveFrames.find(static_cast<uint32_t>(m));
        if (lf == liveFrames.end())
        {
            ++frameUnchecked;
        }
        else
        {
            ++frameChecked;
            if (lf->second.hash == replayed[m].frameHash && lf->second.width == replayed[m].width && lf->second.height == replayed[m].height)
            {
                ++frameMatched;
            }
            else if (mismatches.size() < 12)
            {
                char line[256];
                std::snprintf(line, sizeof(line), "  frame of marker %zu: game %ux%u %016llx, replay %ux%u %016llx", m, lf->second.width,
                              lf->second.height, static_cast<unsigned long long>(lf->second.hash), replayed[m].width, replayed[m].height,
                              static_cast<unsigned long long>(replayed[m].frameHash));
                mismatches.push_back(line);
            }
        }
        const auto ld = liveDisplays.find(static_cast<uint32_t>(m));
        if (ld == liveDisplays.end())
        {
            ++dispUnchecked;
        }
        else
        {
            ++dispChecked;
            if (ld->second == replayed[m].displayHash)
            {
                ++dispMatched;
            }
            else if (mismatches.size() < 12)
            {
                char line[256];
                std::snprintf(line, sizeof(line), "  displayed pages of marker %zu: game %016llx, replay %016llx", m,
                              static_cast<unsigned long long>(ld->second), static_cast<unsigned long long>(replayed[m].displayHash));
                mismatches.push_back(line);
            }
        }
    }
    for (const auto &entry : liveFrames)
        if (useCpu && (entry.first >= replayed.size() || !replayed[entry.first].done))
            ++frameNoMarker;

    if (!opt.hashesFile.empty())
    {
        if (FILE *f = std::fopen(opt.hashesFile.c_str(), "w"))
        {
            for (size_t m = 0; m < replayed.size(); ++m)
                if (replayed[m].done)
                    std::fprintf(f, "%zu %ux%u %016llx %016llx\n", m, replayed[m].width, replayed[m].height,
                                 static_cast<unsigned long long>(replayed[m].frameHash), static_cast<unsigned long long>(replayed[m].displayHash));
            std::fclose(f);
        }
        else
        {
            std::fprintf(stderr, "cannot write %s\n", opt.hashesFile.c_str());
        }
    }

    const uint64_t markers = replayed.size();
    std::printf("replayed %llu markers, %llu records (%llu primitives, %.1f MB uploaded); stopped at: %s\n", static_cast<unsigned long long>(markers),
                static_cast<unsigned long long>(records), static_cast<unsigned long long>(submits), static_cast<double>(uploadBytes) / 1.0e6,
                stopReason.c_str());
    std::printf("settings: threads %s, defer %s%s%s\n", opt.threads >= 0 ? std::to_string(opt.threads).c_str() : "default",
                opt.defer >= 0 ? std::to_string(opt.defer).c_str() : "default", opt.dropState.empty() ? "" : ", dropped state: ",
                opt.dropState.c_str());
    if (presenter)
    {
        const GSGpuBackend::Stats ds = gpuBackend->GetStats();
        std::printf("direct path: compared %llu pictures (every %u marker%s; %llu field pictures, %llu magenta placeholders): %llu differ "
                    "(%llu pixels in all, largest channel difference %d)\n",
                    static_cast<unsigned long long>(directCompared), opt.directEvery, opt.directEvery == 1 ? "" : "s",
                    static_cast<unsigned long long>(directFieldPictures), static_cast<unsigned long long>(directMagenta),
                    static_cast<unsigned long long>(directDiffering), static_cast<unsigned long long>(directPixelsDiffering), directMaxDiff);
        std::printf("direct path: fields published fast %llu (%llu with fallback frames), slow %llu, invalid %llu, dropped %llu; "
                    "GS-side MarkPresentation %.3f ms per marker; host-side Pull %.3f ms, Record+submit %.3f ms per picture\n",
                    static_cast<unsigned long long>(ds.fieldsFast), static_cast<unsigned long long>(ds.fieldsWithCandidates),
                    static_cast<unsigned long long>(ds.fieldsSlow), static_cast<unsigned long long>(ds.fieldsInvalid),
                    static_cast<unsigned long long>(ds.fieldsDropped),
                    markers ? gpuSide.markSeconds * 1000.0 / static_cast<double>(markers) : 0.0,
                    directCompared ? directPullSeconds * 1000.0 / static_cast<double>(directCompared) : 0.0,
                    directCompared ? directRecordSeconds * 1000.0 / static_cast<double>(directCompared) : 0.0);
    }
    const double gameRate = lastHostNs > firstHostNs ? static_cast<double>(markers - 1) * 1.0e9 / static_cast<double>(lastHostNs - firstHostNs) : 0.0;

    if (useCpu)
    {
        std::printf("frames (what Present returned in the game): checked %llu, matched %llu, mismatched %llu, unchecked %llu\n",
                    static_cast<unsigned long long>(frameChecked), static_cast<unsigned long long>(frameMatched),
                    static_cast<unsigned long long>(frameChecked - frameMatched), static_cast<unsigned long long>(frameUnchecked));
        std::printf("displayed VRAM pages (every marker): checked %llu, matched %llu, mismatched %llu, unchecked %llu\n",
                    static_cast<unsigned long long>(dispChecked), static_cast<unsigned long long>(dispMatched),
                    static_cast<unsigned long long>(dispChecked - dispMatched), static_cast<unsigned long long>(dispUnchecked));
    }
    else
    {
        std::printf("frames (what Present returned in the game): not comparable with --backend gpu (%llu live hashes ignored)\n",
                    static_cast<unsigned long long>(liveFrames.size()));
        std::printf("displayed VRAM pages (every marker): not comparable with --backend gpu (%llu live hashes ignored)\n",
                    static_cast<unsigned long long>(liveDisplays.size()));
    }
    const uint64_t callChecks = cpuSide.callChecks + gpuSide.callChecks;
    const uint64_t callMismatches = cpuSide.callMismatches + gpuSide.callMismatches;
    std::printf("calls with results (ReadVram, ClearFramebuffer, ConsumeLocalToHostBytes): checked %llu, mismatched %llu\n",
                static_cast<unsigned long long>(callChecks), static_cast<unsigned long long>(callMismatches));
    if (frameNoMarker != 0)
        std::printf("note: %llu live hashes belong to markers that were not replayed\n", static_cast<unsigned long long>(frameNoMarker));
    for (const std::string &line : mismatches)
        std::printf("%s\n", line.c_str());
    for (const Side *side : {&cpuSide, &gpuSide})
        for (size_t i = 0; i < side->callProblems.size() && i < 5; ++i)
            std::printf("  %s\n", side->callProblems[i].c_str());

    const double cpuRate = static_cast<double>(markers) / std::max(cpuSide.seconds, 1e-9);
    const double gpuRate = static_cast<double>(markers) / std::max(opt.compare ? gpuSide.seconds : seconds, 1e-9);
    if (!opt.compare)
    {
        if (markers > 0 && seconds > 0.0)
            std::printf("speed: %.1f markers/s (%.2f s for the replay%s); the game ran this clip at %.1f markers/s\n",
                        static_cast<double>(markers) / seconds, seconds, pngCount ? ", PNG writing not counted" : "", gameRate);
        if (useGpu)
            std::printf("speed (gpu: %s on %s): %.1f markers/s\n", gpuDriver.c_str(), gpuDeviceName.c_str(), gpuRate);
    }
    else
    {
        std::printf("speed: cpu %.1f markers/s (%.2f s in calls and waits), gpu %.1f markers/s (%.2f s, %s on %s); the game ran this clip at %.1f markers/s\n",
                    cpuRate, cpuSide.seconds, gpuRate, gpuSide.seconds, gpuDriver.c_str(), gpuDeviceName.c_str(), gameRate);
    }
    if (pngCount)
        std::printf("wrote %llu PNG files to %s\n", static_cast<unsigned long long>(pngCount), opt.pngDir.c_str());

    double mean = 0.0, median = 0.0, p95 = 0.0, worstShare = 0.0;
    uint32_t worstMarker = 0;
    int worstMaxDiff = 0, overallMaxDiff = 0;
    uint64_t over1 = 0, over5 = 0;
    if (opt.compare)
    {
        for (size_t i = 0; i < shares.size(); ++i)
        {
            mean += shares[i];
            over1 += shares[i] > 1.0 ? 1u : 0u;
            over5 += shares[i] > 5.0 ? 1u : 0u;
            overallMaxDiff = std::max(overallMaxDiff, maxDiffs[i]);
            if (shares[i] > worstShare || i == 0)
            {
                worstShare = shares[i];
                worstMarker = shareMarkers[i];
                worstMaxDiff = maxDiffs[i];
            }
        }
        if (!shares.empty())
            mean /= static_cast<double>(shares.size());
        median = percentile(shares, 0.5);
        p95 = percentile(shares, 0.95);
        std::printf("compare %s (%s on %s): %zu pictures; share of pixels differing by more than %d in any channel: mean %.4f%%, median %.4f%%, "
                    "p95 %.4f%%, worst %.4f%% (marker %u, max channel difference %d); frames over 1%%: %llu, over 5%%: %llu; "
                    "largest channel difference in any frame: %d\n",
                    clip.c_str(), gpuDriver.c_str(), gpuDeviceName.c_str(), shares.size(), kPixelTolerance, mean, median, p95, worstShare, worstMarker,
                    worstMaxDiff, static_cast<unsigned long long>(over1), static_cast<unsigned long long>(over5), overallMaxDiff);
        if (sizeMismatches || framesWithoutPicture)
            std::printf("compare: %llu pictures differ in size, %llu markers had no picture on one side\n",
                        static_cast<unsigned long long>(sizeMismatches), static_cast<unsigned long long>(framesWithoutPicture));
        // The worst frames, written now that the whole clip is known.
        if (!opt.diffDir.empty())
        {
            std::sort(worst.begin(), worst.end(), [](const WorstFrame &a, const WorstFrame &b) { return a.share > b.share; });
            for (const WorstFrame &w : worst)
            {
                char name[96];
                std::snprintf(name, sizeof(name), "%s_worst_m%05u_", clip.c_str(), w.marker);
                const std::string base = opt.diffDir + "/" + name;
                const Picture diff = makeDiffPicture(w.cpu, w.gpu);
                if (writePicture(base + "cpu.png", w.cpu) && writePicture(base + "gpu.png", w.gpu) && writePicture(base + "diff.png", diff))
                    diffPngCount += 3;
                std::printf("  worst frame: marker %u, %.4f%% of pixels over %d, max channel difference %d -> %s*.png\n", w.marker, w.share,
                            kPixelTolerance, w.maxDiff, base.c_str());
            }
            std::printf("wrote %llu diff PNG files to %s\n", static_cast<unsigned long long>(diffPngCount), opt.diffDir.c_str());
        }
    }

    // --vs-scale1: how far the scaled pictures, averaged down, are from the scale-1 pictures
    double dsMean[kSeries] = {}, dsMedian[kSeries] = {}, dsP95[kSeries] = {}, dsWorst[kSeries] = {};
    uint32_t dsWorstMarker[kSeries] = {};
    int dsMaxDiff[kSeries] = {};
    if (opt.vsScale1)
    {
        for (int k = 0; k < kSeries; ++k)
        {
            DownsampleSeries &series = downsample[k];
            for (size_t i = 0; i < series.shares.size(); ++i)
            {
                dsMean[k] += series.shares[i];
                dsMaxDiff[k] = std::max(dsMaxDiff[k], series.maxDiffs[i]);
                if (series.shares[i] > dsWorst[k] || i == 0)
                {
                    dsWorst[k] = series.shares[i];
                    dsWorstMarker[k] = series.markers[i];
                }
            }
            if (!series.shares.empty())
                dsMean[k] /= static_cast<double>(series.shares.size());
            dsMedian[k] = percentile(series.shares, 0.5);
            dsP95[k] = percentile(series.shares, 0.95);
            std::printf("downsample %s scale %u vs 1, %s: %zu pictures; share of pixels differing by more than %d in any colour channel: "
                        "mean %.4f%%, median %.4f%%, p95 %.4f%%, worst %.4f%% (marker %u); largest channel difference %d\n",
                        clip.c_str(), opt.scale,
                        k == 0 ? "averaged in blocks at multiples of the scale"
                               : (k == 1 ? "averaged in blocks moved up and left by half the scale"
                                         : (k == 2 ? "taking the pixel at each block's top-left corner" : "taking the pixel half a scale-1 pixel right and down of that corner")),
                        series.shares.size(),
                        kDownsampleTolerance, dsMean[k], dsMedian[k], dsP95[k], dsWorst[k], dsWorstMarker[k], dsMaxDiff[k]);
            if (!opt.diffDir.empty())
            {
                std::sort(series.worst.begin(), series.worst.end(), [](const WorstFrame &a, const WorstFrame &b) { return a.share > b.share; });
                for (const WorstFrame &w : series.worst)
                {
                    char name[96];
                    std::snprintf(name, sizeof(name), "%s_x%u_%s%u_worst_m%05u_", clip.c_str(), opt.scale, k == 2 ? "pick" : (k == 3 ? "half" : "shift"), series.shift, w.marker);
                    const std::string base = opt.diffDir + "/" + name;
                    const Picture diff = makeDiffPicture(w.cpu, w.gpu);
                    if (writePicture(base + "scale1.png", w.cpu) && writePicture(base + "averaged.png", w.gpu) && writePicture(base + "diff.png", diff))
                        std::printf("  worst frame (shift %u): marker %u, %.4f%% of pixels over %d, max channel difference %d -> %s*.png\n", series.shift,
                                    w.marker, w.share, kDownsampleTolerance, w.maxDiff, base.c_str());
                }
            }
        }
        if (downsampleUnscaled)
            std::printf("downsample: %llu markers had no picture at the scaled size on one side (a placeholder or a picture composed from real pixels)\n",
                        static_cast<unsigned long long>(downsampleUnscaled));
    }

    GSGpuBackend::Stats gs;
    if (useGpu)
    {
        gs = gpuBackend->GetStats();
        const double perFrame = markers ? static_cast<double>(gs.flushes) / static_cast<double>(markers) : 0.0;
        std::printf("gpu stats: %llu primitives (%llu culled before drawing, %llu unsupported), %llu vertices, %llu draw calls in %llu flushes "
                    "(%.2f flushes per frame, %.1f draws per flush, most %llu), %llu render passes\n",
                    static_cast<unsigned long long>(gs.primitives), static_cast<unsigned long long>(gs.primitivesCulled),
                    static_cast<unsigned long long>(gs.primitivesUnsupported), static_cast<unsigned long long>(gs.vertices),
                    static_cast<unsigned long long>(gs.drawCalls), static_cast<unsigned long long>(gs.flushes), perFrame,
                    gs.flushes ? static_cast<double>(gs.drawCalls) / static_cast<double>(gs.flushes) : 0.0,
                    static_cast<unsigned long long>(gs.maxDrawsPerFlush), static_cast<unsigned long long>(gs.renderPasses));
        std::printf("gpu stats: %llu pipelines created; textures: %llu lookups = %llu recent + %llu signature + %llu content hits, %llu misses "
                    "(%llu created, %.1f MB, decode %.1f ms, %llu evicted); %llu frame targets, %llu depth targets\n",
                    static_cast<unsigned long long>(gs.pipelinesCreated), static_cast<unsigned long long>(gs.texLookups),
                    static_cast<unsigned long long>(gs.texHitsRecent), static_cast<unsigned long long>(gs.texHitsSignature),
                    static_cast<unsigned long long>(gs.texHitsContent), static_cast<unsigned long long>(gs.texMisses),
                    static_cast<unsigned long long>(gs.texturesCreated), static_cast<double>(gs.textureBytesCreated) / 1.0e6,
                    static_cast<double>(gs.texDecodeNanos) / 1.0e6, static_cast<unsigned long long>(gs.texturesEvicted),
                    static_cast<unsigned long long>(gs.frameTargets), static_cast<unsigned long long>(gs.depthTargets));
        std::printf("gpu cost (ms on the replaying thread): texture hashing %.1f (%.0f MB), creating %.1f, staging %.1f, decoding %.1f "
                    "(%.2f M texels of %.2f M declared, %llu regions); flushing %.1f (map %.1f, record %.1f, submit %.1f, evict %.1f)\n",
                    static_cast<double>(gs.texHashNanos) / 1.0e6, static_cast<double>(gs.texHashBytes) / 1048576.0,
                    static_cast<double>(gs.texCreateNanos) / 1.0e6, static_cast<double>(gs.texStageNanos) / 1.0e6,
                    static_cast<double>(gs.texDecodeNanos) / 1.0e6, static_cast<double>(gs.texTexelsDecoded) / 1.0e6,
                    static_cast<double>(gs.texTexelsDeclared) / 1.0e6, static_cast<unsigned long long>(gs.texFillRegions),
                    static_cast<double>(gs.flushTotalNanos) / 1.0e6, static_cast<double>(gs.flushMapNanos) / 1.0e6,
                    static_cast<double>(gs.flushRecordNanos) / 1.0e6, static_cast<double>(gs.flushSubmitNanos) / 1.0e6,
                    static_cast<double>(gs.flushEvictNanos) / 1.0e6);
        std::printf("gpu stats: %llu upload transfers (%.1f MB), %llu Z page copies into the depth target (%llu pages); %llu prims with a "
                    "blend equation that has no exact fixed-function form; %llu prims using unimplemented features; %.1f MB read back\n",
                    static_cast<unsigned long long>(gs.uploadTransfers), static_cast<double>(gs.uploadBytes) / 1.0e6,
                    static_cast<unsigned long long>(gs.depthRestores), static_cast<unsigned long long>(gs.depthRestoreTiles),
                    static_cast<unsigned long long>(gs.blendUnsupported), static_cast<unsigned long long>(gs.ignoredFeatures),
                    static_cast<double>(gs.downloadBytes) / 1.0e6);
    }

    if (!opt.jsonFile.empty() && useGpu)
    {
        if (FILE *f = std::fopen(opt.jsonFile.c_str(), "w"))
        {
            std::fprintf(f, "{\n  \"clip\": \"%s\",\n  \"driver\": \"%s\",\n  \"device\": \"%s\",\n  \"scale\": %u,\n  \"fields\": \"%s\",\n  \"mode\": \"%s\",\n  \"markers\": %llu,\n",
                         clip.c_str(), gpuDriver.c_str(), gpuDeviceName.c_str(), opt.scale, opt.fields.c_str(), opt.compare ? "compare" : "gpu",
                         static_cast<unsigned long long>(markers));
            std::fprintf(f, "  \"speed_markers_per_s\": {\"gpu\": %.2f, \"cpu\": %.2f},\n", gpuRate, useCpu ? cpuRate : 0.0);
            if (opt.compare)
            {
                std::fprintf(f, "  \"tolerance\": %d,\n  \"share_percent\": {\"mean\": %.6f, \"median\": %.6f, \"p95\": %.6f, \"worst\": %.6f, \"worst_marker\": %u, "
                                "\"frames_over_1pct\": %llu, \"frames_over_5pct\": %llu},\n  \"max_channel_difference\": %d,\n",
                             kPixelTolerance, mean, median, p95, worstShare, worstMarker, static_cast<unsigned long long>(over1),
                             static_cast<unsigned long long>(over5), overallMaxDiff);
                std::fprintf(f, "  \"cpu_hashes\": {\"frames_checked\": %llu, \"frames_matched\": %llu, \"display_checked\": %llu, \"display_matched\": %llu},\n",
                             static_cast<unsigned long long>(frameChecked), static_cast<unsigned long long>(frameMatched),
                             static_cast<unsigned long long>(dispChecked), static_cast<unsigned long long>(dispMatched));
                std::fprintf(f, "  \"per_marker\": [");
                for (size_t i = 0; i < shares.size(); ++i)
                    std::fprintf(f, "%s[%u, %.5f, %d]", i ? ", " : "", shareMarkers[i], shares[i], maxDiffs[i]);
                std::fprintf(f, "],\n");
            }
            if (opt.vsScale1)
            {
                std::fprintf(f, "  \"downsample\": {\"scale\": %u, \"tolerance\": %d, \"series\": [", opt.scale, kDownsampleTolerance);
                for (int k = 0; k < kSeries; ++k)
                    std::fprintf(f, "%s{\"kind\": \"%s\", \"shift\": %u, \"pictures\": %zu, \"mean\": %.6f, \"median\": %.6f, \"p95\": %.6f, \"worst\": %.6f, \"worst_marker\": %u, \"max_channel_difference\": %d}",
                                 k ? ", " : "", k == 2 ? "pick" : (k == 3 ? "pickhalf" : "average"), downsample[k].shift, downsample[k].shares.size(), dsMean[k], dsMedian[k], dsP95[k], dsWorst[k],
                                 dsWorstMarker[k], dsMaxDiff[k]);
                std::fprintf(f, "]},\n");
            }
            std::fprintf(f, "  \"gpu_stats\": {\"primitives\": %llu, \"culled\": %llu, \"vertices\": %llu, \"draw_calls\": %llu, \"flushes\": %llu, "
                            "\"render_passes\": %llu, \"max_draws_per_flush\": %llu, \"pipelines_created\": %llu, \"texture_lookups\": %llu, "
                            "\"tex_hits_recent\": %llu, \"tex_hits_signature\": %llu, \"tex_hits_content\": %llu, \"tex_misses\": %llu, "
                            "\"textures_created\": %llu, \"texture_bytes_created\": %llu, \"textures_evicted\": %llu, \"upload_transfers\": %llu, "
                            "\"depth_restores\": %llu, \"depth_restore_tiles\": %llu, \"upload_bytes\": %llu, \"frame_targets\": %llu, \"depth_targets\": %llu, \"blend_unsupported\": %llu, \"ignored_features\": %llu}\n}\n",
                         static_cast<unsigned long long>(gs.primitives), static_cast<unsigned long long>(gs.primitivesCulled),
                         static_cast<unsigned long long>(gs.vertices), static_cast<unsigned long long>(gs.drawCalls),
                         static_cast<unsigned long long>(gs.flushes), static_cast<unsigned long long>(gs.renderPasses),
                         static_cast<unsigned long long>(gs.maxDrawsPerFlush), static_cast<unsigned long long>(gs.pipelinesCreated),
                         static_cast<unsigned long long>(gs.texLookups), static_cast<unsigned long long>(gs.texHitsRecent),
                         static_cast<unsigned long long>(gs.texHitsSignature), static_cast<unsigned long long>(gs.texHitsContent),
                         static_cast<unsigned long long>(gs.texMisses), static_cast<unsigned long long>(gs.texturesCreated),
                         static_cast<unsigned long long>(gs.textureBytesCreated), static_cast<unsigned long long>(gs.texturesEvicted),
                         static_cast<unsigned long long>(gs.uploadTransfers), static_cast<unsigned long long>(gs.depthRestores),
                         static_cast<unsigned long long>(gs.depthRestoreTiles), static_cast<unsigned long long>(gs.uploadBytes),
                         static_cast<unsigned long long>(gs.frameTargets), static_cast<unsigned long long>(gs.depthTargets),
                         static_cast<unsigned long long>(gs.blendUnsupported), static_cast<unsigned long long>(gs.ignoredFeatures));
            std::fclose(f);
        }
        else
        {
            std::fprintf(stderr, "cannot write %s\n", opt.jsonFile.c_str());
        }
    }

    bool failed = callMismatches != 0 || directDiffering != 0;
    if (useCpu)
        failed = failed || frameMatched != frameChecked || dispMatched != dispChecked;
    if (!failed && useCpu && frameChecked + dispChecked == 0)
        std::printf("warning: the recording has no check values\n");
    bool pictureOk = true;
    if (opt.compare)
    {
        pictureOk = mean < kMeanThresholdPercent && worstShare < kWorstThresholdPercent && sizeMismatches == 0 && framesWithoutPicture == 0;
        std::printf("COMPARE: %s (thresholds: mean < %.1f%%, worst frame < %.1f%%)\n", pictureOk ? "within thresholds" : "OUTSIDE thresholds",
                    kMeanThresholdPercent, kWorstThresholdPercent);
    }
    std::printf("%s\n", (failed || !pictureOk) ? "RESULT: MISMATCH" : (useCpu ? "RESULT: ok" : "RESULT: ok (pictures not compared)"));
    std::fflush(stdout);

    presenter.reset();
    refBackend.reset();
    gpuBackend.reset();
    if (gpuDevice)
    {
        SDL_DestroyGPUDevice(gpuDevice);
        SDL_Quit();
    }
    return (failed || !pictureOk) ? 1 : 0;
}
