// How the game chooses and wires up the GPU GS renderer. See include/runtime/gs/gs_gpu_host.h for the
// variables it reads. Needs SDL (it asks the host for its SDL_GPU device); nothing the generated game
// code includes refers to it.

#include "runtime/gs/gs_gpu_host.h"

#include "runtime/gs/gs_backend_probe.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_gpu_present.h"

#include "ps2_host_gpu.h"

#include <cstdio>
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
        const char *wanted = std::getenv("PS2X_GS_BACKEND");
        const bool gpu = wanted && std::strcmp(wanted, "gpu") == 0;
        if (wanted && !gpu && std::strcmp(wanted, "cpu") != 0)
            std::fprintf(stderr, "[gs] PS2X_GS_BACKEND=%s is not cpu or gpu, using cpu\n", wanted);
        const bool stats = envFlag("PS2X_GS_STATS");

        std::unique_ptr<GSRasterBackend> backend;
        if (gpu)
        {
            if (const char *scale = std::getenv("PS2X_GS_SCALE"))
                if (std::strcmp(scale, "1") != 0)
                    std::fprintf(stderr, "[gs] PS2X_GS_SCALE=%s is not supported yet (only 1), using 1\n", scale);
            g.fields = GSGpuBackend::FieldMode::Progressive;
            if (const char *fields = std::getenv("PS2X_GS_FIELDS"))
            {
                if (std::strcmp(fields, "weave") == 0)
                    g.fields = GSGpuBackend::FieldMode::Weave;
                else if (std::strcmp(fields, "progressive") != 0)
                    std::fprintf(stderr, "[gs] PS2X_GS_FIELDS=%s is not progressive or weave, using progressive\n", fields);
            }
            SDL_GPUDevice *device = ps2x_host::gpuDevice();
            if (!device)
            {
                say("[gs] PS2X_GS_BACKEND=gpu needs the SDL_GPU presenter, which is not in use (PS2X_PRESENT=renderer, or its device could not be created); keeping the CPU renderer");
            }
            else
            {
                auto candidate = std::make_unique<GSGpuBackend>(device, 1, g.fields);
                if (!candidate->Ready())
                {
                    say("[gs] the GPU renderer could not be set up (see above); keeping the CPU renderer");
                }
                else
                {
                    g.readback = envFlag("PS2X_GS_GPU_READBACK");
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
                    std::snprintf(line, sizeof(line), "[gs] GS renderer: gpu, scale 1, fields %s, %s, SDL_GPU driver %s",
                                  progressive ? "progressive (each field drawn as a 640x448 picture, newest field shown)" : "weave (fields woven into 448 lines)",
                                  direct ? (progressive ? "path B (direct: field textures on the GPU, no readback)"
                                                        : "path B (direct: field textures on the GPU, deinterlaced in a shader, no readback)")
                                         : (progressive ? "path A (readback: CPU pixels)" : "path A (readback: CPU pixels, CPU deinterlacer)"),
                                  ps2x_host::gpu_present::driverName().c_str());
                    if (candidate)
                    {
                        say(line);
                        if (std::getenv("PS2X_GS_RECORD"))
                            say("[gs] PS2X_GS_RECORD is set, but only the CPU renderer records GS traffic; nothing is recorded with PS2X_GS_BACKEND=gpu");
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
        }
        return size;
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

    std::string debugPanelNote()
    {
        if (!g.backend)
            return "";
        return g.readback ? "GPU renderer (readback path): GS memory views show uploaded data only; drawn pixels are on the GPU, not in them."
                          : "GPU renderer (direct path): GS memory views show uploaded data only; drawn pixels are on the GPU, not in them.";
    }
}
