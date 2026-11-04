#pragma once

// The host layer: everything the runtime needs from the machine it runs on, which is the window,
// the picture, keyboard and controller input, and audio output.
//
// Desktop builds implement this with SDL3 in src/lib/ps2_host_sdl.cpp. The functions here are
// plain C++ so the kernel stubs and the pad code that include this header do not need SDL's headers.
// The PS Vita build keeps using raylib directly and is not built by us: for the input functions it
// gets inline versions below, and ps2_runtime.cpp keeps its old raylib presentation code under
// PLATFORM_VITA.
//
// Nothing generated from the game includes this header.

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(PLATFORM_VITA)
#include "raylib.h"
#endif

namespace ps2x_host
{
    // Keyboard keys the pad mapping uses. A key is named by its place on a US keyboard
    // (a "scancode"), not by the letter it types, so the layout does not matter.
    enum class Key : uint8_t
    {
        Up,
        Down,
        Left,
        Right,
        W,
        A,
        S,
        D,
        X,
        C,
        Z,
        V,
        Q,
        E,
        Space,
        Escape,
        Enter,
        Tab,
        LeftShift,
        RightShift,
        LeftCtrl,
        RightCtrl,
        One,
        Three,
        Keypad0,
        Keypad1,
        F1,
        Count
    };

    enum class PadButton : uint8_t
    {
        Up,
        Down,
        Left,
        Right,
        Cross,
        Circle,
        Square,
        Triangle,
        L1,
        R1,
        L2,
        R2,
        L3,
        R3,
        Start,
        Select,
        Count
    };

    enum class PadAxis : uint8_t
    {
        LeftX,
        LeftY,
        RightX,
        RightY
    };

    // Who is asking. A press that begins and ends between two reads is remembered ("latched")
    // until each reader has seen it once, so a short tap is never lost. Every reader gets its own
    // copy of the latch. Pad ports 0 and 1 use readers 0 and 1.
    enum Reader : int
    {
        ReaderPort0 = 0,
        ReaderPort1 = 1,
        ReaderUi = 2,
        ReaderDebug = 3,
        ReaderCount = 4
    };

    enum class ScaleMode : uint8_t
    {
        Sharp,    // whole-pixel scaling with soft pixel edges (SDL_SCALEMODE_PIXELART)
        Nearest,
        Bilinear
    };

#if defined(PLATFORM_VITA)
    inline bool windowReady() { return IsWindowReady(); }
    inline bool gamepadAvailable() { return IsGamepadAvailable(0); }
    inline float gamepadAxis(PadAxis axis)
    {
        switch (axis)
        {
        case PadAxis::LeftX:
            return GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
        case PadAxis::LeftY:
            return GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
        case PadAxis::RightX:
            return GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_X);
        default:
            return GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_Y);
        }
    }
    inline bool gamepadButtonDown(PadButton button, int)
    {
        static const int map[] = {GAMEPAD_BUTTON_LEFT_FACE_UP, GAMEPAD_BUTTON_LEFT_FACE_DOWN,
                                  GAMEPAD_BUTTON_LEFT_FACE_LEFT, GAMEPAD_BUTTON_LEFT_FACE_RIGHT,
                                  GAMEPAD_BUTTON_RIGHT_FACE_DOWN, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT,
                                  GAMEPAD_BUTTON_RIGHT_FACE_LEFT, GAMEPAD_BUTTON_RIGHT_FACE_UP,
                                  GAMEPAD_BUTTON_LEFT_TRIGGER_1, GAMEPAD_BUTTON_RIGHT_TRIGGER_1,
                                  GAMEPAD_BUTTON_LEFT_TRIGGER_2, GAMEPAD_BUTTON_RIGHT_TRIGGER_2,
                                  GAMEPAD_BUTTON_LEFT_THUMB, GAMEPAD_BUTTON_RIGHT_THUMB,
                                  GAMEPAD_BUTTON_MIDDLE_RIGHT, GAMEPAD_BUTTON_MIDDLE_LEFT};
        return IsGamepadButtonDown(0, map[static_cast<int>(button)]);
    }
    inline bool keyDown(Key key, int)
    {
        static const int map[] = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_W, KEY_A, KEY_S, KEY_D, KEY_X, KEY_C,
                                  KEY_Z, KEY_V, KEY_Q, KEY_E, KEY_SPACE, KEY_ESCAPE, KEY_ENTER, KEY_TAB,
                                  KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT, KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL, KEY_ONE,
                                  KEY_THREE, KEY_KP_0, KEY_KP_1, KEY_F1};
        return IsKeyDown(map[static_cast<int>(key)]);
    }
#else
    // ---- Input (safe to call from any thread, before or after the window exists) ----

    // True while the key is held, or if it went down since this reader last asked about it.
    bool keyDown(Key key, int reader);
    // True once per press: only if the key went down since this reader last asked (not while held).
    bool keyPressed(Key key, int reader);

    bool gamepadAvailable();
    bool gamepadButtonDown(PadButton button, int reader);
    // -1 to 1; 0 when no controller is connected. Down and right are positive.
    float gamepadAxis(PadAxis axis);
    // Name SDL gives the controller in use, or "" when none.
    std::string gamepadName();

    // While true, the game reads no keyboard input. The debug panel sets it while it is showing.
    void setKeyboardCapturedByUi(bool captured);

    // ---- Window and picture (main thread only) ----

    // Starts SDL, opens a resizable window and a renderer, and creates the texture the game's
    // picture is shown through. Returns false and prints the reason on failure.
    bool createWindow(const char *title, int width, int height, ScaleMode scale);
    bool windowReady();
    void destroyWindow();

    // Handles every pending window, keyboard and controller event. Returns false once the user
    // asked to close the window (or the process was asked to quit).
    bool pumpEvents();

    // Size of the picture texture: the largest picture the runtime shows (see FB_WIDTH/FB_HEIGHT in
    // ps2_runtime.cpp). Smaller pictures use its top-left corner.
    constexpr int kPictureWidth = 640;
    constexpr int kPictureHeight = 512;

    // Replaces the whole picture texture with kPictureWidth x kPictureHeight RGBA pixels,
    // `pitchBytes` between rows.
    void updatePicture(const void *rgbaPixels, int pitchBytes);

    // Clears the window, then draws the top-left srcWidth x srcHeight pixels of the picture
    // texture into the largest `aspect` (width / height) shaped box that fits, centred.
    void drawPicture(int srcWidth, int srcHeight, float aspect);

    // Shows what was drawn, then waits until 1/60 s has passed since the previous call.
    void presentAndPace();

    // ---- Debug panel hooks ----

    // The panel's ImGui backends need these. The pointers are SDL_Window* and SDL_Renderer*.
    void *nativeWindow();
    void *nativeRenderer();
    // Called for every SDL event on the main thread, before the game sees it (the pointer is an SDL_Event*).
    void setEventHook(void (*hook)(const void *sdlEvent));

    // ---- Audio ----

    // Fills `frames` stereo frames of signed 16-bit samples. Called on the audio thread.
    using AudioMixFn = void (*)(void *user, int16_t *interleavedStereo, unsigned frames);

    // Opens the default playback device with a 48 kHz stereo stream pulling from `mix`.
    // PS2X_AUDIO_STATS=1 prints the peak and RMS of what is handed to SDL once a second.
    bool startAudioStream(AudioMixFn mix, void *user);
    void stopAudioStream();
    // True once the audio subsystem started and a device was opened.
    bool audioReady();
#endif
} // namespace ps2x_host
