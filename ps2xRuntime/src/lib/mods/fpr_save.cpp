#include "fpr_save.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace ps2x::mods::fpr
{
    namespace fs = std::filesystem;

    // ---- SHA-256 ---------------------------------------------------------------------------------------------------------

    namespace
    {
        constexpr uint32_t kSha256K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

        void sha256Block(uint32_t state[8], const uint8_t *block)
        {
            uint32_t w[64];
            for (int i = 0; i < 16; ++i)
                w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
            for (int i = 16; i < 64; ++i)
            {
                const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
            for (int i = 0; i < 64; ++i)
            {
                const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                const uint32_t ch = (e & f) ^ (~e & g);
                const uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
                const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                const uint32_t t2 = s0 + maj;
                h = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = b;
                b = a;
                a = t1 + t2;
            }
            state[0] += a;
            state[1] += b;
            state[2] += c;
            state[3] += d;
            state[4] += e;
            state[5] += f;
            state[6] += g;
            state[7] += h;
        }
    }

    std::string sha256Hex(const uint8_t *data, size_t size)
    {
        uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        size_t offset = 0;
        for (; offset + 64 <= size; offset += 64)
            sha256Block(state, data + offset);
        uint8_t tail[128] = {};
        const size_t rest = size - offset;
        if (rest)
            std::memcpy(tail, data + offset, rest);
        tail[rest] = 0x80;
        const size_t tailSize = rest + 1 + 8 <= 64 ? 64 : 128;
        const uint64_t bits = static_cast<uint64_t>(size) * 8;
        for (int i = 0; i < 8; ++i)
            tail[tailSize - 1 - static_cast<size_t>(i)] = static_cast<uint8_t>(bits >> (8 * i));
        sha256Block(state, tail);
        if (tailSize == 128)
            sha256Block(state, tail + 64);
        char text[65];
        for (int i = 0; i < 8; ++i)
            std::snprintf(text + i * 8, 9, "%08x", state[i]);
        return std::string(text, 64);
    }

    // ---- the CRC table and the integrity data ----------------------------------------------------------------------------

    uint32_t crc(const uint8_t *data, size_t size, const CrcTable &table)
    {
        uint32_t c = 0;
        for (size_t i = 0; i < size; ++i)
            c = (c << 8) ^ table[((c >> 24) ^ data[i]) & 0xFF];
        return c;
    }

    namespace
    {
        // SHA-256 of the 1024 table bytes (little endian), the same fingerprint tools/save_checksums.py checks. A fingerprint, not data.
        constexpr const char *kTableSha256 = "d59001fff49cc61287800a987b3036deeea3e86eda7b80b80fd4197cf1d562e3";
        constexpr uint32_t kTableVaddr = 0x0032E8D0;

        uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
        void putLe32(uint8_t *p, uint32_t v)
        {
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(v >> 8);
            p[2] = static_cast<uint8_t>(v >> 16);
            p[3] = static_cast<uint8_t>(v >> 24);
        }
    }

    bool crcTableFingerprintOk(const CrcTable &table)
    {
        uint8_t raw[1024];
        for (size_t i = 0; i < 256; ++i)
            putLe32(raw + i * 4, table[i]);
        return sha256Hex(raw, sizeof(raw)) == kTableSha256;
    }

    bool crcTableFromElf(const fs::path &elf, CrcTable &out, std::string *error)
    {
        auto fail = [&](const std::string &message) {
            if (error)
                *error = message;
            return false;
        };
        std::ifstream in(elf, std::ios::binary);
        if (!in)
            return fail("the game executable cannot be opened");
        uint8_t header[52];
        in.read(reinterpret_cast<char *>(header), sizeof(header));
        if (in.gcount() != static_cast<std::streamsize>(sizeof(header)) || std::memcmp(header, "\x7f""ELF", 4) != 0 || header[4] != 1 || header[5] != 1)
            return fail("the game executable is not a little-endian 32-bit ELF file");
        const uint32_t phoff = le32(header + 28);
        const uint32_t phentsize = static_cast<uint32_t>(header[42] | (header[43] << 8));
        const uint32_t phnum = static_cast<uint32_t>(header[44] | (header[45] << 8));
        if (phentsize < 32 || phnum == 0 || phnum > 64)
            return fail("the game executable has an unexpected program header table");
        for (uint32_t i = 0; i < phnum; ++i)
        {
            in.clear();
            in.seekg(static_cast<std::streamoff>(phoff) + static_cast<std::streamoff>(i) * phentsize);
            uint8_t ph[32];
            in.read(reinterpret_cast<char *>(ph), sizeof(ph));
            if (in.gcount() != static_cast<std::streamsize>(sizeof(ph)))
                break;
            const uint32_t type = le32(ph), offset = le32(ph + 4), vaddr = le32(ph + 8), filesz = le32(ph + 16);
            if (type == 1 && vaddr <= kTableVaddr && static_cast<uint64_t>(kTableVaddr) + 1024 <= static_cast<uint64_t>(vaddr) + filesz)
            {
                in.clear();
                in.seekg(static_cast<std::streamoff>(offset) + (kTableVaddr - vaddr));
                uint8_t raw[1024];
                in.read(reinterpret_cast<char *>(raw), sizeof(raw));
                if (in.gcount() != static_cast<std::streamsize>(sizeof(raw)))
                    return fail("the game executable is too short to hold the checksum table");
                for (size_t k = 0; k < 256; ++k)
                    out[k] = le32(raw + k * 4);
                if (!crcTableFingerprintOk(out))
                    return fail("this is not the NTSC-U v1.01 executable (SLUS_217.02): its checksum table is different");
                return true;
            }
        }
        return fail("the checksum table is not inside a loadable segment of the game executable");
    }

    Integrity checkIntegrity(const std::vector<uint8_t> &image, const CrcTable &table)
    {
        Integrity r;
        r.sizeOk = image.size() == kSaveSize;
        if (!r.sizeOk)
            return r;
        for (size_t k = 0; k < kBlocks; ++k)
        {
            if (crc(image.data() + k * kBlockSize, kBlockSize, table) == le32(image.data() + kTableOff + k * 4))
                ++r.blocksOk;
            else
                r.badBlocks.push_back(k);
        }
        uint8_t parity[kBlockSize] = {};
        for (size_t k = 0; k < kBlocks; ++k)
            for (size_t i = 0; i < kBlockSize; ++i)
                parity[i] ^= image[k * kBlockSize + i];
        r.parityOk = std::memcmp(parity, image.data() + kParityOff, kBlockSize) == 0;
        r.finalOk = crc(parity, kBlockSize, table) == le32(image.data() + kFinalOff);
        r.markersOk = le32(image.data() + kSettingsMarkerOff) == kSettingsMarker && le32(image.data() + kRosterMarkerOff) == kRosterMarker;
        return r;
    }

    void fixIntegrity(std::vector<uint8_t> &image, const CrcTable &table)
    {
        uint8_t parity[kBlockSize] = {};
        for (size_t k = 0; k < kBlocks; ++k)
        {
            const uint8_t *block = image.data() + k * kBlockSize;
            putLe32(image.data() + kTableOff + k * 4, crc(block, kBlockSize, table));
            for (size_t i = 0; i < kBlockSize; ++i)
                parity[i] ^= block[i];
        }
        std::memcpy(image.data() + kParityOff, parity, kBlockSize);
        putLe32(image.data() + kFinalOff, crc(parity, kBlockSize, table));
    }

    // ---- text ------------------------------------------------------------------------------------------------------------

    bool decodeUtf8(const std::string &utf8, std::vector<uint32_t> &out, std::string *error)
    {
        out.clear();
        const size_t n = utf8.size();
        size_t i = 0;
        while (i < n)
        {
            const uint8_t c = static_cast<uint8_t>(utf8[i]);
            uint32_t cp;
            size_t extra;
            if (c < 0x80)
            {
                cp = c;
                extra = 0;
            }
            else if ((c & 0xE0) == 0xC0)
            {
                cp = c & 0x1F;
                extra = 1;
            }
            else if ((c & 0xF0) == 0xE0)
            {
                cp = c & 0x0F;
                extra = 2;
            }
            else if ((c & 0xF8) == 0xF0)
            {
                cp = c & 0x07;
                extra = 3;
            }
            else
            {
                if (error)
                    *error = "the text is not valid UTF-8";
                return false;
            }
            for (size_t k = 1; k <= extra; ++k)
            {
                if (i + k >= n || (static_cast<uint8_t>(utf8[i + k]) & 0xC0) != 0x80)
                {
                    if (error)
                        *error = "the text is not valid UTF-8";
                    return false;
                }
                cp = (cp << 6) | (static_cast<uint8_t>(utf8[i + k]) & 0x3F);
            }
            const uint32_t minimum[4] = {0, 0x80, 0x800, 0x10000};
            if (cp < minimum[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            {
                if (error)
                    *error = "the text is not valid UTF-8";
                return false;
            }
            out.push_back(cp);
            i += extra + 1;
        }
        return true;
    }

    namespace
    {
        // Unicode -> code page 932, sorted by code point. Generated by gen_cp932_table.py from Python's cp932 codec.
        const uint32_t kCp932[] = {
#include "cp932_table.inc"
        };
    }

    bool encodeGameText(const std::vector<uint32_t> &codePoints, std::vector<uint8_t> &out, std::string *error)
    {
        out.clear();
        for (uint32_t cp : codePoints)
        {
            if (cp == ' ')
                cp = 0x3000;
            else if (cp >= 0x21 && cp <= 0x7E)
                cp += 0xFEE0;
            if (cp < 0x80)
            {
                out.push_back(static_cast<uint8_t>(cp));
                continue;
            }
            const uint32_t* const begin = std::begin(kCp932);
            const uint32_t* const end = std::end(kCp932);
            const uint32_t* it = std::lower_bound(begin, end, cp << 16);
            if (cp > 0xFFFF || it == end || (*it >> 16) != cp)
            {
                if (error)
                {
                    char text[40];
                    std::snprintf(text, sizeof(text), "U+%04X", static_cast<unsigned>(cp));
                    *error = std::string("the character ") + text + " is not in the game's font (it has Shift-JIS characters only)";
                }
                return false;
            }
            const uint32_t code = *it & 0xFFFF;
            if (code < 0x100)
            {
                out.push_back(static_cast<uint8_t>(code));
            }
            else
            {
                out.push_back(static_cast<uint8_t>(code >> 8));
                out.push_back(static_cast<uint8_t>(code & 0xFF));
            }
        }
        return true;
    }

    // ---- fields ----------------------------------------------------------------------------------------------------------

    namespace
    {
        // "12" -> 12 (digits only, at most 4 of them).
        bool parseIndex(const std::string &text, int &out)
        {
            if (text.empty() || text.size() > 4)
                return false;
            int v = 0;
            for (const char c : text)
            {
                if (c < '0' || c > '9')
                    return false;
                v = v * 10 + (c - '0');
            }
            out = v;
            return true;
        }
    }

    bool fieldRange(const std::string &key, FieldRange &out)
    {
        if (key == "o.order")
        {
            out = FieldRange{kOffOrgOrder, static_cast<uint32_t>(kOrgs), 0xFF};
            return true;
        }
        if (key == "g.order")
        {
            out = FieldRange{kOffGroupOrder, static_cast<uint32_t>(kGroups), 0xFF};
            return true;
        }
        if (key.empty())
            return false;
        const char kind = key[0];
        if (kind != 'w' && kind != 'o' && kind != 'g' && kind != 'a')
            return false;
        const size_t dot = key.find('.');
        int index = 0;
        if (!parseIndex(key.substr(1, dot == std::string::npos ? std::string::npos : dot - 1), index))
            return false;
        const std::string part = dot == std::string::npos ? std::string() : key.substr(dot + 1);
        if (kind == 'a')
        {
            if (!part.empty() || index >= kWrestlers)
                return false;
            out = FieldRange{kOffAppearance + static_cast<uint32_t>(index) * kAppearanceStride, kAppearanceStride, 0xFF};
            return true;
        }
        if (kind == 'w')
        {
            if (index >= kWrestlers)
                return false;
            const uint32_t base = kOffNames + static_cast<uint32_t>(index) * kNameStride;
            if (part == "first") out = FieldRange{base + 1, kWidthFirst, 0xFF};
            else if (part == "last") out = FieldRange{base + 15, kWidthLast, 0xFF};
            else if (part == "nick") out = FieldRange{base + 33, kWidthNickname, 0xFF};
            else if (part == "flip") out = FieldRange{base, 1, 0x80};
            else if (part == "space") out = FieldRange{base, 1, 0x03};
            else if (part == "flags") out = FieldRange{base, 1, 0x7C};
            else return false;
            return true;
        }
        const bool org = kind == 'o';
        if (index >= (org ? kOrgs : kGroups))
            return false;
        const uint32_t base = (org ? kOffOrgs : kOffGroups) + static_cast<uint32_t>(index) * (org ? kOrgStride : kGroupStride);
        if (part == "live") out = FieldRange{base, 2, 0xFF};
        else if (part == "name") out = FieldRange{base + 2, kWidthOrgLong, 0xFF};
        else if (part == "short") out = FieldRange{base + 28, kWidthOrgShort, 0xFF};
        else if (org && part == "id") out = FieldRange{base + 42, 2, 0xFF};
        else if (!org && part == "org") out = FieldRange{base + 42, 2, 0xFF};
        else if (!org && part == "kind") out = FieldRange{base + 44, 2, 0xFF};
        else return false;
        return true;
    }

    std::string describeField(const std::string &key)
    {
        if (key == "o.order")
            return "the organisation list order";
        if (key == "g.order")
            return "the group list order";
        if (key.empty())
            return key;
        const size_t dot = key.find('.');
        const std::string number = key.substr(1, dot == std::string::npos ? std::string::npos : dot - 1);
        const std::string part = dot == std::string::npos ? std::string() : key.substr(dot + 1);
        switch (key[0])
        {
        case 'a': return "wrestler " + number + "'s appearance";
        case 'w':
        {
            const std::string who = "wrestler " + number + "'s ";
            if (part == "first") return who + "FIRST name";
            if (part == "last") return who + "LAST name";
            if (part == "nick") return who + "NICKNAME";
            if (part == "flip") return who + "Flip option";
            if (part == "space") return who + "Space option";
            if (part == "flags") return who + "name options";
            break;
        }
        case 'o':
        case 'g':
        {
            const std::string what = key[0] == 'o' ? "organisation " : "group ";
            const std::string who = what + number + "'s ";
            if (part == "live") return what + number;
            if (part == "name") return who + "name";
            if (part == "short") return who + "short name";
            if (part == "id") return who + "id";
            if (part == "org") return who + "organisation";
            if (part == "kind") return who + "alignment";
            break;
        }
        default: break;
        }
        return key;
    }

    std::vector<uint8_t> readField(const std::vector<uint8_t> &image, const FieldRange &range)
    {
        std::vector<uint8_t> out(range.length);
        if (static_cast<size_t>(range.offset) + range.length > image.size())
            return out;
        std::memcpy(out.data(), image.data() + range.offset, range.length);
        if (range.length == 1)
            out[0] = static_cast<uint8_t>(out[0] & range.mask);
        return out;
    }

    void writeField(std::vector<uint8_t> &image, const FieldRange &range, const std::vector<uint8_t> &value)
    {
        if (static_cast<size_t>(range.offset) + range.length > image.size() || value.size() != range.length)
            return;
        if (range.length == 1 && range.mask != 0xFF)
        {
            image[range.offset] = static_cast<uint8_t>((image[range.offset] & ~range.mask) | (value[0] & range.mask));
            return;
        }
        std::memcpy(image.data() + range.offset, value.data(), range.length);
    }
}
