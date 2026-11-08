#include "ui_input.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Input.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <cmath>

namespace ps2x::ui
{
    namespace
    {
        using Rml::Input::KeyIdentifier;

        // How long a direction is held before it repeats, and the gap between repeats. The same feel as a keyboard.
        constexpr double kRepeatDelay = 0.40;
        constexpr double kRepeatInterval = 0.10;

        // The left stick counts as a direction past this share of its range, and stays one until it falls under the
        // lower value, so a stick resting near the edge does not flicker.
        constexpr int kStickOn = 16000;  // of 32767
        constexpr int kStickOff = 10000;

        KeyIdentifier keyIdentifier(SDL_Keycode key)
        {
            using namespace Rml::Input;
            if (key >= SDLK_A && key <= SDLK_Z)
                return static_cast<KeyIdentifier>(KI_A + (key - SDLK_A));
            if (key >= SDLK_0 && key <= SDLK_9)
                return static_cast<KeyIdentifier>(KI_0 + (key - SDLK_0));
            if (key >= SDLK_KP_1 && key <= SDLK_KP_9)
                return static_cast<KeyIdentifier>(KI_NUMPAD1 + (key - SDLK_KP_1));
            if (key >= SDLK_F1 && key <= SDLK_F12)
                return static_cast<KeyIdentifier>(KI_F1 + (key - SDLK_F1));
            switch (key)
            {
            case SDLK_KP_0: return KI_NUMPAD0;
            case SDLK_KP_ENTER: return KI_NUMPADENTER;
            case SDLK_KP_MULTIPLY: return KI_MULTIPLY;
            case SDLK_KP_PLUS: return KI_ADD;
            case SDLK_KP_MINUS: return KI_SUBTRACT;
            case SDLK_KP_PERIOD: return KI_DECIMAL;
            case SDLK_KP_DIVIDE: return KI_DIVIDE;
            case SDLK_SPACE: return KI_SPACE;
            case SDLK_BACKSPACE: return KI_BACK;
            case SDLK_TAB: return KI_TAB;
            case SDLK_RETURN: return KI_RETURN;
            case SDLK_ESCAPE: return KI_ESCAPE;
            case SDLK_PAGEUP: return KI_PRIOR;
            case SDLK_PAGEDOWN: return KI_NEXT;
            case SDLK_END: return KI_END;
            case SDLK_HOME: return KI_HOME;
            case SDLK_LEFT: return KI_LEFT;
            case SDLK_UP: return KI_UP;
            case SDLK_RIGHT: return KI_RIGHT;
            case SDLK_DOWN: return KI_DOWN;
            case SDLK_INSERT: return KI_INSERT;
            case SDLK_DELETE: return KI_DELETE;
            case SDLK_SEMICOLON: return KI_OEM_1;
            case SDLK_EQUALS: return KI_OEM_PLUS;
            case SDLK_COMMA: return KI_OEM_COMMA;
            case SDLK_MINUS: return KI_OEM_MINUS;
            case SDLK_PERIOD: return KI_OEM_PERIOD;
            case SDLK_SLASH: return KI_OEM_2;
            case SDLK_GRAVE: return KI_OEM_3;
            case SDLK_LEFTBRACKET: return KI_OEM_4;
            case SDLK_BACKSLASH: return KI_OEM_5;
            case SDLK_RIGHTBRACKET: return KI_OEM_6;
            case SDLK_APOSTROPHE: return KI_OEM_7;
            case SDLK_LSHIFT: return KI_LSHIFT;
            case SDLK_RSHIFT: return KI_RSHIFT;
            case SDLK_LCTRL: return KI_LCONTROL;
            case SDLK_RCTRL: return KI_RCONTROL;
            case SDLK_LALT: return KI_LMENU;
            case SDLK_RALT: return KI_RMENU;
            default: return KI_UNKNOWN;
            }
        }

        int modifiers(SDL_Keymod mod)
        {
            int result = 0;
            if (mod & SDL_KMOD_CTRL)
                result |= Rml::Input::KM_CTRL;
            if (mod & SDL_KMOD_SHIFT)
                result |= Rml::Input::KM_SHIFT;
            if (mod & SDL_KMOD_ALT)
                result |= Rml::Input::KM_ALT;
            if (mod & SDL_KMOD_GUI)
                result |= Rml::Input::KM_META;
            if (mod & SDL_KMOD_CAPS)
                result |= Rml::Input::KM_CAPSLOCK;
            if (mod & SDL_KMOD_NUM)
                result |= Rml::Input::KM_NUMLOCK;
            return result;
        }

        // Gamepad button to the key the UI sees. KI_UNKNOWN for buttons the UI does not use. The D-pad is handled
        // separately (it feeds the directions with auto-repeat).
        KeyIdentifier gamepadKey(uint8_t button)
        {
            switch (button)
            {
            case SDL_GAMEPAD_BUTTON_SOUTH: return Rml::Input::KI_RETURN;       // A: activate
            case SDL_GAMEPAD_BUTTON_EAST: return Rml::Input::KI_ESCAPE;        // B: back
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return Rml::Input::KI_PRIOR;  // previous tab
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return Rml::Input::KI_NEXT;  // next tab
            default: return Rml::Input::KI_UNKNOWN;
            }
        }

        // Mapped gamepad buttons, numbered for the bit mask in Input::m_buttonsHeld.
        constexpr uint8_t kMappedButtons[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
                                               SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER};

        int mappedIndex(uint8_t button)
        {
            for (int i = 0; i < 4; ++i)
                if (kMappedButtons[i] == button)
                    return i;
            return -1;
        }

        KeyIdentifier directionKey(uint8_t bit)
        {
            switch (bit)
            {
            case 1: return Rml::Input::KI_UP;
            case 2: return Rml::Input::KI_DOWN;
            case 4: return Rml::Input::KI_LEFT;
            default: return Rml::Input::KI_RIGHT;
            }
        }
    }

    bool Input::isInputEvent(uint32_t t)
    {
        switch (t)
        {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            return true;
        default:
            return false;
        }
    }

    void Input::sendDirection(uint8_t bit, bool down)
    {
        if (!m_context)
            return;
        if (down)
            m_context->ProcessKeyDown(directionKey(bit), 0);
        else
            m_context->ProcessKeyUp(directionKey(bit), 0);
    }

    // Applies the D-pad and stick states: sends a key-down for each direction that became held and a key-up for each
    // that was released, and starts or stops the repeat.
    void Input::setDirections(uint8_t dpad, uint8_t stick)
    {
        m_dpad = dpad;
        m_stick = stick;
        const uint8_t now = static_cast<uint8_t>(dpad | stick);
        const uint8_t pressed = static_cast<uint8_t>(now & ~m_held);
        const uint8_t released = static_cast<uint8_t>(m_held & ~now);
        for (uint8_t bit = 1; bit <= 8; bit = static_cast<uint8_t>(bit << 1))
        {
            if (released & bit)
                sendDirection(bit, false);
            if (pressed & bit)
            {
                sendDirection(bit, true);
                m_repeatBit = bit; // the newest press is the one that repeats, as on a keyboard
                m_repeatTimer = 0.0;
                m_repeating = false;
            }
        }
        m_held = now;
        if ((m_held & m_repeatBit) == 0)
        {
            // The repeating direction was let go: fall back to whatever else is still held.
            m_repeatBit = 0;
            for (uint8_t bit = 1; bit <= 8; bit = static_cast<uint8_t>(bit << 1))
                if (m_held & bit)
                    m_repeatBit = bit;
            m_repeatTimer = 0.0;
            m_repeating = false;
        }
    }

    void Input::update(double seconds)
    {
        if (!m_repeatBit)
            return;
        m_repeatTimer += seconds;
        const double threshold = m_repeating ? kRepeatInterval : kRepeatDelay;
        if (m_repeatTimer >= threshold)
        {
            m_repeatTimer -= threshold;
            m_repeating = true;
            sendDirection(m_repeatBit, true);
        }
    }

    void Input::releaseAll()
    {
        if (!m_context)
            return;
        setDirections(0, 0);
        for (int i = 0; i < 4; ++i)
        {
            if (m_buttonsHeld & (1u << i))
                m_context->ProcessKeyUp(gamepadKey(kMappedButtons[i]), 0);
        }
        m_buttonsHeld = 0;
    }

    bool Input::handleEvent(const SDL_Event &e)
    {
        if (!m_context)
            return false;
        switch (e.type)
        {
        case SDL_EVENT_KEY_DOWN:
        {
            const KeyIdentifier key = keyIdentifier(e.key.key);
            if (key != Rml::Input::KI_UNKNOWN)
                m_context->ProcessKeyDown(key, modifiers(e.key.mod));
            return true;
        }
        case SDL_EVENT_KEY_UP:
        {
            const KeyIdentifier key = keyIdentifier(e.key.key);
            if (key != Rml::Input::KI_UNKNOWN)
                m_context->ProcessKeyUp(key, modifiers(e.key.mod));
            return true;
        }
        case SDL_EVENT_TEXT_INPUT:
            m_context->ProcessTextInput(Rml::String(e.text.text));
            return true;
        case SDL_EVENT_MOUSE_MOTION:
            m_context->ProcessMouseMove(static_cast<int>(std::lround(e.motion.x * m_density)),
                                        static_cast<int>(std::lround(e.motion.y * m_density)), modifiers(SDL_GetModState()));
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            // SDL: 1 left, 2 middle, 3 right. RmlUi: 0 left, 1 right, 2 middle.
            int button = -1;
            switch (e.button.button)
            {
            case SDL_BUTTON_LEFT: button = 0; break;
            case SDL_BUTTON_RIGHT: button = 1; break;
            case SDL_BUTTON_MIDDLE: button = 2; break;
            default: break;
            }
            if (button >= 0)
            {
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                    m_context->ProcessMouseButtonDown(button, modifiers(SDL_GetModState()));
                else
                    m_context->ProcessMouseButtonUp(button, modifiers(SDL_GetModState()));
            }
            return true;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            // SDL: positive y is away from the user (up). RmlUi: positive is down.
            m_context->ProcessMouseWheel(Rml::Vector2f(-e.wheel.x, -e.wheel.y), modifiers(SDL_GetModState()));
            return true;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            m_context->ProcessMouseLeave();
            return true;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        {
            const bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            const uint8_t button = e.gbutton.button;
            uint8_t bit = 0;
            switch (button)
            {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: bit = kUp; break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: bit = kDown; break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: bit = kLeft; break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: bit = kRight; break;
            default: break;
            }
            if (bit)
            {
                setDirections(down ? static_cast<uint8_t>(m_dpad | bit) : static_cast<uint8_t>(m_dpad & ~bit), m_stick);
                return true;
            }
            const KeyIdentifier key = gamepadKey(button);
            const int index = mappedIndex(button);
            if (key != Rml::Input::KI_UNKNOWN && index >= 0)
            {
                if (down)
                {
                    m_buttonsHeld |= 1u << index;
                    m_context->ProcessKeyDown(key, 0);
                }
                else
                {
                    m_buttonsHeld &= ~(1u << index);
                    m_context->ProcessKeyUp(key, 0);
                }
            }
            return true;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        {
            if (e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTX && e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY)
                return true;
            const int value = e.gaxis.value;
            uint8_t stick = m_stick;
            const uint8_t neg = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? kLeft : kUp;
            const uint8_t pos = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? kRight : kDown;
            stick = static_cast<uint8_t>(stick & ~(neg | pos));
            auto held = [&](uint8_t bit, bool positive) {
                const int v = positive ? value : -value;
                return v >= kStickOn || (v >= kStickOff && (m_stick & bit));
            };
            if (held(neg, false))
                stick |= neg;
            if (held(pos, true))
                stick |= pos;
            setDirections(m_dpad, stick);
            return true;
        }
        default:
            return false;
        }
    }
}
