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
}
