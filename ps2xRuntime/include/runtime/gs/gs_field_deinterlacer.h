#pragma once

// FPWR: the CPU deinterlacer that turns the game's 224-line field pictures into 448-line pictures.
// It used to live in ps2_runtime.cpp; the replayer and the tests include it too, so the GPU version
// (shaders/field_deint.frag.hlsl, driven by gs_gpu_present.cpp) can be checked against it.
// The code is unchanged. No header that generated game code includes refers to this one.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

// Games in the PS2's field mode draw one 224-line picture per VBlank, with every other picture
// half a line higher. A TV lays those pictures between each other's lines. Shown one at a time
// they bounce, so we rebuild the 448-line picture: lines from the newest picture stay, and the
// lines between come from the previous picture where nothing moved and are interpolated where
// something did.
class GSFieldDeinterlacer
{
public:
    void reset()
    {
        m_hasPrevious = false;
        m_hasOlder = false;
    }

    // For tests and diagnostics: what the last process() call measured (see currentIsLower).
    struct Decision
    {
        bool measured = false; // false: no previous picture, the old answer was kept
        bool lower = false;
        int64_t lowerError = 0, upperError = 0;
    };
    const Decision &lastDecision() const { return m_decision; }

    void process(const uint8_t *current, uint32_t width, uint32_t height, uint8_t *output, size_t outputStride)
    {
        const size_t stride = static_cast<size_t>(width) * 4u;
        const size_t bytes = stride * height;
        if (m_previous.size() != bytes)
        {
            m_previous.resize(bytes);
            m_older.resize(bytes);
            m_motion.resize(static_cast<size_t>(width) * height);
            reset();
        }

        m_decision = {};
        if (m_hasPrevious)
        {
            m_lower = currentIsLower(current, width, height);
            m_decision.measured = true;
            m_decision.lower = m_lower;
        }
        const bool lower = m_lower;

        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = current + y * stride;
            const uint8_t *older = m_older.data() + y * stride;
            uint8_t *motion = m_motion.data() + static_cast<size_t>(y) * width;
            for (uint32_t x = 0; x < width; ++x)
                motion[x] = m_hasOlder ? channelDifference(row + x * 4u, older + x * 4u) : 255u;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            std::memcpy(output + (y * 2u + (lower ? 1u : 0u)) * outputStride, current + y * stride, stride);

            const uint32_t above = lower ? (y > 0u ? y - 1u : 0u) : y;
            const uint32_t below = lower ? y : std::min(y + 1u, height - 1u);
            const uint8_t *rowAbove = current + above * stride;
            const uint8_t *rowBelow = current + below * stride;
            const uint8_t *motionAbove = m_motion.data() + static_cast<size_t>(above) * width;
            const uint8_t *motionBelow = m_motion.data() + static_cast<size_t>(below) * width;
            const uint8_t *previous = m_previous.data() + y * stride;
            uint8_t *target = output + (y * 2u + (lower ? 0u : 1u)) * outputStride;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t left = x > 0u ? x - 1u : 0u;
                const uint32_t right = std::min(x + 1u, width - 1u);
                const uint8_t motion = std::max({motionAbove[left], motionAbove[x], motionAbove[right],
                                                 motionBelow[left], motionBelow[x], motionBelow[right]});
                uint8_t *pixel = target + x * 4u;
                if (m_hasPrevious && motion <= kStillThreshold)
                {
                    std::memcpy(pixel, previous + x * 4u, 4u);
                    continue;
                }
                const uint8_t *a = rowAbove + x * 4u;
                const uint8_t *b = rowBelow + x * 4u;
                for (int channel = 0; channel < 4; ++channel)
                    pixel[channel] = static_cast<uint8_t>((a[channel] + b[channel] + 1) / 2);
            }
        }

        m_older.swap(m_previous);
        std::memcpy(m_previous.data(), current, bytes);
        m_hasOlder = m_hasPrevious;
        m_hasPrevious = true;
    }

private:
    static constexpr uint8_t kStillThreshold = 16u;

    static uint8_t channelDifference(const uint8_t *a, const uint8_t *b)
    {
        const int red = std::abs(static_cast<int>(a[0]) - b[0]);
        const int green = std::abs(static_cast<int>(a[1]) - b[1]);
        const int blue = std::abs(static_cast<int>(a[2]) - b[2]);
        return static_cast<uint8_t>(std::max({red, green, blue}));
    }

    // Which way a picture is offset can't be read from the display registers, so measure it:
    // a picture half a line below the last one sits between that one's rows y and y + 1.
    // A tie, such as two identical pictures, flips the last answer as the fields would.
    bool currentIsLower(const uint8_t *current, uint32_t width, uint32_t height)
    {
        const size_t stride = static_cast<size_t>(width) * 4u;
        int64_t lowerError = 0;
        int64_t upperError = 0;
        for (uint32_t y = 0; y + 1u < height; ++y)
        {
            const uint8_t *row = current + y * stride;
            const uint8_t *nextRow = row + stride;
            const uint8_t *previous = m_previous.data() + y * stride;
            const uint8_t *nextPrevious = previous + stride;
            for (uint32_t x = 0; x < width; x += 4u)
            {
                const size_t i = static_cast<size_t>(x) * 4u + 1u; // green
                const int between = previous[i] + nextPrevious[i];
                lowerError += std::abs(2 * row[i] - between);
                upperError += std::abs(2 * nextRow[i] - between);
            }
        }
        m_decision.lowerError = lowerError;
        m_decision.upperError = upperError;
        if (lowerError == upperError)
            return !m_lower;
        return lowerError < upperError;
    }

    std::vector<uint8_t> m_previous;
    std::vector<uint8_t> m_older;
    std::vector<uint8_t> m_motion;
    bool m_hasPrevious = false;
    bool m_hasOlder = false;
    bool m_lower = false;
    Decision m_decision;
};
