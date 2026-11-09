#pragma once

// The settings overlay inside the desktop host: internal to the host layer, like ps2_host_gpu.h. Includes SDL, so
// nothing the generated game code uses may include it.
//
// What it is: the RmlUi overlay from src/lib/ui (a settings menu, drawn over the game picture) plus the glue that
// makes it part of the game. The overlay itself knows nothing about the game; this file's .cpp is the game side:
//   - opening and closing: the F2 key, the controller's Guide button, or L3 + R3 held together (see ps2_host_ui.cpp
//     for why these and not others);
//   - while it is open: it takes all keyboard, mouse and controller input (the host hides the pad from the game),
//     the guest is paused (ps2_pause.h), and the sound is silent;
//   - the settings it shows, kept in the settings file (ps2_settings.h) and applied through the setters below.
//
// When the build has no UI (PS2X_BUILD_UI off) or the SDL_GPU presenter is not in use, every function here does
// nothing and active() is false: the game runs as before, with one log line saying why.
//
// All functions are for the host's main thread unless noted.

#include "ps2_host_backend.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <cstdint>

namespace ps2x_host
{
    // Settings the overlay changes live. Defined in ps2_host_sdl.cpp and ps2_host_gpu.cpp.
    bool windowFullscreen();
    void setWindowFullscreen(bool fullscreen);
    // 0..1, on top of the base gain (PS2X_AUDIO_GAIN). Any thread.
    void setMasterVolume(float volume);
    float masterVolume();
    // While true the game reads no keyboard, mouse or controller input; the host keeps tracking the real state. When it
    // becomes false again, whatever is still held stays hidden from the game until it is released, and presses that
    // arrived in the meantime are dropped, so nothing sticks or leaks. Any thread.
    void setInputCapturedByOverlay(bool captured);

    namespace gpu_present
    {
        // True: wait for the display (SDL_GPU_PRESENTMODE_VSYNC). False: the host's usual choice, immediate if the window
        // can do it, else mailbox, else vsync.
        void setVsync(bool vsync);
        bool vsync();
        // The picture filter: "sharp", "linear" or "nearest" (anything else is ignored).
        void setPictureFilter(ScaleMode mode);
        ScaleMode pictureFilter();
    }

    namespace ui
    {
        // Creates the overlay for the window and its SDL_GPU device. Returns false (after one log line) when it cannot
        // be had: no UI in this build, PS2X_UI=0, or the assets are missing.
        bool create(SDL_Window *window, SDL_GPUDevice *device, SDL_GPUTextureFormat swapchainFormat);
        // Releases it. Call before the device goes away.
        void destroy();
        bool active();
        bool visible();

        // For every SDL event, after the debug panel's hook and before the game's own handling. Looks for the keys and
        // buttons that open and close the overlay and, while it is open, hands it the input. Returns true when the
        // overlay used the event.
        bool handleEvent(const SDL_Event &event);
        // Once per host frame, before the frame is presented: timers (the L3 + R3 hold), the deferred close, layout.
        void update();
        // Records the overlay over the picture in `swapchain` (its own render passes, loading what is there). Nothing
        // happens when the overlay is closed.
        void record(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUTexture *swapchain, uint32_t width, uint32_t height);
        // PS2X_TEST_PAD=<file>: applies the commands a script appended to the file to an SDL virtual gamepad that the host
        // attached (see ps2_host_ui.cpp). Called before the events are polled; does nothing without the variable.
        void pumpTestPad();
    }
}
