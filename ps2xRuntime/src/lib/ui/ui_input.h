// SDL3 input into RmlUi: keyboard, mouse and game controller.
//
// The overlay is meant to be used with a controller as much as with a mouse, so a controller drives the same
// keyboard events RmlUi already understands. RmlUi's focus navigation moves between elements on the arrow keys
// (an element opts in with the RCSS properties `tab-index: auto; nav: auto`), "Return" on a focused element is a
// click, and a page can listen for Escape to go back. So:
//
//     D-pad, left stick     arrow keys (with auto-repeat while held)
//     A (south)             Return   (activate)
//     B (east)              Escape   (back)
//     left / right shoulder Page Up / Page Down (the settings page uses them to switch tabs)
//
// The mapping is in ui_input.cpp (gamepadKey). Mouse and keyboard events map one to one.
//
// This class only translates events; it opens no devices. The host already opens the gamepads for the game, and a
// test can push synthetic SDL events (SDL_PushEvent) through the same path.
#pragma once

#include <SDL3/SDL_events.h>

#include <cstdint>

namespace Rml
{
    class Context;
}

namespace ps2x::ui
{
    class Input
    {
    public:
        explicit Input(Rml::Context *context = nullptr) : m_context(context) {}

        void setContext(Rml::Context *context) { m_context = context; }

        // On by default: mouse motion that follows a window change or a key or button press within a moment is ignored
        // (see m_mouseQuietUntilNs). Scripted tests that push mouse and key events back to back turn it off.
        void setMouseGuard(bool on) { m_mouseGuard = on; }

        // SDL reports mouse positions in window coordinates (points), the UI works in pixels. On a high-DPI display
        // pass SDL_GetWindowPixelDensity(window); 1.0 otherwise.
        void setPixelDensity(float density) { m_density = density > 0.0f ? density : 1.0f; }

        // Translates one SDL event. Returns true for events the UI is interested in (keyboard, text, mouse and
        // gamepad events), false for the rest (window, audio, quit...). It does not say whether a widget used the
        // event; the overlay swallows all of the first kind while it is visible.
        bool handleEvent(const SDL_Event &event);

        // Call once per frame with the time since the last call. Repeats a held direction (D-pad or stick).
        void update(double seconds);

        // Sends key-ups for everything held, so nothing stays pressed when the overlay closes or loses focus.
        void releaseAll();

        // True for the event types handleEvent() uses.
        static bool isInputEvent(uint32_t sdlEventType);

    private:
        enum Direction : uint8_t
        {
            kUp = 1,
            kDown = 2,
            kLeft = 4,
            kRight = 8
        };

        void setDirections(uint8_t dpad, uint8_t stick);
        void sendDirection(uint8_t bit, bool down);

        Rml::Context *m_context;
        float m_density = 1.0f;
        uint8_t m_dpad = 0;  // directions held on the D-pad
        uint8_t m_stick = 0; // directions the left stick holds past its threshold
        uint8_t m_held = 0;  // the union, as last sent to the UI
        uint8_t m_repeatBit = 0;
        double m_repeatTimer = 0.0;
        bool m_repeating = false;
        uint32_t m_buttonsHeld = 0; // bit per mapped gamepad button, so releaseAll() can release them
        // Mouse motion that is not the player's hand: the pointer sits where it was, and a window that changes size (full
        // screen on or off) or a key or button press just before makes the system report motion over a row, which would
        // move the focus there (hovering a row focuses it). Mouse motion is ignored until this time (SDL ticks, ns).
        uint64_t m_mouseQuietUntilNs = 0;
        bool m_mouseGuard = true;
        void quietMouse(uint64_t nanoseconds);
    };
}
