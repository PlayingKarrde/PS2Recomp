#pragma once

// FPWR: how the game chooses and uses the GPU GS renderer (GPU renderer milestone M3). This header has no
// SDL in it, so ps2_runtime.cpp and the debug panel can include it. The code behind it is
// src/lib/gs/gs_gpu_host.cpp, which does include SDL. No header that generated game code includes refers
// to this one.
//
// Environment variables (all read once, at start):
//   PS2X_GS_BACKEND=cpu|gpu     the GS renderer; cpu is the default and behaves exactly as before.
//   PS2X_GS_SCALE=1             internal resolution multiplier; only 1 exists so far (other values are
//                               refused with a log line and 1 is used).
//   PS2X_GS_GPU_READBACK=1      path A: the GPU renderer returns CPU pixels (GSGpuBackend::Present) and the
//                               runtime's CPU deinterlacer works on them, as with the CPU renderer. Without
//                               it the GPU renderer uses path B, the direct path (see gs_gpu_present.h).
//   PS2X_GS_STATS=1             wrap the chosen renderer in GSBackendProbe: per-call timing and calling
//                               threads, one line a second.
//   PS2X_GS_GPU_STATS=1         GSGpuBackend's own counters, one line a second.
//   PS2X_GS_PRESENT_STATS=1     counters of Compose's branches (gs_presentation.h).

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ps2x_gs
{
    // Main thread, once, after the host window exists. Returns the renderer to install with
    // GS::setRasterBackend, or null to keep the CPU renderer the GS already has. When the GPU renderer
    // was asked for but cannot be used (no SDL_GPU device: PS2X_PRESENT=renderer, or device creation
    // failed), one line says why and the CPU renderer stays.
    std::unique_ptr<GSRasterBackend> createConfiguredBackend();

    // True when the picture reaches the window on the GPU (path B). Main thread only afterwards.
    bool directPictureActive();

    // The size of the picture the host shows this frame.
    struct PictureSize
    {
        uint32_t width = 640;
        uint32_t height = 448;
    };

    // Path B, main thread, once per guest VBlank: takes the newest finished field from the GS thread (never
    // waiting for it or for the GPU). The GPU work that turns it into the picture is recorded later, by the
    // presenter, into the frame's command buffer. Returns the size of the picture that will be drawn.
    PictureSize pullPicture();

    // Main thread, before the host destroys its window and device: releases the presenter's GPU objects and
    // the host's pointer to the backend. Returns true when a GPU renderer was in use: the caller must then
    // replace it (GS::setRasterBackend) before the device goes. With the CPU renderer nothing changes.
    bool shutdown();

    // One line for the debug panel about what its GS views show, or "" with the CPU renderer.
    std::string debugPanelNote();
}
