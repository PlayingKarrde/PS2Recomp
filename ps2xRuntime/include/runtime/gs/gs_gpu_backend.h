#pragma once

// FPWR: GSGpuBackend, a GS renderer on SDL_GPU (GPU renderer milestones M2 and M3; design in docs/gpu-renderer.md).
//
// It implements GSRasterBackend like GSCpuBackend, so the same recorded clips can be replayed through
// either one (ps2x_gs_replay --backend gpu). The game uses it when PS2X_GS_BACKEND=gpu (gs_gpu_host.h).
//
// How it works:
//   - The 4 MiB GS memory image the backend is given (Initialize) is kept up to date by every host to
//     local transfer, exactly as GSCpuBackend does. It never holds drawn pixels: frame and Z buffers
//     exist only as GPU render targets. The game never reads back what it draws (GS census), so
//     ReadVram, SnapshotVram and local-to-host transfers return the memory image without them.
//   - Each primitive becomes vertices (gs_gpu_math.h) in a per-flush list, grouped into draws by pipeline
//     state, texture and scissor. Textures are decoded from the memory image on the CPU into RGBA8, in 16 x 16
//     texel tiles and only where a draw can sample, and cached on the GPU (see the cache comment in
//     gs_gpu_backend.cpp).
//   - Draws are recorded into one command buffer at Flush, Sync, Present or MarkPresentation (and when a
//     list gets large) and submitted at once.
//
// Field publishing (SetFieldPublishing(true), what the game uses): at each guest VBlank (MarkPresentation)
// the GS thread copies the displayed region of the frame target into one of kFieldSlots field textures on
// the GPU, in the same command buffer as the draws, and publishes it. The frame target keeps changing as the
// game draws the next field, so this copy, taken at the VBlank, is what the picture of that VBlank is. Two
// ways to get the picture out of it:
//   Path A, readback: Present() downloads the field and returns CPU pixels, exactly as
//     GSCpuBackend::Present does (the same GSPresent::Compose rules, run on the pixels). The runtime then
//     deinterlaces them on the CPU and uploads them to the window's texture. It is the reference and the
//     fallback (PS2X_GS_GPU_READBACK=1).
//   Path B, direct: the host's main thread takes the newest field (TakeNewestField), builds the 448-line
//     picture from the last three fields on the GPU (gs_gpu_present.h) and draws it into the window.
//     Nothing is read back. See "Field ring" in gs_gpu_backend.cpp for why the hand-over between the two
//     threads is safe without waiting.
// Without field publishing (the replayer's M2 mode, unit tests) Present() reads the frame targets as they are
// when it is called.
//
// Field modes (GPU renderer M4a). The game draws one 640 x 224 field per VBlank, and alternate fields are drawn half
// a line apart (by the Y offset OFY alone; bringup.md "GPU renderer").
//   Weave: the targets are the size of a field (times the scale), each published field is one 224-line picture
//     (FieldInfo::interlaced), and the presenter weaves the last three into 448 lines with a port of the CPU
//     deinterlacer. This is the M3 picture, kept for comparison with the CPU renderer.
//   Progressive: the targets are twice as tall (X scale `scale`, Y scale 2 * `scale`), the half line is removed from
//     the geometry (gs_gpu_math.h, TargetMapping), and every published field is already a 448-line picture. Only the
//     newest field is shown, nothing is woven, so a still scene gives the same picture every frame.
// Path A in progressive mode returns those 448-line pictures; the runtime's CPU deinterlacer only acts on pictures of
// at most 256 lines, so it leaves them alone.
//
// The device is not created here: the replayer creates a headless one, and the game passes the host's
// (ps2x_host::gpuDevice()). This header does not include SDL.

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <memory>

struct SDL_GPUDevice;
struct SDL_GPUTexture;

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
        uint64_t texturesReused = 0;      // textures made from a pooled GPU object (an evicted one of the same size) instead of SDL_CreateGPUTexture
        uint64_t textureBytesCreated = 0;
        uint64_t texLookups = 0;          // textured draws that asked the cache
        uint64_t texHitsRecent = 0;       // same texture as the previous primitive, memory unchanged
        uint64_t texHitsSignature = 0;    // known texture, its memory pages unchanged since
        uint64_t texHitsContent = 0;      // memory changed, but the bytes (and CLUT) are the same as a cached texture
        uint64_t texMisses = 0;           // decoded and uploaded
        uint64_t texDecodeNanos = 0;      // the decode loop of the misses (and the palette), not the staging
        // Where a texture miss and a flush spend their time, in nanoseconds on the GS thread (two clock reads per
        // step, only on the slow paths). Printed by PS2X_GS_GPU_STATS=1 and PS2X_GS_GPU_MARKER_LOG.
        uint64_t texContentLookups = 0;   // lookups that had to hash the texture's pages (the third cache level)
        uint64_t texHashNanos = 0;        // ... and what that hashing took
        uint64_t texHashBytes = 0;
        uint64_t texCreateNanos = 0;      // SDL_CreateGPUTexture of the misses
        uint64_t texStageNanos = 0;       // allocateStage for the misses (growing and zero-filling the stage vector)
        uint64_t texTexelsDeclared = 0;   // width x height of the missed textures
        uint64_t texTexelsDecoded = 0;    // texels the decode loop actually produced (the tiles draws can reach, see decodeTexels)
        uint64_t texFillRegions = 0;      // rectangles decoded and uploaded to a texture
        uint64_t flushMapNanos = 0;       // ensureBuffers, map, memcpy of vertices and stage, unmap
        uint64_t flushRecordNanos = 0;    // acquire the command buffer, record copy and render passes
        uint64_t flushSubmitNanos = 0;    // SDL_SubmitGPUCommandBuffer
        uint64_t flushEvictNanos = 0;     // evictTextures and the release of transient textures
        uint64_t flushTotalNanos = 0;     // flushLocked from start to end (also the waits of wait=true and the backlog)
        uint64_t markNanos = 0;           // MarkPresentation, all of it
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
        // Direct presentation (path B)
        uint64_t presentsFromFields = 0;  // Present() calls answered from the published field
        uint64_t backlogWaits = 0;        // markers at which the GS thread waited for the GPU to catch up (see kMaxFramesInFlight)
        uint64_t backlogWaitNanos = 0;    // ... and how long it waited in all
        uint64_t fieldsFast = 0;          // fields copied on the GPU
        uint64_t fieldsWithCandidates = 0; // ... of which also copied the fallback frames (display frame at fbp 0)
        uint64_t fieldsSlow = 0;          // fields that needed pixels on the CPU (Compose with real pixels)
        uint64_t fieldsInvalid = 0;       // markers with no picture (the display is not set up)
        uint64_t fieldsDropped = 0;       // fields not published because every slot was in use
        uint64_t signaturesLive = 0;      // entries of the texture signature map now (not cleared by ResetStats)
        uint64_t textureBytesLive = 0;    // bytes of cached GPU textures now (not cleared by ResetStats)
        uint64_t texturePoolBytes = 0;    // bytes of evicted GPU textures kept for reuse now (not cleared by ResetStats)
        // GPU memory the render path holds now, in bytes (not cleared by ResetStats). With the texture cache above these make
        // up the backend's share; the host's presenter has its own textures (GSGpuPresenter::MemoryBytes).
        uint64_t frameTargetBytes = 0;    // colour targets
        uint64_t depthTargetBytes = 0;
        uint64_t fieldTextureBytes = 0;   // the field ring
        uint64_t uploadBufferBytes = 0;   // the transfer buffer that stages uploads (host-visible memory)
        uint64_t vertexBufferBytes = 0;
        // Texture replacement (task t1b); zero without a texture pack.
        uint64_t repCreated = 0;          // replacement pictures made on the GPU
        uint64_t repEvicted = 0;          // ... and released again over the budget
        uint64_t repDrawCalls = 0;        // draw calls that used a replacement (gs_rep.frag.hlsl)
    };

    // Texture packs, for the F1 panel (any thread). Counts of the last completed frame (VBlank) and of the whole run.
    struct ReplacementReport
    {
        bool active = false;              // a texture pack is loaded
        bool dumping = false;             // PS2X_TEX_DUMP is on
        uint32_t packFiles = 0;           // texture files indexed
        uint32_t decoded = 0;             // pictures decoded so far ("loaded replacements")
        uint32_t refused = 0;             // files refused (their reasons are in the Mods tab and the log)
        uint32_t pending = 0;             // pictures waiting to be decoded
        uint32_t texturesThisFrame = 0;   // distinct replacement pictures drawn in the last frame
        uint32_t drawsThisFrame = 0;      // draws that used one
        uint64_t gpuBytes = 0;            // replacement pictures on the GPU
        uint64_t cacheBytes = 0;          // decoded pictures held in memory
        uint32_t dumpedFiles = 0;
    };
    ReplacementReport Replacement() const;
    // Reads the texture pack files again (the F1 panel's button; any thread). Textures asked again at their next draw; pictures changed
    // on disk show up. The set of mods is the one the game started with. Returns the number of texture files, 0 without a pack.
    uint32_t ReloadTexturePacks();

    // One field published by the GS thread for the host (path B). A field uses one slot for the displayed
    // region, and up to two more for the frame buffers Compose falls back to when the displayed one is
    // black (see GSPresent::Compose); the host tests for black and chooses on the GPU. In the worst case
    // 3 slots are pinned by the host, 3 are the latest field's and 3 are being written.
    static constexpr int kFieldSlots = 9;
    static constexpr uint32_t kFieldTextureWidth = 640;
    static constexpr uint32_t kFieldTextureHeight = 512;
    // How the targets relate to the game's GS frames; see "Field modes" above.
    enum class FieldMode : uint8_t
    {
        Weave,
        Progressive
    };

    struct FieldInfo
    {
        uint64_t seq = 0;             // 1, 2, 3 ... in publication order; 0 = nothing published yet
        int slot = -1;                // index of the field texture; -1 when !valid
        int candidates[2] = {-1, -1}; // slots holding the fallback frames' regions (same size), in Compose's order
        int candidateCount = 0;
        // What the slots hold, so Present() can run Compose on them: the displayed frame buffer and the region
        // read from it, and the fallback frames (read from their top-left corner).
        GSFrameReg displayFrame{};
        uint32_t originX = 0, originY = 0;
        GSFrameReg candidateFrames[2]{};
        bool valid = false;           // false: this marker had no picture (the display is not set up)
        bool composed = false;        // the slot holds Compose's finished picture (it needed real pixels), not a frame region
        // The picture, in the top-left corner of the slot texture, in texels. In progressive mode a field of 640 x 224 GS
        // pixels is 640 x 448 texels (times the scale): see scaleX and scaleY.
        uint32_t width = 0, height = 0;
        // Weave mode only: SMODE2.INT, the picture is one field of an interlaced frame that the presenter has to weave.
        // Always false in progressive mode, where a field is already a full-height picture.
        bool interlaced = false;
        // Texels per GS pixel of the frame region the slot holds (1 for a composed picture, which is in GS pixels).
        uint32_t scaleX = 1, scaleY = 1;
        bool progressive = false;     // the field was published in progressive mode
        uint32_t displayFbp = 0, sourceFbp = 0;
        bool usedPreferred = false;
    };

    // What MarkPresentation did, for the frontend's debug view.
    struct MarkResult
    {
        bool published = false;       // field publishing is on
        bool direct = false;          // and the host draws the fields itself (path B): nothing calls Present()
        bool valid = false;
        uint32_t width = 0, height = 0, displayFbp = 0, sourceFbp = 0;
        bool usedPreferred = false;
    };

    // `scale` multiplies the render target size (internal resolution, task g4b): 1 to 4 have been exercised, and weave mode
    // only draws at 1 (a larger scale is refused with a log line and 1 is used). `fields` says how the game's field pictures
    // are drawn and shown (see "Field modes" above); weave is the M3 behaviour.
    GSGpuBackend(SDL_GPUDevice *device, uint32_t scale = 1, FieldMode fields = FieldMode::Weave);
    ~GSGpuBackend() override;
    GSGpuBackend(const GSGpuBackend &) = delete;
    GSGpuBackend &operator=(const GSGpuBackend &) = delete;

    // False when the shaders or the sampler could not be created (the reason is logged).
    bool Ready() const;

    FieldMode Fields() const;
    // Target pixels per GS pixel along each axis: the scale in X, and in Y twice that in progressive mode.
    uint32_t TargetScaleX() const;
    uint32_t TargetScaleY() const;
    // The pictures of the field textures and of Present() when fields are published: 640 x 512 texels times the scale (the
    // picture is 640 x 448 times the scale in progressive mode; rows are PictureStride() pixels apart). Without field
    // publishing Present() returns GS-pixel pictures, 640 pixels per row (kPresentationStride).
    uint32_t PictureStride() const;
    uint32_t PictureRows() const;
    // Widescreen prototype (task w1; PS2X_GS_WIDE=<W:H>, read when the backend is created, progressive mode only, off by default):
    // the targets and field textures are (640 + WidePadLeft() + WidePadRight()) GS pixels wide, so the picture shows that many extra
    // columns of the scene left and right of the game's 640. Both are 0 when it is off. The picture's shape, as width / height
    // of the box it should be shown in, is WideAspect() (4/3 when off); it is the width in GS pixels over 480.
    uint32_t WidePadLeft() const;
    uint32_t WidePadRight() const;
    float WideAspect() const;
    // Turns widescreen off again; call before the first draw and before SetFieldPublishing (the host does for path A, which carries a 640-wide picture).
    void DisableWide();

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
    // the draws recorded so far, which are what the displayed frame buffers hold at this point. With
    // field publishing on it also copies the displayed region into a field texture and publishes it.
    // Call it from the one thread that draws (the game's GS thread).
    MarkResult MarkPresentation(const GSPresentationRequest &request);

    // ---- Field publishing (paths A and B) ----
    // Turns it on (creates the field textures). False, with a log line, when the device cannot do it.
    // Do this before the game starts drawing. Present() then answers from the newest published field.
    // `directConsumer` says the host takes the fields itself (GSGpuPresenter) and never calls Present(); the
    // frontend then keeps its debug view of the presentation current from MarkResult.
    bool SetFieldPublishing(bool enabled, bool directConsumer = false);
    bool FieldPublishing() const;

    // Consumer side, any thread, never waits for the GS thread or the GPU (only a lock held for a few
    // instructions). Takes the newest field published after `afterSeq` and pins its slots (see UnpinField);
    // false when there is none. An invalid field (no picture) pins nothing.
    bool TakeNewestField(uint64_t afterSeq, FieldInfo &out);
    // A pinned slot is never overwritten. The consumer holds a pin for as long as it still has to submit
    // commands that read the slot (and for as long as it wants the field as history), then unpins.
    void PinField(int slot);
    void UnpinField(int slot);
    SDL_GPUTexture *FieldTexture(int slot) const;
    SDL_GPUDevice *Device() const;

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
