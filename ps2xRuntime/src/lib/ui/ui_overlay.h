// The UI overlay: RmlUi set up for the game, drawn through SDL_GPU over the game picture.
//
// This is the one class the host (and the standalone demo) talks to. It owns the RmlUi context, the SDL_GPU
// renderer (ui_render_sdlgpu.h), the input translation (ui_input.h) and the fonts and theme loaded from an asset
// folder. The header uses SDL but not RmlUi, so a host file can include it without pulling RmlUi's headers in.
//
// How the host uses it, per frame (the game picture is already in `swapchain`, its render pass has ended):
//
//     overlay.setViewport(windowWidthPx, windowHeightPx);   // cheap when nothing changed
//     overlay.update(dtSeconds);                            // layout, animations, controller auto-repeat
//     overlay.record(cmd, swapchain);                       // records one more render pass over the picture
//
// and for every SDL event: `if (overlay.handleEvent(e)) continue;`. While the overlay is visible it takes all
// keyboard, mouse and gamepad events, so the game gets none. Hidden, it takes none and record() does nothing.
//
// Not thread-safe: use it from the thread that owns the window and the command buffer (the host's main thread).
#pragma once

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <memory>
#include <string>

namespace Rml
{
    class Context;
    class ElementDocument;
}

namespace ps2x::ui
{
    class SdlGpuRenderer;

    struct OverlayConfig
    {
        SDL_GPUDevice *device = nullptr;
        // The colour format of whatever the UI is drawn on: SDL_GetGPUSwapchainTextureFormat() for the window, or
        // the format of an offscreen texture.
        SDL_GPUTextureFormat targetFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
        // The folder that holds fonts/, theme/ and pages/ (see src/lib/ui/assets in the source tree).
        std::string assetRoot;
        // Optional. The window gets the mouse cursor RmlUi asks for.
        SDL_Window *window = nullptr;
    };

    // What the UI has logged since init (RmlUi warnings are usually a page or style sheet problem).
    struct LogCounts
    {
        int errors = 0;
        int warnings = 0;
        int infos = 0;
    };

    class Overlay
    {
    public:
        Overlay();
        ~Overlay();
        Overlay(const Overlay &) = delete;
        Overlay &operator=(const Overlay &) = delete;

        // RmlUi keeps process-wide state, so one Overlay can exist at a time. Loads the fonts; returns false and logs
        // why when the renderer or the fonts fail.
        bool init(const OverlayConfig &config);
        void shutdown();
        bool ready() const { return m_impl != nullptr; }

        // The target's size in pixels, and the UI scale: 1.0 lays pages out as designed for a 720 pixel high, 1000 pixel
        // wide window (RCSS `dp` units are scale pixels). scale <= 0 picks the smaller of height / 720 and width / 1000,
        // rounded down to a multiple of 1/8, so the page keeps its proportions, fits a narrow window, and RmlUi does not
        // build a font atlas for every window size on the way.
        void setViewport(int width, int height, float scale = 0.0f);
        int width() const;
        int height() const;
        float scale() const;
        // SDL mouse positions are in window points; pass SDL_GetWindowPixelDensity(window) on a high-DPI display.
        void setPixelDensity(float density);
        // See Input::setMouseGuard: on by default, off for scripted tests.
        void setMouseGuard(bool on);

        // Loads a page from the asset folder (for example "pages/settings.rml") and shows it. Returns nullptr and
        // logs on failure. A page stays loaded until closePages().
        Rml::ElementDocument *loadPage(const std::string &relativePath);
        void closePages();
        Rml::Context *context();

        void setVisible(bool visible);
        bool visible() const;

        // Returns true when the overlay used the event, so the game should not see it (always for input events
        // while visible, never while hidden).
        bool handleEvent(const SDL_Event &event);
        void update(double seconds);
        // Draws the UI onto `target` (loading what is there). The command buffer is the caller's; nothing is submitted.
        void record(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target);

        // For tests and the demo (drawing a backdrop through the same renderer).
        SdlGpuRenderer &renderer();
        LogCounts logCounts() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
