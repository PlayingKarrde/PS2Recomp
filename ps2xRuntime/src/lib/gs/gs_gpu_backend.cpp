// GSGpuBackend: the GS renderer on SDL_GPU. See include/runtime/gs/gs_gpu_backend.h for the overview and
// docs/gpu-renderer.md for the design. This file is only compiled for desktop and mobile builds (it
// needs SDL); nothing the generated game code includes refers to it.
//
// Reading guide:
//   Impl::Submit             primitive -> vertices (gs_gpu_math.h), draw grouping
//   Impl::ResolveTexture     the texture cache (comment below)
//   Impl::FlushLocked        one command buffer: uploads, then render passes, then submit
//   Impl::Present            reads the displayed frame buffers back and composes the picture
//
// Texture cache. A GS texture is a swizzled region of the 4 MiB memory image that the game overwrites
// again and again (it streams textures through a small VRAM area, re-uploading the same data often).
// A GPU texture is immutable once created and tied to its content, so draws that were recorded before
// an upload still see the old data, and no flush is needed when an upload lands. Lookup has three
// levels, cheapest first:
//   1. "recent": same signature as the previous textured primitive and no memory write since.
//   2. signature: key = TBP, TBW, PSM, size, TEXA, and the resolved CLUT (indexed formats). The entry
//      remembers the epoch at which it was checked; it is still valid if none of the texture's 8 KiB
//      pages was written after that (every host to local transfer and WriteVram stamps the pages it
//      touches with a new epoch).
//   3. content: when the pages were written, hash their bytes (XXH64) together with the signature. The
//      same bytes mean the same texture, so a re-upload of identical data costs a hash, not a decode.
// Only a miss on all three decodes the texture on the CPU (GSMem::ReadTexture, then TEXA or the CLUT)
// into RGBA8 and creates a GPU texture. Cached textures are evicted least recently used first once
// they pass a memory budget, and never while the current draw list uses them.

#include "runtime/gs/gs_gpu_backend.h"

#include "runtime/gs/gs_gpu_math.h"
#include "runtime/gs/gs_presentation.h"
#include "runtime/gs/gs_recording.h"
#include "runtime/gs/gs_texel.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/ps2_gs_transfer.h"

#include "gpu/gpu_shaders.h"
#include "shaders/compiled/gs_frag.h"
#include "shaders/compiled/gszcopy_frag.h"
#include "shaders/compiled/gs_vert.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace GSGpu;
using namespace GSTexel;

namespace
{
    constexpr uint32_t kPageCount = 512u;
    constexpr uint32_t kPageBytes = 8192u;
    constexpr uint32_t kTargetRows = 512u;                 // rows of a frame or depth target (GS limit used by the CPU renderer)
    constexpr uint64_t kTextureBudget = 384ull << 20;      // bytes of cached GPU textures before eviction starts
    constexpr size_t kMaxVerticesPerFlush = 2u << 20;      // a list this long is submitted early
    constexpr size_t kMaxStageBytes = 192u << 20;          // so is one with this much texture data waiting

    uint64_t nowNanos()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    size_t alignUp(size_t v, size_t a)
    {
        return (v + a - 1u) & ~(a - 1u);
    }

    struct GpuTexture
    {
        SDL_GPUTexture *tex = nullptr;
        uint32_t w = 0, h = 0;
        uint64_t bytes = 0;
        uint64_t lastUsedFlush = 0;
    };

    struct SigKey
    {
        uint32_t tbp0, tbwPsm, size, texa;
        uint64_t clutHash;
        bool operator==(const SigKey &o) const
        {
            return tbp0 == o.tbp0 && tbwPsm == o.tbwPsm && size == o.size && texa == o.texa && clutHash == o.clutHash;
        }
    };
    struct SigKeyHash
    {
        size_t operator()(const SigKey &k) const
        {
            uint64_t h = k.clutHash;
            h ^= (static_cast<uint64_t>(k.tbp0) << 32 | k.tbwPsm) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            h ^= (static_cast<uint64_t>(k.size) << 32 | k.texa) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            return static_cast<size_t>(h);
        }
    };
    struct SigEntry
    {
        uint64_t contentHash;
        uint64_t epoch; // memory epoch at which the pages were last known to match
        PageSpan span;
    };

    struct PendingUpload // data waiting in the stage buffer to be copied into a texture
    {
        SDL_GPUTexture *tex;
        uint32_t w, h;
        size_t offset;
    };

    struct FrameTarget
    {
        SDL_GPUTexture *color = nullptr;
        uint32_t fbp = 0, fbw = 0, psm = 0;
        uint32_t w = 0, h = 0; // texture size in pixels (scale applied)
    };
    struct DepthTarget
    {
        SDL_GPUTexture *depth = nullptr;
        uint32_t zbp = 0, fbw = 0, w = 0, h = 0;
        bool needsClear = true; // cleared to Z = 0 the first time a pass uses it
        // The depth target only follows draws. GS memory is the other writer of Z pages (uploads: the entrance
        // movie puts CT32 frames into the Z buffer), so the pages whose memory changed are "dirty" and are
        // copied into the target (decoded as Z24) before the next draw that uses it. A page covers a
        // 64 x 32 pixel tile; `coveredRows` is how many rows draws have used so far.
        uint32_t coveredRows = 0;
        std::bitset<kPageCount> dirty;
    };

    struct Draw
    {
        uint32_t firstVertex = 0, vertexCount = 0;
        uint32_t frame = 0, depth = 0;
        uint32_t pipeline = 0;
        FragUniforms uniforms{};
        SDL_GPUTexture *texture = nullptr; // nullptr: the 1x1 white dummy
        int32_t sx = 0, sy = 0, sw = 0, sh = 0; // scissor, in target pixels
    };

    SDL_GPUBlendFactor toSdl(BlendFactor f)
    {
        switch (f)
        {
        case BlendFactor::Zero: return SDL_GPU_BLENDFACTOR_ZERO;
        case BlendFactor::One: return SDL_GPU_BLENDFACTOR_ONE;
        case BlendFactor::SrcAlpha: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
        }
        return SDL_GPU_BLENDFACTOR_ONE;
    }

    SDL_GPUBlendOp toSdl(BlendOp o)
    {
        switch (o)
        {
        case BlendOp::Add: return SDL_GPU_BLENDOP_ADD;
        case BlendOp::Subtract: return SDL_GPU_BLENDOP_SUBTRACT;
        case BlendOp::ReverseSubtract: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
        }
        return SDL_GPU_BLENDOP_ADD;
    }

    SDL_GPUCompareOp toSdlCompare(uint32_t gsZtst)
    {
        switch (gsZtst)
        {
        case 0: return SDL_GPU_COMPAREOP_NEVER;
        case 1: return SDL_GPU_COMPAREOP_ALWAYS;
        case 2: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        default: return SDL_GPU_COMPAREOP_GREATER;
        }
    }

    // Pipeline state packed into 16 bits: blend factors and op, whether blending is on, depth compare, depth write.
    constexpr uint32_t kZRestoreKey = 1u << 12; // the pipeline that copies decoded Z pages into the depth target

    uint32_t pipelineKey(const BlendState &blend, uint32_t compare, bool depthWrite)
    {
        return static_cast<uint32_t>(blend.src) | (static_cast<uint32_t>(blend.dst) << 3) | (static_cast<uint32_t>(blend.op) << 6) |
               (blend.enable ? 1u << 8 : 0u) | ((compare & 3u) << 9) | (depthWrite ? 1u << 11 : 0u);
    }
}

struct GSGpuBackend::Impl
{
    SDL_GPUDevice *dev = nullptr;
    uint32_t scale = 1;
    mutable std::mutex mutex;
    Stats stats;
    bool ready = false;

    // ---- GS memory image and the state outside it
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0;
    uint64_t epoch = 1;
    std::array<uint64_t, kPageCount> pageEpoch{};
    std::array<uint16_t, 512> clut{};
    std::array<uint32_t, 2> clutCbp{};
    uint64_t clutVersion = 1;
    GSTransferCommand transfer{};
    GSTransferSnapshot transferState{};
    std::vector<uint8_t> localToHost;
    size_t localToHostPos = 0;

    // ---- GPU objects
    SDL_GPUShader *vertexShader = nullptr;
    SDL_GPUShader *fragmentShader = nullptr;
    SDL_GPUShader *zcopyShader = nullptr;
    SDL_GPUSampler *sampler = nullptr;
    SDL_GPUTexture *dummy = nullptr;
    std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline *> pipelines;
    SDL_GPUBuffer *vertexBuffer = nullptr;
    size_t vertexBufferBytes = 0;
    SDL_GPUTransferBuffer *uploadBuffer = nullptr;
    size_t uploadBufferBytes = 0;
    std::vector<FrameTarget> frames;
    std::vector<DepthTarget> depths;

    // ---- texture cache
    std::unordered_map<uint64_t, GpuTexture> contentTextures; // by content hash
    std::unordered_map<SigKey, SigEntry, SigKeyHash> signatures;
    uint64_t textureBytes = 0;
    uint64_t flushIndex = 1;
    bool recentValid = false;
    SigKey recentKey{};
    uint64_t recentEpoch = 0;
    SDL_GPUTexture *recentTexture = nullptr;
    struct ClutMemo
    {
        bool valid = false;
        uint64_t version = 0;
        uint32_t cpsm = 0, csm = 0, csa = 0, psm = 0, texa = 0;
        uint64_t hash = 0;
    } clutMemo;

    // ---- the draw list being recorded
    std::vector<Vertex> vertices;
    std::vector<Draw> draws;
    std::vector<uint8_t> stage; // texture and seed pixels waiting to be uploaded
    std::vector<PendingUpload> pendingUploads;
    std::vector<SDL_GPUTexture *> transientTextures; // decoded Z pages: released once the list is submitted
    GSPresentationRequest lastMarker{};
    bool loggedUnsupported[16] = {};

    ~Impl();
    bool init(SDL_GPUDevice *device, uint32_t s);

    // memory
    void touchSpan(const PageSpan &span);
    void touchAll();
    void touchBuffer(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    uint64_t spanEpoch(const PageSpan &span) const;

    // drawing
    void submit(const GSPrimitiveBatch &batch);
    void submitSetup(const GSDrawState &state, const Setup &setup);
    void note(unsigned kind, const char *text);
    uint32_t frameIndex(const GSFrameReg &frame);
    uint32_t depthIndex(const GSZbufReg &zbuf, uint32_t fbw);
    void markDepthDirty(const PageSpan &touched);
    void restoreDepth(uint32_t depthIdx, uint32_t frameIdx, uint32_t rows);
    SDL_GPUGraphicsPipeline *pipeline(uint32_t key);
    SDL_GPUTexture *resolveTexture(const GSDrawState &state);
    uint64_t clutHash(const GSDrawState &state);
    SDL_GPUTexture *createTexture(const GSDrawState &state, uint64_t contentHash, uint32_t w, uint32_t h);
    size_t allocateStage(size_t bytes);
    void flushLocked(bool wait);
    void evictTextures();
    bool ensureBuffers(size_t vertexBytes, size_t transferBytes);

    // readback
    bool downloadRegion(SDL_GPUTexture *tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint8_t> &out);
    bool copyFrame(const GSFrameReg &frame, uint32_t width, uint32_t height, std::vector<uint8_t> &out, bool preserveAlpha,
                   bool useLocalMemoryLayout, bool frameBaseIsPages, uint32_t originX, uint32_t originY);
};

// ------------------------------------------------------------------------------------------------
// setup and teardown

bool GSGpuBackend::Impl::init(SDL_GPUDevice *device, uint32_t s)
{
    dev = device;
    scale = std::max(1u, s);
    if (!dev)
    {
        std::fprintf(stderr, "[gs:gpu] no GPU device\n");
        return false;
    }
    if (!SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
    {
        std::fprintf(stderr, "[gs:gpu] D32_FLOAT depth targets are not supported by this device\n");
        return false;
    }
    vertexShader = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::gs_vert);
    fragmentShader = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::gs_frag);
    zcopyShader = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::gszcopy_frag);
    if (!vertexShader || !fragmentShader || !zcopyShader)
        return false;

    SDL_GPUSamplerCreateInfo si = {};
    si.min_filter = SDL_GPU_FILTER_NEAREST;
    si.mag_filter = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler = SDL_CreateGPUSampler(dev, &si);
    if (!sampler)
    {
        std::fprintf(stderr, "[gs:gpu] SDL_CreateGPUSampler: %s\n", SDL_GetError());
        return false;
    }

    // A 1x1 white texture for draws without a texture (the shader still has a sampler bound).
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = 1;
    ti.height = 1;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    dummy = SDL_CreateGPUTexture(dev, &ti);
    if (!dummy)
    {
        std::fprintf(stderr, "[gs:gpu] SDL_CreateGPUTexture(dummy): %s\n", SDL_GetError());
        return false;
    }
    const size_t offset = allocateStage(4);
    const uint32_t white = 0xFFFFFFFFu;
    std::memcpy(stage.data() + offset, &white, 4);
    pendingUploads.push_back({dummy, 1, 1, offset});
    return true;
}

GSGpuBackend::Impl::~Impl()
{
    if (!dev)
        return;
    SDL_WaitForGPUIdle(dev);
    for (auto &p : pipelines)
        SDL_ReleaseGPUGraphicsPipeline(dev, p.second);
    for (auto &t : contentTextures)
        SDL_ReleaseGPUTexture(dev, t.second.tex);
    for (auto &f : frames)
        SDL_ReleaseGPUTexture(dev, f.color);
    for (auto &d : depths)
        SDL_ReleaseGPUTexture(dev, d.depth);
    if (dummy)
        SDL_ReleaseGPUTexture(dev, dummy);
    if (vertexBuffer)
        SDL_ReleaseGPUBuffer(dev, vertexBuffer);
    if (uploadBuffer)
        SDL_ReleaseGPUTransferBuffer(dev, uploadBuffer);
    if (sampler)
        SDL_ReleaseGPUSampler(dev, sampler);
    if (vertexShader)
        SDL_ReleaseGPUShader(dev, vertexShader);
    if (fragmentShader)
        SDL_ReleaseGPUShader(dev, fragmentShader);
    if (zcopyShader)
        SDL_ReleaseGPUShader(dev, zcopyShader);
    for (SDL_GPUTexture *t : transientTextures)
        SDL_ReleaseGPUTexture(dev, t);
}

void GSGpuBackend::Impl::note(unsigned kind, const char *text)
{
    ++stats.ignoredFeatures;
    if (kind < 16 && !loggedUnsupported[kind])
    {
        loggedUnsupported[kind] = true;
        std::fprintf(stderr, "[gs:gpu] not implemented, drawn without it: %s (logged once)\n", text);
    }
}

// ------------------------------------------------------------------------------------------------
// GS memory epochs

void GSGpuBackend::Impl::touchSpan(const PageSpan &span)
{
    if (span.count == 0)
        return;
    ++epoch;
    for (uint32_t i = 0; i < span.count; ++i)
        pageEpoch[(span.first + i) & (kPageCount - 1u)] = epoch;
}

void GSGpuBackend::Impl::touchAll()
{
    ++epoch;
    pageEpoch.fill(epoch);
}

void GSGpuBackend::Impl::touchBuffer(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const PageSpan span = BufferPageSpan(psm, base, bw, x, y, w, h);
    touchSpan(span);
    markDepthDirty(span);
}

// A write to GS memory (an upload, a local copy) that lands on pages of a Z buffer: those pages are copied into
// the depth target before the next draw that uses it (restoreDepth).
void GSGpuBackend::Impl::markDepthDirty(const PageSpan &touched)
{
    for (DepthTarget &d : depths)
    {
        if (d.coveredRows == 0)
            continue; // not used by a draw yet: it is filled from memory when it first is
        const uint32_t tileRows = (d.coveredRows + 31u) / 32u;
        for (uint32_t tr = 0; tr < tileRows; ++tr)
            for (uint32_t tc = 0; tc < d.fbw; ++tc)
            {
                const uint32_t page = (d.zbp + tr * d.fbw + tc) & (kPageCount - 1u);
                // is the page inside the touched span (which may wrap around the end of memory)?
                const uint32_t offset = (page + kPageCount - touched.first) & (kPageCount - 1u);
                if (offset < touched.count)
                    d.dirty.set(page);
            }
    }
}

// Copies the dirty Z pages of a depth target (Z24 pixels decoded from the memory image) into it, as a draw
// recorded ahead of the primitive that is about to use it. `rows` is how many rows that primitive's scissor
// reaches; rows not used before are filled from memory too.
void GSGpuBackend::Impl::restoreDepth(uint32_t depthIdx, uint32_t frameIdx, uint32_t rows)
{
    DepthTarget &d = depths[depthIdx];
    rows = std::min(rows, d.h / scale);
    if (rows > d.coveredRows)
    {
        for (uint32_t tr = d.coveredRows / 32u; tr < (rows + 31u) / 32u; ++tr)
            for (uint32_t tc = 0; tc < d.fbw; ++tc)
                d.dirty.set((d.zbp + tr * d.fbw + tc) & (kPageCount - 1u));
        d.coveredRows = rows;
    }
    if (d.dirty.none())
        return;

    const uint32_t tileRows = (d.coveredRows + 31u) / 32u;
    const uint32_t texW = d.fbw * 64u, texH = tileRows * 32u;
    const size_t offset = allocateStage(static_cast<size_t>(texW) * texH * 4u);
    uint8_t *px = stage.data() + offset;
    std::memset(px, 0, static_cast<size_t>(texW) * texH * 4u);
    std::vector<std::pair<uint32_t, uint32_t>> tiles;
    for (uint32_t tr = 0; tr < tileRows; ++tr)
        for (uint32_t tc = 0; tc < d.fbw; ++tc)
        {
            if (!d.dirty.test((d.zbp + tr * d.fbw + tc) & (kPageCount - 1u)))
                continue;
            tiles.emplace_back(tr, tc);
            for (uint32_t y = 0; y < 32u; ++y)
                for (uint32_t x = 0; x < 64u; ++x)
                {
                    const uint32_t z = GSTransfer::ReadPixel(vram, GS_PSM_Z24, d.zbp << 5u, d.fbw, tc * 64u + x, tr * 32u + y) & 0xFFFFFFu;
                    uint8_t *p = px + (static_cast<size_t>(tr * 32u + y) * texW + tc * 64u + x) * 4u;
                    p[0] = static_cast<uint8_t>(z);
                    p[1] = static_cast<uint8_t>(z >> 8);
                    p[2] = static_cast<uint8_t>(z >> 16);
                    p[3] = 0xFF;
                }
        }
    d.dirty.reset();

    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = texW;
    ti.height = texH;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *tex = SDL_CreateGPUTexture(dev, &ti);
    if (!tex)
    {
        std::fprintf(stderr, "[gs:gpu] cannot create the %ux%u Z page texture: %s\n", texW, texH, SDL_GetError());
        return;
    }
    pendingUploads.push_back({tex, texW, texH, offset});
    transientTextures.push_back(tex);

    Draw draw;
    draw.frame = frameIdx;
    draw.depth = depthIdx;
    draw.pipeline = kZRestoreKey | pipelineKey(BlendState{}, 1u, true);
    draw.texture = tex;
    draw.firstVertex = static_cast<uint32_t>(vertices.size());
    const FrameTarget &target = frames[frameIdx];
    draw.sx = 0;
    draw.sy = 0;
    draw.sw = static_cast<int32_t>(target.w);
    draw.sh = static_cast<int32_t>(target.h);
    const float fs = static_cast<float>(scale);
    for (const auto &t : tiles)
    {
        const float x0 = static_cast<float>(t.second * 64u), y0 = static_cast<float>(t.first * 32u);
        const float u0 = x0 / static_cast<float>(texW), u1 = (x0 + 64.0f) / static_cast<float>(texW);
        const float v0 = y0 / static_cast<float>(texH), v1 = (y0 + 32.0f) / static_cast<float>(texH);
        auto make = [&](float x, float y, float u, float v)
        {
            Vertex r;
            r.x = x * fs;
            r.y = y * fs;
            r.z = 0.0f;
            r.rgba = 0xFFFFFFFFu;
            r.u = u;
            r.v = v;
            r.q = 1.0f;
            return r;
        };
        const Vertex tl = make(x0, y0, u0, v0), tr = make(x0 + 64.0f, y0, u1, v0), br = make(x0 + 64.0f, y0 + 32.0f, u1, v1),
                     bl = make(x0, y0 + 32.0f, u0, v1);
        const Vertex quad[6] = {tl, tr, br, tl, br, bl};
        vertices.insert(vertices.end(), quad, quad + 6);
    }
    draw.vertexCount = static_cast<uint32_t>(tiles.size() * 6u);
    stats.vertices += draw.vertexCount;
    ++stats.depthRestores;
    stats.depthRestoreTiles += tiles.size();
    draws.push_back(draw);
}

uint64_t GSGpuBackend::Impl::spanEpoch(const PageSpan &span) const
{
    uint64_t newest = 0;
    for (uint32_t i = 0; i < span.count; ++i)
        newest = std::max(newest, pageEpoch[(span.first + i) & (kPageCount - 1u)]);
    return newest;
}

// ------------------------------------------------------------------------------------------------
// targets and pipelines

uint32_t GSGpuBackend::Impl::frameIndex(const GSFrameReg &frame)
{
    const uint32_t fbw = std::max<uint32_t>(frame.fbw, 1u);
    for (uint32_t i = 0; i < frames.size(); ++i)
        if (frames[i].fbp == frame.fbp && frames[i].fbw == fbw && frames[i].psm == frame.psm)
            return i;

    FrameTarget t;
    t.fbp = frame.fbp;
    t.fbw = fbw;
    t.psm = frame.psm;
    t.w = fbw * 64u * scale;
    t.h = kTargetRows * scale;
    SDL_GPUTextureCreateInfo ci = {};
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = t.w;
    ci.height = t.h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    t.color = SDL_CreateGPUTexture(dev, &ci);
    if (!t.color)
    {
        std::fprintf(stderr, "[gs:gpu] cannot create a %ux%u frame target: %s\n", t.w, t.h, SDL_GetError());
        return ~0u;
    }
    ++stats.frameTargets;

    // Start from what the memory image holds, so a buffer the game does not redraw in full shows what
    // it showed before (the picture of a clip's first frames depends on it).
    const size_t offset = allocateStage(static_cast<size_t>(t.w) * t.h * 4u);
    uint32_t *px = reinterpret_cast<uint32_t *>(stage.data() + offset);
    const uint32_t basePtr = frame.fbp << 5u;
    const bool sixteen = frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S;
    for (uint32_t y = 0; y < t.h; ++y)
        for (uint32_t x = 0; x < t.w; ++x)
        {
            const uint32_t raw = GSTransfer::ReadPixel(vram, frame.psm, basePtr, fbw, x / scale, y / scale);
            px[static_cast<size_t>(y) * t.w + x] = sixteen ? Rgba5551ToRgba8888(static_cast<uint16_t>(raw)) : raw;
        }
    pendingUploads.push_back({t.color, t.w, t.h, offset});
    frames.push_back(t);
    return static_cast<uint32_t>(frames.size() - 1u);
}

uint32_t GSGpuBackend::Impl::depthIndex(const GSZbufReg &zbuf, uint32_t fbw)
{
    const uint32_t w = std::max(fbw, 1u) * 64u * scale;
    for (uint32_t i = 0; i < depths.size(); ++i)
        if (depths[i].zbp == zbuf.zbp && depths[i].w == w)
            return i;

    DepthTarget d;
    d.zbp = zbuf.zbp;
    d.fbw = std::max(fbw, 1u);
    d.w = w;
    d.h = kTargetRows * scale;
    SDL_GPUTextureCreateInfo ci = {};
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    ci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    ci.width = d.w;
    ci.height = d.h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    d.depth = SDL_CreateGPUTexture(dev, &ci);
    if (!d.depth)
    {
        std::fprintf(stderr, "[gs:gpu] cannot create a %ux%u depth target: %s\n", d.w, d.h, SDL_GetError());
        return ~0u;
    }
    ++stats.depthTargets;
    depths.push_back(d);
    return static_cast<uint32_t>(depths.size() - 1u);
}

SDL_GPUGraphicsPipeline *GSGpuBackend::Impl::pipeline(uint32_t key)
{
    const auto found = pipelines.find(key);
    if (found != pipelines.end())
        return found->second;

    BlendState blend;
    blend.src = static_cast<BlendFactor>(key & 7u);
    blend.dst = static_cast<BlendFactor>((key >> 3) & 7u);
    blend.op = static_cast<BlendOp>((key >> 6) & 3u);
    blend.enable = ((key >> 8) & 1u) != 0u;
    const uint32_t compare = (key >> 9) & 3u;
    const bool depthWrite = ((key >> 11) & 1u) != 0u;

    SDL_GPUVertexBufferDescription vb = {};
    vb.slot = 0;
    vb.pitch = sizeof(Vertex);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[4] = {};
    attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, x)};
    attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(Vertex, z)};
    attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(Vertex, rgba)};
    attrs[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, u)};

    SDL_GPUColorTargetDescription ct = {};
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    const bool zrestore = (key & kZRestoreKey) != 0u;
    if (zrestore)
    {
        ct.blend_state.enable_color_write_mask = true; // no colour channel is written
        ct.blend_state.color_write_mask = 0;
    }
    if (blend.enable)
    {
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = toSdl(blend.src);
        ct.blend_state.dst_color_blendfactor = toSdl(blend.dst);
        ct.blend_state.color_blend_op = toSdl(blend.op);
        // The alpha channel just stores what the shader wrote (the blend coefficient).
        ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }

    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vertexShader;
    info.fragment_shader = zrestore ? zcopyShader : fragmentShader;
    info.vertex_input_state.vertex_buffer_descriptions = &vb;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attrs;
    info.vertex_input_state.num_vertex_attributes = 4;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.rasterizer_state.enable_depth_clip = false; // clamp: a Z of 2^24 or more still draws
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.enable_depth_test = true; // ALWAYS is a test that passes: writes still happen
    info.depth_stencil_state.enable_depth_write = depthWrite;
    info.depth_stencil_state.compare_op = toSdlCompare(compare);
    info.target_info.color_target_descriptions = &ct;
    info.target_info.num_color_targets = 1;
    info.target_info.has_depth_stencil_target = true;
    info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &info);
    if (!p)
        std::fprintf(stderr, "[gs:gpu] SDL_CreateGPUGraphicsPipeline: %s\n", SDL_GetError());
    ++stats.pipelinesCreated;
    pipelines[key] = p;
    return p;
}

// ------------------------------------------------------------------------------------------------
// textures

size_t GSGpuBackend::Impl::allocateStage(size_t bytes)
{
    const size_t offset = alignUp(stage.size(), 256u);
    stage.resize(offset + bytes);
    return offset;
}

uint64_t GSGpuBackend::Impl::clutHash(const GSDrawState &state)
{
    const GSTex0Reg &t = state.context.tex0;
    const uint32_t texa = static_cast<uint32_t>(state.texa.ta0) | (static_cast<uint32_t>(state.texa.ta1) << 8) | (state.texa.aem ? 1u << 16 : 0u);
    if (clutMemo.valid && clutMemo.version == clutVersion && clutMemo.cpsm == t.cpsm && clutMemo.csm == t.csm &&
        clutMemo.csa == t.csa && clutMemo.psm == t.psm && clutMemo.texa == texa)
        return clutMemo.hash;
    const uint32_t count = isFourBitIndexedPsm(t.psm) ? 16u : 256u;
    uint32_t palette[256];
    for (uint32_t i = 0; i < count; ++i)
        palette[i] = LookupClutEntry(clut.data(), state.texa, static_cast<uint8_t>(i), t.cpsm, t.csm, t.csa, t.psm);
    clutMemo = {true, clutVersion, t.cpsm, t.csm, t.csa, t.psm, texa, GSRec::Hash64(palette, count * 4u, count)};
    return clutMemo.hash;
}

SDL_GPUTexture *GSGpuBackend::Impl::createTexture(const GSDrawState &state, uint64_t contentHash, uint32_t w, uint32_t h)
{
    const GSTex0Reg &t = state.context.tex0;
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *tex = SDL_CreateGPUTexture(dev, &ti);
    if (!tex)
    {
        std::fprintf(stderr, "[gs:gpu] cannot create a %ux%u texture: %s\n", w, h, SDL_GetError());
        return nullptr;
    }

    // Decode the whole declared texture from the memory image, one texel at a time, like the CPU
    // renderer (GSCpuBackend::DecodeTexel).
    const uint64_t t0 = nowNanos();
    const size_t bytes = static_cast<size_t>(w) * h * 4u;
    const size_t offset = allocateStage(bytes);
    uint32_t *out = reinterpret_cast<uint32_t *>(stage.data() + offset);
    GSMem::TexturePageCache cache;
    const bool indexed = isFourBitIndexedPsm(t.psm) || isEightBitIndexedPsm(t.psm);
    uint32_t palette[256];
    if (indexed)
    {
        const uint32_t count = isFourBitIndexedPsm(t.psm) ? 16u : 256u;
        for (uint32_t i = 0; i < 256u; ++i)
            palette[i] = LookupClutEntry(clut.data(), state.texa, static_cast<uint8_t>(i % count), t.cpsm, t.csm, t.csa, t.psm);
    }
    for (uint32_t v = 0; v < h; ++v)
        for (uint32_t u = 0; u < w; ++u)
        {
            const uint32_t raw = GSMem::ReadTexture(cache, vram, t.psm, t.tbp0, t.tbw, u, v);
            uint32_t texel;
            switch (t.psm)
            {
            case GS_PSM_CT32:
            case GS_PSM_Z32:
            case GS_PSM_CT24:
            case GS_PSM_Z24:
                texel = applyTexa(state.texa, t.psm, raw);
                break;
            case GS_PSM_CT16:
            case GS_PSM_CT16S:
            case GS_PSM_Z16:
            case GS_PSM_Z16S:
                texel = applyTexa(state.texa, t.psm, Rgba5551ToRgba8888(static_cast<uint16_t>(raw)));
                break;
            case GS_PSM_T8:
            case GS_PSM_T8H:
                texel = palette[raw & 0xFFu];
                break;
            case GS_PSM_T4:
            case GS_PSM_T4HL:
            case GS_PSM_T4HH:
                texel = palette[raw & 0x0Fu];
                break;
            default:
                texel = 0xFFFF00FFu;
                break;
            }
            out[static_cast<size_t>(v) * w + u] = texel;
        }
    stats.texDecodeNanos += nowNanos() - t0;
    pendingUploads.push_back({tex, w, h, offset});

    GpuTexture g;
    g.tex = tex;
    g.w = w;
    g.h = h;
    g.bytes = bytes;
    g.lastUsedFlush = flushIndex;
    contentTextures[contentHash] = g;
    textureBytes += bytes;
    ++stats.texturesCreated;
    stats.textureBytesCreated += bytes;
    return tex;
}

SDL_GPUTexture *GSGpuBackend::Impl::resolveTexture(const GSDrawState &state)
{
    ++stats.texLookups;
    const GSTex0Reg &t = state.context.tex0;
    const bool indexed = isFourBitIndexedPsm(t.psm) || isEightBitIndexedPsm(t.psm);
    SigKey key;
    key.tbp0 = t.tbp0;
    key.tbwPsm = static_cast<uint32_t>(t.tbw) | (static_cast<uint32_t>(t.psm) << 8);
    key.size = static_cast<uint32_t>(state.textureWidth) | (static_cast<uint32_t>(state.textureHeight) << 16);
    key.texa = static_cast<uint32_t>(state.texa.ta0) | (static_cast<uint32_t>(state.texa.ta1) << 8) | (state.texa.aem ? 1u << 16 : 0u);
    key.clutHash = indexed ? clutHash(state) : 0u;

    // 1. recent
    if (recentValid && recentEpoch == epoch && recentKey == key)
    {
        ++stats.texHitsRecent;
        return recentTexture;
    }

    const uint32_t w = std::max<uint32_t>(state.textureWidth, 1u);
    const uint32_t h = std::max<uint32_t>(state.textureHeight, 1u);
    SDL_GPUTexture *result = nullptr;
    auto touch = [&](uint64_t contentHash) -> bool
    {
        const auto it = contentTextures.find(contentHash);
        if (it == contentTextures.end())
            return false;
        it->second.lastUsedFlush = flushIndex;
        result = it->second.tex;
        return true;
    };

    // 2. signature
    PageSpan span = BufferPageSpan(t.psm, t.tbp0, t.tbw, 0, 0, w, h);
    const auto sig = signatures.find(key);
    if (sig != signatures.end())
    {
        span = sig->second.span;
        if (spanEpoch(span) <= sig->second.epoch && touch(sig->second.contentHash))
        {
            ++stats.texHitsSignature;
            sig->second.epoch = epoch;
            recentValid = true;
            recentKey = key;
            recentEpoch = epoch;
            recentTexture = result;
            return result;
        }
    }

    // 3. content: the bytes of the pages the texture can live in, with everything else the decode depends on
    uint64_t hash = SigKeyHash{}(key);
    for (uint32_t i = 0; i < span.count; ++i)
        hash = GSRec::Hash64(vram + static_cast<size_t>((span.first + i) & (kPageCount - 1u)) * kPageBytes, kPageBytes, hash);
    if (touch(hash))
    {
        ++stats.texHitsContent;
    }
    else
    {
        ++stats.texMisses;
        result = createTexture(state, hash, w, h);
        if (!result)
            return nullptr;
    }
    signatures[key] = {hash, epoch, span};
    recentValid = true;
    recentKey = key;
    recentEpoch = epoch;
    recentTexture = result;
    return result;
}

void GSGpuBackend::Impl::evictTextures()
{
    if (textureBytes <= kTextureBudget)
        return;
    // Least recently used first, never what the list just submitted used.
    std::vector<std::pair<uint64_t, uint64_t>> order; // lastUsedFlush, key
    order.reserve(contentTextures.size());
    for (const auto &e : contentTextures)
        if (e.second.lastUsedFlush + 1u < flushIndex)
            order.emplace_back(e.second.lastUsedFlush, e.first);
    std::sort(order.begin(), order.end());
    const uint64_t target = kTextureBudget / 4u * 3u;
    for (const auto &o : order)
    {
        if (textureBytes <= target)
            break;
        const auto it = contentTextures.find(o.second);
        SDL_ReleaseGPUTexture(dev, it->second.tex);
        textureBytes -= it->second.bytes;
        contentTextures.erase(it);
        ++stats.texturesEvicted;
    }
    // Signatures pointing at evicted textures miss on lookup and are rebuilt; nothing to fix here.
    recentValid = false;
}

// ------------------------------------------------------------------------------------------------
// primitives

void GSGpuBackend::Impl::submitSetup(const GSDrawState &state, const Setup &setup)
{
    const GSContext &ctx = state.context;
    ++stats.primitives;
    if (BoxOutsideScissor(setup, ctx.scissor))
    {
        ++stats.primitivesCulled;
        return;
    }

    // Features the clips never use (GS census). They are counted and logged, not drawn.
    if (state.prim.fge) note(0, "fog (PRIM.FGE)");
    if (state.prim.aa1) note(1, "edge anti-aliasing (PRIM.AA1)");
    if (ctx.frame.fbmsk != 0u) note(2, "frame buffer write mask (FBMSK)");
    if ((ctx.fba & 1ull) != 0u) note(3, "FBA");
    if ((state.dthe & 1ull) != 0u) note(4, "dithering (DTHE)");
    if (state.pabe) note(5, "per-pixel alpha blend enable (PABE)");
    if ((ctx.test & 1ull) != 0u && ((ctx.test >> 12) & 3u) != 0u) note(6, "alpha test fail action other than KEEP (AFAIL)");
    if (((ctx.test >> 14) & 1ull) != 0u) note(7, "destination alpha test (DATE)");
    if (ctx.zbuf.psm != GS_PSM_Z24 && ctx.zbuf.psm != 0u) note(8, "Z buffer formats other than Z24");
    if (ctx.frame.psm != GS_PSM_CT32) note(9, "frame buffer formats other than CT32 (drawn as 32-bit)");

    const uint32_t frame = frameIndex(ctx.frame);
    const uint32_t depth = depthIndex(ctx.zbuf, ctx.frame.fbw);
    if (frame == ~0u || depth == ~0u)
        return;

    // Z pages that memory changed (or that no draw has used yet) are copied in ahead of this primitive.
    restoreDepth(depth, frame, static_cast<uint32_t>(ctx.scissor.y1) + 1u);

    const BlendState blend = BlendFromAlpha(state.prim.abe, ctx.alpha);
    if (!blend.supported)
    {
        ++stats.blendUnsupported;
        note(10, "a blend equation with no exact fixed-function form (drawn as (Cs-Cd)*C+Cd)");
    }
    const uint32_t compare = DepthCompare(ctx.test);
    const bool depthWrite = !ctx.zbuf.zmask;

    Draw d;
    d.frame = frame;
    d.depth = depth;
    d.pipeline = pipelineKey(blend, compare, depthWrite);
    d.uniforms = BuildFragUniforms(state, setup.texelCoords, blend.coefficientFromFix);
    d.texture = nullptr;
    if (state.prim.tme)
    {
        d.texture = resolveTexture(state);
        if (!d.texture)
            return;
    }
    const FrameTarget &target = frames[frame];
    const int32_t s = static_cast<int32_t>(scale);
    d.sx = std::min<int32_t>(static_cast<int32_t>(ctx.scissor.x0) * s, static_cast<int32_t>(target.w));
    d.sy = std::min<int32_t>(static_cast<int32_t>(ctx.scissor.y0) * s, static_cast<int32_t>(target.h));
    d.sw = std::min<int32_t>((static_cast<int32_t>(ctx.scissor.x1) + 1) * s, static_cast<int32_t>(target.w)) - d.sx;
    d.sh = std::min<int32_t>((static_cast<int32_t>(ctx.scissor.y1) + 1) * s, static_cast<int32_t>(target.h)) - d.sy;
    if (d.sw <= 0 || d.sh <= 0)
    {
        ++stats.primitivesCulled;
        return;
    }

    const uint32_t first = static_cast<uint32_t>(vertices.size());
    vertices.insert(vertices.end(), setup.v, setup.v + setup.count);
    stats.vertices += setup.count;
    if (!draws.empty())
    {
        Draw &last = draws.back();
        if (last.frame == d.frame && last.depth == d.depth && last.pipeline == d.pipeline && last.texture == d.texture &&
            last.sx == d.sx && last.sy == d.sy && last.sw == d.sw && last.sh == d.sh && last.uniforms == d.uniforms)
        {
            last.vertexCount += setup.count;
            return;
        }
    }
    d.firstVertex = first;
    d.vertexCount = setup.count;
    draws.push_back(d);
}

void GSGpuBackend::Impl::submit(const GSPrimitiveBatch &batch)
{
    Setup setup;
    switch (batch.state.prim.type)
    {
    case GS_PRIM_SPRITE:
        if (batch.vertexCount < 2u || !BuildSprite(batch, scale, setup))
            return;
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        if (batch.vertexCount < 3u)
            return;
        if (!BuildTriangle(batch, scale, setup))
        {
            ++stats.primitives;
            ++stats.primitivesCulled;
            return;
        }
        break;
    default:
        ++stats.primitivesUnsupported;
        note(11, "points and lines");
        return;
    }
    submitSetup(batch.state, setup);
    if (vertices.size() >= kMaxVerticesPerFlush || stage.size() >= kMaxStageBytes)
        flushLocked(false);
}

// ------------------------------------------------------------------------------------------------
// recording a command buffer

bool GSGpuBackend::Impl::ensureBuffers(size_t vertexBytes, size_t transferBytes)
{
    if (vertexBytes > vertexBufferBytes)
    {
        if (vertexBuffer)
            SDL_ReleaseGPUBuffer(dev, vertexBuffer);
        size_t size = std::max<size_t>(1u << 20, vertexBufferBytes);
        while (size < vertexBytes)
            size *= 2u;
        SDL_GPUBufferCreateInfo bi = {};
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = static_cast<Uint32>(size);
        vertexBuffer = SDL_CreateGPUBuffer(dev, &bi);
        vertexBufferBytes = vertexBuffer ? size : 0;
        if (!vertexBuffer)
        {
            std::fprintf(stderr, "[gs:gpu] SDL_CreateGPUBuffer(%zu): %s\n", size, SDL_GetError());
            return false;
        }
    }
    if (transferBytes > uploadBufferBytes)
    {
        if (uploadBuffer)
            SDL_ReleaseGPUTransferBuffer(dev, uploadBuffer);
        size_t size = std::max<size_t>(4u << 20, uploadBufferBytes);
        while (size < transferBytes)
            size *= 2u;
        SDL_GPUTransferBufferCreateInfo tbi = {};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = static_cast<Uint32>(size);
        uploadBuffer = SDL_CreateGPUTransferBuffer(dev, &tbi);
        uploadBufferBytes = uploadBuffer ? size : 0;
        if (!uploadBuffer)
        {
            std::fprintf(stderr, "[gs:gpu] SDL_CreateGPUTransferBuffer(%zu): %s\n", size, SDL_GetError());
            return false;
        }
    }
    return true;
}

void GSGpuBackend::Impl::flushLocked(bool wait)
{
    if (draws.empty() && pendingUploads.empty())
    {
        if (wait)
            SDL_WaitForGPUIdle(dev);
        return;
    }
    const size_t vertexBytes = vertices.size() * sizeof(Vertex);
    const size_t stageBase = alignUp(vertexBytes, 256u);
    if (!ensureBuffers(vertexBytes, stageBase + stage.size()))
    {
        draws.clear();
        vertices.clear();
        pendingUploads.clear();
        stage.clear();
        return;
    }

    uint8_t *map = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(dev, uploadBuffer, true));
    if (!map)
    {
        std::fprintf(stderr, "[gs:gpu] SDL_MapGPUTransferBuffer: %s\n", SDL_GetError());
        return;
    }
    if (vertexBytes)
        std::memcpy(map, vertices.data(), vertexBytes);
    if (!stage.empty())
        std::memcpy(map + stageBase, stage.data(), stage.size());
    SDL_UnmapGPUTransferBuffer(dev, uploadBuffer);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    {
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        if (vertexBytes)
        {
            SDL_GPUTransferBufferLocation src = {uploadBuffer, 0};
            SDL_GPUBufferRegion dst = {vertexBuffer, 0, static_cast<Uint32>(vertexBytes)};
            SDL_UploadToGPUBuffer(copy, &src, &dst, true);
        }
        for (const PendingUpload &u : pendingUploads)
        {
            SDL_GPUTextureTransferInfo src = {};
            src.transfer_buffer = uploadBuffer;
            src.offset = static_cast<Uint32>(stageBase + u.offset);
            src.pixels_per_row = u.w;
            src.rows_per_layer = u.h;
            SDL_GPUTextureRegion dst = {};
            dst.texture = u.tex;
            dst.w = u.w;
            dst.h = u.h;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
        }
        SDL_EndGPUCopyPass(copy);
    }

    SDL_GPURenderPass *pass = nullptr;
    uint32_t curFrame = ~0u, curDepth = ~0u;
    uint32_t curPipeline = ~0u;
    SDL_GPUTexture *curTexture = reinterpret_cast<SDL_GPUTexture *>(~uintptr_t(0));
    for (const Draw &d : draws)
    {
        if (!pass || d.frame != curFrame || d.depth != curDepth)
        {
            if (pass)
                SDL_EndGPURenderPass(pass);
            SDL_GPUColorTargetInfo color = {};
            color.texture = frames[d.frame].color;
            color.load_op = SDL_GPU_LOADOP_LOAD;
            color.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPUDepthStencilTargetInfo depth = {};
            depth.texture = depths[d.depth].depth;
            depth.clear_depth = 0.0f; // the GS clears Z to 0 and keeps the larger value
            depth.load_op = depths[d.depth].needsClear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            depths[d.depth].needsClear = false;
            depth.store_op = SDL_GPU_STOREOP_STORE;
            depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
            ++stats.renderPasses;
            const float globals[4] = {static_cast<float>(frames[d.frame].w), static_cast<float>(frames[d.frame].h), 0.0f, 0.0f};
            SDL_PushGPUVertexUniformData(cmd, 0, globals, sizeof(globals));
            SDL_GPUBufferBinding vb = {vertexBuffer, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
            curFrame = d.frame;
            curDepth = d.depth;
            curPipeline = ~0u;
            curTexture = reinterpret_cast<SDL_GPUTexture *>(~uintptr_t(0));
        }
        if (d.pipeline != curPipeline)
        {
            SDL_GPUGraphicsPipeline *p = pipeline(d.pipeline);
            if (!p)
                continue;
            SDL_BindGPUGraphicsPipeline(pass, p);
            curPipeline = d.pipeline;
        }
        if (d.texture != curTexture)
        {
            SDL_GPUTextureSamplerBinding binding = {d.texture ? d.texture : dummy, sampler};
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            curTexture = d.texture;
        }
        SDL_PushGPUFragmentUniformData(cmd, 0, &d.uniforms, sizeof(d.uniforms));
        const SDL_Rect scissor = {d.sx, d.sy, d.sw, d.sh};
        SDL_SetGPUScissor(pass, &scissor);
        SDL_DrawGPUPrimitives(pass, d.vertexCount, 1, d.firstVertex, 0);
        ++stats.drawCalls;
    }
    if (pass)
        SDL_EndGPURenderPass(pass);

    ++stats.flushes;
    stats.maxDrawsPerFlush = std::max<uint64_t>(stats.maxDrawsPerFlush, draws.size());
    SDL_SubmitGPUCommandBuffer(cmd);
    if (wait)
        SDL_WaitForGPUIdle(dev);

    draws.clear();
    vertices.clear();
    pendingUploads.clear();
    stage.clear();
    for (SDL_GPUTexture *t : transientTextures)
        SDL_ReleaseGPUTexture(dev, t); // safe while the submitted command buffer still uses it
    transientTextures.clear();
    ++flushIndex;
    evictTextures();
}

// ------------------------------------------------------------------------------------------------
// reading frames back

bool GSGpuBackend::Impl::downloadRegion(SDL_GPUTexture *tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint8_t> &out)
{
    const size_t bytes = static_cast<size_t>(w) * h * 4u;
    SDL_GPUTransferBufferCreateInfo tbi = {};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = static_cast<Uint32>(bytes);
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!tb)
    {
        std::fprintf(stderr, "[gs:gpu] SDL_CreateGPUTransferBuffer(download): %s\n", SDL_GetError());
        return false;
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region = {};
    region.texture = tex;
    region.x = x;
    region.y = y;
    region.w = w;
    region.h = h;
    region.d = 1;
    SDL_GPUTextureTransferInfo dst = {tb, 0, w, h};
    SDL_DownloadFromGPUTexture(copy, &region, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    bool ok = false;
    if (fence)
    {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
        const uint8_t *mapped = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(dev, tb, false));
        if (mapped)
        {
            out.assign(mapped, mapped + bytes);
            SDL_UnmapGPUTransferBuffer(dev, tb);
            stats.downloadBytes += bytes;
            ok = true;
        }
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return ok;
}

// What GSCpuBackend::CopyFrameToHostRgba does, for a frame buffer that lives in a render target: the
// displayed region goes into the top-left corner of a kHostFrameWidth x kHostFrameHeight RGBA8 buffer.
// A frame buffer that was never drawn to is read from the memory image like the CPU renderer does.
bool GSGpuBackend::Impl::copyFrame(const GSFrameReg &frame, uint32_t width, uint32_t height, std::vector<uint8_t> &out,
                                   bool preserveAlpha, bool useLocalMemoryLayout, bool frameBaseIsPages,
                                   uint32_t originX, uint32_t originY)
{
    const uint32_t fbw = frame.fbw ? frame.fbw : GSPresent::kHostFrameWidth / 64u;
    const FrameTarget *target = nullptr;
    if (useLocalMemoryLayout && frameBaseIsPages)
        for (const FrameTarget &f : frames)
            if (f.fbp == frame.fbp && f.fbw == fbw && f.psm == frame.psm)
                target = &f;
    if (!target || (originX + width) * scale > target->w || (originY + height) * scale > target->h || scale != 1u)
        return GSPresent::CopyFrameFromMemory(vram, vramSize, frame, width, height, out, preserveAlpha, useLocalMemoryLayout,
                                              frameBaseIsPages, originX, originY);

    std::vector<uint8_t> region;
    if (!downloadRegion(target->color, originX, originY, width, height, region))
        return false;
    out.assign(static_cast<size_t>(GSPresent::kHostFrameWidth) * GSPresent::kHostFrameHeight * 4u, 0u);
    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *dst = out.data() + static_cast<size_t>(y) * GSPresent::kHostFrameWidth * 4u;
        const uint8_t *src = region.data() + static_cast<size_t>(y) * width * 4u;
        std::memcpy(dst, src, static_cast<size_t>(width) * 4u);
        for (uint32_t x = 0; x < width; ++x)
        {
            // The target's alpha is the blend coefficient with 0x80 = 1.0 (0..255 in a unorm byte);
            // give it back the GS meaning (0..0x80), or 0xFF when the picture does not use alpha.
            dst[x * 4u + 3u] = preserveAlpha ? static_cast<uint8_t>((static_cast<uint32_t>(src[x * 4u + 3u]) * 128u + 127u) / 255u) : 255u;
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// GSGpuBackend

GSGpuBackend::GSGpuBackend(SDL_GPUDevice *device, uint32_t scale) : m_impl(std::make_unique<Impl>())
{
    GSMem::InitLookupTables(); // the swizzle tables: textures and uploads need them even when no CPU renderer exists
    m_impl->ready = m_impl->init(device, scale);
}

GSGpuBackend::~GSGpuBackend() = default;

bool GSGpuBackend::Ready() const
{
    return m_impl->ready;
}

void GSGpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (vram && vramSize < GSMem::MEMORY_SIZE)
    {
        std::fprintf(stderr, "[gs:gpu] GS memory image must be at least 4 MiB\n");
        return;
    }
    if (s.ready)
        s.flushLocked(false);
    s.vram = vram;
    s.vramSize = vramSize;
    s.clut.fill(0u);
    s.clutCbp.fill(0u);
    ++s.clutVersion;
    s.transfer = {};
    s.transfer.direction = 3u;
    s.transferState = {};
    s.transferState.direction = 3u;
    s.localToHost.clear();
    s.localToHostPos = 0u;
    s.touchAll();
    s.recentValid = false;
}

void GSGpuBackend::Reset()
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.ready)
        s.flushLocked(false);
    s.clut.fill(0u);
    s.clutCbp.fill(0u);
    ++s.clutVersion;
    s.transfer = {};
    s.transfer.direction = 3u;
    s.transferState = {};
    s.transferState.direction = 3u;
    s.localToHost.clear();
    s.localToHostPos = 0u;
    s.touchAll();
    s.recentValid = false;
}

void GSGpuBackend::ImportRecordingState(const GSRec::BackendState &state)
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.clut = state.clut;
    s.clutCbp = state.clutCbp;
    ++s.clutVersion;
    s.transfer = state.transfer;
    s.transferState = state.transferState;
    s.localToHost = state.localToHost;
    s.localToHostPos = static_cast<size_t>(state.localToHostReadPos);
}

void GSGpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.ready || !s.vram || batch.vertexCount == 0u)
        return;
    s.submit(batch);
}

void GSGpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.vram || (!isFourBitIndexedPsm(tex0.psm) && !isEightBitIndexedPsm(tex0.psm)))
        return;
    if (!ClutLoadWanted(tex0, s.clutCbp))
        return;
    // The CLUT is read from the memory image as it is now. Textures are resolved when a primitive is
    // submitted, so later CLUT loads do not change draws already recorded.
    GSMem::TexturePageCache cache;
    LoadClutFromMemory(s.clut, tex0, texclut, [&](uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
                       { return GSMem::ReadTexture(cache, s.vram, psm, base, bw, x, y); });
    ++s.clutVersion;
}

void GSGpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.transfer = command;
    GSTransfer::Begin(command, s.transferState);
    if (!s.vram)
        return;
    const auto &b = command.bitbltbuf;
    if (command.direction == 2u)
    {
        GSTransfer::LocalToLocal(s.vram, s.transfer, s.transferState);
        s.touchBuffer(b.dpsm, b.dbp, std::max<uint32_t>(b.dbw, 1u), command.trxpos.dsax, command.trxpos.dsay, command.trxreg.rrw, command.trxreg.rrh);
    }
    else if (command.direction == 1u)
    {
        GSTransfer::LocalToHost(s.vram, s.transfer, s.transferState, s.localToHost, s.localToHostPos);
    }
}

void GSGpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!data || sizeBytes == 0u || !s.vram || s.transferState.direction != 0u)
        return;
    GSTransfer::UploadImage(s.vram, s.transfer, s.transferState, data, sizeBytes);
    const auto &b = s.transfer.bitbltbuf;
    s.touchBuffer(b.dpsm, b.dbp, std::max<uint32_t>(b.dbw, 1u), s.transfer.trxpos.dsax, s.transfer.trxpos.dsay,
                  s.transfer.trxreg.rrw, s.transfer.trxreg.rrh);
    ++s.stats.uploadTransfers;
    s.stats.uploadBytes += sizeBytes;
}

void GSGpuBackend::Flush()
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.ready)
        s.flushLocked(false);
}

void GSGpuBackend::TextureFlush()
{
    // Texture reads come from cached immutable textures that follow the memory image through page
    // epochs, so there is nothing to flush.
}

void GSGpuBackend::Sync(GSSyncReason reason)
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.ready)
        return;
    // Like the CPU renderer, FINISH only has to start the work; the other reasons wait for it.
    s.flushLocked(reason != GSSyncReason::Finish);
}

void GSGpuBackend::MarkPresentation(const GSPresentationRequest &request)
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.ready || !s.vram)
        return;
    s.lastMarker = request;
    s.flushLocked(false);
}

PresentationFrame GSGpuBackend::Present(const GSPresentationRequest &request)
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.ready || !s.vram)
        return {};
    s.flushLocked(true);
    ++s.stats.presents;
    return GSPresent::Compose(request, [&s](const GSFrameReg &frame, uint32_t width, uint32_t height, std::vector<uint8_t> &pixels,
                                            bool preserveAlpha, bool useLocalMemoryLayout, bool frameBaseIsPages,
                                            uint32_t originX, uint32_t originY)
                              { return s.copyFrame(frame, width, height, pixels, preserveAlpha, useLocalMemoryLayout,
                                                   frameBaseIsPages, originX, originY); });
}

bool GSGpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.ready || !s.vram || context.frame.fbw == 0u)
        return false;
    if (context.frame.psm != GS_PSM_CT32 && context.frame.psm != GS_PSM_CT24 && context.frame.psm != GS_PSM_CT16 &&
        context.frame.psm != GS_PSM_CT16S)
        return false;
    // The CPU renderer fills the scissor box with the colour. Here it is a flat sprite over that box,
    // with no blending and no depth test or write.
    GSPrimitiveBatch batch;
    batch.vertexCount = 2;
    batch.state.context = context;
    batch.state.context.xyoffset = {};
    batch.state.context.test = 0u; // ZTE off: the depth test always passes
    batch.state.context.zbuf.zmask = true;
    batch.state.prim.type = GS_PRIM_SPRITE;
    uint8_t a = static_cast<uint8_t>(rgba >> 24u);
    if ((context.fba & 1ull) != 0ull && context.frame.psm != GS_PSM_CT24)
        a |= 0x80u;
    const uint32_t x1 = std::max<uint32_t>(context.scissor.x0, context.scissor.x1);
    const uint32_t y1 = std::max<uint32_t>(context.scissor.y0, context.scissor.y1);
    GSVertex v0, v1;
    v0.x = static_cast<float>(context.scissor.x0);
    v0.y = static_cast<float>(context.scissor.y0);
    v1.x = static_cast<float>(x1 + 1u);
    v1.y = static_cast<float>(y1 + 1u);
    v1.r = static_cast<uint8_t>(rgba);
    v1.g = static_cast<uint8_t>(rgba >> 8u);
    v1.b = static_cast<uint8_t>(rgba >> 16u);
    v1.a = a;
    batch.vertices[0] = v0;
    batch.vertices[1] = v1;
    batch.state.context.scissor.x1 = static_cast<uint16_t>(x1);
    batch.state.context.scissor.y1 = static_cast<uint16_t>(y1);
    s.submit(batch);
    return true;
}

uint32_t GSGpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!dst || maxBytes == 0u || s.localToHostPos >= s.localToHost.size())
        return 0u;
    const size_t count = std::min<size_t>(maxBytes, s.localToHost.size() - s.localToHostPos);
    std::memcpy(dst, s.localToHost.data() + s.localToHostPos, count);
    s.localToHostPos += count;
    s.transferState.localToHostPendingBytes = s.localToHost.size() - s.localToHostPos;
    return static_cast<uint32_t>(count);
}

uint32_t GSGpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.vram)
        return 0u;
    // The memory image never holds pixels drawn by this backend (the GS census found no readbacks).
    return GSTransfer::ReadPixel(s.vram, psm, base, bw, x, y);
}

void GSGpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.vram)
        return;
    GSTransfer::WritePixel(s.vram, psm, base, bw, x, y, value);
    s.touchBuffer(psm, base, bw, x, y, 1u, 1u);
}

void GSGpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.vram || s.vramSize == 0u)
    {
        out.clear();
        return;
    }
    out.assign(s.vram, s.vram + s.vramSize); // the memory image, without drawn pixels
}

GSTransferSnapshot GSGpuBackend::GetTransferSnapshot() const
{
    Impl &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.mutex);
    GSTransferSnapshot result = s.transferState;
    result.localToHostPendingBytes = s.localToHostPos < s.localToHost.size() ? s.localToHost.size() - s.localToHostPos : 0u;
    return result;
}

GSGpuBackend::Stats GSGpuBackend::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->stats;
}

void GSGpuBackend::ResetStats()
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const uint64_t pipelines = m_impl->stats.pipelinesCreated;
    m_impl->stats = {};
    m_impl->stats.pipelinesCreated = pipelines;
}
