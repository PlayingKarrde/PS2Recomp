// ps2x_gpu_smoke: checks the GPU toolchain end to end before any GS work is built on it.
//
//   ps2x_gpu_smoke [--driver vulkan|direct3d12|metal] [--png path] [--window N] [--debug]
//
// One HLSL shader pair (ps2xRuntime/shaders/smoke.{vert,frag}.hlsl) was compiled offline into SPIR-V,
// DXIL and MSL (build_shaders.py) and committed as headers. This program loads the format the
// chosen SDL_GPU driver accepts and draws a small scene:
//
//   (a) an opaque quad textured with a 4x4 test texture (nearest sampling) and a vertex colour of 0x80 grey,
//   (b) a quad blended with src*a + dst*(1-a), a = 0x80/255, over part of (a) and part of the background,
//   (c) a quad behind (a) under a GREATER_OR_EQUAL depth test (the GS's GEQUAL): it must be rejected
//       where it overlaps (a) and drawn where it does not,
//   (d) a quad at exactly the same depth as (a): GEQUAL must let it through (GREATER would not).
//
// Headless (default): the scene is drawn into an offscreen 640x448 RGBA8 target with a depth target,
// the target is downloaded through a transfer buffer, and sample pixels in every region are compared
// with values computed here. One PASS or FAIL line per check; the exit code is 1 on any FAIL, 2 when
// the GPU could not be set up. --png writes the downloaded picture.
//
// --window N: also creates a 640x448 window, claims it for the device, draws the same scene for N
// frames and blits it to the swapchain (the first frame is checked and written to --png as well).

#include "gpu/gpu_shaders.h"
#include "shaders/compiled/smoke_frag.h"
#include "shaders/compiled/smoke_vert.h"
#include "../common/png_write.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kWidth = 640;
    constexpr uint32_t kHeight = 448;
    constexpr uint8_t kClear[3] = {32, 48, 64};
    constexpr float kGrey = 128.0f / 255.0f; // vertex colour 0x80

    struct Vertex
    {
        float x, y; // pixels, origin top-left
        float z;    // 0..1, larger is nearer (GS convention; cleared to 0, tested with GEQUAL)
        uint8_t rgba[4];
        float u, v;
    };
    static_assert(sizeof(Vertex) == 24, "vertex layout must match the attribute offsets below");

    struct Quad
    {
        float x0, y0, x1, y1, z;
        uint8_t rgba[4];
    };

    // The scene. Draw order is A, C, D (opaque), then B (blended); the vertex buffer holds them in that order.
    //   A: z 0.5, grey 0x80      C: z 0.25, white, overlaps A's top-left corner
    //   D: z 0.5 (= A), white, overlaps A's bottom strip      B: z 0.9, white with alpha 0x80, depth write off
    constexpr Quad kA = {32, 32, 288, 224, 0.5f, {0x80, 0x80, 0x80, 0xFF}};
    constexpr Quad kC = {0, 0, 96, 96, 0.25f, {0xFF, 0xFF, 0xFF, 0xFF}};
    constexpr Quad kD = {32, 192, 160, 288, 0.5f, {0xFF, 0xFF, 0xFF, 0xFF}};
    constexpr Quad kB = {160, 128, 416, 320, 0.9f, {0xFF, 0xFF, 0xFF, 0x80}};
    constexpr const Quad *kDrawOrder[4] = {&kA, &kC, &kD, &kB};

    // The 4x4 test texture: every texel differs in all three colour channels.
    void texel(int tx, int ty, uint8_t out[4])
    {
        out[0] = static_cast<uint8_t>(40 + 60 * tx);
        out[1] = static_cast<uint8_t>(40 + 60 * ty);
        out[2] = static_cast<uint8_t>(250 - 40 * (tx + ty));
        out[3] = 255;
    }

    // Texel colour of quad q at pixel (px, py), nearest sampling at the pixel centre, times the vertex RGB.
    void quadColour(const Quad &q, int px, int py, float out[3])
    {
        const float u = (px + 0.5f - q.x0) / (q.x1 - q.x0);
        const float v = (py + 0.5f - q.y0) / (q.y1 - q.y0);
        const int tx = std::clamp(static_cast<int>(std::floor(u * 4.0f)), 0, 3);
        const int ty = std::clamp(static_cast<int>(std::floor(v * 4.0f)), 0, 3);
        uint8_t t[4];
        texel(tx, ty, t);
        for (int c = 0; c < 3; ++c)
            out[c] = static_cast<float>(t[c]) * (static_cast<float>(q.rgba[c]) / 255.0f);
    }

    struct Check
    {
        const char *name;
        int x, y;
        float expect[3];
    };

    std::vector<Check> buildChecks()
    {
        std::vector<Check> checks;
        auto opaque = [&](const char *name, const Quad &q, int x, int y) {
            Check c{name, x, y, {}};
            quadColour(q, x, y, c.expect);
            checks.push_back(c);
        };
        auto flat = [&](const char *name, int x, int y, const float rgb[3]) {
            Check c{name, x, y, {rgb[0], rgb[1], rgb[2]}};
            checks.push_back(c);
        };

        const float clear[3] = {kClear[0], kClear[1], kClear[2]};
        opaque("a_opaque_textured_texel(2,0)", kA, 192, 56);
        opaque("a_opaque_textured_texel(3,0)", kA, 256, 56);
        opaque("a_opaque_textured_texel(1,1)", kA, 128, 104);

        // (c) behind (a): rejected over A, drawn on the two strips where it sticks out.
        opaque("c_behind_rejected_over_a", kA, 64, 64);
        opaque("c_behind_drawn_outside_a_left", kC, 16, 60);
        opaque("c_behind_drawn_outside_a_top", kC, 60, 12);

        // (d) same depth as (a): GEQUAL passes, so D replaces A.
        opaque("d_equal_depth_passes_over_a", kD, 100, 208);
        opaque("d_drawn_over_background", kD, 100, 270);

        // (b) blend over A and over the background.
        const float af = static_cast<float>(kB.rgba[3]) / 255.0f;
        for (int pass = 0; pass < 2; ++pass)
        {
            const int x = pass == 0 ? 240 : 360;
            const int y = pass == 0 ? 170 : 270;
            float src[3], dst[3];
            quadColour(kB, x, y, src);
            if (pass == 0)
                quadColour(kA, x, y, dst);
            else
                std::memcpy(dst, clear, sizeof(dst));
            float out[3];
            for (int c = 0; c < 3; ++c)
                out[c] = src[c] * af + dst[c] * (1.0f - af);
            flat(pass == 0 ? "b_blend_over_a" : "b_blend_over_background", x, y, out);
        }

        flat("background_untouched", 600, 400, clear);
        return checks;
    }

    [[noreturn]] void die(const char *what)
    {
        std::fprintf(stderr, "FATAL: %s: %s\n", what, SDL_GetError());
        std::exit(2);
    }

    const char *textureFormatName(SDL_GPUTextureFormat f)
    {
        switch (f)
        {
        case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case SDL_GPU_TEXTUREFORMAT_D24_UNORM: return "D24_UNORM";
        case SDL_GPU_TEXTUREFORMAT_D32_FLOAT: return "D32_FLOAT";
        default: return "(other)";
        }
    }

    struct Options
    {
        const char *driver = nullptr;
        const char *png = nullptr;
        int windowFrames = 0;
        bool debug = false;
    };

    // Everything the scene needs. Created once; the scene is drawn into `target` every frame.
    struct Scene
    {
        SDL_GPUDevice *device = nullptr;
        SDL_GPUShader *vert = nullptr;
        SDL_GPUShader *frag = nullptr;
        SDL_GPUGraphicsPipeline *opaquePipe = nullptr;
        SDL_GPUGraphicsPipeline *blendPipe = nullptr;
        SDL_GPUTexture *testTexture = nullptr;
        SDL_GPUSampler *sampler = nullptr;
        SDL_GPUBuffer *vertices = nullptr;
        SDL_GPUTexture *target = nullptr;
        SDL_GPUTexture *depth = nullptr;
        SDL_GPUTextureFormat depthFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    };

    SDL_GPUGraphicsPipeline *makePipeline(Scene &s, bool blend)
    {
        SDL_GPUVertexBufferDescription vb = {};
        vb.slot = 0;
        vb.pitch = sizeof(Vertex);
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[4] = {};
        attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, x)};
        attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(Vertex, z)};
        attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(Vertex, rgba)};
        attrs[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, u)};

        SDL_GPUColorTargetDescription ct = {};
        ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        if (blend)
        {
            ct.blend_state.enable_blend = true;
            ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        }

        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = s.vert;
        info.fragment_shader = s.frag;
        info.vertex_input_state.vertex_buffer_descriptions = &vb;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attrs;
        info.vertex_input_state.num_vertex_attributes = 4;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = !blend;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        info.target_info.color_target_descriptions = &ct;
        info.target_info.num_color_targets = 1;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = s.depthFormat;
        return SDL_CreateGPUGraphicsPipeline(s.device, &info);
    }

    // Uploads the test texture and the quad vertices, waiting until the copy is done.
    void uploadResources(Scene &s)
    {
        std::vector<Vertex> verts;
        for (const Quad *q : kDrawOrder)
        {
            const Vertex v[4] = {
                {q->x0, q->y0, q->z, {q->rgba[0], q->rgba[1], q->rgba[2], q->rgba[3]}, 0, 0},
                {q->x1, q->y0, q->z, {q->rgba[0], q->rgba[1], q->rgba[2], q->rgba[3]}, 1, 0},
                {q->x1, q->y1, q->z, {q->rgba[0], q->rgba[1], q->rgba[2], q->rgba[3]}, 1, 1},
                {q->x0, q->y1, q->z, {q->rgba[0], q->rgba[1], q->rgba[2], q->rgba[3]}, 0, 1},
            };
            const int order[6] = {0, 1, 2, 0, 2, 3};
            for (int i : order)
                verts.push_back(v[i]);
        }
        const Uint32 vertexBytes = static_cast<Uint32>(verts.size() * sizeof(Vertex));

        uint8_t texels[4 * 4 * 4];
        for (int ty = 0; ty < 4; ++ty)
            for (int tx = 0; tx < 4; ++tx)
                texel(tx, ty, &texels[(ty * 4 + tx) * 4]);

        SDL_GPUTransferBufferCreateInfo tbi = {};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = vertexBytes + sizeof(texels);
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(s.device, &tbi);
        if (!tb)
            die("SDL_CreateGPUTransferBuffer(upload)");
        uint8_t *map = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(s.device, tb, false));
        if (!map)
            die("SDL_MapGPUTransferBuffer(upload)");
        std::memcpy(map, verts.data(), vertexBytes);
        std::memcpy(map + vertexBytes, texels, sizeof(texels));
        SDL_UnmapGPUTransferBuffer(s.device, tb);

        SDL_GPUBufferCreateInfo bi = {};
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = vertexBytes;
        s.vertices = SDL_CreateGPUBuffer(s.device, &bi);
        if (!s.vertices)
            die("SDL_CreateGPUBuffer");

        SDL_GPUTextureCreateInfo ti = {};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = 4;
        ti.height = 4;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
        s.testTexture = SDL_CreateGPUTexture(s.device, &ti);
        if (!s.testTexture)
            die("SDL_CreateGPUTexture(test texture)");

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s.device);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src = {tb, 0};
        SDL_GPUBufferRegion dstBuf = {s.vertices, 0, vertexBytes};
        SDL_UploadToGPUBuffer(copy, &src, &dstBuf, false);
        SDL_GPUTextureTransferInfo srcTex = {tb, vertexBytes, 4, 4};
        SDL_GPUTextureRegion dstTex = {};
        dstTex.texture = s.testTexture;
        dstTex.w = 4;
        dstTex.h = 4;
        dstTex.d = 1;
        SDL_UploadToGPUTexture(copy, &srcTex, &dstTex, false);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (!fence)
            die("submit upload");
        SDL_WaitForGPUFences(s.device, true, &fence, 1);
        SDL_ReleaseGPUFence(s.device, fence);
        SDL_ReleaseGPUTransferBuffer(s.device, tb);
    }

    void createScene(Scene &s)
    {
        const char *vfmt = nullptr;
        const char *ffmt = nullptr;
        s.vert = ps2x::gpu::createShader(s.device, ps2x::gpu::shaders::smoke_vert, &vfmt);
        s.frag = ps2x::gpu::createShader(s.device, ps2x::gpu::shaders::smoke_frag, &ffmt);
        if (!s.vert || !s.frag)
        {
            std::fprintf(stderr, "FATAL: could not create the shaders\n");
            std::exit(2);
        }
        std::printf("shader format used: vertex %s, fragment %s\n", vfmt, ffmt);

        // Depth target: D24 if the device offers it as a depth target, else D32 float.
        const SDL_GPUTextureUsageFlags depthUsage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        if (SDL_GPUTextureSupportsFormat(s.device, SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTURETYPE_2D, depthUsage))
            s.depthFormat = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
        else if (SDL_GPUTextureSupportsFormat(s.device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D, depthUsage))
            s.depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        else
        {
            std::fprintf(stderr, "FATAL: neither D24_UNORM nor D32_FLOAT is supported as a depth target\n");
            std::exit(2);
        }
        std::printf("depth format: %s (D24_UNORM supported: %s, D32_FLOAT supported: %s)\n",
                    textureFormatName(s.depthFormat),
                    SDL_GPUTextureSupportsFormat(s.device, SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTURETYPE_2D, depthUsage) ? "yes" : "no",
                    SDL_GPUTextureSupportsFormat(s.device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D, depthUsage) ? "yes" : "no");

        s.opaquePipe = makePipeline(s, false);
        s.blendPipe = makePipeline(s, true);
        if (!s.opaquePipe || !s.blendPipe)
            die("SDL_CreateGPUGraphicsPipeline");

        SDL_GPUSamplerCreateInfo si = {};
        si.min_filter = SDL_GPU_FILTER_NEAREST;
        si.mag_filter = SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        s.sampler = SDL_CreateGPUSampler(s.device, &si);
        if (!s.sampler)
            die("SDL_CreateGPUSampler");

        SDL_GPUTextureCreateInfo ci = {};
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ci.width = kWidth;
        ci.height = kHeight;
        ci.layer_count_or_depth = 1;
        ci.num_levels = 1;
        ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
        s.target = SDL_CreateGPUTexture(s.device, &ci);
        if (!s.target)
            die("SDL_CreateGPUTexture(color target)");

        SDL_GPUTextureCreateInfo di = ci;
        di.format = s.depthFormat;
        di.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        s.depth = SDL_CreateGPUTexture(s.device, &di);
        if (!s.depth)
            die("SDL_CreateGPUTexture(depth target)");

        uploadResources(s);
    }

    // Records the whole scene into `cmd`, leaving the picture in s.target.
    void drawScene(Scene &s, SDL_GPUCommandBuffer *cmd)
    {
        SDL_GPUColorTargetInfo color = {};
        color.texture = s.target;
        color.clear_color = {kClear[0] / 255.0f, kClear[1] / 255.0f, kClear[2] / 255.0f, 1.0f};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo depth = {};
        depth.texture = s.depth;
        depth.clear_depth = 0.0f; // the GS clears Z to 0 and keeps the larger value
        depth.load_op = SDL_GPU_LOADOP_CLEAR;
        depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
        const float globals[4] = {static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 0.0f};
        SDL_PushGPUVertexUniformData(cmd, 0, globals, sizeof(globals));
        SDL_GPUBufferBinding vb = {s.vertices, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_GPUTextureSamplerBinding tex = {s.testTexture, s.sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &tex, 1);

        SDL_BindGPUGraphicsPipeline(pass, s.opaquePipe);
        SDL_DrawGPUPrimitives(pass, 18, 1, 0, 0); // A, C, D
        SDL_BindGPUGraphicsPipeline(pass, s.blendPipe);
        SDL_DrawGPUPrimitives(pass, 6, 1, 18, 0); // B
        SDL_EndGPURenderPass(pass);
    }

    // Copies s.target to the CPU (RGBA8, kWidth*4 bytes per row), after drawing the scene again.
    std::vector<uint8_t> drawAndDownload(Scene &s)
    {
        SDL_GPUTransferBufferCreateInfo tbi = {};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tbi.size = kWidth * kHeight * 4;
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(s.device, &tbi);
        if (!tb)
            die("SDL_CreateGPUTransferBuffer(download)");

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s.device);
        drawScene(s, cmd);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion region = {};
        region.texture = s.target;
        region.w = kWidth;
        region.h = kHeight;
        region.d = 1;
        SDL_GPUTextureTransferInfo dst = {tb, 0, kWidth, kHeight};
        SDL_DownloadFromGPUTexture(copy, &region, &dst);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (!fence)
            die("submit download");
        SDL_WaitForGPUFences(s.device, true, &fence, 1);
        SDL_ReleaseGPUFence(s.device, fence);

        std::vector<uint8_t> pixels(tbi.size);
        const uint8_t *map = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(s.device, tb, false));
        if (!map)
            die("SDL_MapGPUTransferBuffer(download)");
        std::memcpy(pixels.data(), map, pixels.size());
        SDL_UnmapGPUTransferBuffer(s.device, tb);
        SDL_ReleaseGPUTransferBuffer(s.device, tb);
        return pixels;
    }

    // Returns the number of failed checks.
    int runChecks(const std::vector<uint8_t> &pixels)
    {
        constexpr int kTolerance = 2;
        int failures = 0;
        for (const Check &c : buildChecks())
        {
            const uint8_t *p = &pixels[(static_cast<size_t>(c.y) * kWidth + c.x) * 4];
            bool ok = true;
            for (int i = 0; i < 3; ++i)
                ok = ok && std::fabs(static_cast<float>(p[i]) - c.expect[i]) <= kTolerance + 0.5f;
            std::printf("%s %s at (%d,%d): got (%u,%u,%u) expected (%.1f,%.1f,%.1f) tolerance +-%d\n",
                        ok ? "PASS" : "FAIL", c.name, c.x, c.y, p[0], p[1], p[2],
                        c.expect[0], c.expect[1], c.expect[2], kTolerance);
            failures += ok ? 0 : 1;
        }
        return failures;
    }

    void usage()
    {
        std::fprintf(stderr, "usage: ps2x_gpu_smoke [--driver vulkan|direct3d12|metal] [--png path] [--window N] [--debug]\n");
    }
}

int main(int argc, char **argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--driver" && i + 1 < argc)
            opt.driver = argv[++i];
        else if (a == "--png" && i + 1 < argc)
            opt.png = argv[++i];
        else if (a == "--window" && i + 1 < argc)
            opt.windowFrames = std::max(1, std::atoi(argv[++i]));
        else if (a == "--debug")
            opt.debug = true;
        else
        {
            usage();
            return 2;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
        die("SDL_Init(VIDEO)");
    std::printf("SDL version: %d.%d.%d, video driver: %s\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
                SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)");

    const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
    Scene scene;
    scene.device = SDL_CreateGPUDevice(formats, opt.debug, opt.driver);
    if (!scene.device)
    {
        std::fprintf(stderr, "FATAL: SDL_CreateGPUDevice(driver=%s): %s\n", opt.driver ? opt.driver : "(auto)", SDL_GetError());
        return 2;
    }
    std::printf("requested driver: %s\n", opt.driver ? opt.driver : "(none, SDL picks)");
    std::printf("SDL_GetGPUDeviceDriver: %s\n", SDL_GetGPUDeviceDriver(scene.device));
    {
        const SDL_PropertiesID props = SDL_GetGPUDeviceProperties(scene.device);
        std::printf("device name: %s\n", SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_NAME_STRING, "(none)"));
        std::printf("driver name: %s\n", SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, "(none)"));
        std::printf("driver version: %s\n", SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, "(none)"));
        std::printf("driver info: %s\n", SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_DRIVER_INFO_STRING, "(none)"));
        std::printf("accepted shader formats: 0x%x\n", static_cast<unsigned>(SDL_GetGPUShaderFormats(scene.device)));
    }

    SDL_Window *window = nullptr;
    if (opt.windowFrames > 0)
    {
        window = SDL_CreateWindow("ps2x_gpu_smoke", static_cast<int>(kWidth), static_cast<int>(kHeight), 0);
        if (!window)
            die("SDL_CreateWindow");
        if (!SDL_ClaimWindowForGPUDevice(scene.device, window))
            die("SDL_ClaimWindowForGPUDevice");
        std::printf("swapchain format: %s\n", textureFormatName(SDL_GetGPUSwapchainTextureFormat(scene.device, window)));
    }

    createScene(scene);

    int failures = 0;
    {
        // Check the picture first, in both modes; in window mode this is the same scene that is presented below.
        const std::vector<uint8_t> pixels = drawAndDownload(scene);
        failures += runChecks(pixels);
        if (opt.png)
        {
            if (ps2x::test::writePng(opt.png, pixels.data(), kWidth, kHeight, kWidth * 4))
                std::printf("wrote %s\n", opt.png);
            else
            {
                std::fprintf(stderr, "FAIL could not write %s\n", opt.png);
                ++failures;
            }
        }
    }

    if (window)
    {
        const Uint64 t0 = SDL_GetTicksNS();
        int presented = 0;
        bool quit = false;
        for (int frame = 0; frame < opt.windowFrames && !quit; ++frame)
        {
            SDL_Event e;
            while (SDL_PollEvent(&e))
                if (e.type == SDL_EVENT_QUIT)
                    quit = true;

            SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(scene.device);
            drawScene(scene, cmd);
            SDL_GPUTexture *swap = nullptr;
            Uint32 sw = 0, sh = 0;
            if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &sw, &sh))
                die("SDL_WaitAndAcquireGPUSwapchainTexture");
            if (swap)
            {
                SDL_GPUBlitInfo blit = {};
                blit.source.texture = scene.target;
                blit.source.w = kWidth;
                blit.source.h = kHeight;
                blit.destination.texture = swap;
                blit.destination.w = sw;
                blit.destination.h = sh;
                blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
                blit.filter = SDL_GPU_FILTER_NEAREST;
                SDL_BlitGPUTexture(cmd, &blit);
                ++presented;
            }
            SDL_SubmitGPUCommandBuffer(cmd);
        }
        SDL_WaitForGPUIdle(scene.device);
        const double ms = static_cast<double>(SDL_GetTicksNS() - t0) / 1.0e6;
        std::printf("window: presented %d of %d frames in %.0f ms (%.1f fps)\n", presented, opt.windowFrames, ms,
                    presented * 1000.0 / std::max(ms, 1.0));
        if (presented != opt.windowFrames && !quit)
        {
            std::printf("FAIL window presented only %d of %d frames\n", presented, opt.windowFrames);
            ++failures;
        }
        else
            std::printf("PASS window presented %d frames\n", presented);
    }

    SDL_WaitForGPUIdle(scene.device);
    if (window)
        SDL_ReleaseWindowFromGPUDevice(scene.device, window);
    SDL_ReleaseGPUGraphicsPipeline(scene.device, scene.opaquePipe);
    SDL_ReleaseGPUGraphicsPipeline(scene.device, scene.blendPipe);
    SDL_ReleaseGPUShader(scene.device, scene.vert);
    SDL_ReleaseGPUShader(scene.device, scene.frag);
    SDL_ReleaseGPUSampler(scene.device, scene.sampler);
    SDL_ReleaseGPUBuffer(scene.device, scene.vertices);
    SDL_ReleaseGPUTexture(scene.device, scene.testTexture);
    SDL_ReleaseGPUTexture(scene.device, scene.target);
    SDL_ReleaseGPUTexture(scene.device, scene.depth);
    SDL_DestroyGPUDevice(scene.device);
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();

    std::printf("%s: %d failure(s)\n", failures == 0 ? "RESULT: ok" : "RESULT: FAILED", failures);
    return failures == 0 ? 0 : 1;
}
