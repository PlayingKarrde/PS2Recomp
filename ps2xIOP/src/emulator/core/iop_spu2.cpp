#include "iop_spu2.h"

#include <algorithm>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamWords = 0x100000u;
        constexpr uint32_t kRamMask = kRamWords - 1u;
        constexpr int32_t kMaxEnvelope = 0x7FFF;

        // Register offsets within a core. The second core is at +0x400.
        constexpr uint32_t kVoiceRegsEnd = 0x180u;
        constexpr uint32_t kPmon = 0x180u;
        constexpr uint32_t kNon = 0x184u;
        constexpr uint32_t kVmixL = 0x188u;
        constexpr uint32_t kVmixEL = 0x18Cu;
        constexpr uint32_t kVmixR = 0x190u;
        constexpr uint32_t kVmixER = 0x194u;
        constexpr uint32_t kMmix = 0x198u;
        constexpr uint32_t kAttr = 0x19Au;
        constexpr uint32_t kIrqa = 0x19Cu;
        constexpr uint32_t kKon = 0x1A0u;
        constexpr uint32_t kKoff = 0x1A4u;
        constexpr uint32_t kTsa = 0x1A8u;
        constexpr uint32_t kDataPort = 0x1ACu;
        constexpr uint32_t kAdmas = 0x1B0u;
        constexpr uint32_t kVoiceAddrBase = 0x1C0u;
        constexpr uint32_t kVoiceAddrEnd = 0x2E0u;
        constexpr uint32_t kEndx = 0x340u;
        constexpr uint32_t kStatx = 0x344u;
        constexpr uint32_t kMasterBase = 0x760u;
        constexpr uint32_t kMasterStride = 0x28u;
        constexpr uint32_t kIrqInfo = 0x7C2u;
        constexpr uint16_t kAttrIrqEnable = 0x40u;

        constexpr std::array<std::array<int32_t, 2>, 5> kAdpcmFilters{{{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}}};

        int32_t clampS16(int32_t value)
        {
            return std::clamp<int32_t>(value, -32768, 32767);
        }

        // A volume register either holds a fixed level (bit 15 clear) or starts a sweep.
        // Sweeps are not stepped: the sweep's direction picks the end it is heading for.
        int32_t volumeLevel(uint16_t reg, int32_t current)
        {
            if ((reg & 0x8000u) == 0u)
                return static_cast<int16_t>(reg << 1u);
            const bool decrease = (reg & 0x2000u) != 0u;
            const bool negativePhase = (reg & 0x1000u) != 0u;
            if (decrease)
                return 0;
            if (current != 0)
                return current;
            return negativePhase ? -32768 : 32767;
        }

        void setLow16(uint32_t &value, uint16_t low)
        {
            value = (value & 0xFF0000u) | low;
        }

        void setHigh8(uint32_t &value, uint16_t high)
        {
            value = (value & 0x00FFFFu) | ((static_cast<uint32_t>(high) & 0xFFu) << 16u);
        }

        // Shared step for every envelope phase: rate packs a shift and a step, as on the PS1 SPU.
        void stepEnvelope(int32_t &level, uint32_t &counter, uint32_t rate, bool exponential, bool decrease)
        {
            const uint32_t shift = rate >> 2u;
            int32_t step = decrease ? -8 + static_cast<int32_t>(rate & 3u) : 7 - static_cast<int32_t>(rate & 3u);
            uint32_t ticks = 1u << (shift > 11u ? shift - 11u : 0u);
            if (shift < 11u)
                step <<= (11u - shift);
            if (exponential && !decrease && level > 0x6000)
                ticks *= 4u;
            if (exponential && decrease)
                step = (step * level) / 0x8000;
            if (++counter >= ticks)
            {
                counter = 0u;
                level = std::clamp(level + step, 0, kMaxEnvelope);
            }
        }
    }

    IopSpu2::IopSpu2()
        : m_ram(kRamWords)
    {
    }

    void IopSpu2::reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        m_cores = {};
        m_raw = {};
        m_irqInfo = 0u;
    }

    uint16_t IopSpu2::readRegister(uint32_t offset)
    {
        offset &= 0x7FFu;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (offset == kIrqInfo)
        {
            const uint16_t info = m_irqInfo;
            m_irqInfo = 0u;
            return info;
        }
        if (offset < 0x760u)
        {
            const int coreIndex = offset >= 0x400u ? 1 : 0;
            const uint32_t reg = offset - (coreIndex ? 0x400u : 0u);
            Core &core = m_cores[coreIndex];
            if (reg < kVoiceRegsEnd)
            {
                const Voice &voice = core.voices[reg >> 4u];
                switch (reg & 0xFu)
                {
                case 0xAu:
                    return static_cast<uint16_t>(voice.env.level);
                case 0xCu:
                    return static_cast<uint16_t>(voice.volXL >> 1);
                case 0xEu:
                    return static_cast<uint16_t>(voice.volXR >> 1);
                default:
                    break;
                }
            }
            else if (reg >= kVoiceAddrBase && reg < kVoiceAddrEnd)
            {
                const uint32_t rel = reg - kVoiceAddrBase;
                const Voice &voice = core.voices[rel / 12u];
                const uint32_t field = rel % 12u;
                uint32_t address = field < 4u ? voice.ssa : field < 8u ? voice.lsa : voice.nax;
                // NAX is the running read position, so report progress through the block.
                if (field >= 8u && voice.blockLoaded)
                    address = (voice.nax - 8u + 1u + (voice.samplePos * 7u) / 28u) & kRamMask;
                return static_cast<uint16_t>((field & 2u) ? address : address >> 16u);
            }
            switch (reg)
            {
            case kEndx:
                return static_cast<uint16_t>(core.endx);
            case kEndx + 2u:
                return static_cast<uint16_t>(core.endx >> 16u);
            case kStatx:
                return core.statx;
            case kAttr:
                return core.attr;
            case kTsa:
                return static_cast<uint16_t>(core.tsa >> 16u);
            case kTsa + 2u:
                return static_cast<uint16_t>(core.tsa);
            case kDataPort:
            {
                const uint16_t value = m_ram[core.tsa & kRamMask];
                core.tsa = (core.tsa + 1u) & kRamMask;
                return value;
            }
            default:
                break;
            }
        }
        return m_raw[offset >> 1u];
    }

    void IopSpu2::writePort(Core &core, uint16_t value)
    {
        m_ram[core.tsa & kRamMask] = value;
        core.tsa = (core.tsa + 1u) & kRamMask;
    }

    void IopSpu2::writeRegister(uint32_t offset, uint16_t value)
    {
        offset &= 0x7FFu;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_raw[offset >> 1u] = value;

        if (offset >= kMasterBase && offset < kMasterBase + 2u * kMasterStride)
        {
            const int coreIndex = offset >= kMasterBase + kMasterStride ? 1 : 0;
            Core &core = m_cores[coreIndex];
            switch (offset - kMasterBase - coreIndex * kMasterStride)
            {
            case 0x0u:
                core.mvolL = value;
                break;
            case 0x2u:
                core.mvolR = value;
                break;
            case 0x8u:
                core.avolL = value;
                break;
            case 0xAu:
                core.avolR = value;
                break;
            case 0xCu:
                core.bvolL = value;
                break;
            case 0xEu:
                core.bvolR = value;
                break;
            default:
                break;
            }
            return;
        }
        if (offset >= 0x760u)
            return;

        const int coreIndex = offset >= 0x400u ? 1 : 0;
        const uint32_t reg = offset - (coreIndex ? 0x400u : 0u);
        Core &core = m_cores[coreIndex];

        if (reg < kVoiceRegsEnd)
        {
            Voice &voice = core.voices[reg >> 4u];
            switch (reg & 0xFu)
            {
            case 0x0u:
                voice.volL = value;
                voice.volXL = volumeLevel(value, voice.volXL);
                break;
            case 0x2u:
                voice.volR = value;
                voice.volXR = volumeLevel(value, voice.volXR);
                break;
            case 0x4u:
                voice.pitch = value;
                break;
            case 0x6u:
                voice.adsr1 = value;
                break;
            case 0x8u:
                voice.adsr2 = value;
                break;
            default:
                break;
            }
            return;
        }
        if (reg >= kVoiceAddrBase && reg < kVoiceAddrEnd)
        {
            const uint32_t rel = reg - kVoiceAddrBase;
            Voice &voice = core.voices[rel / 12u];
            const uint32_t field = rel % 12u;
            uint32_t &address = field < 4u ? voice.ssa : field < 8u ? voice.lsa : voice.nax;
            if (field & 2u)
                setLow16(address, value);
            else
                setHigh8(address, value);
            return;
        }

        const auto writeLowHigh = [&](uint32_t &mask, uint32_t base)
        {
            if (reg == base)
                setLow16(mask, value);
            else
                setHigh8(mask, value);
        };

        switch (reg)
        {
        case kPmon:
        case kPmon + 2u:
            writeLowHigh(core.pmon, kPmon);
            break;
        case kNon:
        case kNon + 2u:
            writeLowHigh(core.non, kNon);
            break;
        case kVmixL:
        case kVmixL + 2u:
            writeLowHigh(core.vmixL, kVmixL);
            break;
        case kVmixEL:
        case kVmixEL + 2u:
            writeLowHigh(core.vmixEL, kVmixEL);
            break;
        case kVmixR:
        case kVmixR + 2u:
            writeLowHigh(core.vmixR, kVmixR);
            break;
        case kVmixER:
        case kVmixER + 2u:
            writeLowHigh(core.vmixER, kVmixER);
            break;
        case kMmix:
            core.mmix = value;
            break;
        case kAttr:
            core.attr = value;
            break;
        case kIrqa:
            setHigh8(core.irqa, value);
            break;
        case kIrqa + 2u:
            setLow16(core.irqa, value);
            break;
        case kKon:
            for (int i = 0; i < 16; ++i)
                if (value & (1u << i))
                    keyOn(core, i);
            break;
        case kKon + 2u:
            for (int i = 0; i < 8; ++i)
                if (value & (1u << i))
                    keyOn(core, 16 + i);
            break;
        case kKoff:
        case kKoff + 2u:
        {
            const int first = reg == kKoff ? 0 : 16;
            const int count = reg == kKoff ? 16 : 8;
            for (int i = 0; i < count; ++i)
            {
                Envelope &env = core.voices[first + i].env;
                if ((value & (1u << i)) && env.phase != Envelope::Phase::Off)
                {
                    env.phase = Envelope::Phase::Release;
                    env.counter = 0u;
                }
            }
            break;
        }
        case kTsa:
            setHigh8(core.tsa, value);
            break;
        case kTsa + 2u:
            setLow16(core.tsa, value);
            break;
        case kDataPort:
            writePort(core, value);
            break;
        case kAdmas:
            core.admas = value;
            if ((value & (1u << coreIndex)) == 0u)
            {
                core.admaL.clear();
                core.admaR.clear();
                core.admaWaiting = false;
            }
            break;
        case kEndx:
        case kEndx + 2u:
            // Any write clears the end flags.
            core.endx = 0u;
            break;
        case kStatx:
            core.statx = value;
            break;
        default:
            break;
        }
    }

    bool IopSpu2::startDma(int coreIndex, std::vector<uint8_t> &ram, uint32_t address, uint32_t bytes, bool toSpu)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Core &core = m_cores[coreIndex];
        core.statx |= 0x80u;
        address %= static_cast<uint32_t>(ram.size());
        bytes = std::min<uint32_t>(bytes, static_cast<uint32_t>(ram.size()) - address);

        if (toSpu && (core.admas & (1u << coreIndex)) != 0u)
        {
            // Auto-DMA data alternates 512 bytes of left samples with 512 bytes of right samples.
            constexpr uint32_t kBlockBytes = 0x200u;
            const uint32_t blocks = bytes / (2u * kBlockBytes);
            const auto *samples = reinterpret_cast<const int16_t *>(ram.data() + address);
            for (uint32_t block = 0; block < blocks; ++block)
            {
                const int16_t *left = samples + block * (kBlockBytes / sizeof(int16_t)) * 2u;
                const int16_t *right = left + kBlockBytes / sizeof(int16_t);
                core.admaL.insert(core.admaL.end(), left, left + kBlockBytes / sizeof(int16_t));
                core.admaR.insert(core.admaR.end(), right, right + kBlockBytes / sizeof(int16_t));
            }
            core.admaChunkFrames = blocks * (kBlockBytes / sizeof(int16_t));
            core.admaWaiting = true;
            return true;
        }

        const uint32_t words = bytes / sizeof(uint16_t);
        const uint32_t tsa = core.tsa & kRamMask;
        if (toSpu)
        {
            const auto *source = reinterpret_cast<const uint16_t *>(ram.data() + address);
            for (uint32_t i = 0; i < words; ++i)
                m_ram[(tsa + i) & kRamMask] = source[i];
        }
        else
        {
            auto *destination = reinterpret_cast<uint16_t *>(ram.data() + address);
            for (uint32_t i = 0; i < words; ++i)
                destination[i] = m_ram[(tsa + i) & kRamMask];
        }
        core.tsa = (tsa + words) & kRamMask;
        return false;
    }

    bool IopSpu2::takeAdmaComplete(int coreIndex)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Core &core = m_cores[coreIndex];
        const bool complete = core.admaComplete;
        core.admaComplete = false;
        return complete;
    }

    bool IopSpu2::takeIrq()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_irqInfo != 0u;
    }

    void IopSpu2::keyOn(Core &core, int voiceIndex)
    {
        Voice &voice = core.voices[voiceIndex];
        voice.env = {};
        voice.env.phase = Envelope::Phase::Attack;
        voice.nax = voice.ssa & kRamMask & ~7u;
        voice.blockLoaded = false;
        voice.samplePos = 0u;
        voice.pitchCounter = 0u;
        voice.hist1 = 0;
        voice.hist2 = 0;
        voice.interp = {};
        core.endx &= ~(1u << voiceIndex);
    }

    void IopSpu2::decodeBlock(Core &core, int coreIndex, int voiceIndex)
    {
        Voice &voice = core.voices[voiceIndex];

        // Finish the previous block: its flags say where to go next.
        if (voice.blockLoaded)
        {
            if (voice.blockFlags & 1u)
            {
                core.endx |= 1u << voiceIndex;
                if (voice.blockFlags & 2u)
                {
                    voice.nax = voice.lsa & kRamMask & ~7u;
                }
                else
                {
                    voice.env.phase = Envelope::Phase::Off;
                    voice.env.level = 0;
                }
            }
        }

        const uint32_t address = voice.nax & kRamMask;
        const uint16_t header = m_ram[address];
        const uint32_t filter = std::min<uint32_t>((header >> 4u) & 0xFu, 4u);
        const uint32_t shift = header & 0xFu;
        voice.blockFlags = static_cast<uint8_t>(header >> 8u);
        if (voice.blockFlags & 4u)
            voice.lsa = address;

        const int32_t f0 = kAdpcmFilters[filter][0];
        const int32_t f1 = kAdpcmFilters[filter][1];
        for (uint32_t i = 0; i < 28u; ++i)
        {
            const uint16_t word = m_ram[(address + 1u + i / 4u) & kRamMask];
            const int32_t nibble = (word >> ((i & 3u) * 4u)) & 0xFu;
            int32_t sample = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12u));
            sample >>= shift > 12u ? 12u : shift;
            sample += (voice.hist1 * f0 + voice.hist2 * f1) / 64;
            sample = clampS16(sample);
            voice.block[i] = static_cast<int16_t>(sample);
            voice.hist2 = voice.hist1;
            voice.hist1 = sample;
        }

        // The IRQ address fires when the chip reads that word of sound RAM.
        for (int c = 0; c < 2; ++c)
        {
            const Core &irqCore = m_cores[c];
            if ((irqCore.attr & kAttrIrqEnable) == 0u)
                continue;
            const uint32_t irqa = irqCore.irqa & kRamMask;
            if (irqa >= address && irqa < address + 8u)
                m_irqInfo |= static_cast<uint16_t>(4u << c);
        }

        voice.nax = (address + 8u) & kRamMask;
        voice.blockLoaded = true;
        voice.samplePos = 0u;
        (void)coreIndex;
    }

    int32_t IopSpu2::nextSample(Core &core, int coreIndex, int voiceIndex)
    {
        Voice &voice = core.voices[voiceIndex];

        uint32_t pitch = voice.pitch;
        if (voiceIndex > 0 && (core.pmon & (1u << voiceIndex)) != 0u)
        {
            const int32_t modulator = core.voices[voiceIndex - 1].lastOutput;
            pitch = static_cast<uint32_t>(std::max<int32_t>((static_cast<int32_t>(pitch) * (modulator + 32768)) >> 15, 0));
        }
        pitch = std::min<uint32_t>(pitch, 0x3FFFu);

        voice.pitchCounter += pitch;
        while (voice.pitchCounter >= 0x1000u)
        {
            voice.pitchCounter -= 0x1000u;
            if (!voice.blockLoaded || voice.samplePos >= 28u)
            {
                decodeBlock(core, coreIndex, voiceIndex);
                if (voice.env.phase == Envelope::Phase::Off)
                    return 0;
            }
            voice.interp[0] = voice.interp[1];
            voice.interp[1] = voice.interp[2];
            voice.interp[2] = voice.interp[3];
            voice.interp[3] = voice.block[voice.samplePos++];
        }

        // Catmull-Rom over the last four decoded samples, in 12-bit fixed point.
        const int32_t t = static_cast<int32_t>(voice.pitchCounter);
        const int32_t p0 = voice.interp[0], p1 = voice.interp[1], p2 = voice.interp[2], p3 = voice.interp[3];
        const int64_t a = -p0 + 3 * p1 - 3 * p2 + p3;
        const int64_t b = 2 * p0 - 5 * p1 + 4 * p2 - p3;
        const int64_t c = p2 - p0;
        int64_t value = (a * t) >> 12;
        value = ((value + b) * t) >> 12;
        value = ((value + c) * t) >> 12;
        value = (value >> 1) + p1;
        return clampS16(static_cast<int32_t>(value));
    }

    void IopSpu2::advanceEnvelope(Voice &voice)
    {
        Envelope &env = voice.env;
        switch (env.phase)
        {
        case Envelope::Phase::Off:
            break;
        case Envelope::Phase::Attack:
            stepEnvelope(env.level, env.counter, (voice.adsr1 >> 8u) & 0x7Fu, (voice.adsr1 & 0x8000u) != 0u, false);
            if (env.level >= kMaxEnvelope)
            {
                env.phase = Envelope::Phase::Decay;
                env.counter = 0u;
            }
            break;
        case Envelope::Phase::Decay:
        {
            stepEnvelope(env.level, env.counter, ((voice.adsr1 >> 4u) & 0xFu) << 2u, true, true);
            const int32_t sustainLevel = ((voice.adsr1 & 0xFu) + 1) * 0x800;
            if (env.level <= sustainLevel)
            {
                env.phase = Envelope::Phase::Sustain;
                env.counter = 0u;
            }
            break;
        }
        case Envelope::Phase::Sustain:
            stepEnvelope(env.level, env.counter, (voice.adsr2 >> 6u) & 0x7Fu,
                         (voice.adsr2 & 0x8000u) != 0u, (voice.adsr2 & 0x4000u) != 0u);
            break;
        case Envelope::Phase::Release:
            stepEnvelope(env.level, env.counter, (voice.adsr2 & 0x1Fu) << 2u, (voice.adsr2 & 0x20u) != 0u, true);
            if (env.level <= 0)
            {
                env.level = 0;
                env.phase = Envelope::Phase::Off;
            }
            break;
        }
    }

    void IopSpu2::admaFrame(Core &core, int32_t &left, int32_t &right)
    {
        if (core.admaL.empty())
        {
            left = 0;
            right = 0;
            return;
        }
        left = core.admaL.front();
        right = core.admaR.front();
        core.admaL.pop_front();
        core.admaR.pop_front();
        // The driver refills when the transfer it started is reported complete. Reporting that
        // while one chunk is still queued keeps the stream from running dry.
        if (core.admaWaiting && core.admaL.size() <= core.admaChunkFrames)
        {
            core.admaWaiting = false;
            core.admaComplete = true;
        }
    }

    void IopSpu2::mixFrame(int16_t &outL, int16_t &outR)
    {
        int32_t extL = 0, extR = 0;
        for (int coreIndex = 0; coreIndex < 2; ++coreIndex)
        {
            Core &core = m_cores[coreIndex];
            int64_t voiceL = 0, voiceR = 0;
            for (int i = 0; i < 24; ++i)
            {
                Voice &voice = core.voices[i];
                if (voice.env.phase == Envelope::Phase::Off)
                {
                    voice.lastOutput = 0;
                    continue;
                }
                int32_t sample;
                if (core.non & (1u << i))
                {
                    sample = m_noise;
                    (void)nextSample(core, coreIndex, i);
                }
                else
                {
                    sample = nextSample(core, coreIndex, i);
                }
                if (voice.env.phase == Envelope::Phase::Off)
                {
                    voice.lastOutput = 0;
                    continue;
                }
                advanceEnvelope(voice);
                const int32_t enveloped = (sample * voice.env.level) >> 15;
                voice.lastOutput = enveloped;
                if (core.vmixL & (1u << i))
                    voiceL += (enveloped * voice.volXL) >> 15;
                if (core.vmixR & (1u << i))
                    voiceR += (enveloped * voice.volXR) >> 15;
            }

            int32_t inputL = 0, inputR = 0;
            admaFrame(core, inputL, inputR);

            int64_t mixL = 0, mixR = 0;
            // MMIX: bits 2/3 external input (the other core's output), 6/7 auto-DMA, 10/11 voices.
            // AVOL and BVOL are plain signed levels: unlike the voice and master volumes they have no sweep bit.
            if (core.mmix & 0x800u)
                mixL += voiceL;
            if (core.mmix & 0x400u)
                mixR += voiceR;
            if (core.mmix & 0x080u)
                mixL += (inputL * static_cast<int16_t>(core.bvolL)) >> 15;
            if (core.mmix & 0x040u)
                mixR += (inputR * static_cast<int16_t>(core.bvolR)) >> 15;
            if (coreIndex == 1)
            {
                if (core.mmix & 0x008u)
                    mixL += (extL * static_cast<int16_t>(core.avolL)) >> 15;
                if (core.mmix & 0x004u)
                    mixR += (extR * static_cast<int16_t>(core.avolR)) >> 15;
            }

            const int32_t masterL = volumeLevel(core.mvolL, 32767);
            const int32_t masterR = volumeLevel(core.mvolR, 32767);
            extL = clampS16(static_cast<int32_t>((std::clamp<int64_t>(mixL, -32768, 32767) * masterL) >> 15));
            extR = clampS16(static_cast<int32_t>((std::clamp<int64_t>(mixR, -32768, 32767) * masterR) >> 15));
        }

        // Noise generator: a simple LFSR is close enough for the rare voice that uses it.
        if (++m_noiseCounter >= 8u)
        {
            m_noiseCounter = 0u;
            const uint32_t bit = (static_cast<uint32_t>(m_noise) ^ (static_cast<uint32_t>(m_noise) >> 3u)) & 1u;
            m_noise = static_cast<int16_t>((static_cast<uint32_t>(m_noise) >> 1u) | (bit << 15u));
        }

        outL = static_cast<int16_t>(extL);
        outR = static_cast<int16_t>(extR);
    }

    void IopSpu2::mix(int16_t *output, size_t frames)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (size_t i = 0; i < frames; ++i)
            mixFrame(output[i * 2u], output[i * 2u + 1u]);
    }
}
