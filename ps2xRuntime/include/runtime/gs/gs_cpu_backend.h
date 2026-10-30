#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_texture_page_cache.h"

#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class GSCpuBackend final : public GSRasterBackend
{
public:
    GSCpuBackend();
    ~GSCpuBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    void ResetUnlocked();
    void LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    uint32_t ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    void DrawPrimitive(const GSPrimitiveBatch &batch);
    void DrawSprite(const GSPrimitiveBatch &batch);
    void DrawTriangle(const GSPrimitiveBatch &batch);
    void DrawLine(const GSPrimitiveBatch &batch);
    void WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);
    // FPWR: texture sampling goes through a per-primitive texel cache (decoded RGBA, indexed by
    // wrapped texel coordinates) and large primitives are rasterized in row bands on worker threads.
    // `pageCache` is the calling thread's VRAM page cache; `storeTexels` is false on worker threads,
    // which only read the texel cache.
    uint32_t SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v,
                           GSMem::TexturePageCache &pageCache, bool storeTexels);
    uint32_t DecodeTexel(const GSDrawState &state, int u, int v, GSMem::TexturePageCache &pageCache);
    uint32_t FetchTexel(const GSDrawState &state, int u, int v, GSMem::TexturePageCache &pageCache, bool storeTexels);
    void PrepareTexelCache(const GSPrimitiveBatch &batch, bool decodeFootprint);
    using RowBandFunc = std::function<void(int firstRow, int lastRow, GSMem::TexturePageCache &pageCache, bool storeTexels)>;
    void RasterizeRows(int firstRow, int lastRow, uint64_t pixelArea, const GSPrimitiveBatch &batch, const RowBandFunc &fn);
    uint32_t LookupCLUT(const GSDrawState &state, uint8_t index, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm);

    void UploadImageUnlocked(const uint8_t *data, uint32_t sizeBytes);
    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY) const;

    using WriteVramFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using ReadVramFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};
    std::array<uint16_t, 512> m_clut{};
    std::array<uint32_t, 2> m_clutCbp{};
    GSMem::TexturePageCache m_texturePageCache;

    struct TexelCache
    {
        std::vector<uint32_t> texels;
        std::vector<uint32_t> stamps; // texel is valid when stamps[i] == generation
        uint32_t generation = 0;
        int width = 0;
        int height = 0;
        bool active = false;
    };
    TexelCache m_texelCache;
    std::vector<GSMem::TexturePageCache> m_workerPageCaches;

    // FPWR: triangles and sprites are queued, then drawn together by all raster threads. Each
    // thread owns whole row bands for the length of a batch and walks it in order, so every
    // pixel still sees its primitives in submission order. A full queue is handed to a drawer
    // thread as a batch ("kick") and the caller carries on; batches are drawn one after another.
    // Ordering against everything else is kept as follows:
    // - entry points that read or write VRAM directly kick and wait until every batch is drawn;
    //   FINISH and Flush() only kick, and presentation waits without holding m_mutex;
    // - a primitive that writes memory the queue reads or writes differently, or reads memory the
    //   queue writes, kicks the queue first (batches are drawn in order, so no wait is needed);
    // - a CLUT load does not kick: each queued primitive keeps the CLUT it was queued with;
    // - a host-to-GS upload over memory queued primitives read saves the old pages first, and those
    //   primitives read the saved copies ("epochs" below). An upload over memory the queue writes
    //   kicks and waits;
    // - a kicked batch takes its own copy of every page it reads (after waiting for batches that
    //   write them), so an upload or CLUT load only waits for batches that write what it touches.
    // PS2X_GS_DEFER=0 turns the queue off.
    struct QueuedPrimitive
    {
        GSPrimitiveBatch batch;
        int minY;
        int maxY;
        uint32_t clut;  // index into Batch::cluts
        uint32_t epoch; // uploads that saved pages before this primitive was queued
    };
    struct VramRange
    {
        uint32_t begin; // bytes, [begin, end)
        uint32_t end;
        uint32_t key; // buffer base/width/format for writes; 0 for reads
    };
    struct SavedPage
    {
        uint32_t epoch; // content seen by primitives with epoch <= this
        std::vector<uint8_t> bytes;
    };
    struct Batch
    {
        std::vector<QueuedPrimitive> prims;
        std::vector<std::array<uint16_t, 512>> cluts;
        std::unordered_map<uint32_t, std::vector<SavedPage>> savedPages; // by page index, oldest first
        std::vector<VramRange> reads;
        std::vector<VramRange> writes;
    };
    struct BandContext
    {
        int first;
        int last;
        GSMem::TexturePageCache *pageCache;
        TexelCache *texelCache;
        const Batch *batch;
        uint32_t epoch;
    };
    bool QueuePrimitive(const GSPrimitiveBatch &batch);
    void CollectUploadRange(const GSTransferCommand &command, std::vector<VramRange> &out) const;
    bool PrepareUpload(const GSTransferCommand &command);
    void KickQueueUnlocked();                        // hand the queue to the drawer
    uint64_t KickedBatches() const;                  // batches kicked so far
    void WaitForBatches(uint64_t count) const;       // until `count` batches are drawn
    void FlushQueueUnlocked();                       // kick and wait until everything is drawn
    void WaitIfInFlight(const std::vector<VramRange> &ranges, bool includeReads);
    void DrawBatch(const Batch &batch);
    void DrawerMain();
    static const uint8_t *SavedPageSource(const void *band, uint32_t pageBase);
    TexelCache &ActiveTexelCache() { return t_band ? *t_band->texelCache : m_texelCache; }
    Batch m_pending; // the queue being filled
    std::vector<TexelCache> m_workerTexelCaches;
    bool m_clutChanged = true;
    uint32_t m_queueEpoch = 0;
    size_t m_queueSizeAtTransfer = 0;
    static thread_local BandContext *t_band;
    static thread_local const uint16_t *t_clut; // CLUT of the queued primitive being drawn

    mutable std::mutex m_drawMutex; // guards the fields below
    mutable std::condition_variable m_drawWake;
    mutable std::condition_variable m_drawDone;
    std::deque<std::unique_ptr<Batch>> m_inFlight; // front is being drawn
    uint64_t m_batchesKicked = 0;
    uint64_t m_batchesDrawn = 0;
    bool m_drawerStop = false;
    std::thread m_drawer;

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;
};
