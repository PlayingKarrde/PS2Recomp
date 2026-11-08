#pragma once

// FPWR: how the game chooses and uses the GPU GS renderer (GPU renderer milestone M3). This header has no
// SDL in it, so ps2_runtime.cpp and the debug panel can include it. The code behind it is
// src/lib/gs/gs_gpu_host.cpp, which does include SDL. No header that generated game code includes refers
// to this one.
//
// Environment variables (all read once, at start):
//   PS2X_GS_BACKEND=cpu|gpu     the GS renderer; gpu is the default (since M5). Without an SDL_GPU device it falls
//                               back to cpu with one log line. Unset with PS2X_GS_RECORD set picks cpu, the only
//                               renderer that records; the debug panel's VRAM views also need cpu.
//   PS2X_GS_SCALE=1..4          internal resolution multiplier N (default 1): the targets are 640N x 448N per field
//                               (progressive mode), and the picture the host shows is that size, scaled to the window
//                               with linear filtering. Anything else than 1 to 4 is refused with a log line and 1 is
//                               used; so is N > 1 with PS2X_GS_FIELDS=weave (the weave is built at scale 1 only) and
//                               with PS2X_GS_GPU_READBACK=1 (the host's CPU picture is 640x512). Read once at start.
//   PS2X_GS_SPRITE_SAMPLE=gs|target
//                               how textured sprites (the game's full-screen backgrounds) are sampled when N > 1: at the
//                               centre of their GS pixel (the scale-1 picture enlarged) or at each target pixel, with
//                               bilinear samples held to the two texels the GS pixel's own sample uses (smooth). The
//                               default is gs at N = 1 and target above.
//   PS2X_GS_SAMPLE_GRID=corner|centre
//                               where the target pixels of a scale-1 pixel sample a triangle (gs_gpu_math.h,
//                               TargetMapping). corner (the default) is the GS's own convention; centre is for measurement.
//   PS2X_GS_FIELDS=progressive|weave
//                               how the game's 224-line fields are drawn and shown (GSGpuBackend::FieldMode).
//                               progressive (the default) draws every field as a 640x448 picture, without the
//                               half line the game offsets alternate fields by, and shows the newest field as it
//                               is. weave keeps each field 224 lines high and weaves the last three into 448
//                               lines, like the CPU renderer's picture.
//   PS2X_GS_GPU_READBACK=1      path A: the GPU renderer returns CPU pixels (GSGpuBackend::Present) and the
//                               runtime's CPU deinterlacer works on them, as with the CPU renderer. Without
//                               it the GPU renderer uses path B, the direct path (see gs_gpu_present.h).
//                               Both paths follow PS2X_GS_FIELDS (in progressive mode path A's pictures are
//                               448 lines, which the CPU deinterlacer leaves alone).
//   PS2X_GS_WIDE=<W:H>          widescreen prototype (task w1; progressive mode and path B only, off by default): the targets and
//                               the picture are wider than the game's 640 GS pixels, so a 4:3 picture becomes W:H and shows the
//                               extra scene left and right of it (see GSGpuBackend, WidePadLeft). PS2X_GS_WIDE_FILL=0 stops
//                               full-screen fills and fades being stretched over the new columns. PS2X_ASPECT still overrides the
//                               shape the picture is shown in.
//   PS2X_GS_W1_LOG=<file>       test aid: one CSV row per VBlank with where the primitives lie against the 640-wide screen.
//   PS2X_GS_W1_PRIMS=<file>     test aid (with PS2X_GS_W1_LOG): one line per primitive for the VBlanks that draw at most 150.
//   PS2X_GS_W1_REGS=1           test aid (gs_frontend.cpp): each distinct value written to PRIM, PRMODE(CONT), FRAME, ZBUF, XYOFFSET,
//                               SCISSOR and TEST is logged once.
//   PS2X_WINDOW=<W>x<H>         the host window's size (default 640x480), to look at a widened picture at its own shape.
//   PS2X_GS_STABILITY=<file>    test aid, path B: after each new picture, append a CSV row with the share of pixels
//                               that differ from the previous picture (waits for the GPU; see gs_gpu_present.cpp).
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
        // The shape (width / height) the picture should be shown in when it is a widened one (PS2X_GS_WIDE); 0 for the usual 4:3.
        float aspect = 0.0f;
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
