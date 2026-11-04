#pragma once

// FPWR: GSGpuBackend, a GS renderer on SDL_GPU (GPU renderer milestone M2; design in docs/gpu-renderer.md).
//
// It implements GSRasterBackend like GSCpuBackend, so the same recorded clips can be replayed through
// either one (ps2x_gs_replay --backend gpu). In M2 it is only used by the replayer; the game still draws
// with GSCpuBackend.
//
// How it works:
//   - The 4 MiB GS memory image the backend is given (Initialize) is kept up to date by every host to
//     local transfer, exactly as GSCpuBackend does. It never holds drawn pixels: frame and Z buffers
//     exist only as GPU render targets. The game never reads back what it draws (GS census), so
//     ReadVram, SnapshotVram and local-to-host transfers return the memory image without them.
//   - Each primitive becomes vertices (gs_gpu_math.h) in a per-flush list, grouped into draws by pipeline
//     state, texture and scissor. Textures are decoded from the memory image on the CPU into RGBA8 and
//     cached on the GPU (see the cache comment in gs_gpu_backend.cpp).
//   - Draws are recorded into one command buffer at Flush, Sync, Present or MarkPresentation (and when a
//     list gets large) and submitted at once.
//
// The device is not created here: the replayer creates a headless one, and the game will pass the host's
// (ps2x_host::gpuDevice()). This header does not include SDL.

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <memory>

struct SDL_GPUDevice;

namespace GSRec
{
    struct BackendState;
}

class GSGpuBackend final : public GSRasterBackend
{
public:
    // Counters for the replayer's report. Reset with ResetStats().
    struct Stats
    {
        uint64_t primitives = 0;          // Submit calls with a drawable primitive type
        uint64_t primitivesCulled = 0;    // dropped before drawing: no area, or outside the scissor
        uint64_t primitivesUnsupported = 0; // points and lines (the clips have none)
        uint64_t vertices = 0;
        uint64_t drawCalls = 0;
        uint64_t renderPasses = 0;
        uint64_t flushes = 0;             // command buffers submitted with draws or uploads
        uint64_t maxDrawsPerFlush = 0;
        uint64_t pipelinesCreated = 0;    // cumulative since construction (not cleared by ResetStats)
        uint64_t texturesCreated = 0;
        uint64_t texturesEvicted = 0;
        uint64_t textureBytesCreated = 0;
        uint64_t texLookups = 0;          // textured draws that asked the cache
        uint64_t texHitsRecent = 0;       // same texture as the previous primitive, memory unchanged
        uint64_t texHitsSignature = 0;    // known texture, its memory pages unchanged since
        uint64_t texHitsContent = 0;      // memory changed, but the bytes (and CLUT) are the same as a cached texture
        uint64_t texMisses = 0;           // decoded and uploaded
        uint64_t texDecodeNanos = 0;
        uint64_t uploadTransfers = 0;     // BeginTransfer calls that wrote GS memory
        uint64_t uploadBytes = 0;
        uint64_t depthRestores = 0;       // times Z pages changed by GS memory writes were copied into the depth target
        uint64_t depthRestoreTiles = 0;   // 64 x 32 pixel pages copied in those
        uint64_t frameTargets = 0;        // colour targets created
        uint64_t depthTargets = 0;
        uint64_t blendUnsupported = 0;    // prims whose blend equation has no exact fixed-function form
        uint64_t ignoredFeatures = 0;     // prims using a GS feature the backend does not implement (see the log)
        uint64_t presents = 0;
        uint64_t downloadBytes = 0;
    };

    // `scale` multiplies the render target size; only 1 is supported in M2.
    GSGpuBackend(SDL_GPUDevice *device, uint32_t scale = 1);
    ~GSGpuBackend() override;
    GSGpuBackend(const GSGpuBackend &) = delete;
    GSGpuBackend &operator=(const GSGpuBackend &) = delete;

    // False when the shaders or the sampler could not be created (the reason is logged).
    bool Ready() const;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;

    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    // Submits everything, then reads the displayed frame buffers back and composes the picture the way
    // GSCpuBackend::Present does (gs_presentation.cpp). The picture is the buffers as they are now; call
    // MarkPresentation at the vertical blank and Present right after it.
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    // Memory image only: it never holds pixels drawn by this backend.
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    // Called at each guest vertical blank (GSCpuBackend::MarkPresentation does the same job): submits
    // the draws recorded so far, which are what the displayed frame buffers hold at this point.
    void MarkPresentation(const GSPresentationRequest &request);

    // The backend state a recording starts from (CLUT, transfer in progress, pending local-to-host
    // bytes). The recording's texture page cache has no equivalent here.
    void ImportRecordingState(const GSRec::BackendState &state);

    Stats GetStats() const;
    void ResetStats();

    // Row stride, in pixels, of the pictures Present() returns (the same as GSCpuBackend).
    static constexpr uint32_t kPresentationStride = 640u;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
