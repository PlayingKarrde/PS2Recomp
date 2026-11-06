#pragma once

// The desktop host's SDL_GPU presenter: internal to the host layer, not part of ps2_host_backend.h.
//
// This header includes SDL, so nothing the generated game code uses may include it. It is used by
// ps2_host_sdl.cpp (which owns the window and chooses between this presenter and the SDL_Renderer
// fallback) and by ps2_debug_panel.cpp (which draws its ImGui windows through the same device).
//
// What the presenter does, once per host frame, all on the main thread (the thread that created the
// window; with the GS GPU renderer (PS2X_GS_BACKEND=gpu) the game thread also records and submits its own
// command buffers on this device, but never touches the window or the swapchain):
//   1. stagePicture()   copies the CPU picture into a transfer buffer (cycled, so it never waits for the GPU).
//   2. present()        acquires one command buffer and records, in order:
//        - recordUpload()  a copy pass: transfer buffer -> the 640x512 RGBA8 picture texture (cycled),
//        - the debug panel's prepare callback (ImGui uploads its vertex buffers; must be outside a render pass),
//        - recordBlit()    a render pass on the swapchain image: clear to black, draw the 4:3 box with
//                          the picture texture through shaders/present.{vert,frag}.hlsl, then the
//                          debug panel's draw callback.
//      and submits it.
// recordUpload() and recordBlit() are separate on purpose. recordBlit() takes the texture to draw as a
// parameter; a later milestone (the GPU GS backend) will call it with its own frame target and no
// recordUpload(), so the CPU picture and the transfer buffer disappear from that path. That path is not
// built yet.
//
// The device is created once, with every shader format SDL_GPU can use (SPIR-V, DXIL, MSL), because the
// GS GPU backend will share it (gpuDevice()).

#include "ps2_host_backend.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <string>
#include <vector>

namespace ps2x_host
{
    // The SDL_GPU device while the GPU presenter is the active one; nullptr with the SDL_Renderer fallback.
    SDL_GPUDevice *gpuDevice();
    // Colour format of the swapchain images (what a render pass that draws to the window must declare).
    SDL_GPUTextureFormat gpuSwapchainFormat();

    // Something drawn on top of the picture in the same render pass, set by the debug panel for one frame.
    // `prepare` runs after the picture upload and before the render pass; `draw` runs inside the pass.
    // Either may be null. present() forgets the overlay, so the owner sets it again every frame it shows.
    struct GpuOverlay
    {
        void (*prepare)(SDL_GPUCommandBuffer *commandBuffer, void *user) = nullptr;
        void (*draw)(SDL_GPUCommandBuffer *commandBuffer, SDL_GPURenderPass *renderPass, void *user) = nullptr;
        void *user = nullptr;
    };
    void setGpuOverlay(const GpuOverlay &overlay);

    // The direct picture path (the GS GPU renderer's path B, gs_gpu_present.h). While a source is set, present()
    // asks it for the picture texture instead of uploading the CPU picture. Everything is called on the main thread.
    struct PictureSource
    {
        // Records the commands that produce the picture into `commandBuffer` (outside any pass) and returns the
        // textureWidth x textureHeight RGBA8 texture to draw (the picture in its top-left corner), or null.
        SDL_GPUTexture *(*record)(SDL_GPUCommandBuffer *commandBuffer, void *user) = nullptr;
        // The size of that texture: kPictureWidth x kPictureHeight at scale 1, that times the scale of the GS renderer above
        // (internal resolution, GPU renderer M4b). Fixed for as long as the source is set.
        uint32_t textureWidth = 640, textureHeight = 512;
        // Called after that command buffer was submitted.
        void (*submitted)(void *user) = nullptr;
        // The picture texture's pixels, waiting for the GPU (PS2X_PRESENT_DUMP).
        bool (*download)(void *user, std::vector<uint8_t> &rgba) = nullptr;
        void *user = nullptr;
    };

    namespace gpu_present
    {
        void setPictureSource(const PictureSource &source);
        bool hasPictureSource();
        // The pixels of the picture texture the source made (its textureWidth x textureHeight RGBA8). False without a source.
        bool downloadPicture(std::vector<uint8_t> &rgba);
        // The size of the picture texture the source makes (kPictureWidth x kPictureHeight without a source).
        void pictureTextureSize(int &width, int &height);

        // Creates the device, claims the window and builds the pipeline. Returns false and fills
        // `failure` (and releases everything it made) when any step fails; the caller then falls back
        // to SDL_Renderer.
        bool create(SDL_Window *window, ScaleMode scale, std::string &failure);
        void destroy();
        bool active();

        // One line about the device for the startup log: driver, GPU, shader format, swapchain, present mode.
        std::string describe();
        // The name of the driver SDL picked ("direct3d12", "vulkan", "metal"), or "".
        std::string driverName();

        // Copies a kPictureWidth x kPictureHeight RGBA picture (`pitchBytes` between rows) for the next present().
        void stagePicture(const void *rgbaPixels, int pitchBytes);
        // What present() draws: the top-left srcWidth x srcHeight pixels of the picture, in the largest box
        // of shape `aspect` (width / height; <= 0 means the picture's own shape) that fits the window, centred.
        void setPictureBox(int srcWidth, int srcHeight, float aspect);
        // Records, submits and queues for presentation one frame. A window that cannot be drawn to right
        // now (minimised) just skips the drawing.
        void present();
    }
}
