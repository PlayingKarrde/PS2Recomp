#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace ps2x::iop::detail
{
    // The PS2's sound chip as the IOP sees it: two cores of 24 ADPCM voices each, 2 MB of sound
    // RAM, and an auto-DMA input per core for streamed PCM. Registers are written by the IOP thread
    // and mixed on the host's audio thread, so everything here is guarded by one mutex.
    // Effects (reverb) are not emulated yet: the wet signal is dropped.
    class IopSpu2
    {
    public:
        static constexpr uint32_t OutputRate = 48000u;

        IopSpu2();

        void reset();

        [[nodiscard]] uint16_t readRegister(uint32_t offset);
        void writeRegister(uint32_t offset, uint16_t value);

        // A DMA channel started. Returns true when the data went to the auto-DMA input, in which
        // case completion should wait for takeAdmaComplete() instead of a fixed delay.
        bool startDma(int core, std::vector<uint8_t> &ram, uint32_t address, uint32_t bytes, bool toSpu);
        [[nodiscard]] bool takeAdmaComplete(int core);
        [[nodiscard]] bool takeIrq();

        // Fills interleaved stereo frames.
        void mix(int16_t *output, size_t frames);

    private:
        struct Envelope
        {
            enum class Phase
            {
                Off,
                Attack,
                Decay,
                Sustain,
                Release
            };
            Phase phase = Phase::Off;
            int32_t level = 0;
            uint32_t counter = 0;
        };

        struct Voice
        {
            uint16_t volL = 0, volR = 0;
            int32_t volXL = 0, volXR = 0;
            uint16_t pitch = 0;
            uint16_t adsr1 = 0, adsr2 = 0;
            uint32_t ssa = 0, lsa = 0, nax = 0;
            Envelope env;
            bool blockLoaded = false;
            uint8_t blockFlags = 0;
            uint32_t samplePos = 0; // next decoded sample within the block
            uint32_t pitchCounter = 0;
            std::array<int16_t, 28> block{};
            int32_t hist1 = 0, hist2 = 0;
            std::array<int32_t, 4> interp{};
            int32_t lastOutput = 0;
        };

        struct Core
        {
            std::array<Voice, 24> voices{};
            uint32_t pmon = 0, non = 0, vmixL = 0, vmixR = 0, vmixEL = 0, vmixER = 0;
            uint32_t endx = 0;
            uint32_t irqa = 0, tsa = 0;
            uint16_t mmix = 0, attr = 0, admas = 0, statx = 0;
            uint16_t mvolL = 0, mvolR = 0, avolL = 0, avolR = 0, bvolL = 0, bvolR = 0;
            int32_t mvolXL = 0, mvolXR = 0;
            uint32_t konHi = 0, koffHi = 0;
            std::deque<int16_t> admaL, admaR;
            uint32_t admaChunkFrames = 0;
            bool admaWaiting = false;
            bool admaComplete = false;
            uint16_t admaLastL = 0, admaLastR = 0;
        };

        void keyOn(Core &core, int voiceIndex);
        void decodeBlock(Core &core, int coreIndex, int voiceIndex);
        int32_t nextSample(Core &core, int coreIndex, int voiceIndex);
        void advanceEnvelope(Voice &voice);
        void writePort(Core &core, uint16_t value);
        void mixFrame(int16_t &outL, int16_t &outR);
        void admaFrame(Core &core, int32_t &left, int32_t &right);

        std::mutex m_mutex;
        std::vector<uint16_t> m_ram;
        std::array<Core, 2> m_cores{};
        std::array<uint16_t, 0x800 / 2> m_raw{};
        uint16_t m_irqInfo = 0;
        uint32_t m_noiseCounter = 0;
        int32_t m_noise = 1;
    };
}
