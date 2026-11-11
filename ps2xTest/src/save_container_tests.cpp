#include "MiniTest.h"

#include "save_container.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Tests for the save container readers (src/lib/mods/save_container): raw save, .max, .psu, .xps/.sps, .cbs, .psv and .ps2 card images.
//
// Nothing here contains game data. The containers are built in memory by small writers in this file (the writers are the tests' own, and
// are not part of the product), plus two files made by mymcplus (the formats' reference tool), kept as byte lists: a tiny .max and a
// .cbs. The last suite reads the player's own real files under baserom/ (a community .max and Seth's .ps2 card images) and compares them
// with what mymcplus extracted earlier; it says "skipped" when they are not there.

namespace
{
    namespace fs = std::filesystem;
    using namespace ps2x::mods;
    using Bytes = std::vector<uint8_t>;

    // ---- small helpers --------------------------------------------------------------------------------------------------------
    void put16(Bytes &b, uint32_t v)
    {
        b.push_back(static_cast<uint8_t>(v));
        b.push_back(static_cast<uint8_t>(v >> 8));
    }
    void put32(Bytes &b, uint32_t v)
    {
        put16(b, v & 0xFFFF);
        put16(b, v >> 16);
    }
    void putAt32(Bytes &b, size_t at, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            b[at + i] = static_cast<uint8_t>(v >> (8 * i));
    }
    void putText(Bytes &b, const std::string &text, size_t width)
    {
        for (size_t i = 0; i < width; ++i)
            b.push_back(i < text.size() ? static_cast<uint8_t>(text[i]) : 0);
    }
    void append(Bytes &b, const Bytes &more) { b.insert(b.end(), more.begin(), more.end()); }

    Bytes bytesOf(const std::string &s) { return Bytes(s.begin(), s.end()); }

    // Deterministic noise.
    Bytes pattern(size_t n, uint32_t seed)
    {
        Bytes b(n);
        uint32_t s = seed * 2654435761u + 12345u;
        for (uint8_t &x : b)
        {
            s = s * 1664525u + 1013904223u;
            x = static_cast<uint8_t>(s >> 24);
        }
        return b;
    }

    // Deterministic text-like data that has a lot of repeats, so that the LZARI matcher has something to find.
    Bytes compressible(size_t n, uint32_t seed)
    {
        static const char *words[] = {"wrestler ", "ring ", "belt ", "ROPE ", "match-", "0123456789 ", "Fire Pro ", "tag team ", "\x01\x02\x03\x04"};
        Bytes b;
        uint32_t s = seed * 2654435761u + 99u;
        while (b.size() < n)
        {
            s = s * 1664525u + 1013904223u;
            const char *w = words[(s >> 24) % 9];
            b.insert(b.end(), w, w + std::strlen(w));
            if ((s >> 16) % 11 == 0) // a little noise now and then
                b.push_back(static_cast<uint8_t>(s >> 8));
        }
        b.resize(n);
        return b;
    }

    bool oneLine(const std::string &s) { return s.find('\n') == std::string::npos && s.find('\r') == std::string::npos; }
    bool contains(const std::string &hay, const std::string &needle) { return hay.find(needle) != std::string::npos; }

    // A save folder as the tests build it and expect it back.
    struct F
    {
        std::string name;
        Bytes data;
    };
    struct Folder
    {
        std::string name;
        std::vector<F> files;
    };

    // The folder a game would write: the save file named like the folder, then icon.sys and view.ico.
    Folder sampleFolder(const std::string &name, size_t mainSize, uint32_t seed)
    {
        Folder f;
        f.name = name;
        f.files.push_back({name, compressible(mainSize, seed)});
        f.files.push_back({"icon.sys", pattern(964, seed + 1)});
        f.files.push_back({"view.ico", pattern(2500, seed + 2)});
        return f;
    }

    // Checks that an entry holds exactly the folder: its name, the main file, the other files in order.
    void expectSame(TestCase &t, const SaveEntry &e, const Folder &f, const std::string &label)
    {
        t.IsTrue(e.folder == f.name, label + ": folder name '" + e.folder + "'");
        t.IsTrue(e.problem.empty(), label + ": problem '" + e.problem + "'");
        t.IsTrue(!f.files.empty() && e.save == f.files[0].data, label + ": main file differs");
        t.IsTrue(!f.files.empty() && e.saveName == f.files[0].name, label + ": main file name '" + e.saveName + "'");
        t.IsTrue(e.others.size() + 1 == f.files.size(), label + ": file count");
        for (size_t i = 0; i + 1 < f.files.size() && i < e.others.size(); ++i)
        {
            t.IsTrue(e.others[i].name == f.files[i + 1].name, label + ": name of file " + std::to_string(i + 1) + " is '" + e.others[i].name + "'");
            t.IsTrue(e.others[i].data == f.files[i + 1].data, label + ": data of file " + std::to_string(i + 1));
        }
    }

    // Reads and checks the call's own contract: false comes with a one-line message, true with an empty one.
    bool readOk(TestCase &t, const Bytes &bytes, const std::string &hint, std::vector<SaveEntry> &out, const std::string &label)
    {
        std::string error;
        const bool ok = readSaveContainerBytes(bytes, hint, out, error);
        if (ok)
            t.IsTrue(error.empty(), label + ": a message with a success: " + error);
        else
        {
            t.IsTrue(!error.empty() && oneLine(error), label + ": the refusal is not one line: '" + error + "'");
            t.IsTrue(out.empty(), label + ": entries with a refusal");
        }
        return ok;
    }

    // ---- writers (the tests' own) ---------------------------------------------------------------------------------------------
    constexpr uint32_t kDirMode = 0x8427, kFileMode = 0x8497;

    Bytes dirent(uint32_t mode, uint32_t length, uint32_t cluster, const std::string &name)
    {
        Bytes e;
        put16(e, mode);
        put16(e, 0);
        put32(e, length);
        const uint8_t tod[8] = {0, 5, 4, 3, 10, 11, 0xEA, 0x07}; // 2026-11-10 03:04:05
        e.insert(e.end(), tod, tod + 8);
        put32(e, cluster);
        put32(e, 0);
        e.insert(e.end(), tod, tod + 8);
        put32(e, 0);
        e.resize(64, 0);
        putText(e, name, 448);
        return e;
    }

    // .psu: the folder, ".", "..", then each file with its entry and its data padded to 1024 bytes.
    Bytes makePsu(const Folder &f, bool padLast)
    {
        Bytes b;
        append(b, dirent(kDirMode, static_cast<uint32_t>(f.files.size() + 2), 0, f.name));
        append(b, dirent(kDirMode, 0, 0, "."));
        append(b, dirent(kDirMode, 0, 0, ".."));
        for (size_t i = 0; i < f.files.size(); ++i)
        {
            append(b, dirent(kFileMode, static_cast<uint32_t>(f.files[i].data.size()), 0, f.files[i].name));
            append(b, f.files[i].data);
            if (padLast || i + 1 < f.files.size())
                b.resize(b.size() + (((f.files[i].data.size() + 1023) & ~size_t(1023)) - f.files[i].data.size()), 0); // the data to 1024 bytes
        }
        return b;
    }

    // .xps/.sps: "SharkPortSave", type, three long strings, length, then each entry as a 98-byte header (plus `extra` more) and its data.
    Bytes makeSps(const Folder &f, size_t extra)
    {
        auto entry = [&](const std::string &name, uint32_t length, uint32_t mode) {
            Bytes e;
            put16(e, static_cast<uint32_t>(98 + extra));
            putText(e, name, 64);
            put32(e, length);
            e.resize(e.size() + 8, 0);
            put16(e, ((mode & 0xFF) << 8) | (mode >> 8)); // the mode is stored with its bytes swapped
            put16(e, 0);
            const uint8_t tod[8] = {0, 5, 4, 3, 10, 11, 0xEA, 0x07};
            e.insert(e.end(), tod, tod + 8);
            e.insert(e.end(), tod, tod + 8);
            e.resize(98 + extra, 0xEE);
            return e;
        };
        auto longString = [](Bytes &b, const std::string &s) {
            put32(b, static_cast<uint32_t>(s.size()));
            b.insert(b.end(), s.begin(), s.end());
        };
        Bytes b = {0x0d, 0, 0, 0, 'S', 'h', 'a', 'r', 'k', 'P', 'o', 'r', 't', 'S', 'a', 'v', 'e'};
        put32(b, 2);
        longString(b, f.name);
        longString(b, "2026-11-10");
        longString(b, "made by the unit test");
        put32(b, 0x1234);
        append(b, entry(f.name, static_cast<uint32_t>(f.files.size() + 2), kDirMode));
        for (const F &file : f.files)
        {
            append(b, entry(file.name, static_cast<uint32_t>(file.data.size()), kFileMode));
            append(b, file.data);
        }
        put32(b, 0); // the checksum nobody checks
        return b;
    }

    // .psv (PS2 type): header, ten u32, the folder's info, each file's info and offset, then the data.
    Bytes makePsv(const Folder &f, uint32_t saveType)
    {
        Bytes b = {0, 'V', 'S', 'P'};
        put32(b, 0);
        b.resize(b.size() + 40 + 8, 0x5A);
        put32(b, 0x2C);
        put32(b, saveType);
        const size_t n = f.files.size();
        const size_t infoSize = 56;
        const size_t dataStart = 64 + 40 + infoSize + n * (infoSize + 4);
        put32(b, 0x2C);
        for (int i = 0; i < 8; ++i)
            put32(b, 0);
        put32(b, static_cast<uint32_t>(n));
        auto info = [&](uint32_t size, uint32_t mode, const std::string &name) {
            const uint8_t tod[8] = {0, 5, 4, 3, 10, 11, 0xEA, 0x07};
            b.insert(b.end(), tod, tod + 8);
            b.insert(b.end(), tod, tod + 8);
            put32(b, size);
            put32(b, mode);
            putText(b, name, 32);
        };
        info(static_cast<uint32_t>(n + 2), kDirMode, f.name);
        size_t offset = dataStart;
        for (const F &file : f.files)
        {
            info(static_cast<uint32_t>(file.data.size()), kFileMode, file.name);
            put32(b, static_cast<uint32_t>(offset));
            offset += file.data.size();
        }
        for (const F &file : f.files)
            append(b, file.data);
        return b;
    }

    // LZARI, encoding only, so that the tests can make .max files of any size. A literal or a match (3..60 bytes, up to 4000 back)
    // per step; the model is Haruhiko Okumura's, the same one the reader has to follow.
    class LzariEncoder
    {
    public:
        static Bytes encode(const Bytes &src)
        {
            LzariEncoder e;
            e.start();
            size_t pos = 0;
            while (pos < src.size())
            {
                size_t bestLen = 0, bestDist = 0;
                const size_t maxDist = std::min<size_t>(pos, 4000);
                const size_t maxLen = std::min<size_t>(60, src.size() - pos);
                for (size_t d = 1; d <= maxDist; ++d)
                {
                    size_t l = 0;
                    while (l < maxLen && src[pos + l] == src[pos + l - d])
                        ++l;
                    if (l > bestLen)
                    {
                        bestLen = l;
                        bestDist = d;
                        if (l == maxLen)
                            break;
                    }
                }
                if (bestLen >= 3)
                {
                    e.encodeChar(static_cast<int>(253 + bestLen));
                    e.encodePosition(static_cast<int>(bestDist - 1));
                    pos += bestLen;
                }
                else
                {
                    e.encodeChar(src[pos]);
                    ++pos;
                }
            }
            e.end();
            return e.out_;
        }

    private:
        static constexpr int N = 4096, NCHAR = 314;
        static constexpr uint64_t Q1 = 1u << 15, Q2 = 2 * Q1, Q3 = 3 * Q1, Q4 = 4 * Q1;
        static constexpr uint32_t MAXCUM = (1u << 15) - 1;
        uint32_t charToSym_[NCHAR], symToChar_[NCHAR + 1], symFreq_[NCHAR + 1], symCum_[NCHAR + 1], posCum_[N + 1];
        uint64_t low_ = 0, high_ = Q4;
        int shifts_ = 0;
        Bytes out_;
        unsigned buf_ = 0, mask_ = 128;

        void putBit(int bit)
        {
            if (bit)
                buf_ |= mask_;
            mask_ >>= 1;
            if (mask_ == 0)
            {
                out_.push_back(static_cast<uint8_t>(buf_));
                buf_ = 0;
                mask_ = 128;
            }
        }
        void outputBit(int bit)
        {
            putBit(bit);
            for (; shifts_ > 0; --shifts_)
                putBit(!bit);
        }
        void start()
        {
            symCum_[NCHAR] = 0;
            for (int sym = NCHAR; sym >= 1; --sym)
            {
                charToSym_[sym - 1] = static_cast<uint32_t>(sym);
                symToChar_[sym] = static_cast<uint32_t>(sym - 1);
                symFreq_[sym] = 1;
                symCum_[sym - 1] = symCum_[sym] + symFreq_[sym];
            }
            symFreq_[0] = 0;
            posCum_[N] = 0;
            for (int i = N; i >= 1; --i)
                posCum_[i - 1] = posCum_[i] + 10000u / static_cast<uint32_t>(200 + i);
        }
        void updateModel(int sym)
        {
            if (symCum_[0] >= MAXCUM)
            {
                uint32_t c = 0;
                for (int i = NCHAR; i > 0; --i)
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
            {
                const uint32_t chI = symToChar_[i], chSym = symToChar_[sym];
                symToChar_[i] = chSym;
                symToChar_[sym] = chI;
                charToSym_[chI] = static_cast<uint32_t>(sym);
                charToSym_[chSym] = static_cast<uint32_t>(i);
            }
            symFreq_[i]++;
            while (--i >= 0)
                symCum_[i]++;
        }
        void scale()
        {
            for (;;)
            {
                if (high_ <= Q2)
                    outputBit(0);
                else if (low_ >= Q2)
                {
                    outputBit(1);
                    low_ -= Q2;
                    high_ -= Q2;
                }
                else if (low_ >= Q1 && high_ <= Q3)
                {
                    ++shifts_;
                    low_ -= Q1;
                    high_ -= Q1;
                }
                else
                    break;
                low_ += low_;
                high_ += high_;
            }
        }
        void encodeChar(int ch)
        {
            const int sym = static_cast<int>(charToSym_[ch]);
            const uint64_t range = high_ - low_;
            high_ = low_ + range * symCum_[sym - 1] / symCum_[0];
            low_ += range * symCum_[sym] / symCum_[0];
            scale();
            updateModel(sym);
        }
        void encodePosition(int position)
        {
            const uint64_t range = high_ - low_;
            high_ = low_ + range * posCum_[position] / posCum_[0];
            low_ += range * posCum_[position + 1] / posCum_[0];
            scale();
        }
        void end()
        {
            ++shifts_;
            outputBit(low_ < Q1 ? 0 : 1);
            if (mask_ != 128)
                out_.push_back(static_cast<uint8_t>(buf_));
        }
    };

    // .max: header (0x5C bytes), then the LZARI-packed list of (length, name[32], data), each file ending on (offset + 8) % 16 == 0.
    Bytes makeMax(const Folder &f, bool clenIsLength)
    {
        Bytes plain;
        for (const F &file : f.files)
        {
            put32(plain, static_cast<uint32_t>(file.data.size()));
            putText(plain, file.name, 32);
            append(plain, file.data);
            plain.resize(((plain.size() + 8 + 15) & ~size_t(15)) - 8, 0);
        }
        const Bytes packed = LzariEncoder::encode(plain);
        Bytes b = bytesOf("Ps2PowerSave");
        put32(b, 0);
        putText(b, f.name, 32);
        putText(b, "test save", 32);
        put32(b, clenIsLength ? static_cast<uint32_t>(plain.size()) : static_cast<uint32_t>(packed.size() + 4));
        put32(b, static_cast<uint32_t>(f.files.size()));
        put32(b, static_cast<uint32_t>(plain.size()));
        append(b, packed);
        return b;
    }

    // .ps2: a memory card image, 1024-byte clusters (two 512-byte pages). The layout is the real one: the superblock in page 0, the
    // indirect FAT cluster(s) from 8, the FAT clusters after them, then the allocatable clusters (from 41 on an 8 MB card), the root
    // directory in allocatable cluster 0.
    struct CardOptions
    {
        bool ecc = true;          // 16 spare bytes after every page, as the emulator's images have
        bool scatter = false;     // give clusters out in a scrambled order, so every chain jumps about
        uint32_t stride = 37;     // the scrambling: the k-th cluster given out is 1 + (k-1)*stride mod (allocatable clusters - 1)
        uint32_t *highest = nullptr; // if set, receives the highest allocatable cluster number given out
        uint32_t clusters = 8192; // clusters on the card (8192 = 8 MB)
        std::string breakFolder;  // cut the cluster chain of this folder's main file after its first cluster
    };

    Bytes makeCard(const std::vector<Folder> &folders, const CardOptions &o)
    {
        constexpr uint32_t cl = 1024;
        const uint32_t total = o.clusters;
        const uint32_t fatEstimate = (total + 255) / 256;
        const uint32_t ifcClusters = (fatEstimate + 255) / 256;       // 1 up to a 64 MB card
        const uint32_t allocOffset = 8 + ifcClusters + fatEstimate;   // 41 for an 8 MB card, as on a real one
        const uint32_t allocEnd = total - allocOffset - 16;           // 8135 for an 8 MB card
        Bytes img(size_t(total) * cl, 0);
        const uint32_t fatClusters = (allocEnd + 255) / 256;
        std::vector<uint32_t> fat(size_t(fatClusters) * 256, 0x7FFFFFFFu);
        uint32_t nextIdx = 0;
        auto alloc = [&]() -> uint32_t {
            const uint32_t k = nextIdx++;
            if (k >= allocEnd)
                throw std::runtime_error("test card is full");
            const uint32_t idx = (o.scatter && k > 0) ? 1 + uint32_t((uint64_t(k - 1) * o.stride) % (allocEnd - 1)) : k;
            if (o.highest != nullptr)
                *o.highest = std::max(*o.highest, idx);
            return idx;
        };
        auto chainOf = [&](size_t bytes, bool cut) {
            std::vector<uint32_t> c;
            const size_t n = std::max<size_t>(1, (bytes + cl - 1) / cl);
            for (size_t i = 0; i < n; ++i)
                c.push_back(alloc());
            for (size_t i = 0; i < n; ++i)
                fat[c[i]] = (i + 1 < n && !(cut && i == 0)) ? (c[i + 1] | 0x80000000u) : 0xFFFFFFFFu;
            return c;
        };
        auto fill = [&](const std::vector<uint32_t> &chain, const Bytes &data) {
            for (size_t i = 0; i < chain.size(); ++i)
            {
                const size_t from = i * cl;
                if (from >= data.size())
                    break;
                std::memcpy(&img[size_t(allocOffset + chain[i]) * cl], &data[from], std::min<size_t>(cl, data.size() - from));
            }
        };

        const size_t rootEntries = 2 + folders.size();
        const std::vector<uint32_t> rootChain = chainOf(rootEntries * 512, false); // allocatable cluster 0
        Bytes root;
        append(root, dirent(kDirMode, static_cast<uint32_t>(rootEntries), rootChain[0], "."));
        append(root, dirent(kDirMode, 0, 0, ".."));
        for (const Folder &folder : folders)
        {
            std::vector<uint32_t> firsts;
            for (size_t i = 0; i < folder.files.size(); ++i)
            {
                const F &file = folder.files[i];
                if (file.data.empty())
                {
                    firsts.push_back(0xFFFFFFFFu);
                    continue;
                }
                const bool cut = (i == 0 && folder.name == o.breakFolder && file.data.size() > 2 * cl);
                const std::vector<uint32_t> c = chainOf(file.data.size(), cut);
                fill(c, file.data);
                firsts.push_back(c[0]);
            }
            const size_t entries = 2 + folder.files.size();
            const std::vector<uint32_t> dirChain = chainOf(entries * 512, false);
            Bytes dir;
            append(dir, dirent(kDirMode, static_cast<uint32_t>(entries), dirChain[0], "."));
            append(dir, dirent(kDirMode, 0, rootChain[0], ".."));
            for (size_t i = 0; i < folder.files.size(); ++i)
                append(dir, dirent(kFileMode, static_cast<uint32_t>(folder.files[i].data.size()), firsts[i], folder.files[i].name));
            fill(dirChain, dir);
            append(root, dirent(kDirMode, static_cast<uint32_t>(entries), dirChain[0], folder.name));
        }
        fill(rootChain, root);

        // The indirect FAT clusters (from 8) list the FAT clusters (after them); the FAT clusters hold `fat`.
        for (uint32_t i = 0; i < fatClusters; ++i)
        {
            const uint32_t fatCluster = 8 + ifcClusters + i;
            putAt32(img, size_t(8 + i / 256) * cl + 4 * (i % 256), fatCluster);
            for (uint32_t k = 0; k < 256; ++k)
                putAt32(img, size_t(fatCluster) * cl + 4 * k, fat[size_t(i) * 256 + k]);
        }
        const std::string magic = "Sony PS2 Memory Card Format ";
        std::memcpy(img.data(), magic.data(), magic.size());
        std::memcpy(img.data() + 28, "1.2.0.0", 7);
        auto sb16 = [&](size_t at, uint32_t v) { img[at] = static_cast<uint8_t>(v); img[at + 1] = static_cast<uint8_t>(v >> 8); };
        sb16(0x28, 512);
        sb16(0x2A, 2);
        sb16(0x2C, 16);
        sb16(0x2E, 0xFF00);
        putAt32(img, 0x30, total);
        putAt32(img, 0x34, allocOffset);
        putAt32(img, 0x38, allocEnd);
        putAt32(img, 0x3C, 0);
        putAt32(img, 0x40, total / 16 - 1);
        putAt32(img, 0x44, total / 16 - 2);
        for (uint32_t i = 0; i < 32; ++i) // indirect_fat_cluster_list
            putAt32(img, 0x50 + 4 * i, i < ifcClusters ? 8 + i : 0);
        img[0x150] = 2; // card type: PS2
        img[0x151] = 0x2B;

        if (!o.ecc)
            return img;
        Bytes withEcc;
        withEcc.reserve(img.size() / 512 * 528);
        for (size_t p = 0; p < img.size() / 512; ++p)
        {
            withEcc.insert(withEcc.end(), img.begin() + p * 512, img.begin() + (p + 1) * 512);
            for (int i = 0; i < 16; ++i)
                withEcc.push_back(static_cast<uint8_t>(0xA0 + i)); // the reader does not look at the spare bytes
        }
        return withEcc;
    }

    // ---- known answers made by mymcplus ---------------------------------------------------------------------------------------
    // A .max that mymcplus 3.0.5 wrote: folder BATEST-00001, files BATEST-00001 (kMaxMain) and icon.sys (kMaxIcon).
    Bytes maxMain()
    {
        Bytes b;
        for (int i = 0; i < 6; ++i)
            append(b, bytesOf("FPR-TEST-SAVE "));
        for (int i = 0; i < 64; ++i)
            b.push_back(static_cast<uint8_t>(i));
        for (int i = 0; i < 5; ++i)
            append(b, bytesOf("abcabcabcabc"));
        b.resize(b.size() + 40, 0);
        for (int i = 0; i < 3; ++i)
            append(b, bytesOf("The quick brown fox jumps over the lazy dog. "));
        return b;
    }
    Bytes maxIcon()
    {
        Bytes b;
        for (int i = 0; i < 4; ++i)
            append(b, bytesOf("icon.sys test "));
        return b;
    }
    const uint8_t kMaxKnown[] = {
        0x50, 0x73, 0x32, 0x50, 0x6f, 0x77, 0x65, 0x72, 0x53, 0x61, 0x76, 0x65, 0xe0, 0xa8, 0x64, 0xdf,
        0x42, 0x41, 0x54, 0x45, 0x53, 0x54, 0x2d, 0x30, 0x30, 0x30, 0x30, 0x31, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xd2, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x02, 0x00, 0x00, 0x98, 0x73, 0x67, 0xcc,
        0x12, 0x79, 0xc7, 0xac, 0xca, 0x21, 0x71, 0xde, 0xf6, 0x56, 0xed, 0x5f, 0x40, 0x56, 0xe9, 0x0a,
        0x7d, 0x5e, 0x72, 0x19, 0xcd, 0x33, 0xc6, 0xb6, 0xbe, 0x30, 0x92, 0xc1, 0xa6, 0xa7, 0x12, 0x01,
        0x2a, 0xf4, 0x92, 0xa5, 0xbb, 0x6e, 0x2a, 0x27, 0xfc, 0xcc, 0x31, 0xba, 0xcc, 0x88, 0x66, 0x0b,
        0x61, 0x26, 0x96, 0xef, 0xc9, 0x4a, 0x3a, 0x9b, 0xec, 0xcf, 0x7d, 0x5a, 0x96, 0xdb, 0x3f, 0xc7,
        0x84, 0xfa, 0x1b, 0xf6, 0x1f, 0x79, 0xcf, 0x1b, 0xa9, 0xd7, 0xef, 0x54, 0xd8, 0x84, 0x2d, 0x12,
        0xf5, 0x05, 0xfd, 0x3d, 0x09, 0x8d, 0x97, 0x6d, 0x74, 0x26, 0x04, 0x9c, 0xc7, 0xfa, 0xf6, 0x26,
        0x09, 0xeb, 0xc6, 0x72, 0xfb, 0x8a, 0x6b, 0xe1, 0x6c, 0x16, 0x47, 0xaf, 0x90, 0xf1, 0x2b, 0x9f,
        0xb2, 0x30, 0xcf, 0xd5, 0x0a, 0x4f, 0x5c, 0x8c, 0xd4, 0xc5, 0x24, 0x59, 0x70, 0x99, 0x7f, 0x13,
        0x3d, 0x64, 0x8e, 0x49, 0xed, 0x81, 0x7e, 0xeb, 0xcb, 0x74, 0x6f, 0x10, 0x8d, 0x72, 0xb8, 0xbf,
        0xa7, 0x4e, 0x7f, 0x61, 0x1f, 0x5e, 0x1c, 0xd7, 0x08, 0x9d, 0x31, 0x54, 0x60, 0x78, 0x19, 0xba,
        0x35, 0xba, 0x48, 0x5c, 0x56, 0xdb, 0xbb, 0x66, 0xbc, 0x3c, 0x90, 0xff, 0x89, 0x14, 0xe0, 0xaa,
        0x2b, 0x92, 0x45, 0x41, 0x87, 0x5f, 0xcb, 0x6b, 0x7f, 0x99, 0x9d, 0x4a, 0x20, 0x4a, 0x59, 0x91,
        0xe9, 0xd8, 0xdb, 0x50, 0xca, 0x97, 0x43, 0x9f, 0x99, 0xda,
    };

    // A .cbs made with mymcplus's cipher and zlib level 9: folder BATEST-00002, files BATEST-00002 (cbsMain) and icon.sys (cbsIcon).
    Bytes cbsMain()
    {
        Bytes b;
        for (int i = 0; i < 30; ++i)
            append(b, bytesOf("CBS-KNOWN-ANSWER "));
        for (int i = 0; i < 256; ++i)
            b.push_back(static_cast<uint8_t>(i));
        return b;
    }
    Bytes cbsIcon()
    {
        Bytes b;
        for (int i = 0; i < 20; ++i)
            append(b, bytesOf("icon "));
        return b;
    }
    const uint8_t kCbsKnown[] = {
        0x43, 0x46, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x01, 0x00, 0x00, 0xe2, 0x03, 0x00, 0x00,
        0x5b, 0x01, 0x00, 0x00, 0x42, 0x41, 0x54, 0x45, 0x53, 0x54, 0x2d, 0x30, 0x30, 0x30, 0x30, 0x32,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0x84, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4b, 0x6e, 0x6f, 0x77,
        0x6e, 0x20, 0x61, 0x6e, 0x73, 0x77, 0x65, 0x72, 0x20, 0x74, 0x65, 0x73, 0x74, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb7, 0x9c, 0x7b, 0x01, 0x1b, 0xf4, 0x52, 0x48,
        0xdf, 0x01, 0xf9, 0xc7, 0x7d, 0x24, 0x7e, 0x79, 0x1f, 0x16, 0x88, 0x41, 0x69, 0x41, 0xfe, 0x03,
        0x68, 0xbe, 0xff, 0xe3, 0xdb, 0x6a, 0xf8, 0xda, 0x7c, 0xba, 0x2c, 0x90, 0xc7, 0x2b, 0xb3, 0x3d,
        0xc9, 0xa2, 0xbf, 0xe1, 0xcb, 0x4e, 0x73, 0xc7, 0x01, 0x82, 0x08, 0x1c, 0x2c, 0xdb, 0x20, 0x60,
        0xb4, 0x74, 0x56, 0x58, 0xac, 0x7e, 0xa2, 0xff, 0xaa, 0x3e, 0x56, 0x4b, 0xb9, 0x23, 0x7a, 0x0e,
        0x24, 0xe6, 0x5e, 0xb7, 0x6c, 0xa1, 0x54, 0x57, 0x4e, 0x6e, 0xa8, 0x01, 0xa1, 0x8b, 0x72, 0x96,
        0x92, 0x8c, 0xfc, 0xee, 0x66, 0xf9, 0x48, 0x08, 0xf5, 0x78, 0x34, 0x2b, 0x33, 0x91, 0x4a, 0x13,
        0x40, 0x27, 0x04, 0x62, 0x0c, 0x63, 0x90, 0x13, 0x2c, 0x6b, 0xf6, 0xd4, 0x7c, 0x99, 0x30, 0xd0,
        0xca, 0xe6, 0x9d, 0xae, 0x06, 0xa8, 0xa0, 0x0a, 0x2e, 0xb1, 0x1b, 0x37, 0x4a, 0xb9, 0x71, 0x0e,
        0x97, 0x6b, 0x76, 0x3d, 0x8e, 0x84, 0xab, 0x54, 0xf2, 0x72, 0x19, 0x9d, 0xfe, 0x19, 0x4e, 0x53,
        0xa7, 0x67, 0xee, 0xa1, 0x79, 0x29, 0xaf, 0xc8, 0x46, 0xef, 0x1d, 0x96, 0x99, 0xa1, 0x48, 0xb8,
        0x39, 0xe6, 0xa4, 0xb3, 0x73, 0x4b, 0xf7, 0x8a, 0xbd, 0xbb, 0x56, 0x10, 0x46, 0xef, 0xf6, 0xe6,
        0xff, 0x04, 0x9c, 0x36, 0xb9, 0x6c, 0x75, 0xae, 0xd1, 0xb4, 0x04, 0xc9, 0xda, 0x94, 0x0f, 0xb6,
        0xaf, 0xfe, 0xb6, 0x12, 0x74, 0xb1, 0xe5, 0xf7, 0x73, 0x31, 0x75, 0x7f, 0xbf, 0x84, 0x40, 0x91,
        0x3b, 0xd5, 0x8a, 0x26, 0x97, 0xf1, 0x01, 0x12, 0x3a, 0x13, 0x00, 0x8c, 0x89, 0x15, 0xe4, 0x0c,
        0x86, 0x3e, 0xc5, 0x02, 0xce, 0x3b, 0x2c, 0xe8, 0x89, 0x7d, 0x3c, 0x76, 0x7d, 0x24, 0xb2, 0x7b,
        0x72, 0x0a, 0x0c, 0xce, 0xd7, 0x5f, 0xe4, 0x46, 0xc9, 0xcd, 0xde, 0x86, 0x47, 0xa9, 0x5b, 0xa5,
        0xe8, 0x61, 0x1e, 0x5b, 0xb1, 0x61, 0xcf, 0xc2, 0x25, 0x84, 0xfc, 0xd5, 0x90, 0x8a, 0xcd, 0x6c,
        0x18, 0x45, 0x7b, 0xb9, 0x2c, 0x89, 0x7c, 0xf1, 0xd3, 0x25, 0x56, 0xb1, 0x3f, 0xe0, 0x1c, 0xb3,
        0xce, 0x11, 0x99, 0x50, 0x08, 0x16, 0xcf, 0xb1, 0x71, 0xa5, 0x5b, 0xcf, 0x72, 0x8e, 0x6e, 0xfc,
        0x85, 0xc3, 0xcd, 0xd3, 0x4f, 0x91, 0x56, 0x27, 0xe4, 0x6c, 0x30, 0x35, 0xc8, 0xa9, 0x05, 0x8a,
        0xce, 0x67, 0x3a, 0x8c, 0x6b, 0xea, 0xc2, 0xa3, 0x2d, 0x3c, 0x36, 0xa7, 0x9c, 0xf6, 0x01, 0xed,
        0x6c, 0xb9, 0x02,
    };
    Bytes knownBytes(const uint8_t *p, size_t n) { return Bytes(p, p + n); }

    // ---- hammering a reader with damaged files ---------------------------------------------------------------------------------
    // Every cut and every flipped byte must end in a clean answer: false with one line, or true with whatever could be read. A crash,
    // a hang or a runaway allocation is the failure (a sanitiser build turns a stray read into a crash).
    uint32_t nextRandom(uint32_t &s)
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }

    void hammer(TestCase &t, const Bytes &good, const std::string &label, int cuts, int flips, uint32_t seed, size_t flipFrom, size_t flipTo)
    {
        std::vector<SaveEntry> out;
        for (int i = 0; i < cuts; ++i)
        {
            const size_t len = i < cuts / 2 ? size_t(i) * 2 : size_t(uint64_t(good.size()) * i / cuts);
            Bytes cut(good.begin(), good.begin() + std::min(len, good.size()));
            readOk(t, cut, "x.bin", out, label + " cut " + std::to_string(len));
        }
        uint32_t s = seed;
        flipTo = std::min(flipTo, good.size());
        for (int i = 0; i < flips; ++i)
        {
            Bytes bad = good;
            const size_t at = flipFrom + nextRandom(s) % (flipTo - flipFrom);
            bad[at] = static_cast<uint8_t>(nextRandom(s));
            if (i % 3 == 0)
                bad[at] = 0xFF;
            readOk(t, bad, "x.bin", out, label + " flip at " + std::to_string(at));
        }
    }

    // ---- real files -----------------------------------------------------------------------------------------------------------
    fs::path findRepoRoot()
    {
        std::error_code ec;
        fs::path dir = fs::current_path(ec);
        for (int i = 0; i < 8 && !dir.empty(); ++i)
        {
            if (fs::exists(dir / "baserom" / "saves" / "001_baseline" / "BASLUS-21702", ec) && fs::exists(dir / "docs", ec))
                return dir;
            if (!dir.has_parent_path() || dir.parent_path() == dir)
                break;
            dir = dir.parent_path();
        }
        return fs::path();
    }

    Bytes readFile(const fs::path &p)
    {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream o;
        o << in.rdbuf();
        const std::string s = o.str();
        return Bytes(s.begin(), s.end());
    }

    // The files of an extracted save folder (mymcplus export), as a Folder.
    Folder loadFolder(const fs::path &dir, const std::string &name)
    {
        Folder f;
        f.name = name;
        std::vector<fs::path> names;
        for (const auto &e : fs::directory_iterator(dir))
            if (e.is_regular_file())
                names.push_back(e.path());
        std::sort(names.begin(), names.end());
        // the main file first, as expectSame wants
        std::stable_partition(names.begin(), names.end(), [&](const fs::path &p) { return p.filename().string() == f.name; });
        for (const fs::path &p : names)
            f.files.push_back({p.filename().string(), readFile(p)});
        return f;
    }

    const SaveEntry *findFolder(const std::vector<SaveEntry> &entries, const std::string &name)
    {
        for (const SaveEntry &e : entries)
            if (e.folder == name)
                return &e;
        return nullptr;
    }

    // The others of a real folder come in container order; compare as sets by name.
    void expectSameUnordered(TestCase &t, const SaveEntry &e, const Folder &f, const std::string &label)
    {
        t.IsTrue(e.folder == f.name, label + ": folder name");
        t.IsTrue(!f.files.empty() && e.save == f.files[0].data && e.saveName == f.files[0].name, label + ": main file");
        std::map<std::string, const Bytes *> want;
        for (size_t i = 1; i < f.files.size(); ++i)
            want[f.files[i].name] = &f.files[i].data;
        t.IsTrue(e.others.size() == want.size(), label + ": other files: " + std::to_string(e.others.size()) + " vs " + std::to_string(want.size()));
        for (const SaveFile &o : e.others)
        {
            const auto it = want.find(o.name);
            t.IsTrue(it != want.end() && *it->second == o.data, label + ": file " + o.name);
        }
    }
} // namespace

void register_save_container_tests()
{
    // ------------------------------------------------------------------------------------------------------------ formats
    MiniTest::Case("SaveContainerFormats", [](TestCase &tc)
    {
        tc.Run("a raw save is recognised by its size and named by its file name", [](TestCase &t)
        {
            const Bytes raw = pattern(0xDF1E4, 7);
            std::vector<SaveEntry> out;
            t.IsTrue(readOk(t, raw, "BASLUS-21702", out, "raw"), "reads");
            t.IsTrue(out.size() == 1 && out[0].format == "raw" && out[0].save == raw, "one raw entry holding the bytes");
            t.IsTrue(out.size() == 1 && out[0].folder == "BASLUS-21702" && out[0].saveName == "BASLUS-21702", "named like the file");
            t.IsTrue(readOk(t, raw, "mysave.bin", out, "raw 2"), "reads under another name");
            t.IsTrue(out.size() == 1 && out[0].folder.empty() && out[0].saveName == "mysave.bin", "no folder name to give");
            t.IsTrue(saveContainerKind(raw, "") == "raw", "kind raw");
            // one byte more or less is not a raw save
            Bytes longer = raw;
            longer.push_back(0);
            t.IsFalse(readOk(t, longer, "x", out, "raw+1"), "one byte too long is refused");
            Bytes shorter(raw.begin(), raw.end() - 1);
            t.IsFalse(readOk(t, shorter, "x", out, "raw-1"), "one byte too short is refused");
        });

        tc.Run("psu: files, padding, missing padding at the end", [](TestCase &t)
        {
            const Folder f = sampleFolder("BASLUS-21702", 5000, 3);
            std::vector<SaveEntry> out;
            for (const bool padLast : {true, false})
            {
                const Bytes psu = makePsu(f, padLast);
                t.IsTrue(readOk(t, psu, "save.psu", out, "psu"), "reads");
                t.IsTrue(out.size() == 1 && out[0].format == "psu", "one psu entry");
                if (!out.empty())
                    expectSame(t, out[0], f, padLast ? "psu" : "psu unpadded");
                t.IsTrue(saveContainerKind(psu, "") == "psu", "kind psu");
            }
            // a file of exactly 1024 bytes needs no padding, and an empty file adds none
            Folder g = sampleFolder("BASLUS-21702", 1024, 4);
            g.files.push_back({"empty.bin", {}});
            t.IsTrue(readOk(t, makePsu(g, true), "x.psu", out, "psu 1024"), "reads");
            if (out.size() == 1)
            {
                t.IsTrue(out[0].others.size() == 3 && out[0].others[2].name == "empty.bin" && out[0].others[2].data.empty(), "the empty file is kept");
                t.IsTrue(out[0].save.size() == 1024, "1024-byte main file");
            }
        });

        tc.Run("max: a file written by mymcplus (known answer)", [](TestCase &t)
        {
            std::vector<SaveEntry> out;
            const Bytes max = knownBytes(kMaxKnown, sizeof(kMaxKnown));
            t.IsTrue(max.size() > 0x5C && std::memcmp(max.data(), "Ps2PowerSave", 12) == 0, "the known file is a .max");
            t.IsTrue(readOk(t, max, "x.max", out, "max KAT"), "reads");
            Folder f;
            f.name = "BATEST-00001";
            f.files = {{"BATEST-00001", maxMain()}, {"icon.sys", maxIcon()}};
            t.IsTrue(out.size() == 1 && out[0].format == "max", "one max entry");
            if (!out.empty())
                expectSame(t, out[0], f, "max KAT");
        });

        tc.Run("max: our own encoder, small and large (past the 4096-byte window), either size convention", [](TestCase &t)
        {
            std::vector<SaveEntry> out;
            for (const size_t size : {size_t(0), size_t(1), size_t(100), size_t(5000), size_t(40000)})
            {
                for (const bool clenIsLength : {false, true})
                {
                    const Folder f = sampleFolder("BASLUS-21702", size, uint32_t(size) + 5);
                    const Bytes max = makeMax(f, clenIsLength);
                    const std::string label = "max " + std::to_string(size) + (clenIsLength ? " clen=length" : "");
                    t.IsTrue(readOk(t, max, "x.max", out, label), label + " reads");
                    if (out.size() == 1)
                        expectSame(t, out[0], f, label);
                }
            }
            // noise does not compress; the encoder then writes almost only literals
            Folder noise = sampleFolder("BASLUS-21702", 0, 1);
            noise.files[0].data = pattern(3000, 99);
            t.IsTrue(readOk(t, makeMax(noise, false), "x.max", out, "max noise"), "noise reads");
            if (out.size() == 1)
                expectSame(t, out[0], noise, "max noise");
        });

        tc.Run("xps/sps: files, a longer entry header", [](TestCase &t)
        {
            const Folder f = sampleFolder("BASLUS-21702", 3000, 11);
            std::vector<SaveEntry> out;
            for (const size_t extra : {size_t(0), size_t(6)})
            {
                const Bytes sps = makeSps(f, extra);
                t.IsTrue(readOk(t, sps, "save.xps", out, "sps"), "reads");
                t.IsTrue(out.size() == 1 && out[0].format == "sps", "one sps entry");
                if (!out.empty())
                    expectSame(t, out[0], f, "sps extra " + std::to_string(extra));
                t.IsTrue(saveContainerKind(sps, "") == "sps", "kind sps");
            }
        });

        tc.Run("cbs: a file written with mymcplus's cipher and zlib (known answer), either length convention", [](TestCase &t)
        {
            Folder f;
            f.name = "BATEST-00002";
            f.files = {{"BATEST-00002", cbsMain()}, {"icon.sys", cbsIcon()}};
            std::vector<SaveEntry> out;
            Bytes cbs = knownBytes(kCbsKnown, sizeof(kCbsKnown));
            t.IsTrue(cbs.size() > 0x128 && std::memcmp(cbs.data(), "CFU\0", 4) == 0, "the known file is a .cbs");
            t.IsTrue(readOk(t, cbs, "x.cbs", out, "cbs KAT"), "reads");
            t.IsTrue(out.size() == 1 && out[0].format == "cbs", "one cbs entry");
            if (!out.empty())
                expectSame(t, out[0], f, "cbs");

            // header length hlen at 8, flen at 16: the body length, or the whole file's length
            const uint32_t hlen = uint32_t(cbs[8]) | (uint32_t(cbs[9]) << 8) | (uint32_t(cbs[10]) << 16);
            const uint32_t flen = uint32_t(cbs[16]) | (uint32_t(cbs[17]) << 8) | (uint32_t(cbs[18]) << 16);
            t.IsTrue(hlen == cbs.size() - flen, "the known file has flen = body length");
            putAt32(cbs, 16, flen + hlen);
            t.IsTrue(readOk(t, cbs, "x.cbs", out, "cbs flen=total"), "reads with flen = the whole file");
            if (out.size() == 1)
                expectSame(t, out[0], f, "cbs flen=total");
            putAt32(cbs, 16, flen + 3); // neither convention
            t.IsFalse(readOk(t, cbs, "x.cbs", out, "cbs flen odd"), "a length that fits neither is refused");
            // a bad zlib stream: flip the first body byte (the cipher turns that into a different first byte of the stream)
            Bytes bad = knownBytes(kCbsKnown, sizeof(kCbsKnown));
            bad[hlen] ^= 0x55;
            t.IsFalse(readOk(t, bad, "x.cbs", out, "cbs bad zlib"), "a broken zlib header is refused");
        });

        tc.Run("psv: a PS2 save; a PS1 one and a wrong version are refused", [](TestCase &t)
        {
            const Folder f = sampleFolder("BASLUS-21702", 4000, 21);
            std::vector<SaveEntry> out;
            const Bytes psv = makePsv(f, 2);
            t.IsTrue(readOk(t, psv, "save.psv", out, "psv"), "reads");
            t.IsTrue(out.size() == 1 && out[0].format == "psv", "one psv entry");
            if (!out.empty())
                expectSame(t, out[0], f, "psv");
            t.IsTrue(saveContainerKind(psv, "") == "psv", "kind psv");

            std::string error;
            t.IsFalse(readSaveContainerBytes(makePsv(f, 1), "x.psv", out, error), "a PS1 save is refused");
            t.IsTrue(contains(error, "PlayStation 1") && oneLine(error), "with a line that says why: " + error);
            Bytes v1 = psv;
            v1[4] = 1;
            t.IsFalse(readOk(t, v1, "x.psv", out, "psv v1"), "an unknown version is refused");
            Bytes inside = psv;
            inside[0x68 + 20] = 0x10; // the folder's mode becomes a file's
            inside[0x68 + 21] = 0x80;
            t.IsFalse(readOk(t, inside, "x.psv", out, "psv dir"), "a folder entry that is not a folder is refused");
        });

        tc.Run("ps2 card: with and without ECC, plain and scattered clusters, several folders", [](TestCase &t)
        {
            // a card as a player has it: other games' saves too, in an order that is not alphabetical
            const std::vector<Folder> folders = {sampleFolder("BASCUS-90001", 2200, 31), sampleFolder("BASLUS-21702", 913892, 32),
                                                 sampleFolder("BESLES-55041", 6000, 33)};
            for (const bool ecc : {true, false})
            {
                for (const bool scatter : {false, true})
                {
                    CardOptions o;
                    o.ecc = ecc;
                    o.scatter = scatter;
                    const Bytes card = makeCard(folders, o);
                    t.IsTrue(card.size() == (ecc ? 8650752u : 8388608u), "card size");
                    t.IsTrue(saveContainerKind(card, "") == "ps2", "kind ps2");
                    std::vector<SaveEntry> out;
                    const std::string label = std::string("card ") + (ecc ? "ecc" : "plain") + (scatter ? " scattered" : "");
                    t.IsTrue(readOk(t, card, "x.ps2", out, label), label + " reads");
                    t.IsTrue(out.size() == 3, label + ": three folders, got " + std::to_string(out.size()));
                    for (size_t i = 0; i < out.size() && i < folders.size(); ++i)
                    {
                        t.IsTrue(out[i].format == "ps2", label + ": format");
                        expectSame(t, out[i], folders[i], label + " " + folders[i].name);
                    }
                }
            }
        });

        tc.Run("ps2 card: an empty card, a damaged folder, an empty file", [](TestCase &t)
        {
            std::vector<SaveEntry> out;
            CardOptions o;
            o.clusters = 2048;
            t.IsTrue(readOk(t, makeCard({}, o), "x.ps2", out, "empty card"), "an empty card reads");
            t.IsTrue(out.empty(), "with no folders");

            // a card whose second folder has a cut cluster chain: the other folders are still returned
            const std::vector<Folder> folders = {sampleFolder("BASLUS-21702", 9000, 41), sampleFolder("BADATA-SYSTEM", 20000, 42), sampleFolder("BESLES-55041", 3000, 43)};
            CardOptions d;
            d.breakFolder = "BADATA-SYSTEM";
            t.IsTrue(readOk(t, makeCard(folders, d), "x.ps2", out, "broken"), "reads");
            t.IsTrue(out.size() == 3, "all three folders are listed");
            if (out.size() == 3)
            {
                expectSame(t, out[0], folders[0], "intact first");
                expectSame(t, out[2], folders[2], "intact last");
                t.IsTrue(out[1].folder == "BADATA-SYSTEM" && !out[1].problem.empty() && oneLine(out[1].problem), "the damaged one says so: '" + out[1].problem + "'");
                t.IsTrue(out[1].save.empty() && out[1].others.empty(), "and holds nothing");
            }

            // a zero-length file has no cluster chain at all
            std::vector<Folder> withEmpty = {sampleFolder("BASLUS-21702", 3000, 44)};
            withEmpty[0].files.push_back({"zero.dat", {}});
            t.IsTrue(readOk(t, makeCard(withEmpty, o), "x.ps2", out, "zero file"), "reads");
            if (out.size() == 1)
                expectSame(t, out[0], withEmpty[0], "zero file");
        });

        tc.Run("ps2 card: the geometry comes from the superblock (a 16 MB card)", [](TestCase &t)
        {
            // 16384 clusters, 64 FAT clusters, the allocatable area starting at cluster 73 instead of 41.
            const std::vector<Folder> folders = {sampleFolder("BASLUS-21702", 300000, 51)};
            CardOptions o;
            o.clusters = 16384;
            std::vector<SaveEntry> out;
            t.IsTrue(readOk(t, makeCard(folders, o), "x.ps2", out, "16 MB"), "reads");
            if (out.size() == 1)
                expectSame(t, out[0], folders[0], "16 MB");
        });

        tc.Run("ps2 card: past 64 MB the FAT needs a second indirect cluster", [](TestCase &t)
        {
            // 70000 clusters: 274 FAT clusters, so FAT cluster numbers 256 and up live behind indirect_fat_cluster_list[1]. A file whose
            // clusters are scattered over the whole card has to use both.
            const std::vector<Folder> folders = {sampleFolder("BASLUS-21702", 120000, 52), sampleFolder("BESLES-55041", 50000, 53)};
            CardOptions o;
            o.clusters = 70000;
            o.ecc = false;
            o.scatter = true;
            o.stride = 541;
            uint32_t highest = 0;
            o.highest = &highest;
            const Bytes card = makeCard(folders, o);
            t.IsTrue(highest >= 65536, "the writer used clusters past 65536 (" + std::to_string(highest) + ")");
            std::vector<SaveEntry> out;
            t.IsTrue(readOk(t, card, "x.ps2", out, "70000 clusters"), "reads");
            t.IsTrue(out.size() == 2, "two folders");
            if (out.size() == 2)
            {
                expectSame(t, out[0], folders[0], "big card 0");
                expectSame(t, out[1], folders[1], "big card 1");
            }
        });

        tc.Run("the main file: named like the folder, else the largest that is not an icon", [](TestCase &t)
        {
            std::vector<SaveEntry> out;
            Folder f;
            f.name = "BASLUS-21702";
            f.files = {{"icon.sys", pattern(964, 1)}, {"view.ico", pattern(5000, 2)}, {"DATA", pattern(300, 3)}, {"BIG", pattern(900, 4)}, {"baslus-21702", pattern(10, 5)}};
            t.IsTrue(readOk(t, makePsu(f, true), "x.psu", out, "main"), "reads");
            t.IsTrue(out.size() == 1 && out[0].saveName == "baslus-21702" && out[0].save == f.files[4].data, "the file named like the folder wins, in any case");
            f.files.pop_back();
            t.IsTrue(readOk(t, makePsu(f, true), "x.psu", out, "main 2"), "reads");
            t.IsTrue(out.size() == 1 && out[0].saveName == "BIG" && out[0].save == f.files[3].data, "else the largest that is not icon.sys or an .ico file");
            t.IsTrue(out.size() == 1 && out[0].others.size() == 3, "the rest are the others");
            Folder only;
            only.name = "BADATA-ICONS";
            only.files = {{"icon.sys", pattern(100, 1)}, {"x.ICO", pattern(100, 2)}};
            t.IsTrue(readOk(t, makePsu(only, true), "x.psu", out, "icons only"), "reads");
            t.IsTrue(out.size() == 1 && out[0].save.empty() && out[0].saveName.empty() && out[0].others.size() == 2, "a folder of only icons has no main file");
        });
    });

    // ------------------------------------------------------------------------------------------------------------ detection and refusals
    MiniTest::Case("SaveContainerDetection", [](TestCase &tc)
    {
        tc.Run("the format comes from the bytes, not from the extension", [](TestCase &t)
        {
            const Folder f = sampleFolder("BASLUS-21702", 2500, 61);
            std::vector<SaveEntry> out;
            struct Item
            {
                Bytes bytes;
                const char *format;
            };
            const std::vector<Item> items = {{makePsu(f, true), "psu"}, {makeMax(f, false), "max"}, {makeSps(f, 0), "sps"}, {makePsv(f, 2), "psv"},
                                             {knownBytes(kCbsKnown, sizeof(kCbsKnown)), "cbs"}};
            for (const char *name : {"save.bin", "save.txt", "save.max", "save.psu", "save.cbs", "", "SAVE.PS2"})
            {
                for (const Item &it : items)
                {
                    t.IsTrue(readOk(t, it.bytes, name, out, std::string(it.format) + " as " + name), std::string(it.format) + " as '" + name + "' reads");
                    t.IsTrue(out.size() == 1 && out[0].format == it.format, std::string(it.format) + " as '" + name + "' keeps its format");
                    t.IsTrue(saveContainerKind(it.bytes, name) == it.format, std::string(it.format) + " kind");
                }
            }
            CardOptions o;
            o.clusters = 2048;
            const Bytes card = makeCard({f}, o);
            for (const char *name : {"card.max", "card.psu", "card", "card.vmc"})
            {
                t.IsTrue(readOk(t, card, name, out, "card"), "a card as '" + std::string(name) + "' reads");
                t.IsTrue(out.size() == 1 && out[0].format == "ps2", "and stays a card");
            }
        });

        tc.Run("a PS3 .vme is refused with the convert-first line; so is a card behind a header", [](TestCase &t)
        {
            CardOptions o;
            o.clusters = 2048;
            Bytes vme(0x80, 0x11);
            append(vme, makeCard({sampleFolder("BASLUS-21702", 2000, 71)}, o));
            std::vector<SaveEntry> out;
            std::string error;
            t.IsFalse(readSaveContainerBytes(vme, "SCEVMC0.VME", out, error), "a card behind a 0x80-byte header is refused");
            t.IsTrue(contains(error, "Convert it to .ps2 first") && contains(error, "Conversion Kit") && oneLine(error), "with the one line: " + error);
            t.IsTrue(out.empty(), "no entries");
            t.IsTrue(saveContainerKind(vme, "") == "vme", "kind vme");
            // the same line from the extension alone (a header this code does not know)
            error.clear();
            t.IsFalse(readSaveContainerBytes(pattern(5000, 3), "SCEVMC1.vme", out, error), "an unknown .vme is refused");
            t.IsTrue(contains(error, "Convert it to .ps2 first") && oneLine(error), "with the same line: " + error);
        });

        tc.Run("things that are not saves give one line", [](TestCase &t)
        {
            std::vector<SaveEntry> out;
            std::string error;
            t.IsFalse(readSaveContainerBytes({}, "empty.bin", out, error), "an empty file is refused");
            t.IsTrue(!error.empty() && oneLine(error), "with a line: " + error);
            t.IsFalse(readSaveContainerBytes(pattern(100, 1), "x", out, error), "100 noise bytes are refused");
            t.IsFalse(readSaveContainerBytes(pattern(100000, 1), "x", out, error), "100000 noise bytes are refused");
            t.IsFalse(readSaveContainerBytes(bytesOf("PK\x03\x04 a zip file would be a mod, not a save"), "x.zip", out, error), "a zip is refused");
            t.IsTrue(contains(error, ".max") && oneLine(error), "and the line names the formats that do work: " + error);
            // a PSU needs all three directory entries
            Bytes psu = makePsu(sampleFolder("BASLUS-21702", 100, 1), true);
            psu[512 + 64] = 'X'; // "." becomes "X"
            t.IsFalse(readOk(t, psu, "x.psu", out, "psu no dot"), "a PSU without its '.' entry is not recognised");
            psu = makePsu(sampleFolder("BASLUS-21702", 100, 1), true);
            psu.resize(1000);
            t.IsFalse(readOk(t, psu, "x.psu", out, "psu short"), "a PSU cut inside its first entries is not recognised");
        });

        tc.Run("readSaveContainer: files and folders on disk", [](TestCase &t)
        {
            std::error_code ec;
            const fs::path dir = fs::temp_directory_path() / "ps2x_save_container_test";
            fs::remove_all(dir, ec);
            fs::create_directories(dir / "BASLUS-21702", ec);
            const Folder f = sampleFolder("BASLUS-21702", 4000, 81);
            auto write = [](const fs::path &p, const Bytes &b) {
                std::ofstream o(p, std::ios::binary);
                o.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
            };
            for (const F &file : f.files)
                write(dir / "BASLUS-21702" / file.name, file.data);
            write(dir / "a.max", makeMax(f, false));
            write(dir / "empty.dat", {});
            write(dir / "BASLUS-21702.raw", pattern(0xDF1E4, 2));

            std::vector<SaveEntry> out;
            std::string error;
            t.IsTrue(readSaveContainer(dir / "a.max", out, error), "a .max file reads: " + error);
            t.IsTrue(out.size() == 1 && out[0].format == "max", "as max");
            if (!out.empty())
                expectSame(t, out[0], f, "disk max");

            t.IsTrue(readSaveContainer(dir / "BASLUS-21702", out, error), "a folder reads: " + error);
            t.IsTrue(out.size() == 1 && out[0].format == "folder", "as a folder");
            if (!out.empty())
            {
                t.IsTrue(out[0].folder == "BASLUS-21702" && out[0].save == f.files[0].data && out[0].saveName == "BASLUS-21702", "the folder's main file");
                t.IsTrue(out[0].others.size() == 2 && out[0].others[0].name == "icon.sys" && out[0].others[1].name == "view.ico", "and the others, sorted by name");
            }

            // a raw file takes its folder name from its own name, else from the folder it is in
            fs::create_directories(dir / "BASLUS-21702-b", ec);
            fs::copy_file(dir / "BASLUS-21702.raw", dir / "BASLUS-21702-b" / "mysave", ec);
            t.IsTrue(readSaveContainer(dir / "BASLUS-21702-b" / "mysave", out, error), "a raw file reads: " + error);
            t.IsTrue(out.size() == 1 && out[0].format == "raw" && out[0].saveName == "mysave", "as raw");
            t.IsTrue(out.size() == 1 && out[0].folder == "BASLUS-21702-b", "named by the folder it sits in");
            fs::create_directories(dir / "other", ec);
            fs::copy_file(dir / "BASLUS-21702.raw", dir / "other" / "mysave", ec);
            t.IsTrue(readSaveContainer(dir / "other" / "mysave", out, error) && out.size() == 1 && out[0].folder.empty(), "no folder name when neither looks like one");

            // a folder of save folders (a PCSX2 folder memory card): one entry per sub-folder, in name order; a stray file beside them is ignored
            const Folder g = sampleFolder("BESLES-55041", 1500, 82);
            fs::create_directories(dir / "pile" / "BESLES-55041", ec);
            fs::create_directories(dir / "pile" / "BASLUS-21702", ec);
            fs::create_directories(dir / "pile" / "EMPTY", ec);
            for (const F &file : f.files)
                write(dir / "pile" / "BASLUS-21702" / file.name, file.data);
            for (const F &file : g.files)
                write(dir / "pile" / "BESLES-55041" / file.name, file.data);
            write(dir / "pile" / "_pcsx2_superblock", pattern(100, 5));
            t.IsTrue(readSaveContainer(dir / "pile", out, error), "a folder of folders reads: " + error);
            t.IsTrue(out.size() == 3, "one entry per sub-folder, got " + std::to_string(out.size()));
            if (out.size() == 3)
            {
                expectSame(t, out[0], f, "pile 0");
                expectSame(t, out[1], g, "pile 1");
                t.IsTrue(out[0].format == "folder" && out[2].folder == "EMPTY" && out[2].save.empty() && out[2].others.empty(), "and an empty one is just empty");
            }

            t.IsFalse(readSaveContainer(dir / "missing.max", out, error), "a missing file is refused");
            t.IsTrue(!error.empty() && oneLine(error) && !contains(error, "ps2x_save_container_test"), "with a line that has no path in it: " + error);
            t.IsFalse(readSaveContainer(dir / "empty.dat", out, error), "an empty file is refused");
            t.IsTrue(!error.empty() && oneLine(error), "with a line: " + error);
            fs::remove_all(dir, ec);
        });
    });

    // ------------------------------------------------------------------------------------------------------------ damaged input
    MiniTest::Case("SaveContainerDamaged", [](TestCase &tc)
    {
        tc.Run("cut and corrupted files of every format end in a clean answer", [](TestCase &t)
        {
            const Folder f = sampleFolder("BASLUS-21702", 6000, 91);
            hammer(t, makePsu(f, true), "psu", 160, 200, 1, 0, 2048);
            hammer(t, makeSps(f, 3), "sps", 160, 200, 2, 0, 400);
            hammer(t, makePsv(f, 2), "psv", 160, 200, 3, 0, 400);
            hammer(t, knownBytes(kCbsKnown, sizeof(kCbsKnown)), "cbs", 160, 300, 4, 0, sizeof(kCbsKnown));
            hammer(t, knownBytes(kMaxKnown, sizeof(kMaxKnown)), "max KAT", 160, 300, 5, 0, sizeof(kMaxKnown));
            const Bytes max = makeMax(f, false);
            hammer(t, max, "max", 120, 300, 6, 0, max.size());
        });

        tc.Run("cut and corrupted card images end in a clean answer", [](TestCase &t)
        {
            const std::vector<Folder> folders = {sampleFolder("BASLUS-21702", 20000, 95), sampleFolder("BADATA-SYSTEM", 3000, 96)};
            for (const bool ecc : {true, false})
            {
                CardOptions o;
                o.clusters = 1024;
                o.ecc = ecc;
                o.scatter = true;
                const Bytes card = makeCard(folders, o);
                // the superblock, the FAT and the directories sit in the first 60 clusters, and the files after them
                hammer(t, card, ecc ? "card ecc" : "card", 24, 400, 7, 0, ecc ? 60 * 1056 : 60 * 1024);
                hammer(t, card, ecc ? "card ecc data" : "card data", 0, 120, 8, 0, card.size());
            }
            // a card that claims a large size but is small, and a loop in a cluster chain
            CardOptions o;
            o.clusters = 1024;
            o.ecc = false;
            Bytes card = makeCard(folders, o);
            putAt32(card, 0x30, 0x00FFFFFF);
            std::vector<SaveEntry> out;
            t.IsFalse(readOk(t, card, "x.ps2", out, "huge card"), "a card larger than its file is refused");
            // cluster numbers that are all ones, in the two places the FAT is found from (found by the mutation fuzzer: an unset cache
            // matched them and an empty buffer was read)
            card = makeCard(folders, o);
            putAt32(card, 0x50, 0xFFFFFFFFu); // indirect_fat_cluster_list[0]
            t.IsFalse(readOk(t, card, "x.ps2", out, "ifc ones"), "an indirect FAT cluster number of all ones is refused");
            card = makeCard(folders, o);
            putAt32(card, size_t(8) * 1024, 0xFFFFFFFFu); // the first FAT cluster's number, in the indirect cluster
            t.IsFalse(readOk(t, card, "x.ps2", out, "fat ones"), "a FAT cluster number of all ones is refused");
            card = makeCard(folders, o);
            // allocatable cluster 0 is the root directory; make its FAT entry point to itself (FAT cluster 0 is cluster 9)
            putAt32(card, size_t(9) * 1024, 0x80000000u);
            readOk(t, card, "x.ps2", out, "loop"); // whatever the answer, it must come, and be well formed
        });
    });

    // ------------------------------------------------------------------------------------------------------------ real files
    MiniTest::Case("SaveContainerRealFiles", [](TestCase &tc)
    {
        tc.Run("the community .max equals what mymcplus extracted", [](TestCase &t)
        {
            const fs::path root = findRepoRoot();
            const fs::path dir = root / "baserom" / "saves" / "community" / "gamefaqs-16161-us-rename";
            std::error_code ec;
            if (root.empty() || !fs::exists(dir / "fire-pro-wrestling-returns.16161.max", ec) || !fs::exists(dir / "BASLUS-21702" / "BASLUS-21702", ec))
            {
                std::printf("(skipped: needs baserom/ with the community save) ");
                return;
            }
            std::vector<SaveEntry> out;
            std::string error;
            t.IsTrue(readSaveContainer(dir / "fire-pro-wrestling-returns.16161.max", out, error), "reads: " + error);
            t.IsTrue(out.size() == 1 && out[0].format == "max", "one max entry");
            if (out.size() == 1)
            {
                t.IsTrue(out[0].save.size() == 913892, "the save is 913,892 bytes");
                expectSameUnordered(t, out[0], loadFolder(dir / "BASLUS-21702", "BASLUS-21702"), "community max");
            }
            // the same save as an extracted folder and as a raw file
            t.IsTrue(readSaveContainer(dir / "BASLUS-21702", out, error) && out.size() == 1, "the extracted folder reads: " + error);
            if (out.size() == 1)
                expectSameUnordered(t, out[0], loadFolder(dir / "BASLUS-21702", "BASLUS-21702"), "community folder");
            t.IsTrue(readSaveContainer(dir / "BASLUS-21702" / "BASLUS-21702", out, error) && out.size() == 1, "the raw file reads: " + error);
            t.IsTrue(out.size() == 1 && out[0].format == "raw" && out[0].folder == "BASLUS-21702" && out[0].save.size() == 913892, "as raw, named BASLUS-21702");
        });

        tc.Run("Seth's .ps2 card images equal what mymcplus extracted", [](TestCase &t)
        {
            const fs::path root = findRepoRoot();
            const fs::path saves = root / "baserom" / "saves";
            std::error_code ec;
            std::vector<std::string> names;
            if (!root.empty())
                for (const auto &e : fs::directory_iterator(saves, ec))
                    if (e.is_regular_file() && e.path().extension() == ".ps2" && fs::is_directory(saves / e.path().stem(), ec))
                        names.push_back(e.path().stem().string());
            if (names.empty())
            {
                std::printf("(skipped: needs baserom/saves/*.ps2 with their extracted folders) ");
                return;
            }
            std::sort(names.begin(), names.end());
            for (const std::string &name : names)
            {
                std::vector<SaveEntry> out;
                std::string error;
                t.IsTrue(readSaveContainer(saves / (name + ".ps2"), out, error), name + " reads: " + error);
                const SaveEntry *e = findFolder(out, "BASLUS-21702");
                t.IsTrue(e != nullptr, name + " has BASLUS-21702 (" + std::to_string(out.size()) + " folders)");
                if (e != nullptr)
                {
                    t.IsTrue(e->format == "ps2" && e->problem.empty(), name + ": format and problem");
                    t.IsTrue(e->save.size() == 913892, name + ": 913,892 bytes");
                    expectSameUnordered(t, *e, loadFolder(saves / name, "BASLUS-21702"), name);
                }
            }
            std::printf("(%zu cards) ", names.size());
        });

        tc.Run("an empty card made by another tool (the community kit's mymc) reads as a card with no saves", [](TestCase &t)
        {
            const fs::path root = findRepoRoot();
            std::error_code ec;
            fs::path card;
            if (!root.empty())
                for (const auto &e : fs::recursive_directory_iterator(root / "baserom" / "community", fs::directory_options::skip_permission_denied, ec))
                    if (e.is_regular_file() && e.path().filename() == "Mcd001.ps2")
                        card = e.path();
            if (card.empty())
            {
                std::printf("(skipped: the community kit is not in baserom/) ");
                return;
            }
            std::vector<SaveEntry> out;
            std::string error;
            t.IsTrue(readSaveContainer(card, out, error), "reads: " + error);
            t.IsTrue(out.empty(), "with no saves (mymcplus lists only . and ..)");
        });

        tc.Run("files made by mymcplus and by the fixture script match their extracted contents", [](TestCase &t)
        {
            // baserom/out/tmp/s4a_fixtures/<container file> next to expect/<container file>/<folder>/<file>, made by s4a_make_fixtures.py
            const fs::path root = findRepoRoot();
            const fs::path fx = root / "baserom" / "out" / "tmp" / "s4a_fixtures";
            std::error_code ec;
            if (root.empty() || !fs::exists(fx / "expect", ec))
            {
                std::printf("(skipped: no fixtures) ");
                return;
            }
            int files = 0;
            for (const auto &c : fs::directory_iterator(fx / "expect", ec))
            {
                if (!c.is_directory())
                    continue;
                const std::string container = c.path().filename().string();
                std::vector<SaveEntry> out;
                std::string error;
                t.IsTrue(readSaveContainer(fx / container, out, error), container + " reads: " + error);
                std::set<std::string> want;
                for (const auto &d : fs::directory_iterator(c.path(), ec))
                    if (d.is_directory())
                        want.insert(d.path().filename().string());
                std::set<std::string> got;
                for (const SaveEntry &e : out)
                    got.insert(e.folder);
                t.IsTrue(want == got, container + ": the folders found are the folders expected");
                for (const std::string &folder : want)
                {
                    const SaveEntry *e = findFolder(out, folder);
                    if (e == nullptr)
                        continue;
                    Folder f = loadFolder(c.path() / folder, folder);
                    // the expected main file is the one named like the folder, or else the largest non-icon file
                    if (f.files.empty() || f.files[0].name != folder)
                    {
                        const std::string wantName = e->saveName;
                        auto it = std::find_if(f.files.begin(), f.files.end(), [&](const F &x) { return x.name == wantName; });
                        if (it != f.files.end())
                            std::iter_swap(f.files.begin(), it);
                    }
                    expectSameUnordered(t, *e, f, container + "/" + folder);
                    ++files;
                }
            }
            std::printf("(%d folders) ", files);
        });
    });
}
