// GSGpuPresenter: see include/runtime/gs/gs_gpu_present.h. Main thread only (it records into the
// window's command buffer). Needs SDL; nothing the generated game code includes refers to it.
//
// What Record() puts into the command buffer for a new field (all render passes, in this order):
//   1. field_black1, field_black2   only when the field carries fallback frames (display frame at fbp 0):
//                                   which of the pictures are not all black                -> flags (1 x 1)
//   2. field_select                 the field's picture, or the first non-black fallback   -> resolved[cur]
//                                   (this is also the copy into the presenter's own texture, so the field slot
//                                   is free as soon as the commands are submitted)
//   3. field_lower1, field_lower2   interlaced fields with a previous field: is the newest field the lower
//                                   one                                                    -> state[next]
//   4. field_deint                  the 448-line picture from resolved[cur], [prev], [older] -> picture
// A progressive picture is copied from resolved[cur] into the picture texture (mode 1 of field_deint); a
// marker without a picture clears the picture texture to magenta.
// resolved[] is a ring of three: the last three fields' pictures, which is the deinterlacer's history.

#include "runtime/gs/gs_gpu_present.h"
#include "runtime/gs/gs_gpu_threads.h"

#include "gpu/gpu_shaders.h"
#include "shaders/compiled/field_black1_frag.h"
#include "shaders/compiled/field_black2_frag.h"
#include "shaders/compiled/field_deint_frag.h"
#include "shaders/compiled/field_lower1_frag.h"
#include "shaders/compiled/field_lower2_frag.h"
#include "shaders/compiled/field_select_frag.h"
#include "shaders/compiled/present_vert.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{
    // Must match cbuffer Place in present.vert.hlsl (also used by the host's presenter).
    struct PlaceUniform
    {
        float rect[4];   // x0, y0, x1, y1 of the rectangle, in target pixels
        float uvRect[4]; // unused here
        float outputSize[2];
        float pad[2];
    };
    static_assert(sizeof(PlaceUniform) == 48, "cbuffer Place in present.vert.hlsl is 3 x float4");

    struct DeintUniform // cbuffer Params in field_deint.frag.hlsl
    {
        uint32_t info[4]; // width, height, has previous, has older
        uint32_t mode[4]; // 0 deinterlace, 1 copy
    };
    struct InfoUniform // cbuffer Params in the other field_*.frag.hlsl: one uint4
    {
        uint32_t info[4];
    };

    constexpr uint32_t kColumns = 160; // 640 / 4: width of the lower-field sums texture

    // The CPU path's rule for "this is one field of an interlaced picture" (UploadFrame in ps2_runtime.cpp).
    bool isFieldPicture(const GSGpuBackend::FieldInfo &f)
    {
        return f.interlaced && f.height != 0u && f.height * 2u <= 512u && f.width != 0u && f.width <= 640u;
    }
}

struct GSGpuPresenter::Impl
{
    GSGpuBackend &backend;
    SDL_GPUDevice *dev = nullptr;
    bool ready = false;

    SDL_GPUShader *vertexShader = nullptr;
    enum
    {
        kBlack1,
        kBlack2,
        kSelect,
        kLower1,
        kLower2,
        kDeint,
        kShaderCount
    };
    SDL_GPUShader *fragment[kShaderCount] = {};
    SDL_GPUGraphicsPipeline *pipeline[kShaderCount] = {};
    SDL_GPUSampler *sampler = nullptr;

    SDL_GPUTexture *picture = nullptr;     // 640 x 512 RGBA8: what the host draws
    SDL_GPUTexture *resolved[3] = {};      // 640 x 512 RGBA8: the last three fields' pictures
    SDL_GPUTexture *blackCounts = nullptr; // 640 x 1 RGBA32F
    SDL_GPUTexture *flags = nullptr;       // 1 x 1 RGBA8: pictures that are not all black
    SDL_GPUTexture *sums = nullptr;        // 160 x 1 R32G32_FLOAT
    SDL_GPUTexture *state[2] = {};         // 1 x 1 RGBA8: red 1 = the newest field is the lower one
    int stateIndex = 0;
    int curIndex = 0; // resolved[curIndex] gets the next field; [curIndex + 2] % 3 is the previous one
    bool needsInit = true;

    // What the next Record() has to do.
    enum class Pending
    {
        None,
        Magenta,
        Field
    };
    Pending pending = Pending::Magenta; // until the first field arrives, like the CPU path's first frame
    GSGpuBackend::FieldInfo cur;
    uint64_t lastSeq = 0;
    Size shown; // the size of the picture the texture holds, or will hold after the next Record()

    // The deinterlacer's history, as the CPU deinterlacer keeps it.
    bool hasPrev = false, hasOlder = false;
    uint32_t histPixels = 0; // width * height of the field in the previous picture

    std::vector<GSGpuBackend::FieldInfo> unpinAfterSubmit;

    explicit Impl(GSGpuBackend &b) : backend(b) {}
    ~Impl();
    bool init();
    SDL_GPUGraphicsPipeline *makePipeline(SDL_GPUShader *fragmentShader, SDL_GPUTextureFormat format);
    SDL_GPUTexture *makeTexture(SDL_GPUTextureFormat format, uint32_t w, uint32_t h);
    bool pass(SDL_GPUCommandBuffer *cb, SDL_GPUTexture *target, int which, uint32_t rectW, uint32_t rectH, uint32_t targetW,
              uint32_t targetH, const void *fragmentUniform, uint32_t fragmentUniformBytes,
              const SDL_GPUTextureSamplerBinding *bindings, uint32_t bindingCount);
    void clear(SDL_GPUCommandBuffer *cb, SDL_GPUTexture *target, float r, float g, float b, float a);
    void unpinField(const GSGpuBackend::FieldInfo &f);
};

void GSGpuPresenter::Impl::unpinField(const GSGpuBackend::FieldInfo &f)
{
    if (!f.valid || f.slot < 0)
        return;
    backend.UnpinField(f.slot);
    for (int i = 0; i < f.candidateCount; ++i)
        backend.UnpinField(f.candidates[i]);
}

GSGpuPresenter::Impl::~Impl()
{
    for (const GSGpuBackend::FieldInfo &f : unpinAfterSubmit)
        unpinField(f);
    if (pending == Pending::Field)
        unpinField(cur);
    if (!dev)
        return;
    SDL_WaitForGPUIdle(dev);
    for (SDL_GPUGraphicsPipeline *p : pipeline)
        if (p)
            SDL_ReleaseGPUGraphicsPipeline(dev, p);
    for (SDL_GPUShader *f : fragment)
        if (f)
            SDL_ReleaseGPUShader(dev, f);
    if (vertexShader)
        SDL_ReleaseGPUShader(dev, vertexShader);
    if (sampler)
        SDL_ReleaseGPUSampler(dev, sampler);
    for (SDL_GPUTexture *t : {picture, resolved[0], resolved[1], resolved[2], blackCounts, flags, sums, state[0], state[1]})
        if (t)
            SDL_ReleaseGPUTexture(dev, t);
}

SDL_GPUTexture *GSGpuPresenter::Impl::makeTexture(SDL_GPUTextureFormat format, uint32_t w, uint32_t h)
{
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = format;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *t = SDL_CreateGPUTexture(dev, &ti);
    if (!t)
        std::fprintf(stderr, "[gs:present] cannot create a %ux%u texture: %s\n", w, h, SDL_GetError());
    return t;
}

SDL_GPUGraphicsPipeline *GSGpuPresenter::Impl::makePipeline(SDL_GPUShader *fragmentShader, SDL_GPUTextureFormat format)
{
    SDL_GPUColorTargetDescription target = {};
    target.format = format; // no blending: the pass writes the pixels as they are
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vertexShader;
    info.fragment_shader = fragmentShader;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &info);
    if (!p)
        std::fprintf(stderr, "[gs:present] SDL_CreateGPUGraphicsPipeline (target format %d): %s\n", static_cast<int>(format), SDL_GetError());
    return p;
}

bool GSGpuPresenter::Impl::init()
{
    dev = backend.Device();
    if (!dev)
        return false;
    for (SDL_GPUTextureFormat format : {SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT})
        if (!SDL_GPUTextureSupportsFormat(dev, format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))
        {
            std::fprintf(stderr, "[gs:present] a 32-bit float render target format is not supported by this device\n");
            return false;
        }
    vertexShader = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::present_vert);
    fragment[kBlack1] = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::field_black1_frag);
    fragment[kBlack2] = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::field_black2_frag);
    fragment[kSelect] = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::field_select_frag);
    fragment[kLower1] = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::field_lower1_frag);
    fragment[kLower2] = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::field_lower2_frag);
    fragment[kDeint] = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::field_deint_frag);
    if (!vertexShader)
        return false;
    for (SDL_GPUShader *f : fragment)
        if (!f)
            return false;
    const SDL_GPUTextureFormat formats[kShaderCount] = {
        SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT,       SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM};
    bool pipelinesOk = true;
    for (int i = 0; i < kShaderCount; ++i)
    {
        pipeline[i] = makePipeline(fragment[i], formats[i]);
        if (!pipeline[i])
        {
            std::fprintf(stderr, "[gs:present] pipeline %d failed\n", i);
            pipelinesOk = false;
        }
    }
    if (!pipelinesOk)
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
        std::fprintf(stderr, "[gs:present] SDL_CreateGPUSampler: %s\n", SDL_GetError());
        return false;
    }
    picture = makeTexture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, kPictureWidth, kPictureHeight);
    for (SDL_GPUTexture *&t : resolved)
        t = makeTexture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, kPictureWidth, kPictureHeight);
    blackCounts = makeTexture(SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, kPictureWidth, 1);
    flags = makeTexture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1);
    sums = makeTexture(SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT, kColumns, 1);
    state[0] = makeTexture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1);
    state[1] = makeTexture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1);
    return picture && resolved[0] && resolved[1] && resolved[2] && blackCounts && flags && sums && state[0] && state[1];
}

// One render pass that draws the rectangle (0, 0)-(rectW, rectH) of `target` with pipeline `which`. The target is
// cleared to zero first (what is outside the rectangle stays zero, like the CPU picture buffer).
bool GSGpuPresenter::Impl::pass(SDL_GPUCommandBuffer *cb, SDL_GPUTexture *target, int which, uint32_t rectW, uint32_t rectH,
                                uint32_t targetW, uint32_t targetH, const void *fragmentUniform, uint32_t fragmentUniformBytes,
                                const SDL_GPUTextureSamplerBinding *bindings, uint32_t bindingCount)
{
    SDL_GPUColorTargetInfo color = {};
    color.texture = target;
    color.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    ps2x_gs::DebugPassLock passLock; // validation only: see gs_gpu_threads.h
    SDL_GPURenderPass *p = SDL_BeginGPURenderPass(cb, &color, 1, nullptr);
    if (!p)
    {
        std::fprintf(stderr, "[gs:present] SDL_BeginGPURenderPass: %s\n", SDL_GetError());
        return false;
    }
    PlaceUniform place = {};
    place.rect[2] = static_cast<float>(rectW);
    place.rect[3] = static_cast<float>(rectH);
    place.outputSize[0] = static_cast<float>(targetW);
    place.outputSize[1] = static_cast<float>(targetH);
    SDL_BindGPUGraphicsPipeline(p, pipeline[which]);
    SDL_PushGPUVertexUniformData(cb, 0, &place, sizeof(place));
    SDL_PushGPUFragmentUniformData(cb, 0, fragmentUniform, fragmentUniformBytes);
    SDL_BindGPUFragmentSamplers(p, 0, bindings, bindingCount);
    SDL_DrawGPUPrimitives(p, 6, 1, 0, 0);
    SDL_EndGPURenderPass(p);
    return true;
}

void GSGpuPresenter::Impl::clear(SDL_GPUCommandBuffer *cb, SDL_GPUTexture *target, float r, float g, float b, float a)
{
    SDL_GPUColorTargetInfo color = {};
    color.texture = target;
    color.clear_color = {r, g, b, a};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    ps2x_gs::DebugPassLock passLock; // validation only: see gs_gpu_threads.h
    if (SDL_GPURenderPass *p = SDL_BeginGPURenderPass(cb, &color, 1, nullptr))
        SDL_EndGPURenderPass(p);
}

GSGpuPresenter::GSGpuPresenter(GSGpuBackend &backend) : m_impl(std::make_unique<Impl>(backend))
{
    m_impl->ready = m_impl->init();
}

GSGpuPresenter::~GSGpuPresenter() = default;

bool GSGpuPresenter::Ready() const
{
    return m_impl->ready;
}

SDL_GPUTexture *GSGpuPresenter::PictureTexture() const
{
    return m_impl->picture;
}

GSGpuPresenter::Size GSGpuPresenter::Pull()
{
    Impl &s = *m_impl;
    if (!s.ready)
        return s.shown;
    GSGpuBackend::FieldInfo f;
    if (s.backend.TakeNewestField(s.lastSeq, f))
    {
        s.lastSeq = f.seq;
        if (s.pending == Impl::Pending::Field)
            s.unpinField(s.cur); // no command reads it: the host skipped that field
        if (!f.valid)
        {
            s.pending = Impl::Pending::Magenta; // history stays as it is, like the CPU deinterlacer
            s.shown = Size{640, 448};
        }
        else
        {
            s.pending = Impl::Pending::Field;
            s.cur = f;
            s.shown = Size{f.width, isFieldPicture(f) ? f.height * 2u : f.height};
        }
    }
    return s.shown;
}

SDL_GPUTexture *GSGpuPresenter::Record(SDL_GPUCommandBuffer *cb)
{
    Impl &s = *m_impl;
    if (!s.ready || !cb)
        return nullptr;
    if (s.needsInit)
    {
        s.needsInit = false;
        s.clear(cb, s.state[0], 0.0f, 0.0f, 0.0f, 1.0f); // "the newest field is the upper one", like the CPU's first answer
        s.clear(cb, s.state[1], 0.0f, 0.0f, 0.0f, 1.0f);
        s.clear(cb, s.flags, 0.0f, 0.0f, 0.0f, 1.0f);
        s.clear(cb, s.sums, 0.0f, 0.0f, 0.0f, 0.0f);
        s.clear(cb, s.blackCounts, 0.0f, 0.0f, 0.0f, 0.0f);
        s.clear(cb, s.picture, 0.0f, 0.0f, 0.0f, 0.0f);
        for (SDL_GPUTexture *t : s.resolved)
            s.clear(cb, t, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (s.pending == Impl::Pending::None)
        return s.picture;
    if (s.pending == Impl::Pending::Magenta)
    {
        s.clear(cb, s.picture, 1.0f, 0.0f, 1.0f, 1.0f); // the placeholder UploadFrame shows when there is no picture
        s.pending = Impl::Pending::None;
        return s.picture;
    }

    const GSGpuBackend::FieldInfo f = s.cur;
    SDL_GPUTexture *displayed = s.backend.FieldTexture(f.slot);
    if (!displayed)
    {
        s.unpinField(f);
        s.pending = Impl::Pending::None;
        return s.picture;
    }
    SDL_GPUSampler *smp = s.sampler;

    // 1-2. The field's own picture: the displayed region, or a fallback frame when it is black.
    SDL_GPUTexture *fallback[2] = {displayed, displayed};
    for (int i = 0; i < f.candidateCount; ++i)
        if (SDL_GPUTexture *t = s.backend.FieldTexture(f.candidates[i]))
            fallback[i] = t;
    const uint32_t candidateCount = static_cast<uint32_t>(f.candidateCount);
    if (candidateCount != 0)
    {
        const SDL_GPUTextureSamplerBinding pictures[3] = {{displayed, smp}, {fallback[0], smp}, {fallback[1], smp}};
        InfoUniform u = {{f.width, f.height, candidateCount, 0u}};
        s.pass(cb, s.blackCounts, Impl::kBlack1, kPictureWidth, 1, kPictureWidth, 1, &u, sizeof(u), pictures, 3);
        const SDL_GPUTextureSamplerBinding counts[1] = {{s.blackCounts, smp}};
        s.pass(cb, s.flags, Impl::kBlack2, 1, 1, 1, 1, &u, sizeof(u), counts, 1);
    }
    SDL_GPUTexture *current = s.resolved[s.curIndex];
    {
        const SDL_GPUTextureSamplerBinding pictures[4] = {{displayed, smp}, {fallback[0], smp}, {fallback[1], smp}, {s.flags, smp}};
        InfoUniform u = {{f.width, f.height, candidateCount, 0u}};
        s.pass(cb, current, Impl::kSelect, f.width, f.height, kPictureWidth, kPictureHeight, &u, sizeof(u), pictures, 4);
    }

    if (isFieldPicture(f))
    {
        // The CPU deinterlacer drops its history when the field's size changes.
        if (s.hasPrev && s.histPixels != f.width * f.height)
        {
            s.hasPrev = false;
            s.hasOlder = false;
        }
        SDL_GPUTexture *previous = s.resolved[(s.curIndex + 2) % 3];
        SDL_GPUTexture *older = s.resolved[(s.curIndex + 1) % 3];
        if (s.hasPrev)
        {
            InfoUniform lower = {{f.width, f.height, (f.width + 3u) / 4u, 0u}};
            const SDL_GPUTextureSamplerBinding first[2] = {{current, smp}, {previous, smp}};
            s.pass(cb, s.sums, Impl::kLower1, kColumns, 1, kColumns, 1, &lower, sizeof(lower), first, 2);
            SDL_GPUTexture *stateNow = s.state[s.stateIndex];
            SDL_GPUTexture *stateNext = s.state[1 - s.stateIndex];
            const SDL_GPUTextureSamplerBinding second[2] = {{s.sums, smp}, {stateNow, smp}};
            s.pass(cb, stateNext, Impl::kLower2, 1, 1, 1, 1, &lower, sizeof(lower), second, 2);
            s.stateIndex = 1 - s.stateIndex;
        }
        DeintUniform params = {{f.width, f.height, s.hasPrev ? 1u : 0u, s.hasOlder ? 1u : 0u}, {0u, 0u, 0u, 0u}};
        const SDL_GPUTextureSamplerBinding bindings[4] = {
            {current, smp}, {previous, smp}, {older, smp}, {s.state[s.stateIndex], smp}};
        s.pass(cb, s.picture, Impl::kDeint, f.width, f.height * 2u, kPictureWidth, kPictureHeight, &params, sizeof(params), bindings, 4);

        // This field is now the previous one, and the previous one the older one.
        s.curIndex = (s.curIndex + 1) % 3;
        s.hasOlder = s.hasPrev;
        s.hasPrev = true;
        s.histPixels = f.width * f.height;
    }
    else
    {
        // A progressive picture is shown as it is, and the CPU deinterlacer forgets its history.
        DeintUniform params = {{f.width, f.height, 0u, 0u}, {1u, 0u, 0u, 0u}};
        const SDL_GPUTextureSamplerBinding bindings[4] = {{current, smp}, {current, smp}, {current, smp}, {s.state[s.stateIndex], smp}};
        s.pass(cb, s.picture, Impl::kDeint, f.width, f.height, kPictureWidth, kPictureHeight, &params, sizeof(params), bindings, 4);
        s.hasPrev = s.hasOlder = false;
    }
    // The field slots were read by the commands above: give them back once the command buffer is submitted.
    s.unpinAfterSubmit.push_back(f);
    s.pending = Impl::Pending::None;
    return s.picture;
}

void GSGpuPresenter::Submitted()
{
    Impl &s = *m_impl;
    for (const GSGpuBackend::FieldInfo &f : s.unpinAfterSubmit)
        s.unpinField(f);
    s.unpinAfterSubmit.clear();
}

// Reads a small texture (w x h, bytesPerPixel) back, waiting for the GPU.
static bool downloadSmall(SDL_GPUDevice *dev, SDL_GPUTexture *texture, uint32_t w, uint32_t h, uint32_t bytesPerPixel, std::vector<uint8_t> &out)
{
    const size_t bytes = static_cast<size_t>(w) * h * bytesPerPixel;
    SDL_GPUTransferBufferCreateInfo tbi = {};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = static_cast<Uint32>(bytes);
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!tb)
        return false;
    ps2x_gs::DefragGuard window; // Vulkan only: held from here to the submit, see gs_gpu_threads.h
    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTextureRegion region = {};
    region.texture = texture;
    region.w = w;
    region.h = h;
    region.d = 1;
    SDL_GPUTextureTransferInfo dst = {tb, 0, w, h};
    SDL_DownloadFromGPUTexture(copy, &region, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    window.Unlock();
    bool ok = false;
    if (fence)
    {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
        if (const uint8_t *mapped = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(dev, tb, false)))
        {
            out.assign(mapped, mapped + bytes);
            SDL_UnmapGPUTransferBuffer(dev, tb);
            ok = true;
        }
    }
    {
        ps2x_gs::DefragGuard defragGuard;
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }
    return ok;
}

bool GSGpuPresenter::DebugDecision(uint64_t &lowerSum, uint64_t &upperSum, bool &lower)
{
    Impl &s = *m_impl;
    std::vector<uint8_t> sumsBytes, stateBytes;
    if (!s.ready || !downloadSmall(s.dev, s.sums, kColumns, 1, 8, sumsBytes) || !downloadSmall(s.dev, s.state[s.stateIndex], 1, 1, 4, stateBytes))
        return false;
    const float *sums = reinterpret_cast<const float *>(sumsBytes.data());
    lowerSum = upperSum = 0;
    for (uint32_t c = 0; c < kColumns; ++c)
    {
        lowerSum += static_cast<uint64_t>(sums[c * 2] + 0.5f);
        upperSum += static_cast<uint64_t>(sums[c * 2 + 1] + 0.5f);
    }
    lower = stateBytes[0] > 127;
    return true;
}

bool GSGpuPresenter::Download(std::vector<uint8_t> &rgba)
{
    Impl &s = *m_impl;
    if (!s.ready)
        return false;
    const size_t bytes = static_cast<size_t>(kPictureWidth) * kPictureHeight * 4u;
    SDL_GPUTransferBufferCreateInfo tbi = {};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = static_cast<Uint32>(bytes);
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(s.dev, &tbi);
    if (!tb)
        return false;
    ps2x_gs::DefragGuard window; // Vulkan only: held from here to the submit, see gs_gpu_threads.h
    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(s.dev);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTextureRegion region = {};
    region.texture = s.picture;
    region.w = kPictureWidth;
    region.h = kPictureHeight;
    region.d = 1;
    SDL_GPUTextureTransferInfo dst = {tb, 0, kPictureWidth, kPictureHeight};
    SDL_DownloadFromGPUTexture(copy, &region, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    window.Unlock();
    bool ok = false;
    if (fence)
    {
        SDL_WaitForGPUFences(s.dev, true, &fence, 1);
        SDL_ReleaseGPUFence(s.dev, fence);
        if (const uint8_t *mapped = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(s.dev, tb, false)))
        {
            rgba.assign(mapped, mapped + bytes);
            SDL_UnmapGPUTransferBuffer(s.dev, tb);
            ok = true;
        }
    }
    {
        ps2x_gs::DefragGuard defragGuard;
        SDL_ReleaseGPUTransferBuffer(s.dev, tb);
    }
    return ok;
}
