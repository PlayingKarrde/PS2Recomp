#pragma once

// FPWR: the texture key tracker (task t1b, built from the census code of task t1a; docs/mod-layer.md step 2).
//
// What it is for. A texture pack names a game texture by a key, and the key has to be the same every time the texture appears, in
// every run. t1a (bringup.md "t1a") found the stable name: the data the game UPLOADED to build the texture (an image transfer into
// GS memory), not the place in GS memory where it sits or the part of it a scene happens to reach. This file watches the uploads
// and answers, for a texture a draw is about to use ("declared" at TBP, TBW, PSM, width, height):
//   - which transfers wrote its image (the "writers"),
//   - the key k3_i: a hash of what those transfers carried (their bytes, formats and sizes) and where the texture sits in them,
//   - the REPLACED RECTANGLE: the part of the declared texture that those transfers filled. A texture is often declared larger
//     than the image uploaded into it (a 48 x 48 image in a 64 x 64 texture); the rectangle is the same every time the texture
//     appears, so a replacement image has one fixed size, and the dump and the replacement use the same rectangle.
// The tracker is always built; it runs only when something asks for it (a texture pack is on, or PS2X_TEX_DUMP, or the census).
// Nothing here includes SDL, and no header that generated game code includes refers to this one.
//
// ---- How the writers of a texture are found ----
// The tracker keeps, for each of the 16384 blocks (256 bytes) of GS memory, which host-to-local transfer wrote it last (from the
// transfer's geometry: the GS page and block layouts; the census's verify mode, PS2X_TEX_CENSUS_VERIFY=1, checked that against the
// bytes really changed: 0 mismatches over the 9 clips). A local-to-local copy or WriteVram marks its blocks as written by nobody.
//   1. The texture's valid area is its declared size cut to TBW * 64 texels in width (a texture declared 1024 wide at TBW 10 has
//      640 columns of real data; the rest aliases other memory).
//   2. The ANCHOR is the transfer that wrote the block holding texel (0, 0). No anchor (nothing wrote it): no key (status NoWriter).
//   3. The WRITERS are the anchor and every transfer newer than the anchor that wrote any other block of the valid area. Older
//      writers of other blocks are leftovers of earlier textures in the same streaming slot and are ignored.
//   4. A writer whose format and buffer width are the texture's (same PSM and DBW = TBW) and whose position in its buffer is a
//      whole number of pages from the texture's start covers an exact rectangle: its own rectangle, moved to the texture's origin.
//      Any other writer (a texture cut out of a larger blob uploaded as 32-bit colour, or one page of a canvas) covers the blocks
//      it wrote, as a rectangle of whole blocks.
//   5. The replaced rectangle is the bounding box of what the writers cover, cut to the valid area.
//   6. Textures composed of small pieces (a text canvas: pages of one format uploaded at another buffer width, or more than
//      kMaxWriters writers) are "composed": they change with every string and are not worth replacing. They get no key.
//
// ---- The keys (64-bit hashes, XXH64-based, written as 16 hex digits) ----
//   k3_i  the writers' (payload hash, texture start relative to the transfer's, position in the buffer), sorted, with the PSM, TBW and
//         the size of the rectangle. For an indexed texture it covers the indices only: one key for every recolouring.
//   k3_c  k3_i plus the palette entries the rectangle's indices use, resolved through the live CLUT (hashing only the used entries
//         keeps keys from splitting on entries the part never shows). Indexed textures only, and only up to kMaxColourTexels.
//   k2s_i the fallback for a texture without a writer: a hash of the 16 x 16 tiles the first draw reaches (raw indices or colours).
// A transfer's payload hash covers its bytes (independent of how the DMA chunks them), the destination format and the size of its
// rectangle, not where it was sent to.
//
// Threads: every call comes from the one thread that draws (under the backend's mutex); the class is not thread-safe.

#include "runtime/gs/gs_texture_page_cache.h"
#include "runtime/gs/gs_types.h"

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace TexKeys
{
    // ---- GS memory geometry ----
    constexpr uint32_t kBlocks = 16384u;

    struct Geom
    {
        uint32_t pageW, pageH, blockW, blockH, cols;
        const uint8_t *table;
    };
    // The page and block layout of a pixel format; false for formats the tracker does not know (Z formats).
    bool GeometryOf(uint32_t psm, Geom &g);

    // Calls f(block, texelX0, texelY0, texelX1, texelY1) for every block of GS memory that the rectangle (x, y, w, h) of a buffer at
    // block pointer `bp` with width `bw` (in units of 64 pixels) touches; the texel rectangle is the whole block (inclusive).
    template <class F>
    void ForEachBlock(uint32_t psm, uint32_t bp, uint32_t bw, int x, int y, int w, int h, F &&f)
    {
        Geom g;
        if (!GeometryOf(psm, g) || w <= 0 || h <= 0 || x < 0 || y < 0)
            return;
        const uint32_t perRow = std::max<uint32_t>((bw * 64u) / g.pageW, 1u);
        const uint32_t bx0 = static_cast<uint32_t>(x) / g.blockW, bx1 = static_cast<uint32_t>(x + w - 1) / g.blockW;
        const uint32_t by0 = static_cast<uint32_t>(y) / g.blockH, by1 = static_cast<uint32_t>(y + h - 1) / g.blockH;
        for (uint32_t by = by0; by <= by1; ++by)
            for (uint32_t bx = bx0; bx <= bx1; ++bx)
            {
                const uint32_t px = bx * g.blockW, py = by * g.blockH;
                const uint32_t page = (py / g.pageH) * perRow + px / g.pageW;
                const uint32_t inPage = g.table[((py % g.pageH) / g.blockH) * g.cols + (px % g.pageW) / g.blockW];
                f((bp + page * 32u + inPage) & (kBlocks - 1u), static_cast<int>(px), static_cast<int>(py), static_cast<int>(px + g.blockW - 1u),
                  static_cast<int>(py + g.blockH - 1u));
            }
    }

    // ---- hashes ----
    inline uint64_t Rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
    uint64_t Mix64(uint64_t x);
    uint64_t Combine(uint64_t h, uint64_t v);
    uint64_t HashBytes(const void *data, size_t size, uint64_t seed);

    // A hash of a byte stream that does not depend on how the bytes arrive in chunks (the DMA splits a transfer wherever it likes).
    struct Stream
    {
        uint64_t lane[4] = {0x243F6A8885A308D3ull, 0x13198A2E03707344ull, 0xA4093822299F31D0ull, 0x082EFA98EC4E6C89ull};
        uint8_t buf[32] = {};
        uint32_t bufLen = 0;
        uint64_t len = 0;
        void update(const uint8_t *p, size_t n);
        uint64_t finish() const;
    };

    // What one block's writer contributes to a key.
    struct WriterEntry
    {
        uint64_t payload = 0;
        int32_t rel = 0; // the texture's pointer minus the transfer's (blocks)
        uint16_t dsax = 0, dsay = 0;
        bool operator<(const WriterEntry &o) const
        {
            if (payload != o.payload)
                return payload < o.payload;
            if (rel != o.rel)
                return rel < o.rel;
            if (dsax != o.dsax)
                return dsax < o.dsax;
            return dsay < o.dsay;
        }
        bool operator==(const WriterEntry &o) const { return payload == o.payload && rel == o.rel && dsax == o.dsax && dsay == o.dsay; }
    };

    struct WriterInfo
    {
        WriterEntry entry;
        uint32_t cells = 0;
        uint32_t dbw = 0, dbp = 0, dpsm = 0, w = 0, h = 0;
    };

    // One host-to-local transfer as the tracker remembers it.
    struct TransferRec
    {
        Stream stream;
        uint32_t dbp = 0, dbw = 0, dpsm = 0, x = 0, y = 0, w = 0, h = 0;
        uint64_t bytes = 0;
        uint64_t marker = 0;
        uint64_t seq = 0;      // 1, 2, 3 ... in the order the transfers began
        uint32_t refs = 0;     // blocks that name this record as their writer
        bool inUse = false;
        bool closed = false;
        uint64_t payloadCache = 0;
        uint64_t payload() const; // the hash of the transfer's bytes, format and size (cached once the transfer is closed)
    };

    struct Rect
    {
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1; // inclusive
        bool empty() const { return x1 < x0 || y1 < y0; }
        int width() const { return empty() ? 0 : x1 - x0 + 1; }
        int height() const { return empty() ? 0 : y1 - y0 + 1; }
        int64_t area() const { return empty() ? 0 : static_cast<int64_t>(width()) * height(); }
        bool operator==(const Rect &o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
        bool contains(const Rect &o) const { return !empty() && !o.empty() && o.x0 >= x0 && o.y0 >= y0 && o.x1 <= x1 && o.y1 <= y1; }
    };
    Rect Unite(const Rect &a, const Rect &b);

    // A texture as a draw declares it.
    struct TextureDesc
    {
        uint32_t tbp = 0, tbw = 0, psm = 0;
        uint32_t width = 1, height = 1; // declared (TW, TH)
        bool indexed() const;
    };

    // The largest rectangle (texels) for which the colouring key k3_c is formed.
    constexpr int64_t kMaxColourTexels = 256 * 256;

    // The most writers a texture may have to be keyed; more is a texture composed of pieces (the 256 tiles of a tiled texture).
    constexpr uint32_t kMaxWriters = 16;

    struct LiveKey
    {
        enum class Status : uint8_t
        {
            Ok,
            NoWriter,  // nothing wrote the texture's first block (no upload to tie it to)
            Composed,  // made of small pieces: a text canvas, a tiled texture
            Empty      // the writers cover nothing of the valid area
        };
        Status status = Status::NoWriter;
        uint64_t k3i = 0;
        Rect rect;                // the replaced rectangle, in texels of the declared texture
        uint32_t writers = 0;
        bool exact = true;        // every writer covered an exact rectangle (see rule 4), not whole blocks
        uint64_t anchorPayload = 0;
        uint64_t anchorSeq = 0;
        uint32_t cells = 0;       // blocks of the valid area the writers wrote
        uint32_t validWidth = 0;  // the valid area's width
    };

    class Tracker
    {
    public:
        Tracker();
        ~Tracker();
        Tracker(const Tracker &) = delete;
        Tracker &operator=(const Tracker &) = delete;

        // Host-to-local transfers (direction 0) start a record and mark their blocks as written by it; a local-to-local copy marks
        // its destination blocks as unknown. UploadData is called with each chunk of a transfer's data.
        void BeginTransfer(const GSTransferCommand &command, uint64_t marker);
        void UploadData(const uint8_t *data, uint32_t bytes);
        // A single pixel written to GS memory by the runtime (WriteVram): its block is unknown from now on.
        void WritePixel(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
        // The memory image was replaced or reset: nothing is known about who wrote it.
        void MemoryReset();

        // The current transfer (the last one begun), or nullptr.
        const TransferRec *CurrentTransfer() const;
        uint64_t TransferCount() const { return m_seq; }
        // Called when a transfer is complete (the next one begins, or the tracker is reset) with the finished record. The census uses
        // it to tabulate uploads.
        void SetCloseCallback(std::function<void(const TransferRec &)> callback) { m_onClose = std::move(callback); }

        // Which transfers wrote the blocks of a rectangle of a texture or CLUT, added to `out` (with the number of blocks each wrote).
        void CollectWriters(uint32_t psm, uint32_t tbp, uint32_t tbw, int x0, int y0, int x1, int y1, std::vector<WriterInfo> &out, uint32_t &cells,
                            uint32_t &known) const;

        // The live key of a texture, as described at the top of this file.
        LiveKey KeyFor(const TextureDesc &texture) const;

    private:
        void closeCurrent();
        void releaseRecord(uint32_t index);
        const TransferRec &recFor(uint32_t blockWriterValue) const { return m_recs[blockWriterValue - 1u]; }

        std::vector<TransferRec> m_recs;
        std::vector<uint32_t> m_free;
        std::vector<uint32_t> m_blockWriter; // record index + 1, 0 = unknown
        int m_cur = -1;
        uint64_t m_seq = 0;
        std::function<void(const TransferRec &)> m_onClose;
    };

    // ---- reading a texture's texels ----
    // The raw value of every texel of `rect` of a declared texture, row by row: one byte per texel for indexed textures (the palette
    // index), four for the others (RGBA as the renderer decodes them: r | g << 8 | b << 16 | a << 24, alpha as the GS sees it, 0x80 =
    // opaque). Reads GS memory as it is now.
    void ReadTexels(const uint8_t *vram, const TextureDesc &texture, const GSTexaReg &texa, const Rect &rect, std::vector<uint8_t> &out);

    // k3_c from k3_i: the key with the palette entries the rectangle's indices use. `indices` is ReadTexels' output for an indexed
    // texture; `palette` holds 256 resolved entries (r | g << 8 | b << 16 | a << 24).
    uint64_t ColouringKey(uint64_t k3i, const std::vector<uint8_t> &indices, const uint32_t *palette);
    // How many different palette entries `indices` uses, and the bit set of them.
    std::bitset<256> UsedIndices(const std::vector<uint8_t> &indices);

    // The fallback key for a texture without a writer: the 16 x 16 tiles that `reach` touches (raw values), with the format.
    // Also returns the rectangle it stands for (the tiles, cut to the declared size).
    uint64_t FallbackKey(const uint8_t *vram, const TextureDesc &texture, const GSTexaReg &texa, const Rect &reach, Rect &rectOut);

    // A key as 16 hex digits.
    std::string KeyName(uint64_t key);
}
