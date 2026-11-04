// A small PNG writer for the test tools (ps2x_gs_replay, ps2x_gpu_smoke): 8-bit RGB, stored (not
// compressed) deflate blocks, no dependency. The alpha channel of the input is dropped.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ps2x::test
{
    inline uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc = 0)
    {
        static uint32_t table[256];
        static bool ready = false;
        if (!ready)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            ready = true;
        }
        crc = ~crc;
        for (size_t i = 0; i < size; ++i)
            crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
        return ~crc;
    }

    inline void put32be(std::vector<uint8_t> &out, uint32_t v)
    {
        out.push_back(static_cast<uint8_t>(v >> 24));
        out.push_back(static_cast<uint8_t>(v >> 16));
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v));
    }

    inline void pngChunk(std::vector<uint8_t> &out, const char *tag, const std::vector<uint8_t> &body)
    {
        put32be(out, static_cast<uint32_t>(body.size()));
        const size_t start = out.size();
        out.insert(out.end(), tag, tag + 4);
        out.insert(out.end(), body.begin(), body.end());
        put32be(out, crc32(out.data() + start, out.size() - start));
    }

    // Writes RGBA8 pixels (rows `strideBytes` apart) as an RGB PNG. Returns false if the file cannot be written.
    inline bool writePng(const std::string &path, const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t strideBytes)
    {
        std::vector<uint8_t> raw;
        raw.reserve(static_cast<size_t>(height) * (1 + width * 3));
        for (uint32_t y = 0; y < height; ++y)
        {
            raw.push_back(0); // filter: none
            const uint8_t *row = rgba + static_cast<size_t>(y) * strideBytes;
            for (uint32_t x = 0; x < width; ++x)
            {
                raw.push_back(row[x * 4]);
                raw.push_back(row[x * 4 + 1]);
                raw.push_back(row[x * 4 + 2]);
            }
        }
        std::vector<uint8_t> z = {0x78, 0x01};
        uint32_t a = 1, b = 0;
        for (uint8_t v : raw)
        {
            a = (a + v) % 65521u;
            b = (b + a) % 65521u;
        }
        for (size_t pos = 0; pos < raw.size() || pos == 0;)
        {
            const size_t n = std::min<size_t>(65535, raw.size() - pos);
            const bool last = pos + n >= raw.size();
            z.push_back(last ? 1 : 0);
            z.push_back(static_cast<uint8_t>(n));
            z.push_back(static_cast<uint8_t>(n >> 8));
            z.push_back(static_cast<uint8_t>(~n));
            z.push_back(static_cast<uint8_t>((~n) >> 8));
            z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
            pos += n;
            if (last)
                break;
        }
        put32be(z, (b << 16) | a);

        std::vector<uint8_t> file = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        std::vector<uint8_t> ihdr;
        put32be(ihdr, width);
        put32be(ihdr, height);
        ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
        pngChunk(file, "IHDR", ihdr);
        pngChunk(file, "IDAT", z);
        pngChunk(file, "IEND", {});
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
        std::fclose(f);
        return ok;
    }
}
