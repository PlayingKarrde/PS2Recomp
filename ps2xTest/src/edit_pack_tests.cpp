#include "MiniTest.h"

#include "data_pack.h"
#include "edit_pack.h"
#include "edit_text.h"
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
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Tests for created wrestlers in data packs (task e2): which edit slot each edit file gets (edit_pack.h planSlots), the profile card with
// edits (profile.h: the ledger's rules for slots: add, the player's own wrestlers never touched, later packs, off restores, the player's
// change kept, slot reuse, groups), reading a pack's edit files through the manager, and the importer carrying a save's created wrestlers
// (save_import.h extractEdits, unmatchedChanges, importEntries).
//
// Nothing here contains game data. The saves are synthetic images with a made-up checksum table; the wrestlers are the made-up slot of
// edit_text_tests.cpp.

namespace
{
    namespace fs = std::filesystem;
    using namespace ps2x::mods;
    using namespace ps2x::mods::fpr;
    namespace ed = ps2x::mods::edit;
    namespace imp = ps2x::mods::import;

    // ---- helpers --------------------------------------------------------------------------------------------------------

    fs::path tempDir(const char *name)
    {
        fs::path dir = fs::temp_directory_path() / (std::string("ps2x_editpack_test_") + name);
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

    bool anyContains(const std::vector<std::string> &lines, const std::string &needle)
    {
        return std::any_of(lines.begin(), lines.end(), [&](const std::string &l) { return contains(l, needle); });
    }

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

    // A small valid save: 327 wrestlers with names, 4 organisations and 6 groups in use, identity list orders, and no created wrestlers.
    Bytes makeSave()
    {
        Bytes img(kSaveSize, 0);
        putU32(img, kSettingsMarkerOff, kSettingsMarker);
        putU32(img, kRosterMarkerOff, kRosterMarker);
        for (int k = 0; k < kWrestlers; ++k)
        {
            const uint32_t base = kOffNames + static_cast<uint32_t>(k) * kNameStride;
            put(img, base + 1, textBytes("First" + std::to_string(k), kWidthFirst));
            put(img, base + 15, textBytes("Last" + std::to_string(k), kWidthLast));
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
        // the 827-id list: the built-ins, then all 500 edit slots
        for (int i = 0; i < 327; ++i)
            putU16(img, kRoster + 4 + 2 * static_cast<size_t>(i), i);
        for (int i = 0; i < 500; ++i)
            putU16(img, kRoster + 4 + 2 * (327 + static_cast<size_t>(i)), 0x2000 + i);
        img[0] = 0x05;
        img[0x1234] = 0x77;
        fixIntegrity(img, testTable());
        return img;
    }

    // ---- the made-up wrestler (edit_text_tests.cpp builds the same one) ---------------------------------------------------------------

    Bytes syntheticSlot()
    {
        Bytes slot(ed::kSlotSize, 0);
        putU16(slot, 0, 1);
        const size_t r = ed::kRecordAt;
        slot[r + 0x00] = 1;
        put(slot, r + 0x02, textBytes("Karl", 14));
        put(slot, r + 0x12, textBytes("Gotch", 18));
        put(slot, r + 0x26, textBytes("God", 24));
        putU16(slot, r + 0x42, 150);
        putU16(slot, r + 0x44, 40);
        putU16(slot, r + 0x46, 1900);
        slot[r + 0x48] = 1;
        slot[r + 0x49] = 1;
        putU16(slot, r + 0x4A, 0xFFFF);
        slot[r + 0x5C] = slot[r + 0x5D] = 3;
        for (size_t i = 0; i < 24; ++i)
            slot[r + 0x69 + i] = 3;
        putU16(slot, r + 0x82, 6 | 0x4000 | 0x1000);
        putU16(slot, r + 0x84, 29);
        putU16(slot, r + 0x86, 64 | 0x2000);
        putU16(slot, ed::kBudgetAt, 380);
        slot[ed::kAppearanceAt + 2] = 1;
        slot[ed::kAppearanceAt + 3] = 1;
        return slot;
    }

    Bytes wrestler(const std::string &first, int group = 0xFFFF, const std::string &last = "Gotch")
    {
        Bytes s = syntheticSlot();
        put(s, ed::kRecordAt + 0x02, textBytes(first, 14));
        put(s, ed::kRecordAt + 0x12, textBytes(last, 18));
        ed::setSlotGroup(s, static_cast<uint16_t>(group));
        return s;
    }

    std::string wrestlerText(const std::string &first, int group = 0xFFFF)
    {
        std::string text, error;
        ed::slotToText(wrestler(first, group), nullptr, text, &error);
        return text;
    }

    using EditList = std::vector<std::pair<std::string, std::string>>;

    profile::PackInput pack(const std::string &id, const EditList &edits, const std::string &version = "1.0.0", const std::string &renamesJson = std::string())
    {
        profile::PackInput p;
        p.id = id;
        p.name = id;
        p.version = version;
        if (!renamesJson.empty())
        {
            p.hasFile = true;
            p.renamesJson = renamesJson;
        }
        for (const auto &e : edits)
        {
            profile::EditFile f;
            f.name = e.first;
            f.text = e.second;
            p.edits.push_back(std::move(f));
        }
        return p;
    }

    bool slotUsed(const Bytes &image, int k) { return readU16(image, editSlotOffset(k)) != 0; }

    std::vector<int> usedSlots(const Bytes &image)
    {
        std::vector<int> out;
        for (int k = 0; k < kEditSlotCount; ++k)
            if (slotUsed(image, k))
                out.push_back(k);
        return out;
    }

    Bytes slotAt(const Bytes &image, int k)
    {
        const size_t o = editSlotOffset(k);
        return Bytes(image.begin() + static_cast<std::ptrdiff_t>(o), image.begin() + static_cast<std::ptrdiff_t>(o + kEditSlotSize));
    }

    // The FIRST name of the created wrestler in slot k, as the bytes of the name field up to the first zero, turned into text the simple way.
    bool isNamed(const Bytes &image, int k, const std::string &first)
    {
        const Bytes want = textBytes(first, 14);
        const size_t o = editSlotOffset(k) + ed::kRecordAt + 0x02;
        return std::equal(want.begin(), want.end(), image.begin() + static_cast<std::ptrdiff_t>(o));
    }

    void putSlot(Bytes &image, int k, const Bytes &slot) { put(image, editSlotOffset(k), slot); }

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

    void playerEdit(const fs::path &save, const std::function<void(Bytes &)> &edit)
    {
        Bytes image = readAll(save);
        edit(image);
        fixIntegrity(image, testTable());
        writeAll(save, image);
    }

    // The ledger's entry for one key, as the file holds it ("" when there is none).
    std::string ledgerEntry(const fs::path &ledger, const std::string &key)
    {
        json::Value doc;
        std::string error;
        if (!json::parse(readText(ledger), doc, &error))
            return "unreadable";
        const json::Value *fields = doc.find("fields");
        const json::Value *entry = fields ? fields->find(key) : nullptr;
        if (!entry)
            return std::string();
        const json::Value *pack = entry->find("pack");
        const json::Value *file = entry->find("file");
        return (pack ? pack->text : std::string()) + "|" + (file ? file->text : std::string());
    }

    Bytes sameAfterFix(Bytes image)
    {
        fixIntegrity(image, testTable());
        return image;
    }

    fs::path findRepoRoot()
    {
        std::error_code ec;
        fs::path dir = fs::current_path(ec);
        for (int i = 0; i < 8 && !dir.empty(); ++i)
        {
            if (fs::exists(dir / "tools" / "fprsave.py", ec) && fs::exists(dir / "baserom" / "saves" / "001_baseline", ec))
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

    Result installPackage(Manager &m, const fs::path &dir, const std::string &id, const EditList &files, const std::string &kind = "data")
    {
        std::string toml = "id = \"" + id + "\"\nname = \"Pack " + id + "\"\nversion = \"1.0.0\"\nauthors = [\"Tester\"]\ngame = \"SLUS_217.02\"\napi = 1\nkind = \"" + kind + "\"\n";
        std::vector<std::pair<std::string, std::string>> all = {{"mod.toml", toml}};
        all.insert(all.end(), files.begin(), files.end());
        std::string error;
        const auto bytes = buildPackage(all, &error);
        Result r;
        if (!bytes)
        {
            r.message = error;
            return r;
        }
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path file = dir / (id + ".src.fprmod");
        writeBytesToFile(file, *bytes);
        return m.install(file);
    }
}

void register_edit_pack_tests()
{
    // ---------------------------------------------------------------------------------------------------------- the plan
    MiniTest::Case("EditPackSlots", [](TestCase &tc)
    {
        tc.Run("the ledger's field key for a slot: e<slot>, the whole 0x44C bytes", [](TestCase &t)
        {
            FieldRange r;
            t.IsTrue(fieldRange("e0", r) && r.offset == 0x3F398 && r.length == 0x44C && r.mask == 0xFF, "slot 0");
            t.IsTrue(fieldRange("e499", r) && r.offset == 0x3F398 + 499 * 0x44C && r.length == 0x44C, "slot 499");
            t.IsFalse(fieldRange("e500", r), "slot 500 does not exist");
            t.IsFalse(fieldRange("e1.first", r) || fieldRange("e", r) || fieldRange("ex", r), "no parts, a number is needed");
            t.Equals(describeField("e3"), std::string("the created wrestler in edit slot 3"), "what the pack's page calls it");
            t.Equals(editSlotOffset(2), uint32_t(0x3F398 + 2 * 0x44C), "editSlotOffset");
            Bytes image(kSaveSize, 0);
            const Bytes value(0x44C, 0xAB);
            fieldRange("e7", r);
            writeField(image, r, value);
            t.IsTrue(readField(image, r) == value && image[editSlotOffset(7) - 1] == 0 && image[editSlotOffset(8)] == 0, "write and read touch only that slot");
        });

        tc.Run("new edits take the lowest free slots, never a used one", [](TestCase &t)
        {
            std::vector<bool> used(500, false);
            const auto free = ed::planSlots(used, {}, {{"p", "a"}, {"p", "b"}});
            t.IsTrue(free[0].placed && free[0].slot == 0 && free[1].slot == 1 && !free[0].kept, "an empty card: slots 0 and 1");
            used[0] = used[1] = used[3] = true;
            const auto some = ed::planSlots(used, {}, {{"p", "a"}, {"p", "b"}, {"q", "a"}});
            t.IsTrue(some[0].slot == 2 && some[1].slot == 4 && some[2].slot == 5, "around the player's slots 0, 1 and 3: 2, 4, 5");
        });

        tc.Run("an edit keeps the slot the ledger gave it; a slot that was freed is reused", [](TestCase &t)
        {
            std::vector<bool> used(500, false);
            const auto kept = ed::planSlots(used, {{7, "p", "a", false}}, {{"p", "b"}, {"p", "a"}});
            t.IsTrue(kept[1].slot == 7 && !kept[1].kept, "a keeps slot 7");
            t.IsTrue(kept[0].slot == 0, "b, new, takes the lowest free slot, which is not 7's business");
            const auto order = ed::planSlots(used, {{7, "p", "a", false}, {0, "p", "b", false}}, {{"p", "a"}, {"p", "b"}});
            t.IsTrue(order[0].slot == 7 && order[1].slot == 0, "the order of the packs does not move anything");
            const auto reuse = ed::planSlots(used, {{0, "p", "gone", false}}, {{"q", "new"}});
            t.IsTrue(reuse[0].slot == 0, "the slot of an edit that is no longer asked for is free again");
            used[7] = true;
            const auto lost = ed::planSlots(used, {{7, "p", "a", false}}, {{"p", "a"}});
            t.IsTrue(lost[0].placed && lost[0].slot == 0, "if the player now uses the claimed slot, the edit goes to a free one");
        });

        tc.Run("a slot the player changed is held: kept, and nobody else takes it", [](TestCase &t)
        {
            std::vector<bool> used(500, false);
            used[2] = true;
            const auto held = ed::planSlots(used, {{0, "p", "a", true}}, {{"p", "a"}, {"p", "b"}});
            t.IsTrue(held[0].placed && held[0].slot == 0 && held[0].kept, "a is kept in slot 0");
            t.IsTrue(held[1].slot == 1 && !held[1].kept, "b does not take the held slot 0, nor the player's slot 2");
            const auto other = ed::planSlots(used, {{0, "p", "a", true}}, {{"q", "c"}});
            t.IsTrue(other[0].slot == 0, "a held slot of an edit that is not asked for any more is not reserved (it is the player's now, free or not)");
            const auto reserved = ed::planSlots(used, {{0, "p", "a", true}}, {{"p", "a"}, {"q", "c"}});
            t.IsTrue(reserved[1].slot == 1, "and while it is asked for, it is");
        });

        tc.Run("the same file name in two packs is two wrestlers; with no free slot the edit is skipped", [](TestCase &t)
        {
            std::vector<bool> used(500, false);
            const auto two = ed::planSlots(used, {}, {{"p", "x"}, {"q", "x"}});
            t.IsTrue(two[0].slot == 0 && two[1].slot == 1, "pack and file name together make the identity");
            std::fill(used.begin(), used.end(), true);
            used[499] = false;
            const auto full = ed::planSlots(used, {}, {{"p", "a"}, {"p", "b"}});
            t.IsTrue(full[0].placed && full[0].slot == 499 && !full[1].placed, "the last free slot goes to the first edit, the next has no room");
            t.IsTrue(ed::isEditFileName("a.edit.toml") && ed::isEditFileName("A.EDIT.TOML") && !ed::isEditFileName(".edit.toml") && !ed::isEditFileName("a.toml"),
                     "file names");
        });
    });

    // ---------------------------------------------------------------------------------------------------------- the profile card
    MiniTest::Case("EditPackProfile", [](TestCase &tc)
    {
        tc.Run("edit files become created wrestlers in the profile, and the base card is left alone", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("add", &base);
            const profile::Result r = profile::prepare(cards.config({pack("a.edits", {{"b.edit.toml", wrestlerText("Beta")}, {"a.edit.toml", wrestlerText("Alpha")}})}));
            t.IsTrue(r.applied && r.problem.empty(), "applied: " + r.problem);
            t.Equals(r.packs[0].editsAdded, size_t(2), "two created wrestlers");
            t.Equals(r.packs[0].written, size_t(0), "and no renamed fields");
            const Bytes image = readAll(cards.profileSave());
            t.IsTrue(usedSlots(image) == std::vector<int>({0, 1}), "slots 0 and 1");
            t.IsTrue(isNamed(image, 0, "Alpha") && isNamed(image, 1, "Beta"), "in the order of the file names, not of the list");
            t.IsTrue(slotAt(image, 0) == wrestler("Alpha"), "the slot is the text's 1,100 bytes");
            t.IsTrue(checkIntegrity(image, testTable()).allOk(), "the integrity data is valid");
            t.IsTrue(anyContains(r.packs[0].notes, "Adds 2 created wrestlers (edit slots)"), "the pack's page says so");
            t.IsTrue(readAll(cards.base0 / kSaveName / kSaveName) == base, "the base card is not written");
            t.Equals(ledgerEntry(cards.ledger(), "e0"), std::string("a.edits|a.edit.toml"), "the ledger names the pack and the file for slot 0");
            t.Equals(ledgerEntry(cards.ledger(), "e1"), std::string("a.edits|b.edit.toml"), "and for slot 1");
        });

        tc.Run("a second start with the same packs changes nothing", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("again", &base);
            const auto packs = std::vector<profile::PackInput>{pack("a.edits", {{"a.edit.toml", wrestlerText("Alpha")}, {"b.edit.toml", wrestlerText("Beta")}})};
            profile::prepare(cards.config(packs));
            const Bytes first = readAll(cards.profileSave());
            const std::string ledger = readText(cards.ledger());
            const profile::Result again = profile::prepare(cards.config(packs));
            t.IsTrue(again.applied && !again.imageChanged, "the save is not rewritten");
            t.IsTrue(readAll(cards.profileSave()) == first && readText(cards.ledger()) == ledger, "same save, same ledger");
            t.Equals(again.packs[0].editsAdded, size_t(2), "the wrestlers are still the pack's");
        });

        tc.Run("turning the pack off puts the slots back exactly", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("off", &base);
            profile::prepare(cards.config({pack("a.edits", {{"a.edit.toml", wrestlerText("Alpha")}})}));
            const profile::Result off = profile::prepare(cards.config({}));
            t.IsTrue(off.applied, "the profile is still the card");
            t.IsTrue(readAll(cards.profileSave()) == base, "the profile's save is the base card's save again, byte for byte");
            t.Equals(ledgerEntry(cards.ledger(), "e0"), std::string(), "and the ledger forgot the slot");
            const profile::Result on = profile::prepare(cards.config({pack("a.edits", {{"a.edit.toml", wrestlerText("Alpha")}})}));
            t.IsTrue(on.packs[0].editsAdded == 1 && isNamed(readAll(cards.profileSave()), 0, "Alpha"), "on again: the wrestler is back in slot 0");
        });

        tc.Run("the player's own created wrestlers are never touched", [](TestCase &t)
        {
            Bytes base = makeSave();
            putSlot(base, 0, wrestler("Mine"));
            putSlot(base, 2, wrestler("MineToo"));
            fixIntegrity(base, testTable());
            Cards cards("own", &base);
            const profile::Result r = profile::prepare(cards.config({pack("a.edits", {{"a.edit.toml", wrestlerText("Alpha")}, {"b.edit.toml", wrestlerText("Beta")}})}));
            const Bytes image = readAll(cards.profileSave());
            t.IsTrue(usedSlots(image) == std::vector<int>({0, 1, 2, 3}), "the pack's two went to the free slots 1 and 3");
            t.IsTrue(isNamed(image, 0, "Mine") && isNamed(image, 1, "Alpha") && isNamed(image, 2, "MineToo") && isNamed(image, 3, "Beta"), "who is where");
            profile::prepare(cards.config({}));
            t.IsTrue(readAll(cards.profileSave()) == base, "off: only the player's two are left, as they were");
            (void)r;
        });

        tc.Run("two packs: both add, the slots stay with their edits when the order changes, and a freed slot is reused", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("two", &base);
            const auto p1 = pack("one.pack", {{"x.edit.toml", wrestlerText("OneX")}});
            const auto p2 = pack("two.pack", {{"x.edit.toml", wrestlerText("TwoX")}, {"y.edit.toml", wrestlerText("TwoY")}});
            profile::prepare(cards.config({p1, p2}));
            Bytes image = readAll(cards.profileSave());
            t.IsTrue(usedSlots(image) == std::vector<int>({0, 1, 2}) && isNamed(image, 0, "OneX") && isNamed(image, 1, "TwoX") && isNamed(image, 2, "TwoY"),
                     "three wrestlers, the same file name in two packs is two wrestlers");
            profile::prepare(cards.config({p2, p1}));
            image = readAll(cards.profileSave());
            t.IsTrue(isNamed(image, 0, "OneX") && isNamed(image, 1, "TwoX") && isNamed(image, 2, "TwoY"), "the other order of the packs: nobody moves");
            profile::prepare(cards.config({p2}));
            image = readAll(cards.profileSave());
            t.IsTrue(usedSlots(image) == std::vector<int>({1, 2}) && !slotUsed(image, 0), "pack one off: slot 0 is empty, the others stay");
            const auto p3 = pack("three.pack", {{"z.edit.toml", wrestlerText("ThreeZ")}});
            profile::prepare(cards.config({p2, p3}));
            image = readAll(cards.profileSave());
            t.IsTrue(isNamed(image, 0, "ThreeZ"), "a new edit takes the freed slot 0 (the lowest)");
        });

        tc.Run("an updated edit keeps its slot", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("update", &base);
            profile::prepare(cards.config({pack("a.edits", {{"a.edit.toml", wrestlerText("Alpha")}, {"b.edit.toml", wrestlerText("Beta")}}, "1.0.0")}));
            profile::prepare(cards.config({pack("a.edits", {{"a.edit.toml", wrestlerText("Alpha2")}, {"b.edit.toml", wrestlerText("Beta")}}, "1.1.0")}));
            const Bytes image = readAll(cards.profileSave());
            t.IsTrue(isNamed(image, 0, "Alpha2") && isNamed(image, 1, "Beta") && usedSlots(image).size() == 2, "the new text, the same slot, nothing duplicated");
        });

        tc.Run("a wrestler the player changed in the game is kept and noted; one the player deleted stays deleted", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("player", &base);
            const auto edits = pack("a.edits", {{"a.edit.toml", wrestlerText("Alpha")}, {"b.edit.toml", wrestlerText("Beta")}});
            profile::prepare(cards.config({edits}));
            playerEdit(cards.profileSave(), [](Bytes &image) {
                put(image, editSlotOffset(0) + ed::kRecordAt + 0x02, textBytes("Changed", 14)); // slot 0: the player renamed it in the game
                std::fill(image.begin() + static_cast<std::ptrdiff_t>(editSlotOffset(1)), image.begin() + static_cast<std::ptrdiff_t>(editSlotOffset(1) + kEditSlotSize), 0); // slot 1: deleted
            });
            const profile::Result r = profile::prepare(cards.config({edits}));
            Bytes image = readAll(cards.profileSave());
            t.IsTrue(isNamed(image, 0, "Changed") && !slotUsed(image, 1), "the player's version stays, the deleted one is not brought back");
            t.Equals(r.packs[0].editsKept, size_t(2), "both counted as kept");
            t.IsTrue(anyContains(r.packs[0].notes, "Kept your change to the created wrestler in edit slot 0 (a.edit.toml)"), "the pack's page says so for slot 0");
            t.IsTrue(anyContains(r.packs[0].notes, "edit slot 1 (b.edit.toml)"), "and for slot 1");
            const profile::Result third = profile::prepare(cards.config({edits}));
            t.IsFalse(third.imageChanged, "a third start changes nothing");
            const auto more = pack("b.pack", {{"c.edit.toml", wrestlerText("Gamma")}});
            profile::prepare(cards.config({edits, more}));
            image = readAll(cards.profileSave());
            t.IsTrue(isNamed(image, 2, "Gamma") && !slotUsed(image, 1), "a new edit does not take the slot held for the deleted one");
            profile::prepare(cards.config({}));
            image = readAll(cards.profileSave());
            t.IsTrue(isNamed(image, 0, "Changed") && usedSlots(image) == std::vector<int>({0}), "all packs off: the player's changed wrestler stays, the pack's other wrestlers go");
            t.Equals(ledgerEntry(cards.ledger(), "e0"), std::string(), "and it is the player's now (no ledger entry)");
        });

        tc.Run("a group the card does not have is replaced by none, with a note; a group a pack creates is there", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("groups", &base);
            const std::string renames =
                "{\"format\": \"fpr-rename\", \"version\": 1, \"groups\": {\"20\": {\"name\": \"New\", \"short\": \"N\", \"org\": 1, \"kind\": 1, \"created\": true}}}";
            const auto edits = pack("g.pack", {{"in.edit.toml", wrestlerText("InGroup", 3)}, {"made.edit.toml", wrestlerText("Made", 20)}, {"gone.edit.toml", wrestlerText("Gone", 50)}},
                                    "1.0.0", renames);
            const profile::Result r = profile::prepare(cards.config({edits}));
            const Bytes image = readAll(cards.profileSave());
            t.IsTrue(r.applied && r.packs[0].error.empty(), "applied: " + r.packs[0].error);
            auto groupOf = [&](const char *name) {
                for (int k : usedSlots(image))
                    if (isNamed(image, k, name))
                        return static_cast<int>(readU16(image, editSlotOffset(k) + ed::kGroupAt));
                return -2;
            };
            t.Equals(groupOf("InGroup"), 3, "a group of the card stays");
            t.Equals(groupOf("Made"), 20, "a group the same pack creates stays");
            t.Equals(groupOf("Gone"), 0xFFFF, "a group that does not exist becomes none");
            t.IsTrue(anyContains(r.packs[0].notes, "data/edits/gone.edit.toml: group 50 is not in your card"), "with a note");
            t.Equals(r.packs[0].written > 0 ? size_t(1) : size_t(0), size_t(1), "the renames part still wrote its fields");
        });

        tc.Run("a bad edit file is skipped with a note and the others still apply", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("bad", &base);
            profile::PackInput p = pack("a.edits", {{"a.edit.toml", wrestlerText("Alpha")}, {"b.edit.toml", "format = \"fpr-edit\"\nversion = 1\n"}});
            profile::EditFile unreadable;
            unreadable.name = "c.edit.toml";
            unreadable.readError = "the file could not be read";
            p.edits.push_back(unreadable);
            const profile::Result r = profile::prepare(cards.config({p}));
            const Bytes image = readAll(cards.profileSave());
            t.IsTrue(usedSlots(image) == std::vector<int>({0}) && isNamed(image, 0, "Alpha"), "only the good one is in the card");
            t.Equals(r.packs[0].editsSkipped, size_t(2), "two skipped");
            t.IsTrue(anyContains(r.packs[0].notes, "data/edits/b.edit.toml: the table [name] is missing"), "the reason for the invalid one");
            t.IsTrue(anyContains(r.packs[0].notes, "data/edits/c.edit.toml: the file could not be read"), "and for the unreadable one");
            t.IsTrue(r.packs[0].error.empty(), "the pack itself is not refused");
        });

        tc.Run("with every slot in use there is no room: a note, and nothing else changes", [](TestCase &t)
        {
            Bytes base = makeSave();
            for (int k = 0; k < kEditSlotCount; ++k)
                putSlot(base, k, wrestler("P" + std::to_string(k)));
            fixIntegrity(base, testTable());
            Cards cards("full", &base);
            const profile::Result r = profile::prepare(cards.config({pack("a.edits", {{"a.edit.toml", wrestlerText("Alpha")}})}));
            t.Equals(r.packs[0].editsAdded, size_t(0), "nothing added");
            t.Equals(r.packs[0].editsSkipped, size_t(1), "one skipped");
            t.IsTrue(anyContains(r.packs[0].notes, "1 created wrestler could not be added: all 500 edit slots are in use"), "with the reason");
            t.IsTrue(readAll(cards.profileSave()) == base, "the card is as it was");
        });

        tc.Run("a pack of 501 edits fills all 500 slots and skips the last; it takes a fraction of a second", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("many", &base);
            EditList edits;
            for (int i = 0; i < 501; ++i)
            {
                char name[32];
                std::snprintf(name, sizeof(name), "w%03d.edit.toml", i);
                edits.push_back({name, wrestlerText("W" + std::to_string(i))});
            }
            const profile::Result r = profile::prepare(cards.config({pack("many.pack", edits)}));
            const Bytes image = readAll(cards.profileSave());
            t.IsTrue(r.applied && usedSlots(image).size() == 500, "all 500 slots are used");
            t.Equals(r.packs[0].editsAdded, size_t(500), "500 added");
            t.Equals(r.packs[0].editsSkipped, size_t(1), "one skipped for want of room");
            t.IsTrue(isNamed(image, 0, "W0") && isNamed(image, 499, "W499"), "in file-name order");
            t.IsTrue(checkIntegrity(image, testTable()).allOk(), "the save is valid");
            std::printf("[%.0f ms for 501 edits] ", r.milliseconds);
            t.IsTrue(r.milliseconds < 3000, "fast enough (" + std::to_string(r.milliseconds) + " ms)");
            const profile::Result again = profile::prepare(cards.config({pack("many.pack", edits)}));
            t.IsFalse(again.imageChanged, "a second start changes nothing");
        });

        tc.Run("renames and edits in one pack; a pack with only edits is not 'nothing'; a pack with neither says so", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("mixed", &base);
            const std::string renames = "{\"format\": \"fpr-rename\", \"version\": 1, \"wrestlers\": {\"4\": {\"first\": \"Renamed\"}}}";
            const profile::PackInput both = pack("both.pack", {{"a.edit.toml", wrestlerText("Alpha")}}, "1.0.0", renames);
            const profile::PackInput onlyEdits = pack("edits.pack", {{"b.edit.toml", wrestlerText("Beta")}});
            const profile::PackInput none = pack("none.pack", {});
            const profile::Result r = profile::prepare(cards.config({both, onlyEdits, none}));
            const Bytes image = readAll(cards.profileSave());
            t.IsTrue(isNamed(image, 0, "Alpha") && isNamed(image, 1, "Beta"), "both packs' wrestlers are in");
            t.IsTrue(anyContains(r.packs[0].notes, "Changes 1 field") && anyContains(r.packs[0].notes, "Adds 1 created wrestler (edit slots)"), "the first pack's page: renames and edits");
            t.IsTrue(!anyContains(r.packs[1].notes, "changes nothing"), "a pack with only edits does not say it changes nothing");
            t.IsTrue(anyContains(r.packs[2].notes, "has no data/renames.json or data/edits/*.edit.toml, so it changes nothing"), "an empty pack does");
            Bytes player;
            std::string why;
            t.IsTrue(profile::playerSave(cards.profile, testTable(), player, &why), why);
            t.IsTrue(player == base, "the importer's view of the player's own card has none of the pack's wrestlers or names");
        });

        tc.Run("a ledger written before slots existed (no 'file' member) still reads, and a slot entry survives a restart of the process", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Cards cards("ledger", &base);
            const auto packs = std::vector<profile::PackInput>{pack("a.edits", {{"a.edit.toml", wrestlerText("Alpha")}})};
            profile::prepare(cards.config(packs));
            const std::string text = readText(cards.ledger());
            t.IsTrue(contains(text, "\"file\": \"a.edit.toml\""), "the file name is in the ledger");
            // a rename-only ledger of the earlier format: no 'file' anywhere
            const Bytes plain = makeSave();
            Cards old("ledger_old", &plain);
            const std::string renames = "{\"format\": \"fpr-rename\", \"version\": 1, \"wrestlers\": {\"4\": {\"first\": \"Renamed\"}}}";
            profile::prepare(old.config({pack("r.pack", {}, "1.0.0", renames)}));
            t.IsFalse(contains(readText(old.ledger()), "\"file\""), "a rename field has no file member");
            const profile::Result r = profile::prepare(old.config({pack("r.pack", {{"a.edit.toml", wrestlerText("Alpha")}}, "1.0.0", renames)}));
            t.IsTrue(r.applied && r.packs[0].editsAdded == 1 && r.packs[0].written == 1, "a pack gaining an edit file later works with the old ledger");
        });
    });

    // ---------------------------------------------------------------------------------------------------------- the manager's side
    MiniTest::Case("EditPackManager", [](TestCase &tc)
    {
        tc.Run("a package's data/edits/*.edit.toml files are read in name order, and only those", [](TestCase &t)
        {
            const fs::path root = tempDir("manager");
            std::error_code ec;
            fs::create_directories(root / "mods", ec);
            Manager manager;
            Manager::Config config;
            config.modsDir = root / "mods";
            config.statePath = root / "mods.toml";
            t.IsTrue(manager.init(config), "starts");
            const Result installed = installPackage(manager, root / "src", "e.edits",
                                                    {{"data/edits/b.edit.toml", "second"}, {"data/edits/A.EDIT.TOML", "first"}, {"data/edits/readme.txt", "no"},
                                                     {"data/edits/sub/c.edit.toml", "deep"}, {"data/other/d.edit.toml", "elsewhere"}, {"data/renames.json", "{}"}});
            t.IsTrue(installed.ok, installed.message);
            profile::PackInput p;
            ed::readEditFiles(manager, "e.edits", p);
            t.Equals(p.edits.size(), size_t(2), "two edit files");
            t.IsTrue(p.edits.size() == 2 && p.edits[0].name == "A.EDIT.TOML" && p.edits[0].text == "first" && p.edits[1].name == "b.edit.toml" && p.edits[1].text == "second",
                     "in name order, with their text");
            profile::PackInput none;
            ed::readEditFiles(manager, "nobody", none);
            t.IsTrue(none.edits.empty(), "an unknown mod has none");
            const Result noEdits = installPackage(manager, root / "src", "n.none", {{"data/renames.json", "{}"}});
            t.IsTrue(noEdits.ok, noEdits.message);
            profile::PackInput empty;
            ed::readEditFiles(manager, "n.none", empty);
            t.IsTrue(empty.edits.empty(), "a pack without edit files has none");
        });
    });

    // ---------------------------------------------------------------------------------------------------------- the importer
    MiniTest::Case("EditPackImport", [](TestCase &tc)
    {
        tc.Run("extractEdits carries the created wrestlers the base lacks, as files named after their slots", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Bytes save = base;
            putSlot(save, 0, wrestler("Alpha"));
            putSlot(save, 2, wrestler("Beta", 3));
            Bytes garbage(kEditSlotSize, 0);
            garbage[0] = 1; // used, but not a wrestler the text can say
            putSlot(save, 5, garbage);
            fixIntegrity(save, testTable());
            imp::Extracted out;
            std::string error;
            t.IsTrue(imp::extractEdits(save, base, nullptr, out, &error), error);
            t.Equals(out.edits.size(), size_t(2), "two files");
            t.IsTrue(out.edits.size() == 2 && out.edits[0].file == "000-alpha-gotch.edit.toml" && out.edits[1].file == "002-beta-gotch.edit.toml", "named after the slot and the name");
            t.Equals(out.counts.edits, size_t(2), "counted");
            t.IsTrue(out.leftOut.size() == 1 && contains(out.leftOut[0], "edit slot 5"), "the slot the text cannot say is left out, with its number");
            Bytes back;
            t.IsTrue(ed::textToSlot(out.edits[1].text, nullptr, back, nullptr, &error) && back == wrestler("Beta", 3), "a file reads back to the slot's bytes: " + error);
            // a wrestler the base has already, in another slot, is not carried again
            Bytes withBase = base;
            putSlot(withBase, 9, wrestler("Alpha"));
            fixIntegrity(withBase, testTable());
            imp::Extracted second;
            t.IsTrue(imp::extractEdits(save, withBase, nullptr, second, &error), error);
            t.IsTrue(second.edits.size() == 1 && second.edits[0].file == "002-beta-gotch.edit.toml", "only the one the base does not have");
        });

        tc.Run("created wrestlers a pack carries are not 'other changes'; the ones it cannot carry are", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Bytes save = base;
            putSlot(save, 0, wrestler("Alpha"));
            putSlot(save, 1, wrestler("Beta"));
            fixIntegrity(save, testTable());
            imp::Extracted out;
            std::string error;
            t.IsTrue(imp::extractEdits(save, base, nullptr, out, &error), error);
            imp::Unmatched none;
            t.IsTrue(imp::unmatchedChanges(save, base, "{\"format\": \"fpr-rename\", \"version\": 1}", out.edits, none, &error), error);
            t.IsFalse(none.any(), "nothing is left over");
            imp::Unmatched without;
            t.IsTrue(imp::unmatchedChanges(save, base, "{\"format\": \"fpr-rename\", \"version\": 1}", without, &error), error);
            t.IsTrue(without.any() && without.areas.size() == 1 && without.areas[0] == "2 created wrestlers (edit slots)", "without the files they would be reported");
            // a wrestler in another group than the card has
            Bytes strange = base;
            putSlot(strange, 0, wrestler("Far", 77));
            fixIntegrity(strange, testTable());
            imp::Extracted farOut;
            t.IsTrue(imp::extractEdits(strange, base, nullptr, farOut, &error), error);
            imp::Unmatched far;
            t.IsTrue(imp::unmatchedChanges(strange, base, "{\"format\": \"fpr-rename\", \"version\": 1}", farOut.edits, far, &error), error);
            t.IsTrue(far.any() && far.areas[0] == "1 created wrestler (edit slot)", "a group the card lacks cannot be carried, so it is reported");
            // the wrestler list order of the editor: edit ids moved among the built-ins are not a change a pack must carry
            Bytes reordered = save;
            const uint16_t moved = readU16(reordered, kRoster + 4 + 2 * 327);
            for (size_t i = 327; i > 40; --i)
                putU16(reordered, kRoster + 4 + 2 * i, readU16(reordered, kRoster + 4 + 2 * (i - 1)));
            putU16(reordered, kRoster + 4 + 2 * 40, moved);
            fixIntegrity(reordered, testTable());
            imp::Unmatched listed;
            t.IsTrue(imp::unmatchedChanges(reordered, base, "{\"format\": \"fpr-rename\", \"version\": 1}", out.edits, listed, &error), error);
            t.IsFalse(listed.any(), "an edit id placed among the built-ins in the editor's list is not an 'other change'");
        });

        tc.Run("importing a save with created wrestlers installs a pack with edit files; applied to the card it gives the same wrestlers", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Bytes save = base;
            putSlot(save, 0, wrestler("Alpha", 2));
            putSlot(save, 4, wrestler("Beta"));
            putSlot(save, 5, wrestler("Gamma", 3));
            fixIntegrity(save, testTable());
            Bytes withRename = save;
            put(withRename, kOffNames + 8 * kNameStride + 1, textBytes("Renamed", kWidthFirst));
            fixIntegrity(withRename, testTable());

            for (const bool renames : {false, true})
            {
                const Bytes &source = renames ? withRename : save;
                struct Rig
                {
                    fs::path root, mods, state, base0, profile, scratch;
                    Manager manager;
                } rig;
                rig.root = tempDir(renames ? "import_rig_a" : "import_rig_b");
                rig.mods = rig.root / "mods";
                rig.state = rig.root / "mods.toml";
                rig.base0 = rig.root / "mc0";
                rig.profile = rig.root / "profile";
                rig.scratch = rig.root / "import-tmp";
                writeAll(rig.base0 / kSaveName / kSaveName, base);
                Manager::Config config;
                config.modsDir = rig.mods;
                config.statePath = rig.state;
                rig.manager.init(config);
                imp::Env env;
                env.manager = &rig.manager;
                env.crcTable = &testTable();
                env.profileDir = rig.profile;
                env.baseCard0 = rig.base0;
                env.scratchDir = rig.scratch;
                SaveEntry entry;
                entry.folder = "BASLUS-21702";
                entry.save = source;
                entry.saveName = entry.folder;
                entry.format = "ps2";
                const imp::Outcome o = imp::importEntries(env, {entry}, "Created Wrestlers.ps2", "");
                t.IsTrue(o.ok && o.turnedOn, "imported: " + o.message);
                t.Equals(o.counts.edits, size_t(3), "three created wrestlers");
                t.IsTrue(contains(o.message, "3 created wrestlers") && (renames ? contains(o.message, "1 wrestler,") : !contains(o.message, "1 wrestler,")), o.message);
                t.IsFalse(o.otherChanges, "no 'other changes' remain");
                const auto info = rig.manager.find(o.modKey);
                t.IsTrue(info && info->kind == Kind::Data, "installed as a data pack");
                t.Equals(rig.manager.hasFile(o.modKey, "data/renames.json") ? 1 : 0, renames ? 1 : 0, renames ? "it has a renames file" : "an edits-only pack has no renames file");
                profile::PackInput input;
                input.id = o.modKey;
                input.version = info ? info->version : "1.0.0";
                if (rig.manager.hasFile(o.modKey, "data/renames.json"))
                {
                    input.hasFile = true;
                    const auto bytes = rig.manager.readFile(o.modKey, "data/renames.json");
                    input.renamesJson.assign(bytes->begin(), bytes->end());
                }
                ed::readEditFiles(rig.manager, o.modKey, input);
                t.Equals(input.edits.size(), size_t(3), "three edit files in the package");
                Cards applied("import_apply", &base);
                const profile::Result r = profile::prepare(applied.config({input}));
                t.IsTrue(r.applied && r.packs[0].editsAdded == 3, "applied to the card: three wrestlers added");
                const Bytes image = readAll(applied.profileSave());
                std::multiset<Bytes> got, want;
                for (int k : usedSlots(image))
                    got.insert(slotAt(image, k));
                for (int k : usedSlots(source))
                    want.insert(slotAt(source, k));
                t.IsTrue(got == want, "the card has exactly the wrestlers the save had");
            }
        });

        tc.Run("a save whose only difference is a wrestler the text cannot say imports nothing and reports an 'other change'", [](TestCase &t)
        {
            const Bytes base = makeSave();
            Bytes save = base;
            Bytes garbage(kEditSlotSize, 0);
            garbage[0] = 1;
            putSlot(save, 3, garbage);
            fixIntegrity(save, testTable());
            const fs::path root = tempDir("import_garbage");
            writeAll(root / "mc0" / kSaveName / kSaveName, base);
            Manager manager;
            Manager::Config config;
            config.modsDir = root / "mods";
            config.statePath = root / "mods.toml";
            manager.init(config);
            imp::Env env;
            env.manager = &manager;
            env.crcTable = &testTable();
            env.profileDir = root / "profile";
            env.baseCard0 = root / "mc0";
            env.scratchDir = root / "import-tmp";
            SaveEntry entry;
            entry.folder = "BASLUS-21702";
            entry.save = save;
            entry.saveName = entry.folder;
            entry.format = "ps2";
            const imp::Outcome o = imp::importEntries(env, {entry}, "x.ps2", "");
            t.IsTrue(!o.ok && o.nothingToImport && o.otherChanges, "nothing to import, with the other change noted: " + o.message);
            t.IsTrue(contains(o.message, "This save also has other changes that packs cannot carry yet."), o.message);
            t.IsTrue(o.unmatched.areas.size() == 1 && o.unmatched.areas[0] == "1 created wrestler (edit slot)", "and it is the created wrestler");
        });
    });
    // ---------------------------------------------------------------------------------------------------------- the twin in Python
    MiniTest::Case("EditPackPython", [](TestCase &tc)
    {
        tc.Run("a save's created wrestlers become the very files tools/fprsave.py edits writes", [](TestCase &t)
        {
            const fs::path root = findRepoRoot();
            if (root.empty() || !runCommand("python -I -c \"pass\""))
            {
                std::printf("(skipped: needs baserom/ with the saves and python on the path) ");
                return;
            }
            const fs::path baseDir = root / "baserom" / "saves" / "001_baseline";
            const fs::path proofs = root / "baserom" / "out" / "test" / "e1b_proofs";
            std::error_code ec;
            if (!fs::exists(baseDir / kSaveName, ec) || !fs::exists(proofs, ec))
            {
                std::printf("(skipped: no saves with created wrestlers under baserom/) ");
                return;
            }
            const Bytes base = readAll(baseDir / kSaveName);
            const fs::path work = tempDir("python_edits");
            const std::string tool = "python -I " + q(root / "tools" / "fprsave.py") + " ";
            size_t files = 0, saves = 0;
            for (const char *name : {"A_default.bin", "B3_moves.bin", "B9_flags.bin", "C2_appearance.bin", "D1_deleted0.bin", "D2_created_again.bin"})
            {
                const fs::path file = proofs / name;
                if (!fs::exists(file, ec))
                    continue;
                const Bytes save = readAll(file);
                imp::Extracted got;
                std::string error;
                t.IsTrue(imp::extractEdits(save, base, nullptr, got, &error), std::string(name) + ": " + error);
                const fs::path out = work / name;
                t.IsTrue(runCommand(tool + "edits " + q(file) + " --base " + q(baseDir) + " --out " + q(out)), std::string(name) + ": fprsave.py edits ran");
                std::vector<std::string> names;
                for (const auto &entry : fs::directory_iterator(out, ec))
                    names.push_back(entry.path().filename().string());
                std::sort(names.begin(), names.end());
                std::vector<std::string> ours;
                for (const imp::EditPack &e : got.edits)
                    ours.push_back(e.file);
                t.IsTrue(names == ours, std::string(name) + ": the same file names");
                bool same = names == ours;
                for (const imp::EditPack &e : got.edits)
                    if (fs::exists(out / e.file, ec))
                        same = same && readText(out / e.file) == e.text;
                t.IsTrue(same && !got.edits.empty(), std::string(name) + ": the same text, byte for byte (" + std::to_string(got.edits.size()) + " files)");
                files += got.edits.size();
                ++saves;
            }
            t.IsTrue(saves >= 3 && files >= 3, "enough saves with created wrestlers were found");
            std::printf("[%zu files from %zu saves] ", files, saves);
        });
    });
}
