#include "MiniTest.h"
#include "runtime/ps2_memory.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"
#include "Stubs/Helpers/Support.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// Tests for the development aids in src/lib/Kernel/Stubs/Helpers/RomOverride.{h,cpp} (task a3): serving a file of FPR.ROM from a host
// folder (PS2X_ROM_OVERRIDE_DIR) and the match probe's field specs and CSV sampler.
//
// The archives here are made up: a directory scrambled with a key of the tests' own, written from the layout in docs/formats/fpr-rom.md
// (not by calling the code under test), plus small data sectors. Nothing is game data. The last suite reads the player's own FPR.ROM when
// it is under baserom/ and compares the C++ directory decoder with tools/inventory.py's table of contents (baserom/out/fpr_rom_toc.csv);
// it says "skipped" when they are not there.

namespace
{
    namespace fs = std::filesystem;
    namespace ro = ps2x_rom_override;
    using Bytes = std::vector<uint8_t>;

    // ---- a made-up archive ------------------------------------------------------------------------------------------------

    // The doc's key layout: for byte p counted from 0x80, block = p/8, half = (p/4)%2, r = p%4, key = cycle[half][(7*block+r)%11].
    const uint8_t kCycle[2][11] = {{0x11, 0x23, 0x35, 0x47, 0x59, 0x6B, 0x7D, 0x8F, 0xA1, 0xB3, 0xC5},
                                   {0xD7, 0xE9, 0xFB, 0x0D, 0x1F, 0x31, 0x43, 0x55, 0x67, 0x79, 0x8B}};

    uint8_t testKey(uint32_t pos)
    {
        const uint32_t p = pos - 0x80;
        const uint32_t block = p / 8, half = (p / 4) % 2, r = p % 4;
        return kCycle[half][(7 * block + r) % 11];
    }

    void put32(Bytes &b, size_t off, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            b[off + i] = static_cast<uint8_t>(v >> (8 * i));
    }

    uint32_t get32(const Bytes &b, size_t off)
    {
        return b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (uint32_t(b[off + 3]) << 24);
    }

    struct Item
    {
        std::string name;
        bool dir;
        uint32_t a, b;
    };

    // One directory block: count word, the "." and ".." entries, `items`, then the name pool. Returns its bytes; `length` is its size.
    Bytes makeBlock(uint32_t countWord, const std::vector<Item> &items, uint32_t selfA, uint32_t selfB, uint32_t parentA, uint32_t parentB)
    {
        const uint32_t n = 2 + static_cast<uint32_t>(items.size());
        const uint32_t pool = 4 + 12 * n;
        Bytes b(pool, 0);
        put32(b, 0, countWord);
        std::string names;
        names.append(".\0..\0", 5);
        auto entry = [&](uint32_t i, uint32_t nameOff, bool dir, uint32_t a, uint32_t bb)
        {
            put32(b, 4 + 12 * i, (dir ? 0x80000000u : 0u) | nameOff);
            put32(b, 4 + 12 * i + 4, a);
            put32(b, 4 + 12 * i + 8, bb);
        };
        entry(0, pool, true, selfA, selfB);
        entry(1, pool + 2, true, parentA, parentB);
        for (size_t i = 0; i < items.size(); ++i)
        {
            entry(2 + static_cast<uint32_t>(i), pool + static_cast<uint32_t>(names.size()), items[i].dir, items[i].a, items[i].b);
            names += items[i].name;
            names.push_back('\0');
        }
        b.insert(b.end(), names.begin(), names.end());
        return b;
    }

    struct Archive
    {
        Bytes plain;     // the header with the directory unscrambled
        Bytes scrambled; // as on disc
        uint32_t wazaEntry = 0;
        uint32_t topEntry = 0;
        uint32_t otherEntry = 0;
    };

    // Layout: root at 0x80 with a folder "yoneda" (block at 0x200) and a file "top.bin"; yoneda holds "fprwaza.bin" and "other.BIN".
    // Files: top.bin sector 22 (4096 bytes), yoneda/fprwaza.bin sector 24 (3000), yoneda/other.bin sector 26 (100).
    Archive makeArchive()
    {
        Archive ar;
        ar.plain.assign(22 * 2048, 0);
        std::memcpy(ar.plain.data(), "ROM ", 4);
        put32(ar.plain, 4, 0x00010000);
        put32(ar.plain, 8, 22);
        put32(ar.plain, 12, 0x46FA0BF2);

        const std::vector<Item> sub = {{"fprwaza.bin", false, 24, 3000}, {"other.BIN", false, 26, 100}};
        Bytes child = makeBlock(2 + static_cast<uint32_t>(sub.size()), sub, 0x200 / 128, 0, 0x80 / 128, 0);
        const uint32_t childLen = static_cast<uint32_t>(child.size());
        // fix the child's "." length now that it is known
        put32(child, 4 + 8, childLen);
        const std::vector<Item> top = {{"yoneda", true, 0x200 / 128, childLen}, {"top.bin", false, 22, 4096}};
        Bytes root = makeBlock(736, top, 0x80 / 128, 0, 0x80 / 128, 0); // the root's count word is wrong in the real archive
        const uint32_t rootLen = static_cast<uint32_t>(root.size());
        put32(root, 4 + 8, rootLen);
        put32(root, 4 + 12 + 8, rootLen);
        put32(child, 4 + 12 + 8, rootLen); // the child's ".." repeats the parent's (a, length)
        std::memcpy(ar.plain.data() + 0x80, root.data(), root.size());
        std::memcpy(ar.plain.data() + 0x200, child.data(), child.size());
        // entry offsets: root entries start at 0x80 + 4; entry i at +12*i
        ar.topEntry = 0x80 + 4 + 12 * 3;
        ar.wazaEntry = 0x200 + 4 + 12 * 2;
        ar.otherEntry = 0x200 + 4 + 12 * 3;
        ar.scrambled = ar.plain;
        for (uint32_t pos = 0x80; pos < ar.scrambled.size(); ++pos)
            ar.scrambled[pos] = static_cast<uint8_t>(ar.plain[pos] ^ testKey(pos));
        return ar;
    }

    int g_dirCounter = 0;

    fs::path makeTempDir(const char *tag)
    {
        std::error_code ec;
        fs::path dir = fs::temp_directory_path(ec) / ("ps2x_romov_" + std::string(tag) + "_" + std::to_string(++g_dirCounter) + "_" +
                                                      std::to_string(reinterpret_cast<uintptr_t>(&g_dirCounter) & 0xFFFF));
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        return dir;
    }

    void writeFile(const fs::path &path, const Bytes &data)
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    }

    Bytes readFile(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    Bytes pattern(size_t n, uint8_t seed)
    {
        Bytes b(n);
        for (size_t i = 0; i < n; ++i)
            b[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 8));
        return b;
    }

    const ro::TocFile *find(const ro::Toc &toc, const std::string &path)
    {
        for (const ro::TocFile &f : toc.files)
            if (f.path == path)
                return &f;
        return nullptr;
    }

    // ---- the real archive, when the player has it ------------------------------------------------------------------------

    fs::path findRepoRoot()
    {
        std::error_code ec;
        fs::path dir = fs::current_path(ec);
        for (int i = 0; i < 8 && !dir.empty(); ++i)
        {
            if (fs::exists(dir / "tools" / "fprwaza.py", ec) && fs::exists(dir / "baserom" / "iso" / "FPR.ROM", ec))
                return dir;
            if (!dir.has_parent_path() || dir.parent_path() == dir)
                break;
            dir = dir.parent_path();
        }
        return fs::path();
    }

    // ---- the probe's made-up guest memory ---------------------------------------------------------------------------------

    struct Rows
    {
        std::string text;
        static void sink(void *user, const std::string &s) { static_cast<Rows *>(user)->text += s; }
        std::vector<std::string> lines() const
        {
            std::vector<std::string> out;
            std::istringstream in(text);
            std::string line;
            while (std::getline(in, line))
                out.push_back(line);
            return out;
        }
    };

    void guestPut32(Bytes &ram, uint32_t addr, uint32_t v)
    {
        put32(ram, addr, v);
    }
}

void register_rom_override_tests()
{
    MiniTest::Case("RomOverride", [](TestCase &tc)
    {
        tc.Run("the directory of a made-up archive is decoded, key recovered, root count word ignored", [](TestCase &t)
        {
            const Archive ar = makeArchive();
            ro::Toc toc;
            std::string error;
            t.IsTrue(ro::decodeToc(ar.scrambled.data(), ar.scrambled.size(), toc, error), "decodes: " + error);
            t.Equals(toc.files.size(), static_cast<size_t>(3), "three files");
            t.Equals(toc.headerSectors, 22u, "22 header sectors");
            const ro::TocFile *top = find(toc, "top.bin");
            const ro::TocFile *waza = find(toc, "yoneda/fprwaza.bin");
            const ro::TocFile *other = find(toc, "yoneda/other.bin");
            t.IsNotNull(top, "top.bin found");
            t.IsNotNull(waza, "yoneda/fprwaza.bin found (folders joined with '/')");
            t.IsNotNull(other, "yoneda/other.bin found (names lower-cased)");
            if (top && waza && other)
            {
                t.Equals(top->sector, 22u, "top.bin sector");
                t.Equals(top->size, 4096u, "top.bin size");
                t.Equals(waza->sector, 24u, "waza sector");
                t.Equals(waza->size, 3000u, "waza size");
                t.Equals(other->sector, 26u, "other sector");
                t.Equals(waza->entryOffset, ar.wazaEntry, "waza entry offset");
                t.Equals(top->entryOffset, ar.topEntry, "top entry offset");
            }
            bool keyOk = true;
            for (uint32_t i = 0; i < ro::kKeyPeriod; ++i)
                keyOk = keyOk && toc.key[i] == testKey(0x80 + i);
            t.IsTrue(keyOk, "all 88 key bytes recovered");
        });

        tc.Run("decoding refuses what is not the archive", [](TestCase &t)
        {
            Archive ar = makeArchive();
            ro::Toc toc;
            std::string error;
            Bytes bad = ar.scrambled;
            bad[0] = 'X';
            t.IsFalse(ro::decodeToc(bad.data(), bad.size(), toc, error), "wrong magic");
            t.IsFalse(error.empty(), "says why");
            Bytes shortHeader(ar.scrambled.begin(), ar.scrambled.begin() + 4096);
            t.IsFalse(ro::decodeToc(shortHeader.data(), shortHeader.size(), toc, error), "header shorter than its own sector count");
            Bytes zeros(22 * 2048, 0);
            std::memcpy(zeros.data(), "ROM ", 4);
            put32(zeros, 8, 22);
            t.IsFalse(ro::decodeToc(zeros.data(), zeros.size(), toc, error), "an empty directory");
        });

        tc.Run("a plan: case-insensitive match, ordering, sectors, unknown names ignored, refusals", [](TestCase &t)
        {
            const Archive ar = makeArchive();
            ro::Toc toc;
            std::string error;
            ro::decodeToc(ar.scrambled.data(), ar.scrambled.size(), toc, error);
            const fs::path dir = makeTempDir("plan");
            writeFile(dir / "Yoneda" / "FprWaza.BIN", pattern(5000, 1));
            writeFile(dir / "yoneda" / "extra.txt", pattern(10, 2));
            writeFile(dir / "top.bin", Bytes());
            const auto files = ro::scanFolder(dir);
            t.IsTrue(files.count("yoneda/fprwaza.bin") == 1, "scan lower-cases the relative path");
            const ro::Plan plan = ro::makePlan(toc, files);
            t.Equals(plan.replacements.size(), static_cast<size_t>(1), "only the waza is accepted");
            if (!plan.replacements.empty())
            {
                const ro::Replacement &r = plan.replacements[0];
                t.Equals(r.path, std::string("yoneda/fprwaza.bin"), "path");
                t.Equals(r.newSize, 5000u, "new size");
                t.Equals(r.sectors, 3u, "3 sectors for 5000 bytes");
                t.Equals(r.virtualSector, ro::kVirtualSectorBase, "first replacement at the virtual base");
                t.Equals(r.originalSector, 24u, "original sector remembered");
                t.Equals(r.originalSize, 3000u, "original size remembered");
            }
            bool refusedEmpty = false, ignored = false;
            for (const std::string &l : plan.log)
            {
                refusedEmpty = refusedEmpty || (l.find("REFUSED top.bin") != std::string::npos && l.find("empty") != std::string::npos);
                ignored = ignored || l.find("ignored yoneda/extra.txt") != std::string::npos;
            }
            t.IsTrue(refusedEmpty, "an empty replacement is refused and named");
            t.IsTrue(ignored, "a file that is not in the archive is ignored and named");
            std::error_code ec;
            fs::remove_all(dir, ec);
        });

        tc.Run("a replacement over the known load limit is refused with a clear line, one at the limit is taken", [](TestCase &t)
        {
            // The game's move table buffer is 0x180000 bytes (docs/formats/fprwaza.md); the archive's path in the made-up directory has
            // to be the real one for the limit to apply.
            Archive ar;
            ar.plain.assign(22 * 2048, 0);
            std::memcpy(ar.plain.data(), "ROM ", 4);
            put32(ar.plain, 8, 22);
            const std::vector<Item> sub = {{"fprwaza.bin", false, 24, 1510204}, {"fprform.bin", false, 800, 5443432}};
            Bytes child = makeBlock(4, sub, 0x200 / 128, 0, 0x80 / 128, 0);
            put32(child, 4 + 8, static_cast<uint32_t>(child.size()));
            const std::vector<Item> top = {{"yoneda", true, 0x200 / 128, static_cast<uint32_t>(child.size())}};
            Bytes root = makeBlock(736, top, 0x80 / 128, 0, 0x80 / 128, 0);
            std::memcpy(ar.plain.data() + 0x80, root.data(), root.size());
            std::memcpy(ar.plain.data() + 0x200, child.data(), child.size());
            ar.scrambled = ar.plain;
            for (uint32_t pos = 0x80; pos < ar.scrambled.size(); ++pos)
                ar.scrambled[pos] ^= testKey(pos);
            ro::Toc toc;
            std::string error;
            t.IsTrue(ro::decodeToc(ar.scrambled.data(), ar.scrambled.size(), toc, error), "decodes: " + error);
            const fs::path dir = makeTempDir("limit");
            writeFile(dir / "yoneda" / "fprwaza.bin", pattern(0x180001, 3));
            const ro::Plan over = ro::makePlan(toc, ro::scanFolder(dir));
            t.IsTrue(over.replacements.empty(), "0x180001 bytes is refused");
            bool said = false;
            for (const std::string &l : over.log)
                said = said || (l.find("REFUSED yoneda/fprwaza.bin") != std::string::npos && l.find("1572864") != std::string::npos);
            t.IsTrue(said, "the line names the file and the limit");
            writeFile(dir / "yoneda" / "fprwaza.bin", pattern(0x180000, 3));
            const ro::Plan at = ro::makePlan(toc, ro::scanFolder(dir));
            t.Equals(at.replacements.size(), static_cast<size_t>(1), "exactly 0x180000 is taken");
            t.Equals(ro::knownLoadLimit("yoneda/fprform.bin"), 0x540000ull, "the pose table's limit is known too");
            t.Equals(ro::knownLoadLimit("aikoh/database.bin"), 0ull, "others are unknown");
            std::error_code ec;
            fs::remove_all(dir, ec);
        });

        tc.Run("a larger replacement of a file with an unknown limit is taken with a warning", [](TestCase &t)
        {
            const Archive ar = makeArchive();
            ro::Toc toc;
            std::string error;
            ro::decodeToc(ar.scrambled.data(), ar.scrambled.size(), toc, error);
            const fs::path dir = makeTempDir("warn");
            writeFile(dir / "yoneda" / "other.bin", pattern(3000, 4));
            const ro::Plan plan = ro::makePlan(toc, ro::scanFolder(dir));
            t.Equals(plan.replacements.size(), static_cast<size_t>(1), "taken");
            bool warned = false;
            for (const std::string &l : plan.log)
                warned = warned || l.find("WARNING") != std::string::npos;
            t.IsTrue(warned, "warns that the game's buffer is unknown");
            std::error_code ec;
            fs::remove_all(dir, ec);
        });

        tc.Run("patching the directory changes exactly the two fields, and the result decodes to the new values", [](TestCase &t)
        {
            const Archive ar = makeArchive();
            ro::Toc toc;
            std::string error;
            ro::decodeToc(ar.scrambled.data(), ar.scrambled.size(), toc, error);
            const fs::path dir = makeTempDir("patch");
            writeFile(dir / "yoneda" / "fprwaza.bin", pattern(5000, 9));
            writeFile(dir / "yoneda" / "other.bin", pattern(50, 9));
            const ro::Plan plan = ro::makePlan(toc, ro::scanFolder(dir));
            t.Equals(plan.replacements.size(), static_cast<size_t>(2), "two replacements");
            Bytes patched = ar.scrambled;
            const size_t fields = ro::patchDirectoryBytes(toc, plan, 0, patched.data(), patched.size());
            t.Equals(fields, static_cast<size_t>(4), "four fields (start and size of two files)");
            size_t differing = 0;
            for (size_t i = 0; i < patched.size(); ++i)
                differing += patched[i] != ar.scrambled[i];
            t.IsTrue(differing >= 1 && differing <= 16, "at most the 16 bytes of the four fields differ");
            ro::Toc after;
            t.IsTrue(ro::decodeToc(patched.data(), patched.size(), after, error), "the patched header still decodes: " + error);
            const ro::TocFile *waza = find(after, "yoneda/fprwaza.bin");
            const ro::TocFile *other = find(after, "yoneda/other.bin");
            const ro::TocFile *top = find(after, "top.bin");
            t.IsTrue(waza && other && top, "all three files present");
            if (waza && other && top)
            {
                t.Equals(waza->size, 5000u, "waza has the new size");
                t.Equals(waza->sector, ro::kVirtualSectorBase, "waza has its virtual sector");
                t.Equals(other->size, 50u, "other has the new size");
                t.Equals(other->sector, ro::kVirtualSectorBase + 3 + 1, "other follows after waza's 3 sectors and one empty sector");
                t.Equals(top->sector, 22u, "top.bin untouched (sector)");
                t.Equals(top->size, 4096u, "top.bin untouched (size)");
            }
            // The key is unchanged.
            t.IsTrue(after.key == toc.key, "the key recovered from the patched header is the same");
            std::error_code ec;
            fs::remove_all(dir, ec);
        });

        tc.Run("patching in pieces (a read that starts or ends inside a field) gives the same bytes as patching all at once", [](TestCase &t)
        {
            const Archive ar = makeArchive();
            ro::Toc toc;
            std::string error;
            ro::decodeToc(ar.scrambled.data(), ar.scrambled.size(), toc, error);
            const fs::path dir = makeTempDir("pieces");
            writeFile(dir / "yoneda" / "fprwaza.bin", pattern(4097, 5));
            const ro::Plan plan = ro::makePlan(toc, ro::scanFolder(dir));
            Bytes whole = ar.scrambled;
            ro::patchDirectoryBytes(toc, plan, 0, whole.data(), whole.size());
            for (size_t chunk : {1u, 3u, 5u, 7u, 13u, 100u, 2048u})
            {
                Bytes pieces = ar.scrambled;
                for (size_t at = 0; at < pieces.size(); at += chunk)
                    ro::patchDirectoryBytes(toc, plan, at, pieces.data() + at, std::min(chunk, pieces.size() - at));
                t.IsTrue(pieces == whole, "chunks of " + std::to_string(chunk) + " bytes");
            }
            // A read away from the directory is not touched.
            Bytes data = pattern(2048, 1);
            const Bytes copy = data;
            t.Equals(ro::patchDirectoryBytes(toc, plan, 40 * 2048ull, data.data(), data.size()), static_cast<size_t>(0), "no field outside the header");
            t.IsTrue(data == copy, "bytes untouched");
            std::error_code ec;
            fs::remove_all(dir, ec);
        });

        tc.Run("virtual sectors are served from the host file, zero past its end, and nothing else is claimed", [](TestCase &t)
        {
            const Archive ar = makeArchive();
            ro::Toc toc;
            std::string error;
            ro::decodeToc(ar.scrambled.data(), ar.scrambled.size(), toc, error);
            const fs::path dir = makeTempDir("virt");
            const Bytes content = pattern(5000, 77);
            writeFile(dir / "yoneda" / "fprwaza.bin", content);
            const ro::Plan plan = ro::makePlan(toc, ro::scanFolder(dir));
            const uint32_t v = plan.replacements[0].virtualSector;
            Bytes buf(3 * 2048, 0xEE);
            t.IsTrue(ro::readVirtualSectors(plan, v, 3, buf.data(), buf.size()), "the whole file");
            t.IsTrue(std::equal(content.begin(), content.end(), buf.begin()), "its bytes");
            bool zeros = true;
            for (size_t i = content.size(); i < buf.size(); ++i)
                zeros = zeros && buf[i] == 0;
            t.IsTrue(zeros, "zero padding after the last byte");
            Bytes second(2048, 0xEE);
            t.IsTrue(ro::readVirtualSectors(plan, v + 1, 1, second.data(), second.size()), "the second sector");
            t.IsTrue(std::equal(second.begin(), second.end(), content.begin() + 2048), "its bytes");
            Bytes shortRead(100, 0xEE);
            t.IsTrue(ro::readVirtualSectors(plan, v + 2, 1, shortRead.data(), shortRead.size()), "a read shorter than a sector");
            t.IsTrue(std::equal(shortRead.begin(), shortRead.end(), content.begin() + 4096), "its bytes");
            Bytes none(2048, 0xEE);
            t.IsFalse(ro::readVirtualSectors(plan, v + 3, 1, none.data(), none.size()), "the sector after the file is not claimed");
            t.IsFalse(ro::readVirtualSectors(plan, v - 1, 1, none.data(), none.size()), "the sector before is not claimed");
            t.IsFalse(ro::readVirtualSectors(plan, 24, 1, none.data(), none.size()), "the file's original sector is not claimed");
            t.Equals(none[0], static_cast<uint8_t>(0xEE), "a refused read leaves the buffer alone");
            std::error_code ec;
            fs::remove_all(dir, ec);
        });

        tc.Run("through readCdSectors: directory patched, virtual sectors served, other sectors and the off switch unchanged", [](TestCase &t)
        {
            const Archive ar = makeArchive();
            const fs::path cd = makeTempDir("cd");
            const fs::path over = makeTempDir("over");
            // FPR.ROM: the header, then 10 data sectors with a pattern per sector.
            Bytes rom = ar.scrambled;
            Bytes data = pattern(10 * 2048, 200);
            rom.insert(rom.end(), data.begin(), data.end());
            writeFile(cd / "FPR.ROM", rom);
            const Bytes replacement = pattern(7000, 31);
            writeFile(over / "yoneda" / "fprwaza.bin", replacement);

            const PS2Runtime::IoPaths saved = PS2Runtime::getIoPaths();
            PS2Runtime::IoPaths paths = saved;
            paths.cdRoot = cd;
            PS2Runtime::setIoPaths(paths);

            CdFileEntry entry;
            t.IsTrue(registerCdFile("FPR.ROM", entry), "the archive is registered with the CD layer");
            const size_t headerBytes = 22 * 2048;

            // off: the bytes are exactly the file's
            ro::resetForTests(fs::path());
            Bytes off(headerBytes);
            t.IsTrue(readCdSectors(entry.baseLbn, 22, off.data(), off.size()), "read the header (override off)");
            t.IsTrue(off == ar.scrambled, "unchanged with the switch off");

            // on
            ro::resetForTests(over);
            Bytes on(headerBytes);
            t.IsTrue(readCdSectors(entry.baseLbn, 22, on.data(), on.size()), "read the header (override on)");
            ro::Toc toc;
            std::string error;
            t.IsTrue(ro::decodeToc(on.data(), on.size(), toc, error), "the served header decodes: " + error);
            const ro::TocFile *waza = find(toc, "yoneda/fprwaza.bin");
            t.IsNotNull(waza, "the file is in the served directory");
            if (waza)
            {
                t.Equals(waza->size, 7000u, "the game is told the new size");
                t.Equals(waza->sector, ro::kVirtualSectorBase, "and the new place");
                Bytes got(4 * 2048, 0xEE);
                t.IsTrue(readCdSectors(entry.baseLbn + waza->sector, 4, got.data(), got.size()), "the read at that place succeeds");
                t.IsTrue(std::equal(replacement.begin(), replacement.end(), got.begin()), "and returns the replacement's bytes");
                t.IsTrue(got[7000] == 0 && got[8191] == 0, "padded with zeros");
            }
            // other data sectors come from the archive as before
            Bytes plainSector(2048), servedSector(2048);
            std::copy(rom.begin() + 25 * 2048, rom.begin() + 26 * 2048, plainSector.begin());
            t.IsTrue(readCdSectors(entry.baseLbn + 25, 1, servedSector.data(), servedSector.size()), "a data sector reads");
            t.IsTrue(plainSector == servedSector, "and is the archive's own");
            // the header read one sector at a time gives the same bytes as in one go
            Bytes oneByOne(headerBytes);
            for (uint32_t s = 0; s < 22; ++s)
                readCdSectors(entry.baseLbn + s, 1, oneByOne.data() + s * 2048, 2048);
            t.IsTrue(oneByOne == on, "a sector at a time gives the same directory");
            // a sector that nobody serves still fails
            Bytes nothing(2048);
            t.IsFalse(readCdSectors(entry.baseLbn + ro::kVirtualSectorBase + 500, 1, nothing.data(), nothing.size()),
                      "an unserved virtual sector is not invented");

            ro::resetForTests(fs::path());
            PS2Runtime::setIoPaths(saved);
            std::error_code ec;
            fs::remove_all(cd, ec);
            fs::remove_all(over, ec);
        });

        tc.Run("the real archive (when present): the C++ directory decoder agrees with tools/inventory.py's table", [](TestCase &t)
        {
            const fs::path root = findRepoRoot();
            const fs::path csv = root / "baserom" / "out" / "fpr_rom_toc.csv";
            std::error_code ec;
            if (root.empty() || !fs::exists(csv, ec))
            {
                std::printf("(skipped: needs baserom/iso/FPR.ROM and baserom/out/fpr_rom_toc.csv) ");
                return;
            }
            std::ifstream in(root / "baserom" / "iso" / "FPR.ROM", std::ios::binary);
            Bytes header(22 * 2048);
            in.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
            ro::Toc toc;
            std::string error;
            t.IsTrue(ro::decodeToc(header.data(), header.size(), toc, error), "decodes the real directory: " + error);
            t.Equals(toc.files.size(), static_cast<size_t>(1797), "1,797 files");
            // the table written by tools/inventory.py: path,dir,name,ext,sector,offset,size,...
            std::ifstream list(csv);
            std::string line;
            std::getline(list, line);
            std::map<std::string, std::pair<uint32_t, uint32_t>> expect;
            while (std::getline(list, line))
            {
                std::vector<std::string> cols;
                std::stringstream ss(line);
                std::string col;
                while (std::getline(ss, col, ','))
                    cols.push_back(col);
                if (cols.size() > 6)
                    expect[cols[0]] = {static_cast<uint32_t>(std::stoul(cols[4])), static_cast<uint32_t>(std::stoul(cols[6]))};
            }
            t.Equals(expect.size(), static_cast<size_t>(1797), "the table has 1,797 rows");
            size_t same = 0;
            for (const ro::TocFile &f : toc.files)
            {
                const auto it = expect.find(f.path);
                same += (it != expect.end() && it->second.first == f.sector && it->second.second == f.size);
            }
            t.Equals(same, static_cast<size_t>(1797), "every file has the same start sector and size as tools/inventory.py reports");
            const ro::TocFile *waza = find(toc, "yoneda/fprwaza.bin");
            t.IsNotNull(waza, "the move table is there");
            if (waza)
            {
                t.Equals(waza->sector, 379297u, "waza start sector");
                t.Equals(waza->size, 1510204u, "waza size");
            }
            // A plan against the real directory, patched, then decoded again.
            const fs::path dir = makeTempDir("real");
            writeFile(dir / "yoneda" / "fprwaza.bin", pattern(1520000, 6));
            const ro::Plan plan = ro::makePlan(toc, ro::scanFolder(dir));
            t.Equals(plan.replacements.size(), static_cast<size_t>(1), "the replacement is accepted");
            Bytes patched = header;
            ro::patchDirectoryBytes(toc, plan, 0, patched.data(), patched.size());
            ro::Toc after;
            t.IsTrue(ro::decodeToc(patched.data(), patched.size(), after, error), "the patched real directory decodes: " + error);
            const ro::TocFile *w2 = find(after, "yoneda/fprwaza.bin");
            t.IsNotNull(w2, "still there");
            if (w2)
            {
                t.Equals(w2->size, 1520000u, "new size");
                t.Equals(w2->sector, ro::kVirtualSectorBase, "new place");
            }
            size_t changed = 0;
            for (const ro::TocFile &f : after.files)
            {
                const ro::TocFile *o = find(toc, f.path);
                changed += (!o || o->sector != f.sector || o->size != f.size);
            }
            t.Equals(changed, static_cast<size_t>(1), "no other file's entry changed");
            fs::remove_all(dir, ec);
        });
    });

    MiniTest::Case("MatchProbe", [](TestCase &tc)
    {
        tc.Run("field specs: offsets, absolute addresses, deref, widths, names", [](TestCase &t)
        {
            std::vector<ps2x_match_probe::FieldSpec> f;
            std::string error;
            t.IsTrue(ps2x_match_probe::parseFieldSpecs("rec=+0x1AC:4, +0x1B0:4,dur=+0x1B0>6:1,@0x37E5C0:2,raw=+0:48", f, error), "parses: " + error);
            t.Equals(f.size(), static_cast<size_t>(5), "five fields");
            if (f.size() == 5)
            {
                t.Equals(f[0].name, std::string("rec"), "name");
                t.Equals(f[0].where, 0x1ACu, "offset");
                t.IsFalse(f[0].absolute, "relative");
                t.Equals(f[1].name, std::string("f1B0"), "generated name");
                t.IsTrue(f[2].deref && f[2].derefOffset == 6 && f[2].width == 1, "deref and width");
                t.IsTrue(f[3].absolute && f[3].where == 0x37E5C0u && f[3].width == 2, "absolute");
                t.Equals(f[4].width, 48u, "a wide field");
            }
            t.IsFalse(ps2x_match_probe::parseFieldSpecs("+0x10", f, error), "missing width");
            t.IsFalse(ps2x_match_probe::parseFieldSpecs("0x10:4", f, error), "missing + or @");
            t.IsFalse(ps2x_match_probe::parseFieldSpecs("+zz:4", f, error), "bad number");
            t.IsFalse(ps2x_match_probe::parseFieldSpecs("+0x10:0", f, error), "zero width");
            t.IsFalse(ps2x_match_probe::parseFieldSpecs("", f, error), "no fields");
        });

        tc.Run("dump spec", [](TestCase &t)
        {
            ps2x_match_probe::Config cfg;
            std::string error;
            t.IsTrue(ps2x_match_probe::parseDumpSpec("out.bin,0x37D3D0,1510204,ptr", cfg, error), "parses: " + error);
            t.IsTrue(cfg.dumpIsPointer && cfg.dumpAddr == 0x37D3D0u && cfg.dumpLen == 1510204u && cfg.dumpFile == "out.bin", "fields");
            ps2x_match_probe::Config plain;
            t.IsTrue(ps2x_match_probe::parseDumpSpec("a.bin,4096,16", plain, error), "no ptr");
            t.IsFalse(plain.dumpIsPointer, "plain address");
            t.IsFalse(ps2x_match_probe::parseDumpSpec("a.bin,4096", plain, error), "too few parts");
            t.IsFalse(ps2x_match_probe::parseDumpSpec("a.bin,4096,16,ptrr", plain, error), "bad flag");
        });

        tc.Run("the sampler writes the header, one row per changed wrestler, and nothing while nothing changes", [](TestCase &t)
        {
            // 3 wrestlers of 0x40 bytes at 0x2000; the base pointer sits at 0x100.
            ps2x_match_probe::Config cfg;
            cfg.basePtrAddr = 0x100;
            cfg.stride = 0x40;
            cfg.count = 3;
            std::string error;
            t.IsTrue(ps2x_match_probe::parseFieldSpecs("rec=+0x10:4,unit=+0x14:4,dur=+0x14>6:1,flag=+0x18:2", cfg.fields, error), error);
            Bytes ram(0x10000, 0);
            Rows rows;
            ps2x_match_probe::Sampler sampler(cfg, Rows::sink, &rows);
            t.Equals(rows.text, std::string("frame,tick,w,rec,unit,dur,flag\n"), "header");
            t.Equals(sampler.sample(ram.data(), static_cast<uint32_t>(ram.size()), 1, 10), static_cast<size_t>(0), "no match yet: no rows");
            t.IsFalse(sampler.matchSeen(), "not seen");

            guestPut32(ram, 0x100, 0x2000);
            guestPut32(ram, 0x2000 + 0x10, 0x5000);        // wrestler 0 rec
            guestPut32(ram, 0x2000 + 0x14, 0x6000);        // wrestler 0 unit pointer
            ram[0x6000 + 6] = 9;                           // its duration
            guestPut32(ram, 0x2040 + 0x10, 0x5100);        // wrestler 1
            t.Equals(sampler.sample(ram.data(), static_cast<uint32_t>(ram.size()), 2, 11), static_cast<size_t>(3), "first sight: one row per wrestler");
            t.IsTrue(sampler.matchSeen(), "match seen");
            t.Equals(sampler.sample(ram.data(), static_cast<uint32_t>(ram.size()), 3, 12), static_cast<size_t>(0), "no change: no rows");
            ram[0x6000 + 6] = 4;
            t.Equals(sampler.sample(ram.data(), static_cast<uint32_t>(ram.size()), 4, 13), static_cast<size_t>(1), "one value changed: one row");
            const auto lines = rows.lines();
            t.Equals(lines.size(), static_cast<size_t>(1 + 3 + 1), "header + 3 + 1 rows");
            if (lines.size() == 5)
            {
                t.Equals(lines[1], std::string("2,11,0,0x5000,0x6000,0x9,0x0"), "wrestler 0 first row (deref read the duration byte)");
                t.Equals(lines[2], std::string("2,11,1,0x5100,0x0,-,0x0"), "wrestler 1: a null pointer reads as '-'");
                t.Equals(lines[3], std::string("2,11,2,0x0,0x0,-,0x0"), "wrestler 2: all zero");
                t.Equals(lines[4], std::string("4,13,0,,,0x4,"), "the changed row holds only the cell that changed");
            }
        });

        tc.Run("absolute fields have their own row, wide fields are hex strings, out-of-range reads are '-'", [](TestCase &t)
        {
            ps2x_match_probe::Config cfg;
            cfg.basePtrAddr = 0x100;
            cfg.stride = 0x40;
            cfg.count = 1;
            std::string error;
            t.IsTrue(ps2x_match_probe::parseFieldSpecs("rng=@0x200:2,raw=+0x8:3,far=+0x8>0x20000000:4", cfg.fields, error), error);
            Bytes ram(0x4000, 0);
            ram[0x200] = 0x34;
            ram[0x201] = 0x12;
            guestPut32(ram, 0x100, 0x1000);
            ram[0x1008] = 0xAA;
            ram[0x1009] = 0xBB;
            ram[0x100A] = 0xCC;
            guestPut32(ram, 0x1008 + 0, 0x00CCBBAA); // the same bytes; the deref field reads a pointer from here
            Rows rows;
            ps2x_match_probe::Sampler sampler(cfg, Rows::sink, &rows);
            sampler.sample(ram.data(), static_cast<uint32_t>(ram.size()), 1, 1);
            const auto lines = rows.lines();
            t.Equals(lines.size(), static_cast<size_t>(3), "header, the absolute row, one wrestler row");
            if (lines.size() == 3)
            {
                t.Equals(lines[1], std::string("1,1,-1,0x1234,,"), "absolute row: its value, the other columns empty");
                t.Equals(lines[2], std::string("1,1,0,,aabbcc,-"), "wrestler row: wide field as hex bytes, absolute column empty, unreadable '-'");
            }
            ram[0x200] = 0x35;
            t.Equals(sampler.sample(ram.data(), static_cast<uint32_t>(ram.size()), 2, 2), static_cast<size_t>(1), "only the absolute row when only it changed");
            const auto after = rows.lines();
            t.IsTrue(after.size() == 4 && after[3] == "2,2,-1,0x1235,,", "its row holds just the changed cell");
        });
    });
}
