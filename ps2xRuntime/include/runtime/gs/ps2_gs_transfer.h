#pragma once

// FPWR: GS transfers (BITBLTBUF/TRXPOS/TRXREG/TRXDIR) on a plain 4 MiB GS memory image, shared by the
// two GS renderers (GSCpuBackend and GSGpuBackend). Moved out of gs_cpu_backend.cpp so there is one
// copy of the upload swizzle: host to local (UploadImage), local to local and local to host.
// The functions only touch the memory image and the GSTransferSnapshot they are given; the
// backends keep the transfer command, the snapshot and the local-to-host buffer themselves.
// No header that generated game code includes refers to this one.

#include "runtime/gs/gs_types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GSTransfer
{
    // One pixel of GS memory in pixel storage mode `psm` (a GSPsm value). An unknown mode reads 0 and
    // writes nothing. `base` is a block pointer (256 bytes per block, 32 blocks per page).
    uint32_t ReadPixel(uint8_t *vram, uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
    void WritePixel(uint8_t *vram, uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    // Starts a transfer: fills in the snapshot the way BeginTransfer always has (position, total
    // pixels, direction; nothing copied yet).
    void Begin(const GSTransferCommand &command, GSTransferSnapshot &state);

    // Direction 0: writes the next `size` bytes of a host to local transfer, advancing `state`.
    void UploadImage(uint8_t *vram, const GSTransferCommand &command, GSTransferSnapshot &state,
                     const uint8_t *data, uint32_t size);

    // Direction 2: the whole copy, at once.
    void LocalToLocal(uint8_t *vram, const GSTransferCommand &command, GSTransferSnapshot &state);

    // Direction 1: reads the whole region into `buffer` (cleared first) and `readPos` back to 0.
    void LocalToHost(uint8_t *vram, const GSTransferCommand &command, GSTransferSnapshot &state,
                     std::vector<uint8_t> &buffer, size_t &readPos);
}
