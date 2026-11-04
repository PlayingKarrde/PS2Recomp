#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <cstring>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

bool PSPadBackend::readState(int port, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    using ps2x_host::Key;
    using ps2x_host::PadAxis;
    using ps2x_host::PadButton;
    // Each pad port is its own reader, so a quick tap is seen once by each (see ps2_host_backend.h).
    const int reader = (port == 1) ? ps2x_host::ReaderPort1 : ps2x_host::ReaderPort0;
    uint16_t btns = 0xFFFFu;
    const bool useGamepad = ps2x_host::gamepadAvailable();
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };
    auto key = [reader](Key k)
    { return ps2x_host::keyDown(k, reader); };
    auto pad = [reader](PadButton b)
    { return ps2x_host::gamepadButtonDown(b, reader); };

    if (useGamepad)
    {
        if (pad(PadButton::Up))
            clearBit(PAD_UP);
        if (pad(PadButton::Down))
            clearBit(PAD_DOWN);
        if (pad(PadButton::Left))
            clearBit(PAD_LEFT);
        if (pad(PadButton::Right))
            clearBit(PAD_RIGHT);
        if (pad(PadButton::Cross))
            clearBit(PAD_CROSS);
        if (pad(PadButton::Circle))
            clearBit(PAD_CIRCLE);
        if (pad(PadButton::Square))
            clearBit(PAD_SQUARE);
        if (pad(PadButton::Triangle))
            clearBit(PAD_TRIANGLE);
        if (pad(PadButton::L1))
            clearBit(PAD_L1);
        if (pad(PadButton::R1))
            clearBit(PAD_R1);
        if (pad(PadButton::L2))
            clearBit(PAD_L2);
        if (pad(PadButton::R2))
            clearBit(PAD_R2);
        if (pad(PadButton::Start))
            clearBit(PAD_START);
        if (pad(PadButton::Select))
            clearBit(PAD_SELECT);
        if (pad(PadButton::L3))
            clearBit(PAD_L3);
        if (pad(PadButton::R3))
            clearBit(PAD_R3);

        float lx = ps2x_host::gamepadAxis(PadAxis::LeftX);
        float ly = ps2x_host::gamepadAxis(PadAxis::LeftY);
        float rx = ps2x_host::gamepadAxis(PadAxis::RightX);
        float ry = ps2x_host::gamepadAxis(PadAxis::RightY);
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
    }

    // The keyboard stays live with a controller connected. Every key is asked about exactly once,
    // so each one's latch is consumed once per read (a || would skip the second key and leave its
    // latch set for later).
    const bool up = key(Key::Up), w = key(Key::W);
    const bool down = key(Key::Down), s = key(Key::S);
    const bool left = key(Key::Left), a = key(Key::A);
    const bool right = key(Key::Right), d = key(Key::D);
    const bool x = key(Key::X), space = key(Key::Space);
    const bool c = key(Key::C), escape = key(Key::Escape);
    const bool z = key(Key::Z), kp0 = key(Key::Keypad0);
    const bool v = key(Key::V), kp1 = key(Key::Keypad1);
    if (up || w)
        clearBit(PAD_UP);
    if (down || s)
        clearBit(PAD_DOWN);
    if (left || a)
        clearBit(PAD_LEFT);
    if (right || d)
        clearBit(PAD_RIGHT);
    if (x || space)
        clearBit(PAD_CROSS);
    if (c || escape)
        clearBit(PAD_CIRCLE);
    if (z || kp0)
        clearBit(PAD_SQUARE);
    if (v || kp1)
        clearBit(PAD_TRIANGLE);
    if (key(Key::Q))
        clearBit(PAD_L1);
    if (key(Key::E))
        clearBit(PAD_R1);
    if (key(Key::LeftShift))
        clearBit(PAD_L2);
    if (key(Key::RightShift))
        clearBit(PAD_R2);
    if (key(Key::Enter))
        clearBit(PAD_START);
    if (key(Key::Tab))
        clearBit(PAD_SELECT);

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
