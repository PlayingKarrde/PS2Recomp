#pragma once

// FPWR: how display registers become a picture, shared by the two GS renderers (GSCpuBackend and
// GSGpuBackend). Compose() is the old GSCpuBackend::PresentFromLocalMemory with the pixel source
// replaced by a callback: it decodes the display size and read origin of the two display circuits,
// picks the source frame (including the "preferred source" and the black-frame fallbacks), blends
// the circuits and applies field presentation. The callback returns the pixels of one frame
// buffer: `outPixels` is kHostFrameWidth x kHostFrameHeight RGBA8, the picture in its top-left
// width x height corner; see GSCpuBackend::CopyFrameToHostRgba for what each argument means.
// No header that generated game code includes refers to this one.

#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace GSPresent
{
    constexpr uint32_t kHostFrameWidth = 640u;  // row stride, in pixels, of the pictures
    constexpr uint32_t kHostFrameHeight = 512u;

    using CopyFrameFn = std::function<bool(const GSFrameReg &frame, uint32_t width, uint32_t height,
                                           std::vector<uint8_t> &outPixels, bool preserveAlpha,
                                           bool useLocalMemoryLayout, bool frameBaseIsPages,
                                           uint32_t originX, uint32_t originY)>;

    // Reads one frame buffer out of a GS memory image (pixel storage mode, base pointer, buffer width as
    // GS memory holds them); this is what GSCpuBackend::CopyFrameToHostRgba does.
    bool CopyFrameFromMemory(uint8_t *vram, uint32_t vramSize, const GSFrameReg &frame, uint32_t width, uint32_t height,
                             std::vector<uint8_t> &outPixels, bool preserveAlpha, bool useLocalMemoryLayout,
                             bool frameBaseIsPages, uint32_t sourceOriginX, uint32_t sourceOriginY);

    PresentationFrame Compose(const GSPresentationRequest &request, const CopyFrameFn &copyFrame);

    // What the picture of a presentation request is, decided from the display registers alone (no
    // pixels). GSGpuBackend's direct path (GPU renderer M3) uses it to copy the displayed region of a
    // frame target on the GPU instead of reading pixels back:
    //   None          neither display circuit is enabled and set up; Compose returns no picture.
    //   DisplayFrame  the picture is exactly the `width` x `height` region of `frame` that starts at
    //                 (originX, originY), copied as it is. Compose gives the same result for these
    //                 requests: both circuits read the same pixels (so blending them changes nothing),
    //                 or one circuit is on and the "preferred source" does not apply.
    //   Complex       anything else (circuits that differ, blending over the background colour,
    //                 SMODE2 field mode, a preferred source): only Compose with real pixels knows.
    // Compose's fallback to another frame when the display frame is black (fbp 0) is not part of the
    // plan; see the note in bringup.md ("M3").
    struct Plan
    {
        enum class Kind : uint8_t
        {
            None,
            DisplayFrame,
            Complex
        };
        Kind kind = Kind::None;
        GSFrameReg frame{};
        uint32_t originX = 0, originY = 0, width = 0, height = 0;
        bool dualCircuit = false;
        bool interlaced = false; // SMODE2.INT: the picture is one field of an interlaced frame
    };
    Plan MakePlan(const GSPresentationRequest &request);

    // Opt-in counters of what Compose does (PS2X_GS_PRESENT_STATS=1), printed every 300 pictures.
    bool StatsEnabled();
}
