#include "runtime/gs/ps2_gs_transfer.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <cstring>

// Moved from gs_cpu_backend.cpp (UploadImageUnlocked, PerformLocalToLocalTransfer,
// PerformLocalToHostTransfer, and the per-format read/write tables); the code is the same.

namespace GSTransfer
{
    using namespace GSMem;

    uint32_t ReadPixel(uint8_t *vram, uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
    {
        switch (psm & 0x3Fu)
        {
        case GS_PSM_CT32: return ReadCT32(vram, base, bw, x, y);
        case GS_PSM_CT24: return ReadCT24(vram, base, bw, x, y);
        case GS_PSM_CT16: return ReadCT16(vram, base, bw, x, y);
        case GS_PSM_CT16S: return ReadCT16S(vram, base, bw, x, y);
        case GS_PSM_T8: return ReadP8(vram, base, bw, x, y);
        case GS_PSM_T8H: return ReadP8H(vram, base, bw, x, y);
        case GS_PSM_T4: return ReadP4(vram, base, bw, x, y);
        case GS_PSM_T4HH: return ReadP4HH(vram, base, bw, x, y);
        case GS_PSM_T4HL: return ReadP4HL(vram, base, bw, x, y);
        case GS_PSM_Z32: return ReadZ32(vram, base, bw, x, y);
        case GS_PSM_Z24: return ReadZ24(vram, base, bw, x, y);
        case GS_PSM_Z16: return ReadZ16(vram, base, bw, x, y);
        case GS_PSM_Z16S: return ReadZ16S(vram, base, bw, x, y);
        default: return ReadNull(vram, base, bw, x, y);
        }
    }

    void WritePixel(uint8_t *vram, uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
    {
        switch (psm & 0x3Fu)
        {
        case GS_PSM_CT32: WriteCT32(vram, base, bw, x, y, value); break;
        case GS_PSM_CT24: WriteCT24(vram, base, bw, x, y, value); break;
        case GS_PSM_CT16: WriteCT16(vram, base, bw, x, y, value); break;
        case GS_PSM_CT16S: WriteCT16S(vram, base, bw, x, y, value); break;
        case GS_PSM_T8: WriteP8(vram, base, bw, x, y, value); break;
        case GS_PSM_T8H: WriteP8H(vram, base, bw, x, y, value); break;
        case GS_PSM_T4: WriteP4(vram, base, bw, x, y, value); break;
        case GS_PSM_T4HH: WriteP4HH(vram, base, bw, x, y, value); break;
        case GS_PSM_T4HL: WriteP4HL(vram, base, bw, x, y, value); break;
        case GS_PSM_Z32: WriteZ32(vram, base, bw, x, y, value); break;
        case GS_PSM_Z24: WriteZ24(vram, base, bw, x, y, value); break;
        case GS_PSM_Z16: WriteZ16(vram, base, bw, x, y, value); break;
        case GS_PSM_Z16S: WriteZ16S(vram, base, bw, x, y, value); break;
        default: WriteNull(vram, base, bw, x, y, value); break;
        }
    }

    void Begin(const GSTransferCommand &command, GSTransferSnapshot &state)
    {
        state.x = command.trxpos.dsax;
        state.y = command.trxpos.dsay;
        state.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * static_cast<uint32_t>(command.trxreg.rrh);
        state.copiedPixels = 0u;
        state.direction = command.direction;
        state.localToHostPendingBytes = 0u;
    }

    void UploadImage(uint8_t *vram, const GSTransferCommand &command, GSTransferSnapshot &state,
                     const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes == 0u || !vram || state.direction != 0u)
            return;
        if (command.trxreg.rrw == 0u || command.trxreg.rrh == 0u || state.totalPixels == 0u)
            return;

        const uint32_t dbp = command.bitbltbuf.dbp;
        const uint32_t dbw = std::max<uint32_t>(command.bitbltbuf.dbw, 1u);
        const uint8_t dpsm = command.bitbltbuf.dpsm;
        const uint32_t rrw = command.trxreg.rrw;
        const uint32_t dsax = command.trxpos.dsax;
        uint32_t offset = 0u;

        auto advancePixel = [&](uint32_t count)
        {
            const uint32_t totalPixels = state.totalPixels;
            state.copiedPixels = std::min<uint32_t>(totalPixels, state.copiedPixels + count);

            if (state.copiedPixels >= totalPixels)
            {
                state.direction = 3u;
                state.totalPixels = 0u;
                return;
            }

            state.x = dsax + (state.copiedPixels % rrw);
            state.y = command.trxpos.dsay + (state.copiedPixels / rrw);
        };

        // Fast path: write the rest of the current row (or as much of it as the data covers) in one
        // span. 4-bit formats need the span to start on a byte, i.e. an even pixel count so far in a
        // transfer whose rows hold whole bytes; anything else falls through to the per-pixel code.
        uint32_t bytesPerPixel = 0u;
        switch (dpsm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
            bytesPerPixel = 4u;
            break;
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            bytesPerPixel = 3u;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            bytesPerPixel = 2u;
            break;
        case GS_PSM_T8:
        case GS_PSM_T8H:
            bytesPerPixel = 1u;
            break;
        default:
            break;
        }
        const bool fourBit = dpsm == GS_PSM_T4 || dpsm == GS_PSM_T4HL || dpsm == GS_PSM_T4HH;

        while (offset < sizeBytes && state.direction == 0u)
        {
            const uint32_t copied = state.copiedPixels;
            const uint32_t column = copied % rrw;
            const uint32_t rowLeft = std::min(rrw - column, state.totalPixels - copied);
            uint32_t count = 0u;
            uint32_t bytes = 0u;
            if (bytesPerPixel != 0u)
            {
                count = std::min(rowLeft, (sizeBytes - offset) / bytesPerPixel);
                bytes = count * bytesPerPixel;
            }
            else if (fourBit && (rrw & 1u) == 0u && (copied & 1u) == 0u)
            {
                count = std::min(rowLeft, (sizeBytes - offset) * 2u);
                bytes = (count + 1u) / 2u;
            }
            if (count != 0u &&
                GSMem::WriteSpan(vram, dpsm, dbp, dbw, dsax + column, command.trxpos.dsay + copied / rrw, data + offset, count))
            {
                offset += bytes;
                advancePixel(count);
                continue;
            }

            switch (dpsm)
            {
            case GS_PSM_CT32:
            case GS_PSM_Z32:
            {
                if (sizeBytes - offset < 4u)
                    return;
                uint32_t value = 0u;
                std::memcpy(&value, data + offset, sizeof(value));
                WritePixel(vram, dpsm, dbp, dbw, state.x, state.y, value);
                offset += 4u;
                advancePixel(1u);
                break;
            }
            case GS_PSM_CT24:
            case GS_PSM_Z24:
            {
                if (sizeBytes - offset < 3u)
                    return;
                const uint32_t value = static_cast<uint32_t>(data[offset]) |
                                       (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
                                       (static_cast<uint32_t>(data[offset + 2u]) << 16u);
                WritePixel(vram, dpsm, dbp, dbw, state.x, state.y, value);
                offset += 3u;
                advancePixel(1u);
                break;
            }
            case GS_PSM_CT16:
            case GS_PSM_CT16S:
            case GS_PSM_Z16:
            case GS_PSM_Z16S:
            {
                if (sizeBytes - offset < 2u)
                    return;
                uint16_t value = 0u;
                std::memcpy(&value, data + offset, sizeof(value));
                WritePixel(vram, dpsm, dbp, dbw, state.x, state.y, value);
                offset += 2u;
                advancePixel(1u);
                break;
            }
            case GS_PSM_T8:
            case GS_PSM_T8H:
                WritePixel(vram, dpsm, dbp, dbw, state.x, state.y, data[offset++]);
                advancePixel(1u);
                break;
            case GS_PSM_T4:
            case GS_PSM_T4HL:
            case GS_PSM_T4HH:
            {
                const uint8_t packed = data[offset++];
                const uint32_t firstPixel = state.copiedPixels;
                WritePixel(vram, dpsm, dbp, dbw,
                           dsax + (firstPixel % rrw),
                           command.trxpos.dsay + (firstPixel / rrw),
                           packed & 0x0Fu);
                if (firstPixel + 1u < state.totalPixels)
                {
                    const uint32_t secondPixel = firstPixel + 1u;
                    WritePixel(vram, dpsm, dbp, dbw,
                               dsax + (secondPixel % rrw),
                               command.trxpos.dsay + (secondPixel / rrw),
                               (packed >> 4u) & 0x0Fu);
                }
                advancePixel(std::min<uint32_t>(2u, state.totalPixels - firstPixel));
                break;
            }
            default:
                return;
            }
        }
    }

    void LocalToLocal(uint8_t *vram, const GSTransferCommand &command, GSTransferSnapshot &state)
    {
        if (!vram)
            return;

        const uint32_t rrw = command.trxreg.rrw;
        const uint32_t rrh = command.trxreg.rrh;
        const uint32_t total = rrw * rrh;
        if (total == 0u)
        {
            state.direction = 3u;
            return;
        }

        for (uint32_t pixel = 0; pixel < total; ++pixel)
        {
            uint32_t x = pixel % rrw;
            uint32_t y = pixel / rrw;
            if ((command.trxpos.dir & 0x2u) != 0u)
                x = rrw - x - 1u;
            if ((command.trxpos.dir & 0x1u) != 0u)
                y = rrh - y - 1u;

            const uint32_t value = ReadPixel(vram, command.bitbltbuf.spsm,
                                             command.bitbltbuf.sbp,
                                             std::max<uint32_t>(command.bitbltbuf.sbw, 1u),
                                             x + command.trxpos.ssax,
                                             y + command.trxpos.ssay);
            WritePixel(vram, command.bitbltbuf.dpsm,
                       command.bitbltbuf.dbp,
                       std::max<uint32_t>(command.bitbltbuf.dbw, 1u),
                       x + command.trxpos.dsax,
                       y + command.trxpos.dsay,
                       value);
        }

        state.copiedPixels = total;
        state.direction = 3u;
    }

    void LocalToHost(uint8_t *vram, const GSTransferCommand &command, GSTransferSnapshot &state,
                     std::vector<uint8_t> &buffer, size_t &readPos)
    {
        buffer.clear();
        readPos = 0u;
        if (!vram)
            return;

        const uint32_t rrw = command.trxreg.rrw;
        const uint32_t rrh = command.trxreg.rrh;
        const uint32_t sbw = std::max<uint32_t>(command.bitbltbuf.sbw, 1u);
        const uint8_t spsm = command.bitbltbuf.spsm;
        const uint32_t bpp = static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm)));
        const uint32_t total = rrw * rrh;
        buffer.reserve((static_cast<size_t>(total) * bpp + 7u) / 8u);

        for (uint32_t pixel = 0u; pixel < total; ++pixel)
        {
            const uint32_t x = pixel % rrw;
            const uint32_t y = pixel / rrw;
            const uint32_t value = ReadPixel(vram, spsm,
                                             command.bitbltbuf.sbp,
                                             sbw,
                                             x + command.trxpos.ssax,
                                             y + command.trxpos.ssay);
            switch (bpp)
            {
            case 32:
                buffer.push_back(static_cast<uint8_t>(value));
                buffer.push_back(static_cast<uint8_t>(value >> 8u));
                buffer.push_back(static_cast<uint8_t>(value >> 16u));
                buffer.push_back(static_cast<uint8_t>(value >> 24u));
                break;
            case 24:
                buffer.push_back(static_cast<uint8_t>(value));
                buffer.push_back(static_cast<uint8_t>(value >> 8u));
                buffer.push_back(static_cast<uint8_t>(value >> 16u));
                break;
            case 16:
                buffer.push_back(static_cast<uint8_t>(value));
                buffer.push_back(static_cast<uint8_t>(value >> 8u));
                break;
            case 8:
                buffer.push_back(static_cast<uint8_t>(value));
                break;
            case 4:
            {
                if ((pixel & 1u) != 0u)
                    break;
                uint32_t next = 0u;
                if (pixel + 1u < total)
                {
                    const uint32_t nextPixel = pixel + 1u;
                    const uint32_t nextX = nextPixel % rrw;
                    const uint32_t nextY = nextPixel / rrw;
                    next = ReadPixel(vram, spsm, command.bitbltbuf.sbp, sbw,
                                     nextX + command.trxpos.ssax,
                                     nextY + command.trxpos.ssay);
                }
                buffer.push_back(static_cast<uint8_t>((value & 0x0Fu) | ((next & 0x0Fu) << 4u)));
                break;
            }
            default:
                break;
            }
        }

        state.copiedPixels = total;
        state.localToHostPendingBytes = buffer.size();
    }
}
