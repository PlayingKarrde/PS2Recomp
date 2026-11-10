#include "MiniTest.h"

#include "runtime/gs/gs_font.h"
#include "runtime/gs/gs_png.h"
#include "runtime/gs/gs_tex_pack.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

// Tests for HD text (task t2): the glyph codes and slots, the file names and size rule, the registry of glyph bitmaps and the HD canvas
// (gs_font.h). The renderer and the shader are covered by the scripted game runs, not here.

namespace
{
    // In-memory files for the atlas.
    class MemorySource final : public TexPack::FileSource
    {
    public:
        struct File
        {
            std::vector<uint8_t> bytes;
            std::string provider = "mod.a";
            uint32_t order = 0;
        };
        std::map<std::string, File> files;
        std::vector<std::string> problems;

        std::vector<Entry> list(const std::string &prefix) override
        {
            std::vector<Entry> out;
            for (const auto &f : files)
                if (f.first.compare(0, prefix.size(), prefix) == 0)
                    out.push_back(Entry{f.first, f.second.provider, f.second.order});
            return out;
        }
        bool read(const std::string &path, std::vector<uint8_t> &bytes, std::string &error) override
        {
            const auto it = files.find(path);
            if (it == files.end())
            {
                error = "missing";
                return false;
            }
            bytes = it->second.bytes;
            return true;
        }
        void problem(const std::string &provider, const std::string &message) override { problems.push_back(provider + ": " + message); }

        void addCoverage(const std::string &name, uint32_t size, const std::vector<uint8_t> &grey, const std::string &provider = "mod.a", uint32_t order = 0)
        {
            File f;
            f.bytes = GSPng::Encode(size, size, 0, grey);
            f.provider = provider;
            f.order = order;
            files[std::string(FontHD::kFontPrefix) + name] = f;
        }
    };

    // A glyph of the game: 512 bytes, two pixels per byte, the left pixel in the low nibble. `pixel(x, y)` returns 0..15.
    template <class F>
    std::vector<uint8_t> makeBitmap(F pixel)
    {
        std::vector<uint8_t> b(512);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; x += 2)
                b[y * 16 + x / 2] = static_cast<uint8_t>((pixel(x, y) & 15) | ((pixel(x + 1, y) & 15) << 4));
        return b;
    }

    // Puts a glyph bitmap into cell (cx, cy) of a canvas of palette indices.
    void putCell(std::vector<uint8_t> &canvas, int cx, int cy, const std::vector<uint8_t> &bitmap)
    {
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; x += 2)
            {
                const uint8_t b = bitmap[y * 16 + x / 2];
                canvas[(cy * 32 + y) * 512 + cx * 32 + x] = b & 15;
                canvas[(cy * 32 + y) * 512 + cx * 32 + x + 1] = b >> 4;
            }
    }
}

void register_gs_font_tests()
{
    MiniTest::Case("FontCodes", [](TestCase &tc)
    {
        tc.Run("slots: the game's routine at 0x138A90, both ways", [](TestCase &t)
        {
            t.Equals(FontHD::SlotOfCode(0x20), 0, "space is slot 0");
            t.Equals(FontHD::SlotOfCode(0x41), 33, "A");
            t.Equals(FontHD::SlotOfCode(0x7F), 95, "0x7F is the last single-byte slot");
            t.Equals(FontHD::SlotOfCode(0x8140), 96, "the first two-byte code follows the 96 ASCII slots");
            t.Equals(FontHD::SlotOfCode(0x8141), 97, "next trail byte");
            t.Equals(FontHD::SlotOfCode(0x817E), 96 + 62, "last trail byte before 0x7F");
            t.Equals(FontHD::SlotOfCode(0x8180), 96 + 63, "0x7F is skipped");
            t.Equals(FontHD::SlotOfCode(0x81FC), 96 + 187, "last trail byte");
            t.Equals(FontHD::SlotOfCode(0x8240), 96 + 188, "next lead byte");
            t.Equals(FontHD::SlotOfCode(0xE040), 96 + 31 * 188, "second range");
            t.Equals(FontHD::SlotOfCode(0xF040), 96 + (31 + 11) * 188, "third range");
            t.Equals(FontHD::SlotOfCode(0xF9FC), 9871, "the last slot");
            t.Equals(FontHD::SlotOfCode(0x817F), -1, "trail 0x7F");
            t.Equals(FontHD::SlotOfCode(0x81FD), -1, "trail 0xFD");
            t.Equals(FontHD::SlotOfCode(0x813F), -1, "trail below 0x40");
            t.Equals(FontHD::SlotOfCode(0xA0A0), -1, "between the ranges");
            t.Equals(FontHD::SlotOfCode(0x1F), -1, "control code");
            t.Equals(FontHD::SlotOfCode(0xFA40), -1, "past the last range");
            bool roundTrip = true;
            for (uint32_t slot = 0; slot < 9872; ++slot)
                if (FontHD::SlotOfCode(FontHD::CodeOfSlot(slot)) != static_cast<int>(slot))
                    roundTrip = false;
            t.IsTrue(roundTrip, "CodeOfSlot and SlotOfCode are inverses over all 9,872 slots");
        });

        tc.Run("characters: Unicode of a code and the codes of a character", [](TestCase &t)
        {
            t.Equals(FontHD::UnicodeOfCode(0x41), 0x41u, "A");
            t.Equals(FontHD::UnicodeOfCode(0x82A0), 0x3042u, "hiragana A");
            t.Equals(FontHD::UnicodeOfCode(0x889F), 0x4E9Cu, "the first kanji");
            t.Equals(FontHD::UnicodeOfCode(0xF040), 0u, "the game's own symbols have no character");
            t.Equals(FontHD::UnicodeOfCode(0x8000), 0u, "no slot");
            const auto codes = FontHD::CodesOfUnicode(0x3042);
            t.IsTrue(codes.size() == 1 && codes[0] == 0x82A0u, "hiragana A has one code");
            const auto a = FontHD::CodesOfUnicode(0x41);
            t.IsTrue(a.size() == 1 && a[0] == 0x41u, "A is the byte 0x41 (and not the full-width A)");
            t.IsTrue(FontHD::CodesOfUnicode(0x1F600).empty(), "no emoji");
            // the NEC duplicates of Shift-JIS: one character, more than one code
            const auto approx = FontHD::CodesOfUnicode(0x2252);
            t.IsTrue(approx.size() >= 2 && std::find(approx.begin(), approx.end(), 0x81E0u) != approx.end() &&
                     std::find(approx.begin(), approx.end(), 0x8790u) != approx.end(), "U+2252 has two codes (0x81E0 and 0x8790)");
        });
    });

    MiniTest::Case("FontFiles", [](TestCase &tc)
    {
        tc.Run("file names: c<code>, u<character>, .bold, any case", [](TestCase &t)
        {
            FontHD::FileName n;
            t.IsTrue(FontHD::ParseFileName("c41.png", n) && !n.unicode && n.value == 0x41u && !n.bold, "c41.png");
            t.IsTrue(FontHD::ParseFileName("C889F.PNG", n) && !n.unicode && n.value == 0x889Fu && !n.bold, "C889F.PNG");
            t.IsTrue(FontHD::ParseFileName("c0041.png", n) && n.value == 0x41u, "leading zeros");
            t.IsTrue(FontHD::ParseFileName("u0041.png", n) && n.unicode && n.value == 0x41u && !n.bold, "u0041.png");
            t.IsTrue(FontHD::ParseFileName("u1f600.bold.png", n) && n.unicode && n.value == 0x1F600u && n.bold, "u1f600.bold.png");
            t.IsTrue(FontHD::ParseFileName("c41.Bold.png", n) && n.bold, ".Bold in any case");
            t.IsFalse(FontHD::ParseFileName("c41.jpg", n), "not a png");
            t.IsFalse(FontHD::ParseFileName("c.png", n), "no digits");
            t.IsFalse(FontHD::ParseFileName("x41.png", n), "unknown prefix");
            t.IsFalse(FontHD::ParseFileName("c41g.png", n), "not hex");
            t.IsFalse(FontHD::ParseFileName("c1234567.png", n), "more than six digits");
            t.IsFalse(FontHD::ParseFileName("c41.idx.png", n), "an index image is not a glyph picture");
            t.IsFalse(FontHD::ParseFileName("0123456789abcdef.png", n), "a texture key is not a glyph");
        });

        tc.Run("size rule: 32 S square, S from 1 to 8", [](TestCase &t)
        {
            std::string why;
            t.Equals(FontHD::ScaleOf(32, 32, &why), 1, "32");
            t.Equals(FontHD::ScaleOf(128, 128), 4, "128");
            t.Equals(FontHD::ScaleOf(256, 256), 8, "256");
            t.Equals(FontHD::ScaleOf(288, 288, &why), 0, "9x");
            t.IsTrue(why.find("32 S") != std::string::npos, "a reason is given: " + why);
            t.Equals(FontHD::ScaleOf(96, 64), 0, "not square");
            t.Equals(FontHD::ScaleOf(100, 100), 0, "not a multiple of 32");
            t.Equals(FontHD::ScaleOf(16, 16), 0, "smaller than the cell");
            t.Equals(FontHD::ScaleOf(0, 0), 0, "empty");
        });
    });

    MiniTest::Case("FontRegistry", [](TestCase &tc)
    {
        tc.Run("a bitmap is found by its hash; the first glyph reported keeps it; blank is ignored", [](TestCase &t)
        {
            FontHD::Registry r;
            const auto a = makeBitmap([](int x, int y) { return (x * 3 + y) & 15; });
            const auto b = makeBitmap([](int x, int y) { return (x + y * 5 + 1) & 15; });
            const std::vector<uint8_t> blank(512, 0);
            r.Note(0x41, false, a.data());
            r.Note(0x41, true, b.data());
            r.Note(0x20, false, blank.data());
            r.Note(0x42, false, a.data());
            t.Equals(r.Size(), static_cast<size_t>(2), "two bitmaps");
            uint32_t id = 0;
            t.IsTrue(r.Lookup(FontHD::BitmapHash(a.data()), id) && FontHD::IdCode(id) == 0x41u && !FontHD::IdBold(id), "a is the plain A (not the later B)");
            t.IsTrue(r.Lookup(FontHD::BitmapHash(b.data()), id) && FontHD::IdCode(id) == 0x41u && FontHD::IdBold(id), "b is the outline A");
            t.IsFalse(r.Lookup(FontHD::BitmapHash(blank.data()), id), "blank was not kept");
            t.Equals(r.Lines().size(), static_cast<size_t>(2), "two lines");
        });
    });

    MiniTest::Case("DevPack", [](TestCase &tc)
    {
        tc.Run("loose files act like a mod that loads last and replace a mod's file of the same path", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            const fs::path dir = fs::temp_directory_path() / "ps2x_devpack_test_a";
            std::error_code ec;
            fs::remove_all(dir, ec);
            auto base = std::make_shared<MemorySource>();
            base->addCoverage("c41.png", 32, std::vector<uint8_t>(32 * 32, 50), "mod.a", 3);
            base->addCoverage("c42.png", 32, std::vector<uint8_t>(32 * 32, 60), "mod.a", 3);
            base->files["assets/textures/0123456789abcdef.png"].bytes = {1, 2, 3};
            {
                TexPack::DevPackSource pack(dir.string(), base, 30, false);
                t.IsTrue(fs::is_directory(dir / "textures") && fs::is_directory(dir / "font"), "the folders are made");
                t.Equals(pack.fileCount(), 0u, "empty");
                auto put = [&](const char *sub, const char *name, const std::vector<uint8_t> &bytes)
                {
                    fs::create_directories(dir / sub, ec);
                    std::ofstream out(dir / sub / name, std::ios::binary);
                    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                };
                put("font", "c41.png", GSPng::Encode(32, 32, 0, std::vector<uint8_t>(32 * 32, 200)));
                put("textures", "fedcba9876543210.png", {9, 9});
                put("textures", "notes.txt", {1});
                t.Equals(pack.fileCount(), 3u, "three loose files");
                const auto fonts = pack.list("assets/font/");
                size_t c41 = 0, c42 = 0;
                for (const auto &e : fonts)
                {
                    if (e.path == "assets/font/c41.png")
                    {
                        ++c41;
                        t.IsTrue(e.provider == "dev-pack" && e.order > 1000u, "the dev pack's c41 has the highest order and its own provider");
                    }
                    if (e.path == "assets/font/c42.png")
                    {
                        ++c42;
                        t.Equals(e.provider, std::string("mod.a"), "c42 still comes from the mod");
                    }
                }
                t.IsTrue(c41 == 1 && c42 == 1, "one entry per path: the loose c41 replaced the mod's");
                const auto textures = pack.list("assets/textures/");
                t.Equals(textures.size(), static_cast<size_t>(3), "the mod's texture, the loose texture and the stray file");
                std::vector<uint8_t> bytes;
                std::string error;
                t.IsTrue(pack.read("assets/font/c41.png", bytes, error) && bytes.size() > 8 && bytes[1] == 'P', "reads the loose file");
                t.IsTrue(pack.read("assets/font/c42.png", bytes, error), "reads the mod's file through the base");
                t.IsFalse(pack.read("assets/font/c99.png", bytes, error), "a missing file");
                t.IsTrue(pack.list("assets/other/").empty(), "other prefixes are not served");
                pack.problem("dev-pack", "c41.png is 3x3");
                pack.problem("dev-pack", "c41.png is 3x3");
                pack.problem("mod.a", "a mod's complaint goes to the base");
                t.Equals(pack.notes().size(), static_cast<size_t>(1), "a dev pack complaint is kept once");
                pack.clearNotes();
                t.IsTrue(pack.notes().empty(), "cleared");
                // the atlas takes the loose file over the mod's
                FontHD::Atlas atlas(std::make_shared<TexPack::DevPackSource>(dir.string(), base, 30, false));
                std::vector<uint8_t> a;
                t.IsTrue(atlas.GlyphPicture(0x41, false, a) && a[16 * 32 + 16] == 200, "the atlas shows the dev pack's c41");
                t.IsTrue(atlas.GlyphPicture(0x42, false, a) && a[16 * 32 + 16] == 60, "and the mod's c42");
            }
            fs::remove_all(dir, ec);
        });

        tc.Run("the watcher reports a change once, after the files stop changing", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            const fs::path dir = fs::temp_directory_path() / "ps2x_devpack_test_b";
            std::error_code ec;
            fs::remove_all(dir, ec);
            TexPack::DevPackSource pack(dir.string(), nullptr, 20, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            t.IsFalse(pack.changed(), "nothing changed yet");
            auto wait = [&](bool expect)
            {
                for (int i = 0; i < 100; ++i)
                {
                    if (pack.changed())
                        return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    if (!expect && i > 15)
                        break;
                }
                return false;
            };
            {
                std::ofstream out(dir / "textures" / "0000000000000001.png", std::ios::binary);
                out << "abc";
            }
            t.IsTrue(wait(true), "a new file is noticed");
            t.IsFalse(wait(false), "and reported once");
            {
                std::ofstream out(dir / "textures" / "0000000000000001.png", std::ios::binary);
                out << "abcdef";
            }
            t.IsTrue(wait(true), "a changed file (another size) is noticed");
            fs::remove(dir / "textures" / "0000000000000001.png", ec);
            t.IsTrue(wait(true), "a removed file is noticed");
            fs::remove_all(dir, ec);
        });
    });

    MiniTest::Case("FontAtlas", [](TestCase &tc)
    {
        tc.Run("a cell with a mod's picture shows the picture; other cells show the original enlarged", [](TestCase &t)
        {
            auto source = std::make_shared<MemorySource>();
            const auto bitmapA = makeBitmap([](int x, int y) { return (x >= 8 && x < 20 && y >= 8 && y < 20) ? 15 : 0; });
            const auto bitmapB = makeBitmap([](int x, int y) { return (x >= 4 && x < 12 && y >= 4 && y < 12) ? 7 : 0; });
            FontHD::Glyphs().Clear();
            FontHD::Glyphs().Note(0x41, false, bitmapA.data());
            FontHD::Glyphs().Note(0x42, false, bitmapB.data());
            std::vector<uint8_t> hdA(64 * 64, 0);
            for (int y = 16; y < 40; ++y)
                for (int x = 16; x < 40; ++x)
                    hdA[y * 64 + x] = 200;
            source->addCoverage("u0041.png", 64, hdA);
            FontHD::Atlas atlas(source);
            t.IsFalse(atlas.empty(), "one glyph file");
            t.Equals(atlas.scale(), 2u, "scale 2");
            std::vector<uint8_t> canvas(512 * 512, 0);
            putCell(canvas, 3, 1, bitmapA);
            putCell(canvas, 5, 2, bitmapB);
            const std::vector<uint8_t> *pixels = nullptr;
            t.IsTrue(atlas.Compose(canvas.data(), pixels) && pixels && pixels->size() == 1024u * 1024u, "composed at 2x");
            if (!pixels || pixels->size() != 1024u * 1024u)
                return;
            auto at = [&](int x, int y) { return (*pixels)[static_cast<size_t>(y) * 1024 + x]; };
            // cell (3,1) holds A: the HD picture, 64 pixels per cell
            t.Equals(static_cast<int>(at(3 * 64 + 20, 1 * 64 + 20)), 200, "inside the HD picture of A");
            t.Equals(static_cast<int>(at(3 * 64 + 5, 1 * 64 + 5)), 0, "outside it");
            // cell (5,2) holds B, a glyph without a picture: the original enlarged (grey level 7 -> 7 * 255 / 15 = 119 in the middle of the block)
            t.Equals(static_cast<int>(at(5 * 64 + 16, 2 * 64 + 16)), 119, "the middle of B's block is the original's level 7");
            t.Equals(static_cast<int>(at(5 * 64 + 40, 2 * 64 + 40)), 0, "B's empty corner");
            // an empty cell stays empty
            t.Equals(static_cast<int>(at(100, 100)), 0, "a blank cell");
            const FontHD::AtlasStats s = atlas.stats();
            t.Equals(s.lastHd, 1u, "one cell from a picture");
            t.Equals(s.lastFallback, 1u, "one cell enlarged");
            t.Equals(s.lastUnknown, 0u, "none unknown");
            // a cell whose glyph the game never reported is enlarged too, and counted
            const auto stranger = makeBitmap([](int x, int y) { return (x == 10 && y == 10) ? 15 : 0; });
            putCell(canvas, 7, 7, stranger);
            t.IsTrue(atlas.Compose(canvas.data(), pixels), "recomposed");
            t.Equals(atlas.stats().lastUnknown, 1u, "one unknown");
            // a cell that changes is redrawn: B's cell now holds A
            putCell(canvas, 5, 2, bitmapA);
            t.IsTrue(atlas.Compose(canvas.data(), pixels), "recomposed again");
            auto at2 = [&](int x, int y) { return (*pixels)[static_cast<size_t>(y) * 1024 + x]; };
            t.Equals(static_cast<int>(at2(5 * 64 + 20, 2 * 64 + 20)), 200, "the changed cell shows A's picture");
        });

        tc.Run("the outline version of a glyph with no picture of its own is the plain picture grown by three pixels", [](TestCase &t)
        {
            auto source = std::make_shared<MemorySource>();
            const auto plain = makeBitmap([](int x, int y) { return (x >= 12 && x < 16 && y >= 12 && y < 16) ? 15 : 0; });
            const auto bold = makeBitmap([](int x, int y) { return (x >= 9 && x < 19 && y >= 9 && y < 19) ? 15 : 0; });
            FontHD::Glyphs().Clear();
            FontHD::Glyphs().Note(0x41, false, plain.data());
            FontHD::Glyphs().Note(0x41, true, bold.data());
            std::vector<uint8_t> hd(32 * 32, 0);
            hd[16 * 32 + 16] = 255;
            source->addCoverage("c41.png", 32, hd);
            FontHD::Atlas atlas(source);
            std::vector<uint8_t> canvas(512 * 512, 0);
            putCell(canvas, 0, 0, bold);
            const std::vector<uint8_t> *pixels = nullptr;
            t.IsTrue(atlas.Compose(canvas.data(), pixels) && pixels, "composed at 1x");
            if (!pixels)
                return;
            auto at = [&](int x, int y) { return (*pixels)[static_cast<size_t>(y) * 512 + x]; };
            t.Equals(static_cast<int>(at(16, 16)), 255, "the dot");
            t.Equals(static_cast<int>(at(13, 19)), 255, "three pixels away is inside the grown dot (a 7 x 7 square)");
            t.Equals(static_cast<int>(at(12, 16)), 0, "four pixels away is not");
            std::vector<uint8_t> direct;
            t.IsTrue(atlas.GlyphPicture(0x41, true, direct) && direct.size() == 1024u && direct[16 * 32 + 13] == 255, "GlyphPicture gives the same grown picture");
        });

        tc.Run("pictures of different scales: the atlas takes the largest and resizes the rest; a bad size is refused with a reason", [](TestCase &t)
        {
            auto source = std::make_shared<MemorySource>();
            std::vector<uint8_t> flat32(32 * 32, 128), flat128(128 * 128, 255), bad(48 * 48, 255);
            source->addCoverage("c41.png", 32, flat32);
            source->addCoverage("c42.png", 128, flat128);
            source->addCoverage("c43.png", 48, bad);
            source->addCoverage("u0044.png", 32, flat32, "mod.b", 1);
            source->addCoverage("c44.png", 128, flat128, "mod.b", 1); // same mod, same glyph: the code name wins over the character name
            FontHD::Atlas atlas(source);
            t.Equals(atlas.scale(), 4u, "the largest scale");
            t.Equals(atlas.stats().files, 4u, "four glyphs have a usable picture (0x41, 0x42, 0x44, and the full-width D 0x8263 through u0044)");
            t.Equals(atlas.stats().refused, 1u, "one refused");
            t.IsTrue(!source->problems.empty() && source->problems[0].find("48x48") != std::string::npos, "the reason names the size");
            std::vector<uint8_t> small;
            t.IsTrue(atlas.GlyphPicture(0x41, false, small) && small.size() == 128u * 128u && small[64 * 128 + 64] == 128, "the 1x picture is resized to 4x");
            std::vector<uint8_t> chosen;
            t.IsTrue(atlas.GlyphPicture(0x44, false, chosen) && chosen[64 * 128 + 64] == 255, "c44 beats u0044 inside one mod");
        });

        tc.Run("a later mod wins; a file for a code the font has no slot for is refused", [](TestCase &t)
        {
            auto source = std::make_shared<MemorySource>();
            std::vector<uint8_t> low(32 * 32, 50), high(32 * 32, 200);
            source->addCoverage("c41.png", 32, low, "mod.a", 0);
            source->addCoverage("u0041.png", 32, high, "mod.b", 1);
            source->addCoverage("c8000.png", 32, high, "mod.a", 0);
            FontHD::Atlas atlas(source);
            std::vector<uint8_t> a;
            t.IsTrue(atlas.GlyphPicture(0x41, false, a) && a[16 * 32 + 16] == 200, "mod.b's u0041 beats mod.a's c41");
            t.IsTrue(!source->problems.empty() && source->problems[0].find("c8000.png") != std::string::npos, "c8000 is refused: no slot");
            t.Equals(atlas.stats().files, 2u, "files counts the glyphs mods provide (u0041 stands for 0x41 and the full-width A 0x8260)");
        });

        tc.Run("an ASCII character name also covers its full-width form, unless the full-width one is named", [](TestCase &t)
        {
            auto source = std::make_shared<MemorySource>();
            std::vector<uint8_t> ascii(32 * 32, 60), wide(32 * 32, 220);
            source->addCoverage("u0041.png", 32, ascii);
            source->addCoverage("u0042.png", 32, ascii);
            source->addCoverage("uff22.png", 32, wide); // the full-width B named by its own character
            FontHD::Atlas atlas(source);
            std::vector<uint8_t> p;
            t.IsTrue(atlas.GlyphPicture(0x8260, false, p) && p[16 * 32 + 16] == 60, "u0041 covers the full-width A (0x8260)");
            t.IsTrue(atlas.GlyphPicture(0x41, false, p) && p[16 * 32 + 16] == 60, "and the ASCII A");
            t.IsTrue(atlas.GlyphPicture(0x8261, false, p) && p[16 * 32 + 16] == 220, "uff22 beats u0042 for the full-width B");
            t.IsTrue(atlas.GlyphPicture(0x42, false, p) && p[16 * 32 + 16] == 60, "u0042 still covers the ASCII B");
        });

        tc.Run("reload reads the files again and the generation changes", [](TestCase &t)
        {
            auto source = std::make_shared<MemorySource>();
            std::vector<uint8_t> grey(32 * 32, 90);
            source->addCoverage("c41.png", 32, grey);
            FontHD::Atlas atlas(source);
            const uint64_t g0 = atlas.generation();
            source->addCoverage("c42.png", 64, std::vector<uint8_t>(64 * 64, 90));
            t.Equals(atlas.reload(), 2u, "two files after the reload");
            t.IsTrue(atlas.generation() != g0, "the generation changed");
            t.Equals(atlas.scale(), 2u, "the scale grew");
        });

        tc.Run("pictures helpers: coverage of alpha or brightness, enlarging, growing", [](TestCase &t)
        {
            // alpha is the coverage when the picture is not opaque
            const uint8_t rgba1[8] = {255, 255, 255, 0, 10, 20, 30, 255};
            std::vector<uint8_t> cov;
            FontHD::CoverageOfRgba(rgba1, 2, cov);
            t.IsTrue(cov.size() == 2 && cov[0] == 0 && cov[1] == 255, "alpha when any pixel is transparent");
            const uint8_t rgba2[8] = {255, 255, 255, 255, 0, 0, 0, 255};
            FontHD::CoverageOfRgba(rgba2, 2, cov);
            t.IsTrue(cov[0] == 255 && cov[1] == 0, "brightness when opaque");
            // enlarging a flat cell of level 15 gives 255 everywhere, level 0 gives 0
            std::vector<uint8_t> flat(32 * 32, 15), big;
            FontHD::EnlargeOriginal(flat.data(), 32, 3, big);
            t.IsTrue(big.size() == 96u * 96u && *std::min_element(big.begin(), big.end()) == 255, "flat 15 stays 255");
            // growing a single pixel by radius 2 gives a 5 x 5 square
            std::vector<uint8_t> dot(16 * 16, 0), grown;
            dot[8 * 16 + 8] = 77;
            FontHD::Dilate(dot, 16, 2, grown);
            int count = 0;
            for (uint8_t v : grown)
                count += v == 77;
            t.Equals(count, 25, "5 x 5 pixels");
            // resizing by the same size is the identity, by 2 keeps a flat picture flat
            std::vector<uint8_t> out;
            FontHD::ResizeSquare(dot, 16, 16, out);
            t.IsTrue(out == dot, "identity");
            std::vector<uint8_t> grey(8 * 8, 100);
            FontHD::ResizeSquare(grey, 8, 16, out);
            t.IsTrue(out.size() == 256u && *std::min_element(out.begin(), out.end()) == 100 && *std::max_element(out.begin(), out.end()) == 100, "flat stays flat");
        });
    });
}
