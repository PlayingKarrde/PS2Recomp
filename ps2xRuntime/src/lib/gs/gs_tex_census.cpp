// The texture census and dumper (task t1a). See include/runtime/gs/gs_tex_census.h for what it measures and the switches.
//
// Reading guide:
//   block geometry          which 256-byte blocks of GS memory a rectangle of a format covers (the GS page and block layouts)
//   Stream                  a hash of a byte stream that does not depend on how the bytes are chunked
//   PNG writer              a small deflate (fixed Huffman codes, LZ77) so the dump folder stays a sane size
//   Census::Impl            instances (one per texture content), uploads, scenes, row output, the dumper
//
// Everything is opt-in; the backend holds no Census unless an environment variable asked for it.

#include "runtime/gs/gs_tex_census.h"

#include "runtime/gs/gs_recording.h"
#include "runtime/gs/gs_texel.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TexCensus
{
namespace
{
    uint64_t nowNs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    inline uint64_t rotl64(uint64_t x, int r)
    {
        return (x << r) | (x >> (64 - r));
    }

    inline uint64_t mix64(uint64_t x)
    {
        x ^= x >> 33;
        x *= 0xFF51AFD7ED558CCDull;
        x ^= x >> 33;
        x *= 0xC4CEB9FE1A85EC53ull;
        x ^= x >> 33;
        return x;
    }

    inline uint64_t combine(uint64_t h, uint64_t v)
    {
        return mix64(h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2)));
    }

    uint64_t hash64(const void *data, size_t size, uint64_t seed)
    {
        return GSRec::Hash64(data, size, seed);
    }

    void appendf(std::string &out, const char *fmt, ...)
    {
        char buffer[512];
        va_list args;
        va_start(args, fmt);
        const int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        if (n > 0)
            out.append(buffer, static_cast<size_t>(std::min<int>(n, sizeof(buffer) - 1)));
    }

    // A hash of a byte stream that does not depend on how the bytes arrive in chunks (the DMA splits a transfer wherever it likes).
    // Four independent lanes take 32 bytes per step, and a partial stripe waits in a small buffer, so the value only depends on the bytes.
    struct Stream
    {
        uint64_t lane[4] = {0x243F6A8885A308D3ull, 0x13198A2E03707344ull, 0xA4093822299F31D0ull, 0x082EFA98EC4E6C89ull};
        uint8_t buf[32] = {};
        uint32_t bufLen = 0;
        uint64_t len = 0;

        static uint64_t round(uint64_t acc, uint64_t w)
        {
            acc ^= rotl64(w * 0xC2B2AE3D27D4EB4Full, 31) * 0x9E3779B185EBCA87ull;
            return rotl64(acc, 27) * 0x9E3779B185EBCA87ull + 0x85EBCA77C2B2AE63ull;
        }

        static void stripe(uint64_t *lanes, const uint8_t *p)
        {
            for (int i = 0; i < 4; ++i)
            {
                uint64_t w;
                std::memcpy(&w, p + 8 * i, 8);
                lanes[i] = round(lanes[i], w);
            }
        }

        void update(const uint8_t *p, size_t n)
        {
            len += n;
            if (bufLen)
            {
                const size_t take = std::min<size_t>(n, 32u - bufLen);
                std::memcpy(buf + bufLen, p, take);
                bufLen += static_cast<uint32_t>(take);
                p += take;
                n -= take;
                if (bufLen < 32u)
                    return;
                stripe(lane, buf);
                bufLen = 0;
            }
            while (n >= 32)
            {
                stripe(lane, p);
                p += 32;
                n -= 32;
            }
            if (n)
            {
                std::memcpy(buf, p, n);
                bufLen = static_cast<uint32_t>(n);
            }
        }

        uint64_t finish() const
        {
            uint64_t l[4] = {lane[0], lane[1], lane[2], lane[3]};
            if (bufLen)
            {
                uint8_t last[32] = {};
                std::memcpy(last, buf, bufLen);
                stripe(l, last);
            }
            uint64_t h = len * 0x9E3779B185EBCA87ull;
            for (int i = 0; i < 4; ++i)
                h = mix64(h ^ l[i]) + static_cast<uint64_t>(i);
            return mix64(h ^ len);
        }
    };

    // ------------------------------------------------------------------------------------------------
    // GS memory blocks. A block is 256 bytes (the unit of TBP and DBP); a page is 32 blocks. The order of the blocks inside a
    // page depends on the format (the GS documentation's tables, as the arrangement of the page's blocks in rows).
    constexpr uint32_t kBlocks = 16384u;
    constexpr uint32_t kTile = 16u;
    constexpr uint32_t kTilesPerSide = 64u;

    const uint8_t kTab32[32] = {0, 1, 4, 5, 16, 17, 20, 21, 2, 3, 6, 7, 18, 19, 22, 23, 8, 9, 12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31};
    const uint8_t kTab16[32] = {0, 2, 8, 10, 1, 3, 9, 11, 4, 6, 12, 14, 5, 7, 13, 15, 16, 18, 24, 26, 17, 19, 25, 27, 20, 22, 28, 30, 21, 23, 29, 31};
    const uint8_t kTab16S[32] = {0, 2, 16, 18, 1, 3, 17, 19, 8, 10, 24, 26, 9, 11, 25, 27, 4, 6, 20, 22, 5, 7, 21, 23, 12, 14, 28, 30, 13, 15, 29, 31};

    struct Geom
    {
        uint32_t pageW, pageH, blockW, blockH, cols;
        const uint8_t *table;
    };

    bool geomOf(uint32_t psm, Geom &g)
    {
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_CT24:
        case GS_PSM_T8H:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            g = {64u, 32u, 8u, 8u, 8u, kTab32};
            return true;
        case GS_PSM_CT16:
            g = {64u, 64u, 16u, 8u, 4u, kTab16};
            return true;
        case GS_PSM_CT16S:
            g = {64u, 64u, 16u, 8u, 4u, kTab16S};
            return true;
        case GS_PSM_T8:
            g = {128u, 64u, 16u, 16u, 8u, kTab32};
            return true;
        case GS_PSM_T4:
            g = {128u, 128u, 32u, 16u, 4u, kTab16};
            return true;
        default:
            return false;
        }
    }

    // Calls f(block) for every block of GS memory that the rectangle (x, y, w, h) of a buffer at block pointer `bp` with width `bw`
    // (in units of 64 pixels) touches. A partly covered block counts.
    template <class F>
    void forEachBlock(uint32_t psm, uint32_t bp, uint32_t bw, int x, int y, int w, int h, F &&f)
    {
        Geom g;
        if (!geomOf(psm, g) || w <= 0 || h <= 0 || x < 0 || y < 0)
            return;
        const uint32_t perRow = (bw * 64u) / g.pageW;
        const uint32_t bx0 = static_cast<uint32_t>(x) / g.blockW, bx1 = static_cast<uint32_t>(x + w - 1) / g.blockW;
        const uint32_t by0 = static_cast<uint32_t>(y) / g.blockH, by1 = static_cast<uint32_t>(y + h - 1) / g.blockH;
        for (uint32_t by = by0; by <= by1; ++by)
            for (uint32_t bx = bx0; bx <= bx1; ++bx)
            {
                const uint32_t px = bx * g.blockW, py = by * g.blockH;
                const uint32_t page = (py / g.pageH) * perRow + px / g.pageW;
                const uint32_t inPage = g.table[((py % g.pageH) / g.blockH) * g.cols + (px % g.pageW) / g.blockW];
                f((bp + page * 32u + inPage) & (kBlocks - 1u));
            }
    }

    // ------------------------------------------------------------------------------------------------
    // A PNG writer: filter "up", LZ77 with a hash chain, one fixed-Huffman deflate block. The files are for people and scripts to read,
    // not to be small, but a dump of thousands of sprites should not fill the disk with stored blocks.
    uint32_t crc32Of(const uint8_t *data, size_t size, uint32_t crc = 0)
    {
        static uint32_t table[256];
        static std::once_flag once;
        std::call_once(once,
                       []
                       {
                           for (uint32_t i = 0; i < 256; ++i)
                           {
                               uint32_t c = i;
                               for (int k = 0; k < 8; ++k)
                                   c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                               table[i] = c;
                           }
                       });
        crc = ~crc;
        for (size_t i = 0; i < size; ++i)
            crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
        return ~crc;
    }

    struct BitOut
    {
        std::vector<uint8_t> &out;
        uint32_t buffer = 0;
        int count = 0;
        explicit BitOut(std::vector<uint8_t> &o) : out(o) {}
        void put(uint32_t value, int bits)
        {
            buffer |= value << count;
            count += bits;
            while (count >= 8)
            {
                out.push_back(static_cast<uint8_t>(buffer & 0xFFu));
                buffer >>= 8;
                count -= 8;
            }
        }
        void putCode(uint32_t code, int bits) // Huffman codes go most significant bit first
        {
            uint32_t r = 0;
            for (int i = 0; i < bits; ++i)
                r = (r << 1) | ((code >> i) & 1u);
            put(r, bits);
        }
        void flush()
        {
            if (count > 0)
            {
                out.push_back(static_cast<uint8_t>(buffer & 0xFFu));
                buffer = 0;
                count = 0;
            }
        }
    };

    void putLiteralLength(BitOut &b, uint32_t sym)
    {
        if (sym < 144u)
            b.putCode(0x30u + sym, 8);
        else if (sym < 256u)
            b.putCode(0x190u + (sym - 144u), 9);
        else if (sym < 280u)
            b.putCode(sym - 256u, 7);
        else
            b.putCode(0xC0u + (sym - 280u), 8);
    }

    const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    void deflateFixed(const uint8_t *data, size_t n, std::vector<uint8_t> &out)
    {
        BitOut b(out);
        b.put(1, 1); // final block
        b.put(1, 2); // fixed Huffman codes
        constexpr uint32_t kWindow = 32768u;
        constexpr uint32_t kHashSize = 1u << 15;
        std::vector<int32_t> head(kHashSize, -1);
        std::vector<int32_t> prev(kWindow, -1);
        auto hashAt = [&](size_t i) -> uint32_t { return ((static_cast<uint32_t>(data[i]) << 10) ^ (static_cast<uint32_t>(data[i + 1]) << 5) ^ data[i + 2]) & (kHashSize - 1u); };
        size_t i = 0;
        while (i < n)
        {
            uint32_t bestLen = 0, bestDist = 0;
            if (i + 3 <= n)
            {
                const uint32_t h = hashAt(i);
                int32_t cand = head[h];
                int chain = 24;
                const size_t maxLen = std::min<size_t>(258, n - i);
                while (cand >= 0 && chain-- > 0 && i - static_cast<size_t>(cand) <= kWindow)
                {
                    const size_t c = static_cast<size_t>(cand);
                    size_t len = 0;
                    while (len < maxLen && data[c + len] == data[i + len])
                        ++len;
                    if (len >= 3 && len > bestLen)
                    {
                        bestLen = static_cast<uint32_t>(len);
                        bestDist = static_cast<uint32_t>(i - c);
                        if (len == maxLen)
                            break;
                    }
                    cand = prev[c & (kWindow - 1u)];
                }
            }
            const size_t step = bestLen >= 3 ? bestLen : 1;
            if (bestLen >= 3)
            {
                uint32_t lc = 0;
                while (lc + 1 < 29 && kLenBase[lc + 1] <= bestLen)
                    ++lc;
                putLiteralLength(b, 257u + lc);
                if (kLenExtra[lc])
                    b.put(bestLen - kLenBase[lc], kLenExtra[lc]);
                uint32_t dc = 0;
                while (dc + 1 < 30 && kDistBase[dc + 1] <= bestDist)
                    ++dc;
                b.putCode(dc, 5);
                if (kDistExtra[dc])
                    b.put(bestDist - kDistBase[dc], kDistExtra[dc]);
            }
            else
            {
                putLiteralLength(b, data[i]);
            }
            for (size_t k = 0; k < step; ++k, ++i)
                if (i + 3 <= n)
                {
                    const uint32_t h = hashAt(i);
                    prev[i & (kWindow - 1u)] = head[h];
                    head[h] = static_cast<int32_t>(i);
                }
        }
        putLiteralLength(b, 256u);
        b.flush();
    }

    void putBe32(std::vector<uint8_t> &v, uint32_t x)
    {
        v.push_back(static_cast<uint8_t>(x >> 24));
        v.push_back(static_cast<uint8_t>(x >> 16));
        v.push_back(static_cast<uint8_t>(x >> 8));
        v.push_back(static_cast<uint8_t>(x));
    }

    void pngChunk(std::vector<uint8_t> &png, const char *tag, const std::vector<uint8_t> &body)
    {
        putBe32(png, static_cast<uint32_t>(body.size()));
        const size_t start = png.size();
        png.insert(png.end(), tag, tag + 4);
        png.insert(png.end(), body.begin(), body.end());
        putBe32(png, crc32Of(png.data() + start, png.size() - start));
    }

    // colorType 6: RGBA8, 3: paletted 8 bit (plte holds RGB triples, trns the alpha values), 0: grey 8 bit. `pixels` holds the rows
    // without filter bytes.
    std::vector<uint8_t> encodePng(uint32_t w, uint32_t h, int colorType, const std::vector<uint8_t> &pixels, const std::vector<uint8_t> &plte,
                                   const std::vector<uint8_t> &trns)
    {
        const size_t bpp = colorType == 6 ? 4 : 1;
        const size_t rowBytes = static_cast<size_t>(w) * bpp;
        std::vector<uint8_t> raw;
        raw.reserve((rowBytes + 1) * h);
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t *row = pixels.data() + y * rowBytes;
            if (y == 0)
            {
                raw.push_back(0);
                raw.insert(raw.end(), row, row + rowBytes);
            }
            else
            {
                raw.push_back(2); // up
                const uint8_t *above = row - rowBytes;
                for (size_t x = 0; x < rowBytes; ++x)
                    raw.push_back(static_cast<uint8_t>(row[x] - above[x]));
            }
        }
        std::vector<uint8_t> z = {0x78, 0x01};
        deflateFixed(raw.data(), raw.size(), z);
        uint32_t a = 1, bsum = 0;
        for (size_t k = 0; k < raw.size();)
        {
            const size_t end = std::min(raw.size(), k + 5552);
            for (; k < end; ++k)
            {
                a += raw[k];
                bsum += a;
            }
            a %= 65521u;
            bsum %= 65521u;
        }
        putBe32(z, (bsum << 16) | a);
        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        std::vector<uint8_t> ihdr;
        putBe32(ihdr, w);
        putBe32(ihdr, h);
        ihdr.push_back(8);
        ihdr.push_back(static_cast<uint8_t>(colorType));
        ihdr.push_back(0);
        ihdr.push_back(0);
        ihdr.push_back(0);
        pngChunk(png, "IHDR", ihdr);
        if (colorType == 3)
        {
            pngChunk(png, "PLTE", plte);
            if (!trns.empty())
                pngChunk(png, "tRNS", trns);
        }
        pngChunk(png, "IDAT", z);
        pngChunk(png, "IEND", {});
        return png;
    }

    // ------------------------------------------------------------------------------------------------
    struct Rect
    {
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
        bool empty() const { return x1 < x0 || y1 < y0; }
        bool contains(const Rect &o) const { return !empty() && !o.empty() && o.x0 >= x0 && o.y0 >= y0 && o.x1 <= x1 && o.y1 <= y1; }
        bool operator==(const Rect &o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
        int64_t area() const { return empty() ? 0 : static_cast<int64_t>(x1 - x0 + 1) * (y1 - y0 + 1); }
    };

    Rect unite(const Rect &a, const Rect &b)
    {
        if (a.empty())
            return b;
        if (b.empty())
            return a;
        return Rect{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
    }

    // The texels the draws sample (see the header): like GSGpu::TextureReach without the one texel of guard on each side.
    Rect tightReach(const GSGpu::Setup &setup, uint64_t clampReg, uint32_t width, uint32_t height, bool bilinear)
    {
        Rect whole{0, 0, static_cast<int>(width) - 1, static_cast<int>(height) - 1};
        if (setup.count == 0 || width == 0u || height == 0u)
            return whole;
        const float size[2] = {static_cast<float>(width), static_cast<float>(height)};
        float lo[2] = {1.0e30f, 1.0e30f}, hi[2] = {-1.0e30f, -1.0e30f};
        for (uint32_t i = 0; i < setup.count; ++i)
        {
            const GSGpu::Vertex &v = setup.v[i];
            float c[2];
            if (setup.texelCoords)
            {
                c[0] = v.u;
                c[1] = v.v;
            }
            else
            {
                if (!(v.q > 0.0f))
                    return whole;
                c[0] = v.u / v.q * size[0];
                c[1] = v.v / v.q * size[1];
            }
            for (int k = 0; k < 2; ++k)
            {
                if (!(std::fabs(c[k]) < 1.0e6f))
                    return whole;
                lo[k] = std::min(lo[k], c[k]);
                hi[k] = std::max(hi[k], c[k]);
            }
        }
        const int extent[2] = {static_cast<int>(width), static_cast<int>(height)};
        const uint32_t mode[2] = {static_cast<uint32_t>(clampReg & 3u), static_cast<uint32_t>((clampReg >> 2) & 3u)};
        const int regionMin[2] = {static_cast<int>((clampReg >> 4) & 0x3FFu), static_cast<int>((clampReg >> 24) & 0x3FFu)};
        const int regionMax[2] = {static_cast<int>((clampReg >> 14) & 0x3FFu), static_cast<int>((clampReg >> 34) & 0x3FFu)};
        int first[2], last[2];
        for (int k = 0; k < 2; ++k)
        {
            int l, h;
            if (bilinear)
            {
                l = static_cast<int>(std::floor(lo[k] - 0.5f));
                h = static_cast<int>(std::floor(hi[k] - 0.5f)) + 1;
            }
            else
            {
                l = static_cast<int>(std::floor(lo[k]));
                h = static_cast<int>(std::ceil(hi[k])) - 1;
                if (h < l)
                    h = l;
            }
            first[k] = 0;
            last[k] = extent[k] - 1;
            switch (mode[k])
            {
            case 0u: // REPEAT
                if (l >= 0 && h <= extent[k] - 1)
                {
                    first[k] = l;
                    last[k] = h;
                }
                break;
            case 1u: // CLAMP
                first[k] = std::clamp(l, 0, extent[k] - 1);
                last[k] = std::clamp(h, 0, extent[k] - 1);
                break;
            case 2u: // REGION_CLAMP
                first[k] = std::clamp(std::min(std::max(l, regionMin[k]), regionMax[k]), 0, extent[k] - 1);
                last[k] = std::clamp(std::min(std::max(h, regionMin[k]), regionMax[k]), 0, extent[k] - 1);
                break;
            default:
                break;
            }
        }
        return Rect{first[0], first[1], last[0], last[1]};
    }

    // One host-to-local transfer, from its BeginTransfer to the next.
    struct UploadRec
    {
        Stream stream;
        uint32_t dbp = 0, dbw = 0, dpsm = 0, x = 0, y = 0, w = 0, h = 0;
        uint64_t bytes = 0;
        uint64_t marker = 0;
        uint32_t refs = 0;   // blocks that name this record as their writer
        bool inUse = false;
        uint64_t payload() const
        {
            return combine(combine(stream.finish(), (static_cast<uint64_t>(dpsm) << 40) | (static_cast<uint64_t>(w) << 20) | h), 0x5041594Cull);
        }
    };

    // Who wrote a block, as a key ingredient: the payload (bytes, format and size of the transfer), where the texture's pointer sits
    // relative to the transfer's, and where the transfer put its rectangle inside its buffer.
    struct WriterEntry
    {
        uint64_t payload = 0;
        int32_t rel = 0;
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

    struct Tile
    {
        std::vector<uint8_t> data; // 16 x 16 indices (indexed textures) or RGBA texels (4 bytes each)
    };

    struct Instance
    {
        uint64_t k1 = 0;
        uint32_t tbp = 0, tbw = 0, psm = 0, w = 0, h = 0, texa = 0;
        uint32_t cpsm = 0, csm = 0, csa = 0, cbp = 0;
        uint64_t clutHash = 0;
        bool indexed = false, four = false;
        int dynamic = 0; // 1: many different contents at one place (movie frames), 2: large colour texture
        uint64_t firstMarker = 0, lastMarker = 0;
        uint64_t draws = 0, sprites = 0, tris = 0, bilinear = 0, nearest = 0, fst = 0, stq = 0;
        uint32_t wmsMask = 0, wmtMask = 0;
        bool region = false;
        int regionMin[2] = {0, 0}, regionMax[2] = {0, 0};
        Rect guard, tight, lastGuard, lastTight;
        double magU[2] = {1.0e30, 0.0}, magV[2] = {1.0e30, 0.0}; // sprites: texels per GS pixel along each axis (min, max)
        double triDensity[2] = {1.0e30, 0.0};                    // triangles: sqrt(texel area / GS pixel area), min and max
        std::bitset<kTilesPerSide * kTilesPerSide> reached;
        std::unordered_map<uint32_t, Tile> tiles; // ty * 64 + tx
        std::vector<WriterInfo> writers;
        uint32_t cells = 0, cellsKnown = 0;
        uint32_t palette[256] = {};
        uint32_t paletteCount = 0;
        std::vector<WriterEntry> clutWriters;
    };

    struct PngJob
    {
        std::string path;
        uint32_t w = 0, h = 0;
        int colorType = 6;
        std::vector<uint8_t> pixels, plte, trns;
        bool markDone = false; // not an image: write `path` (an empty file) once everything before it is written
    };

    struct UploadAgg
    {
        uint64_t count = 0, bytes = 0;
        uint32_t dpsm = 0, dbw = 0, w = 0, h = 0, firstDbp = 0;
        std::set<uint32_t> dbps;
        uint64_t firstMarker = 0, lastMarker = 0;
        std::string firstScene;
    };

    struct KeySet
    {
        uint64_t k1 = 0, k2b_i = 0, k2b_c = 0, k2t_i = 0, k2t_c = 0, k2s_i = 0, k2s_c = 0, k3_i = 0, k3_c = 0, k3m_i = 0, k3m_c = 0;
    };

    uint8_t alpha8(uint32_t gsAlpha)
    {
        return gsAlpha >= 0x80u ? 255u : static_cast<uint8_t>(gsAlpha * 2u);
    }

    // texel as the renderer decodes it: r | g << 8 | b << 16 | a << 24 (alpha as the GS sees it, 0x80 = opaque)
    void texelToRgba8(uint32_t texel, uint8_t *out)
    {
        out[0] = static_cast<uint8_t>(texel & 0xFFu);
        out[1] = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        out[2] = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        out[3] = alpha8(texel >> 24);
    }
} // namespace

// ------------------------------------------------------------------------------------------------
struct Census::Impl
{
    // ---- configuration
    std::string censusPath, dumpDir, scenePath;
    bool dump = false, verify = false, statsOn = false;
    std::string dumpKeyName = "k3m_i";
    uint32_t dumpMax = 40000, dumpCount = 0;

    // ---- output
    std::FILE *csv = nullptr, *tilesCsv = nullptr, *palCsv = nullptr, *texCsv = nullptr;
    uint64_t rowCounter = 0;
    std::string scene = "boot";
    bool finished = false;
    static const char *header()
    {
        return "row,scene,marker_first,marker_last,k1,tbp,tbw,psm,w,h,indexed,cpsm,csm,csa,cbp,clut_hash,texa,"
               "draws,sprite_draws,tri_draws,bilinear_draws,nearest_draws,fst_draws,stq_draws,wms_mask,wmt_mask,region,"
               "g_x0,g_y0,g_x1,g_y1,t_x0,t_y0,t_x1,t_y1,guard_area,tight_area,reached_tiles,"
               "mag_u_min,mag_u_max,mag_v_min,mag_v_max,tri_dens_min,tri_dens_max,dynamic,"
               "k2b_i,k2b_c,k2t_i,k2t_c,k2s_i,k2s_c,k3_i,k3_c,k3m_i,k3m_c,"
               "writers,cells,cells_known,main_payload,main_w,main_h,main_psm,main_dbw,main_dbp,main_rel,main_cells,clut_writer,clut_writers,"
               "used_entries,palette_n";
    }

    // ---- uploads and the block map
    std::vector<UploadRec> recs;
    std::vector<uint32_t> freeRecs;
    std::vector<uint32_t> blockWriter = std::vector<uint32_t>(kBlocks, 0u); // record index + 1, 0 = unknown
    int cur = -1;
    std::unordered_map<uint64_t, UploadAgg> uploads;
    uint64_t uploadsDropped = 0; // distinct transfers not tabled once `uploads` is full
    // verify mode
    std::vector<uint8_t> predicted = std::vector<uint8_t>(kBlocks, 0u);
    std::vector<uint8_t> beforePages;
    GSGpu::PageSpan beforeSpan;
    uint64_t verifyChunks = 0, verifyChangedBlocks = 0, verifyViolations = 0, verifyPredicted = 0, verifyUnchangedPredicted = 0;
    std::vector<uint8_t> changedSeen = std::vector<uint8_t>(kBlocks, 0u);

    // ---- instances
    std::unordered_map<uint64_t, std::unique_ptr<Instance>> instances;
    std::unordered_map<uint64_t, uint32_t> slotCounts;
    std::unordered_map<uint64_t, uint32_t> dumpedArea; // dump key -> area of the file written
    std::unordered_map<uint64_t, std::set<uint64_t>> dumpedPalettes;

    // ---- cost
    uint64_t costNs = 0, drawCalls = 0, uploadCalls = 0, lastStatsMarker = 0, curMarker = 0;
    // where the cost goes (printed with PS2X_TEX_CENSUS_STATS=1 and when a scene closes)
    uint64_t costBeginNs = 0, costAfterNs = 0, costCreateNs = 0, costGrowNs = 0, costPollNs = 0, costCloseNs = 0, instancesCreated = 0, growths = 0, tilesCaptured = 0;
    uint64_t scenesClosed = 0;

    // ---- PNG worker
    std::thread worker;
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<PngJob> queue;
    uint64_t queuedBytes = 0;
    bool workerStop = false;
    std::atomic<uint64_t> pngWritten{0};

    Impl() = default;
    ~Impl()
    {
        stopWorker();
        for (std::FILE *f : {csv, tilesCsv, palCsv, texCsv})
            if (f)
                std::fclose(f);
    }

    // ---------------------------------------------------------------- worker
    void startWorker()
    {
        worker = std::thread(
            [this]
            {
                for (;;)
                {
                    PngJob job;
                    {
                        std::unique_lock<std::mutex> lock(queueMutex);
                        queueCv.wait(lock, [this] { return workerStop || !queue.empty(); });
                        if (queue.empty())
                            return;
                        job = std::move(queue.front());
                        queue.pop_front();
                        queuedBytes -= job.pixels.size();
                    }
                    if (job.markDone)
                    {
                        if (std::FILE *f = std::fopen(job.path.c_str(), "wb"))
                            std::fclose(f);
                        continue;
                    }
                    const std::vector<uint8_t> png = encodePng(job.w, job.h, job.colorType, job.pixels, job.plte, job.trns);
                    if (std::FILE *f = std::fopen(job.path.c_str(), "wb"))
                    {
                        std::fwrite(png.data(), 1, png.size(), f);
                        std::fclose(f);
                        ++pngWritten;
                    }
                }
            });
    }

    void stopWorker()
    {
        if (!worker.joinable())
            return;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            workerStop = true;
        }
        queueCv.notify_all();
        worker.join();
    }

    void enqueue(PngJob &&job)
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        // A scene with thousands of textures closes in one go: do not let the queue grow without limit.
        while (queuedBytes > (768ull << 20) && !workerStop)
        {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            lock.lock();
        }
        queuedBytes += job.pixels.size();
        queue.push_back(std::move(job));
        queueCv.notify_one();
    }

    // ---------------------------------------------------------------- uploads
    void releaseRecord(uint32_t index)
    {
        UploadRec &r = recs[index];
        if (r.refs == 0 && r.inUse && static_cast<int>(index) != cur)
        {
            r.inUse = false;
            freeRecs.push_back(index);
        }
    }

    void closeCurrent(uint64_t marker)
    {
        if (cur < 0)
            return;
        UploadRec &r = recs[static_cast<size_t>(cur)];
        const uint64_t payload = r.payload();
        auto found = uploads.find(payload);
        if (found == uploads.end())
        {
            // A movie uploads thousands of different frames and strips: keep the table (and the file written from it) bounded.
            if (uploads.size() >= 300000u)
            {
                ++uploadsDropped;
                const uint32_t index = static_cast<uint32_t>(cur);
                cur = -1;
                releaseRecord(index);
                return;
            }
            found = uploads.emplace(payload, UploadAgg{}).first;
        }
        UploadAgg &a = found->second;
        if (a.count == 0)
        {
            a.dpsm = r.dpsm;
            a.dbw = r.dbw;
            a.w = r.w;
            a.h = r.h;
            a.firstDbp = r.dbp;
            a.firstMarker = r.marker;
            a.firstScene = scene;
        }
        ++a.count;
        a.bytes += r.bytes;
        a.lastMarker = marker;
        if (a.dbps.size() < 8)
            a.dbps.insert(r.dbp);
        const uint32_t index = static_cast<uint32_t>(cur);
        cur = -1;
        releaseRecord(index);
    }

    void beginTransfer(const GSTransferCommand &c, uint64_t marker)
    {
        closeCurrent(marker);
        uint32_t index;
        if (!freeRecs.empty())
        {
            index = freeRecs.back();
            freeRecs.pop_back();
        }
        else
        {
            index = static_cast<uint32_t>(recs.size());
            recs.emplace_back();
        }
        UploadRec &r = recs[index];
        r = UploadRec{};
        r.inUse = true;
        r.dbp = c.bitbltbuf.dbp;
        r.dbw = std::max<uint32_t>(c.bitbltbuf.dbw, 1u);
        r.dpsm = c.bitbltbuf.dpsm;
        r.x = c.trxpos.dsax;
        r.y = c.trxpos.dsay;
        r.w = c.trxreg.rrw;
        r.h = c.trxreg.rrh;
        r.marker = marker;
        cur = static_cast<int>(index);
        if (verify)
            std::fill(predicted.begin(), predicted.end(), 0);
        forEachBlock(r.dpsm, r.dbp, r.dbw, r.x, r.y, r.w, r.h,
                     [&](uint32_t block)
                     {
                         const uint32_t old = blockWriter[block];
                         if (old != 0u)
                         {
                             UploadRec &o = recs[old - 1u];
                             if (o.refs > 0)
                                 --o.refs;
                             releaseRecord(old - 1u);
                         }
                         blockWriter[block] = index + 1u;
                         ++r.refs;
                         if (verify)
                             predicted[block] = 1;
                     });
        if (verify)
        {
            std::fill(changedSeen.begin(), changedSeen.end(), 0);
            for (uint8_t p : predicted)
                verifyPredicted += p;
        }
    }

    // Which transfer wrote the blocks of a rectangle of a texture (or CLUT), added to `out` (with the number of blocks each wrote).
    void collectWriters(uint32_t psm, uint32_t tbp, uint32_t tbw, int x0, int y0, int x1, int y1, std::vector<WriterInfo> &out, uint32_t &cells,
                        uint32_t &known)
    {
        forEachBlock(psm, tbp, tbw, x0, y0, x1 - x0 + 1, y1 - y0 + 1,
                     [&](uint32_t block)
                     {
                         ++cells;
                         const uint32_t w = blockWriter[block];
                         if (w == 0u)
                             return;
                         ++known;
                         const UploadRec &r = recs[w - 1u];
                         WriterEntry e;
                         e.payload = r.payload();
                         e.rel = static_cast<int32_t>(tbp) - static_cast<int32_t>(r.dbp);
                         e.dsax = static_cast<uint16_t>(r.x);
                         e.dsay = static_cast<uint16_t>(r.y);
                         for (WriterInfo &wi : out)
                             if (wi.entry == e)
                             {
                                 ++wi.cells;
                                 return;
                             }
                         WriterInfo wi;
                         wi.entry = e;
                         wi.cells = 1;
                         wi.dbw = r.dbw;
                         wi.dbp = r.dbp;
                         wi.dpsm = r.dpsm;
                         wi.w = r.w;
                         wi.h = r.h;
                         out.push_back(wi);
                     });
    }

    // ---------------------------------------------------------------- instances
    static uint32_t readTexel(GSMem::TexturePageCache &cache, const uint8_t *vram, const Instance &I, const GSTexaReg &texa, uint32_t u, uint32_t v)
    {
        const uint32_t raw = GSMem::ReadTexture(cache, vram, I.psm, I.tbp, I.tbw, u, v);
        switch (I.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return GSTexel::applyTexa(texa, static_cast<uint8_t>(I.psm), raw);
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return GSTexel::applyTexa(texa, static_cast<uint8_t>(I.psm), GSTexel::Rgba5551ToRgba8888(static_cast<uint16_t>(raw)));
        case GS_PSM_T8:
        case GS_PSM_T8H:
            return raw & 0xFFu;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return raw & 0x0Fu;
        default:
            return 0xFFFF00FFu;
        }
    }

    void captureTiles(Instance &I, const Rect &r, const DrawInfo &d)
    {
        GSMem::TexturePageCache cache;
        for (int ty = r.y0 / static_cast<int>(kTile); ty <= r.y1 / static_cast<int>(kTile); ++ty)
            for (int tx = r.x0 / static_cast<int>(kTile); tx <= r.x1 / static_cast<int>(kTile); ++tx)
            {
                const uint32_t key = static_cast<uint32_t>(ty) * kTilesPerSide + static_cast<uint32_t>(tx);
                if (I.tiles.count(key))
                    continue;
                Tile t;
                t.data.assign(I.indexed ? 256u : 1024u, 0);
                for (uint32_t v = 0; v < kTile; ++v)
                    for (uint32_t u = 0; u < kTile; ++u)
                    {
                        const uint32_t x = static_cast<uint32_t>(tx) * kTile + u, y = static_cast<uint32_t>(ty) * kTile + v;
                        if (x >= I.w || y >= I.h)
                            continue;
                        const uint32_t value = readTexel(cache, d.vram, I, d.state->texa, x, y);
                        if (I.indexed)
                            t.data[v * kTile + u] = static_cast<uint8_t>(value);
                        else
                            std::memcpy(&t.data[(v * kTile + u) * 4u], &value, 4);
                    }
                I.tiles.emplace(key, std::move(t));
                ++tilesCaptured;
            }
    }

    Instance *instanceFor(const DrawInfo &d)
    {
        auto it = instances.find(d.k1);
        if (it != instances.end())
            return it->second.get();
        auto inst = std::make_unique<Instance>();
        Instance &I = *inst;
        const GSDrawState &s = *d.state;
        const GSTex0Reg &t = s.context.tex0;
        I.k1 = d.k1;
        I.tbp = t.tbp0;
        I.tbw = t.tbw;
        I.psm = t.psm;
        I.w = d.width;
        I.h = d.height;
        I.texa = static_cast<uint32_t>(s.texa.ta0) | (static_cast<uint32_t>(s.texa.ta1) << 8) | (s.texa.aem ? 1u << 16 : 0u);
        I.four = GSTexel::isFourBitIndexedPsm(t.psm);
        I.indexed = I.four || GSTexel::isEightBitIndexedPsm(t.psm);
        I.firstMarker = d.marker;
        if (I.indexed)
        {
            I.cpsm = t.cpsm;
            I.csm = t.csm;
            I.csa = t.csa;
            I.cbp = t.cbp;
            I.paletteCount = I.four ? 16u : 256u;
            for (uint32_t i = 0; i < 256u; ++i)
                I.palette[i] = GSTexel::LookupClutEntry(d.clut->data(), s.texa, static_cast<uint8_t>(i % I.paletteCount), t.cpsm, t.csm, t.csa, t.psm);
            std::vector<uint32_t> pal(I.palette, I.palette + I.paletteCount);
            I.clutHash = hash64(pal.data(), pal.size() * 4u, I.paletteCount);
            // who wrote the CLUT: 16 x 16 entries for 8 bit textures, 8 x 2 for 4 bit (CSM1 layout)
            std::vector<WriterInfo> cw;
            uint32_t cells = 0, known = 0;
            collectWriters(t.cpsm, t.cbp, 1, 0, 0, I.four ? 8 : 16, I.four ? 2 : 16, cw, cells, known);
            for (const WriterInfo &w : cw)
                I.clutWriters.push_back(w.entry);
        }
        // movie frames: many different contents at one place, in a non-indexed format
        const uint64_t slot = combine(combine(combine(I.tbp, I.tbw), combine(I.psm, I.w)), I.h);
        const uint32_t seen = ++slotCounts[slot];
        if (!I.indexed && seen > 32u)
            I.dynamic = 1;
        Instance *raw = inst.get();
        instances.emplace(d.k1, std::move(inst));
        return raw;
    }

    void draw(const DrawInfo &d)
    {
        if (!d.state || !d.setup || !d.vram || !d.clut)
            return;
        const uint64_t tCreate = nowNs();
        const size_t before = instances.size();
        Instance &I = *instanceFor(d);
        if (instances.size() != before)
        {
            costCreateNs += nowNs() - tCreate;
            ++instancesCreated;
        }
        const GSDrawState &s = *d.state;
        const GSGpu::Setup &setup = *d.setup;
        I.lastMarker = d.marker;
        ++I.draws;
        const bool sprite = s.prim.type == GS_PRIM_SPRITE;
        sprite ? ++I.sprites : ++I.tris;
        s.linearFilter ? ++I.bilinear : ++I.nearest;
        s.prim.fst ? ++I.fst : ++I.stq;
        const uint64_t clamp = s.context.clamp;
        I.wmsMask |= 1u << (clamp & 3u);
        I.wmtMask |= 1u << ((clamp >> 2) & 3u);
        if (!I.region && ((clamp & 3u) >= 2u || ((clamp >> 2) & 3u) >= 2u))
        {
            I.region = true;
            I.regionMin[0] = static_cast<int>((clamp >> 4) & 0x3FFu);
            I.regionMax[0] = static_cast<int>((clamp >> 14) & 0x3FFu);
            I.regionMin[1] = static_cast<int>((clamp >> 24) & 0x3FFu);
            I.regionMax[1] = static_cast<int>((clamp >> 34) & 0x3FFu);
        }
        if (sprite && setup.count >= 3 && setup.texelCoords)
        {
            float x0 = setup.v[0].x, x1 = x0, y0 = setup.v[0].y, y1 = y0, u0 = setup.v[0].u, u1 = u0, v0 = setup.v[0].v, v1 = v0;
            for (uint32_t i = 1; i < setup.count; ++i)
            {
                x0 = std::min(x0, setup.v[i].x);
                x1 = std::max(x1, setup.v[i].x);
                y0 = std::min(y0, setup.v[i].y);
                y1 = std::max(y1, setup.v[i].y);
                u0 = std::min(u0, setup.v[i].u);
                u1 = std::max(u1, setup.v[i].u);
                v0 = std::min(v0, setup.v[i].v);
                v1 = std::max(v1, setup.v[i].v);
            }
            const double pw = (x1 - x0) / std::max(1u, d.scaleX), ph = (y1 - y0) / std::max(1u, d.scaleY);
            if (pw > 0.0)
            {
                const double mu = (u1 - u0) / pw;
                I.magU[0] = std::min(I.magU[0], mu);
                I.magU[1] = std::max(I.magU[1], mu);
            }
            if (ph > 0.0)
            {
                const double mv = (v1 - v0) / ph;
                I.magV[0] = std::min(I.magV[0], mv);
                I.magV[1] = std::max(I.magV[1], mv);
            }
        }
        else if (!sprite && setup.count == 3)
        {
            double px[3], py[3], tu[3], tv[3];
            bool ok = true;
            for (int i = 0; i < 3; ++i)
            {
                px[i] = setup.v[i].x / std::max(1u, d.scaleX);
                py[i] = setup.v[i].y / std::max(1u, d.scaleY);
                if (setup.texelCoords)
                {
                    tu[i] = setup.v[i].u;
                    tv[i] = setup.v[i].v;
                }
                else if (setup.v[i].q > 0.0f)
                {
                    tu[i] = setup.v[i].u / setup.v[i].q * I.w;
                    tv[i] = setup.v[i].v / setup.v[i].q * I.h;
                }
                else
                    ok = false;
            }
            if (ok)
            {
                const double areaPix = std::fabs((px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]));
                const double areaTex = std::fabs((tu[1] - tu[0]) * (tv[2] - tv[0]) - (tu[2] - tu[0]) * (tv[1] - tv[0]));
                if (areaPix > 1.0e-3)
                {
                    const double density = std::sqrt(areaTex / areaPix);
                    I.triDensity[0] = std::min(I.triDensity[0], density);
                    I.triDensity[1] = std::max(I.triDensity[1], density);
                }
            }
        }
        const GSGpu::TexelRect gr = GSGpu::TextureReach(setup, clamp, I.w, I.h);
        const Rect G{gr.x0, gr.y0, gr.x1, gr.y1};
        const Rect T = tightReach(setup, clamp, I.w, I.h, s.linearFilter);
        if (G == I.lastGuard && T == I.lastTight)
            return;
        const uint64_t tGrow = nowNs();
        struct GrowTimer
        {
            uint64_t &sink;
            uint64_t start;
            uint64_t &count;
            ~GrowTimer()
            {
                sink += nowNs() - start;
                ++count;
            }
        } growTimer{costGrowNs, tGrow, growths};
        I.lastGuard = G;
        I.lastTight = T;
        I.tight = unite(I.tight, T);
        if (I.dynamic == 0 && !I.indexed && G.area() > 65536)
            I.dynamic = 2;
        const Rect grown = unite(I.guard, G);
        const bool guardGrew = !(grown == I.guard);
        I.guard = grown;
        if (I.dynamic != 0 || G.empty())
            return;
        if (guardGrew)
            captureTiles(I, I.guard, d);
        // tiles this draw reaches for the first time: who wrote their blocks
        for (int ty = G.y0 / static_cast<int>(kTile); ty <= G.y1 / static_cast<int>(kTile); ++ty)
            for (int tx = G.x0 / static_cast<int>(kTile); tx <= G.x1 / static_cast<int>(kTile); ++tx)
            {
                const size_t bit = static_cast<size_t>(ty) * kTilesPerSide + static_cast<size_t>(tx);
                if (I.reached.test(bit))
                    continue;
                I.reached.set(bit);
                const int x0 = std::max(tx * static_cast<int>(kTile), 0), y0 = std::max(ty * static_cast<int>(kTile), 0);
                const int x1 = std::min(tx * static_cast<int>(kTile) + static_cast<int>(kTile) - 1, static_cast<int>(I.w) - 1);
                const int y1 = std::min(ty * static_cast<int>(kTile) + static_cast<int>(kTile) - 1, static_cast<int>(I.h) - 1);
                collectWriters(I.psm, I.tbp, I.tbw, x0, y0, x1, y1, I.writers, I.cells, I.cellsKnown);
            }
    }

    // ---------------------------------------------------------------- keys
    // Raw values of the rectangle, row by row: one byte per texel (indices) or four (RGBA).
    static void gather(const Instance &I, const Rect &r, std::vector<uint8_t> &buf, std::bitset<256> &used)
    {
        const size_t bpp = I.indexed ? 1u : 4u;
        buf.assign(static_cast<size_t>(r.area()) * bpp, 0);
        size_t o = 0;
        for (int y = r.y0; y <= r.y1; ++y)
            for (int tx = r.x0 / static_cast<int>(kTile); tx <= r.x1 / static_cast<int>(kTile); ++tx)
            {
                const int xs = std::max(r.x0, tx * static_cast<int>(kTile)), xe = std::min(r.x1, tx * static_cast<int>(kTile) + static_cast<int>(kTile) - 1);
                const auto it = I.tiles.find(static_cast<uint32_t>(y / static_cast<int>(kTile)) * kTilesPerSide + static_cast<uint32_t>(tx));
                const size_t n = static_cast<size_t>(xe - xs + 1) * bpp;
                if (it != I.tiles.end())
                    std::memcpy(&buf[o], &it->second.data[(static_cast<size_t>(y % kTile) * kTile + static_cast<size_t>(xs % kTile)) * bpp], n);
                o += n;
            }
        if (I.indexed)
            for (uint8_t b : buf)
                used.set(b);
    }

    static uint64_t formatSeed(const Instance &I, int rw, int rh)
    {
        return combine(combine(I.psm, static_cast<uint64_t>(rw)), static_cast<uint64_t>(rh));
    }

    // The key with the colouring: the resolved CLUT entries the indices use.
    static uint64_t withColours(const Instance &I, uint64_t indexKey, const std::bitset<256> &used)
    {
        if (!I.indexed)
            return indexKey;
        std::vector<uint32_t> entries;
        for (uint32_t i = 0; i < 256u; ++i)
            if (used.test(i))
            {
                entries.push_back(i);
                entries.push_back(I.palette[i]);
            }
        return hash64(entries.data(), entries.size() * 4u, indexKey);
    }

    static uint64_t withColoursAll(const Instance &I, uint64_t indexKey)
    {
        return I.indexed ? hash64(I.palette, I.paletteCount * 4u, indexKey) : indexKey;
    }

    void computeKeys(const Instance &I, KeySet &k, uint32_t &usedEntries, std::bitset<256> &usedGuard)
    {
        k.k1 = I.k1;
        std::vector<uint8_t> buf;
        std::bitset<256> used;
        if (!I.guard.empty())
        {
            gather(I, I.guard, buf, used);
            k.k2b_i = hash64(buf.data(), buf.size(), formatSeed(I, I.guard.x1 - I.guard.x0 + 1, I.guard.y1 - I.guard.y0 + 1));
            k.k2b_c = withColours(I, k.k2b_i, used);
            usedGuard = used;
        }
        if (!I.tight.empty() && I.guard.contains(I.tight))
        {
            used.reset();
            gather(I, I.tight, buf, used);
            k.k2t_i = hash64(buf.data(), buf.size(), formatSeed(I, I.tight.x1 - I.tight.x0 + 1, I.tight.y1 - I.tight.y0 + 1));
            k.k2t_c = withColours(I, k.k2t_i, used);
        }
        // the 16 x 16 tiles some draw reaches, and the indices used inside them
        {
            used.reset();
            uint64_t h = combine(combine(I.psm, 0x7469ull), I.indexed ? 1u : 0u);
            for (size_t bit = 0; bit < I.reached.size(); ++bit)
            {
                if (!I.reached.test(bit))
                    continue;
                const auto it = I.tiles.find(static_cast<uint32_t>(bit));
                if (it == I.tiles.end())
                    continue;
                h = hash64(it->second.data.data(), it->second.data.size(), combine(h, bit));
                if (I.indexed)
                    markUsed(I, it->second, static_cast<uint32_t>(bit), used);
            }
            k.k2s_i = h;
            k.k2s_c = withColours(I, h, used);
            usedEntries = static_cast<uint32_t>(used.count());
            usedGuard = used;
        }
        // upload-based keys
        if (!I.writers.empty())
        {
            std::vector<WriterInfo> w = I.writers;
            std::sort(w.begin(), w.end(), [](const WriterInfo &a, const WriterInfo &b) { return a.entry < b.entry; });
            const uint64_t seed = combine(combine(I.psm, 0x3333ull), I.tbw);
            std::vector<uint64_t> words;
            for (const WriterInfo &x : w)
            {
                words.push_back(x.entry.payload);
                words.push_back((static_cast<uint64_t>(static_cast<uint32_t>(x.entry.rel)) << 32) | (static_cast<uint64_t>(x.entry.dsax) << 16) | x.entry.dsay);
            }
            k.k3_i = hash64(words.data(), words.size() * 8u, seed);
            k.k3_c = withColours(I, k.k3_i, usedGuard);
            const WriterInfo *main = &w[0];
            for (const WriterInfo &x : w)
                if (x.cells > main->cells)
                    main = &x;
            const uint64_t single[2] = {main->entry.payload,
                                        (static_cast<uint64_t>(static_cast<uint32_t>(main->entry.rel)) << 32) | (static_cast<uint64_t>(main->entry.dsax) << 16) | main->entry.dsay};
            k.k3m_i = hash64(single, sizeof(single), seed);
            k.k3m_c = withColours(I, k.k3m_i, usedGuard);
        }
    }

    // ---------------------------------------------------------------- rows
    static void markUsed(const Instance &I, const Tile &tile, uint32_t tileKey, std::bitset<256> &used)
    {
        const uint32_t tx = tileKey % kTilesPerSide, ty = tileKey / kTilesPerSide;
        for (uint32_t v = 0; v < kTile; ++v)
            for (uint32_t u = 0; u < kTile; ++u)
                if (tx * kTile + u < I.w && ty * kTile + v < I.h)
                    used.set(tile.data[v * kTile + u]);
    }

    static const WriterInfo *mainWriter(const Instance &I)
    {
        const WriterInfo *main = nullptr;
        for (const WriterInfo &x : I.writers)
            if (!main || x.cells > main->cells || (x.cells == main->cells && x.entry < main->entry))
                main = &x;
        return main;
    }

    std::string formatRow(uint64_t id, const Instance &I, const KeySet &k, uint32_t usedEntries) const
    {
        std::string s;
        appendf(s, "%llu,%s,%llu,%llu,%016llx,%u,%u,%u,%u,%u,%d,%u,%u,%u,%u,%016llx,%u,", static_cast<unsigned long long>(id), scene.c_str(),
                static_cast<unsigned long long>(I.firstMarker), static_cast<unsigned long long>(I.lastMarker), static_cast<unsigned long long>(I.k1), I.tbp,
                I.tbw, I.psm, I.w, I.h, I.indexed ? 1 : 0, I.cpsm, I.csm, I.csa, I.cbp, static_cast<unsigned long long>(I.clutHash), I.texa);
        appendf(s, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,", static_cast<unsigned long long>(I.draws), static_cast<unsigned long long>(I.sprites),
                static_cast<unsigned long long>(I.tris), static_cast<unsigned long long>(I.bilinear), static_cast<unsigned long long>(I.nearest),
                static_cast<unsigned long long>(I.fst), static_cast<unsigned long long>(I.stq), I.wmsMask, I.wmtMask);
        if (I.region)
            appendf(s, "%d:%d:%d:%d,", I.regionMin[0], I.regionMax[0], I.regionMin[1], I.regionMax[1]);
        else
            s += "-,";
        appendf(s, "%d,%d,%d,%d,%d,%d,%d,%d,%lld,%lld,%u,", I.guard.x0, I.guard.y0, I.guard.x1, I.guard.y1, I.tight.x0, I.tight.y0, I.tight.x1, I.tight.y1,
                static_cast<long long>(I.guard.area()), static_cast<long long>(I.tight.area()), static_cast<uint32_t>(I.reached.count()));
        appendf(s, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,", I.magU[0] > 1.0e29 ? 0.0 : I.magU[0], I.magU[1], I.magV[0] > 1.0e29 ? 0.0 : I.magV[0], I.magV[1],
                I.triDensity[0] > 1.0e29 ? 0.0 : I.triDensity[0], I.triDensity[1], I.dynamic);
        for (uint64_t key : {k.k2b_i, k.k2b_c, k.k2t_i, k.k2t_c, k.k2s_i, k.k2s_c, k.k3_i, k.k3_c, k.k3m_i, k.k3m_c})
            appendf(s, "%016llx,", static_cast<unsigned long long>(key));
        const WriterInfo *main = mainWriter(I);
        appendf(s, "%u,%u,%u,", static_cast<uint32_t>(I.writers.size()), I.cells, I.cellsKnown);
        if (main)
            appendf(s, "%016llx,%u,%u,%u,%u,%u,%d,%u,", static_cast<unsigned long long>(main->entry.payload), main->w, main->h, main->dpsm, main->dbw, main->dbp,
                    main->entry.rel, main->cells);
        else
            s += "0,0,0,0,0,0,0,0,";
        appendf(s, "%016llx,%u,%u,%u", static_cast<unsigned long long>(I.clutWriters.empty() ? 0ull : I.clutWriters[0].payload),
                static_cast<uint32_t>(I.clutWriters.size()), usedEntries, I.paletteCount);
        return s;
    }

    static uint64_t pickKey(const KeySet &k, const std::string &name)
    {
        if (name == "k1") return k.k1;
        if (name == "k2b_i") return k.k2b_i;
        if (name == "k2b_c") return k.k2b_c;
        if (name == "k2t_i") return k.k2t_i;
        if (name == "k2t_c") return k.k2t_c;
        if (name == "k2s_i") return k.k2s_i;
        if (name == "k2s_c") return k.k2s_c;
        if (name == "k3_i") return k.k3_i;
        if (name == "k3_c") return k.k3_c;
        if (name == "k3m_c") return k.k3m_c;
        return k.k3m_i;
    }

    void writeSidecars(uint64_t id, const Instance &I, const std::bitset<256> &usedGuard)
    {
        (void)usedGuard;
        if (tilesCsv && !I.tight.empty() && I.dynamic == 0)
        {
            // The tiles some draw reaches, hashed over the part of the tile inside the tight reach only: the tile around the edge of
            // an image also holds whatever GS memory held beyond it, which says nothing about the texture. `cov` is that part,
            // x0:y0:x1:y1 inside the tile; two rows can only be compared on a tile where their `cov` is the same.
            std::string s;
            for (int ty = I.tight.y0 / static_cast<int>(kTile); ty <= I.tight.y1 / static_cast<int>(kTile); ++ty)
                for (int tx = I.tight.x0 / static_cast<int>(kTile); tx <= I.tight.x1 / static_cast<int>(kTile); ++tx)
                {
                    const uint32_t key = static_cast<uint32_t>(ty) * kTilesPerSide + static_cast<uint32_t>(tx);
                    if (!I.reached.test(key))
                        continue;
                    const auto it = I.tiles.find(key);
                    if (it == I.tiles.end())
                        continue;
                    const Tile &t = it->second;
                    const int tileX = tx * static_cast<int>(kTile), tileY = ty * static_cast<int>(kTile);
                    const int cx0 = std::max(I.tight.x0, tileX) - tileX, cx1 = std::min(I.tight.x1, tileX + static_cast<int>(kTile) - 1) - tileX;
                    const int cy0 = std::max(I.tight.y0, tileY) - tileY, cy1 = std::min(I.tight.y1, tileY + static_cast<int>(kTile) - 1) - tileY;
                    std::vector<uint32_t> covered; // the texels as colours (for indexed textures: the indices go to `indices`)
                    std::vector<uint8_t> indices;
                    std::bitset<256> seenIndex;
                    std::unordered_set<uint32_t> seenColour;
                    for (int y = cy0; y <= cy1; ++y)
                        for (int x = cx0; x <= cx1; ++x)
                        {
                            if (I.indexed)
                            {
                                const uint8_t v = t.data[static_cast<size_t>(y) * kTile + static_cast<size_t>(x)];
                                indices.push_back(v);
                                covered.push_back(I.palette[v]);
                                seenIndex.set(v);
                            }
                            else
                            {
                                uint32_t px;
                                std::memcpy(&px, &t.data[(static_cast<size_t>(y) * kTile + static_cast<size_t>(x)) * 4u], 4);
                                covered.push_back(px);
                                if (seenColour.size() < 255u)
                                    seenColour.insert(px);
                            }
                        }
                    const uint64_t rgbaHash = hash64(covered.data(), covered.size() * 4u, 0);
                    const uint64_t rawHash = I.indexed ? hash64(indices.data(), indices.size(), 0) : rgbaHash;
                    const uint32_t distinct = I.indexed ? static_cast<uint32_t>(seenIndex.count()) : static_cast<uint32_t>(seenColour.size());
                    appendf(s, "%llu,%d,%d,%016llx,%016llx,%u,%d:%d:%d:%d\n", static_cast<unsigned long long>(id), tx, ty, static_cast<unsigned long long>(rawHash),
                            static_cast<unsigned long long>(rgbaHash), distinct, cx0, cy0, cx1, cy1);
                }
            std::fwrite(s.data(), 1, s.size(), tilesCsv);
        }
        if (palCsv && I.indexed && !I.tight.empty() && I.dynamic == 0)
        {
            // how many texels of the tight reach use each palette entry
            std::vector<uint64_t> hist(256, 0);
            for (int y = I.tight.y0; y <= I.tight.y1; ++y)
                for (int x = I.tight.x0; x <= I.tight.x1; ++x)
                {
                    const auto it = I.tiles.find(static_cast<uint32_t>(y / static_cast<int>(kTile)) * kTilesPerSide + static_cast<uint32_t>(x / static_cast<int>(kTile)));
                    if (it != I.tiles.end())
                        ++hist[it->second.data[static_cast<size_t>(y % kTile) * kTile + static_cast<size_t>(x % kTile)]];
                }
            std::string s;
            appendf(s, "%llu,%016llx,%u,", static_cast<unsigned long long>(id), static_cast<unsigned long long>(I.clutHash), I.paletteCount);
            for (uint32_t i = 0; i < I.paletteCount; ++i)
                appendf(s, "%s%08x", i ? " " : "", I.palette[i]);
            s += ",";
            for (uint32_t i = 0; i < I.paletteCount; ++i)
                appendf(s, "%s%llu", i ? " " : "", static_cast<unsigned long long>(hist[i]));
            s += "\n";
            std::fwrite(s.data(), 1, s.size(), palCsv);
        }
    }

    // ---------------------------------------------------------------- the dumper
    void buildImages(const Instance &I, const Rect &r, std::vector<uint8_t> &rgba, std::vector<uint8_t> &idx) const
    {
        const size_t w = static_cast<size_t>(r.x1 - r.x0 + 1), h = static_cast<size_t>(r.y1 - r.y0 + 1);
        rgba.assign(w * h * 4u, 0);
        if (I.indexed)
            idx.assign(w * h, 0);
        for (int y = r.y0; y <= r.y1; ++y)
            for (int x = r.x0; x <= r.x1; ++x)
            {
                const auto it = I.tiles.find(static_cast<uint32_t>(y / static_cast<int>(kTile)) * kTilesPerSide + static_cast<uint32_t>(x / static_cast<int>(kTile)));
                if (it == I.tiles.end())
                    continue;
                const size_t texel = static_cast<size_t>(y % kTile) * kTile + static_cast<size_t>(x % kTile);
                const size_t o = static_cast<size_t>(y - r.y0) * w + static_cast<size_t>(x - r.x0);
                if (I.indexed)
                {
                    const uint8_t index = it->second.data[texel];
                    idx[o] = index;
                    texelToRgba8(I.palette[index], &rgba[o * 4u]);
                }
                else
                {
                    uint32_t px;
                    std::memcpy(&px, &it->second.data[texel * 4u], 4);
                    texelToRgba8(px, &rgba[o * 4u]);
                }
            }
    }

    // Returns the file name written (without folder), or empty.
    std::string maybeDump(const Instance &I, const KeySet &k)
    {
        if (!dump || I.dynamic != 0 || I.guard.empty() || dumpCount >= dumpMax)
            return {};
        uint64_t key = pickKey(k, dumpKeyName);
        if (key == 0)
            key = k.k2b_i;
        if (key == 0)
            return {};
        const uint32_t area = static_cast<uint32_t>(std::min<int64_t>(I.guard.area(), 0x7FFFFFFF));
        char name[64];
        std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(key));
        auto found = dumpedArea.find(key);
        const bool first = found == dumpedArea.end();
        std::vector<uint8_t> rgba, idx;
        const uint32_t w = static_cast<uint32_t>(I.guard.x1 - I.guard.x0 + 1), h = static_cast<uint32_t>(I.guard.y1 - I.guard.y0 + 1);
        if (first || area > found->second)
        {
            buildImages(I, I.guard, rgba, idx);
            PngJob colour;
            colour.path = dumpDir + "/" + name + ".png";
            colour.w = w;
            colour.h = h;
            colour.colorType = 6;
            colour.pixels = rgba;
            enqueue(std::move(colour));
            if (I.indexed)
            {
                PngJob index;
                index.path = dumpDir + "/" + name + "_idx.png";
                index.w = w;
                index.h = h;
                index.colorType = 3;
                index.pixels = idx;
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    uint8_t px[4];
                    texelToRgba8(I.palette[i], px);
                    index.plte.insert(index.plte.end(), {px[0], px[1], px[2]});
                    index.trns.push_back(px[3]);
                }
                enqueue(std::move(index));
                PngJob pal;
                pal.path = dumpDir + "/" + name + "_pal.png";
                pal.w = 16u * 8u;
                pal.h = (I.paletteCount / 16u) * 8u;
                pal.colorType = 6;
                pal.pixels.assign(static_cast<size_t>(pal.w) * pal.h * 4u, 0);
                for (uint32_t i = 0; i < I.paletteCount; ++i)
                {
                    uint8_t px[4];
                    texelToRgba8(I.palette[i], px);
                    for (uint32_t yy = 0; yy < 8u; ++yy)
                        for (uint32_t xx = 0; xx < 8u; ++xx)
                            std::memcpy(&pal.pixels[((static_cast<size_t>(i / 16u) * 8u + yy) * pal.w + (i % 16u) * 8u + xx) * 4u], px, 4);
                }
                enqueue(std::move(pal));
                dumpedPalettes[key].insert(I.clutHash);
            }
            dumpedArea[key] = area;
            ++dumpCount;
            return std::string(name) + ".png";
        }
        // the same key, another colouring of an indexed texture: keep the coloured picture (up to eight per key)
        if (I.indexed && dumpedPalettes[key].size() < 8 && !dumpedPalettes[key].count(I.clutHash))
        {
            dumpedPalettes[key].insert(I.clutHash);
            buildImages(I, I.guard, rgba, idx);
            PngJob colour;
            char variant[96];
            std::snprintf(variant, sizeof(variant), "%s__%08llx.png", name, static_cast<unsigned long long>(I.clutHash & 0xFFFFFFFFull));
            colour.path = dumpDir + "/" + variant;
            colour.w = w;
            colour.h = h;
            colour.colorType = 6;
            colour.pixels = std::move(rgba);
            enqueue(std::move(colour));
            ++dumpCount;
            return variant;
        }
        return {};
    }

    // ---------------------------------------------------------------- scenes
    void writeUploads()
    {
        std::FILE *f = std::fopen((censusPath + ".uploads.csv").c_str(), "w");
        if (!f)
            return;
        std::fprintf(f, "payload,uploads,bytes,dpsm,dbw,rrw,rrh,distinct_dbp,first_dbp,first_marker,last_marker,first_scene\n");
        for (const auto &e : uploads)
        {
            const UploadAgg &a = e.second;
            std::fprintf(f, "%016llx,%llu,%llu,%u,%u,%u,%u,%zu,%u,%llu,%llu,%s\n", static_cast<unsigned long long>(e.first), static_cast<unsigned long long>(a.count),
                         static_cast<unsigned long long>(a.bytes), a.dpsm, a.dbw, a.w, a.h, a.dbps.size(), a.firstDbp, static_cast<unsigned long long>(a.firstMarker),
                         static_cast<unsigned long long>(a.lastMarker), a.firstScene.c_str());
        }
        std::fclose(f);
    }

    void closeScene(bool final = false)
    {
        const uint64_t tClose = nowNs();
        std::vector<Instance *> list;
        list.reserve(instances.size());
        for (auto &e : instances)
            list.push_back(e.second.get());
        std::sort(list.begin(), list.end(), [](const Instance *a, const Instance *b) { return a->firstMarker != b->firstMarker ? a->firstMarker < b->firstMarker : a->k1 < b->k1; });
        for (Instance *p : list)
        {
            const Instance &I = *p;
            KeySet keys;
            keys.k1 = I.k1;
            uint32_t usedEntries = 0;
            std::bitset<256> usedGuard;
            if (I.dynamic == 0)
                computeKeys(I, keys, usedEntries, usedGuard);
            const uint64_t id = rowCounter++;
            const std::string row = formatRow(id, I, keys, usedEntries);
            if (csv)
                std::fprintf(csv, "%s\n", row.c_str());
            writeSidecars(id, I, usedGuard);
            const std::string file = maybeDump(I, keys);
            if (!file.empty() && texCsv)
                std::fprintf(texCsv, "%s,%s\n", file.c_str(), row.c_str());
        }
        instances.clear();
        slotCounts.clear();
        ++scenesClosed;
        for (std::FILE *f : {csv, tilesCsv, palCsv, texCsv})
            if (f)
                std::fflush(f);
        if (final)
            writeUploads();
        if (verify)
            std::fprintf(stderr,
                         "[tex:census] verify: %llu chunks, %llu blocks predicted, %llu blocks changed, %llu changed blocks outside the prediction (%.4f%%)\n",
                         static_cast<unsigned long long>(verifyChunks), static_cast<unsigned long long>(verifyPredicted),
                         static_cast<unsigned long long>(verifyChangedBlocks), static_cast<unsigned long long>(verifyViolations),
                         verifyChangedBlocks ? 100.0 * static_cast<double>(verifyViolations) / static_cast<double>(verifyChangedBlocks) : 0.0);
        costCloseNs += nowNs() - tClose;
        std::fprintf(stderr, "[tex:census] scene %s closed: %zu rows so far, %zu upload payloads, census cost %.1f ms in %llu draw calls; closing this scene took %.1f ms\n",
                     scene.c_str(), static_cast<size_t>(rowCounter), uploads.size(), static_cast<double>(costNs) / 1.0e6, static_cast<unsigned long long>(drawCalls),
                     static_cast<double>(nowNs() - tClose) / 1.0e6);
    }

    void markDone()
    {
        const std::string path = censusPath + ".done";
        if (worker.joinable())
        {
            PngJob job;
            job.path = path;
            job.markDone = true;
            enqueue(std::move(job));
        }
        else if (std::FILE *f = std::fopen(path.c_str(), "wb"))
            std::fclose(f);
    }

    void pollScene(uint64_t marker)
    {
        (void)marker;
        std::FILE *f = std::fopen(scenePath.c_str(), "rb");
        if (!f)
            return;
        char line[64] = {};
        const size_t n = std::fread(line, 1, sizeof(line) - 1, f);
        std::fclose(f);
        std::string name(line, n);
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' '))
            name.pop_back();
        for (char &c : name)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
                c = '_';
        if (name.empty() || name == scene)
            return;
        closeScene();
        scene = name;
        if (scene == "end")
        {
            closeScene(true);
            markDone();
        }
    }
};

// ------------------------------------------------------------------------------------------------
Census::Census() : m(std::make_unique<Impl>()) {}

Census::~Census()
{
    Finish();
}

std::unique_ptr<Census> Census::FromEnvironment()
{
    const char *censusEnv = std::getenv("PS2X_TEX_CENSUS");
    const char *dumpEnv = std::getenv("PS2X_TEX_DUMP");
    const bool haveCensus = censusEnv && censusEnv[0] != '\0';
    const bool haveDump = dumpEnv && dumpEnv[0] != '\0';
    if (!haveCensus && !haveDump)
        return nullptr;
    std::unique_ptr<Census> c(new Census());
    Impl &m = *c->m;
    m.dump = haveDump;
    if (haveDump)
        m.dumpDir = dumpEnv;
    while (!m.dumpDir.empty() && (m.dumpDir.back() == '/' || m.dumpDir.back() == '\\'))
        m.dumpDir.pop_back();
    m.censusPath = haveCensus ? std::string(censusEnv) : m.dumpDir + "/census.csv";
    m.scenePath = m.censusPath + ".scene";
    if (const char *key = std::getenv("PS2X_TEX_DUMP_KEY"))
        if (key[0] != '\0')
            m.dumpKeyName = key;
    if (const char *max = std::getenv("PS2X_TEX_DUMP_MAX"))
        if (std::atol(max) > 0)
            m.dumpMax = static_cast<uint32_t>(std::atol(max));
    const char *stats = std::getenv("PS2X_TEX_CENSUS_STATS");
    m.statsOn = stats && stats[0] == '1';
    const char *verify = std::getenv("PS2X_TEX_CENSUS_VERIFY");
    m.verify = verify && verify[0] == '1';
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m.censusPath).parent_path(), ec);
    if (m.dump)
        std::filesystem::create_directories(m.dumpDir, ec);
    std::filesystem::remove(m.censusPath + ".done", ec);
    m.csv = std::fopen(m.censusPath.c_str(), "w");
    m.tilesCsv = std::fopen((m.censusPath + ".tiles.csv").c_str(), "w");
    m.palCsv = std::fopen((m.censusPath + ".palettes.csv").c_str(), "w");
    if (!m.csv)
    {
        std::fprintf(stderr, "[tex:census] cannot write %s\n", m.censusPath.c_str());
        return nullptr;
    }
    std::fprintf(m.csv, "%s\n", Impl::header());
    if (m.tilesCsv)
        std::fprintf(m.tilesCsv, "row,tx,ty,raw_hash,rgba_hash,distinct,cov\n");
    if (m.palCsv)
        std::fprintf(m.palCsv, "row,clut_hash,n,palette,hist\n");
    if (m.dump)
    {
        m.texCsv = std::fopen((m.dumpDir + "/textures.csv").c_str(), "w");
        if (m.texCsv)
            std::fprintf(m.texCsv, "file,%s\n", Impl::header());
        m.startWorker();
    }
    std::fprintf(stderr, "[tex:census] on: census %s%s%s, scene file %s%s\n", m.censusPath.c_str(), m.dump ? ", dump " : "", m.dump ? m.dumpDir.c_str() : "",
                 m.scenePath.c_str(), m.verify ? ", verifying the block map" : "");
    return c;
}

void Census::BeginTransfer(const GSTransferCommand &command)
{
    if (command.direction != 0u)
        return;
    const uint64_t t0 = nowNs();
    m->beginTransfer(command, m->curMarker);
    m->costBeginNs += nowNs() - t0;
    m->costNs += nowNs() - t0;
}

void Census::BeforeUpload(const uint8_t *vram)
{
    Impl &s = *m;
    if (!s.verify || s.cur < 0 || !vram)
        return;
    const UploadRec &r = s.recs[static_cast<size_t>(s.cur)];
    s.beforeSpan = GSGpu::BufferPageSpan(r.dpsm, r.dbp, r.dbw, r.x, r.y, r.w, r.h);
    s.beforePages.resize(static_cast<size_t>(s.beforeSpan.count) * 8192u);
    for (uint32_t i = 0; i < s.beforeSpan.count; ++i)
        std::memcpy(&s.beforePages[static_cast<size_t>(i) * 8192u], vram + static_cast<size_t>((s.beforeSpan.first + i) & 511u) * 8192u, 8192u);
}

void Census::AfterUpload(const uint8_t *vram, const uint8_t *data, uint32_t bytes)
{
    Impl &s = *m;
    if (s.cur < 0)
        return;
    const uint64_t t0 = nowNs();
    UploadRec &r = s.recs[static_cast<size_t>(s.cur)];
    r.stream.update(data, bytes);
    r.bytes += bytes;
    ++s.uploadCalls;
    if (s.verify && vram && !s.beforePages.empty())
    {
        ++s.verifyChunks;
        for (uint32_t i = 0; i < s.beforeSpan.count; ++i)
        {
            const uint32_t page = (s.beforeSpan.first + i) & 511u;
            for (uint32_t b = 0; b < 32u; ++b)
            {
                const uint32_t block = page * 32u + b;
                if (std::memcmp(&s.beforePages[static_cast<size_t>(i) * 8192u + b * 256u], vram + static_cast<size_t>(block) * 256u, 256u) != 0)
                {
                    if (!s.changedSeen[block])
                    {
                        s.changedSeen[block] = 1;
                        ++s.verifyChangedBlocks;
                        if (!s.predicted[block])
                            ++s.verifyViolations;
                    }
                }
            }
        }
        s.beforePages.clear();
    }
    s.costAfterNs += nowNs() - t0;
    s.costNs += nowNs() - t0;
}

void Census::MemoryReset()
{
    Impl &s = *m;
    std::fill(s.blockWriter.begin(), s.blockWriter.end(), 0u);
    s.recs.clear();
    s.freeRecs.clear();
    s.cur = -1;
}

void Census::Draw(const DrawInfo &draw)
{
    const uint64_t t0 = nowNs();
    m->curMarker = draw.marker;
    m->draw(draw);
    ++m->drawCalls;
    m->costNs += nowNs() - t0;
}

void Census::Marker(uint64_t marker)
{
    Impl &s = *m;
    s.curMarker = marker;
    const uint64_t t0 = nowNs();
    if ((marker & 7u) == 0u)
    {
        const uint64_t tPoll = nowNs();
        s.pollScene(marker);
        s.costPollNs += nowNs() - tPoll;
    }
    if (s.statsOn && marker - s.lastStatsMarker >= 600u)
    {
        s.lastStatsMarker = marker;
        std::fprintf(stderr, "[tex:census] marker %llu: %zu instances live, %llu rows, cost %.1f ms (%llu draws, %llu uploads), png queue %.1f MB, written %llu\n",
                     static_cast<unsigned long long>(marker), s.instances.size(), static_cast<unsigned long long>(s.rowCounter), static_cast<double>(s.costNs) / 1.0e6,
                     static_cast<unsigned long long>(s.drawCalls), static_cast<unsigned long long>(s.uploadCalls), static_cast<double>(s.queuedBytes) / 1048576.0,
                     static_cast<unsigned long long>(s.pngWritten));
        std::fprintf(stderr,
                     "[tex:census]   cost split ms: transfers begin %.1f, upload data %.1f, new instances %.1f (%llu), growth %.1f (%llu growths, %llu tiles read), scene poll %.1f, scene closes %.1f\n",
                     static_cast<double>(s.costBeginNs) / 1.0e6, static_cast<double>(s.costAfterNs) / 1.0e6, static_cast<double>(s.costCreateNs) / 1.0e6,
                     static_cast<unsigned long long>(s.instancesCreated), static_cast<double>(s.costGrowNs) / 1.0e6, static_cast<unsigned long long>(s.growths),
                     static_cast<unsigned long long>(s.tilesCaptured), static_cast<double>(s.costPollNs) / 1.0e6, static_cast<double>(s.costCloseNs) / 1.0e6);
    }
    s.costNs += nowNs() - t0;
}

void Census::Finish()
{
    Impl &s = *m;
    if (s.finished)
        return;
    s.finished = true;
    s.closeScene(true);
    s.markDone();
    s.stopWorker();
    for (std::FILE *f : {s.csv, s.tilesCsv, s.palCsv, s.texCsv})
        if (f)
            std::fflush(f);
}
} // namespace TexCensus
