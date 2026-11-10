// How the game chooses and wires up the GPU GS renderer. See include/runtime/gs/gs_gpu_host.h for the
// variables it reads. Needs SDL (it asks the host for its SDL_GPU device); nothing the generated game
// code includes refers to it.

#include "runtime/gs/gs_gpu_host.h"

#include "runtime/gs/gs_backend_probe.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_gpu_present.h"
#include "runtime/gs/gs_tex_pack.h"

#include "ps2_host_gpu.h"
#include "ps2_settings.h"

#include <SDL3/SDL_misc.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <cstdlib>
#include <cstring>

namespace ps2x_gs
{
    namespace
    {
        struct State
        {
            GSGpuBackend *backend = nullptr; // owned by the GS (GS::setRasterBackend), valid until shutdown()
            std::unique_ptr<GSGpuPresenter> presenter;
            bool readback = false;
            GSGpuBackend::FieldMode fields = GSGpuBackend::FieldMode::Progressive;
        } g;

        bool envFlag(const char *name)
        {
            const char *value = std::getenv(name);
            return value && value[0] == '1';
        }

        // The host presenter's callbacks for the direct picture path (main thread).
        SDL_GPUTexture *recordPicture(SDL_GPUCommandBuffer *commandBuffer, void *)
        {
            return g.presenter ? g.presenter->Record(commandBuffer) : nullptr;
        }

        void pictureSubmitted(void *)
        {
            if (g.presenter)
                g.presenter->Submitted();
        }

        bool downloadPicture(void *, std::vector<uint8_t> &rgba)
        {
            return g.presenter && g.presenter->Download(rgba);
        }

        void say(const char *text)
        {
            std::printf("%s\n", text);
            std::fflush(stdout);
        }
    }

    std::unique_ptr<GSRasterBackend> createConfiguredBackend()
    {
        // gpu is the default (decided at M5, 2026-11-06). Without a usable SDL_GPU device it falls back to the
        // CPU renderer below. PS2X_GS_RECORD alone also picks cpu: only the CPU renderer records GS traffic.
        const char *wanted = std::getenv("PS2X_GS_BACKEND");
        bool gpu = true;
        if (wanted && std::strcmp(wanted, "cpu") == 0)
            gpu = false;
        else if (wanted && std::strcmp(wanted, "gpu") != 0)
            std::fprintf(stderr, "[gs] PS2X_GS_BACKEND=%s is not cpu or gpu, using the default, gpu\n", wanted);
        if (!wanted && std::getenv("PS2X_GS_RECORD"))
        {
            say("[gs] PS2X_GS_RECORD is set: using the CPU renderer, the only one that records GS traffic (PS2X_GS_BACKEND=gpu overrides)");
            gpu = false;
        }
        const bool stats = envFlag("PS2X_GS_STATS");

        std::unique_ptr<GSRasterBackend> backend;
        if (gpu)
        {
            uint32_t scale = 1;
            if (const char *wantedScale = std::getenv("PS2X_GS_SCALE"))
            {
                char *end = nullptr;
                const long value = std::strtol(wantedScale, &end, 10);
                if (end != wantedScale && *end == '\0' && value >= 1 && value <= 4)
                    scale = static_cast<uint32_t>(value);
                else
                    std::fprintf(stderr, "[gs] PS2X_GS_SCALE=%s is not a whole number from 1 to 4, using 1\n", wantedScale);
            }
            g.fields = GSGpuBackend::FieldMode::Progressive;
            if (const char *fields = std::getenv("PS2X_GS_FIELDS"))
            {
                if (std::strcmp(fields, "weave") == 0)
                    g.fields = GSGpuBackend::FieldMode::Weave;
                else if (std::strcmp(fields, "progressive") != 0)
                    std::fprintf(stderr, "[gs] PS2X_GS_FIELDS=%s is not progressive or weave, using progressive\n", fields);
            }
            g.readback = envFlag("PS2X_GS_GPU_READBACK");
            if (scale > 1 && g.fields == GSGpuBackend::FieldMode::Weave)
            {
                std::fprintf(stderr, "[gs] PS2X_GS_FIELDS=weave draws at scale 1 only (PS2X_GS_SCALE=%u asked for), using 1\n", scale);
                scale = 1;
            }
            if (scale > 1 && g.readback)
            {
                std::fprintf(stderr, "[gs] PS2X_GS_GPU_READBACK=1 (path A) carries the host's 640x512 CPU picture only (PS2X_GS_SCALE=%u asked for), using 1\n", scale);
                scale = 1;
            }
            SDL_GPUDevice *device = ps2x_host::gpuDevice();
            if (!device)
            {
                say("[gs] the GPU renderer needs the SDL_GPU presenter, which is not in use (PS2X_PRESENT=renderer, or its device could not be created); using the CPU renderer");
            }
            else
            {
                auto candidate = std::make_unique<GSGpuBackend>(device, scale, g.fields);
                if (candidate->Ready() && g.readback && candidate->WidePadLeft() + candidate->WidePadRight() != 0u)
                {
                    std::fprintf(stderr, "[gs] PS2X_GS_WIDE needs path B; PS2X_GS_GPU_READBACK=1 carries the host's 640x512 CPU picture only, widescreen off\n");
                    candidate->DisableWide();
                }
                if (!candidate->Ready())
                {
                    say("[gs] the GPU renderer could not be set up (see above); using the CPU renderer");
                }
                else
                {
                    bool direct = false;
                    // Both paths take the picture from the field copies the GS thread makes at each VBlank.
                    const bool fieldsOk = candidate->SetFieldPublishing(true);
                    if (!fieldsOk)
                        say("[gs] the GPU renderer cannot publish field copies on this device; keeping the CPU renderer");
                    if (fieldsOk && !g.readback)
                    {
                        g.presenter = std::make_unique<GSGpuPresenter>(*candidate);
                        if (g.presenter->Ready())
                        {
                            candidate->SetFieldPublishing(true, true); // the host takes the fields itself
                            ps2x_host::PictureSource source;
                            source.record = recordPicture;
                            source.textureWidth = g.presenter->PictureWidth();
                            source.textureHeight = g.presenter->PictureHeight();
                            source.submitted = pictureSubmitted;
                            source.download = downloadPicture;
                            ps2x_host::gpu_present::setPictureSource(source);
                            direct = true;
                        }
                        else
                        {
                            g.presenter.reset();
                            say("[gs] the direct picture path could not be set up (see above); using the readback path");
                            g.readback = true;
                        }
                    }
                    if (!fieldsOk)
                        candidate.reset();
                    if (candidate)
                        g.backend = candidate.get();
                    char line[384];
                    const bool progressive = g.fields == GSGpuBackend::FieldMode::Progressive;
                    std::snprintf(line, sizeof(line), "[gs] GS renderer: gpu, scale %u, fields %s, %s, SDL_GPU driver %s", scale,
                                  progressive ? "progressive (each field drawn as a 640x448 picture, newest field shown)" : "weave (fields woven into 448 lines)",
                                  direct ? (progressive ? "path B (direct: field textures on the GPU, no readback)"
                                                        : "path B (direct: field textures on the GPU, deinterlaced in a shader, no readback)")
                                         : (progressive ? "path A (readback: CPU pixels)" : "path A (readback: CPU pixels, CPU deinterlacer)"),
                                  ps2x_host::gpu_present::driverName().c_str());
                    if (candidate)
                    {
                        say(line);
                        if (std::getenv("PS2X_GS_RECORD"))
                            say("[gs] PS2X_GS_RECORD is set, but only the CPU renderer records GS traffic; nothing is recorded with the GPU renderer (PS2X_GS_BACKEND=cpu records)");
                        backend = std::move(candidate);
                    }
                }
            }
        }
        if (!backend && !stats)
            return nullptr;
        if (!backend)
            backend = std::make_unique<GSCpuBackend>();
        if (stats)
            backend = std::make_unique<GSBackendProbe>(std::move(backend), g.backend ? "gpu" : "cpu");
        return backend;
    }

    bool directPictureActive()
    {
        return g.presenter != nullptr;
    }

    PictureSize pullPicture()
    {
        PictureSize size;
        if (g.presenter)
        {
            const GSGpuPresenter::Size s = g.presenter->Pull();
            size.width = s.width;
            size.height = s.height;
            size.aspect = s.aspect;
        }
        return size;
    }

    // How many GS pixels the widescreen picture (PS2X_GS_WIDE) adds right of the game's 640-wide screen (equal to the pad on the
    // left); 0 when wide is off, the CPU renderer draws, or path A turned it off. Any thread; used by the game override that keeps
    // the match clock at the right edge (src/overrides/fpwr_overrides.cpp, task w2). Declared there, not in a header.
    uint32_t widePadRight()
    {
        return g.backend ? g.backend->WidePadRight() : 0u;
    }

    bool shutdown()
    {
        // The presenter and the backend hold GPU objects, so they must go before the host destroys the device.
        const bool gpuInUse = g.backend != nullptr;
        ps2x_host::gpu_present::setPictureSource(ps2x_host::PictureSource{});
        g.presenter.reset();
        g.backend = nullptr;
        return gpuInUse;
    }

    namespace
    {
        namespace fs = std::filesystem;

        fs::path prefFolder()
        {
            std::string settings = ps2x_settings::path();
            if (settings.empty())
                settings = ps2x_settings::defaultPath();
            if (settings.empty())
                return fs::path();
            return fs::path(std::u8string(settings.begin(), settings.end())).parent_path();
        }

        std::string toUtf8(const fs::path &p)
        {
            const std::u8string text = p.u8string();
            return std::string(text.begin(), text.end());
        }
    }

    bool textureToolsOn()
    {
        return TexPack::Authoring();
    }

    std::string devPackFolder()
    {
        if (const auto pack = TexPack::DevPack())
            return pack->folder();
        const fs::path pref = prefFolder();
        return pref.empty() ? std::string() : toUtf8(pref / "dev-pack");
    }

    std::vector<std::string> devPackNotes()
    {
        const auto pack = TexPack::DevPack();
        return pack ? pack->notes() : std::vector<std::string>();
    }

    std::string dumpsFolder()
    {
        const fs::path pref = prefFolder();
        return pref.empty() ? std::string() : toUtf8(pref / "dumps");
    }

    std::string requestTextureDump()
    {
        if (!g.backend)
            return "Texture dumps need the GPU renderer.";
        if (!TexPack::Authoring())
            return "Turn Texture Tools on (Graphics tab) or set PS2X_AUTHORING=1, then restart: the dump needs them.";
        const std::string root = dumpsFolder();
        if (root.empty())
            return "There is no preference folder to write the dump to.";
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
        const std::string folder = root + "/" + stamp;
        if (!g.backend->RequestTextureDump(folder))
            return g.backend->TextureDumpBusy() ? "A dump is already under way." : "The texture tools are not running (restart with Texture Tools on).";
        return "Dumping the next frames to " + folder + " ...";
    }

    std::string textureDumpMessage(bool *busy)
    {
        if (busy)
            *busy = g.backend && g.backend->TextureDumpBusy();
        return g.backend ? g.backend->TextureDumpMessage() : std::string();
    }

    void openFolder(const std::string &path)
    {
        if (path.empty())
            return;
        std::error_code ec;
        fs::create_directories(fs::path(std::u8string(path.begin(), path.end())), ec);
        std::string url = "file:///";
        for (char c : path)
        {
            if (c == '\\')
                url += '/';
            else if (c == ' ')
                url += "%20";
            else
                url += c;
        }
        SDL_OpenURL(url.c_str());
    }

    std::string debugPanelTextureLine()
    {
        if (!g.backend)
            return "";
        const GSGpuBackend::ReplacementReport r = g.backend->Replacement();
        if (!r.active && !r.dumping && !r.fontActive && !r.authoring)
            return "";
        char text[320];
        int n = 0;
        if (r.active)
            n += std::snprintf(text + n, sizeof(text) - static_cast<size_t>(n),
                               "Texture packs: %u textures replaced this frame (%u draws), %u pictures loaded of %u files, %u refused, %u waiting, %.1f MB on the GPU",
                               r.texturesThisFrame, r.drawsThisFrame, r.decoded, r.packFiles, r.refused, r.pending, static_cast<double>(r.gpuBytes) / 1048576.0);
        if (r.fontActive)
            n += std::snprintf(text + n, sizeof(text) - static_cast<size_t>(n), "%sHD text: %u glyph files at %ux, %u cells HD and %u original in the last canvas%s", n ? "; " : "",
                               r.fontFiles, r.fontScale, r.fontHdCells, r.fontOriginalCells, r.fontRefused ? " (some files refused)" : "");
        if (r.authoring)
            n += std::snprintf(text + n, sizeof(text) - static_cast<size_t>(n), "%sAuthor tools on, dev pack: %u files", n ? "; " : "", r.devPackFiles);
        if (r.dumping)
            n += std::snprintf(text + n, sizeof(text) - static_cast<size_t>(n), "%sTexture dump: %u files written", n ? "; " : "", r.dumpedFiles);
        return std::string(text, static_cast<size_t>(std::max(n, 0)));
    }

    std::string reloadTexturePacks()
    {
        if (!g.backend)
            return "";
        const uint32_t n = g.backend->ReloadTexturePacks();
        char text[96];
        std::snprintf(text, sizeof(text), "Texture packs reloaded: %u texture files", n);
        return text;
    }

    std::string debugPanelNote()
    {
        if (!g.backend)
            return "";
        return g.readback ? "GPU renderer (readback path): GS memory views show uploaded data only; drawn pixels are on the GPU, not in them."
                          : "GPU renderer (direct path): GS memory views show uploaded data only; drawn pixels are on the GPU, not in them.";
    }
}
