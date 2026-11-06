#pragma once

// FPWR: GSGpuPresenter, the main-thread half of the GS GPU renderer's direct presentation path (path B,
// GPU renderer milestone M3; the GS-thread half is GSGpuBackend, see gs_gpu_backend.h).
//
// Once per host frame, on the thread that owns the window:
//   Pull()       takes the newest field the GS thread published, if there is one (no GPU work, no waiting),
//                and says how big the picture will be: the field's size, or twice its height for an
//                interlaced field, as UploadFrame in ps2_runtime.cpp works it out for the CPU picture.
//   Record(cb)   records the GPU work that builds the picture into the frame's command buffer, outside any
//                pass, and returns the RGBA8 texture that holds it (640 x 512 at scale 1; the picture in its top-left
//                corner, zero elsewhere, alpha 255 inside it; larger at a higher scale, see PictureWidth()).
//                Interlaced fields go through the same
//                rules as the CPU deinterlacer (shaders/field_deint.frag.hlsl, field_lower*.frag.hlsl);
//                other pictures are copied as they are; a marker without a picture gives the magenta
//                placeholder UploadFrame shows. When Pull took nothing new, Record leaves the texture as it is.
//   Submitted()  after the command buffer was submitted: gives back the field slots the recorded commands no
//                longer need (a slot is unpinned only after the commands that read it were submitted, which
//                is what makes reusing it by the GS thread safe; see "Field ring" in gs_gpu_backend.cpp).
// The presenter copies each field into textures of its own as it draws it, and those hold the history the
// deinterlacer needs (the previous two fields), so it pins field slots only until Submitted().
//
// It never touches the backend's lock or the swapchain, and never waits for the GPU (Download excepted,
// which is for tests and PS2X_PRESENT_DUMP). This header does not include SDL.

#include "runtime/gs/gs_gpu_backend.h"

#include <cstdint>
#include <memory>
#include <vector>

struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;

class GSGpuPresenter
{
public:
    // The backend must publish fields (SetFieldPublishing(true)) and outlive the presenter.
    explicit GSGpuPresenter(GSGpuBackend &backend);
    ~GSGpuPresenter();
    GSGpuPresenter(const GSGpuPresenter &) = delete;
    GSGpuPresenter &operator=(const GSGpuPresenter &) = delete;

    // False when a shader or texture could not be made (the reason is logged).
    bool Ready() const;

    struct Size
    {
        uint32_t width = 640;
        uint32_t height = 448;
    };
    Size Pull();
    SDL_GPUTexture *Record(SDL_GPUCommandBuffer *commandBuffer);
    void Submitted();

    // The picture texture's pixels (PictureWidth() x PictureHeight() RGBA8, rows PictureWidth() pixels apart; the host's CPU
    // picture is the same at scale 1). Waits for the GPU.
    bool Download(std::vector<uint8_t> &rgba);

    // For tests: the texture Record draws into.
    SDL_GPUTexture *PictureTexture() const;

    // For diagnostics (waits for the GPU): the sums the last "which way is the newest field offset" measurement
    // made (the column sums added up) and the answer it gave. False when there was no such measurement.
    bool DebugDecision(uint64_t &lowerSum, uint64_t &upperSum, bool &lower);

    // The picture texture holds kPictureWidth x kPictureHeight texels at scale 1 and that times the backend's scale above
    // (GSGpuBackend::PictureStride() / PictureRows()): in progressive mode the picture itself is 640 x 448 times the scale.
    static constexpr uint32_t kPictureWidth = 640;
    static constexpr uint32_t kPictureHeight = 512;
    uint32_t PictureWidth() const;
    uint32_t PictureHeight() const;
    // GPU memory the presenter's own textures take, in bytes.
    uint64_t MemoryBytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
