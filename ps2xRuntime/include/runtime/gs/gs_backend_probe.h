#pragma once

// FPWR: an opt-in timing wrapper for any GSRasterBackend (PS2X_GS_STATS=1).
//
// It forwards every call to the backend it wraps, and measures on the calling thread how long each
// kind of call took and which thread made it. Once a second it prints one line:
//
//   [gs:stats] backend=gpu t=12.0s markers=60 calls/frame=3012 backend_ms/frame=1.234 | Submit 2900 0.80ms max 0.12 [GameThread] | ...
//
// "backend_ms/frame" is the time the calling thread spent inside backend calls (including waiting
// for the backend's own lock and for the GPU where a call does so), summed over all kinds, divided
// by the number of presentation markers (guest VBlanks) in that second. A call that is made from two
// threads shows both thread names. The wrapper itself costs two clock reads per call.
//
// It adds nothing to the game when the variable is unset: nothing is wrapped. No header that
// generated game code includes refers to this one.

#include "runtime/gs/gs_backend.h"

#include <functional>
#include <memory>

class GSBackendProbe final : public GSRasterBackend
{
public:
    GSBackendProbe(std::unique_ptr<GSRasterBackend> inner, const char *label);
    ~GSBackendProbe() override;

    GSRasterBackend *Inner() const;

    // Times `doMark` (the frontend's MarkPresentation call on the wrapped backend) and counts a marker.
    void TimedMark(const std::function<void()> &doMark);

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
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
