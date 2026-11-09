// A small PNG codec (inflate, fixed-Huffman deflate, chunk and filter handling). See include/runtime/gs/gs_png.h.

#include "runtime/gs/gs_png.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace GSPng
{
namespace
{
    // ---------------------------------------------------------------------------------------------------------------
    // CRC and Adler checksums
    const uint32_t *crcTable()
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
        return table;
    }

    uint32_t adler32(const uint8_t *data, size_t size)
    {
        uint32_t a = 1, b = 0;
        for (size_t k = 0; k < size;)
        {
            const size_t end = std::min(size, k + 5552);
            for (; k < end; ++k)
            {
                a += data[k];
                b += a;
            }
            a %= 65521u;
            b %= 65521u;
        }
        return (b << 16) | a;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Inflate. The structure follows puff.c (Mark Adler, public domain): canonical Huffman decoding by code length counts.
    struct BitReader
    {
        const uint8_t *p = nullptr;
        size_t n = 0, pos = 0;
        uint64_t buf = 0;
        int cnt = 0;
        bool eof = false;

        uint32_t bits(int need)
        {
            uint64_t val = buf;
            while (cnt < need)
            {
                if (pos >= n)
                {
                    eof = true;
                    return 0;
                }
                val |= static_cast<uint64_t>(p[pos++]) << cnt;
                cnt += 8;
            }
            buf = val >> need;
            cnt -= need;
            return static_cast<uint32_t>(val & ((1ull << need) - 1u));
        }
    };

    struct Huffman
    {
        uint16_t count[16];
        uint16_t symbol[288];
    };

    // Returns 0 for a complete code, > 0 for an incomplete one, < 0 for an over-subscribed one.
    int construct(Huffman &h, const uint16_t *length, int n)
    {
        for (int len = 0; len <= 15; ++len)
            h.count[len] = 0;
        for (int symbol = 0; symbol < n; ++symbol)
            ++h.count[length[symbol]];
        if (h.count[0] == n)
            return 0;
        int left = 1;
        for (int len = 1; len <= 15; ++len)
        {
            left <<= 1;
            left -= h.count[len];
            if (left < 0)
                return left;
        }
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len)
            offs[len + 1] = static_cast<uint16_t>(offs[len] + h.count[len]);
        for (int symbol = 0; symbol < n; ++symbol)
            if (length[symbol] != 0)
                h.symbol[offs[length[symbol]]++] = static_cast<uint16_t>(symbol);
        return left;
    }

    int decodeSymbol(BitReader &s, const Huffman &h)
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len)
        {
            code |= static_cast<int>(s.bits(1));
            if (s.eof)
                return -1;
            const int count = h.count[len];
            if (code - count < first)
                return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -10;
    }

    const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    const uint16_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    const uint16_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    // Decodes the symbols of one block. Returns nullptr when fine, else the reason.
    const char *inflateCodes(BitReader &s, std::vector<uint8_t> &out, size_t maxOut, const Huffman &lencode, const Huffman &distcode)
    {
        for (;;)
        {
            int symbol = decodeSymbol(s, lencode);
            if (symbol < 0)
                return s.eof ? "the compressed data ends too soon" : "the compressed data is damaged (bad code)";
            if (symbol < 256)
            {
                if (out.size() >= maxOut)
                    return "the image holds more data than its header says";
                out.push_back(static_cast<uint8_t>(symbol));
            }
            else if (symbol == 256)
            {
                return nullptr;
            }
            else
            {
                symbol -= 257;
                if (symbol >= 29)
                    return "the compressed data is damaged (bad length)";
                const size_t len = kLenBase[symbol] + s.bits(kLenExtra[symbol]);
                const int dsym = decodeSymbol(s, distcode);
                if (dsym < 0 || dsym >= 30)
                    return s.eof ? "the compressed data ends too soon" : "the compressed data is damaged (bad distance)";
                const size_t dist = kDistBase[dsym] + s.bits(kDistExtra[dsym]);
                if (s.eof)
                    return "the compressed data ends too soon";
                if (dist > out.size())
                    return "the compressed data is damaged (distance too far back)";
                if (out.size() + len > maxOut)
                    return "the image holds more data than its header says";
                const size_t from = out.size() - dist;
                for (size_t i = 0; i < len; ++i)
                    out.push_back(out[from + i]);
            }
        }
    }

    const Huffman &fixedLengths()
    {
        static Huffman h;
        static std::once_flag once;
        std::call_once(once,
                       []
                       {
                           uint16_t lengths[288];
                           int symbol = 0;
                           for (; symbol < 144; ++symbol)
                               lengths[symbol] = 8;
                           for (; symbol < 256; ++symbol)
                               lengths[symbol] = 9;
                           for (; symbol < 280; ++symbol)
                               lengths[symbol] = 7;
                           for (; symbol < 288; ++symbol)
                               lengths[symbol] = 8;
                           construct(h, lengths, 288);
                       });
        return h;
    }

    const Huffman &fixedDistances()
    {
        static Huffman h;
        static std::once_flag once;
        std::call_once(once,
                       []
                       {
                           uint16_t lengths[30];
                           for (int symbol = 0; symbol < 30; ++symbol)
                               lengths[symbol] = 5;
                           construct(h, lengths, 30);
                       });
        return h;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // PNG chunks
    uint32_t be32(const uint8_t *p)
    {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
    }

    struct Parsed
    {
        Info info;
        std::vector<uint8_t> idat, plte, trns;
    };

    bool validDepth(uint8_t colorType, uint8_t depth)
    {
        switch (colorType)
        {
        case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
        case 2: return depth == 8 || depth == 16;
        case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
        case 4: return depth == 8 || depth == 16;
        case 6: return depth == 8 || depth == 16;
        default: return false;
        }
    }

    bool parseChunks(const uint8_t *d, size_t n, Parsed &out, bool wantData, std::string &error)
    {
        static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        if (n < 8 || std::memcmp(d, signature, 8) != 0)
        {
            error = "this is not a PNG file";
            return false;
        }
        size_t pos = 8;
        bool haveHeader = false, sawEnd = false, sawIdat = false;
        while (pos + 12 <= n && !sawEnd)
        {
            const uint32_t len = be32(d + pos);
            if (len > n - pos - 12u)
            {
                error = "the PNG file is cut short";
                return false;
            }
            const uint8_t *tag = d + pos + 4;
            const uint8_t *body = d + pos + 8;
            const uint32_t crc = be32(body + len);
            if (Crc32(tag, static_cast<size_t>(len) + 4u) != crc)
            {
                error = "the PNG file is damaged (a chunk fails its checksum)";
                return false;
            }
            if (std::memcmp(tag, "IHDR", 4) == 0)
            {
                if (haveHeader || len != 13)
                {
                    error = "the PNG header is not valid";
                    return false;
                }
                out.info.width = be32(body);
                out.info.height = be32(body + 4);
                out.info.bitDepth = body[8];
                out.info.colorType = body[9];
                if (body[10] != 0 || body[11] != 0 || body[12] > 1)
                {
                    error = "the PNG header is not valid";
                    return false;
                }
                out.info.interlaced = body[12] == 1;
                if (!validDepth(out.info.colorType, out.info.bitDepth))
                {
                    error = "the PNG has a colour type and bit depth that PNG does not allow";
                    return false;
                }
                if (out.info.width == 0 || out.info.height == 0 || out.info.width > 16384u || out.info.height > 16384u ||
                    static_cast<uint64_t>(out.info.width) * out.info.height > (64ull << 20))
                {
                    error = "the PNG is empty or larger than 16384 x 16384 pixels";
                    return false;
                }
                haveHeader = true;
            }
            else if (!haveHeader)
            {
                error = "the PNG header is missing";
                return false;
            }
            else if (std::memcmp(tag, "PLTE", 4) == 0)
            {
                if (len % 3u != 0 || len == 0 || len > 768u)
                {
                    error = "the PNG palette is not valid";
                    return false;
                }
                out.plte.assign(body, body + len);
            }
            else if (std::memcmp(tag, "tRNS", 4) == 0)
            {
                out.trns.assign(body, body + len);
            }
            else if (std::memcmp(tag, "IDAT", 4) == 0)
            {
                sawIdat = true;
                if (wantData)
                    out.idat.insert(out.idat.end(), body, body + len);
            }
            else if (std::memcmp(tag, "IEND", 4) == 0)
            {
                sawEnd = true;
            }
            pos += 12u + len;
            if (!wantData && haveHeader && sawIdat)
                break; // the header is all the caller wanted
        }
        if (!haveHeader)
        {
            error = "the PNG header is missing";
            return false;
        }
        if (wantData && !sawIdat)
        {
            error = "the PNG has no image data";
            return false;
        }
        return true;
    }

    int channelsOf(uint8_t colorType)
    {
        switch (colorType)
        {
        case 0: return 1;
        case 2: return 3;
        case 3: return 1;
        case 4: return 2;
        default: return 4;
        }
    }

    // zlib + inflate + unfilter. On success `rows` holds height * rowBytes bytes (no filter bytes).
    bool unpackRows(const Parsed &parsed, std::vector<uint8_t> &rows, size_t &rowBytes, std::string &error)
    {
        const Info &info = parsed.info;
        if (info.interlaced)
        {
            error = "interlaced PNGs are not supported; save the picture without interlacing";
            return false;
        }
        const size_t bitsPerPixel = static_cast<size_t>(channelsOf(info.colorType)) * info.bitDepth;
        rowBytes = (static_cast<size_t>(info.width) * bitsPerPixel + 7u) / 8u;
        const size_t bpp = std::max<size_t>(1u, bitsPerPixel / 8u);
        const size_t expected = (rowBytes + 1u) * info.height;
        const std::vector<uint8_t> &z = parsed.idat;
        if (z.size() < 6 || (z[0] & 0x0F) != 8 || ((static_cast<uint32_t>(z[0]) << 8) | z[1]) % 31u != 0 || (z[1] & 0x20) != 0)
        {
            error = "the PNG image data is not a normal zlib stream";
            return false;
        }
        std::vector<uint8_t> raw;
        raw.reserve(expected);
        size_t consumed = 0;
        std::string inflateError;
        if (!Inflate(z.data() + 2, z.size() - 2, raw, expected, inflateError, &consumed))
        {
            error = "the PNG image data cannot be read: " + inflateError;
            return false;
        }
        if (raw.size() != expected)
        {
            error = "the PNG image data is shorter than its header says";
            return false;
        }
        if (2 + consumed + 4 <= z.size() && be32(z.data() + 2 + consumed) != adler32(raw.data(), raw.size()))
        {
            error = "the PNG image data is damaged (its checksum fails)";
            return false;
        }
        rows.resize(rowBytes * info.height);
        for (uint32_t y = 0; y < info.height; ++y)
        {
            const uint8_t filter = raw[y * (rowBytes + 1u)];
            const uint8_t *src = &raw[y * (rowBytes + 1u) + 1u];
            uint8_t *dst = &rows[y * rowBytes];
            const uint8_t *up = y ? &rows[(y - 1u) * rowBytes] : nullptr;
            switch (filter)
            {
            case 0:
                std::memcpy(dst, src, rowBytes);
                break;
            case 1:
                for (size_t x = 0; x < rowBytes; ++x)
                    dst[x] = static_cast<uint8_t>(src[x] + (x >= bpp ? dst[x - bpp] : 0u));
                break;
            case 2:
                for (size_t x = 0; x < rowBytes; ++x)
                    dst[x] = static_cast<uint8_t>(src[x] + (up ? up[x] : 0u));
                break;
            case 3:
                for (size_t x = 0; x < rowBytes; ++x)
                {
                    const unsigned left = x >= bpp ? dst[x - bpp] : 0u;
                    const unsigned above = up ? up[x] : 0u;
                    dst[x] = static_cast<uint8_t>(src[x] + ((left + above) >> 1));
                }
                break;
            case 4:
                for (size_t x = 0; x < rowBytes; ++x)
                {
                    const int a = x >= bpp ? dst[x - bpp] : 0;
                    const int b = up ? up[x] : 0;
                    const int c = (x >= bpp && up) ? up[x - bpp] : 0;
                    const int p = a + b - c;
                    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                    const int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    dst[x] = static_cast<uint8_t>(src[x] + pred);
                }
                break;
            default:
                error = "the PNG image data is damaged (unknown row filter)";
                return false;
            }
        }
        return true;
    }

    // The raw sample at position x of a row of 1, 2, 4 or 8 bit samples.
    inline unsigned sampleAt(const uint8_t *row, uint32_t x, unsigned depth)
    {
        if (depth == 8)
            return row[x];
        const unsigned perByte = 8u / depth;
        const unsigned shift = 8u - depth * (1u + x % perByte);
        return (row[x / perByte] >> shift) & ((1u << depth) - 1u);
    }
}

// -----------------------------------------------------------------------------------------------------------------
uint32_t Crc32(const uint8_t *data, size_t size, uint32_t crc)
{
    const uint32_t *table = crcTable();
    crc = ~crc;
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

bool Inflate(const uint8_t *data, size_t size, std::vector<uint8_t> &out, size_t maxOut, std::string &error, size_t *consumed)
{
    BitReader s;
    s.p = data;
    s.n = size;
    bool last;
    do
    {
        last = s.bits(1) != 0;
        const uint32_t type = s.bits(2);
        if (s.eof)
        {
            error = "the compressed data ends too soon";
            return false;
        }
        const char *problem = nullptr;
        if (type == 0)
        {
            s.buf = 0;
            s.cnt = 0;
            if (s.pos + 4 > s.n)
            {
                error = "the compressed data ends too soon";
                return false;
            }
            const unsigned len = s.p[s.pos] | (s.p[s.pos + 1] << 8);
            const unsigned nlen = s.p[s.pos + 2] | (s.p[s.pos + 3] << 8);
            s.pos += 4;
            if (len != (~nlen & 0xFFFFu))
            {
                error = "the compressed data is damaged (stored block)";
                return false;
            }
            if (s.pos + len > s.n)
            {
                error = "the compressed data ends too soon";
                return false;
            }
            if (out.size() + len > maxOut)
            {
                error = "the image holds more data than its header says";
                return false;
            }
            out.insert(out.end(), s.p + s.pos, s.p + s.pos + len);
            s.pos += len;
        }
        else if (type == 1)
        {
            problem = inflateCodes(s, out, maxOut, fixedLengths(), fixedDistances());
        }
        else if (type == 2)
        {
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            const int nlen = static_cast<int>(s.bits(5)) + 257;
            const int ndist = static_cast<int>(s.bits(5)) + 1;
            const int ncode = static_cast<int>(s.bits(4)) + 4;
            if (s.eof || nlen > 286 || ndist > 30)
            {
                error = s.eof ? "the compressed data ends too soon" : "the compressed data is damaged (bad block header)";
                return false;
            }
            uint16_t lengths[320] = {};
            for (int i = 0; i < ncode; ++i)
                lengths[order[i]] = static_cast<uint16_t>(s.bits(3));
            Huffman lencode, distcode;
            if (construct(lencode, lengths, 19) != 0)
            {
                error = "the compressed data is damaged (bad code lengths)";
                return false;
            }
            int index = 0;
            while (index < nlen + ndist)
            {
                int symbol = decodeSymbol(s, lencode);
                if (symbol < 0)
                {
                    error = s.eof ? "the compressed data ends too soon" : "the compressed data is damaged (bad code)";
                    return false;
                }
                if (symbol < 16)
                {
                    lengths[index++] = static_cast<uint16_t>(symbol);
                }
                else
                {
                    uint16_t len = 0;
                    int repeat;
                    if (symbol == 16)
                    {
                        if (index == 0)
                        {
                            error = "the compressed data is damaged (repeat with nothing before)";
                            return false;
                        }
                        len = lengths[index - 1];
                        repeat = 3 + static_cast<int>(s.bits(2));
                    }
                    else if (symbol == 17)
                    {
                        repeat = 3 + static_cast<int>(s.bits(3));
                    }
                    else
                    {
                        repeat = 11 + static_cast<int>(s.bits(7));
                    }
                    if (index + repeat > nlen + ndist)
                    {
                        error = "the compressed data is damaged (too many code lengths)";
                        return false;
                    }
                    while (repeat--)
                        lengths[index++] = len;
                }
            }
            if (s.eof)
            {
                error = "the compressed data ends too soon";
                return false;
            }
            if (lengths[256] == 0)
            {
                error = "the compressed data is damaged (no end-of-block code)";
                return false;
            }
            int err = construct(lencode, lengths, nlen);
            if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
            {
                error = "the compressed data is damaged (bad literal codes)";
                return false;
            }
            err = construct(distcode, lengths + nlen, ndist);
            if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
            {
                error = "the compressed data is damaged (bad distance codes)";
                return false;
            }
            problem = inflateCodes(s, out, maxOut, lencode, distcode);
        }
        else
        {
            error = "the compressed data is damaged (bad block type)";
            return false;
        }
        if (problem)
        {
            error = problem;
            return false;
        }
    } while (!last);
    if (consumed)
        *consumed = s.pos;
    return true;
}

// -----------------------------------------------------------------------------------------------------------------
bool ReadInfo(const uint8_t *data, size_t size, Info &out, std::string &error)
{
    Parsed parsed;
    if (!parseChunks(data, size, parsed, false, error))
        return false;
    out = parsed.info;
    return true;
}

bool DecodeRgba(const uint8_t *data, size_t size, Image &out, std::string &error)
{
    Parsed parsed;
    if (!parseChunks(data, size, parsed, true, error))
        return false;
    std::vector<uint8_t> rows;
    size_t rowBytes = 0;
    if (!unpackRows(parsed, rows, rowBytes, error))
        return false;
    const Info &info = parsed.info;
    out.width = info.width;
    out.height = info.height;
    out.format = Image::Format::Rgba8;
    out.wasPaletted = info.colorType == 3;
    out.pixels.assign(static_cast<size_t>(info.width) * info.height * 4u, 0);
    const unsigned depth = info.bitDepth;
    const unsigned maxSample = (1u << (depth < 16 ? depth : 8)) - 1u;
    // tRNS colour keys (grey and RGB): the raw sample values of the transparent colour.
    int keyGrey = -1, keyRgb[3] = {-1, -1, -1};
    if (info.colorType == 0 && parsed.trns.size() >= 2)
        keyGrey = (parsed.trns[0] << 8) | parsed.trns[1];
    if (info.colorType == 2 && parsed.trns.size() >= 6)
        for (int c = 0; c < 3; ++c)
            keyRgb[c] = (parsed.trns[c * 2] << 8) | parsed.trns[c * 2 + 1];
    if (info.colorType == 3 && parsed.plte.empty())
    {
        error = "the PNG is paletted but has no palette";
        return false;
    }
    const size_t paletteCount = parsed.plte.size() / 3u;
    for (uint32_t y = 0; y < info.height; ++y)
    {
        const uint8_t *row = &rows[y * rowBytes];
        uint8_t *dst = &out.pixels[static_cast<size_t>(y) * info.width * 4u];
        for (uint32_t x = 0; x < info.width; ++x, dst += 4)
        {
            switch (info.colorType)
            {
            case 0:
            {
                unsigned v;
                unsigned raw;
                if (depth == 16)
                {
                    raw = (row[x * 2] << 8) | row[x * 2 + 1];
                    v = row[x * 2];
                }
                else
                {
                    raw = sampleAt(row, x, depth);
                    v = depth == 8 ? raw : raw * 255u / maxSample;
                }
                dst[0] = dst[1] = dst[2] = static_cast<uint8_t>(v);
                dst[3] = (keyGrey >= 0 && static_cast<int>(raw) == keyGrey) ? 0 : 255;
                break;
            }
            case 2:
            {
                unsigned c[3];
                int match = 0;
                for (int k = 0; k < 3; ++k)
                {
                    if (depth == 16)
                    {
                        const unsigned raw = (row[(x * 3 + k) * 2] << 8) | row[(x * 3 + k) * 2 + 1];
                        c[k] = row[(x * 3 + k) * 2];
                        match += keyRgb[k] >= 0 && static_cast<int>(raw) == keyRgb[k];
                    }
                    else
                    {
                        c[k] = row[x * 3 + k];
                        match += keyRgb[k] >= 0 && static_cast<int>(c[k]) == keyRgb[k];
                    }
                    dst[k] = static_cast<uint8_t>(c[k]);
                }
                dst[3] = match == 3 ? 0 : 255;
                break;
            }
            case 3:
            {
                const unsigned index = sampleAt(row, x, depth);
                if (index >= paletteCount)
                {
                    error = "the PNG uses a palette entry that does not exist";
                    return false;
                }
                dst[0] = parsed.plte[index * 3];
                dst[1] = parsed.plte[index * 3 + 1];
                dst[2] = parsed.plte[index * 3 + 2];
                dst[3] = index < parsed.trns.size() ? parsed.trns[index] : 255;
                break;
            }
            case 4:
            {
                const size_t step = depth == 16 ? 4u : 2u;
                dst[0] = dst[1] = dst[2] = row[x * step];
                dst[3] = row[x * step + (depth == 16 ? 2u : 1u)];
                break;
            }
            default:
            {
                const size_t step = depth == 16 ? 8u : 4u;
                const size_t gap = depth == 16 ? 2u : 1u;
                for (int k = 0; k < 4; ++k)
                    dst[k] = row[x * step + static_cast<size_t>(k) * gap];
                break;
            }
            }
        }
    }
    return true;
}

bool DecodeIndex(const uint8_t *data, size_t size, Image &out, std::string &error)
{
    Parsed parsed;
    if (!parseChunks(data, size, parsed, true, error))
        return false;
    const Info &info = parsed.info;
    if (info.colorType != 0 && info.colorType != 3)
    {
        error = "an index image must be a greyscale or paletted PNG (this one is " +
                std::string(info.colorType == 6 ? "RGBA" : info.colorType == 2 ? "RGB" : "grey with alpha") + ")";
        return false;
    }
    if (info.bitDepth > 8)
    {
        error = "an index image must be 8-bit (this one is 16-bit)";
        return false;
    }
    std::vector<uint8_t> rows;
    size_t rowBytes = 0;
    if (!unpackRows(parsed, rows, rowBytes, error))
        return false;
    out.width = info.width;
    out.height = info.height;
    out.format = Image::Format::Index8;
    out.wasPaletted = info.colorType == 3;
    out.pixels.resize(static_cast<size_t>(info.width) * info.height);
    for (uint32_t y = 0; y < info.height; ++y)
    {
        const uint8_t *row = &rows[y * rowBytes];
        uint8_t *dst = &out.pixels[static_cast<size_t>(y) * info.width];
        for (uint32_t x = 0; x < info.width; ++x)
            dst[x] = static_cast<uint8_t>(sampleAt(row, x, info.bitDepth));
    }
    return true;
}

// -----------------------------------------------------------------------------------------------------------------
// Encoder: filter "up", LZ77 with a hash chain, one fixed-Huffman deflate block.
namespace
{
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
        putBe32(png, Crc32(png.data() + start, png.size() - start));
    }
}

void DeflateFixed(const uint8_t *data, size_t n, std::vector<uint8_t> &out)
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

std::vector<uint8_t> Encode(uint32_t w, uint32_t h, int colorType, const std::vector<uint8_t> &pixels, const std::vector<uint8_t> &plte,
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
    DeflateFixed(raw.data(), raw.size(), z);
    putBe32(z, adler32(raw.data(), raw.size()));
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
} // namespace GSPng
