#include "MiniTest.h"

#include "data_pack.h"
#include "fpr_save.h"
#include "mini_json.h"
#include "mod_manager.h"
#include "mod_package.h"
#include "profile.h"
#include "save_container.h"
#include "save_import.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Tests for the save importer (task s4b; src/lib/mods/save_import.*): the text helpers it adds to fpr_save, the rename set it extracts
// from a save (byte for byte what tools/fprsave.py writes), the region gate, the comparison base, the report lines and the pack it
// installs. The saves are synthetic images built here with a made-up checksum table; the last suite compares with tools/fprsave.py on
// the player's own files under baserom/ and says "skipped" when they are not there.

namespace
{
    namespace fs = std::filesystem;
    using namespace ps2x::mods;
    using namespace ps2x::mods::fpr;
    namespace imp = ps2x::mods::import;

    // ---- helpers --------------------------------------------------------------------------------------------------------

    fs::path tempDir(const char *name)
    {
        fs::path dir = fs::temp_directory_path() / (std::string("ps2x_saveimport_test_") + name);
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

    void writeText(const fs::path &path, const std::string &text) { writeAll(path, Bytes(text.begin(), text.end())); }

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

    // Offsets the importer's tests touch (outside the fields a pack owns).
    constexpr size_t kOffWrestlerGroupTable = kRoster + 0x4748;
    constexpr size_t kOffReferees = kRoster + 0x21A0;
    constexpr size_t kOffEditSlots = kRoster + 0x2E1B8;
    constexpr size_t kEditStride = 0x44C;
    constexpr size_t kOffRings = kOffAppearance + 327 * kAppearanceStride;

    // A small valid save: 327 wrestlers with names, 4 organisations and 6 groups in use, identity list orders, and a few other bytes set
    // so a change that spills over would show.
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
            putU16(img, kOffWrestlerGroupTable + 2 * static_cast<size_t>(k), k % 6);
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
        img[0] = 0x05;
        img[0x1234] = 0x77;
        putU32(img, 0x3F398, 0x00010001);
        fixIntegrity(img, testTable());
        return img;
    }

    Bytes fixed(Bytes image)
    {
        fixIntegrity(image, testTable());
        return image;
    }

    void renameFirst(Bytes &img, int k, const std::string &name) { put(img, kOffNames + static_cast<size_t>(k) * kNameStride + 1, textBytes(name, kWidthFirst)); }
    void renameOrg(Bytes &img, int i, const std::string &name, const std::string &shortName)
    {
        put(img, kOffOrgs + static_cast<size_t>(i) * kOrgStride + 2, textBytes(name, kWidthOrgLong));
        put(img, kOffOrgs + static_cast<size_t>(i) * kOrgStride + 28, textBytes(shortName, kWidthOrgShort));
    }

    // The list order array: `prefix` first (the records in use), the other indices after it in order.
    void setOrder(Bytes &img, size_t offset, int count, const std::vector<int> &prefix)
    {
        std::vector<bool> used(static_cast<size_t>(count), false);
        size_t at = 0;
        for (const int x : prefix)
        {
            img[offset + at++] = static_cast<uint8_t>(x);
            used[static_cast<size_t>(x)] = true;
        }
        for (int i = 0; i < count; ++i)
            if (!used[static_cast<size_t>(i)])
                img[offset + at++] = static_cast<uint8_t>(i);
    }

    imp::Env envFor(Manager *m, const CrcTable *table = &testTable())
    {
        imp::Env env;
        env.manager = m;
        env.crcTable = table;
        return env;
    }

    // A manager in a temporary pref folder, a base card, a profile folder and the places the importer needs.
    struct Rig
    {
        fs::path root, mods, state, base0, profile, scratch;
        Manager manager;
        explicit Rig(const char *name, const Bytes *baseSave = nullptr)
        {
            root = tempDir(name);
            mods = root / "mods";
            state = root / "mods.toml";
            base0 = root / "mc0";
            profile = root / "profile";
            scratch = root / "import-tmp";
            std::error_code ec;
            fs::create_directories(base0, ec);
            if (baseSave)
                writeAll(base0 / kSaveName / kSaveName, *baseSave);
            Manager::Config config;
            config.modsDir = mods;
            config.statePath = state;
            manager.init(config);
        }
        imp::Env env() const
        {
            imp::Env e = envFor(const_cast<Manager *>(&manager));
            e.profileDir = profile;
            e.baseCard0 = base0;
            e.scratchDir = scratch;
            return e;
        }
    };

    SaveEntry entryOf(const std::string &folder, const Bytes &save, const char *format = "ps2")
    {
        SaveEntry e;
        e.folder = folder;
        e.save = save;
        e.saveName = folder;
        e.format = format;
        return e;
    }

    // ---- real data and python ---------------------------------------------------------------------------------------------

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

    // The command's standard output goes to `out` (the error stream is dropped).
    bool runCommandTo(const std::string &command, const fs::path &out)
    {
#if defined(_WIN32)
        const std::string line = "\"" + command + " > \"" + out.string() + "\" 2> NUL\"";
#else
        const std::string line = command + " > \"" + out.string() + "\" 2> /dev/null";
#endif
        return std::system(line.c_str()) == 0;
    }

    std::string q(const fs::path &p) { return "\"" + p.string() + "\""; }

    // A field's bytes as hexadecimal text, and the value a rename file holds for it, the way the importer writes it.
    std::string hexOf(const Bytes &b)
    {
        static const char d[] = "0123456789abcdef";
        std::string out;
        for (const uint8_t x : b)
        {
            out += d[x >> 4];
            out += d[x & 15];
        }
        return out;
    }

    std::string fieldJson(const Bytes &field)
    {
        bool isHex = false;
        const std::string t = fieldTextOrHex(field.data(), field.size(), &isHex);
        return isHex ? "{\"hex\": \"" + t + "\"}" : json::quote(t);
    }

    std::string textOf(const Bytes &field)
    {
        bool isHex = false;
        const std::string t = fieldTextOrHex(field.data(), field.size(), &isHex);
        return isHex ? "hex:" + t : t;
    }

    Bytes bytesOf(std::initializer_list<int> values)
    {
        Bytes out;
        for (const int v : values)
            out.push_back(static_cast<uint8_t>(v));
        return out;
    }
}

void register_save_import_tests()
{
    // ------------------------------------------------------------------------------------------------------------- text
    MiniTest::Case("SaveImportText", [](TestCase &tc)
    {
        tc.Run("SHA-1 answers the standard test vectors", [](TestCase &t)
        {
            const std::string empty;
            t.Equals(sha1Hex(reinterpret_cast<const uint8_t *>(empty.data()), 0), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"), "empty");
            const std::string abc = "abc";
            t.Equals(sha1Hex(reinterpret_cast<const uint8_t *>(abc.data()), abc.size()), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"), "abc");
            const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
            t.Equals(sha1Hex(reinterpret_cast<const uint8_t *>(two.data()), two.size()), std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"), "the 56-byte vector (needs two blocks)");
            const std::string million(1000000, 'a');
            t.Equals(sha1Hex(reinterpret_cast<const uint8_t *>(million.data()), million.size()), std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"), "a million a");
            const std::string fox = "The quick brown fox jumps over the lazy dog";
            t.Equals(sha1Hex(reinterpret_cast<const uint8_t *>(fox.data()), fox.size()), std::string("2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"), "the fox");
        });

        tc.Run("a name field becomes plain text when that stores back to the same bytes, otherwise hex", [](TestCase &t)
        {
            t.Equals(textOf(bytesOf({0x82, 0x60, 0x82, 0x61, 0x82, 0x62, 0})), std::string("ABC"), "full-width capitals show as ASCII");
            t.Equals(textOf(bytesOf({0x82, 0x60, 0x81, 0x40, 0x82, 0x61})), std::string("A B"), "the ideographic space shows as a space");
            t.Equals(textOf(bytesOf({0x82, 0x60, 0x82, 0x61, 0x31})), std::string("hex:8260826131"), "a half-width digit among full-width letters is not plain text: hex");
            t.Equals(textOf(bytesOf({0x61, 0x62})), std::string("hex:6162"), "half-width ASCII alone: hex");
            t.Equals(textOf(Bytes{}), std::string(""), "an empty field is empty text");
            t.Equals(textOf(bytesOf({0x93, 0xFA, 0x96, 0x7B})), std::string("\xE6\x97\xA5\xE6\x9C\xAC"), "kanji stay as they are");
            t.Equals(textOf(bytesOf({0xB6, 0xDE})), std::string("\xEF\xBD\xB6\xEF\xBE\x9E"), "half-width katakana with the voiced mark: the text itself, not folded");
            t.Equals(textOf(bytesOf({0x81, 0x91})), std::string("\xC2\xA2"), "U+FFE0 shows as the cent sign, as Python's NFKC does");
            t.Equals(textOf(bytesOf({0x81, 0xCA})), std::string("\xC2\xAC"), "U+FFE2 shows as the not sign");
            t.Equals(textOf(bytesOf({0x81, 0x60})), std::string("~"), "the full-width tilde shows as ~");
            t.Equals(textOf(bytesOf({0x85, 0x40})).substr(0, 4), std::string("hex:"), "an unassigned two-byte code: hex");
            t.Equals(textOf(bytesOf({0x82, 0x60, 0x81})), std::string("hex:826081"), "a lead byte with nothing after it: hex");
            t.Equals(textOf(bytesOf({0x87, 0x90})), std::string("hex:8790"), "a code the page decodes but never writes (the NEC row): hex");
            // a full field with no terminator, and bytes after the terminator
            Bytes full;
            for (int i = 0; i < 7; ++i)
            {
                full.push_back(0x82);
                full.push_back(static_cast<uint8_t>(0x60 + i));
            }
            t.Equals(textOf(full), std::string("ABCDEFG"), "a full 14-byte field has no terminator and reads whole");
            Bytes after = bytesOf({0x82, 0x60, 0, 0x82, 0x61, 0x82, 0x62});
            t.Equals(textOf(after), std::string("A"), "bytes after the first zero are not part of the text");
            t.Equals(fieldJson(bytesOf({0x82, 0x60, 0x22, 0})), std::string("{\"hex\": \"826022\"}"), "a quote byte alone stays hex");
        });

        tc.Run("decoding is strict and encodes back", [](TestCase &t)
        {
            std::vector<uint32_t> out;
            t.IsTrue(decodeGameBytes(bytesOf({0x82, 0x60, 0x41}).data(), 3, out) && out.size() == 2 && out[0] == 0xFF21 && out[1] == 0x41, "mixed width");
            t.IsFalse(decodeGameBytes(bytesOf({0x81}).data(), 1, out), "a lone lead byte");
            t.IsFalse(decodeGameBytes(bytesOf({0x85, 0x40}).data(), 2, out), "an unassigned code");
            // every character the encoder can write decodes to a character that encodes to the same bytes (the 6 doubles excepted:
            // both of a pair encode to the bytes, only the larger decodes)
            std::vector<uint32_t> one(1);
            size_t checked = 0, mismatches = 0;
            for (uint32_t cp = 0x80; cp < 0x10000; ++cp)
            {
                one[0] = cp;
                Bytes bytes;
                if (cp >= 0xD800 && cp <= 0xDFFF)
                    continue;
                // encodeGameText folds ASCII only; for cp >= 0x80 it is the bare table
                if (!encodeGameText(one, bytes))
                    continue;
                std::vector<uint32_t> back;
                ++checked;
                Bytes again;
                if (!decodeGameBytes(bytes.data(), bytes.size(), back) || back.size() != 1 || !encodeGameText(back, again) || again != bytes)
                    ++mismatches;
            }
            t.IsTrue(checked > 9000, "the table has its entries (" + std::to_string(checked) + ")");
            t.Equals(mismatches, static_cast<size_t>(0), "decode then encode returns the same bytes for every character");
        });

        tc.Run("the file name becomes an id slug, the manifest a TOML file", [](TestCase &t)
        {
            t.Equals(imp::slugOf("GameFAQs Save (16161).max"), std::string("gamefaqs-save-16161"), "words and numbers");
            t.Equals(imp::slugOf("fire-pro-wrestling-returns.16161.max"), std::string("fire-pro-wrestling-returns-16161"), "dots and dashes");
            t.Equals(imp::slugOf("***.ps2"), std::string("save"), "nothing left: save");
            t.Equals(imp::slugOf("\xE6\x97\xA5\xE6\x9C\xAC.max"), std::string("save"), "only non-ASCII: save");
            t.Equals(imp::slugOf("A--B__C.psu"), std::string("a-b-c"), "runs of other characters become one dash");
            t.IsTrue(imp::slugOf(std::string(100, 'x') + ".max").size() == 40, "at most 40 characters");
            t.IsTrue(imp::slugOf(std::string(39, 'a') + "-bbbbb.max").back() != '-', "no dash at the end after cutting");
            for (const char *name : {"GameFAQs Save (16161).max", "12345.max", "x.ps2", "-.max", "UPPER.CBS"})
            {
                std::string why;
                t.IsTrue(validModId("import." + imp::slugOf(name), &why), std::string("the id for ") + name + " is valid: " + why);
            }
            imp::PackInfo info;
            info.id = "import.test";
            info.name = "A \"quoted\" name\\";
            info.description = "line\nbreak and \x01 control";
            const std::string toml = imp::manifestToml(info);
            t.Equals(toml,
                     std::string("id = \"import.test\"\nname = \"A \\\"quoted\\\" name\\\\\"\nversion = \"1.0.0\"\nauthors = [\"Unknown author\"]\n"
                                 "description = \"line\\nbreak and \\u0001 control\"\ngame = \"SLUS_217.02\"\napi = 1\nkind = \"data\"\n"),
                     "the manifest text");
            imp::Counts counts;
            counts.wrestlers = 327;
            counts.groups = 1;
            counts.appearance = 4;
            t.Equals(imp::descriptionFor("x.max", counts),
                     std::string("Imported from x.max: 327 wrestlers, 1 group and 4 appearance records. While it is on, the game plays from a copy of your memory "
                                 "card with these applied; your own card is not changed."),
                     "the description");
            t.IsTrue(imp::isGameFolder("BASLUS-21702") && imp::isGameFolder("BISLPM-66082") && imp::isGameFolder("BESLES-55041") && !imp::isGameFolder("BASLUS-21703") &&
                         !imp::isGameFolder("") && !imp::isGameFolder("basLUS-21702"),
                     "the three region folders and nothing else");
            t.Equals(std::string(imp::regionOf("BISLPM-66082")), std::string("Japan"), "region names");
        });
    });

    // ------------------------------------------------------------------------------------------------------- the rename set
    MiniTest::Case("SaveImportExtract", [](TestCase &tc)
    {
        tc.Run("two equal saves give an empty set in fprsave.py's exact text", [](TestCase &t)
        {
            const Bytes base = makeSave();
            imp::Extracted out;
            std::string error;
            t.IsTrue(imp::extractRenames(base, base, true, "f.max", "my memory card", out, &error), error);
            t.IsTrue(out.empty(), "nothing to carry");
            const std::string want = "{\n \"format\": \"fpr-rename\",\n \"version\": 1,\n \"game\": \"SLUS-21702\",\n \"base_sha1\": \"" + sha1Hex(base) +
                                     "\",\n \"wrestlers\": {},\n \"organisations\": {},\n \"groups\": {},\n \"appearance\": {},\n"
                                     " \"source\": {\"file\": \"f.max\", \"base\": \"my memory card\"}\n}\n";
            t.Equals(out.json, want, "the text");
            imp::Extracted without;
            t.IsTrue(imp::extractRenames(base, base, false, "f.max", "b", without, &error), error);
            t.IsTrue(!contains(without.json, "appearance"), "no appearance section without the flag");
            t.IsFalse(imp::extractRenames(Bytes(100), base, true, "f", "b", out, &error), "a save of the wrong size is refused");
        });

        tc.Run("renames, a changed group, a created organisation and group, and an appearance record", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Bytes save = base;
            renameFirst(save, 3, "Zed");
            save[kOffNames + 3 * kNameStride] ^= 0x80;                                      // flip toggled
            put(save, kOffNames + 4 * kNameStride + 15, textBytes("Hex", kWidthLast));
            Bytes digits(kWidthNickname, 0);
            digits[0] = '2';
            digits[1] = '0';
            digits[2] = '0';
            put(save, kOffNames + 4 * kNameStride + 33, digits);                              // half-width digits: {"hex": ...}
            save[kOffNames + 4 * kNameStride] = 0x02 | 0x04;                                // space 2 and a flag bit the format cannot name
            renameOrg(save, 1, "NewOrg", "NO");
            put(save, kOffGroups + 2 * kGroupStride + 2, textBytes("NewGroup", kWidthOrgLong));
            putU16(save, kOffGroups + 2 * kGroupStride + 42, 3); // moved to organisation 3
            // a created organisation 7 and group 9
            putU16(save, kOffOrgs + 7 * kOrgStride, 1);
            putU16(save, kOffOrgs + 7 * kOrgStride + 42, 0xFFFF);
            renameOrg(save, 7, "Made", "MD");
            putU16(save, kOffGroups + 9 * kGroupStride, 1);
            putU16(save, kOffGroups + 9 * kGroupStride + 42, 7);
            putU16(save, kOffGroups + 9 * kGroupStride + 44, 2);
            put(save, kOffGroups + 9 * kGroupStride + 2, textBytes("MadeGroup", kWidthOrgLong));
            put(save, kOffGroups + 9 * kGroupStride + 28, textBytes("MG", kWidthOrgShort));
            // the list order has the new records last, as the rule says: nothing to write
            setOrder(save, kOffOrgOrder, kOrgs, {0, 1, 2, 3, 7});
            setOrder(save, kOffGroupOrder, kGroups, {0, 1, 2, 3, 4, 5, 9});
            for (size_t i = 0; i < kAppearanceStride; ++i)
                save[kOffAppearance + 5 * kAppearanceStride + i] = static_cast<uint8_t>(i * 7);
            fixIntegrity(save, testTable());

            imp::Extracted out;
            std::string error;
            t.IsTrue(imp::extractRenames(save, base, true, "my save.psu", "my memory card", out, &error), error);
            t.Equals(out.counts.wrestlers, static_cast<size_t>(2), "two wrestlers");
            t.Equals(out.counts.organisations, static_cast<size_t>(2), "an organisation renamed and one created");
            t.Equals(out.counts.groups, static_cast<size_t>(2), "a group changed and one created");
            t.Equals(out.counts.appearance, static_cast<size_t>(1), "one appearance record");
            t.IsFalse(out.counts.orderLists, "the plain rule gives the list order, so none is written");
            t.IsTrue(contains(out.json, "\"3\": {\"first\": \"Zed\", \"last\": \"Last3\", \"nickname\": \"Nick3\", \"flip\": true, \"space\": 0}"), "wrestler 3: " + out.json);
            t.IsTrue(contains(out.json, "\"4\": {\"first\": \"First4\", \"last\": \"Hex\", \"nickname\": {\"hex\": \"323030\"}, \"flip\": false, \"space\": 2, \"flags_raw\": 6}"),
                     "wrestler 4 (hex field, the flags byte kept whole): " + out.json);
            t.IsTrue(contains(out.json, "\"1\": {\"name\": \"NewOrg\", \"short\": \"NO\"}"), "organisation 1");
            t.IsTrue(contains(out.json, "\"7\": {\"name\": \"Made\", \"short\": \"MD\", \"created\": true}"), "organisation 7 created");
            t.IsTrue(contains(out.json, "\"2\": {\"name\": \"NewGroup\", \"short\": \"G2\", \"org\": 3}"), "group 2 moved");
            t.IsTrue(contains(out.json, "\"9\": {\"name\": \"MadeGroup\", \"short\": \"MG\", \"created\": true, \"org\": 7, \"kind\": 2}"), "group 9 created");
            t.IsTrue(contains(out.json, " \"source\": {\"file\": \"my save.psu\", \"base\": \"my memory card\"}\n}\n"), "the source is last");
            // the JSON is the pack's own format, and applying it to the base gives the save in every part a pack owns
            RenamePack pack;
            t.IsTrue(parseRenamePack(out.json, pack, &error), "the runtime parses it: " + error);
            imp::Unmatched left;
            t.IsTrue(imp::unmatchedChanges(save, base, out.json, left, &error), error);
            t.IsFalse(left.any(), "nothing is left over: " + (left.areas.empty() ? std::string() : left.areas.front()));
            // a different list order is written
            Bytes ordered = save;
            setOrder(ordered, kOffOrgOrder, kOrgs, {3, 1, 2, 0, 7});
            fixIntegrity(ordered, testTable());
            imp::Extracted again;
            t.IsTrue(imp::extractRenames(ordered, base, true, "f", "b", again, &error), error);
            t.IsTrue(again.counts.orderLists && contains(again.json, " \"organisation_order\": [3, 1, 2, 0, 7]"), "a changed order: " + again.json.substr(0, 400));
            t.IsTrue(imp::unmatchedChanges(ordered, base, again.json, left, &error) && !left.any(), "and it applies");
        });

        tc.Run("an organisation or group deleted in the save is left out with a note, and counts as another change", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Bytes save = base;
            putU16(save, kOffOrgs + 3 * kOrgStride, 0); // organisation 3 deleted (text kept)
            fixIntegrity(save, testTable());
            imp::Extracted out;
            std::string error;
            t.IsTrue(imp::extractRenames(save, base, true, "f", "b", out, &error), error);
            t.IsTrue(out.empty(), "no pack content");
            t.IsTrue(!out.leftOut.empty() && contains(out.leftOut.front(), "organisation 3 is in use in the base but deleted"), "the note");
            imp::Unmatched left;
            t.IsTrue(imp::unmatchedChanges(save, base, out.json, left, &error) && left.any() && contains(left.areas.front(), "organisation"), "an unmatched change: " + (left.areas.empty() ? std::string() : left.areas.front()));
        });
    });

    // ---------------------------------------------------------------------------------------------- what a pack cannot carry
    MiniTest::Case("SaveImportUnmatched", [](TestCase &tc)
    {
        auto check = [](TestCase &t, const Bytes &base, const Bytes &save, const std::string &expectArea, const std::string &what) {
            imp::Extracted out;
            std::string error;
            t.IsTrue(imp::extractRenames(save, base, true, "f", "b", out, &error), what + ": " + error);
            imp::Unmatched left;
            t.IsTrue(imp::unmatchedChanges(save, base, out.json, left, &error), what + ": " + error);
            if (expectArea.empty())
            {
                t.IsFalse(left.any(), what + ": nothing counted (found: " + (left.areas.empty() ? std::string() : left.areas.front()) + ")");
                return;
            }
            bool found = false;
            for (const std::string &a : left.areas)
                found = found || contains(a, expectArea);
            t.IsTrue(found, what + ": counted as '" + expectArea + "' (found " + std::to_string(left.areas.size()) + " areas, first: " + (left.areas.empty() ? std::string() : left.areas.front()) + ")");
        };
        tc.Run("what is not counted", [check](TestCase &t)
        {
            const Bytes base = makeSave();
            Bytes s = base;
            s[0] = 0x99;
            s[0x1234] ^= 0xFF;
            s[0x5000] = 0x12; // header, saved screen state and options
            check(t, base, fixed(s), "", "header and settings bytes");
            s = base;
            const size_t name = kOffNames + 10 * kNameStride;
            put(s, name + 15 + 13, Bytes{0x41, 0x42, 0x43}); // garbage after the LAST field's terminator
            check(t, base, fixed(s), "", "text after a name's terminator");
            s = base;
            put(s, kOffOrgs + 30 * kOrgStride + 2, textBytes("ghost", kWidthOrgLong)); // a record not in use in either
            put(s, kOffGroups + 40 * kGroupStride + 2, textBytes("ghost", kWidthOrgLong));
            s[kOffOrgOrder + 45] = 9; // the tail of a list order, past the records in use
            check(t, base, fixed(s), "", "deleted records and the tail of a list order");
            s = base;
            renameFirst(s, 8, "Pete");
            renameOrg(s, 2, "Renamed", "RN");
            check(t, base, fixed(s), "", "ordinary renames");
        });
        tc.Run("what is counted", [check](TestCase &t)
        {
            const Bytes base = makeSave();
            Bytes s = base;
            s[kOffEditSlots + 3 * kEditStride + 10] = 1;
            s[kOffEditSlots + 9 * kEditStride] = 1;
            check(t, base, fixed(s), "2 created wrestlers (edit slots)", "edit slots");
            s = base;
            putU16(s, kOffWrestlerGroupTable + 2 * 20, 5);
            check(t, base, fixed(s), "1 wrestler group", "group membership");
            s = base;
            s[kOffReferees + 100] = 1;
            check(t, base, fixed(s), "referees", "referees");
            s = base;
            s[kOffRings + 5] = 1;
            check(t, base, fixed(s), "rings and other tables", "the tables after the appearance records");
            s = base;
            s[kOffEditSlots + 500 * kEditStride + 0x20] = 1;
            check(t, base, fixed(s), "other records", "the six further records");
            s = base;
            s[kRoster + 6] = 1;
            check(t, base, fixed(s), "roster tables", "the roster sort order");
            s = base;
            putU16(s, kOffGroups + 1 * kGroupStride, 0); // group 1 deleted
            check(t, base, fixed(s), "group", "a group deleted in the save");
            s = base;
            putU16(s, kOffOrgs + 1 * kOrgStride, 2); // the in-use flag changed from 1 to 2: not something a rename set says
            check(t, base, fixed(s), "organisation", "an organisation flag a pack cannot write");
        });
    });

    // --------------------------------------------------------------------------------------------------------- the gate
    MiniTest::Case("SaveImportGate", [](TestCase &tc)
    {
        tc.Run("size, integrity and version markers", [](TestCase &t)
        {
            const Bytes good = makeSave();
            t.IsTrue(imp::checkSave(entryOf("BASLUS-21702", good), testTable()).ok, "a good save passes");
            imp::GateResult r = imp::checkSave(entryOf("BASLUS-21702", Bytes(1000)), testTable());
            t.IsFalse(r.ok, "wrong size");
            t.Equals(r.line, std::string("The save in BASLUS-21702 is 1,000 bytes; a Fire Pro Wrestling Returns save is 913,892 bytes."), "its line");
            r = imp::checkSave(entryOf("", Bytes()), testTable());
            t.IsTrue(!r.ok && oneLine(r.line) && contains(r.line, "no save data"), "empty: " + r.line);
            SaveEntry damagedFolder = entryOf("BASLUS-21702", Bytes());
            damagedFolder.problem = "The folder BASLUS-21702 on that card is damaged.";
            r = imp::checkSave(damagedFolder, testTable());
            t.IsTrue(!r.ok && r.line == damagedFolder.problem, "the reader's own line for a damaged folder is passed on: " + r.line);
            Bytes bad = good;
            bad[0x2000] ^= 1;
            r = imp::checkSave(entryOf("BISLPM-66082", bad), testTable());
            t.IsTrue(!r.ok && oneLine(r.line) && contains(r.line, "The save in BISLPM-66082 does not pass the game's own checks (1 of 888 blocks are wrong)"), "one bad block: " + r.line);
            Bytes parity = good;
            parity[kParityOff + 5] ^= 1;
            r = imp::checkSave(entryOf("", parity), testTable());
            t.IsTrue(!r.ok && contains(r.line, "The save does not pass") && contains(r.line, "parity"), "bad parity: " + r.line);
            Bytes old = good;
            putU32(old, kRosterMarkerOff, 0x2E1B7);
            fixIntegrity(old, testTable());
            r = imp::checkSave(entryOf("BESLES-55041", old), testTable());
            t.IsTrue(!r.ok && oneLine(r.line) && contains(r.line, "ERROR OLD VERSION SAVEDATA"), "old version marker: " + r.line);
            // another checksum table: nothing verifies
            CrcTable other = testTable();
            other[1] ^= 0xFFFF;
            t.IsFalse(imp::checkSave(entryOf("BASLUS-21702", good), other).ok, "a save made with another table fails");
        });
    });

    // ---------------------------------------------------------------------------------------------------- the whole import
    MiniTest::Case("SaveImportPipeline", [](TestCase &tc)
    {
        tc.Run("a save becomes an installed, switched-on data pack with a report", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Rig rig("pipeline_basic", &base);
            Bytes save = base;
            renameFirst(save, 3, "Zed");
            renameFirst(save, 4, "Yan");
            renameOrg(save, 1, "NewOrg", "NO");
            fixIntegrity(save, testTable());
            const imp::Outcome o = imp::importEntries(rig.env(), {entryOf("BASLUS-21702", save)}, (fs::path("somewhere") / "Fire Pro US Rename.max").string(), "");
            t.IsTrue(o.ok && o.turnedOn && !o.needsChoice, "imported: " + o.message);
            t.Equals(o.message,
                     std::string("Imported 2 wrestlers, 1 organisation. Restart the game to use it."),
                     "the report line");
            t.Equals(o.modKey, std::string("import.fire-pro-us-rename"), "the id");
            t.Equals(o.counts.wrestlers, static_cast<size_t>(2), "counts");
            t.IsFalse(o.otherChanges, "no other changes");
            const auto info = rig.manager.find("import.fire-pro-us-rename");
            t.IsTrue(info && info->valid && info->enabled && info->active && info->kind == Kind::Data, "the mod is installed, on and active");
            t.IsTrue(info && info->name == "Fire Pro US Rename" && info->version == "1.0.0", "its name comes from the file name");
            t.IsTrue(rig.manager.restartNeeded(), "the Mods tab will say to restart");
            std::string why;
            const auto renames = rig.manager.readFile("import.fire-pro-us-rename", "data/renames.json", 16 << 20, &why);
            imp::Extracted expected;
            t.IsTrue(imp::extractRenames(save, base, true, "Fire Pro US Rename.max", "my memory card", expected, &why), why);
            t.IsTrue(renames && std::string(renames->begin(), renames->end()) == expected.json, "data/renames.json is the extracted set");
            // the scratch folder is gone, and the pack does what it says when the profile is built
            std::error_code ec;
            t.IsFalse(fs::exists(rig.scratch, ec), "the scratch folder is cleaned up");
            profile::Config cfg;
            cfg.profileDir = rig.profile;
            cfg.baseCard0 = rig.base0;
            cfg.baseCard1 = rig.root / "mc1";
            cfg.crcTable = &testTable();
            profile::PackInput pack;
            pack.id = "import.fire-pro-us-rename";
            pack.version = "1.0.0";
            pack.hasFile = true;
            pack.renamesJson = renames ? std::string(renames->begin(), renames->end()) : std::string();
            cfg.packs.push_back(pack);
            const profile::Result built = profile::prepare(cfg);
            t.IsTrue(built.applied, "the profile builds: " + built.problem);
            const Bytes made = readAll(rig.profile / "mc0" / kSaveName / kSaveName);
            imp::Unmatched left;
            t.IsTrue(imp::unmatchedChanges(save, made, "{\"format\": \"fpr-rename\", \"version\": 1}", left, &why) && !left.any(), "the profile card now has the imported names");
            t.IsTrue(checkIntegrity(made, testTable()).allOk(), "and a valid save");
        });

        tc.Run("a second import of the same file gets its own id", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Rig rig("pipeline_twice", &base);
            Bytes save = base;
            renameFirst(save, 3, "Zed");
            fixIntegrity(save, testTable());
            const imp::Outcome a = imp::importEntries(rig.env(), {entryOf("BASLUS-21702", save)}, "same.max", "");
            const imp::Outcome b = imp::importEntries(rig.env(), {entryOf("BASLUS-21702", save)}, "same.max", "");
            t.IsTrue(a.ok && b.ok, a.message + " / " + b.message);
            t.Equals(a.modKey, std::string("import.same"), "first id");
            t.Equals(b.modKey, std::string("import.same-2"), "second id");
            const auto first = rig.manager.find("import.same"), second = rig.manager.find("import.same-2");
            t.IsTrue(first && second && first->name == "same" && second->name == "same (2)", "and a name that tells the two apart");
            t.Equals(rig.manager.list().size(), static_cast<size_t>(2), "two mods");
        });

        tc.Run("the comparison base is the profile card without the packs; without a profile it is the base card", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Rig rig("pipeline_profile", &base);
            // pack A on the profile renames wrestler 0; the player renames wrestler 5 in the game
            const std::string packA = "{\"format\": \"fpr-rename\", \"version\": 1, \"wrestlers\": {\"0\": {\"first\": \"PackA\"}}}";
            profile::Config cfg;
            cfg.profileDir = rig.profile;
            cfg.baseCard0 = rig.base0;
            cfg.baseCard1 = rig.root / "mc1";
            cfg.crcTable = &testTable();
            profile::PackInput pa;
            pa.id = "pack.a";
            pa.version = "1.0.0";
            pa.hasFile = true;
            pa.renamesJson = packA;
            cfg.packs.push_back(pa);
            t.IsTrue(profile::prepare(cfg).applied, "profile built");
            Bytes p = readAll(rig.profile / "mc0" / kSaveName / kSaveName);
            renameFirst(p, 5, "Player");
            fixIntegrity(p, testTable());
            writeAll(rig.profile / "mc0" / kSaveName / kSaveName, p);

            // the community save: the default names, with wrestler 1 and 5 renamed
            Bytes save = base;
            renameFirst(save, 1, "Comm");
            renameFirst(save, 5, "Comm5");
            fixIntegrity(save, testTable());
            const imp::Outcome o = imp::importEntries(rig.env(), {entryOf("BASLUS-21702", save)}, "comm.psu", "");
            t.IsTrue(o.ok, o.message);
            std::string why;
            const auto renames = rig.manager.readFile(o.modKey, "data/renames.json", 16 << 20, &why);
            const std::string text = renames ? std::string(renames->begin(), renames->end()) : std::string();
            t.IsTrue(contains(text, "\"1\": {\"first\": \"Comm\"") && contains(text, "\"5\": {\"first\": \"Comm5\""), "wrestlers 1 and 5 are in the pack: " + text);
            t.IsFalse(contains(text, "\"0\": {"), "wrestler 0 is not: the profile's pack A is taken back out of the comparison");
            t.Equals(o.counts.wrestlers, static_cast<size_t>(2), "two wrestlers");
            // the same save with no profile compares with the base card
            Rig plain("pipeline_noprofile", &base);
            const imp::Outcome o2 = imp::importEntries(plain.env(), {entryOf("BASLUS-21702", save)}, "comm.psu", "");
            t.IsTrue(o2.ok && o2.counts.wrestlers == 2, o2.message);
            // a profile whose save does not pass the checks is not used for the comparison
            Rig broken("pipeline_badprofile", &base);
            Bytes badProfile = base;
            badProfile[0x3000] ^= 1;
            writeAll(broken.profile / "mc0" / kSaveName / kSaveName, badProfile);
            const imp::Outcome o3 = imp::importEntries(broken.env(), {entryOf("BASLUS-21702", save)}, "comm.psu", "");
            t.IsTrue(!o3.ok && oneLine(o3.message) && contains(o3.message, "profile card does not pass"), "refused: " + o3.message);
        });

        tc.Run("every refusal has its one line", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Bytes save = base;
            renameFirst(save, 3, "Zed");
            fixIntegrity(save, testTable());
            {
                Rig none("refuse_nosave");
                const imp::Outcome o = imp::importEntries(none.env(), {entryOf("BASLUS-21702", save)}, "a.max", "");
                t.IsTrue(!o.ok && oneLine(o.message), "no save anywhere: one line");
                t.Equals(o.message, std::string("There is no save on your memory card to compare with. Save once in the game, then import."), "no save on either card");
                t.Equals(none.manager.list().size(), static_cast<size_t>(0), "nothing was installed");
            }
            Rig rig("refuse_all", &base);
            auto line = [&](const std::vector<SaveEntry> &entries, const char *what) {
                const imp::Outcome o = imp::importEntries(rig.env(), entries, "a.max", "");
                t.IsTrue(!o.ok && !o.needsChoice && oneLine(o.message) && !o.message.empty(), std::string(what) + ": " + o.message);
                return o.message;
            };
            t.Equals(line({}, "no saves"), std::string("There is no save in that file."), "an empty container");
            t.IsTrue(contains(line({entryOf("BASCUS-94163", Bytes(10))}, "another game"), "has no Fire Pro Wrestling Returns save") &&
                         contains(line({entryOf("BASCUS-94163", Bytes(10)), entryOf("BASLES-12345", Bytes(10))}, "two others"), "BASCUS-94163, BASLES-12345"),
                     "another game's folder is named");
            t.IsTrue(contains(line({entryOf("BASLUS-21702", Bytes(5000))}, "wrong size"), "5,000 bytes"), "the size line");
            Bytes bad = save;
            bad[0x4000] ^= 1;
            t.IsTrue(contains(line({entryOf("BASLUS-21702", bad)}, "damaged"), "does not pass the game's own checks"), "the integrity line");
            // a table that cannot be read
            imp::Env env = rig.env();
            env.crcTable = nullptr;
            env.loadCrcTable = [](CrcTable &, std::string *why) {
                *why = "no executable";
                return false;
            };
            const imp::Outcome o = imp::importEntries(env, {entryOf("BASLUS-21702", save)}, "a.max", "");
            t.IsTrue(!o.ok && contains(o.message, "checksum table could not be read (no executable)"), o.message);
            // no manager
            imp::Env nomgr = rig.env();
            nomgr.manager = nullptr;
            t.IsTrue(!imp::importEntries(nomgr, {entryOf("BASLUS-21702", save)}, "a.max", "").ok, "no manager: refused");
            // a base card save that is broken
            Bytes wrongBase = base;
            wrongBase[0x100] ^= 1;
            writeAll(rig.base0 / kSaveName / kSaveName, wrongBase);
            t.IsTrue(contains(line({entryOf("BASLUS-21702", save)}, "base broken"), "memory card does not pass the game's own checks"), "a broken base save");
            writeAll(rig.base0 / kSaveName / kSaveName, Bytes(100));
            t.IsTrue(contains(line({entryOf("BASLUS-21702", save)}, "base wrong size"), "is not a Fire Pro Wrestling Returns save (100 bytes)"), "a base of the wrong size");
            t.Equals(rig.manager.list().size(), static_cast<size_t>(0), "no refusal installed anything");
        });

        tc.Run("a save equal to the player's own has nothing to import; other changes are reported", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Rig rig("pipeline_nothing", &base);
            imp::Outcome o = imp::importEntries(rig.env(), {entryOf("BASLUS-21702", base)}, "same.max", "");
            t.IsTrue(!o.ok && o.nothingToImport && oneLine(o.message) && contains(o.message, "Nothing to import"), o.message);
            t.IsFalse(contains(o.message, "other changes"), "no other changes in it");
            // header and settings differences alone do not count
            Bytes header = base;
            header[0] = 0x44;
            header[0x2222] = 9;
            fixIntegrity(header, testTable());
            o = imp::importEntries(rig.env(), {entryOf("BASLUS-21702", header)}, "same.max", "");
            t.IsTrue(o.nothingToImport && !o.otherChanges, "play state is not a change: " + o.message);
            // only created wrestlers differ: nothing to import, but the report says why
            Bytes edits = base;
            edits[kOffEditSlots + 40 * kEditStride + 3] = 1;
            fixIntegrity(edits, testTable());
            o = imp::importEntries(rig.env(), {entryOf("BASLUS-21702", edits)}, "edits.max", "");
            t.IsTrue(o.nothingToImport && o.otherChanges && contains(o.message, "This save also has other changes that packs cannot carry yet."), o.message);
            // names and edit slots: both
            Bytes both = edits;
            renameFirst(both, 2, "Both");
            fixIntegrity(both, testTable());
            o = imp::importEntries(rig.env(), {entryOf("BASLUS-21702", both)}, "both.max", "");
            t.IsTrue(o.ok && o.otherChanges && o.message.size() > 20 &&
                         o.message.find("This save also has other changes that packs cannot carry yet.") == o.message.size() - std::string("This save also has other changes that packs cannot carry yet.").size(),
                     o.message);
            t.IsTrue(!o.unmatched.areas.empty() && contains(o.unmatched.areas.front(), "created wrestler"), "and the log names it");
        });

        tc.Run("the Japanese and European folders are accepted with a note; a card with several saves asks", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Rig rig("pipeline_regions", &base);
            Bytes jp = base;
            renameFirst(jp, 3, "Nihon");
            fixIntegrity(jp, testTable());
            imp::Outcome o = imp::importEntries(rig.env(), {entryOf("BISLPM-66082", jp)}, "jp.psu", "");
            t.IsTrue(o.ok && o.region == "Japan" && contains(o.message, "from a Japanese save") && o.notes.size() == 1 && contains(o.notes[0], "only the US release's layout has been checked"), o.message);
            Bytes eu = base;
            renameFirst(eu, 3, "Europa");
            fixIntegrity(eu, testTable());
            o = imp::importEntries(rig.env(), {entryOf("BESLES-55041", eu)}, "eu.psu", "");
            t.IsTrue(o.ok && contains(o.message, "from a European save"), o.message);
            // two different saves in two folders: the player is asked; the same save under two names is one save
            const size_t installedBefore = rig.manager.list().size();
            o = imp::importEntries(rig.env(), {entryOf("BASLUS-21702", eu), entryOf("BISLPM-66082", jp), entryOf("BASCUS-94163", Bytes(4))}, "card.ps2", "");
            t.IsTrue(!o.ok && o.needsChoice && o.choices.size() == 2 && oneLine(o.message), o.message);
            t.IsTrue(o.choices.size() == 2 && o.choices[0].folder == "BASLUS-21702" && o.choices[0].region == "USA" && o.choices[1].folder == "BISLPM-66082" && o.choices[1].region == "Japan",
                     "the choices are the saves of the game, in directory order");
            t.IsTrue(contains(o.message, "BASLUS-21702, BISLPM-66082"), o.message);
            t.Equals(rig.manager.list().size(), installedBefore, "asking installs nothing");
            o = imp::importEntries(rig.env(), {entryOf("BASLUS-21702", eu), entryOf("BISLPM-66082", jp)}, "card.ps2", "BISLPM-66082");
            t.IsTrue(o.ok && o.region == "Japan" && o.counts.wrestlers == 1, "choosing the Japanese one: " + o.message);
            std::string why;
            const auto renames = rig.manager.readFile(o.modKey, "data/renames.json", 16 << 20, &why);
            t.IsTrue(renames && contains(std::string(renames->begin(), renames->end()), "Nihon"), "and it is that save's names");
            o = imp::importEntries(rig.env(), {entryOf("BASLUS-21702", eu), entryOf("BISLPM-66082", jp)}, "card.ps2", "BESLES-55041");
            t.IsTrue(!o.ok && contains(o.message, "no longer holds a save in BESLES-55041"), "a choice that is not there: " + o.message);
            o = imp::importEntries(rig.env(), {entryOf("BASLUS-21702", eu), entryOf("BISLPM-66082", eu)}, "card.ps2", "");
            t.IsTrue(o.ok && !o.needsChoice, "the same save under two folder names is one save: " + o.message);
            // one of two is damaged: the other is taken without asking
            Bytes damaged = jp;
            damaged[0x700] ^= 1;
            o = imp::importEntries(rig.env(), {entryOf("BISLPM-66082", damaged), entryOf("BASLUS-21702", eu)}, "card.ps2", "");
            t.IsTrue(o.ok && !o.needsChoice, "a damaged save is not a choice: " + o.message);
            // a raw save with no folder name is accepted
            o = imp::importEntries(rig.env(), {entryOf("", jp, "raw")}, "BASLUS-21702", "");
            t.IsTrue(o.ok, "a bare save file: " + o.message);
        });
    });

    // -------------------------------------------------------------------------------------------- the player's own files
    MiniTest::Case("SaveImportRealData", [](TestCase &tc)
    {
        tc.Run("the rename set is byte-identical to tools/fprsave.py renames, and mkmod's files too", [](TestCase &t)
        {
            const fs::path root = findRepoRoot();
            if (root.empty() || !runCommand("python -I -c \"pass\""))
            {
                std::printf("(skipped: needs baserom/ with the saves and python on the path) ");
                return;
            }
            const fs::path saves = root / "baserom" / "saves";
            const fs::path work = tempDir("real_extract");
            const fs::path baseDir = saves / "001_baseline";
            const Bytes base = readAll(baseDir / kSaveName);
            t.Equals(base.size(), kSaveSize, "the default save is there");
            const std::string tool = "python -I " + q(root / "tools" / "fprsave.py") + " ";
            struct Pair
            {
                fs::path save;
                const char *what;
            };
            std::vector<Pair> pairs = {{saves / "community" / "gamefaqs-16161-us-rename", "the community rename set"},
                                       {saves / "005_style_1change", "one style change"},
                                       {saves / "008_style_mma", "another style change"},
                                       {saves / "004_name_1charchange", "one character of a name"},
                                       {saves / "recomp_003_name_flip", "a recomp save with a flipped name"},
                                       {root / "baserom" / "out" / "test" / "s1_proofs" / "created_org_group", "a created organisation and group"}};
            size_t compared = 0;
            for (const Pair &pair : pairs)
            {
                std::error_code ec;
                if (!fs::exists(pair.save, ec))
                    continue;
                // a folder of the card, or the folder holding the file: the same lookup the tool does
                fs::path file = pair.save / kSaveName / kSaveName;
                if (!fs::exists(file, ec))
                    file = pair.save / kSaveName;
                if (!fs::exists(file, ec) || fs::is_directory(file, ec))
                    continue;
                const Bytes save = readAll(file);
                for (const bool withAppearance : {false, true})
                {
                    const fs::path out = work / ("p" + std::to_string(compared) + (withAppearance ? "a" : "n") + ".json");
                    const bool ran = runCommand(tool + "renames " + q(pair.save) + " --base " + q(baseDir) + " --out " + q(out) + (withAppearance ? " --appearance" : ""));
                    t.IsTrue(ran, std::string("fprsave.py renames: ") + pair.what);
                    imp::Extracted got;
                    std::string error;
                    t.IsTrue(imp::extractRenames(save, base, withAppearance, pair.save.filename().string(), baseDir.filename().string(), got, &error), error);
                    t.IsTrue(got.json == readText(out), std::string("byte for byte equal to fprsave.py renames") + (withAppearance ? " --appearance: " : ": ") + pair.what);
                    ++compared;
                }
            }
            t.IsTrue(compared >= 4, "enough pairs were found to compare (" + std::to_string(compared) + ")");
            std::printf("[%zu rename sets compared] ", compared);
        });

        tc.Run("importing each container gives the pack fprsave.py renames --appearance and mkmod write (mod.toml and data/renames.json byte for byte)", [](TestCase &t)
        {
            const fs::path root = findRepoRoot();
            if (root.empty() || !runCommand("python -I -c \"pass\""))
            {
                std::printf("(skipped: needs baserom/ with the saves and python on the path) ");
                return;
            }
            const fs::path saves = root / "baserom" / "saves";
            const fs::path proofs = root / "baserom" / "out" / "test" / "s4_proofs" / "containers";
            const fs::path work = tempDir("real_import");
            CrcTable table{};
            std::string error;
            t.IsTrue(crcTableFromElf(root / "baserom" / "iso" / "SLUS_217.02", table, &error), error);
            const Bytes base = readAll(saves / "001_baseline" / kSaveName);
            // the card the player already has, as Python must see it: a folder named like the importer's "source.base" text
            const fs::path pyBase = work / "my memory card";
            writeAll(pyBase / kSaveName / kSaveName, base);
            const std::string tool = "python -I " + q(root / "tools" / "fprsave.py") + " ";

            std::vector<fs::path> files = {saves / "community" / "gamefaqs-16161-us-rename" / "fire-pro-wrestling-returns.16161.max"};
            for (const char *name : {"comm.psu", "comm.max", "comm.cbs", "comm.sps", "comm.xps", "comm.psv", "comm_card.ps2", "BASLUS-21702", "folder"})
                files.push_back(proofs / name);
            size_t imported = 0;
            for (const fs::path &file : files)
            {
                std::error_code ec;
                if (!fs::exists(file, ec))
                    continue;
                Rig rig((std::string("real_import_") + std::to_string(imported)).c_str(), &base);
                imp::Env env = rig.env();
                env.crcTable = &table;
                const imp::Outcome o = imp::importFile(env, file);
                const std::string what = file.filename().string();
                t.IsTrue(o.ok && o.turnedOn && o.counts.wrestlers == 327, what + ": imported: " + o.message);
                if (!o.ok)
                    continue;
                const auto info = rig.manager.find(o.modKey);
                t.IsTrue(info && info->active && info->kind == Kind::Data, what + ": installed, on and a data pack");
                if (!info)
                    continue;
                const auto toml = readPackageEntry(info->path, "mod.toml", 1 << 20, &error);
                const auto renames = readPackageEntry(info->path, "data/renames.json", 16 << 20, &error);
                t.IsTrue(toml && renames, what + ": the package holds both files: " + error);
                if (!toml || !renames)
                    continue;
                const fs::path out = work / ("py_" + std::to_string(imported));
                const bool ran = runCommand(tool + "mkmod " + q(file) + " --base " + q(pyBase) + " --appearance --id " + o.modKey + " --name " + q(fs::path(info->name)) +
                                            " --author " + q(fs::path(info->authors)) + " --version " + info->version + " --description " + q(fs::path(info->description)) +
                                            " --out " + q(out));
                t.IsTrue(ran, what + ": fprsave.py mkmod ran");
                t.IsTrue(std::string(renames->begin(), renames->end()) == readText(out / "data" / "renames.json"), what + ": data/renames.json equals fprsave.py mkmod --appearance");
                t.IsTrue(std::string(toml->begin(), toml->end()) == readText(out / "mod.toml"), what + ": mod.toml equals fprsave.py mkmod");
                // and `renames --appearance` writes the same document as mkmod's data/renames.json
                const fs::path alone = work / ("renames_" + std::to_string(imported) + ".json");
                t.IsTrue(runCommand(tool + "renames " + q(file) + " --base " + q(pyBase) + " --out " + q(alone) + " --appearance") && readText(alone) == readText(out / "data" / "renames.json"),
                         what + ": fprsave.py renames --appearance writes the same text");
                ++imported;
            }
            t.IsTrue(imported >= 1, "at least the community .max was imported (" + std::to_string(imported) + ")");
            std::printf("[%zu containers imported and compared] ", imported);
        });

        tc.Run("the text of every name field is what Python decides, for every character and for random fields", [](TestCase &t)
        {
            const fs::path root = findRepoRoot();
            if (root.empty() || !runCommand("python -I -c \"pass\""))
            {
                std::printf("(skipped: needs python on the path) ");
                return;
            }
            const fs::path work = tempDir("real_fieldvalue");
            std::vector<Bytes> fields;
            // every character the page can encode, alone and between two letters
            for (uint32_t cp = 0x80; cp < 0x10000; ++cp)
            {
                if (cp >= 0xD800 && cp <= 0xDFFF)
                    continue;
                Bytes b;
                if (!encodeGameText({cp}, b))
                    continue;
                fields.push_back(b);
                Bytes around = bytesOf({0x82, 0x60});
                around.insert(around.end(), b.begin(), b.end());
                around.push_back(0x82);
                around.push_back(0x61);
                fields.push_back(around);
                if (b.size() == 1)
                {
                    Bytes ascii = bytesOf({0x41});
                    ascii.insert(ascii.end(), b.begin(), b.end());
                    fields.push_back(ascii);
                }
            }
            // random fields: random bytes, and random strings made of legal characters
            std::mt19937 rng(20261110);
            for (int i = 0; i < 3000; ++i)
            {
                Bytes b;
                const int n = 1 + static_cast<int>(rng() % 14);
                for (int k = 0; k < n; ++k)
                    b.push_back(static_cast<uint8_t>(rng() % 256));
                fields.push_back(b);
            }
            const uint32_t pool[] = {0x41, 0x61, 0x20, 0x3042, 0xFF21, 0xFF41, 0xFF9E, 0xFF76, 0x3000, 0xFFE0, 0xFFE2, 0x309B, 0x65E5, 0x2252, 0x2160, 0xFF5E, 0x301C, 0x3231, 0xFF0D, 0x2212};
            for (int i = 0; i < 3000; ++i)
            {
                std::vector<uint32_t> cps;
                const int n = 1 + static_cast<int>(rng() % 6);
                for (int k = 0; k < n; ++k)
                    cps.push_back(pool[rng() % (sizeof(pool) / sizeof(pool[0]))]);
                Bytes b;
                if (encodeGameText(cps, b) && b.size() <= 24)
                    fields.push_back(b);
            }
            std::string input;
            for (const Bytes &b : fields)
                input += hexOf(b) + "\n";
            const fs::path in = work / "fields.txt", out = work / "values.txt";
            writeText(in, input);
            t.IsTrue(runCommandTo("python -I " + q(root / "tools" / "fprsave.py") + " fieldvalue " + q(in), out), "fprsave.py fieldvalue");
            std::istringstream answers(readText(out));
            std::string answer;
            size_t index = 0, mismatches = 0;
            std::string firstBad;
            while (std::getline(answers, answer))
            {
                if (index >= fields.size())
                    break;
                if (!answer.empty() && answer.back() == '\r')
                    answer.pop_back();
                const std::string mine = fieldJson(fields[index]);
                if (mine != answer)
                {
                    if (++mismatches == 1)
                        firstBad = hexOf(fields[index]) + ": C++ " + mine + " / Python " + answer;
                }
                ++index;
            }
            t.Equals(index, fields.size(), "Python answered every field");
            t.Equals(mismatches, static_cast<size_t>(0), "no field differs from Python's decision (first: " + firstBad + ")");
            std::printf("[%zu fields compared] ", fields.size());
        });
    });
}
