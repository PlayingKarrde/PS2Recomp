// ps2x_gs_replay: plays a GS recording (see ps2xRuntime/include/runtime/gs/gs_recording.h and
// docs/formats/gs-recording.md) through the CPU renderer without the game, and checks that every
// picture comes out the same as when the game ran.
//
//   ps2x_gs_replay <file> [--png-dir D] [--png-every N] [--threads N] [--defer 0|1]
//                         [--hashes F] [--max-markers N] [--drop-state LIST] [--info]
//
// How it works. The recording starts at a presentation marker with the whole GS memory and the
// backend's other state. The player loads that into a fresh GSCpuBackend, then makes every call
// the game made, in order. At each marker it waits for the drawer to finish, asks for the picture
// (Present) and hashes it. While the game ran, the presenter thread did the same for the markers it
// showed and the recording kept the hashes ("live" hashes); for every marker the recording
// also holds a hash of the displayed VRAM pages. The player compares both kinds.
//
// Exit code: 0 when everything that could be checked matched, 1 when something did not, 2 on
// usage errors or a file that cannot be read.

#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_recording.h"
#include "../common/png_write.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using ps2x::test::writePng;

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
        std::string pngDir;
        uint32_t pngEvery = 30;
        std::string hashesFile;
        int threads = -1;
        int defer = -1;
        uint64_t maxMarkers = ~0ull;
        std::string dropState; // debugging: start without part of the recorded backend state
        bool info = false;
    };

    void usage()
    {
        std::fprintf(stderr,
                     "usage: ps2x_gs_replay <file> [--png-dir D] [--png-every N] [--threads N] [--defer 0|1]\n"
                     "                             [--hashes F] [--max-markers N] [--drop-state LIST] [--info]\n"
                     "  --png-dir D     write the picture of every Nth marker to D/marker_NNNNN.png\n"
                     "  --png-every N   with --png-dir: every Nth marker (default 30)\n"
                     "  --threads N     raster threads, including the caller (PS2X_GS_THREADS; 1 = no pool, nothing queued)\n"
                     "  --defer 0|1     0 draws every primitive at once on the calling thread (PS2X_GS_DEFER=0)\n"
                     "  --hashes F      write one line per marker with the replayed hashes, to compare two runs\n"
                     "  --max-markers N stop after N markers\n"
                     "  --drop-state L  debugging: start without part of the recorded backend state (comma list of\n"
                     "                  clut, pagecache, transfer) to see whether the replay still matches\n"
                     "  --info          list the records in the file and stop\n");
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
            else if (arg == "--info")
            {
                o.info = true;
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
}

int main(int argc, char **argv)
{
    Options opt;
    if (!parse(argc, argv, opt))
    {
        usage();
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
    GSCpuBackend backend;
    backend.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
    backend.ImportRecordingState(start);

    std::vector<Replayed> replayed;
    std::map<uint32_t, LiveFrame> liveFrames;
    std::map<uint32_t, uint64_t> liveDisplays;
    uint64_t callChecks = 0, callMismatches = 0;
    uint64_t records = 0, submits = 0, uploadBytes = 0;
    uint64_t firstHostNs = 0, lastHostNs = 0;
    double pngSeconds = 0.0;
    uint64_t pngCount = 0;
    std::string stopReason = "End record";
    std::vector<std::string> callProblems;

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
        switch (rec.type)
        {
        case GSRec::RT_Submit:
            backend.Submit(rec.batch);
            ++submits;
            break;
        case GSRec::RT_LoadClut:
            backend.LoadClut(rec.tex0, rec.texclut);
            break;
        case GSRec::RT_BeginTransfer:
            backend.BeginTransfer(rec.transfer);
            break;
        case GSRec::RT_UploadImage:
            backend.UploadImage(rec.data, rec.size);
            uploadBytes += rec.size;
            break;
        case GSRec::RT_Flush:
            backend.Flush();
            break;
        case GSRec::RT_TextureFlush:
            backend.TextureFlush();
            break;
        case GSRec::RT_Sync:
            backend.Sync(rec.syncReason);
            break;
        case GSRec::RT_ClearFramebuffer:
        {
            const bool got = backend.ClearFramebuffer(rec.context, rec.rgba);
            ++callChecks;
            if (got != rec.result)
            {
                ++callMismatches;
                callProblems.push_back("ClearFramebuffer returned " + std::to_string(got) + ", the game saw " + std::to_string(rec.result));
            }
            break;
        }
        case GSRec::RT_WriteVram:
            backend.WriteVram(rec.psm, rec.base, rec.bw, rec.x, rec.y, rec.value);
            break;
        case GSRec::RT_Reset:
            backend.Reset();
            break;
        case GSRec::RT_ReadVram:
        {
            const uint32_t got = backend.ReadVram(rec.psm, rec.base, rec.bw, rec.x, rec.y);
            ++callChecks;
            if (got != rec.value)
            {
                ++callMismatches;
                callProblems.push_back("ReadVram returned " + std::to_string(got) + ", the game saw " + std::to_string(rec.value));
            }
            break;
        }
        case GSRec::RT_ConsumeLocalToHost:
        {
            std::vector<uint8_t> buffer(std::max<uint32_t>(rec.maxBytes, 1u));
            const uint32_t got = backend.ConsumeLocalToHostBytes(buffer.data(), rec.maxBytes);
            ++callChecks;
            if (got != rec.size || (got != 0 && std::memcmp(buffer.data(), rec.data, got) != 0))
            {
                ++callMismatches;
                callProblems.push_back("ConsumeLocalToHostBytes returned " + std::to_string(got) + " bytes, the game saw " + std::to_string(rec.size));
            }
            break;
        }
        case GSRec::RT_Marker:
        {
            if (replayed.size() <= rec.marker)
                replayed.resize(static_cast<size_t>(rec.marker) + 1);
            if (replayed.size() == 1)
                firstHostNs = rec.hostNs;
            lastHostNs = rec.hostNs;
            backend.MarkPresentation(rec.request);
            backend.Sync(GSSyncReason::Presentation); // waits until the drawer has copied VRAM for this marker
            Replayed &out = replayed[rec.marker];
            const PresentationFrame frame = backend.Present(rec.request);
            if (frame)
            {
                out.width = frame.width;
                out.height = frame.height;
                out.frameHash = GSRec::HashFrame(frame.pixels.data(), frame.width, frame.height, GSCpuBackend::kPresentationStride * 4u);
            }
            // Nothing else has been submitted since the wait, so VRAM is as it was at the marker.
            out.displayHash = GSCpuBackend::HashPresentedPages(vram.data(), rec.request);
            out.done = true;
            if (!opt.pngDir.empty() && frame && rec.marker % opt.pngEvery == 0)
            {
                const auto p0 = Clock::now();
                char name[64];
                std::snprintf(name, sizeof(name), "marker_%05u.png", rec.marker);
                if (writePng(opt.pngDir + "/" + name, frame.pixels.data(), frame.width, frame.height, GSCpuBackend::kPresentationStride * 4u))
                    ++pngCount;
                else
                    std::fprintf(stderr, "cannot write %s/%s (does the folder exist?)\n", opt.pngDir.c_str(), name);
                pngSeconds += std::chrono::duration<double>(Clock::now() - p0).count();
            }
            break;
        }
        case GSRec::RT_LiveFrame:
            liveFrames[rec.marker] = {rec.width, rec.height, rec.hash};
            break;
        case GSRec::RT_LiveDisplay:
            liveDisplays[rec.marker] = rec.hash;
            break;
        default:
            break;
        }
        if (replayed.size() >= opt.maxMarkers && rec.type == GSRec::RT_Marker)
        {
            stopReason = "--max-markers";
            break;
        }
    }
    backend.Sync(GSSyncReason::DebugReadback);
    std::cout.rdbuf(coutBuffer);
    const double seconds = std::chrono::duration<double>(Clock::now() - t0).count() - pngSeconds;

    // Compare with what the game saw.
    uint64_t frameChecked = 0, frameMatched = 0, frameUnchecked = 0, frameNoMarker = 0;
    uint64_t dispChecked = 0, dispMatched = 0, dispUnchecked = 0;
    std::vector<std::string> mismatches;
    for (size_t m = 0; m < replayed.size(); ++m)
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
        if (entry.first >= replayed.size() || !replayed[entry.first].done)
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
    std::printf("frames (what Present returned in the game): checked %llu, matched %llu, mismatched %llu, unchecked %llu\n",
                static_cast<unsigned long long>(frameChecked), static_cast<unsigned long long>(frameMatched),
                static_cast<unsigned long long>(frameChecked - frameMatched), static_cast<unsigned long long>(frameUnchecked));
    std::printf("displayed VRAM pages (every marker): checked %llu, matched %llu, mismatched %llu, unchecked %llu\n",
                static_cast<unsigned long long>(dispChecked), static_cast<unsigned long long>(dispMatched),
                static_cast<unsigned long long>(dispChecked - dispMatched), static_cast<unsigned long long>(dispUnchecked));
    std::printf("calls with results (ReadVram, ClearFramebuffer, ConsumeLocalToHostBytes): checked %llu, mismatched %llu\n",
                static_cast<unsigned long long>(callChecks), static_cast<unsigned long long>(callMismatches));
    if (frameNoMarker != 0)
        std::printf("note: %llu live hashes belong to markers that were not replayed\n", static_cast<unsigned long long>(frameNoMarker));
    for (const std::string &line : mismatches)
        std::printf("%s\n", line.c_str());
    for (size_t i = 0; i < callProblems.size() && i < 5; ++i)
        std::printf("  %s\n", callProblems[i].c_str());
    if (markers > 0 && seconds > 0.0)
        std::printf("speed: %.1f markers/s (%.2f s for the replay%s); the game ran this clip at %.1f markers/s\n",
                    static_cast<double>(markers) / seconds, seconds, pngCount ? ", PNG writing not counted" : "",
                    lastHostNs > firstHostNs ? static_cast<double>(markers - 1) * 1.0e9 / static_cast<double>(lastHostNs - firstHostNs) : 0.0);
    if (pngCount)
        std::printf("wrote %llu PNG files to %s\n", static_cast<unsigned long long>(pngCount), opt.pngDir.c_str());

    const bool failed = frameMatched != frameChecked || dispMatched != dispChecked || callMismatches != 0;
    if (!failed && frameChecked + dispChecked == 0)
        std::printf("warning: the recording has no check values\n");
    std::printf("%s\n", failed ? "RESULT: MISMATCH" : "RESULT: ok");
    std::fflush(stdout);
    return failed ? 1 : 0;
}
