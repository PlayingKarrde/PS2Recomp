#include "MiniTest.h"

#include "edit_text.h"
#include "fpr_save.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Tests for the text form of a created wrestler (task e2; src/lib/mods/edit_text.*, docs/formats/edit-text.md): reading and writing the
// TOML text, the refusals (one line each), the move-name check, the names of a pack's edit files, and a comparison with tools/fpredit.py
// byte for byte on made-up slots, on every used slot of the saves under baserom/ and on the 340 built-in records.
//
// Nothing here contains game data. The made-up slots come from the schema (fpredit.py random-slots); the real files are read from
// baserom/ at test time, and that part says "skipped" when they are not there.

namespace
{
    namespace fs = std::filesystem;
    namespace ed = ps2x::mods::edit;
    using ps2x::mods::edit::Bytes;

    fs::path tempDir(const char *name)
    {
        fs::path dir = fs::temp_directory_path() / (std::string("ps2x_edittext_test_") + name);
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

    void writeText(const fs::path &path, const std::string &text)
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    bool contains(const std::string &haystack, const std::string &needle) { return haystack.find(needle) != std::string::npos; }

    fs::path findToolsRoot()
    {
        std::error_code ec;
        fs::path dir = fs::current_path(ec);
        for (int i = 0; i < 8 && !dir.empty(); ++i)
        {
            if (fs::exists(dir / "tools" / "fpredit.py", ec))
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

    // ---- the made-up slot (tools/fpredit.py synthetic_slot builds the very same bytes) ----------------------------------------------

    Bytes gameText(const std::string &utf8, size_t width)
    {
        std::vector<uint32_t> points;
        ps2x::mods::fpr::decodeUtf8(utf8, points);
        Bytes out;
        ps2x::mods::fpr::encodeGameText(points, out);
        out.resize(width, 0);
        return out;
    }

    void put16(Bytes &b, size_t at, unsigned v)
    {
        b[at] = static_cast<uint8_t>(v & 0xFF);
        b[at + 1] = static_cast<uint8_t>(v >> 8);
    }

    Bytes syntheticSlot()
    {
        Bytes slot(ed::kSlotSize, 0);
        put16(slot, 0, 1);
        const size_t r = ed::kRecordAt;
        slot[r + 0x00] = 1;
        const Bytes first = gameText("Karl", 14), last = gameText("Gotch", 18), nick = gameText("God", 24);
        std::copy(first.begin(), first.end(), slot.begin() + static_cast<std::ptrdiff_t>(r + 0x02));
        std::copy(last.begin(), last.end(), slot.begin() + static_cast<std::ptrdiff_t>(r + 0x12));
        std::copy(nick.begin(), nick.end(), slot.begin() + static_cast<std::ptrdiff_t>(r + 0x26));
        put16(slot, r + 0x42, 150);
        put16(slot, r + 0x44, 40);
        put16(slot, r + 0x46, 1900);
        slot[r + 0x48] = 1;
        slot[r + 0x49] = 1;
        put16(slot, r + 0x4A, 0xFFFF);
        slot[r + 0x5C] = slot[r + 0x5D] = 3;
        for (size_t i = 0; i < 24; ++i)
            slot[r + 0x69 + i] = 3;
        put16(slot, r + 0x82, 6 | 0x4000 | 0x1000);
        put16(slot, r + 0x84, 29);
        put16(slot, r + 0x86, 64 | 0x2000);
        put16(slot, ed::kBudgetAt, 380);
        slot[ed::kAppearanceAt + 2] = 1;
        slot[ed::kAppearanceAt + 3] = 1;
        return slot;
    }

    std::string textOf(const Bytes &slot, const ed::MoveNames *names = nullptr)
    {
        std::string text, error;
        const bool ok = ed::slotToText(slot, names, text, &error);
        if (!ok)
            std::printf("[slotToText failed: %s] ", error.c_str());
        return text;
    }

    int g_miss = 0; // substitutions that found nothing (a test bug)

    std::string sub(std::string text, const std::string &from, const std::string &to)
    {
        const size_t at = text.find(from);
        if (at == std::string::npos || text.find(from, at + 1) != std::string::npos)
        {
            ++g_miss;
            return text;
        }
        return text.replace(at, from.size(), to);
    }

    struct ErrorCase
    {
        const char *what;
        std::function<std::string(const std::string &)> change;
        std::string want;
    };

    std::vector<ErrorCase> errorCases()
    {
        const std::string styles = "Strong, Tech., Wrestling, Power, Lucha, Shooting, Mysterious, Koppo, Kobudo, Boxing, MMA";
        auto S = [](const std::string &from, const std::string &to) {
            return [from, to](const std::string &t) { return sub(t, from, to); };
        };
        // the same table as ERROR_CASES in tools/fpredit.py
        return {
            {"a wrong format", S("format = \"fpr-edit\"", "format = \"fpr-rename\""), "'format' must be \"fpr-edit\""},
            {"a wrong version", S("version = 1\n", "version = 2\n"), "'version' must be 1 (the only version this game reads)"},
            {"an unknown top-level key", S("version = 1\n", "version = 1\nnonsense = 1\n"), "unknown key 'nonsense' (keys starting with _ are ignored)"},
            {"an unknown key in a table", S("flip = false", "flip = false\nfrist = 1"), "name: unknown key 'frist' (keys starting with _ are ignored)"},
            {"a name too long", S("first = \"Karl\"", "first = \"ABCDEFGH\""), "name.first: 'ABCDEFGH' has 8 characters; the FIRST field takes at most 7"},
            {"a height out of range", S("height = 150", "height = 300"), "parameters.height: 300 is out of range (149 to 220, or \"unknown\")"},
            {"a word that is not allowed", S("height = 150", "height = \"tall\""), "parameters.height: the only word allowed is \"unknown\""},
            {"a fraction", S("height = 150", "height = 150.5"), "parameters.height: must be a whole number or \"unknown\", not something else"},
            {"a group out of range", S("group = \"none\"", "group = 100"), "parameters.group: 100 is out of range (0 to 99, or \"none\")"},
            {"a voice category out of range", S("voice1 = { category = 0, number = 0 }", "voice1 = { category = 11, number = 0 }"),
             "parameters.voice1.category: 11 is out of range (0 to 10)"},
            {"a voice of the wrong shape", S("voice1 = { category = 0, number = 0 }", "voice1 = 5"),
             "parameters.voice1: must be { category = 0 to 10, number = 0 to 255 }"},
            {"a missing parameter", S("weapon = 0  # 0 to 7\n", ""), "parameters.weapon is missing"},
            {"an enum word that is not one", S("class = \"heavy\"", "class = \"giant\""), "parameters.class: 'giant' is not a value (heavy, junior)"},
            {"a skill out of range", S("offense = { punch = 1,", "offense = { punch = 11,"), "skills.offense.punch: 11 is out of range (1 to 10)"},
            {"a move id too high", S("slot00 = { id = 6,", "slot00 = { id = 2001,"), "moves.slot00.id: 2001 is out of range (0 to 2000)"},
            {"a variant that is not one", S("variant = \"V1\"", "variant = \"V3\""), "moves.slot00.variant: 'V3' is not a variant (V1, V2)"},
            {"a custom move", S("slot01 = { id = 29 }", "slot01 = { move = \"some.mod/Flying Thing\" }"),
             "moves.slot01: custom moves (move = \"<mod id>/<name>\") are not supported yet; give the move number as id"},
            {"two finishers", S("slot00 = { id = 6, variant = \"V1\", mark = \"S\" }", "slot00 = { id = 6, variant = \"V1\", mark = \"C\" }"),
             "moves: 2 moves are marked C; only one finisher is allowed"},
            {"five specials",
             [](const std::string &t) {
                 std::string x = sub(t, "slot00 = { id = 6, variant = \"V1\", mark = \"S\" }", "slot00 = { id = 6, mark = \"S\" }");
                 x = sub(x, "slot01 = { id = 29 }", "slot01 = { id = 29, mark = \"S\" }");
                 x = sub(x, "slot03 = { id = 0 }", "slot03 = { id = 0, mark = \"S\" }");
                 x = sub(x, "slot04 = { id = 0 }", "slot04 = { id = 0, mark = \"S\" }");
                 return sub(x, "slot05 = { id = 0 }", "slot05 = { id = 0, mark = \"S\" }");
             },
             "moves: 5 moves are marked S; at most 4 are allowed"},
            {"a CPU column that does not add up", S("stand_back = [[0, 0, 0, 0, 0, 0, 0, 100], ", "stand_back = [[0, 0, 0, 0, 0, 0, 0, 99], "),
             "cpu_logic.stand_back: column 1 adds up to 99, not 100"},
            {"a CPU column of the wrong length", S("stand_back = [[0, 0, 0, 0, 0, 0, 0, 100], ", "stand_back = [[0, 0, 0, 0, 0, 0, 100], "),
             "cpu_logic.stand_back: column 1 must be 8 whole numbers"},
            {"a CPU rest of the wrong length", S("rest = \"" + std::string(212, '0') + "\"", "rest = \"" + std::string(210, '0') + "\""),
             "cpu_logic.rest: must be 212 hexadecimal digits (106 bytes)"},
            {"a style that is not one", S("style = \"Tech.\"", "style = \"Sumo\""), "appearance.style: 'Sumo' is not a style (" + styles + ")"},
            {"a missing table", [](const std::string &t) { return t.substr(0, t.find("\n[appearance]")); }, "the table [appearance] is missing"},
            {"hex that is not hex", S("first = \"Karl\"", "first = { hex = \"zz\" }"), "name.first: the hex value is not an even number of hexadecimal digits"},
            {"hex too long", S("first = \"Karl\"", "first = { hex = \"" + std::string(30, '0') + "\" }"), "name.first: the hex value is 15 bytes; the FIRST field holds 14"},
            {"a number where text belongs", S("first = \"Karl\"", "first = 5"), "name.first: must be text or { hex = \"...\" }, not a number"},
            {"a profile line too long", S("line1 = \"\"", "line1 = \"" + std::string(22, 'A') + "\""),
             "profile.line1: '" + std::string(22, 'A') + "' has 22 characters; the profile line field takes at most 21"},
            {"a finisher name too long", S("finisher_name = \"\"", "finisher_name = \"" + std::string(21, 'B') + "\""),
             "moves.finisher_name: '" + std::string(21, 'B') + "' has 21 characters; the finisher name field takes at most 20"},
            {"a budget out of range", S("point_budget = 380", "point_budget = 70000"), "skills.point_budget: 70000 is out of range (0 to 65535)"},
            {"flags_raw out of range", S("flip = false", "flip = false\nflags_raw = 300"), "name.flags_raw: 300 is out of range (0 to 255)"},
        };
    }

    // The bytes of file `path` as slots (or records) of `size` bytes.
    std::vector<Bytes> chunks(const Bytes &all, size_t size)
    {
        std::vector<Bytes> out;
        for (size_t at = 0; at + size <= all.size(); at += size)
            out.emplace_back(all.begin() + static_cast<std::ptrdiff_t>(at), all.begin() + static_cast<std::ptrdiff_t>(at + size));
        return out;
    }

    // C++ and Python against each other on the slots (or records) in `bin`. Returns the number compared; failures go to `t`.
    size_t compareWithPython(TestCase &t, const fs::path &root, const fs::path &work, const fs::path &bin, bool records, const std::string &what)
    {
        const Bytes all = readAll(bin);
        const size_t size = records ? ed::kRecordSize : ed::kSlotSize;
        const std::vector<Bytes> items = chunks(all, size);
        const std::string tool = "python -I " + q(root / "tools" / "fpredit.py") + " ";
        const fs::path cpp = work / ("cpp_" + what), py = work / ("py_" + what), back = work / (what + "_back.bin");
        std::error_code ec;
        fs::remove_all(cpp, ec);
        fs::remove_all(py, ec);
        fs::create_directories(cpp, ec);
        size_t compared = 0, unwritten = 0;
        for (size_t i = 0; i < items.size(); ++i)
        {
            std::string text, error;
            char name[32];
            std::snprintf(name, sizeof(name), "%06zu.edit.toml", i);
            const bool ok = records ? ed::recordToText(items[i], nullptr, text, &error) : ed::slotToText(items[i], nullptr, text, &error);
            if (!ok)
            {
                ++unwritten;
                continue;
            }
            writeText(cpp / name, text);
        }
        t.Equals(unwritten, size_t(0), what + ": every item can be written as text");
        t.IsTrue(runCommand(tool + "bulk-totext " + q(bin) + " " + q(py) + " --no-names" + (records ? " --record" : "")), what + ": fpredit.py bulk-totext ran");
        t.IsTrue(runCommand(tool + "bulk-fromtext " + q(cpp) + " " + q(back) + (records ? " --record" : "") + " --no-names"), what + ": fpredit.py reads the C++ texts");
        const Bytes pyBack = readAll(back);
        // Python's texts are the C++ texts, byte for byte, and C++ reads Python's texts back to the same bytes
        size_t same = 0, readBack = 0;
        Bytes expected;
        for (size_t i = 0; i < items.size(); ++i)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "%06zu.edit.toml", i);
            if (!fs::exists(cpp / name, ec))
                continue;
            ++compared;
            const std::string cppText = readText(cpp / name), pyText = readText(py / name);
            same += cppText == pyText;
            Bytes got;
            std::string error;
            const bool ok = records ? ed::textToRecord(pyText, nullptr, got, nullptr, &error) : ed::textToSlot(pyText, nullptr, got, nullptr, &error);
            Bytes want = items[i];
            if (!records)
                want[0] = 1, want[1] = 0;
            if (ok && got == want)
                ++readBack;
            expected.insert(expected.end(), want.begin(), want.end());
        }
        t.Equals(same, compared, what + ": the text C++ writes equals the text fpredit.py writes, byte for byte");
        t.Equals(readBack, compared, what + ": C++ reads fpredit.py's text back to the very same bytes");
        t.IsTrue(pyBack == expected, what + ": fpredit.py reads C++'s text back to the very same bytes");
        return compared;
    }
}

void register_edit_text_tests()
{
    // ---------------------------------------------------------------------------------------------------- the made-up slot
    MiniTest::Case("EditText", [](TestCase &t_)
    {
        t_.Run("a slot becomes text and the text becomes the slot again", [](TestCase &t)
        {
            const Bytes slot = syntheticSlot();
            std::string text, error;
            t.IsTrue(ed::slotToText(slot, nullptr, text, &error), error);
            t.IsTrue(text.rfind("# Fire Pro Wrestling Returns: one created wrestler", 0) == 0, "starts with the header comment");
            t.IsTrue(contains(text, "\nformat = \"fpr-edit\"\nversion = 1\n"), "format and version come first");
            t.IsTrue(contains(text, "first = \"Karl\"  # up to 7 characters"), "names are plain text with a hint");
            t.IsTrue(contains(text, "slot00 = { id = 6, variant = \"V1\", mark = \"S\" }"), "a move with its variant and mark");
            t.IsTrue(contains(text, "slot02 = { id = 64, mark = \"C\" }") && contains(text, "slot03 = { id = 0 }"), "the finisher, and an empty slot");
            t.IsTrue(contains(text, "group = \"none\"") && contains(text, "height = 150"), "words for unknown and none");
            t.IsTrue(contains(text, "class = \"heavy\"") && contains(text, "move_speed = 0") && contains(text, "offense = { punch = 1, "), "enums and shifted values");
            Bytes back;
            std::vector<std::string> warnings;
            t.IsTrue(ed::textToSlot(text, nullptr, back, &warnings, &error), error);
            t.IsTrue(back == slot && warnings.empty(), "the same 1,100 bytes");
            t.IsTrue(!contains(text, "[unmapped]") && !contains(text, "flags_raw ="), "nothing unmapped to carry");
        });

        t_.Run("the invalid texts are each refused with one expected line", [](TestCase &t)
        {
            const std::string text = textOf(syntheticSlot());
            g_miss = 0;
            size_t good = 0;
            const std::vector<ErrorCase> cases = errorCases();
            for (const ErrorCase &c : cases)
            {
                Bytes out;
                std::string error;
                const bool ok = ed::textToSlot(c.change(text), nullptr, out, nullptr, &error);
                t.IsFalse(ok, std::string(c.what) + ": refused");
                t.Equals(error, c.want, std::string(c.what) + ": the line");
                good += !ok && error == c.want;
            }
            t.Equals(g_miss, 0, "every change found its text");
            std::printf("[%zu of %zu refusals as expected] ", good, cases.size());
            Bytes out;
            std::string error;
            t.IsFalse(ed::textToSlot(text + "= = =\n", nullptr, out, nullptr, &error), "invalid TOML is refused");
            t.IsTrue(error.rfind("this is not valid TOML", 0) == 0 && error.find('\n') == std::string::npos, "with one line that starts 'this is not valid TOML': " + error);
        });

        t_.Run("comments, key order, upper case words and a bare record", [](TestCase &t)
        {
            const Bytes slot = syntheticSlot();
            const std::string text = textOf(slot);
            Bytes out;
            std::string error;
            t.IsTrue(ed::textToSlot(sub(text, "version = 1\n", "version = 1\n_note = \"hello\"\n"), nullptr, out, nullptr, &error) && out == slot, "a key starting with _ is a comment: " + error);
            std::string windows = "\xEF\xBB\xBF";
            for (const char c : text)
                windows += c == '\n' ? std::string("\r\n") : std::string(1, c);
            t.IsTrue(ed::textToSlot(windows, nullptr, out, nullptr, &error) && out == slot, "a byte order mark and Windows line ends are fine: " + error);
            t.IsTrue(ed::textToSlot(sub(sub(text, "class = \"heavy\"", "class = \"HEAVY\""), "style = \"Tech.\"", "style = \"tech.\""), nullptr, out, nullptr, &error) && out == slot,
                     "enum words ignore capitals: " + error);
            t.IsTrue(ed::textToSlot(sub(text, "height = 150", "height = \"UNKNOWN\""), nullptr, out, nullptr, &error), error);
            t.Equals(unsigned(out[ed::kRecordAt + 0x42]) | (unsigned(out[ed::kRecordAt + 0x43]) << 8), 0xFFFFu, "\"unknown\" is the stored 0xFFFF");
            // a record only: no [appearance], no extras
            const Bytes record(slot.begin() + 2, slot.begin() + 2 + static_cast<std::ptrdiff_t>(ed::kRecordSize));
            std::string recText;
            t.IsTrue(ed::recordToText(record, nullptr, recText, &error), error);
            t.IsTrue(!contains(recText, "[appearance]") && !contains(recText, "point_budget") && !contains(recText, "finisher_name"), "no slot extras in a record text");
            Bytes recBack;
            t.IsTrue(ed::textToRecord(recText, nullptr, recBack, nullptr, &error) && recBack == record, "the record reads back: " + error);
            t.IsFalse(ed::textToSlot(recText, nullptr, out, nullptr, &error), "a record text is not a whole edit");
            t.Equals(error, std::string("the table [appearance] is missing"), "and says why");
            t.IsFalse(ed::textToRecord(text, nullptr, recBack, nullptr, &error), "a whole edit is not a record text");
        });

        t_.Run("odd bytes survive: leftovers after a name, unused flag bits, the bytes the game never uses", [](TestCase &t)
        {
            Bytes slot = syntheticSlot();
            // leftover bytes after the first zero of the finisher name and of a profile line
            slot[ed::kFinisherAt + 10] = 0x82;
            slot[ed::kFinisherAt + 11] = 0x60;
            slot[ed::kRecordAt + 0x20C + 9] = 0x41;
            slot[ed::kRecordAt + 0x01] = 0x80 | 0x04 | 0x02;  // flip, an unused bit, space "equals"
            slot[ed::kRecordAt + 0x10] = 7;                   // a pad byte
            slot[ed::kRecordAt + 0x81] = 9;
            std::string text, error;
            t.IsTrue(ed::slotToText(slot, nullptr, text, &error), error);
            t.IsTrue(contains(text, "finisher_name = { hex = \"") && contains(text, "line1 = { hex = \""), "text with leftovers is carried as hex");
            t.IsTrue(contains(text, "flags_raw = 134") && contains(text, "space = \"equals\"") && contains(text, "flip = true"), "the unused flag bit: " + text.substr(0, 0));
            t.IsTrue(contains(text, "[unmapped]\npad_bytes = \""), "the unused bytes are carried");
            Bytes back;
            t.IsTrue(ed::textToSlot(text, nullptr, back, nullptr, &error) && back == slot, "all of it reads back: " + error);
        });

        t_.Run("a slot that is outside what the editor allows is not turned into text", [](TestCase &t)
        {
            Bytes slot = syntheticSlot();
            slot[ed::kRecordAt + 0x4E] = 9; // gender 9
            std::string text, error;
            t.IsFalse(ed::slotToText(slot, nullptr, text, &error), "a gender of 9 cannot be carried");
            t.Equals(error, std::string("parameters.gender: the stored value 9 is outside the range the text can show (0 to 3)"), "and says which value");
            slot = syntheticSlot();
            slot[ed::kRecordAt + 0x5C] = 9; // a speed the text cannot show
            t.IsFalse(ed::slotToText(slot, nullptr, text, &error), "a speed of 9 cannot be carried");
            t.IsTrue(contains(error, "parameters.move_speed"), error);
            slot = syntheticSlot();
            slot[ed::kRecordAt + 0x12A] = 5;
            slot[ed::kRecordAt + 0x12A + 1] = 3; // a CPU column that goes down
            t.IsFalse(ed::slotToText(slot, nullptr, text, &error), "a CPU column that goes down cannot be carried");
            t.Equals(error, std::string("the slot does not survive a round trip through the text (cpu_logic.stand_back: column 1 has a percentage outside 0 to 100)"),
                     "and the line says what the text could not hold");
            Bytes empty(ed::kSlotSize, 0);
            t.IsFalse(ed::slotToText(empty, nullptr, text, &error), "an empty slot has no text");
            t.Equals(error, std::string("the slot is empty"), "and says so");
        });

        t_.Run("move names are a readable check: shown when known, a warning when they differ, never an error", [](TestCase &t)
        {
            const Bytes slot = syntheticSlot();
            const ed::MoveNames names = [](int id, std::string &name) {
                if (id < 1 || id >= 2000)
                    return false;
                name = "name" + std::to_string(id);
                return true;
            };
            const std::string text = textOf(slot, &names);
            t.IsTrue(contains(text, "slot00 = { id = 6, name = \"name6\", variant = \"V1\", mark = \"S\" }"), "the name sits between the id and the flags");
            t.IsTrue(contains(text, "slot03 = { id = 0 }"), "an empty slot has none");
            Bytes back;
            std::vector<std::string> warnings;
            std::string error;
            t.IsTrue(ed::textToSlot(text, &names, back, &warnings, &error) && back == slot && warnings.empty(), "matching names: no warning");
            const ed::MoveNames other = [](int id, std::string &name) {
                name = "other" + std::to_string(id);
                return id >= 1 && id < 2000;
            };
            t.IsTrue(ed::textToSlot(text, &other, back, &warnings, &error) && back == slot, "different names are not an error");
            t.IsTrue(warnings.size() == 3 && contains(warnings[0], "moves.slot00: the name 'name6' is not the name of move 6 in your game ('other6')"), warnings.empty() ? "no warning" : warnings[0]);
            t.IsTrue(ed::textToSlot(text, nullptr, back, &warnings, &error) && warnings.empty(), "no names given: no check");
            t.IsTrue(ed::textToSlot(sub(text, "name = \"name6\"", "name = \" NAME6 \""), &names, back, &warnings, &error) && warnings.empty(), "capitals and spaces around a name do not matter");
        });

        t_.Run("edit file names and display names", [](TestCase &t)
        {
            Bytes slot = syntheticSlot();
            t.Equals(ed::editFileName(7, slot), std::string("007-karl-gotch.edit.toml"), "slot number and the name as the game shows it");
            t.Equals(ed::displayName(slot), std::string("Karl Gotch"), "display name");
            slot[ed::kRecordAt + 1] = 0x80;
            t.Equals(ed::editFileName(12, slot), std::string("012-gotch-karl.edit.toml"), "Flip puts the last name first");
            std::fill(slot.begin() + 2 + 2, slot.begin() + 2 + 2 + 14, 0);
            std::fill(slot.begin() + 2 + 0x12, slot.begin() + 2 + 0x12 + 18, 0);
            t.Equals(ed::editFileName(5, slot), std::string("005.edit.toml"), "a name with no letters or digits gives just the number");
            slot[ed::kRecordAt + 1] = 0; // no Flip: FIRST first
            const Bytes jp = gameText("A. B-C9", 14);
            std::copy(jp.begin(), jp.end(), slot.begin() + 2 + 2);
            t.Equals(ed::editFileName(499, slot), std::string("499-a-b-c9.edit.toml"), "runs of anything else become one dash");
            const Bytes longName = gameText("ABCDEFG", 14), longLast = gameText("HIJKLMNOP", 18);
            std::copy(longName.begin(), longName.end(), slot.begin() + 2 + 2);
            std::copy(longLast.begin(), longLast.end(), slot.begin() + 2 + 0x12);
            t.Equals(ed::editFileName(1, slot), std::string("001-abcdefg-hijklmnop.edit.toml"), "a full name (7 and 9 characters) is 17 characters long");
        });

        t_.Run("how long one edit file takes to read and to write", [](TestCase &t)
        {
            const Bytes slot = syntheticSlot();
            const std::string text = textOf(slot);
            Bytes out;
            std::string error, again;
            const auto t0 = std::chrono::steady_clock::now();
            const int n = 100;
            for (int i = 0; i < n; ++i)
                ed::textToSlot(text, nullptr, out, nullptr, &error);
            const auto t1 = std::chrono::steady_clock::now();
            for (int i = 0; i < n; ++i)
                ed::slotToText(slot, nullptr, again, &error);
            const auto t2 = std::chrono::steady_clock::now();
            const double read = std::chrono::duration<double, std::milli>(t1 - t0).count() / n, write = std::chrono::duration<double, std::milli>(t2 - t1).count() / n;
            std::printf("[%.2f ms to read, %.2f ms to write (%zu bytes of text)] ", read, write, text.size());
            t.IsTrue(out == slot && again == text, "same result every time");
        });

        t_.Run("the schema the C++ twin reads", [](TestCase &t)
        {
            const auto params = ed::parameters();
            t.Equals(params.size(), size_t(32), "32 lines of [parameters]");
            auto find = [&](const char *key) {
                for (const auto &p : params)
                    if (p.key == key)
                        return p;
                return ed::ParameterInfo();
            };
            t.IsTrue(find("height").lo == 149 && find("height").hi == 220 && find("height").word == "unknown" && find("height").unknownStored == 0xFFFF, "height");
            t.IsTrue(find("class").names.size() == 2 && find("class").delta == 1, "class is an enum stored as index + 1");
            t.IsTrue(find("voice1").voice && find("voice2").offset == 0x64, "voices");
            t.IsTrue(find("group").word == "none" && find("move_speed").delta == 3, "none and the speeds");
        });
    });

    // ---------------------------------------------------------------------------------------------------- the twin in Python
    MiniTest::Case("EditTextPython", [](TestCase &tc)
    {
        tc.Run("edit_schema.inc is what `fpredit.py schema --cpp` writes now", [](TestCase &t)
        {
            const fs::path root = findToolsRoot();
            const fs::path inc = root / "external" / "PS2Recomp" / "ps2xRuntime" / "src" / "lib" / "mods" / "edit_schema.inc";
            std::error_code ec;
            if (root.empty() || !fs::exists(inc, ec) || !runCommand("python -I -c \"pass\""))
            {
                std::printf("(skipped: needs the repository's tools/ and python on the path) ");
                return;
            }
            const fs::path work = tempDir("schema");
            t.IsTrue(runCommand("python -I " + q(root / "tools" / "fpredit.py") + " schema --cpp " + q(work / "schema.inc")), "fpredit.py schema --cpp ran");
            t.IsTrue(readText(work / "schema.inc") == readText(inc), "the committed table is the generated one");
        });

        tc.Run("made-up slots: C++ and fpredit.py write and read the same text and bytes", [](TestCase &t)
        {
            const fs::path root = findToolsRoot();
            if (root.empty() || !runCommand("python -I -c \"pass\""))
            {
                std::printf("(skipped: needs the repository's tools/ and python on the path) ");
                return;
            }
            const fs::path work = tempDir("random");
            const fs::path bin = work / "random.bin";
            t.IsTrue(runCommand("python -I " + q(root / "tools" / "fpredit.py") + " random-slots 250 20261110 " + q(bin)), "fpredit.py random-slots ran");
            const size_t n = compareWithPython(t, root, work, bin, false, "random");
            t.Equals(n, size_t(250), "all 250 made-up slots compared");
            std::printf("[%zu slots] ", n);
        });

        tc.Run("the player's own files: every used slot of the saves and the 340 built-in records", [](TestCase &t)
        {
            const fs::path root = findToolsRoot();
            std::error_code ec;
            if (root.empty() || !fs::exists(root / "baserom" / "saves" / "001_baseline", ec) || !runCommand("python -I -c \"pass\""))
            {
                std::printf("(skipped: needs baserom/ and python on the path) ");
                return;
            }
            const fs::path work = tempDir("real");
            // every distinct used slot of every raw save under baserom/
            std::set<Bytes> seen;
            Bytes corpus;
            const size_t saveSize = ps2x::mods::fpr::kSaveSize;
            for (fs::recursive_directory_iterator it(root / "baserom", fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
            {
                std::error_code e2;
                if (!it->is_regular_file(e2) || it->file_size(e2) != saveSize)
                    continue;
                const std::string name = it->path().filename().string();
                if (name != "BASLUS-21702" && it->path().extension() != ".bin")
                    continue;
                const Bytes save = readAll(it->path());
                for (int s = 0; s < ps2x::mods::fpr::kEditSlotCount; ++s)
                {
                    const size_t o = ps2x::mods::fpr::editSlotOffset(s);
                    Bytes slot(save.begin() + static_cast<std::ptrdiff_t>(o), save.begin() + static_cast<std::ptrdiff_t>(o + ed::kSlotSize));
                    if (!(slot[0] | slot[1]) || !seen.insert(slot).second)
                        continue;
                    corpus.insert(corpus.end(), slot.begin(), slot.end());
                }
            }
            if (corpus.empty())
            {
                std::printf("(skipped: no save with a created wrestler under baserom/) ");
            }
            else
            {
                writeText(work / "real.bin", std::string(corpus.begin(), corpus.end()));
                const size_t n = compareWithPython(t, root, work, work / "real.bin", false, "real");
                t.IsTrue(n >= 5, "the saves hold created wrestlers (" + std::to_string(n) + " distinct)");
                std::printf("[%zu real slots] ", n);
            }
            if (fs::exists(root / "baserom" / "out" / "fpr_rom_toc.csv", ec) && fs::exists(root / "baserom" / "iso" / "FPR.ROM", ec))
            {
                t.IsTrue(runCommand("python -I " + q(root / "tools" / "fpredit.py") + " dump-builtins " + q(work / "builtin.bin")), "fpredit.py dump-builtins ran");
                const size_t n = compareWithPython(t, root, work, work / "builtin.bin", true, "builtin");
                t.Equals(n, size_t(340), "all 340 built-in records compared");
            }
            else
            {
                std::printf("(skipped the built-in records: no FPR.ROM table of contents) ");
            }
        });
    });
}
