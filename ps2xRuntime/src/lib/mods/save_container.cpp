#include "save_container.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <fstream>
#include <new>
#include <string_view>
#include <utility>

// See save_container.h for what is read. The layouts below follow mymcplus (mymcplus/save/format_*.py and ps2mc.py), which is what the
// formats' community documentation is, and the unit tests compare the results with it. Everything here only reads; every size taken
// from a file is checked against the bytes that are really there before it is used, so a damaged or hostile file ends in an error line
// and not in a crash or a huge allocation.
//
// The CodeBreaker cipher state (kCbsRc4State) is a constant of that format, as published by mymc/mymcplus (GPL-3.0, like this fork).

namespace ps2x::mods
{
    namespace
    {
        namespace fs = std::filesystem;
        using Bytes = std::vector<uint8_t>;

        constexpr size_t kMaxContainerBytes = size_t(256) << 20; // a file larger than this is not a save container
        constexpr size_t kMaxExpanded = size_t(64) << 20;        // the most a .max or .cbs may unpack to
        constexpr size_t kMaxFilesPerFolder = 4096;
        constexpr size_t kMaxRootEntries = 65536;
        constexpr size_t kRawSaveSize = 0xDF1E4; // Fire Pro Returns' whole save (see fpr_save.h kSaveSize); the only raw file recognised

        // A reading error. `message` is one line that starts in lower case; the caller puts the format's name in front of it.
        struct ReadFail
        {
            std::string message;
        };

        [[noreturn]] void fail(std::string message) { throw ReadFail{std::move(message)}; }

        uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
        uint32_t rd32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

        // The text up to the first zero byte (names are zero-terminated in fixed-size fields).
        std::string cstr(const uint8_t *p, size_t n)
        {
            size_t k = 0;
            while (k < n && p[k] != 0)
                ++k;
            return std::string(reinterpret_cast<const char *>(p), k);
        }

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        bool endsWith(const std::string &s, const char *suffix)
        {
            const size_t n = std::strlen(suffix);
            return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
        }

        // A cursor over a byte vector. Every read checks that the bytes are there.
        class Cursor
        {
        public:
            explicit Cursor(const Bytes &bytes, size_t at = 0) : b_(bytes), pos_(at) {}

            size_t pos() const { return pos_; }
            size_t left() const { return pos_ <= b_.size() ? b_.size() - pos_ : 0; }
            void need(uint64_t n) const
            {
                if (n > left())
                    fail("the file ends too soon (it is cut short or damaged)");
            }
            void seek(size_t at)
            {
                if (at > b_.size())
                    fail("the file ends too soon (it is cut short or damaged)");
                pos_ = at;
            }
            const uint8_t *take(size_t n)
            {
                need(n);
                const uint8_t *p = b_.data() + pos_;
                pos_ += n;
                return p;
            }
            void skip(size_t n) { (void)take(n); }
            uint16_t u16() { return rd16(take(2)); }
            uint32_t u32() { return rd32(take(4)); }
            // A string with a 32-bit length in front of it (SharkPort); the text itself is not used.
            void skipLongString() { skip(u32()); }

        private:
            const Bytes &b_;
            size_t pos_;
        };

        // ---- memory-card directory entries (the PSU file and the card image use the same 512-byte entry) ------------------------
        constexpr size_t kDirentSize = 512;
        constexpr uint32_t kDfFile = 0x0010, kDfDir = 0x0020, kDfExists = 0x8000;

        bool modeIsFile(uint32_t mode) { return (mode & (kDfFile | kDfDir | kDfExists)) == (kDfFile | kDfExists); }
        bool modeIsDir(uint32_t mode) { return (mode & (kDfFile | kDfDir | kDfExists)) == (kDfDir | kDfExists); }

        struct Dirent
        {
            uint32_t mode = 0;
            uint32_t length = 0;  // bytes for a file, number of entries for a folder
            uint32_t cluster = 0; // first cluster (card images)
            std::string name;
        };

        // mode u16, u16, length u32, created[8], cluster u32, parent u32, modified[8], attr u32, pad[28], name[448]
        Dirent parseDirent(const uint8_t *p)
        {
            Dirent d;
            d.mode = rd16(p);
            d.length = rd32(p + 4);
            d.cluster = rd32(p + 16);
            d.name = cstr(p + 64, 448);
            return d;
        }

        // ---- building the result ---------------------------------------------------------------------------------------------
        bool isIconFile(const std::string &name)
        {
            const std::string l = lower(name);
            return l == "icon.sys" || endsWith(l, ".ico");
        }

        // The folder's main file: the one named like the folder, else the largest that is not an icon file.
        SaveEntry makeEntry(std::string folder, const char *format, std::vector<SaveFile> files)
        {
            SaveEntry e;
            e.folder = std::move(folder);
            e.format = format;
            size_t main = files.size();
            if (!e.folder.empty())
            {
                const std::string want = lower(e.folder);
                for (size_t i = 0; i < files.size(); ++i)
                {
                    if (lower(files[i].name) == want)
                    {
                        main = i;
                        break;
                    }
                }
            }
            if (main == files.size())
            {
                size_t best = 0;
                for (size_t i = 0; i < files.size(); ++i)
                {
                    if (isIconFile(files[i].name))
                        continue;
                    if (main == files.size() || files[i].data.size() > best)
                    {
                        main = i;
                        best = files[i].data.size();
                    }
                }
            }
            for (size_t i = 0; i < files.size(); ++i)
            {
                if (i == main)
                {
                    e.save = std::move(files[i].data);
                    e.saveName = std::move(files[i].name);
                }
                else
                {
                    e.others.push_back(std::move(files[i]));
                }
            }
            return e;
        }

        // A card folder name: B, then A/E/I/K (America, Europe, Japan, Korea...), then the rest ("BASLUS-21702", "BISLPM-66082").
        bool looksLikeFolderName(const std::string &name)
        {
            if (name.size() < 10 || name[0] != 'B' || std::strchr("AEIK", name[1]) == nullptr)
                return false;
            for (const char c : name)
                if (static_cast<unsigned char>(c) < 0x21 || c == '/' || c == '\\' || c == '.')
                    return false;
            return true;
        }

        // ---- .max: Action Replay MAX -----------------------------------------------------------------------------------------
        // LZARI (Haruhiko Okumura's adaptive arithmetic coding plus a 4096-byte sliding window), decoding only. Written from his
        // public-domain LZARI.C; the constants cannot change without breaking compatibility.
        class Lzari
        {
        public:
            Lzari(const uint8_t *in, size_t n) : in_(in), n_(n) {}

            // Unpacks exactly `outLen` bytes into `out`. False when the data is damaged (the input ran out, or the model reached an
            // impossible state).
            bool decode(uint8_t *out, size_t outLen)
            {
                startModel();
                low_ = 0;
                high_ = kQ4;
                value_ = 0;
                for (int i = 0; i < kM + 2; ++i)
                    value_ = 2 * value_ + getBit();

                std::memset(text_, 0, sizeof(text_));
                std::memset(text_, ' ', kN - kF);
                int r = kN - kF;
                size_t count = 0;
                while (count < outLen)
                {
                    const int c = decodeChar();
                    if (c < 0)
                        return false;
                    if (c < 256)
                    {
                        out[count++] = static_cast<uint8_t>(c);
                        text_[r++] = static_cast<uint8_t>(c);
                        r &= kN - 1;
                    }
                    else
                    {
                        const int pos = decodePosition();
                        if (pos < 0)
                            return false;
                        const int i = (r - pos - 1) & (kN - 1);
                        const int j = c - 255 + kThreshold;
                        for (int k = 0; k < j && count < outLen; ++k)
                        {
                            const uint8_t ch = text_[(i + k) & (kN - 1)];
                            out[count++] = ch;
                            text_[r++] = ch;
                            r &= kN - 1;
                        }
                    }
                    if (padBytes_ > 8)
                        return false; // reading far past the end of the data
                }
                return true;
            }

        private:
            static constexpr int kN = 4096;       // window size
            static constexpr int kF = 60;         // longest match
            static constexpr int kThreshold = 2;  // matches are at least kThreshold + 1 long
            static constexpr int kNChar = 256 - kThreshold + kF; // 314 symbols: 256 bytes and 58 match lengths
            static constexpr int kM = 15;
            static constexpr uint64_t kQ1 = uint64_t(1) << kM, kQ2 = 2 * kQ1, kQ3 = 3 * kQ1, kQ4 = 4 * kQ1;
            static constexpr uint32_t kMaxCum = (uint32_t(1) << kM) - 1;

            const uint8_t *in_;
            size_t n_;
            size_t pos_ = 0;
            unsigned buf_ = 0, mask_ = 0;
            int padBytes_ = 0;

            uint64_t low_ = 0, high_ = 0, value_ = 0;
            uint32_t symToChar_[kNChar + 1];
            uint32_t symFreq_[kNChar + 1];
            uint32_t symCum_[kNChar + 1];
            uint32_t posCum_[kN + 1];
            uint8_t text_[kN];

            int getBit()
            {
                if ((mask_ >>= 1) == 0)
                {
                    if (pos_ < n_)
                    {
                        buf_ = in_[pos_++];
                    }
                    else
                    {
                        buf_ = 0;
                        ++padBytes_;
                    }
                    mask_ = 128;
                }
                return (buf_ & mask_) != 0 ? 1 : 0;
            }

            void startModel()
            {
                symCum_[kNChar] = 0;
                for (int sym = kNChar; sym >= 1; --sym)
                {
                    symToChar_[sym] = static_cast<uint32_t>(sym - 1);
                    symFreq_[sym] = 1;
                    symCum_[sym - 1] = symCum_[sym] + symFreq_[sym];
                }
                symFreq_[0] = 0;
                symToChar_[0] = 0;
                posCum_[kN] = 0;
                for (int i = kN; i >= 1; --i)
                    posCum_[i - 1] = posCum_[i] + 10000u / static_cast<uint32_t>(200 + i);
            }

            void updateModel(int sym)
            {
                if (symCum_[0] >= kMaxCum)
                {
                    uint32_t c = 0;
                    for (int i = kNChar; i > 0; --i)
                    {
                        symCum_[i] = c;
                        symFreq_[i] = (symFreq_[i] + 1) >> 1;
                        c += symFreq_[i];
                    }
                    symCum_[0] = c;
                }
                int i = sym;
                while (symFreq_[i] == symFreq_[i - 1])
                    --i;
                if (i < sym)
                    std::swap(symToChar_[i], symToChar_[sym]);
                symFreq_[i]++;
                while (--i >= 0)
                    symCum_[i]++;
            }

            int searchSym(uint32_t x) const
            {
                int i = 1, j = kNChar;
                while (i < j)
                {
                    const int k = (i + j) / 2;
                    if (symCum_[k] > x)
                        i = k + 1;
                    else
                        j = k;
                }
                return i;
            }

            int searchPos(uint32_t x) const
            {
                int i = 1, j = kN;
                while (i < j)
                {
                    const int k = (i + j) / 2;
                    if (posCum_[k] > x)
                        i = k + 1;
                    else
                        j = k;
                }
                return i - 1;
            }

            // Scales the interval back up after a symbol; false if it would not end (damaged data).
            bool renormalise()
            {
                for (int guard = 0; guard < 64; ++guard)
                {
                    if (low_ >= kQ2)
                    {
                        value_ -= kQ2;
                        low_ -= kQ2;
                        high_ -= kQ2;
                    }
                    else if (low_ >= kQ1 && high_ <= kQ3)
                    {
                        value_ -= kQ1;
                        low_ -= kQ1;
                        high_ -= kQ1;
                    }
                    else if (high_ > kQ2)
                    {
                        return true;
                    }
                    low_ += low_;
                    high_ += high_;
                    value_ = 2 * value_ + static_cast<uint64_t>(getBit());
                }
                return false;
            }

            int decodeChar()
            {
                const uint64_t range = high_ - low_;
                if (range == 0 || value_ < low_ || symCum_[0] == 0)
                    return -1;
                const uint32_t x = static_cast<uint32_t>(((value_ - low_ + 1) * symCum_[0] - 1) / range);
                const int sym = searchSym(x);
                high_ = low_ + (range * symCum_[sym - 1]) / symCum_[0];
                low_ += (range * symCum_[sym]) / symCum_[0];
                if (!renormalise())
                    return -1;
                const int ch = static_cast<int>(symToChar_[sym]);
                updateModel(sym);
                return ch;
            }

            int decodePosition()
            {
                const uint64_t range = high_ - low_;
                if (range == 0 || value_ < low_)
                    return -1;
                const uint32_t x = static_cast<uint32_t>(((value_ - low_ + 1) * posCum_[0] - 1) / range);
                const int position = searchPos(x);
                high_ = low_ + (range * posCum_[position]) / posCum_[0];
                low_ += (range * posCum_[position + 1]) / posCum_[0];
                if (!renormalise())
                    return -1;
                return position;
            }
        };

        std::vector<SaveEntry> readMax(const Bytes &b)
        {
            // magic[12] crc u32 dirname[32] iconsysname[32] clen u32 dirlen u32 length u32   (0x5C bytes)
            Cursor c(b);
            c.skip(12);
            (void)c.u32(); // CRC-32 of the header and the packed data; mymcplus does not check it either
            const std::string dirname = cstr(c.take(32), 32);
            c.skip(32);
            const uint32_t clen = c.u32();
            const uint32_t dirlen = c.u32();
            const uint32_t length = c.u32();

            // Some saves store the unpacked size where the packed size belongs; then the packed data is the rest of the file.
            size_t packedLen;
            if (clen == length)
            {
                packedLen = c.left();
            }
            else
            {
                if (clen < 4)
                    fail("the header is damaged");
                packedLen = clen - 4;
                c.need(packedLen);
            }
            const uint8_t *packed = c.take(packedLen);

            if (length > kMaxExpanded)
                fail("the unpacked size is not believable");
            if (dirlen == 0 || dirlen > kMaxFilesPerFolder)
                fail("the file count is not believable");

            Bytes s(length);
            if (!Lzari(packed, packedLen).decode(s.data(), s.size()))
                fail("the compressed data is damaged");

            std::vector<SaveFile> files;
            size_t off = 0;
            for (uint32_t i = 0; i < dirlen; ++i)
            {
                if (off > s.size() || s.size() - off < 36)
                    fail("the file list ends too soon");
                const uint32_t l = rd32(s.data() + off);
                SaveFile f;
                f.name = cstr(s.data() + off + 4, 32);
                off += 36;
                if (l > s.size() - off)
                    fail("a file is cut short");
                f.data.assign(s.begin() + static_cast<std::ptrdiff_t>(off), s.begin() + static_cast<std::ptrdiff_t>(off + l));
                files.push_back(std::move(f));
                off += l;
                off = ((off + 8 + 15) & ~size_t(15)) - 8; // each file ends so that (offset + 8) is a multiple of 16
            }
            std::vector<SaveEntry> out;
            out.push_back(makeEntry(dirname, "max", std::move(files)));
            return out;
        }

        // ---- .psu: EMS --------------------------------------------------------------------------------------------------------
        bool looksLikePsu(const Bytes &b)
        {
            if (b.size() < kDirentSize * 3)
                return false;
            const Dirent dir = parseDirent(b.data());
            const Dirent dot = parseDirent(b.data() + kDirentSize);
            const Dirent dotdot = parseDirent(b.data() + kDirentSize * 2);
            return modeIsDir(dir.mode) && modeIsDir(dot.mode) && modeIsDir(dotdot.mode) && dir.length >= 2 && dot.name == "." && dotdot.name == "..";
        }

        std::vector<SaveEntry> readPsu(const Bytes &b)
        {
            Cursor c(b);
            const Dirent dir = parseDirent(c.take(kDirentSize));
            const Dirent dot = parseDirent(c.take(kDirentSize));
            const Dirent dotdot = parseDirent(c.take(kDirentSize));
            if (!modeIsDir(dir.mode) || !modeIsDir(dot.mode) || !modeIsDir(dotdot.mode) || dir.length < 2)
                fail("the folder header is damaged");
            const uint32_t count = dir.length - 2; // the folder's own length counts "." and ".."
            if (count > kMaxFilesPerFolder)
                fail("the file count is not believable");

            std::vector<SaveFile> files;
            for (uint32_t i = 0; i < count; ++i)
            {
                const Dirent ent = parseDirent(c.take(kDirentSize));
                if (!modeIsFile(ent.mode))
                    fail("the save holds a sub-folder, which a save cannot");
                c.need(ent.length);
                SaveFile f;
                f.name = ent.name;
                const uint8_t *p = c.take(ent.length);
                f.data.assign(p, p + ent.length);
                files.push_back(std::move(f));
                // Each file is padded with zeros to a multiple of 1024 bytes. The padding of the last file is often left off.
                const size_t pad = ((ent.length + 1023) & ~size_t(1023)) - ent.length;
                c.skip(std::min(pad, c.left()));
            }
            std::vector<SaveEntry> out;
            out.push_back(makeEntry(dir.name, "psu", std::move(files)));
            return out;
        }

        // ---- .xps / .sps: X-Port and SharkPort -------------------------------------------------------------------------------
        constexpr uint8_t kSpsMagic[17] = {0x0d, 0, 0, 0, 'S', 'h', 'a', 'r', 'k', 'P', 'o', 'r', 't', 'S', 'a', 'v', 'e'};
        constexpr size_t kSpsEntryHeader = 98;

        struct SpsEntry
        {
            std::string name;
            uint32_t length = 0;
            uint32_t mode = 0;
        };

        // hlen u16, name[64], length u32, pad[8], mode u16 (bytes swapped), pad[2], created[8], modified[8]; then hlen-98 more bytes
        SpsEntry readSpsEntry(Cursor &c)
        {
            const size_t start = c.pos();
            const uint8_t *p = c.take(kSpsEntryHeader);
            const uint16_t hlen = rd16(p);
            if (hlen < kSpsEntryHeader)
                fail("an entry header is too short");
            SpsEntry e;
            e.name = cstr(p + 2, 64);
            e.length = rd32(p + 66);
            const uint16_t rawMode = rd16(p + 78);
            e.mode = static_cast<uint32_t>(((rawMode >> 8) & 0xFF) | ((rawMode & 0xFF) << 8)); // stored byte-swapped
            c.seek(start + hlen);
            return e;
        }

        std::vector<SaveEntry> readSps(const Bytes &b)
        {
            Cursor c(b);
            c.skip(sizeof(kSpsMagic));
            (void)c.u32(); // save type
            c.skipLongString(); // folder name (the entry below has it too)
            c.skipLongString(); // date
            c.skipLongString(); // comment
            (void)c.u32();      // length of what follows

            const SpsEntry dir = readSpsEntry(c);
            if (!modeIsDir(dir.mode) || dir.length < 2)
                fail("the folder header is damaged");
            const uint32_t count = dir.length - 2;
            if (count > kMaxFilesPerFolder)
                fail("the file count is not believable");

            std::vector<SaveFile> files;
            for (uint32_t i = 0; i < count; ++i)
            {
                const SpsEntry ent = readSpsEntry(c);
                if (!modeIsFile(ent.mode))
                    fail("the save holds a sub-folder, which a save cannot");
                c.need(ent.length);
                SaveFile f;
                f.name = ent.name;
                const uint8_t *p = c.take(ent.length);
                f.data.assign(p, p + ent.length);
                files.push_back(std::move(f));
            }
            // A 4-byte checksum follows; it is not checked.
            std::vector<SaveEntry> out;
            out.push_back(makeEntry(dir.name, "sps", std::move(files)));
            return out;
        }

        // ---- .cbs: CodeBreaker ------------------------------------------------------------------------------------------------
        constexpr uint8_t kCbsMagic[4] = {'C', 'F', 'U', 0};

        // The initial permutation of the RC4 variant CodeBreaker uses (no key schedule: this is the state it starts from).
        constexpr uint8_t kCbsRc4State[256] = {
            0x5f, 0x1f, 0x85, 0x6f, 0x31, 0xaa, 0x3b, 0x18, 0x21, 0xb9, 0xce, 0x1c, 0x07, 0x4c, 0x9c, 0xb4,
            0x81, 0xb8, 0xef, 0x98, 0x59, 0xae, 0xf9, 0x26, 0xe3, 0x80, 0xa3, 0x29, 0x2d, 0x73, 0x51, 0x62,
            0x7c, 0x64, 0x46, 0xf4, 0x34, 0x1a, 0xf6, 0xe1, 0xba, 0x3a, 0x0d, 0x82, 0x79, 0x0a, 0x5c, 0x16,
            0x71, 0x49, 0x8e, 0xac, 0x8c, 0x9f, 0x35, 0x19, 0x45, 0x94, 0x3f, 0x56, 0x0c, 0x91, 0x00, 0x0b,
            0xd7, 0xb0, 0xdd, 0x39, 0x66, 0xa1, 0x76, 0x52, 0x13, 0x57, 0xf3, 0xbb, 0x4e, 0xe5, 0xdc, 0xf0,
            0x65, 0x84, 0xb2, 0xd6, 0xdf, 0x15, 0x3c, 0x63, 0x1d, 0x89, 0x14, 0xbd, 0xd2, 0x36, 0xfe, 0xb1,
            0xca, 0x8b, 0xa4, 0xc6, 0x9e, 0x67, 0x47, 0x37, 0x42, 0x6d, 0x6a, 0x03, 0x92, 0x70, 0x05, 0x7d,
            0x96, 0x2f, 0x40, 0x90, 0xc4, 0xf1, 0x3e, 0x3d, 0x01, 0xf7, 0x68, 0x1e, 0xc3, 0xfc, 0x72, 0xb5,
            0x54, 0xcf, 0xe7, 0x41, 0xe4, 0x4d, 0x83, 0x55, 0x12, 0x22, 0x09, 0x78, 0xfa, 0xde, 0xa7, 0x06,
            0x08, 0x23, 0xbf, 0x0f, 0xcc, 0xc1, 0x97, 0x61, 0xc5, 0x4a, 0xe6, 0xa0, 0x11, 0xc2, 0xea, 0x74,
            0x02, 0x87, 0xd5, 0xd1, 0x9d, 0xb7, 0x7e, 0x38, 0x60, 0x53, 0x95, 0x8d, 0x25, 0x77, 0x10, 0x5e,
            0x9b, 0x7f, 0xd8, 0x6e, 0xda, 0xa2, 0x2e, 0x20, 0x4f, 0xcd, 0x8f, 0xcb, 0xbe, 0x5a, 0xe0, 0xed,
            0x2c, 0x9a, 0xd4, 0xe2, 0xaf, 0xd0, 0xa9, 0xe8, 0xad, 0x7a, 0xbc, 0xa8, 0xf2, 0xee, 0xeb, 0xf5,
            0xa6, 0x99, 0x28, 0x24, 0x6c, 0x2b, 0x75, 0x5d, 0xf8, 0xd3, 0x86, 0x17, 0xfb, 0xc0, 0x7b, 0xb3,
            0x58, 0xdb, 0xc7, 0x4b, 0xff, 0x04, 0x50, 0xe9, 0x88, 0x69, 0xc9, 0x2a, 0xab, 0xfd, 0x5b, 0x1b,
            0x8a, 0xd9, 0xec, 0x27, 0x44, 0x0e, 0x33, 0xc8, 0x6b, 0x93, 0x32, 0x48, 0xb6, 0x30, 0x43, 0xa5,
        };

        // The cipher is its own inverse: the same call encrypts and decrypts.
        void cbsCrypt(uint8_t *data, size_t n)
        {
            uint8_t s[256];
            std::memcpy(s, kCbsRc4State, sizeof(s));
            unsigned j = 0;
            for (size_t ii = 0; ii < n; ++ii)
            {
                const unsigned i = static_cast<unsigned>((ii + 1) & 255);
                j = (j + s[i]) & 255;
                std::swap(s[i], s[j]);
                data[ii] ^= s[(s[i] + s[j]) & 255];
            }
        }

        // Unpacks a zlib stream into at most `limit` bytes (what the header promises). Fewer bytes are fine; a bad stream is not.
        Bytes inflateCapped(const Bytes &body, size_t limit)
        {
            Bytes out(limit);
            mz_stream z;
            std::memset(&z, 0, sizeof(z));
            if (mz_inflateInit(&z) != MZ_OK)
                fail("the zlib reader could not start");
            z.next_in = body.data();
            z.avail_in = static_cast<mz_uint32>(body.size());
            z.next_out = out.data();
            z.avail_out = static_cast<mz_uint32>(limit);
            int rc = MZ_OK;
            // Stop when the stream ends, when the room the header promised is full, or when a call moves nothing (miniz keeps
            // answering MZ_OK for a full output buffer, so the loop has to watch for that itself).
            while (rc == MZ_OK && z.avail_out > 0)
            {
                const mz_ulong before = z.total_in + z.total_out;
                rc = mz_inflate(&z, MZ_SYNC_FLUSH);
                if (rc == MZ_OK && z.total_in + z.total_out == before)
                    break;
            }
            const size_t produced = limit - z.avail_out;
            mz_inflateEnd(&z);
            // MZ_STREAM_END: finished. MZ_BUF_ERROR: ran out of input. MZ_OK: the room is full (the header's promise).
            if (rc != MZ_STREAM_END && rc != MZ_BUF_ERROR && rc != MZ_OK)
                fail("the compressed data is damaged");
            out.resize(produced);
            return out;
        }

        std::vector<SaveEntry> readCbs(const Bytes &b)
        {
            Cursor c(b);
            c.skip(4);
            (void)c.u32();
            const uint32_t hlen = c.u32();
            if (hlen < 92 + 32)
                fail("the header is too short");
            // dlen u32, flen u32, dirname[32], created[8], modified[8], six u32 (the fourth is the folder mode), title[hlen-92]
            const uint8_t *h = c.take(hlen - 12);
            const uint32_t dlen = rd32(h);
            const uint32_t flen = rd32(h + 4);
            const std::string dirname = cstr(h + 8, 32);

            // flen is either the length of the packed body or the length of the whole file.
            const size_t have = std::min<size_t>(flen, c.left());
            if (have != flen && int64_t(have) != int64_t(flen) - int64_t(hlen))
                fail("the file ends too soon (it is cut short or damaged)");
            if (dlen > kMaxExpanded)
                fail("the unpacked size is not believable");
            const uint8_t *p = c.take(have);
            Bytes body(p, p + have);
            cbsCrypt(body.data(), body.size());
            Bytes plain = inflateCapped(body, dlen);

            // Each file: created[8] modified[8] size u32 mode u16 u16 u32 u32 name[32] = 64 bytes, then the data.
            std::vector<SaveFile> files;
            size_t off = 0;
            while (off < plain.size())
            {
                if (plain.size() - off < 64)
                    fail("the file list ends too soon");
                if (files.size() >= kMaxFilesPerFolder)
                    fail("the file count is not believable");
                const uint8_t *e = plain.data() + off;
                const uint32_t size = rd32(e + 16);
                const uint16_t mode = rd16(e + 20);
                SaveFile f;
                f.name = cstr(e + 32, 32);
                off += 64;
                if (size > plain.size() - off)
                    fail("a file is cut short");
                if ((mode & kDfDir) != 0)
                    fail("the save holds a sub-folder, which a save cannot");
                f.data.assign(plain.begin() + static_cast<std::ptrdiff_t>(off), plain.begin() + static_cast<std::ptrdiff_t>(off + size));
                files.push_back(std::move(f));
                off += size;
            }
            std::vector<SaveEntry> out;
            out.push_back(makeEntry(dirname, "cbs", std::move(files)));
            return out;
        }

        // ---- .psv: PS3 export -------------------------------------------------------------------------------------------------
        constexpr uint8_t kPsvMagic[4] = {0, 'V', 'S', 'P'};

        struct PsvFileInfo
        {
            std::string name;
            uint32_t size = 0;
            uint32_t mode = 0;
        };

        // created[8] modified[8] size u32 mode u32 name[32]
        PsvFileInfo readPsvFileInfo(Cursor &c)
        {
            const uint8_t *p = c.take(56);
            PsvFileInfo f;
            f.size = rd32(p + 16);
            f.mode = rd32(p + 20);
            f.name = cstr(p + 24, 32);
            return f;
        }

        std::vector<SaveEntry> readPsv(const Bytes &b)
        {
            // magic[4] version u32 signature[40] pad[8] u32 save type u32   (0x40 bytes)
            Cursor c(b);
            c.skip(4);
            const uint32_t version = c.u32();
            c.skip(40 + 8 + 4);
            const uint32_t saveType = c.u32();
            if (version != 0)
                fail("this version of the format is not known");
            if (saveType == 1)
                fail("this holds a PlayStation 1 save, not a PS2 one");
            if (saveType != 2)
                fail("this kind of save is not known");

            // Ten u32: ?, then position and size of icon.sys and of three icons, then the number of files.
            c.skip(9 * 4);
            const uint32_t fileCount = c.u32();
            if (fileCount > kMaxFilesPerFolder)
                fail("the file count is not believable");
            const PsvFileInfo dir = readPsvFileInfo(c);
            if (!modeIsDir(dir.mode))
                fail("the folder header is damaged");

            std::vector<std::pair<PsvFileInfo, uint32_t>> infos;
            for (uint32_t i = 0; i < fileCount; ++i)
            {
                const PsvFileInfo f = readPsvFileInfo(c);
                const uint32_t offset = c.u32();
                if (modeIsDir(f.mode))
                    fail("the save holds a sub-folder, which a save cannot");
                infos.emplace_back(f, offset);
            }
            std::vector<SaveFile> files;
            for (const auto &[info, offset] : infos)
            {
                Cursor d(b);
                d.seek(offset);
                d.need(info.size);
                SaveFile f;
                f.name = info.name;
                const uint8_t *p = d.take(info.size);
                f.data.assign(p, p + info.size);
                files.push_back(std::move(f));
            }
            std::vector<SaveEntry> out;
            out.push_back(makeEntry(dir.name, "psv", std::move(files)));
            return out;
        }

        // ---- .ps2: a whole memory card image ----------------------------------------------------------------------------------
        constexpr char kCardMagic[] = "Sony PS2 Memory Card Format "; // 28 characters, the trailing space is part of it
        constexpr size_t kCardMagicLen = sizeof(kCardMagic) - 1;
        constexpr uint32_t kFatAllocated = 0x80000000u, kFatChainEnd = 0xFFFFFFFFu, kFatMask = 0x7FFFFFFFu;

        // The card's own file system, read only. Nothing here writes, repairs or checks ECC: the spare bytes after each page are skipped.
        class CardImage
        {
        public:
            explicit CardImage(const Bytes &img) : img_(img)
            {
                if (img.size() < 0x154 || std::memcmp(img.data(), kCardMagic, kCardMagicLen) != 0)
                    fail("this is not a memory card image");
                pageSize_ = rd16(&img[0x28]);
                pagesPerCluster_ = rd16(&img[0x2A]);
                clustersPerCard_ = rd32(&img[0x30]);
                allocOffset_ = rd32(&img[0x34]);
                allocEnd_ = rd32(&img[0x38]);
                rootCluster_ = rd32(&img[0x3C]);
                for (int i = 0; i < 32; ++i)
                    ifc_[i] = rd32(&img[0x50 + 4 * i]);

                if (pageSize_ != 512 && pageSize_ != 1024 && pageSize_ != 2048 && pageSize_ != 4096)
                    fail("the page size is not one a card has");
                if (pagesPerCluster_ == 0 || pagesPerCluster_ > 16)
                    fail("the cluster size is not one a card has");
                if (clustersPerCard_ == 0 || clustersPerCard_ > (uint32_t(1) << 24) || allocOffset_ >= clustersPerCard_ || allocEnd_ > clustersPerCard_)
                    fail("the card's size fields are not believable");
                clusterSize_ = size_t(pageSize_) * pagesPerCluster_;
                entriesPerCluster_ = clusterSize_ / 4;

                // The images people keep come with the 16 spare bytes of ECC after every 512-byte page, or without them. The file's size says which.
                const size_t spare = size_t((pageSize_ + 127) / 128) * 4;
                const uint64_t pages = uint64_t(clustersPerCard_) * pagesPerCluster_;
                const uint64_t eccSize = pages * (pageSize_ + spare);
                const uint64_t plainSize = pages * pageSize_;
                if (img.size() >= eccSize)
                {
                    rawPage_ = pageSize_ + spare;
                    ecc_ = true;
                }
                else if (img.size() >= plainSize)
                {
                    rawPage_ = pageSize_;
                    ecc_ = false;
                }
                else
                {
                    fail("the card image is cut short");
                }
            }

            // One entry for every folder in the root directory (a folder that cannot be read completely has `problem` set).
            std::vector<SaveEntry> readFolders() const
            {
                std::vector<Dirent> root = readDirectory(rootCluster_, 0, true);
                std::vector<SaveEntry> out;
                for (size_t i = 2; i < root.size(); ++i)
                {
                    const Dirent &d = root[i];
                    if (!modeIsDir(d.mode) || d.name == "." || d.name == "..")
                        continue;
                    try
                    {
                        std::vector<SaveFile> files;
                        if (d.length >= 2)
                        {
                            const std::vector<Dirent> entries = readDirectory(d.cluster, d.length, false);
                            for (size_t j = 2; j < entries.size(); ++j)
                            {
                                if (!modeIsFile(entries[j].mode))
                                    continue;
                                SaveFile f;
                                f.name = entries[j].name;
                                f.data = readData(entries[j].cluster, entries[j].length);
                                files.push_back(std::move(f));
                            }
                        }
                        out.push_back(makeEntry(d.name, "ps2", std::move(files)));
                    }
                    catch (const ReadFail &f)
                    {
                        SaveEntry e;
                        e.folder = d.name;
                        e.format = "ps2";
                        e.problem = "This folder on the card is damaged: " + f.message + ".";
                        out.push_back(std::move(e));
                    }
                }
                return out;
            }

        private:
            const Bytes &img_;
            uint32_t pageSize_ = 0, pagesPerCluster_ = 0, clustersPerCard_ = 0, allocOffset_ = 0, allocEnd_ = 0, rootCluster_ = 0;
            uint32_t ifc_[32] = {};
            size_t rawPage_ = 0, clusterSize_ = 0, entriesPerCluster_ = 0;
            bool ecc_ = false;

            // The two most recent FAT-level clusters, so that a long chain does not re-read them for every link.
            mutable uint32_t cachedIndirect_ = kFatChainEnd, cachedFat_ = kFatChainEnd;
            mutable Bytes indirectBuf_, fatBuf_;

            // A cluster by its number on the card (not counting the allocatable offset).
            void readCluster(uint32_t n, Bytes &dst) const
            {
                if (n >= clustersPerCard_)
                    fail("a cluster number is outside the card");
                dst.resize(clusterSize_);
                if (!ecc_)
                {
                    std::memcpy(dst.data(), img_.data() + size_t(n) * clusterSize_, clusterSize_);
                    return;
                }
                for (uint32_t p = 0; p < pagesPerCluster_; ++p)
                {
                    const size_t page = size_t(n) * pagesPerCluster_ + p;
                    std::memcpy(dst.data() + size_t(p) * pageSize_, img_.data() + page * rawPage_, pageSize_);
                }
            }

            uint32_t entryAt(const Bytes &cluster, size_t index) const { return rd32(cluster.data() + index * 4); }

            // The FAT entry of allocatable cluster n.
            uint32_t fatLookup(uint32_t n) const
            {
                if (n >= allocEnd_)
                    fail("a FAT index is outside the card");
                const size_t fatIndex = n / entriesPerCluster_;
                const size_t offset = n % entriesPerCluster_;
                const size_t dbl = fatIndex / entriesPerCluster_;
                if (dbl >= 32)
                    fail("a FAT index is outside the card");
                // (A buffer is only trusted when it holds a cluster: an unset cache must not match a bad cluster number.)
                const uint32_t indirect = ifc_[dbl];
                if (indirectBuf_.empty() || indirect != cachedIndirect_)
                {
                    readCluster(indirect, indirectBuf_);
                    cachedIndirect_ = indirect;
                }
                const uint32_t fatCluster = entryAt(indirectBuf_, fatIndex % entriesPerCluster_);
                if (fatBuf_.empty() || fatCluster != cachedFat_)
                {
                    readCluster(fatCluster, fatBuf_);
                    cachedFat_ = fatCluster;
                }
                return entryAt(fatBuf_, offset);
            }

            // The clusters of a file or folder, in order.
            std::vector<uint32_t> chain(uint32_t first, size_t wanted) const
            {
                std::vector<uint32_t> out;
                uint32_t cur = first;
                while (out.size() < wanted)
                {
                    if (cur >= allocEnd_)
                        fail("a cluster chain leaves the card");
                    out.push_back(cur);
                    if (out.size() >= wanted)
                        break;
                    const uint32_t next = fatLookup(cur);
                    if (next == kFatChainEnd || (next & kFatAllocated) == 0)
                        fail("a cluster chain ends too soon");
                    cur = next & kFatMask;
                }
                return out;
            }

            Bytes readData(uint32_t first, uint64_t length) const
            {
                if (length == 0)
                    return {};
                if (length > uint64_t(clustersPerCard_) * clusterSize_)
                    fail("a file is longer than the card");
                const size_t clusters = static_cast<size_t>((length + clusterSize_ - 1) / clusterSize_);
                const std::vector<uint32_t> list = chain(first, clusters);
                Bytes out;
                out.reserve(clusters * clusterSize_);
                Bytes tmp;
                for (const uint32_t n : list)
                {
                    readCluster(n + allocOffset_, tmp);
                    out.insert(out.end(), tmp.begin(), tmp.end());
                }
                out.resize(static_cast<size_t>(length));
                return out;
            }

            // The entries of a directory. For the root the length is not known yet: it is the length field of the first entry (".").
            std::vector<Dirent> readDirectory(uint32_t first, uint32_t count, bool isRoot) const
            {
                if (isRoot)
                {
                    const Bytes head = readData(first, kDirentSize);
                    const Dirent dot = parseDirent(head.data());
                    if (!modeIsDir(dot.mode) || dot.name != "." || dot.length < 2)
                        fail("the root directory is damaged");
                    count = dot.length;
                }
                if (count > kMaxRootEntries)
                    fail("a directory is not believably long");
                const Bytes raw = readData(first, uint64_t(count) * kDirentSize);
                std::vector<Dirent> out;
                out.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                    out.push_back(parseDirent(raw.data() + size_t(i) * kDirentSize));
                if (isRoot && (out.size() < 2 || out[1].name != ".." || !modeIsDir(out[1].mode)))
                    fail("the root directory is damaged");
                return out;
            }
        };

        std::vector<SaveEntry> readCard(const Bytes &b) { return CardImage(b).readFolders(); }

        // ---- a folder someone already extracted -------------------------------------------------------------------------------
        std::string utf8(const fs::path &p)
        {
            const auto u = p.u8string();
            return std::string(u.begin(), u.end());
        }

        bool readWholeFile(const fs::path &path, Bytes &out)
        {
            std::error_code ec;
            const uintmax_t size = fs::file_size(path, ec);
            if (ec || size > kMaxContainerBytes)
                return false;
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            out.resize(static_cast<size_t>(size));
            if (size != 0)
                in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size));
            return static_cast<uintmax_t>(in.gcount()) == size;
        }

        // ---- finding out what a file is ---------------------------------------------------------------------------------------
        enum class Kind
        {
            None,
            Raw,
            Max,
            Psu,
            Sps,
            Cbs,
            Psv,
            Card,
            Vme
        };

        bool startsWith(const Bytes &b, const uint8_t *magic, size_t n) { return b.size() >= n && std::memcmp(b.data(), magic, n) == 0; }

        Kind detect(const Bytes &b, const std::string &nameHint)
        {
            static const uint8_t kMaxMagic[12] = {'P', 's', '2', 'P', 'o', 'w', 'e', 'r', 'S', 'a', 'v', 'e'};
            if (startsWith(b, kMaxMagic, sizeof(kMaxMagic)))
                return Kind::Max;
            if (startsWith(b, kCbsMagic, sizeof(kCbsMagic)))
                return Kind::Cbs;
            if (startsWith(b, kSpsMagic, sizeof(kSpsMagic)))
                return Kind::Sps;
            if (startsWith(b, kPsvMagic, sizeof(kPsvMagic)))
                return Kind::Psv;
            if (startsWith(b, reinterpret_cast<const uint8_t *>(kCardMagic), kCardMagicLen))
                return Kind::Card;
            // A PS3 ".vme" is a card image behind a 0x80-byte header.
            if (b.size() >= 0x80 + kCardMagicLen && std::memcmp(b.data() + 0x80, kCardMagic, kCardMagicLen) == 0)
                return Kind::Vme;
            if (endsWith(lower(nameHint), ".vme"))
                return Kind::Vme;
            if (b.size() == kRawSaveSize)
                return Kind::Raw;
            if (looksLikePsu(b))
                return Kind::Psu;
            return Kind::None;
        }

        const char *kindName(Kind k)
        {
            switch (k)
            {
            case Kind::Raw: return "raw";
            case Kind::Max: return "max";
            case Kind::Psu: return "psu";
            case Kind::Sps: return "sps";
            case Kind::Cbs: return "cbs";
            case Kind::Psv: return "psv";
            case Kind::Card: return "ps2";
            case Kind::Vme: return "vme";
            case Kind::None: break;
            }
            return "";
        }

        const char *kindLabel(Kind k)
        {
            switch (k)
            {
            case Kind::Raw: return "Save file";
            case Kind::Max: return "MAX save";
            case Kind::Psu: return "PSU save";
            case Kind::Sps: return "XPS/SPS save";
            case Kind::Cbs: return "CBS save";
            case Kind::Psv: return "PSV save";
            case Kind::Card: return "Memory card image";
            default: break;
            }
            return "Save";
        }
    } // namespace

    std::string saveContainerKind(const std::vector<uint8_t> &bytes, const std::string &nameHint) { return kindName(detect(bytes, nameHint)); }

    bool readSaveContainerBytes(const std::vector<uint8_t> &bytes, const std::string &nameHint, std::vector<SaveEntry> &out, std::string &error)
    {
        out.clear();
        error.clear();
        Kind kind = Kind::None;
        try
        {
            kind = detect(bytes, nameHint);
            switch (kind)
            {
            case Kind::Max:
                out = readMax(bytes);
                break;
            case Kind::Psu:
                out = readPsu(bytes);
                break;
            case Kind::Sps:
                out = readSps(bytes);
                break;
            case Kind::Cbs:
                out = readCbs(bytes);
                break;
            case Kind::Psv:
                out = readPsv(bytes);
                break;
            case Kind::Card:
                out = readCard(bytes);
                break;
            case Kind::Raw:
            {
                SaveEntry e;
                e.format = "raw";
                e.save = bytes;
                e.saveName = nameHint;
                if (looksLikeFolderName(nameHint))
                    e.folder = nameHint;
                out.push_back(std::move(e));
                break;
            }
            case Kind::Vme:
                error = "That is a PS3 .vme memory card, which cannot be read directly. Convert it to .ps2 first "
                        "(C-Drive's Fire Pro R Save Conversion Kit does this).";
                return false;
            case Kind::None:
                error = "That is not a save file this importer can read (.max, .psu, .xps, .sps, .cbs, .psv, .ps2 or a raw save).";
                return false;
            }
            return true;
        }
        catch (const ReadFail &f)
        {
            out.clear();
            error = std::string(kindLabel(kind)) + " is not readable: " + f.message + ".";
        }
        catch (const std::bad_alloc &)
        {
            out.clear();
            error = "There is not enough memory to read that file.";
        }
        catch (const std::exception &)
        {
            out.clear();
            error = std::string(kindLabel(kind)) + " could not be read.";
        }
        return false;
    }

    bool readSaveContainer(const std::filesystem::path &path, std::vector<SaveEntry> &out, std::string &error)
    {
        out.clear();
        error.clear();
        try
        {
            std::error_code ec;
            if (fs::is_directory(path, ec))
            {
                std::vector<fs::path> dirs;
                std::vector<fs::path> own;
                for (const auto &it : fs::directory_iterator(path, ec))
                {
                    std::error_code ec2;
                    if (it.is_directory(ec2))
                        dirs.push_back(it.path());
                    else if (it.is_regular_file(ec2))
                        own.push_back(it.path());
                }
                if (ec)
                {
                    error = "That folder cannot be read.";
                    return false;
                }
                const auto byName = [](const fs::path &a, const fs::path &b) { return utf8(a.filename()) < utf8(b.filename()); };
                std::sort(dirs.begin(), dirs.end(), byName);
                std::sort(own.begin(), own.end(), byName);

                // The files of one folder, as an entry named after the folder.
                const auto folderEntry = [&](const fs::path &dir, const std::vector<fs::path> &names) -> bool {
                    if (names.size() > kMaxFilesPerFolder)
                    {
                        error = "That folder holds too many files to be a save.";
                        return false;
                    }
                    std::vector<SaveFile> files;
                    for (const fs::path &p : names)
                    {
                        SaveFile f;
                        f.name = utf8(p.filename());
                        if (!readWholeFile(p, f.data))
                        {
                            error = "A file in that folder cannot be read.";
                            return false;
                        }
                        files.push_back(std::move(f));
                    }
                    const fs::path leaf = dir.has_filename() ? dir.filename() : dir.parent_path().filename();
                    out.push_back(makeEntry(utf8(leaf), "folder", std::move(files)));
                    return true;
                };

                if (dirs.empty())
                    return folderEntry(path, own);
                // A folder of save folders (a PCSX2 folder memory card, or several extracted saves): one entry for each sub-folder.
                // Files lying beside the sub-folders are not saves and are left alone.
                for (const fs::path &d : dirs)
                {
                    std::vector<fs::path> names;
                    std::error_code ec3;
                    for (const auto &it : fs::directory_iterator(d, ec3))
                    {
                        std::error_code ec4;
                        if (it.is_regular_file(ec4))
                            names.push_back(it.path());
                    }
                    if (ec3)
                    {
                        error = "That folder cannot be read.";
                        out.clear();
                        return false;
                    }
                    std::sort(names.begin(), names.end(), byName);
                    if (!folderEntry(d, names))
                    {
                        out.clear();
                        return false;
                    }
                }
                return true;
            }

            Bytes bytes;
            if (!readWholeFile(path, bytes))
            {
                std::error_code sizeEc;
                const uintmax_t size = fs::exists(path, sizeEc) ? fs::file_size(path, sizeEc) : 0;
                error = (!sizeEc && size > kMaxContainerBytes) ? "That file is far too large to be a save." : "That file cannot be read.";
                return false;
            }
            const std::string name = utf8(path.filename());
            if (!readSaveContainerBytes(bytes, name, out, error))
                return false;
            // A raw save with a name of its own was named by readSaveContainerBytes; otherwise the folder it sits in may name it.
            if (out.size() == 1 && out[0].format == "raw" && out[0].folder.empty())
            {
                const std::string parent = utf8(path.parent_path().filename());
                if (looksLikeFolderName(parent))
                    out[0].folder = parent;
            }
            return true;
        }
        catch (const std::bad_alloc &)
        {
            out.clear();
            error = "There is not enough memory to read that file.";
        }
        catch (const std::exception &)
        {
            out.clear();
            error = "That file cannot be read.";
        }
        return false;
    }
} // namespace ps2x::mods
