#include "MiniTest.h"

#include "data_pack.h"
#include "fpr_save.h"
#include "mini_json.h"
#include "mod_manager.h"
#include "mod_package.h"
#include "profile.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// Tests for data packs (src/lib/mods: mini_json, fpr_save, data_pack, profile): the JSON reader, the save layout and integrity code,
// the rename file's parser and its refusals, applying a rename file to a save image, and the profile card with its ledger (add,
// later wins, remove restores, a change by the player is kept, a new version of a pack, crash-safe writes, Keep this card).
//
// Nothing here contains game data. The saves are synthetic images built in memory with a made-up checksum table. The last suite
// compares the C++ result with tools/fprsave.py on the player's own files under baserom/ and says "skipped" when they are not there.

namespace
{
    namespace fs = std::filesystem;
    using namespace ps2x::mods;
    using namespace ps2x::mods::fpr;

    // ---- helpers --------------------------------------------------------------------------------------------------------

    fs::path tempDir(const char *name)
    {
        fs::path dir = fs::temp_directory_path() / (std::string("ps2x_datapack_test_") + name);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        return dir;
    }

    Bytes readAll(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        const std::string s = out.str();
        return Bytes(s.begin(), s.end());
    }

    std::string readText(const fs::path &path)
    {
        const Bytes b = readAll(path);
        return std::string(b.begin(), b.end());
    }

    void writeAll(const fs::path &path, const Bytes &bytes)
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    bool contains(const std::string &haystack, const std::string &needle) { return haystack.find(needle) != std::string::npos; }
    bool oneLine(const std::string &text) { return text.find('\n') == std::string::npos && text.find('\r') == std::string::npos; }

    // A made-up checksum table (the standard CRC-32 one, MSB first): the algorithm is the game's, the table is not.
    const CrcTable &testTable()
    {
        static const CrcTable table = []() {
            CrcTable t{};
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i << 24;
                for (int k = 0; k < 8; ++k)
                    c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
                t[i] = c;
            }
            return t;
        }();
        return table;
    }

    Bytes textBytes(const std::string &utf8, uint32_t width)
    {
        std::vector<uint32_t> points;
        decodeUtf8(utf8, points);
        Bytes out;
        encodeGameText(points, out);
        out.resize(width, 0);
        return out;
    }

    void put(Bytes &image, size_t offset, const Bytes &bytes) { std::copy(bytes.begin(), bytes.end(), image.begin() + static_cast<std::ptrdiff_t>(offset)); }
    void putU16(Bytes &image, size_t offset, int v)
    {
        image[offset] = static_cast<uint8_t>(v & 0xFF);
        image[offset + 1] = static_cast<uint8_t>(v >> 8);
    }
    void putU32(Bytes &image, size_t offset, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            image[offset + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
    }

    // A small valid save: 10 wrestlers with names, 4 organisations and 6 groups in use, identity list orders, and a few other bytes
    // set so a change that spills over would show.
    Bytes makeSave()
    {
        Bytes img(kSaveSize, 0);
        putU32(img, kSettingsMarkerOff, kSettingsMarker);
        putU32(img, kRosterMarkerOff, kRosterMarker);
        for (int k = 0; k < kWrestlers; ++k)
        {
            const uint32_t base = kOffNames + static_cast<uint32_t>(k) * kNameStride;
            img[base] = static_cast<uint8_t>(k % 4 == 0 ? 0x80 : 0x00);
            put(img, base + 1, textBytes("First" + std::to_string(k), kWidthFirst));
            put(img, base + 15, textBytes("Last" + std::to_string(k), kWidthLast));
            put(img, base + 33, textBytes("Nick" + std::to_string(k), kWidthNickname));
            putU16(img, kRoster + 0x4748 + 2 * static_cast<size_t>(k), k % 6);
        }
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t base = kOffOrgs + static_cast<uint32_t>(i) * kOrgStride;
            putU16(img, base, 1);
            put(img, base + 2, textBytes("Org" + std::to_string(i), kWidthOrgLong));
            put(img, base + 28, textBytes("O" + std::to_string(i), kWidthOrgShort));
            putU16(img, base + 42, i);
        }
        for (int i = 0; i < kOrgs; ++i)
            img[kOffOrgOrder + static_cast<size_t>(i)] = static_cast<uint8_t>(i);
        for (int i = 0; i < 6; ++i)
        {
            const uint32_t base = kOffGroups + static_cast<uint32_t>(i) * kGroupStride;
            putU16(img, base, 1);
            put(img, base + 2, textBytes("Group" + std::to_string(i), kWidthOrgLong));
            put(img, base + 28, textBytes("G" + std::to_string(i), kWidthOrgShort));
            putU16(img, base + 42, i % 4);
            putU16(img, base + 44, i % 3);
        }
        for (int i = 0; i < kGroups; ++i)
            img[kOffGroupOrder + static_cast<size_t>(i)] = static_cast<uint8_t>(i);
        for (int k = 0; k < kWrestlers; ++k)
            putU16(img, kOffAppearance + static_cast<size_t>(k) * kAppearanceStride, 0xFFFF);
        // some bytes outside everything a rename pack owns
        img[0] = 0x05;
        img[0x1234] = 0x77;
        putU32(img, 0x3F398, 0x00010001);
        fixIntegrity(img, testTable());
        return img;
    }

    std::string renamesDoc(const std::string &body, const std::string &extra = std::string())
    {
        return "{\"format\": \"fpr-rename\", \"version\": 1" + extra + body + "}";
    }

    // The rename file for "wrestlers": {"<slot>": first-name, ...}, a quick form.
    std::string renameFirst(const std::map<int, std::string> &names)
    {
        std::string body = ", \"wrestlers\": {";
        bool first = true;
        for (const auto &n : names)
        {
            body += std::string(first ? "" : ", ") + "\"" + std::to_string(n.first) + "\": {\"first\": \"" + n.second + "\"}";
            first = false;
        }
        return renamesDoc(body + "}");
    }

    RenamePack mustParse(TestCase &t, const std::string &text, const std::string &what)
    {
        RenamePack pack;
        std::string error;
        t.IsTrue(parseRenamePack(text, pack, &error), what + ": " + error);
        return pack;
    }

    Bytes applyToImage(TestCase &t, const Bytes &base, const std::string &text, const std::string &what)
    {
        const RenamePack pack = mustParse(t, text, what);
        ApplyOutcome out;
        t.IsTrue(applyRenamePack(pack, base, nullptr, out), what + ": " + out.error);
        Bytes image = base;
        applyWrites(image, out.writes);
        fixIntegrity(image, testTable());
        return image;
    }

    std::string applyError(const Bytes &base, const std::string &text)
    {
        RenamePack pack;
        std::string error;
        if (!parseRenamePack(text, pack, &error))
            return "PARSE: " + error;
        ApplyOutcome out;
        if (applyRenamePack(pack, base, nullptr, out))
            return std::string();
        return out.error;
    }

    std::string firstName(const Bytes &image, int slot)
    {
        const size_t o = kOffNames + static_cast<size_t>(slot) * kNameStride + 1;
        Bytes field(image.begin() + static_cast<std::ptrdiff_t>(o), image.begin() + static_cast<std::ptrdiff_t>(o + kWidthFirst));
        return std::string(field.begin(), field.end());
    }

    bool sameFirst(const Bytes &image, int slot, const std::string &utf8)
    {
        const Bytes want = textBytes(utf8, kWidthFirst);
        return firstName(image, slot) == std::string(want.begin(), want.end());
    }

    profile::PackInput packInput(const std::string &id, const std::string &version, const std::string &json)
    {
        profile::PackInput p;
        p.id = id;
        p.name = id;
        p.version = version;
        p.hasFile = true;
        p.renamesJson = json;
        return p;
    }

    // A base card and a profile folder in a temporary place.
    struct Cards
    {
        fs::path root, base0, base1, profile;
        explicit Cards(const char *name, const Bytes *save)
        {
            root = tempDir(name);
            base0 = root / "mc0";
            base1 = root / "mc1";
            profile = root / "profile";
            std::error_code ec;
            fs::create_directories(base0, ec);
            fs::create_directories(base1, ec);
            if (save)
            {
                writeAll(base0 / kSaveName / kSaveName, *save);
                writeAll(base0 / kSaveName / "icon.sys", Bytes{1, 2, 3, 4});
                writeAll(base0 / kSaveName / "view.ico", Bytes{9, 8, 7});
            }
        }
        fs::path profileSave() const { return profile / "mc0" / kSaveName / kSaveName; }
        fs::path ledger() const { return profile / "ledger.json"; }
        profile::Config config(std::vector<profile::PackInput> packs) const
        {
            profile::Config c;
            c.profileDir = profile;
            c.baseCard0 = base0;
            c.baseCard1 = base1;
            c.packs = std::move(packs);
            c.crcTable = &testTable();
            return c;
        }
    };

    // Player edits in the game: change bytes in the profile save and fix its checksums (the game writes a valid save).
    void playerEdit(const fs::path &save, const std::function<void(Bytes &)> &edit)
    {
        Bytes image = readAll(save);
        edit(image);
        fixIntegrity(image, testTable());
        writeAll(save, image);
    }

    // ---- real data: where the player's files are, for the byte-identity test --------------------------------------------

    fs::path findRepoRoot()
    {
        std::error_code ec;
        fs::path dir = fs::current_path(ec);
        for (int i = 0; i < 8 && !dir.empty(); ++i)
        {
            if (fs::exists(dir / "tools" / "fprsave.py", ec) && fs::exists(dir / "baserom" / "saves" / "001_baseline", ec) &&
                fs::exists(dir / "baserom" / "iso" / "SLUS_217.02", ec))
                return dir;
            if (!dir.has_parent_path() || dir.parent_path() == dir)
                break;
            dir = dir.parent_path();
        }
        return fs::path();
    }

    bool runCommand(const std::string &command)
    {
#if defined(_WIN32)
        const std::string line = "\"" + command + " > NUL 2>&1\"";
#else
        const std::string line = command + " > /dev/null 2>&1";
#endif
        return std::system(line.c_str()) == 0;
    }

    std::string q(const fs::path &p) { return "\"" + p.string() + "\""; }
}

void register_data_pack_tests()
{
    // ---------------------------------------------------------------------------------------------------------- JSON
    MiniTest::Case("DataPackJson", [](TestCase &tc)
    {
        tc.Run("values, escapes and a round trip", [](TestCase &t)
        {
            json::Value v;
            std::string error;
            t.IsTrue(json::parse(" {\"a\": [1, -2, 3.5, true, false, null, \"x\\n\\u00e9\\ud83d\\ude00\"], \"b\": {\"c\": {}}, \"d\": []} ", v, &error), error);
            t.IsTrue(v.isObject() && v.find("a") && v.find("a")->items.size() == 7, "shape");
            const json::Value &a = *v.find("a");
            t.IsTrue(a.items[0].isInt() && a.items[0].integer == 1 && a.items[1].integer == -2, "integers stay exact");
            t.IsTrue(a.items[2].type == json::Value::Type::Double && a.items[2].number == 3.5, "a fraction is a double");
            t.IsTrue(a.items[3].isBool() && a.items[3].boolean && a.items[4].isBool() && !a.items[4].boolean && a.items[5].isNull(), "literals");
            t.Equals(a.items[6].text, std::string("x\n\xC3\xA9\xF0\x9F\x98\x80"), "escapes and a surrogate pair become UTF-8");
            json::Value big;
            t.IsTrue(json::parse("[9223372036854775807, -9223372036854775808, 9223372036854775808]", big, &error), error);
            t.IsTrue(big.items[0].isInt() && big.items[0].integer == INT64_MAX && big.items[1].isInt() && big.items[1].integer == INT64_MIN, "64-bit ends are exact");
            t.IsTrue(big.items[2].type == json::Value::Type::Double, "one past is a double");
            const std::string text = json::dump(v, false);
            json::Value again;
            t.IsTrue(json::parse(text, again, &error), "dump parses: " + error);
            t.Equals(json::dump(again, false), text, "dump is stable");
            json::Value pretty;
            t.IsTrue(json::parse(json::dump(v, true), pretty, &error), "pretty dump parses: " + error);
            t.Equals(json::dump(pretty, false), text, "pretty and compact agree");
            t.Equals(json::quote("a\"b\\c\n\x01"), std::string("\"a\\\"b\\\\c\\n\\u0001\""), "quote escapes");
        });

        tc.Run("every kind of bad document is refused with one line naming the place", [](TestCase &t)
        {
            struct Bad
            {
                const char *text;
                const char *expect;
            };
            const Bad bad[] = {
                {"", "line 1, column 1: the file ends where a value was expected"},
                {"{\"a\": 1,}", "line 1, column 9: a member name in quotes was expected"},
                {"{a: 1}", "line 1, column 2: a member name in quotes was expected"},
                {"{\"a\": 1, \"a\": 2}", "the name \"a\" is used twice in one object"},
                {"[1, 2,]", "this is not a value"},
                {"[1 2]", "a ',' or ']' was expected"},
                {"{\"a\": 1} x", "there is more text after the end of the document"},
                {"\"abc", "never ends"},
                {"\"a\\qb\"", "'\\q' is not a JSON escape"},
                {"\"\\ud800\"", "high surrogate"},
                {"\"\\udc00\"", "low surrogate"},
                {"\"\\u12\"", "four hexadecimal digits"},
                {"\"a\nb\"", "control character"},
                {"07", "cannot start with 0"},
                {"1.", "digits after the point"},
                {"-", "not complete"},
                {"nul", "this is not a value"},
                {"\xEF\xBB\xBF{}", "byte order mark"},
                {"{\n  \"a\": tru\n}", "line 2, column 8"},
            };
            for (const Bad &b : bad)
            {
                json::Value v;
                std::string error;
                t.IsFalse(json::parse(b.text, v, &error), std::string("refused: ") + b.text);
                t.IsTrue(contains(error, b.expect) && oneLine(error), std::string("message for '") + b.text + "': " + error);
                t.IsTrue(v.isNull(), "nothing is left in the output");
            }
            std::string deep(80, '[');
            deep += std::string(80, ']');
            json::Value v;
            std::string error;
            t.IsFalse(json::parse(deep, v, &error), "80 levels deep is refused");
            t.IsTrue(contains(error, "deeper than 64"), error);
        });
    });

    // ------------------------------------------------------------------------------------------------- save layout, text
    MiniTest::Case("DataPackSave", [](TestCase &tc)
    {
        tc.Run("SHA-256 vectors", [](TestCase &t)
        {
            t.Equals(sha256Hex(nullptr, 0), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "empty");
            const std::string abc = "abc";
            t.Equals(sha256Hex(reinterpret_cast<const uint8_t *>(abc.data()), abc.size()), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "abc");
            const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; // 56 bytes: the padding needs a second block
            t.Equals(sha256Hex(reinterpret_cast<const uint8_t *>(two.data()), two.size()), std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "56 bytes");
            const std::string million(1000000, 'a');
            t.Equals(sha256Hex(reinterpret_cast<const uint8_t *>(million.data()), million.size()), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "a million a");
        });

        tc.Run("the checksum, parity and final value are the game's", [](TestCase &t)
        {
            const CrcTable &table = testTable();
            // bit by bit, the way the table was made
            auto slow = [](const uint8_t *p, size_t n) {
                uint32_t c = 0;
                for (size_t i = 0; i < n; ++i)
                {
                    c ^= static_cast<uint32_t>(p[i]) << 24;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
                }
                return c;
            };
            const uint8_t data[] = {1, 2, 3, 250, 0, 77};
            t.Equals(crc(data, sizeof(data), table), slow(data, sizeof(data)), "table crc equals the bitwise one");

            Bytes image = makeSave();
            Integrity in = checkIntegrity(image, table);
            t.IsTrue(in.allOk() && in.markersOk && in.blocksOk == kBlocks, "a built save verifies");
            Bytes edited = image;
            edited[0x5000] ^= 0x5A;
            in = checkIntegrity(edited, table);
            t.IsTrue(!in.allOk() && in.badBlocks.size() == 1 && in.badBlocks[0] == 0x5000 / kBlockSize, "one flipped byte flags exactly its block");
            fixIntegrity(edited, table);
            t.IsTrue(checkIntegrity(edited, table).allOk() && edited[0x5000] == (image[0x5000] ^ 0x5A), "fix repairs it and keeps the edit");
            Bytes again = edited;
            fixIntegrity(again, table);
            t.IsTrue(again == edited, "fix on a fixed image changes nothing");
            Bytes parityBad = image;
            parityBad[kParityOff + 7] ^= 1;
            t.IsFalse(checkIntegrity(parityBad, table).parityOk, "a wrong parity byte is seen");
            Bytes shortImage(100, 0);
            t.IsFalse(checkIntegrity(shortImage, table).sizeOk, "a wrong size is seen");
            Bytes noMarker = image;
            noMarker[kRosterMarkerOff] ^= 1;
            fixIntegrity(noMarker, table);
            t.IsTrue(checkIntegrity(noMarker, table).allOk() && !checkIntegrity(noMarker, table).markersOk, "a changed layout marker is reported apart from integrity");
        });

        tc.Run("the checksum table is read from an executable and checked", [](TestCase &t)
        {
            const fs::path dir = tempDir("elf");
            // a minimal ELF32 little endian with one loadable segment covering the table's address, holding zeros
            Bytes elf(0x100 + 0x400, 0);
            const uint8_t magic[] = {0x7F, 'E', 'L', 'F', 1, 1, 1};
            std::copy(std::begin(magic), std::end(magic), elf.begin());
            putU32(elf, 28, 52);                       // e_phoff
            elf[42] = 32;                              // e_phentsize
            elf[44] = 1;                               // e_phnum
            putU32(elf, 52 + 0, 1);                    // PT_LOAD
            putU32(elf, 52 + 4, 0x100);                // file offset
            putU32(elf, 52 + 8, 0x32E800);             // vaddr
            putU32(elf, 52 + 16, 0x400 + 0xD0);        // filesz
            elf.resize(0x100 + 0x400 + 0xD0, 0);
            writeAll(dir / "fake.elf", elf);
            CrcTable table{};
            std::string error;
            t.IsFalse(crcTableFromElf(dir / "fake.elf", table, &error), "a table with the wrong fingerprint is refused");
            t.IsTrue(contains(error, "not the NTSC-U v1.01 executable"), error);
            writeAll(dir / "text.txt", Bytes{'h', 'i'});
            t.IsFalse(crcTableFromElf(dir / "text.txt", table, &error) || !contains(error, "ELF"), "a text file is not an ELF: " + error);
            t.IsFalse(crcTableFromElf(dir / "nothing.elf", table, &error) || !contains(error, "cannot be opened"), "a missing file: " + error);
            Bytes small = elf;
            small.resize(0x100 + 0x100);
            putU32(small, 52 + 16, 0x100);
            writeAll(dir / "small.elf", small);
            t.IsFalse(crcTableFromElf(dir / "small.elf", table, &error), "a segment that stops before the table is refused");
            t.IsTrue(contains(error, "not inside a loadable segment"), error);
            t.IsFalse(crcTableFingerprintOk(testTable()), "the made-up test table is not the game's");
            const fs::path root = findRepoRoot();
            if (!root.empty())
            {
                CrcTable real{};
                t.IsTrue(crcTableFromElf(root / "baserom" / "iso" / "SLUS_217.02", real, &error), "the player's own executable: " + error);
                t.IsTrue(crcTableFingerprintOk(real), "its table has the known fingerprint");
            }
            else
            {
                std::printf("(skipped the real executable: no baserom/) ");
            }
        });

        tc.Run("text becomes the game's Shift-JIS, full width", [](TestCase &t)
        {
            auto encode = [&](const std::string &utf8, std::string *error = nullptr) {
                std::vector<uint32_t> points;
                Bytes out;
                if (!decodeUtf8(utf8, points, error) || !encodeGameText(points, out, error))
                    return std::string("FAIL");
                std::string hex;
                char b[4];
                for (const uint8_t c : out)
                {
                    std::snprintf(b, sizeof(b), "%02X", c);
                    hex += b;
                }
                return hex;
            };
            t.Equals(encode("A"), std::string("8260"), "A becomes the full-width A");
            t.Equals(encode("Az 09"), std::string("8260") + "829A" + "8140" + "824F" + "8258", "letters, a space (U+3000) and digits");
            t.Equals(encode("~!"), std::string("8160") + "8149", "punctuation folds too");
            t.Equals(encode("\xE3\x81\x82"), std::string("82A0"), "hiragana a");
            t.Equals(encode("\xE6\xBC\xA2"), std::string("8ABF"), "a kanji");
            t.Equals(encode("\xE3\x83\xBB"), std::string("8145"), "the middle dot");
            t.Equals(encode("\xEF\xBD\xB1"), std::string("B1"), "half-width katakana is one byte");
            t.Equals(encode("\xEF\xBD\x9E"), std::string("8160"), "fullwidth tilde");
            t.Equals(encode(""), std::string(""), "empty");
            std::string error;
            t.Equals(encode("\xF0\x9F\x98\x80", &error), std::string("FAIL"), "an emoji has no code");
            t.IsTrue(contains(error, "U+1F600") && contains(error, "Shift-JIS"), error);
            t.Equals(encode("\xC3\x28", &error), std::string("FAIL"), "bad UTF-8");
            t.IsTrue(contains(error, "UTF-8"), error);
            t.Equals(encode("\xED\xA0\x80", &error), std::string("FAIL"), "a surrogate encoded in UTF-8 is refused");
            t.Equals(encode("\xC0\x80", &error), std::string("FAIL"), "an overlong form is refused");
        });

        tc.Run("fields: ranges, masks and descriptions", [](TestCase &t)
        {
            FieldRange r;
            t.IsTrue(fieldRange("w288.first", r) && r.offset == 0x19BD7 && r.length == 14 && r.mask == 0xFF, "slot 288 FIRST is at 0x19BD7 as documented");
            t.IsTrue(fieldRange("w288.last", r) && r.offset == 0x19BE5 && r.length == 18, "LAST at 0x19BE5");
            t.IsTrue(fieldRange("w288.flip", r) && r.offset == 0x19BD6 && r.length == 1 && r.mask == 0x80, "Flip is bit 7 of 0x19BD6");
            t.IsTrue(fieldRange("w288.space", r) && r.offset == 0x19BD6 && r.mask == 0x03, "Space is bits 0-1");
            t.IsTrue(fieldRange("o3.name", r) && r.offset == kOffOrgs + 3 * 44 + 2 && r.length == 26, "organisation name");
            t.IsTrue(fieldRange("g99.kind", r) && r.offset == kOffGroups + 99 * 46 + 44 && r.length == 2, "group kind");
            t.IsTrue(fieldRange("o.order", r) && r.length == 50 && fieldRange("g.order", r) && r.length == 100, "order arrays");
            t.IsTrue(fieldRange("a326", r) && r.length == 448, "appearance");
            for (const char *bad : {"", "w", "w327.first", "w1.nope", "o50.name", "g100.name", "o1.org", "g1.id", "x1.first", "w1", "a1.x", "w-1.first", "w1.first.x", "o.name"})
                t.IsFalse(fieldRange(bad, r), std::string("not a field: '") + bad + "'");
            t.Equals(describeField("w12.first"), std::string("wrestler 12's FIRST name"), "description");
            t.Equals(describeField("g5.short"), std::string("group 5's short name"), "description of a group field");
            t.Equals(describeField("o.order"), std::string("the organisation list order"), "description of an order");

            Bytes image(kSaveSize, 0);
            image[0x19BD6] = 0x5B;
            FieldRange flip, space, flags;
            fieldRange("w288.flip", flip);
            fieldRange("w288.space", space);
            fieldRange("w288.flags", flags);
            writeField(image, flip, Bytes{0x80});
            t.Equals(static_cast<int>(image[0x19BD6]), 0xDB, "writing Flip changes bit 7 only");
            writeField(image, space, Bytes{0x02});
            t.Equals(static_cast<int>(image[0x19BD6]), 0xDA, "writing Space changes bits 0-1 only");
            writeField(image, flags, Bytes{0x00});
            t.Equals(static_cast<int>(image[0x19BD6]), 0x82, "writing the flag bits changes bits 2-6 only");
            t.Equals(static_cast<int>(readField(image, space)[0]), 2, "a masked read gives the field's bits");
        });
    });

    // ---------------------------------------------------------------------------------------------------- rename file
    MiniTest::Case("DataPackRenames", [](TestCase &tc)
    {
        tc.Run("a full rename file parses", [](TestCase &t)
        {
            const std::string text = renamesDoc(
                ", \"wrestlers\": {\"0\": {\"first\": \"Karl\", \"last\": \"Gotch\", \"nickname\": \"God\", \"flip\": false, \"space\": 3}, "
                "\"172\": {\"first\": {\"hex\": \"323030\"}, \"flags_raw\": 255}}, "
                "\"organisations\": {\"1\": {\"name\": \"NJPW\", \"short\": \"NJ\"}, \"7\": {\"name\": \"New\", \"short\": \"N\", \"created\": true}}, "
                "\"groups\": {\"2\": {\"name\": \"Stable\", \"short\": \"S\", \"org\": 1, \"kind\": 2}}, "
                "\"organisation_order\": [7, 0, 1, 2, 3], \"group_order\": [0, 1, 2], "
                "\"appearance\": {\"16\": \"" + std::string(896, 'a') + "\"}, \"_comment\": \"ignored\"",
                ", \"game\": \"SLUS-21702\", \"base_sha1\": \"00\", \"source\": {\"file\": \"x\"}");
            const RenamePack p = mustParse(t, text, "full file");
            t.IsTrue(p.wrestlers.size() == 2 && p.wrestlers[0].slot == 0 && p.wrestlers[1].slot == 172, "wrestlers, in slot order");
            t.IsTrue(p.wrestlers[0].first && p.wrestlers[0].first->size() == kWidthFirst && (*p.wrestlers[0].first)[0] == 0x82 && (*p.wrestlers[0].first)[1] == 0x6A, "FIRST is full-width K, padded to 14 bytes");
            t.IsTrue(p.wrestlers[0].flip && !*p.wrestlers[0].flip && p.wrestlers[0].space && *p.wrestlers[0].space == 3, "flip and space");
            t.IsTrue(p.wrestlers[1].first && (*p.wrestlers[1].first)[0] == '2' && (*p.wrestlers[1].first)[3] == 0 && p.wrestlers[1].flagsRaw && *p.wrestlers[1].flagsRaw == 255, "hex field and flags_raw");
            t.IsTrue(p.orgs.size() == 2 && p.orgs[1].created && p.groups.size() == 1 && p.groups[0].org && *p.groups[0].org == 1 && *p.groups[0].kind == 2, "records");
            t.IsTrue(p.orgOrder && p.orgOrder->size() == 5 && (*p.orgOrder)[0] == 7 && p.groupOrder && p.groupOrder->size() == 3, "order lists");
            t.IsTrue(p.appearance.size() == 1 && p.appearance[0].first == 16 && p.appearance[0].second.size() == 448 && p.appearance[0].second[0] == 0xAA, "appearance");
            t.Equals(p.entryCount(), static_cast<size_t>(2 + 2 + 1 + 1), "entry count");
            const RenamePack empty = mustParse(t, "{\"format\": \"fpr-rename\", \"version\": 1}", "empty file");
            t.Equals(empty.entryCount(), static_cast<size_t>(0), "an empty file is fine");
        });

        tc.Run("every mistake is refused with one line saying where", [](TestCase &t)
        {
            struct Bad
            {
                std::string text;
                const char *expect;
            };
            auto w = [](const std::string &entry) { return renamesDoc(", \"wrestlers\": {" + entry + "}"); };
            const std::vector<Bad> bad = {
                {"{", "this is not valid JSON"},
                {"[]", "the document must be an object"},
                {"{\"version\": 1}", "'format' must be \"fpr-rename\""},
                {"{\"format\": \"other\", \"version\": 1}", "'format' must be \"fpr-rename\""},
                {"{\"format\": \"fpr-rename\", \"version\": 2}", "'version' must be 1"},
                {"{\"format\": \"fpr-rename\", \"version\": true}", "'version' must be 1"},
                {renamesDoc(", \"wrestler\": {}"), "unknown key 'wrestler'"},
                {renamesDoc(", \"game\": 5"), "'game' must be text"},
                {renamesDoc(", \"wrestlers\": []"), "wrestlers: must be an object"},
                {w("\"327\": {}"), "wrestler slot 327 is out of range (0 to 326)"},
                {w("\"abc\": {}"), "wrestler slot 'abc' is not a number"},
                {w("\"-1\": {}"), "is not a number"},
                {w("\"7\": {}, \"07\": {}"), "slot 7 appears twice"},
                {w("\"1\": 5"), "wrestlers.1: must be an object"},
                {w("\"1\": {\"frist\": \"X\"}"), "wrestlers.1: unknown key 'frist'"},
                {w("\"1\": {\"first\": \"ABCDEFGH\"}"), "wrestlers.1.first: 'ABCDEFGH' has 8 characters; the FIRST field takes at most 7"},
                {w("\"1\": {\"last\": \"ABCDEFGHIJ\"}"), "the LAST field takes at most 9"},
                {w("\"1\": {\"nickname\": \"ABCDEFGHIJKLM\"}"), "the NICKNAME field takes at most 12"},
                {w("\"1\": {\"first\": 5}"), "wrestlers.1.first: must be text or {\"hex\""},
                {w("\"1\": {\"first\": \"\\ud83d\\ude00\"}"), "U+1F600"},
                {w("\"1\": {\"first\": {\"hex\": \"abc\"}}"), "not an even number of hexadecimal digits"},
                {w("\"1\": {\"first\": {\"hex\": \"zz\"}}"), "not an even number of hexadecimal digits"},
                {w("\"1\": {\"first\": {\"hex\": \"" + std::string(30, 'a') + "\"}}"), "the hex value is 15 bytes; the FIRST field holds 14"},
                {w("\"1\": {\"first\": {\"hex\": \"aa\", \"x\": 1}}"), "must be {\"hex\""},
                {w("\"1\": {\"flip\": 1}"), "wrestlers.1.flip: must be true or false"},
                {w("\"1\": {\"space\": 4}"), "wrestlers.1.space: 4 is out of range (0 to 3)"},
                {w("\"1\": {\"space\": 1.5}"), "must be a whole number"},
                {w("\"1\": {\"flags_raw\": 256}"), "wrestlers.1.flags_raw: 256 is out of range"},
                {renamesDoc(", \"organisations\": {\"50\": {}}"), "organisations organisation 50 is out of range (0 to 49)"},
                {renamesDoc(", \"organisations\": {\"1\": {\"org\": 1}}"), "organisations.1: unknown key 'org'"},
                {renamesDoc(", \"organisations\": {\"1\": {\"name\": \"ABCDEFGHIJKLM\"}}"), "the name field takes at most 12"},
                {renamesDoc(", \"organisations\": {\"1\": {\"short\": \"ABCDEFG\"}}"), "the short name field takes at most 6"},
                {renamesDoc(", \"groups\": {\"100\": {}}"), "groups group 100 is out of range (0 to 99)"},
                {renamesDoc(", \"groups\": {\"1\": {\"org\": 50}}"), "groups.1.org: 50 is out of range (0 to 49)"},
                {renamesDoc(", \"groups\": {\"1\": {\"kind\": 3}}"), "groups.1.kind: 3 is out of range (0 to 2)"},
                {renamesDoc(", \"groups\": {\"1\": {\"created\": \"yes\"}}"), "groups.1.created: must be true or false"},
                {renamesDoc(", \"organisation_order\": 5"), "organisation_order: must be a list"},
                {renamesDoc(", \"organisation_order\": [1, 1]"), "organisation_order: 1 is listed twice"},
                {renamesDoc(", \"group_order\": [100]"), "group_order[0]: 100 is out of range"},
                {renamesDoc(", \"appearance\": {\"1\": \"abcd\"}"), "the record must be 448 bytes, not 2"},
                {renamesDoc(", \"appearance\": {\"1\": 5}"), "must be the record as hexadecimal text"},
                {renamesDoc(", \"appearance\": {\"327\": \"\"}"), "appearance slot 327 is out of range"},
            };
            for (const Bad &b : bad)
            {
                RenamePack pack;
                std::string error;
                t.IsFalse(parseRenamePack(b.text, pack, &error), "refused: " + b.text.substr(0, 90));
                t.IsTrue(contains(error, b.expect), "message for " + b.text.substr(0, 90) + " -> " + error + " (wanted: " + b.expect + ")");
                t.IsTrue(oneLine(error) && !error.empty(), "one line: " + error);
                t.IsTrue(pack.entryCount() == 0, "nothing is left in the output");
            }
        });

        tc.Run("applying: wrestlers, flags, an existing organisation and group", [](TestCase &t)
        {
            const Bytes base = makeSave();
            const std::string doc = renamesDoc(
                ", \"wrestlers\": {\"1\": {\"first\": \"Karl\", \"last\": \"Gotch\", \"nickname\": \"God\", \"flip\": true, \"space\": 1}, "
                "\"4\": {\"flip\": false}, \"5\": {\"space\": 2}}, "
                "\"organisations\": {\"1\": {\"name\": \"NJPW Pro\", \"short\": \"NJ\"}}, "
                "\"groups\": {\"3\": {\"kind\": 2, \"org\": 2}}");
            const Bytes out = applyToImage(t, base, doc, "apply");
            t.IsTrue(checkIntegrity(out, testTable()).allOk(), "the result verifies");
            t.IsTrue(sameFirst(out, 1, "Karl"), "FIRST written");
            const size_t n1 = kOffNames + 1 * kNameStride;
            const Bytes last = textBytes("Gotch", kWidthLast), nick = textBytes("God", kWidthNickname);
            t.IsTrue(std::equal(last.begin(), last.end(), out.begin() + static_cast<std::ptrdiff_t>(n1 + 15)) &&
                         std::equal(nick.begin(), nick.end(), out.begin() + static_cast<std::ptrdiff_t>(n1 + 33)), "LAST and NICKNAME written");
            t.Equals(static_cast<int>(out[n1]), 0x81, "flip ON (bit 7) and space 1 on a record whose flags were 0");
            t.Equals(static_cast<int>(out[kOffNames + 4 * kNameStride]), 0x00, "flip OFF cleared bit 7 of slot 4 (it was 0x80)");
            t.Equals(static_cast<int>(out[kOffNames + 5 * kNameStride]), static_cast<int>(base[kOffNames + 5 * kNameStride] & 0xFC) | 2, "space alone keeps the other bits");
            const Bytes orgName = textBytes("NJPW Pro", kWidthOrgLong);
            t.IsTrue(std::equal(orgName.begin(), orgName.end(), out.begin() + static_cast<std::ptrdiff_t>(kOffOrgs + 1 * kOrgStride + 2)), "organisation name");
            t.Equals(static_cast<int>(readU16(out, kOffGroups + 3 * kGroupStride + 42)), 2, "group organisation");
            t.Equals(static_cast<int>(readU16(out, kOffGroups + 3 * kGroupStride + 44)), 2, "group kind");
            // every byte outside the named fields and the integrity data is unchanged
            size_t differing = 0;
            for (size_t i = 0; i < kDataEnd; ++i)
                differing += out[i] != base[i];
            t.IsTrue(differing > 0 && differing < 120, "only the named fields moved: " + std::to_string(differing) + " bytes");
            t.IsTrue(out[0] == 0x05 && out[0x1234] == 0x77, "bytes of other regions are untouched");
            // nothing else changes: an empty file is the identity
            const Bytes same = applyToImage(t, base, "{\"format\": \"fpr-rename\", \"version\": 1}", "empty");
            t.IsTrue(same == base, "an empty rename file leaves the save byte for byte as it was");
        });

        tc.Run("applying: flags_raw means the whole flags byte, with flip and space on top", [](TestCase &t)
        {
            const Bytes base = makeSave();
            const size_t o = kOffNames + 2 * kNameStride;
            auto flagsAfter = [&](const std::string &entry) {
                const Bytes out = applyToImage(t, base, renamesDoc(", \"wrestlers\": {\"2\": {" + entry + "}}"), entry);
                return static_cast<int>(out[o]);
            };
            t.Equals(flagsAfter("\"flags_raw\": 253"), 253, "flags_raw alone sets the byte (253 = 0xFD)");
            t.Equals(flagsAfter("\"flags_raw\": 253, \"flip\": false"), 0x7D, "flip OFF overrides bit 7");
            t.Equals(flagsAfter("\"flags_raw\": 253, \"space\": 2"), 0xFE, "space overrides bits 0-1");
            t.Equals(flagsAfter("\"flags_raw\": 4, \"flip\": true, \"space\": 3"), 0x87, "all three");
        });

        tc.Run("applying: new organisations and groups, and the list orders", [](TestCase &t)
        {
            const Bytes base = makeSave();
            const std::string doc = renamesDoc(
                ", \"organisations\": {\"4\": {\"name\": \"Fresh Org\", \"short\": \"FO\", \"created\": true}}, "
                "\"groups\": {\"6\": {\"name\": \"Fresh Group\", \"short\": \"FG\", \"org\": 4, \"created\": true}, "
                "\"7\": {\"name\": \"Second\", \"short\": \"SG\", \"org\": 0, \"kind\": 0}}");
            const Bytes out = applyToImage(t, base, doc, "create");
            t.IsTrue(checkIntegrity(out, testTable()).allOk(), "verifies");
            const size_t o4 = kOffOrgs + 4 * kOrgStride;
            t.Equals(static_cast<int>(readU16(out, o4)), 1, "the organisation is in use");
            t.Equals(static_cast<int>(readU16(out, o4 + 42)), 0xFFFF, "an organisation made in the game carries id 0xFFFF");
            const size_t g6 = kOffGroups + 6 * kGroupStride;
            t.Equals(static_cast<int>(readU16(out, g6)), 1, "the group is in use");
            t.Equals(static_cast<int>(readU16(out, g6 + 42)), 4, "its organisation (a new one, valid because it is in use by then)");
            t.Equals(static_cast<int>(readU16(out, g6 + 44)), 1, "the game's default kind is Neutral");
            t.Equals(static_cast<int>(readU16(out, kOffGroups + 7 * kGroupStride + 44)), 0, "a given kind wins");
            // order: the old order of what is in use, then the new ones, then the rest in index order
            std::vector<int> orgOrder(out.begin() + kOffOrgOrder, out.begin() + kOffOrgOrder + kOrgs);
            t.IsTrue(orgOrder[0] == 0 && orgOrder[1] == 1 && orgOrder[2] == 2 && orgOrder[3] == 3 && orgOrder[4] == 4 && orgOrder[5] == 5, "new organisation goes last of the used ones");
            std::vector<int> groupOrder(out.begin() + kOffGroupOrder, out.begin() + kOffGroupOrder + 8);
            t.IsTrue(groupOrder == (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}), "new groups go last");
            // an explicit order
            const Bytes ordered = applyToImage(t, base, renamesDoc(", \"organisations\": {\"4\": {\"name\": \"N\", \"short\": \"N\"}}, \"organisation_order\": [4, 0, 1, 3, 2]"), "ordered");
            t.IsTrue(ordered[kOffOrgOrder] == 4 && ordered[kOffOrgOrder + 1] == 0 && ordered[kOffOrgOrder + 4] == 2 && ordered[kOffOrgOrder + 5] == 5 && ordered[kOffOrgOrder + 49] == 49,
                     "the given order, then the unused indices in order");
        });

        tc.Run("applying: what cannot be done is refused in one line", [](TestCase &t)
        {
            const Bytes base = makeSave();
            auto err = [&](const std::string &body) { return applyError(base, renamesDoc(body)); };
            std::string e = err(", \"organisations\": {\"10\": {\"name\": \"X\"}}");
            t.IsTrue(contains(e, "organisation 10 is not in this save, so the pack must give both a name and a short name") && oneLine(e), e);
            e = err(", \"groups\": {\"20\": {\"name\": \"X\", \"short\": \"X\"}}");
            t.IsTrue(contains(e, "group 20 is not in this save, so the pack must give its organisation (org)"), e);
            e = err(", \"groups\": {\"1\": {\"org\": 40}}");
            t.IsTrue(contains(e, "group 1: organisation 40 is not in use in this save"), e);
            e = err(", \"organisation_order\": [0, 1, 2]");
            t.IsTrue(contains(e, "organisation_order must list each organisation in use exactly once (in use: 0, 1, 2, 3)"), e);
            e = err(", \"group_order\": [0, 1, 2, 3, 4, 5, 6]");
            t.IsTrue(contains(e, "group_order must list each group in use exactly once"), e);
            const RenamePack pack = mustParse(t, renameFirst({{1, "A"}}), "ok");
            ApplyOutcome out;
            t.IsFalse(applyRenamePack(pack, Bytes(100, 0), nullptr, out) || out.error.empty(), "a save of the wrong size is refused");
            Bytes twice = base;
            twice[kOffOrgOrder + 1] = 0; // the base array lists 0 twice
            e = applyError(twice, renamesDoc(", \"organisations\": {\"4\": {\"name\": \"N\", \"short\": \"N\"}}"));
            t.IsTrue(contains(e, "lists 0 twice"), e);
        });

        tc.Run("a frozen field is not written, and later decisions see the old bytes", [](TestCase &t)
        {
            const Bytes base = makeSave();
            const RenamePack pack = mustParse(t, renameFirst({{1, "Aaa"}, {2, "Bbb"}}), "pack");
            std::set<std::string> frozen = {"w1.first"};
            ApplyOutcome out;
            t.IsTrue(applyRenamePack(pack, base, &frozen, out), out.error);
            t.IsTrue(out.writes.size() == 1 && out.writes[0].key == "w2.first", "only the other field is written");
            t.IsTrue(out.suppressed.size() == 1 && out.suppressed[0] == "w1.first", "the held-back field is reported");
            // a created record whose live flag is held back stays out of use, so a group pointing at it is refused
            const RenamePack make = mustParse(t, renamesDoc(", \"organisations\": {\"4\": {\"name\": \"N\", \"short\": \"N\"}}, \"groups\": {\"6\": {\"name\": \"G\", \"short\": \"G\", \"org\": 4}}"), "make");
            std::set<std::string> holdLive = {"o4.live"};
            ApplyOutcome blocked;
            t.IsFalse(applyRenamePack(make, base, &holdLive, blocked), "the group's organisation is not in use");
            t.IsTrue(contains(blocked.error, "organisation 4 is not in use"), blocked.error);
        });
    });

    // ------------------------------------------------------------------------------------------------ profile and ledger
    MiniTest::Case("DataPackProfile", [](TestCase &tc)
    {
        tc.Run("the first session copies the base card, applies the pack and leaves the base card alone", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("first", &base);
            const std::string json = renameFirst({{1, "Karl"}, {2, "Gotch"}});
            const Bytes baseBefore = readAll(cards.base0 / kSaveName / kSaveName);
            profile::Result r = profile::prepare(cards.config({packInput("a.pack", "1.0.0", json)}));
            t.IsTrue(r.problem.empty() && r.saveFound && r.applied && r.imageChanged, "applied: " + r.problem);
            t.Equals(r.root0, cards.profile / "mc0", "the card root is the profile's mc0");
            t.IsTrue(contains(r.line, "profile card with 1 data pack on") && contains(r.line, "your own card is not changed"), r.line);
            const Bytes saved = readAll(cards.profileSave());
            t.IsTrue(checkIntegrity(saved, testTable()).allOk(), "the profile's save verifies");
            t.IsTrue(sameFirst(saved, 1, "Karl") && sameFirst(saved, 2, "Gotch") && sameFirst(saved, 3, "First3"), "the names are in the profile");
            t.IsTrue(readAll(cards.base0 / kSaveName / kSaveName) == baseBefore, "the base card's save is untouched");
            t.IsTrue(readAll(cards.profile / "mc0" / kSaveName / "icon.sys") == (Bytes{1, 2, 3, 4}), "the icon came along");
            t.IsTrue(fs::exists(cards.ledger()) && !fs::exists(cards.profile / "ledger.json.new"), "the ledger is in place, with no update left over");
            const std::string ledger = readText(cards.ledger());
            t.IsTrue(contains(ledger, "\"w1.first\"") && contains(ledger, "\"pack\": \"a.pack\"") && contains(ledger, "\"version\": \"1.0.0\""), "the ledger names the pack and version");
            t.IsTrue(r.packs.size() == 1 && r.packs[0].written == 2 && r.packs[0].error.empty(), "the report counts the fields");
            bool said = false;
            for (const auto &n : r.packs[0].notes)
                said = said || contains(n, "Changes 2 fields");
            t.IsTrue(said, "the pack gets a line saying what it changed");
        });

        tc.Run("a second session with nothing new changes no file", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("again", &base);
            const std::vector<profile::PackInput> packs = {packInput("a.pack", "1.0.0", renameFirst({{1, "Karl"}}))};
            profile::prepare(cards.config(packs));
            const Bytes save1 = readAll(cards.profileSave());
            const std::string ledger1 = readText(cards.ledger());
            fs::last_write_time(cards.profileSave(), fs::file_time_type::clock::now() - std::chrono::hours(5));
            const auto stamp = fs::last_write_time(cards.profileSave());
            profile::Result r = profile::prepare(cards.config(packs));
            t.IsTrue(r.applied && !r.imageChanged, "nothing was rewritten");
            t.IsTrue(readAll(cards.profileSave()) == save1 && readText(cards.ledger()) == ledger1, "same bytes");
            t.IsTrue(fs::last_write_time(cards.profileSave()) == stamp, "the save file was not even touched");
            // and a third time, after the game re-saved the very same bytes
            writeAll(cards.profileSave(), save1);
            r = profile::prepare(cards.config(packs));
            t.IsTrue(!r.imageChanged && r.packs[0].kept == 0, "a re-save of the same card keeps everything and holds nothing");
        });

        tc.Run("no save anywhere: nothing is applied and the pack says so", [](TestCase &t)
        {
            Cards cards("nosave", nullptr);
            profile::Result r = profile::prepare(cards.config({packInput("a.pack", "1.0.0", renameFirst({{1, "Karl"}}))}));
            t.IsTrue(!r.saveFound && !r.applied && r.problem.empty(), "not a failure");
            t.IsTrue(contains(r.line, "has no save yet"), r.line);
            t.IsTrue(r.packs[0].notes.size() == 1 && contains(r.packs[0].notes[0], "No save to apply to yet: save once in the game, then restart."), "the pack's note");
            t.IsFalse(fs::exists(cards.profileSave()) || fs::exists(cards.ledger()), "no save and no ledger were made up");
            t.IsTrue(fs::is_directory(r.root0), "the profile card folder exists, so the game can save into it");
            // the player saves once in the game
            const Bytes base = makeSave();
            writeAll(cards.profileSave(), base);
            r = profile::prepare(cards.config({packInput("a.pack", "1.0.0", renameFirst({{1, "Karl"}}))}));
            t.IsTrue(r.applied && sameFirst(readAll(cards.profileSave()), 1, "Karl"), "after the first save the pack applies");
        });

        tc.Run("a base card that appears later is copied into an empty profile slot only", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("slots", &base);
            writeAll(cards.base1 / kSaveName / kSaveName, Bytes{5, 5, 5});
            writeAll(cards.base1 / kSaveName / "view.ico.mctmp", Bytes{1});
            profile::prepare(cards.config({packInput("a.pack", "1.0.0", renameFirst({{1, "Karl"}}))}));
            t.IsTrue(readAll(cards.profile / "mc1" / kSaveName / kSaveName) == (Bytes{5, 5, 5}), "slot 2 came along too, untouched by packs");
            t.IsFalse(fs::exists(cards.profile / "mc1" / kSaveName / "view.ico.mctmp"), "a scratch file is not copied");
            // the base card changes; the profile keeps its own
            Bytes other = makeSave();
            other[0x2000] = 0x11;
            fixIntegrity(other, testTable());
            writeAll(cards.base0 / kSaveName / kSaveName, other);
            profile::prepare(cards.config({packInput("a.pack", "1.0.0", renameFirst({{1, "Karl"}}))}));
            t.IsTrue(readAll(cards.profileSave())[0x2000] == 0, "the profile is not overwritten by a newer base card");
        });

        tc.Run("a later pack wins a field, and the order of the packs decides", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("later", &base);
            const auto a = packInput("a.pack", "1.0.0", renameFirst({{1, "FromA"}, {2, "OnlyA"}}));
            const auto b = packInput("b.pack", "1.0.0", renameFirst({{1, "FromB"}, {3, "OnlyB"}}));
            profile::Result r = profile::prepare(cards.config({a, b}));
            Bytes saved = readAll(cards.profileSave());
            t.IsTrue(sameFirst(saved, 1, "FromB") && sameFirst(saved, 2, "OnlyA") && sameFirst(saved, 3, "OnlyB"), "B wins slot 1, both keep their own");
            t.IsTrue(r.packs[0].written == 1 && r.packs[0].overridden == 1 && r.packs[1].written == 2, "A has one field overridden: written " + std::to_string(r.packs[0].written));
            t.IsTrue(contains(readText(cards.ledger()), "\"w1.first\": {\"pack\": \"b.pack\""), "the ledger names the winner");
            r = profile::prepare(cards.config({b, a}));
            saved = readAll(cards.profileSave());
            t.IsTrue(sameFirst(saved, 1, "FromA"), "reversed: A wins slot 1");
            t.IsTrue(sameFirst(saved, 2, "OnlyA") && sameFirst(saved, 3, "OnlyB"), "the rest stays");
            t.IsTrue(contains(readText(cards.ledger()), "\"w1.first\": {\"pack\": \"a.pack\""), "the ledger follows");
        });

        tc.Run("turning a pack off puts the before bytes back, to the byte", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("remove", &base);
            const auto a = packInput("a.pack", "1.0.0", renameFirst({{1, "FromA"}, {2, "OnlyA"}}));
            const auto b = packInput("b.pack", "1.0.0", renameFirst({{1, "FromB"}, {3, "OnlyB"}}));
            profile::prepare(cards.config({a, b}));
            // B off: slot 1 goes back to A's value (A is still on), slot 3 to the original
            profile::prepare(cards.config({a}));
            Bytes saved = readAll(cards.profileSave());
            t.IsTrue(sameFirst(saved, 1, "FromA") && sameFirst(saved, 2, "OnlyA") && sameFirst(saved, 3, "First3"), "B is gone, A's value shows again");
            // A off too: the profile is the base card again, byte for byte
            profile::Result r = profile::prepare(cards.config({}));
            saved = readAll(cards.profileSave());
            t.IsTrue(saved == base, "with no pack the profile's save equals the base card's");
            t.IsTrue(r.imageChanged, "it was rewritten");
            const std::string ledger = readText(cards.ledger());
            t.IsFalse(contains(ledger, "\"w"), "the ledger is empty");
            // and on again
            profile::prepare(cards.config({a}));
            t.IsTrue(sameFirst(readAll(cards.profileSave()), 1, "FromA"), "back on");
        });

        tc.Run("a change the player made in the game is kept, and said", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("player", &base);
            const auto a = packInput("a.pack", "1.0.0", renameFirst({{1, "FromA"}, {2, "OnlyA"}}));
            profile::prepare(cards.config({a}));
            playerEdit(cards.profileSave(), [](Bytes &image) {
                put(image, kOffNames + 1 * kNameStride + 1, textBytes("Mine", kWidthFirst)); // wrestler 1 renamed by hand
                image[0x1234] = 0x99;                                                        // and something the pack never touches
                put(image, kOffNames + 8 * kNameStride + 1, textBytes("Other", kWidthFirst)); // a wrestler the pack does not mention
            });
            profile::Result r = profile::prepare(cards.config({a}));
            Bytes saved = readAll(cards.profileSave());
            t.IsTrue(sameFirst(saved, 1, "Mine") && sameFirst(saved, 2, "OnlyA"), "the player's name stays, the pack's other field stays");
            t.IsTrue(saved[0x1234] == 0x99 && sameFirst(saved, 8, "Other"), "other edits survive");
            t.Equals(r.packs[0].kept, static_cast<size_t>(1), "one field kept");
            bool noted = false;
            for (const auto &n : r.packs[0].notes)
                noted = noted || n == "Kept your change to wrestler 1's FIRST name.";
            t.IsTrue(noted, "the note says which field");
            // a second session says it again and still keeps it
            r = profile::prepare(cards.config({a}));
            t.IsTrue(r.packs[0].kept == 1 && sameFirst(readAll(cards.profileSave()), 1, "Mine"), "still kept the next time");
            // the pack goes off but another stays: the player's name stays, the pack's other field goes back
            const auto other = packInput("o.pack", "1.0.0", renameFirst({{5, "Five"}}));
            profile::prepare(cards.config({other}));
            saved = readAll(cards.profileSave());
            t.IsTrue(sameFirst(saved, 1, "Mine"), "after the pack went off the player's name is still there");
            t.IsTrue(sameFirst(saved, 2, "First2"), "while the pack's other field is back to what it was");
            t.IsTrue(sameFirst(saved, 5, "Five") && saved[0x1234] == 0x99 && sameFirst(saved, 8, "Other"), "the other pack and the player's other edits are there");
            t.IsFalse(contains(readText(cards.ledger()), "w1.first"), "the player's field is no longer in the ledger");
            // the pack comes back: it takes the field again, and the player's name becomes the new before
            profile::prepare(cards.config({a, other}));
            saved = readAll(cards.profileSave());
            t.IsTrue(sameFirst(saved, 1, "FromA"), "turned on again, the pack applies");
            profile::prepare(cards.config({other}));
            t.IsTrue(sameFirst(readAll(cards.profileSave()), 1, "Mine"), "and off again, the player's name returns");
        });

        tc.Run("a pack changed in an update: new values, dropped fields, new version in the ledger", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("version", &base);
            profile::prepare(cards.config({packInput("a.pack", "1.0.0", renameFirst({{1, "OneA"}, {2, "TwoA"}}))}));
            profile::Result r = profile::prepare(cards.config({packInput("a.pack", "2.0.0", renameFirst({{1, "OneB"}, {4, "FourB"}}))}));
            const Bytes saved = readAll(cards.profileSave());
            t.IsTrue(sameFirst(saved, 1, "OneB"), "the new value");
            t.IsTrue(sameFirst(saved, 2, "First2"), "the field the update dropped is back to what it was");
            t.IsTrue(sameFirst(saved, 4, "FourB"), "the field it added");
            const std::string ledger = readText(cards.ledger());
            t.IsTrue(contains(ledger, "\"version\": \"2.0.0\"") && !contains(ledger, "\"version\": \"1.0.0\""), "the ledger names version 2.0.0");
            t.IsTrue(r.packs[0].kept == 0 && r.imageChanged, "nothing was held back");
        });

        tc.Run("a pack that cannot be used is reported, and the others still apply", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("badpack", &base);
            const auto good = packInput("good.pack", "1.0.0", renameFirst({{1, "Good"}}));
            const auto first = packInput("bad.pack", "1.0.0", renameFirst({{2, "WasOk"}}));
            profile::prepare(cards.config({first, good}));
            t.IsTrue(sameFirst(readAll(cards.profileSave()), 2, "WasOk"), "the pack was fine at first");
            auto broken = packInput("bad.pack", "1.0.1", "{\"format\": \"fpr-rename\", \"version\": 1, \"wrestlers\": {\"2\": {\"first\": \"WayTooLong\"}}}");
            profile::PackInput none;
            none.id = "empty.pack";
            none.version = "1.0.0";
            profile::PackInput unreadable;
            unreadable.id = "gone.pack";
            unreadable.hasFile = true;
            unreadable.readError = "the file is larger than 16 MiB";
            profile::Result r = profile::prepare(cards.config({broken, good, none, unreadable}));
            t.IsTrue(contains(r.packs[0].error, "data/renames.json: wrestlers.2.first:") && oneLine(r.packs[0].error), "the broken pack says why: " + r.packs[0].error);
            t.IsTrue(r.packs[1].error.empty() && r.packs[1].written == 1, "the good pack applied");
            t.IsTrue(r.packs[2].error.empty() && contains(r.packs[2].notes.at(0), "has no data/renames.json"), "a pack with no renames file says it changes nothing");
            t.IsTrue(contains(r.packs[3].error, "larger than 16 MiB"), "an unreadable file is reported");
            const Bytes saved = readAll(cards.profileSave());
            t.IsTrue(sameFirst(saved, 1, "Good") && sameFirst(saved, 2, "First2"), "the broken pack's earlier change is undone");
        });

        tc.Run("a record a pack created goes away with the pack", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("created", &base);
            const std::string doc = renamesDoc(", \"organisations\": {\"4\": {\"name\": \"Fresh\", \"short\": \"F\", \"created\": true}}, \"groups\": {\"6\": {\"name\": \"FG\", \"short\": \"G\", \"org\": 4, \"created\": true}}");
            profile::prepare(cards.config({packInput("a.pack", "1.0.0", doc)}));
            Bytes saved = readAll(cards.profileSave());
            t.IsTrue(orgLive(saved, 4) && groupLive(saved, 6), "created");
            profile::prepare(cards.config({}));
            saved = readAll(cards.profileSave());
            t.IsTrue(saved == base, "gone, with the list orders, byte for byte");
        });

        tc.Run("a crash at any point leaves a state the next session finishes", [](TestCase &t)
        {
            const Bytes base = makeSave();
            const auto v1 = packInput("a.pack", "1.0.0", renameFirst({{1, "OneA"}, {2, "TwoA"}}));
            const auto v2 = packInput("a.pack", "2.0.0", renameFirst({{1, "OneB"}, {3, "ThreeB"}}));
            // the reference: no crash
            Cards clean("crash_ref", &base);
            profile::prepare(clean.config({v1}));
            profile::prepare(clean.config({v2}));
            const Bytes refSave = readAll(clean.profileSave());
            const std::string refLedger = readText(clean.ledger());
            for (int failpoint : {1, 2})
            {
                Cards cards(failpoint == 1 ? "crash_1" : "crash_2", &base);
                profile::prepare(cards.config({v1}));
                const Bytes savedBefore = readAll(cards.profileSave());
                profile::Config cfg = cards.config({v2});
                cfg.failpoint = failpoint;
                profile::prepare(cfg); // "dies" here
                t.IsTrue(fs::exists(cards.profile / "ledger.json.new"), "an update was left half done (failpoint " + std::to_string(failpoint) + ")");
                if (failpoint == 1)
                    t.IsTrue(readAll(cards.profileSave()) == savedBefore, "the save is still the old one");
                else
                    t.IsTrue(readAll(cards.profileSave()) != savedBefore, "the save is already the new one");
                profile::Result r = profile::prepare(cards.config({v2}));
                t.IsFalse(fs::exists(cards.profile / "ledger.json.new"), "settled");
                t.IsTrue(readAll(cards.profileSave()) == refSave, "the save is what a clean run gives (failpoint " + std::to_string(failpoint) + ")");
                t.IsTrue(readText(cards.ledger()) == refLedger, "so is the ledger (failpoint " + std::to_string(failpoint) + ")");
                t.IsTrue(r.packs[0].kept == 0, "nothing is mistaken for a change of the player's");
                bool settledLogged = false;
                for (const auto &line : r.log)
                    settledLogged = settledLogged || contains(line, "interrupted");
                t.IsTrue(settledLogged, "the log says an update was settled");
            }
        });

        tc.Run("files are written through a temporary file and a rename", [](TestCase &t)
        {
            const fs::path dir = tempDir("atomic");
            const fs::path target = dir / "sub" / "file.bin";
            t.IsTrue(profile::writeFileAtomic(target, "first"), "creates the folder and the file");
            t.Equals(readText(target), std::string("first"), "content");
            t.IsFalse(fs::exists(dir / "sub" / "file.bin.tmp"), "no temporary file is left");
            // a leftover from a crash is replaced
            writeAll(dir / "sub" / "file.bin.tmp", Bytes{'j', 'u', 'n', 'k'});
            t.IsTrue(profile::writeFileAtomic(target, "second"), "overwrites");
            t.Equals(readText(target), std::string("second"), "new content");
            t.IsFalse(fs::exists(dir / "sub" / "file.bin.tmp"), "the leftover is gone");
            t.IsTrue(profile::writeFileAtomic(dir / "empty", ""), "an empty file");
            t.Equals(readAll(dir / "empty").size(), static_cast<size_t>(0), "is empty");
            // a target that cannot be replaced: the old state stays and nothing is left behind
            const fs::path busy = dir / "folder";
            fs::create_directories(busy / "inner");
            t.IsFalse(profile::writeFileAtomic(busy, "x"), "a folder in the way: the write fails");
            t.IsTrue(fs::is_directory(busy / "inner"), "the folder is intact");
            t.IsFalse(fs::exists(dir / "folder.tmp"), "and no temporary file remains");
        });

        tc.Run("a save the game would refuse is not touched", [](TestCase &t)
        {
            const std::vector<profile::PackInput> packs = {packInput("a.pack", "1.0.0", renameFirst({{1, "Karl"}}))};
            {
                const Bytes base = makeSave();
                Cards cards("corrupt", &base);
                Bytes bad = base;
                bad[0x4000] ^= 1;
                bad[0x9000] ^= 1; // two bad blocks: the game would reject it
                writeAll(cards.base0 / kSaveName / kSaveName, bad);
                profile::Result r = profile::prepare(cards.config(packs));
                t.IsTrue(!r.applied && contains(r.problem, "does not pass the game's own checks"), r.problem);
                t.IsTrue(readAll(cards.profileSave()) == bad, "the profile's copy was left as it was");
                t.IsTrue(contains(r.packs[0].notes.at(0), "does not pass"), "the pack carries the reason");
                t.IsFalse(fs::exists(cards.ledger()), "no ledger");
            }
            {
                Cards cards("wrongsize", nullptr);
                writeAll(cards.base0 / kSaveName / kSaveName, Bytes(1000, 1));
                profile::Result r = profile::prepare(cards.config(packs));
                t.IsTrue(!r.applied && contains(r.problem, "not a Fire Pro Wrestling Returns save"), r.problem);
            }
            {
                const Bytes base = makeSave();
                Cards cards("oldversion", &base);
                Bytes old = base;
                putU32(old, kRosterMarkerOff, 0x1234);
                fixIntegrity(old, testTable());
                writeAll(cards.base0 / kSaveName / kSaveName, old);
                profile::Result r = profile::prepare(cards.config(packs));
                t.IsTrue(!r.applied && contains(r.problem, "not from this version of the game"), r.problem);
            }
            {
                const Bytes base = makeSave();
                Cards cards("notable", &base);
                profile::Config cfg = cards.config(packs);
                cfg.crcTable = nullptr;
                cfg.crcTableError = "no executable";
                profile::Result r = profile::prepare(cfg);
                t.IsTrue(!r.applied && contains(r.problem, "checksum table could not be read (no executable)"), r.problem);
                t.IsTrue(readAll(cards.profileSave()) == base, "the base card's copy is there, unchanged, so the game still plays");
            }
        });

        tc.Run("a ledger that cannot be read is set aside", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("badledger", &base);
            const auto a = packInput("a.pack", "1.0.0", renameFirst({{1, "Karl"}}));
            profile::prepare(cards.config({a}));
            writeAll(cards.ledger(), Bytes{'n', 'o', 't', ' ', 'j', 's', 'o', 'n'});
            profile::Result r = profile::prepare(cards.config({a}));
            t.IsTrue(fs::exists(cards.profile / "ledger.json.bad"), "the old file is kept");
            t.IsTrue(fs::exists(cards.ledger()), "and a new one made");
            bool said = false;
            for (const auto &n : r.packs[0].notes)
                said = said || contains(n, "ledger could not be read");
            t.IsTrue(said, "the pack says so");
            t.IsTrue(sameFirst(readAll(cards.profileSave()), 1, "Karl"), "the pack still applies");
            // entries that make no sense are dropped, the rest kept
            writeAll(cards.ledger(), Bytes{});
            std::string text = "{\"format\": \"fpr-ledger\", \"version\": 1, \"fields\": {\"w9.nope\": {\"pack\": \"x\", \"version\": \"1.0.0\", \"before\": \"00\", \"written\": \"00\"}, "
                               "\"w1.first\": {\"pack\": \"a.pack\", \"version\": \"1.0.0\", \"before\": \"00\", \"written\": \"00\"}}}";
            writeAll(cards.ledger(), Bytes(text.begin(), text.end()));
            r = profile::prepare(cards.config({a}));
            t.IsTrue(r.problem.empty(), "entries of the wrong size or key do not stop the session: " + r.problem);
        });

        tc.Run("Keep this card copies the profile's save to the base card after backing it up", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("keep", &base);
            profile::KeepResult none = profile::keepCard(cards.profile, cards.base0);
            t.IsTrue(!none.ok && contains(none.message, "no save to keep"), none.message);
            profile::prepare(cards.config({packInput("a.pack", "1.0.0", renameFirst({{1, "Karl"}}))}));
            t.IsTrue(profile::profileHasSave(cards.profile), "the profile has a save");
            const Bytes profileSave = readAll(cards.profileSave());
            const Bytes baseBefore = readAll(cards.base0 / kSaveName / kSaveName);
            const profile::KeepResult k = profile::keepCard(cards.profile, cards.base0);
            t.IsTrue(k.ok && oneLine(k.message), k.message);
            t.IsTrue(readAll(cards.base0 / kSaveName / kSaveName) == profileSave, "the base card now holds the profile's save");
            t.IsTrue(!k.backup.empty() && k.backup.filename().string().rfind("base-backup-", 0) == 0 && k.backup.parent_path() == cards.profile, "the backup folder is named by date and time");
            t.IsTrue(readAll(k.backup / kSaveName / kSaveName) == baseBefore, "it holds the base card's old save");
            t.IsTrue(readAll(k.backup / kSaveName / "icon.sys") == (Bytes{1, 2, 3, 4}), "and its other files");
            t.IsTrue(readAll(cards.base0 / kSaveName / "icon.sys") == readAll(cards.profile / "mc0" / kSaveName / "icon.sys"), "every file of the folder went");
            const profile::KeepResult again = profile::keepCard(cards.profile, cards.base0);
            t.IsTrue(again.ok && again.backup != k.backup, "a second keep makes its own backup folder");
            // a base card that has no folder yet
            Cards fresh("keep_fresh", nullptr);
            writeAll(fresh.profileSave(), profileSave);
            const profile::KeepResult f = profile::keepCard(fresh.profile, fresh.base0);
            t.IsTrue(f.ok && f.backup.empty() && readAll(fresh.base0 / kSaveName / kSaveName) == profileSave, "keeping onto an empty base card needs no backup");
        });

        tc.Run("the second card slot's folder is next to the first, as the card code derives it", [](TestCase &t)
        {
            t.Equals(profile::slot2Root(fs::path("/x/profile/mc0")), fs::path("/x/profile/mc1"), "mc0 -> mc1");
            t.Equals(profile::slot2Root(fs::path("/x/profile/MC0")), fs::path("/x/profile/mc1"), "the name is matched without regard to capitals");
            t.Equals(profile::slot2Root(fs::path("/x/card")), fs::path("/x/card_slot1"), "another name gets a suffix");
            t.IsTrue(!profile::baseCardLine().empty() && contains(profile::baseCardLine(), "your own card"), "the line for no data packs");
        });
    });

    // ------------------------------------------------------------------------------------------- the manager's side of it
    MiniTest::Case("DataPackManager", [](TestCase &tc)
    {
        tc.Run("a data pack's files and notes are reachable through the manager, as the host reads them", [](TestCase &t)
        {
            const fs::path root = tempDir("manager");
            std::error_code ec;
            fs::create_directories(root / "mods", ec);
            std::string error;
            auto build = [&](const std::string &id, const std::string &kind, std::vector<std::pair<std::string, std::string>> files) {
                std::string toml = "id = \"" + id + "\"\nname = \"Pack " + id + "\"\nversion = \"1.2.3\"\nauthors = [\"Tester\"]\ngame = \"SLUS_217.02\"\napi = 1\nkind = \"" + kind + "\"\n";
                files.insert(files.begin(), {"mod.toml", toml});
                auto bytes = buildPackage(files, &error);
                t.IsTrue(bytes.has_value(), "package: " + error);
                if (bytes)
                    writeBytesToFile(root / "mods" / (id + ".fprmod"), *bytes);
            };
            const std::string doc = renameFirst({{1, "Karl"}});
            build("a.data", "data", {{"data/renames.json", doc}});
            build("b.empty", "data", {{"data/readme.txt", "nothing"}});
            build("c.assets", "assets", {{"assets/x.txt", "x"}});
            ps2x::mods::Manager manager;
            ps2x::mods::Manager::Config config;
            config.modsDir = root / "mods";
            config.statePath = root / "mods.toml";
            t.IsTrue(manager.init(config), "starts");
            for (const char *id : {"a.data", "b.empty", "c.assets"})
                t.IsTrue(manager.setEnabled(id, true).ok, std::string("on: ") + id);
            manager.commitSession();
            const auto ids = manager.sessionActive();
            t.IsTrue(ids.size() == 3, "all three are in the session, in load order");
            size_t dataPacks = 0;
            for (const std::string &id : ids)
            {
                const auto info = manager.find(id);
                if (info && info->kind == ps2x::mods::Kind::Data)
                    ++dataPacks;
            }
            t.Equals(dataPacks, static_cast<size_t>(2), "two of them are data packs (the kind decides, not the content)");
            t.IsTrue(manager.hasFile("a.data", "data/renames.json"), "hasFile: the renames file");
            t.IsFalse(manager.hasFile("b.empty", "data/renames.json"), "hasFile: a data pack without one");
            t.IsFalse(manager.hasFile("a.data", "data/missing.json") || manager.hasFile("nobody", "data/renames.json"), "hasFile: other paths and mods");
            const auto bytes = manager.readFile("a.data", "data/renames.json", 16ull * 1024 * 1024, &error);
            t.IsTrue(bytes && std::string(bytes->begin(), bytes->end()) == doc, "the file reads back as it was written");
            RenamePack pack;
            t.IsTrue(bytes && parseRenamePack(std::string(bytes->begin(), bytes->end()), pack, &error), "and parses: " + error);
            std::string why;
            t.IsFalse(manager.readFile("a.data", "data/renames.json", 10, &why).has_value() || why.empty(), "a file over the size limit is refused with a reason: " + why);
            // notes
            manager.addInfoNote("a.data", "one");
            manager.addInfoNote("a.data", "one");
            manager.addInfoNote("a.data", "two");
            t.Equals(manager.infoNotes("a.data").size(), static_cast<size_t>(2), "an info note is kept once");
            for (int i = 0; i < 100; ++i)
                manager.addInfoNote("a.data", "line " + std::to_string(i));
            t.Equals(manager.infoNotes("a.data").size(), ps2x::mods::Manager::kMaxInfoNotes, "and at most the cap");
            t.IsTrue(manager.infoNotes("b.empty").empty() && manager.runtimeNotes("a.data").empty(), "other mods and the complaint list are separate");
        });
    });

    // --------------------------------------------------------------------------------------------- the player's own files
    MiniTest::Case("DataPackRealSave", [](TestCase &tc)
    {
        tc.Run("C++ apply is byte-identical to tools/fprsave.py apply on the community rename set", [](TestCase &t)
        {
            const fs::path root = findRepoRoot();
            const fs::path community = root / "baserom" / "saves" / "community" / "gamefaqs-16161-us-rename";
            if (root.empty() || !fs::exists(community) || !runCommand("python -I -c \"pass\""))
            {
                std::printf("(skipped: needs baserom/ with the saves and python on the path) ");
                return;
            }
            const fs::path work = tempDir("realsave");
            const fs::path baseDir = root / "baserom" / "saves" / "001_baseline";
            const Bytes base = readAll(baseDir / kSaveName);
            t.Equals(base.size(), kSaveSize, "the default save is there");
            CrcTable table{};
            std::string error;
            t.IsTrue(crcTableFromElf(root / "baserom" / "iso" / "SLUS_217.02", table, &error), error);

            struct Case
            {
                const char *name;
                std::string extra;
            };
            const Case cases[] = {{"names", ""}, {"appearance", " --appearance"}};
            for (const Case &c : cases)
            {
                const fs::path renames = work / (std::string(c.name) + ".json");
                const fs::path expected = work / (std::string(c.name) + ".py.dat");
                const std::string tool = "python -I " + q(root / "tools" / "fprsave.py") + " ";
                t.IsTrue(runCommand(tool + "renames " + q(community) + " --base " + q(baseDir) + " --out " + q(renames) + c.extra), std::string("fprsave.py renames ") + c.name);
                t.IsTrue(runCommand(tool + "apply " + q(baseDir) + " " + q(renames) + " --out " + q(expected) + " --quiet"), std::string("fprsave.py apply ") + c.name);
                const Bytes want = readAll(expected);
                t.Equals(want.size(), kSaveSize, std::string("python wrote a whole save: ") + c.name);

                RenamePack pack;
                t.IsTrue(parseRenamePack(readText(renames), pack, &error), std::string("the C++ parser reads the Python file ") + c.name + ": " + error);
                ApplyOutcome out;
                t.IsTrue(applyRenamePack(pack, base, nullptr, out), std::string("apply ") + c.name + ": " + out.error);
                Bytes got = base;
                applyWrites(got, out.writes);
                fixIntegrity(got, table);
                t.IsTrue(got == want, std::string("byte for byte equal to fprsave.py apply: ") + c.name);
                t.IsTrue(checkIntegrity(got, table).allOk(), "and the game's checks pass");
                size_t differing = 0;
                for (size_t i = 0; i < got.size(); ++i)
                    differing += got[i] != base[i];
                std::printf("[%s: %zu entries, %zu field writes, %zu bytes differ from the default save] ", c.name, pack.entryCount(), out.writes.size(), differing);

                // through the profile code as well: a base card holding the default save, one pack
                Cards cards((std::string("realsave_card_") + c.name).c_str(), &base);
                profile::Config cfg = cards.config({packInput("community.rename", "1.0.0", readText(renames))});
                cfg.crcTable = &table;
                const profile::Result r = profile::prepare(cfg);
                t.IsTrue(r.applied && r.problem.empty(), std::string("profile build ") + c.name + ": " + r.problem);
                t.IsTrue(readAll(cards.profileSave()) == want, std::string("the profile's save equals fprsave.py apply: ") + c.name);
                const profile::Result again = profile::prepare(cfg);
                t.IsTrue(again.applied && !again.imageChanged, std::string("a second build changes nothing: ") + c.name);
                profile::Config off = cards.config({});
                off.crcTable = &table;
                const profile::Result removed = profile::prepare(off);
                std::printf("[profile %s: first build %.1f ms, up to date %.1f ms, pack off %.1f ms] ", c.name, r.milliseconds, again.milliseconds, removed.milliseconds);
                t.IsTrue(readAll(cards.profileSave()) == base, std::string("and turning the pack off gives the default save again: ") + c.name);
            }

            // the organisation-and-group creating set made in the game in task s1, when it is there
            const fs::path created = root / "baserom" / "out" / "test" / "s1_proofs" / "created_org_group";
            if (fs::exists(created))
            {
                const fs::path renames = work / "created.json";
                const fs::path expected = work / "created.py.dat";
                const std::string tool = "python -I " + q(root / "tools" / "fprsave.py") + " ";
                t.IsTrue(runCommand(tool + "renames " + q(created) + " --base " + q(baseDir) + " --out " + q(renames)), "renames of the created set");
                t.IsTrue(runCommand(tool + "apply " + q(baseDir) + " " + q(renames) + " --out " + q(expected) + " --quiet"), "apply of the created set");
                RenamePack pack;
                t.IsTrue(parseRenamePack(readText(renames), pack, &error), error);
                ApplyOutcome out;
                t.IsTrue(applyRenamePack(pack, base, nullptr, out), out.error);
                Bytes got = base;
                applyWrites(got, out.writes);
                fixIntegrity(got, table);
                t.IsTrue(got == readAll(expected), "the created organisation and group: equal to fprsave.py apply");
            }
        });
    });
}
