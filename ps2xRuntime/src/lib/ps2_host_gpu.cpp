// The desktop host's SDL_GPU presenter. See ps2_host_gpu.h for how a frame goes through it and why
// the upload and the blit are separate functions.
//
// Everything here runs on the main thread (the one that created the window).
//
// What it has to match: the SDL_Renderer path it replaces (still there as a fallback) did
//   SDL_RenderClear(black, alpha 255) -> SDL_RenderTexture(picture, srcRect, boxRect) -> SDL_RenderPresent
// with the picture texture set to SDL_BLENDMODE_BLEND and one of three scale modes. So this clears the
// swapchain image to opaque black, draws the same box with the same alpha blend, and has the same three
// scale modes (nearest sampler, linear sampler, or SDL's pixel-art shader, see present.frag.hlsl).

#include "ps2_host_gpu.h"

#include "ps2_host_ui.h"
#include "ps2_settings.h"
#include "gpu/gpu_shaders.h"
#include "runtime/gs/gs_gpu_threads.h"
#include "shaders/compiled/present_frag.h"
#include "shaders/compiled/present_vert.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ps2x_host
{
    namespace
    {
        struct Presenter
        {
            SDL_Window *window = nullptr;
            SDL_GPUDevice *device = nullptr;
            bool windowClaimed = false;

            SDL_GPUShader *vertexShader = nullptr;
            SDL_GPUShader *fragmentShader = nullptr;
            SDL_GPUGraphicsPipeline *pipeline = nullptr;
            SDL_GPUSampler *sampler = nullptr;        // linear (sharp and linear filters)
            SDL_GPUSampler *samplerNearest = nullptr; // nearest (PS2X_SCALE=nearest)

            // The CPU picture: staged into the transfer buffer, copied into the texture.
            SDL_GPUTransferBuffer *transfer = nullptr;
            SDL_GPUTexture *picture = nullptr;
            bool staged = false;     // a new picture waits in the transfer buffer
            bool hasPicture = false; // the texture holds a picture (else only the clear colour is shown)

            SDL_GPUTextureFormat swapchainFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
            SDL_GPUPresentMode presentMode = SDL_GPU_PRESENTMODE_VSYNC;
            ScaleMode scale = ScaleMode::Sharp;
            const char *vertexFormat = "?";
            const char *fragmentFormat = "?";
            bool debugMode = false;

            int boxSourceWidth = kPictureWidth;
            int boxSourceHeight = kPictureHeight;
            float boxAspect = 0.0f;

            GpuOverlay overlay;
            PictureSource source; // set: the picture comes from the GS GPU renderer's texture, not from stagePicture()
            bool loggedProblem = false;
            bool presentModeForced = false; // PS2X_GPU_PRESENT_MODE named the mode: the overlay's V-Sync setting leaves it alone

            // Task g5: the window command buffer acquires the swapchain texture before it takes the Vulkan defragmentation
            // guard (gs_gpu_threads.h). PS2X_GPU_ACQUIRE_FIRST=0 restores the old order (guard held across the acquire).
            bool acquireFirst = true;
            // PS2X_GPU_GUARD_STATS=1: once a second, how long the swapchain acquire blocked and how long each thread waited
            // for and held the guard.
            bool guardStats = false;
            // PS2X_TEST_ACQUIRE_DELAY_MS=<n>: sleep n ms after every swapchain acquire, imitating a driver whose acquire
            // blocks until the display has freed an image (FIFO/vsync-only drivers: Wayland, Android).
            uint32_t testAcquireDelayMs = 0;
            uint64_t statsLastNs = 0, statsFrames = 0, statsAcquireNs = 0, statsAcquireMaxNs = 0, statsPresentNs = 0, statsPresentMaxNs = 0;
            // where present() spends its time (ms summed per second): waiting for the guard, recording the picture and the overlay, the blit, the submit
            uint64_t statsLockNs = 0, statsRecordNs = 0, statsBlitNs = 0, statsSubmitNs = 0;
        };
        Presenter g;

        const char *presentModeName(SDL_GPUPresentMode mode)
        {
            switch (mode)
            {
            case SDL_GPU_PRESENTMODE_IMMEDIATE:
                return "immediate";
            case SDL_GPU_PRESENTMODE_MAILBOX:
                return "mailbox";
            default:
                return "vsync";
            }
        }

        const char *formatName(SDL_GPUTextureFormat format)
        {
            switch (format)
            {
            case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
                return "B8G8R8A8_UNORM";
            case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
                return "R8G8B8A8_UNORM";
            case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:
                return "B8G8R8A8_UNORM_SRGB";
            case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
                return "R8G8B8A8_UNORM_SRGB";
            case SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM:
                return "R10G10B10A2_UNORM";
            case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT:
                return "R16G16B16A16_FLOAT";
            default:
                return "(other)";
            }
        }

        bool envFlag(const char *name)
        {
            const char *value = std::getenv(name);
            return value && value[0] == '1';
        }

        // PS2X_GPU_DEFRAG_GUARD=0 turns the Vulkan guard off (gs_gpu_threads.h), to show what it prevents.
        bool envOff(const char *name)
        {
            const char *value = std::getenv(name);
            return value && value[0] == '0';
        }

        uint64_t nowNs()
        {
            return ps2x_gs::guardNowNs();
        }

        // PS2X_GPU_PRESENT_MODE=immediate|mailbox|vsync asks for a present mode (a test hook: a driver that offers only
        // vsync, the usual case on Linux/Wayland and Android, can be imitated on a desktop GPU).
        bool presentModeFromEnv(SDL_GPUPresentMode &mode)
        {
            const char *value = std::getenv("PS2X_GPU_PRESENT_MODE");
            if (!value || !value[0])
                return false;
            if (std::strcmp(value, "immediate") == 0)
                mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
            else if (std::strcmp(value, "mailbox") == 0)
                mode = SDL_GPU_PRESENTMODE_MAILBOX;
            else if (std::strcmp(value, "vsync") == 0)
                mode = SDL_GPU_PRESENTMODE_VSYNC;
            else
            {
                std::fprintf(stderr, "[host] PS2X_GPU_PRESENT_MODE=%s is not immediate, mailbox or vsync, ignored\n", value);
                return false;
            }
            return true;
        }

        // The host paces itself at 60 Hz, so unless V-Sync is asked for the swapchain must not also wait for the display:
        // immediate if the window can do it, else mailbox, else vsync (always available).
        SDL_GPUPresentMode unsyncedPresentMode(SDL_GPUDevice *device, SDL_Window *window)
        {
            if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_IMMEDIATE))
                return SDL_GPU_PRESENTMODE_IMMEDIATE;
            if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX))
                return SDL_GPU_PRESENTMODE_MAILBOX;
            return SDL_GPU_PRESENTMODE_VSYNC;
        }

        // Reasons are collected into one string, which the caller prints with the fallback notice.
        bool fail(std::string &failure, const char *what)
        {
            failure = std::string(what) + ": " + SDL_GetError();
            gpu_present::destroy();
            return false;
        }

        // ---- uniform blocks: must match the cbuffers in present.vert.hlsl and present.frag.hlsl ----

        struct PlaceUniform
        {
            float rect[4];   // x0, y0, x1, y1 of the box, window pixels
            float uvRect[4]; // u0, v0, u1, v1
            float outputSize[2];
            float pad[2];
        };
        static_assert(sizeof(PlaceUniform) == 48, "cbuffer Place in present.vert.hlsl is 3 x float4");

        struct ParamsUniform
        {
            float texelSize[4]; // 1/w, 1/h, w, h of the picture texture
            float mode[4];      // x: 0 plain, 1 pixel art
        };
        static_assert(sizeof(ParamsUniform) == 32, "cbuffer Params in present.frag.hlsl is 2 x float4");

        // The upload half: transfer buffer -> picture texture. Both are cycled, so a frame that is still
        // being drawn from the old contents keeps them and this frame gets fresh storage. The whole
        // texture is replaced every time, so nothing is lost by cycling.
        void recordUpload(SDL_GPUCommandBuffer *commandBuffer)
        {
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commandBuffer);
            if (!copy)
                return;
            SDL_GPUTextureTransferInfo source = {};
            source.transfer_buffer = g.transfer;
            source.offset = 0;
            source.pixels_per_row = kPictureWidth;
            source.rows_per_layer = kPictureHeight;
            SDL_GPUTextureRegion destination = {};
            destination.texture = g.picture;
            destination.w = kPictureWidth;
            destination.h = kPictureHeight;
            destination.d = 1;
            SDL_UploadToGPUTexture(copy, &source, &destination, true);
            SDL_EndGPUCopyPass(copy);
        }

        // The blit half: clear the swapchain image to black, draw `source` in the 4:3 box, then the overlay.
        // `source` is any RGBA texture sampled by the present shaders; `sourceWidth` x `sourceHeight` is its
        // full size (the sharp filter needs it). The box maths is the old drawPicture() maths, unchanged.
        void recordBlit(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUTexture *swapchain, Uint32 outputWidth,
                        Uint32 outputHeight, SDL_GPUTexture *source, int sourceWidth, int sourceHeight)
        {
            SDL_GPUColorTargetInfo color = {};
            color.texture = swapchain;
            color.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
            color.load_op = SDL_GPU_LOADOP_CLEAR;
            color.store_op = SDL_GPU_STOREOP_STORE;
            // With the settings overlay open, the picture, the overlay and the debug panel are three render passes in
            // that order, so the debug panel draws above the overlay (its ImGui windows are tools: they must stay
            // reachable). Closed, the overlay costs nothing and the picture and the panel share one pass, as before.
            const bool overlayOpen = ui::visible();
            ps2x_gs::DebugPassLock passLock; // validation only: see gs_gpu_threads.h
            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commandBuffer, &color, 1, nullptr);
            if (!pass)
            {
                if (!g.loggedProblem)
                    std::fprintf(stderr, "[host] SDL_BeginGPURenderPass failed: %s\n", SDL_GetError());
                g.loggedProblem = true;
                return;
            }

            if (source)
            {
                const float screenWidth = static_cast<float>(outputWidth);
                const float screenHeight = static_cast<float>(outputHeight);
                const float boxSourceWidth = static_cast<float>(std::max(1, g.boxSourceWidth));
                const float boxSourceHeight = static_cast<float>(std::max(1, g.boxSourceHeight));
                const float shape = g.boxAspect > 0.0f ? g.boxAspect : boxSourceWidth / boxSourceHeight;
                const float dstWidth = std::min(screenWidth, screenHeight * shape);
                const float dstHeight = dstWidth / shape;
                const float dstX = (screenWidth - dstWidth) * 0.5f;
                const float dstY = (screenHeight - dstHeight) * 0.5f;

                PlaceUniform place = {};
                place.rect[0] = dstX;
                place.rect[1] = dstY;
                place.rect[2] = dstX + dstWidth;
                place.rect[3] = dstY + dstHeight;
                place.uvRect[0] = 0.0f;
                place.uvRect[1] = 0.0f;
                place.uvRect[2] = boxSourceWidth / static_cast<float>(sourceWidth);
                place.uvRect[3] = boxSourceHeight / static_cast<float>(sourceHeight);
                place.outputSize[0] = screenWidth;
                place.outputSize[1] = screenHeight;

                ParamsUniform params = {};
                params.texelSize[0] = 1.0f / static_cast<float>(sourceWidth);
                params.texelSize[1] = 1.0f / static_cast<float>(sourceHeight);
                params.texelSize[2] = static_cast<float>(sourceWidth);
                params.texelSize[3] = static_cast<float>(sourceHeight);
                // The pixel-art filter makes a low-resolution picture crisp when it is enlarged. A picture at a higher internal
                // resolution (GPU renderer M4b) is shown with plain linear filtering, as the filter only widens its blend
                // zones when the window is smaller than the picture.
                params.mode[0] = (g.scale == ScaleMode::Sharp && sourceWidth <= kPictureWidth) ? 1.0f : 0.0f;

                SDL_BindGPUGraphicsPipeline(pass, g.pipeline);
                SDL_PushGPUVertexUniformData(commandBuffer, 0, &place, sizeof(place));
                SDL_PushGPUFragmentUniformData(commandBuffer, 0, &params, sizeof(params));
                SDL_GPUTextureSamplerBinding binding = {source, g.scale == ScaleMode::Nearest && g.samplerNearest ? g.samplerNearest : g.sampler};
                SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
                SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
            }

            if (!overlayOpen)
            {
                if (g.overlay.draw)
                    g.overlay.draw(commandBuffer, pass, g.overlay.user);
                SDL_EndGPURenderPass(pass);
                return;
            }
            SDL_EndGPURenderPass(pass);
            passLock.Unlock(); // the overlay and the panel take the lock for their own passes
            ui::record(commandBuffer, swapchain, outputWidth, outputHeight);
            if (g.overlay.draw)
            {
                SDL_GPUColorTargetInfo panel = {};
                panel.texture = swapchain;
                panel.load_op = SDL_GPU_LOADOP_LOAD;
                panel.store_op = SDL_GPU_STOREOP_STORE;
                ps2x_gs::DebugPassLock panelLock;
                if (SDL_GPURenderPass *panelPass = SDL_BeginGPURenderPass(commandBuffer, &panel, 1, nullptr))
                {
                    g.overlay.draw(commandBuffer, panelPass, g.overlay.user);
                    SDL_EndGPURenderPass(panelPass);
                }
            }
        }
    } // namespace

    SDL_GPUDevice *gpuDevice()
    {
        return g.device;
    }

    SDL_GPUTextureFormat gpuSwapchainFormat()
    {
        return g.swapchainFormat;
    }

    void setGpuOverlay(const GpuOverlay &overlay)
    {
        g.overlay = overlay;
    }

    namespace gpu_present
    {
        bool active()
        {
            return g.device != nullptr && g.pipeline != nullptr;
        }

        void setVsync(bool vsync)
        {
            if (!active() || g.presentModeForced)
                return;
            const SDL_GPUPresentMode mode = vsync ? SDL_GPU_PRESENTMODE_VSYNC : unsyncedPresentMode(g.device, g.window);
            if (mode == g.presentMode)
                return;
            if (!SDL_SetGPUSwapchainParameters(g.device, g.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
            {
                std::fprintf(stderr, "[host] swapchain present mode %s refused (%s), keeping %s\n", presentModeName(mode), SDL_GetError(),
                             presentModeName(g.presentMode));
                return;
            }
            std::printf("[host] present mode now %s\n", presentModeName(mode));
            std::fflush(stdout);
            g.presentMode = mode;
        }

        bool vsync()
        {
            return g.presentMode == SDL_GPU_PRESENTMODE_VSYNC;
        }

        void setPictureFilter(ScaleMode mode)
        {
            g.scale = mode;
        }

        ScaleMode pictureFilter()
        {
            return g.scale;
        }

        void setPictureSource(const PictureSource &source)
        {
            g.source = source;
        }

        bool hasPictureSource()
        {
            return g.source.record != nullptr;
        }

        bool downloadPicture(std::vector<uint8_t> &rgba)
        {
            return g.source.download && g.source.download(g.source.user, rgba);
        }

        void pictureTextureSize(int &width, int &height)
        {
            width = g.source.record ? static_cast<int>(g.source.textureWidth) : kPictureWidth;
            height = g.source.record ? static_cast<int>(g.source.textureHeight) : kPictureHeight;
        }

        bool create(SDL_Window *window, ScaleMode scale, std::string &failure)
        {
            g = Presenter{};
            g.window = window;
            g.scale = scale;
            g.debugMode = envFlag("PS2X_GPU_DEBUG");
            ps2x_gs::debugPassSerialization().store(g.debugMode); // the GS thread and this one both record render passes
            ps2x_gs::guardThreadIsMain() = true; // this is the host's main thread (for PS2X_GPU_GUARD_STATS)
            g.acquireFirst = !envOff("PS2X_GPU_ACQUIRE_FIRST");
            g.guardStats = envFlag("PS2X_GPU_GUARD_STATS");
            if (const char *delay = std::getenv("PS2X_TEST_ACQUIRE_DELAY_MS"))
                g.testAcquireDelayMs = static_cast<uint32_t>(std::max(0, std::atoi(delay)));
            ps2x_gs::defragGuardStatsEnabled().store(g.guardStats);

            // All three shader formats are allowed so SDL can pick whichever driver the machine has: Vulkan
            // takes SPIR-V, Direct3D 12 takes DXIL, Metal takes MSL. SDL_GPU_DRIVER (SDL_HINT_GPU_DRIVER)
            // in the environment still chooses the driver. The properties form of device creation
            // defaults debug mode to true, so it is set explicitly: validation layers cost speed, and
            // PS2X_GPU_DEBUG=1 turns them on for a check.
            SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, g.debugMode);
            SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN, false);
            SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VERBOSE_BOOLEAN, false);
            SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
            SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
            SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
            g.device = SDL_CreateGPUDeviceWithProperties(props);
            SDL_DestroyProperties(props);
            if (!g.device)
                return fail(failure, "SDL_CreateGPUDevice");

            // SDL's Vulkan driver defragments memory inside a submit, racing with a release on the other thread (gs_gpu_threads.h).
            if (const char *driver = SDL_GetGPUDeviceDriver(g.device))
                ps2x_gs::defragGuardEnabled().store(std::strcmp(driver, "vulkan") == 0 && !envOff("PS2X_GPU_DEFRAG_GUARD"));
            if (!SDL_ClaimWindowForGPUDevice(g.device, window))
                return fail(failure, "SDL_ClaimWindowForGPUDevice");
            g.windowClaimed = true;

            // The host loop paces itself at 60 Hz, so the swapchain must not also wait for the display:
            // immediate if the window can do it, else mailbox, else vsync (always available).
            SDL_GPUPresentMode forced = SDL_GPU_PRESENTMODE_VSYNC;
            // "Forced" means the player set the variable. A value that came from the settings file (ps2_settings.h puts it
            // in the same variable) leaves the overlay's V-Sync setting free to change it.
            g.presentModeForced = ps2x_settings::fixedByEnvironment("vsync");
            if (presentModeFromEnv(forced) && (forced == SDL_GPU_PRESENTMODE_VSYNC || SDL_WindowSupportsGPUPresentMode(g.device, window, forced)))
                g.presentMode = forced;
            else
                g.presentMode = unsyncedPresentMode(g.device, window);
            if (!SDL_SetGPUSwapchainParameters(g.device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, g.presentMode))
            {
                std::fprintf(stderr, "[host] swapchain present mode %s refused (%s), using vsync\n",
                             presentModeName(g.presentMode), SDL_GetError());
                g.presentMode = SDL_GPU_PRESENTMODE_VSYNC;
                if (!SDL_SetGPUSwapchainParameters(g.device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, g.presentMode))
                    return fail(failure, "SDL_SetGPUSwapchainParameters");
            }
            g.swapchainFormat = SDL_GetGPUSwapchainTextureFormat(g.device, window);

            g.vertexShader = ps2x::gpu::createShader(g.device, ps2x::gpu::shaders::present_vert, &g.vertexFormat);
            g.fragmentShader = ps2x::gpu::createShader(g.device, ps2x::gpu::shaders::present_frag, &g.fragmentFormat);
            if (!g.vertexShader || !g.fragmentShader)
                return fail(failure, "creating the present shaders");

            // One alpha blend, the same as SDL_BLENDMODE_BLEND: colour = src * srcA + dst * (1 - srcA),
            // alpha = srcA + dst * (1 - srcA). No vertex buffer (the vertex shader makes the corners), no depth.
            SDL_GPUColorTargetDescription target = {};
            target.format = g.swapchainFormat;
            target.blend_state.enable_blend = true;
            target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            SDL_GPUGraphicsPipelineCreateInfo pipeline = {};
            pipeline.vertex_shader = g.vertexShader;
            pipeline.fragment_shader = g.fragmentShader;
            pipeline.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pipeline.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            pipeline.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            pipeline.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
            pipeline.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
            pipeline.target_info.color_target_descriptions = &target;
            pipeline.target_info.num_color_targets = 1;
            g.pipeline = SDL_CreateGPUGraphicsPipeline(g.device, &pipeline);
            if (!g.pipeline)
                return fail(failure, "SDL_CreateGPUGraphicsPipeline");

            // Nearest for PS2X_SCALE=nearest; linear for bilinear, and also for sharp, which does its own
            // weighting in the shader on top of linear fetches (SDL does the same). Both samplers exist, so the
            // overlay's picture filter setting can switch between them while the game runs.
            for (const bool nearest : {false, true})
            {
                const SDL_GPUFilter filter = nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
                SDL_GPUSamplerCreateInfo sampler = {};
                sampler.min_filter = filter;
                sampler.mag_filter = filter;
                sampler.mipmap_mode = nearest ? SDL_GPU_SAMPLERMIPMAPMODE_NEAREST : SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
                sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
                sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
                sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
                sampler.max_lod = 16.0f; // the picture of the GS GPU renderer at a higher scale has mip levels (the CPU picture has one)
                (nearest ? g.samplerNearest : g.sampler) = SDL_CreateGPUSampler(g.device, &sampler);
                if (!(nearest ? g.samplerNearest : g.sampler))
                    return fail(failure, "SDL_CreateGPUSampler");
            }

            SDL_GPUTextureCreateInfo texture = {};
            texture.type = SDL_GPU_TEXTURETYPE_2D;
            texture.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            texture.width = kPictureWidth;
            texture.height = kPictureHeight;
            texture.layer_count_or_depth = 1;
            texture.num_levels = 1;
            texture.sample_count = SDL_GPU_SAMPLECOUNT_1;
            g.picture = SDL_CreateGPUTexture(g.device, &texture);
            if (!g.picture)
                return fail(failure, "SDL_CreateGPUTexture (picture)");

            SDL_GPUTransferBufferCreateInfo transfer = {};
            transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            transfer.size = static_cast<Uint32>(kPictureWidth) * kPictureHeight * 4u;
            g.transfer = SDL_CreateGPUTransferBuffer(g.device, &transfer);
            if (!g.transfer)
                return fail(failure, "SDL_CreateGPUTransferBuffer (picture)");
            return true;
        }

        void destroy()
        {
            if (g.device)
            {
                SDL_WaitForGPUIdle(g.device);
                if (g.transfer)
                    SDL_ReleaseGPUTransferBuffer(g.device, g.transfer);
                if (g.picture)
                    SDL_ReleaseGPUTexture(g.device, g.picture);
                if (g.sampler)
                    SDL_ReleaseGPUSampler(g.device, g.sampler);
                if (g.samplerNearest)
                    SDL_ReleaseGPUSampler(g.device, g.samplerNearest);
                if (g.pipeline)
                    SDL_ReleaseGPUGraphicsPipeline(g.device, g.pipeline);
                if (g.fragmentShader)
                    SDL_ReleaseGPUShader(g.device, g.fragmentShader);
                if (g.vertexShader)
                    SDL_ReleaseGPUShader(g.device, g.vertexShader);
                if (g.windowClaimed)
                    SDL_ReleaseWindowFromGPUDevice(g.device, g.window);
                SDL_DestroyGPUDevice(g.device);
            }
            ps2x_gs::debugPassSerialization().store(false);
            ps2x_gs::defragGuardEnabled().store(false);
            ps2x_gs::defragGuardStatsEnabled().store(false);
            g = Presenter{};
        }

        std::string driverName()
        {
            const char *driver = g.device ? SDL_GetGPUDeviceDriver(g.device) : nullptr;
            return driver ? driver : "";
        }

        std::string describe()
        {
            if (!g.device)
                return "";
            const char *gpuName = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(g.device), SDL_PROP_GPU_DEVICE_NAME_STRING, "?");
            const char *driver = SDL_GetGPUDeviceDriver(g.device);
            char text[384];
            std::snprintf(text, sizeof(text), "gpu (driver %s, device \"%s\", shaders %s, swapchain %s, present mode %s%s)",
                          driver ? driver : "?", gpuName, g.vertexFormat, formatName(g.swapchainFormat),
                          presentModeName(g.presentMode), g.debugMode ? ", validation on" : "");
            return text;
        }

        void stagePicture(const void *rgbaPixels, int pitchBytes)
        {
            if (!active() || !rgbaPixels)
                return;
            // cycle = true: if the GPU is still reading last frame's contents, SDL hands back fresh storage
            // instead of making this call wait.
            uint8_t *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(g.device, g.transfer, true));
            if (!mapped)
            {
                if (!g.loggedProblem)
                    std::fprintf(stderr, "[host] could not map the picture transfer buffer: %s\n", SDL_GetError());
                g.loggedProblem = true;
                return;
            }
            const size_t rowBytes = static_cast<size_t>(kPictureWidth) * 4u;
            const uint8_t *rows = static_cast<const uint8_t *>(rgbaPixels);
            if (static_cast<size_t>(pitchBytes) == rowBytes)
            {
                std::memcpy(mapped, rows, rowBytes * kPictureHeight);
            }
            else
            {
                const size_t copyBytes = std::min(rowBytes, static_cast<size_t>(std::max(0, pitchBytes)));
                for (int y = 0; y < kPictureHeight; ++y)
                    std::memcpy(mapped + static_cast<size_t>(y) * rowBytes, rows + static_cast<size_t>(y) * static_cast<size_t>(pitchBytes), copyBytes);
            }
            SDL_UnmapGPUTransferBuffer(g.device, g.transfer);
            g.staged = true;
        }

        void setPictureBox(int srcWidth, int srcHeight, float aspect)
        {
            g.boxSourceWidth = srcWidth;
            g.boxSourceHeight = srcHeight;
            g.boxAspect = aspect;
        }

        void printGuardStats()
        {
            const uint64_t now = nowNs();
            if (g.statsLastNs == 0)
            {
                g.statsLastNs = now;
                return;
            }
            if (now - g.statsLastNs < 1000000000ull)
                return;
            const double seconds = static_cast<double>(now - g.statsLastNs) / 1e9;
            const double frames = static_cast<double>(std::max<uint64_t>(g.statsFrames, 1));
            uint64_t cur[2][6];
            for (int role = 0; role < 2; ++role)
            {
                ps2x_gs::GuardRoleStats &r = ps2x_gs::guardStats(role == 1);
                cur[role][0] = r.acquisitions.exchange(0);
                cur[role][1] = r.waits.exchange(0);
                cur[role][2] = r.waitNs.exchange(0);
                cur[role][3] = r.waitMaxNs.exchange(0);
                cur[role][4] = r.holdNs.exchange(0);
                cur[role][5] = r.holdMaxNs.exchange(0);
            }
            // gs / main: acquisitions per second, how many had to wait, wait ms per second (and per presented frame), the worst
            // wait, ms per second the guard was held, the longest hold
            std::fprintf(stderr,
                         "[gpu:guard] t=%.1fs present-mode=%s acquire-first=%d frames=%llu | swapchain acquire avg %.3f max %.3f ms, present() avg %.3f max %.3f ms"
                         " | gs: acq %.0f/s waited %.0f/s wait %.3f ms/s (%.3f ms/frame) max %.3f ms, hold %.2f ms/s max %.3f ms"
                         " | main: acq %.0f/s waited %.0f/s wait %.3f ms/s max %.3f ms, hold %.2f ms/s max %.3f ms\n",
                         static_cast<double>(SDL_GetTicksNS()) / 1e9, presentModeName(g.presentMode), g.acquireFirst ? 1 : 0, static_cast<unsigned long long>(g.statsFrames),
                         static_cast<double>(g.statsAcquireNs) / 1e6 / frames, static_cast<double>(g.statsAcquireMaxNs) / 1e6,
                         static_cast<double>(g.statsPresentNs) / 1e6 / frames, static_cast<double>(g.statsPresentMaxNs) / 1e6,
                         cur[0][0] / seconds, cur[0][1] / seconds, cur[0][2] / 1e6 / seconds, cur[0][2] / 1e6 / frames, cur[0][3] / 1e6,
                         cur[0][4] / 1e6 / seconds, cur[0][5] / 1e6,
                         cur[1][0] / seconds, cur[1][1] / seconds, cur[1][2] / 1e6 / seconds, cur[1][3] / 1e6, cur[1][4] / 1e6 / seconds, cur[1][5] / 1e6);
            std::fprintf(stderr, "[gpu:present-phases] ms per frame: guard wait %.3f, record picture+overlay %.3f, blit %.3f, submit %.3f\n",
                         static_cast<double>(g.statsLockNs) / 1e6 / frames, static_cast<double>(g.statsRecordNs) / 1e6 / frames,
                         static_cast<double>(g.statsBlitNs) / 1e6 / frames, static_cast<double>(g.statsSubmitNs) / 1e6 / frames);
            g.statsLockNs = g.statsRecordNs = g.statsBlitNs = g.statsSubmitNs = 0;
            g.statsLastNs = now;
            g.statsFrames = g.statsAcquireNs = g.statsAcquireMaxNs = g.statsPresentNs = g.statsPresentMaxNs = 0;
        }

        void present()
        {
            if (!active())
                return;
            const uint64_t presentStart = g.guardStats ? nowNs() : 0;
            // Vulkan only: the guard is held from before the first command that uses a texture to the submit, see
            // gs_gpu_threads.h. With acquireFirst the swapchain image is acquired before that (a wait for the display
            // must not keep the GS thread out); otherwise (PS2X_GPU_ACQUIRE_FIRST=0) the guard is held from here, as in M3.
            ps2x_gs::DefragGuard window(!g.acquireFirst);
            SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(g.device);
            if (!commandBuffer)
            {
                if (!g.loggedProblem)
                    std::fprintf(stderr, "[host] SDL_AcquireGPUCommandBuffer failed: %s\n", SDL_GetError());
                g.loggedProblem = true;
                g.overlay = GpuOverlay{};
                return;
            }

            // Blocks until the swapchain has a free image, then hands one over. The texture is null
            // (without an error) when the window cannot be drawn to, for example while minimised; the
            // frame is then skipped, but the commands so far (the picture upload) are still submitted.
            SDL_GPUTexture *swapchain = nullptr;
            Uint32 width = 0, height = 0;
            const auto acquireSwapchain = [&]()
            {
                const uint64_t acquireStart = g.guardStats ? nowNs() : 0;
                if (!SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, g.window, &swapchain, &width, &height))
                {
                    if (!g.loggedProblem)
                        std::fprintf(stderr, "[host] could not acquire the swapchain image: %s\n", SDL_GetError());
                    g.loggedProblem = true;
                    swapchain = nullptr;
                }
                if (g.testAcquireDelayMs)
                    SDL_Delay(g.testAcquireDelayMs);
                if (g.guardStats)
                {
                    const uint64_t ns = nowNs() - acquireStart;
                    g.statsAcquireNs += ns;
                    g.statsAcquireMaxNs = std::max(g.statsAcquireMaxNs, ns);
                }
            };
            uint64_t phaseStart = g.guardStats ? nowNs() : 0;
            const auto endPhase = [&](uint64_t &sum)
            {
                if (!g.guardStats)
                    return;
                const uint64_t now = nowNs();
                sum += now - phaseStart;
                phaseStart = now;
            };
            if (g.acquireFirst)
            {
                acquireSwapchain();
                phaseStart = g.guardStats ? nowNs() : 0;
                window.Lock();
                endPhase(g.statsLockNs);
            }

            // The picture: built on the GPU by the GS renderer's presenter (path B), or the CPU picture uploaded here.
            SDL_GPUTexture *picture = nullptr;
            if (g.source.record)
            {
                picture = g.source.record(commandBuffer, g.source.user);
            }
            else
            {
                if (g.staged)
                {
                    recordUpload(commandBuffer);
                    g.staged = false;
                    g.hasPicture = true;
                }
                picture = g.hasPicture ? g.picture : nullptr;
            }
            if (g.overlay.prepare)
                g.overlay.prepare(commandBuffer, g.overlay.user);
            endPhase(g.statsRecordNs);

            if (!g.acquireFirst)
            {
                acquireSwapchain();
                phaseStart = g.guardStats ? nowNs() : 0;
            }
            if (swapchain && width != 0 && height != 0)
            {
                const int textureWidth = g.source.record ? static_cast<int>(g.source.textureWidth) : kPictureWidth;
                const int textureHeight = g.source.record ? static_cast<int>(g.source.textureHeight) : kPictureHeight;
                recordBlit(commandBuffer, swapchain, width, height, picture, textureWidth, textureHeight);
            }
            endPhase(g.statsBlitNs);

            const bool submitted = SDL_SubmitGPUCommandBuffer(commandBuffer);
            window.Unlock();
            endPhase(g.statsSubmitNs);
            if (!submitted && !g.loggedProblem)
            {
                std::fprintf(stderr, "[host] SDL_SubmitGPUCommandBuffer failed: %s\n", SDL_GetError());
                g.loggedProblem = true;
            }
            // Only now may the GS thread reuse the field textures this frame's commands read.
            if (g.source.submitted)
                g.source.submitted(g.source.user);
            g.overlay = GpuOverlay{};
            if (g.guardStats)
            {
                const uint64_t ns = nowNs() - presentStart;
                g.statsPresentNs += ns;
                g.statsPresentMaxNs = std::max(g.statsPresentMaxNs, ns);
                ++g.statsFrames;
                printGuardStats();
            }
        }
    } // namespace gpu_present
} // namespace ps2x_host
