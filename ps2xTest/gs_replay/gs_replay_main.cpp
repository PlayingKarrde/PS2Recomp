// ps2x_gs_replay: plays a GS recording (see ps2xRuntime/include/runtime/gs/gs_recording.h and
// docs/formats/gs-recording.md) through a GS renderer without the game.
//
//   ps2x_gs_replay <file> [--backend cpu|gpu] [--compare] [--driver D] [--scale N] [--gpu-debug]
//                         [--png-dir D] [--png-every N] [--diff-dir D] [--diff-worst N] [--diff-every N]
//                         [--json F] [--threads N] [--defer 0|1] [--hashes F] [--max-markers N]
//                         [--drop-state LIST] [--info] [--presents]
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
#include "runtime/gs/gs_gpu_present.h"
#include "runtime/gs/gs_recording.h"
#include "../common/png_write.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <chrono>
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
        int onlyState = -1;     // debugging: draw only primitives of this recorded state id
        int skipState = -1;     // debugging: skip primitives of this recorded state id
    };

    void usage()
    {
        std::fprintf(stderr,
                     "usage: ps2x_gs_replay <file> [--backend cpu|gpu] [--compare] [--driver D] [--scale N] [--gpu-debug]\n"
                     "                             [--png-dir D] [--png-every N] [--diff-dir D] [--diff-worst N] [--diff-every N]\n"
                     "                             [--json F] [--threads N] [--defer 0|1] [--hashes F] [--max-markers N]\n"
                     "                             [--drop-state LIST] [--info] [--presents]\n"
                     "                             [--direct [--direct-every N] [--direct-dir D]] [--no-fields]\n"
                     "  --backend B     the renderer to replay through: cpu (default) or gpu (SDL_GPU, headless)\n"
                     "  --compare       replay through both and measure how the pictures differ\n"
                     "  --driver D      SDL_GPU driver for the gpu renderer: vulkan, direct3d12 or metal (default: SDL picks)\n"
                     "  --scale N       render scale of the gpu renderer (only 1 is supported)\n"
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
                     "  --info          list the records in the file and stop\n"
                     "  --presents      list the distinct presentation requests in the file and stop\n");
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

    // One picture of a marker, as RGBA rows kStride pixels wide.
    struct Picture
    {
        std::vector<uint8_t> pixels;
        uint32_t width = 0, height = 0;
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

    DiffResult diffPictures(const Picture &a, const Picture &b)
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
            const uint8_t *pa = a.pixels.data() + static_cast<size_t>(y) * kStride * 4u;
            const uint8_t *pb = b.pixels.data() + static_cast<size_t>(y) * kStride * 4u;
            for (uint32_t x = 0; x < w; ++x)
            {
                int worst = 0;
                for (int c = 0; c < 4; ++c)
                    worst = std::max(worst, std::abs(static_cast<int>(pa[x * 4u + c]) - static_cast<int>(pb[x * 4u + c])));
                r.maxDiff = std::max(r.maxDiff, worst);
                r.differing += worst > kPixelTolerance ? 1u : 0u;
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
        d.pixels.assign(static_cast<size_t>(kStride) * 512u * 4u, 255u);
        for (uint32_t y = 0; y < d.height; ++y)
            for (uint32_t x = 0; x < d.width; ++x)
            {
                const uint8_t *pa = a.pixels.data() + (static_cast<size_t>(y) * kStride + x) * 4u;
                const uint8_t *pb = b.pixels.data() + (static_cast<size_t>(y) * kStride + x) * 4u;
                uint8_t *pd = d.pixels.data() + (static_cast<size_t>(y) * kStride + x) * 4u;
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

    Picture toPicture(const PresentationFrame &frame)
    {
        Picture p;
        p.width = frame.width;
        p.height = frame.height;
        if (frame)
            p.pixels = frame.pixels;
        return p;
    }

    bool writePicture(const std::string &path, const Picture &p)
    {
        if (p.pixels.empty() || p.width == 0 || p.height == 0)
            return false;
        return writePng(path, p.pixels.data(), p.width, p.height, kStride * 4u);
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
    Side cpuSide, gpuSide;
    std::unique_ptr<GSCpuBackend> cpuBackend;
    std::unique_ptr<GSGpuBackend> gpuBackend;
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
        std::printf("gpu: driver %s, device \"%s\", scale %u\n", gpuDriver.c_str(), gpuDeviceName.c_str(), opt.scale);
        gpuSide.name = "gpu";
        gpuSide.vram = vram;
        gpuBackend = std::make_unique<GSGpuBackend>(gpuDevice, opt.scale);
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

    std::vector<Replayed> replayed; // the CPU side's when there is one, else the GPU's
    std::map<uint32_t, LiveFrame> liveFrames;
    std::map<uint32_t, uint64_t> liveDisplays;
    uint64_t records = 0, submits = 0, uploadBytes = 0;
    uint64_t firstHostNs = 0, lastHostNs = 0;
    double pngSeconds = 0.0;
    uint64_t pngCount = 0, diffPngCount = 0;
    std::string stopReason = "End record";
    const std::string clip = clipName(opt.file);

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
            ++submits;
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
                pictures[1] = toPicture(frame);
                if (!useCpu && frame)
                    replayed[rec.marker].frameHash = GSRec::HashFrame(frame.pixels.data(), frame.width, frame.height, kStride * 4u);
            }
            if (presenter && rec.marker % opt.directEvery == opt.directEvery - 1u)
            {
                // Path A: the picture the runtime would have built from Present()'s pixels (UploadFrame in ps2_runtime.cpp).
                std::vector<uint8_t> expected(static_cast<size_t>(kStride) * 512u * 4u, 0u);
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
                                    readback.pixels.data() + static_cast<size_t>(y) * kStride * 4u, static_cast<size_t>(width) * 4u);
                    const bool fieldPictures = (rec.request.smode2 & 1ull) != 0ull && height != 0u && height * 2u <= 512u && width != 0u && width <= 640u;
                    if (fieldPictures)
                    {
                        readbackDeinterlacer.process(packed.data(), width, height, expected.data(), static_cast<size_t>(kStride) * 4u);
                        pictureHeight = height * 2u;
                        ++directFieldPictures;
                    }
                    else
                    {
                        readbackDeinterlacer.reset();
                        for (uint32_t y = 0; y < std::min<uint32_t>(height, 512u); ++y)
                            std::memcpy(expected.data() + static_cast<size_t>(y) * kStride * 4u, packed.data() + static_cast<size_t>(y) * width * 4u,
                                        static_cast<size_t>(std::min<uint32_t>(width, 640u)) * 4u);
                        pictureHeight = height;
                    }
                    pictureWidth = width;
                }
                (void)pictureWidth;
                (void)pictureHeight;

                // Path B: the picture built on the GPU from the field the marker published.
                const auto d0 = Clock::now();
                presenter->Pull();
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
                        a.width = b.width = diff.width = 640;
                        a.height = b.height = diff.height = 512;
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
                    if (pictures[0].width != pictures[1].width || pictures[0].height != pictures[1].height)
                        ++sizeMismatches;
                    const DiffResult d = diffPictures(pictures[0], pictures[1]);
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
                            const Picture diff = makeDiffPicture(pictures[0], pictures[1]);
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
                                w.gpu = pictures[1];
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
    }
    if (useCpu)
        cpuBackend->Sync(GSSyncReason::DebugReadback);
    if (useGpu)
        gpuBackend->Sync(GSSyncReason::DebugReadback);
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
            std::fprintf(f, "{\n  \"clip\": \"%s\",\n  \"driver\": \"%s\",\n  \"device\": \"%s\",\n  \"scale\": %u,\n  \"mode\": \"%s\",\n  \"markers\": %llu,\n",
                         clip.c_str(), gpuDriver.c_str(), gpuDeviceName.c_str(), opt.scale, opt.compare ? "compare" : "gpu",
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
    gpuBackend.reset();
    if (gpuDevice)
    {
        SDL_DestroyGPUDevice(gpuDevice);
        SDL_Quit();
    }
    return (failed || !pictureOk) ? 1 : 0;
}
