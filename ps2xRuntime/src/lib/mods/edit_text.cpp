#include "edit_text.h"

#include "fpr_save.h"

#include <toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>

namespace ps2x::mods::edit
{
    namespace schema
    {
        struct ParamSpec
        {
            const char *key;
            int kind; // 0 a number (or name), 1 a voice: { category, number }
            int off;
            int size;
            int lo, hi, delta;
            long long unknown; // the stored value the word stands for, -1 for none
            const char *word;
            const char *const *names;
            int nameCount;
            const char *comment;
        };
        struct SituationSpec
        {
            const char *key;
            int rows, cols;
        };
        struct CommentSpec
        {
            const char *key;
            const char *text;
        };
#include "edit_schema.inc"
    } // namespace schema

    namespace
    {
        using schema::ParamSpec;

        constexpr int kMoveSlots = 84;
        constexpr int kMoveIdMax = 2000;
        constexpr int kMaxS = 4, kMaxC = 1;
        constexpr int kLogicSize = 226;
        constexpr int kProfileChars = 21, kFinisherChars = 20;
        constexpr int kSkillDelta = 2;
        constexpr size_t kOffenseAt = 0x69, kDefenseAt = 0x75;
        constexpr size_t kLogicAt = 0x12A;
        constexpr int kLogicDecoded = 120; // bytes of the block the situations cover
        constexpr size_t kAppearanceRestAt = 6;
        // The bytes the game never uses, in the order the text carries them.
        constexpr struct
        {
            size_t at, size;
        } kPads[] = {{0x10, 2}, {0x24, 2}, {0x3E, 2}, {0x61, 1}, {0x66, 2}, {0x81, 1}};
        constexpr size_t kPadBytes = 10;

        struct NameField
        {
            const char *key;
            size_t off, width;
            int chars;
            const char *label;
        };
        constexpr NameField kNameFields[] = {{"first", 0x02, 14, 7, "FIRST"}, {"last", 0x12, 18, 9, "LAST"}, {"nickname", 0x26, 24, 12, "NICKNAME"}};

        struct Failure
        {
            std::string message;
        };
        [[noreturn]] void fail(std::string message) { throw Failure{std::move(message)}; }

        uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
        void wr16(uint8_t *p, uint32_t v)
        {
            p[0] = static_cast<uint8_t>(v & 0xFF);
            p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        }

        std::string hexOf(const uint8_t *p, size_t n)
        {
            static const char digits[] = "0123456789abcdef";
            std::string out;
            out.reserve(n * 2);
            for (size_t i = 0; i < n; ++i)
            {
                out += digits[p[i] >> 4];
                out += digits[p[i] & 15];
            }
            return out;
        }

        std::string lowerAscii(std::string text)
        {
            for (char &c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return text;
        }

        std::string tomlString(const std::string &text)
        {
            std::string out = "\"";
            for (const char ch : text)
            {
                const unsigned char c = static_cast<unsigned char>(ch);
                if (ch == '"') out += "\\\"";
                else if (ch == '\\') out += "\\\\";
                else if (ch == '\n') out += "\\n";
                else if (c < 0x20 || c == 0x7F)
                {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                }
                else out += ch;
            }
            return out + "\"";
        }

        // A text field of the record as TOML: plain text when that stores back to the same bytes, otherwise { hex = "..." } (bytes after the
        // first zero make it hex too, with the trailing zeros left off, so nothing is lost).
        std::string tomlField(const uint8_t *raw, size_t width)
        {
            size_t n = 0;
            while (n < width && raw[n] != 0)
                ++n;
            bool tail = false;
            for (size_t i = n; i < width; ++i)
                tail = tail || raw[i] != 0;
            if (tail)
            {
                size_t end = width;
                while (end > 0 && raw[end - 1] == 0)
                    --end;
                return "{ hex = \"" + hexOf(raw, end) + "\" }";
            }
            bool isHex = false;
            const std::string t = fpr::fieldTextOrHex(raw, width, &isHex);
            return isHex ? "{ hex = \"" + t + "\" }" : tomlString(t);
        }

        std::string commentFor(const char *key)
        {
            for (const auto &c : schema::kComments)
                if (std::strcmp(c.key, key) == 0)
                    return c.text;
            return std::string();
        }
        std::string trailing(const char *key)
        {
            const std::string c = commentFor(key);
            return c.empty() ? c : "  # " + c;
        }

        // ---- text of a record ----------------------------------------------------------------------------------------------------

        struct Extras
        {
            uint16_t budget = 0;
            const uint8_t *finisher = nullptr; // 40 bytes
            const uint8_t *appearance = nullptr; // 448 bytes
        };

        std::string shownValue(const ParamSpec &p, long long stored)
        {
            if (p.unknown >= 0 && stored == p.unknown)
                return tomlString(p.word);
            const long long v = stored - p.delta;
            if (v < p.lo || v > p.hi)
                fail(std::string("parameters.") + p.key + ": the stored value " + std::to_string(stored) + " is outside the range the text can show (" +
                     std::to_string(p.lo + p.delta) + " to " + std::to_string(p.hi + p.delta) + ")");
            if (p.names)
                return tomlString(p.names[v]);
            return std::to_string(v);
        }

        std::string emit(const uint8_t *rec, const Extras *extras, const MoveNames *names)
        {
            std::string out;
            auto line = [&](const std::string &text) { out += text + "\n"; };
            line(std::string("# ") + schema::kHeaderComment);
            line(std::string("format = \"") + kFormat + "\"");
            line("version = " + std::to_string(kVersion));
            const uint8_t flags = rec[0x01];

            line("");
            line("[name]");
            for (const NameField &f : kNameFields)
                line(std::string(f.key) + " = " + tomlField(rec + f.off, f.width) + trailing(f.key));
            line(std::string("flip = ") + ((flags & 0x80) ? "true" : "false") + trailing("flip"));
            line("space = " + tomlString(schema::kSeparatorNames[flags & 3]) + trailing("space"));
            if (flags & 0x7C)
                line("flags_raw = " + std::to_string(flags) + trailing("flags_raw"));

            line("");
            line("[profile]");
            line("line1 = " + tomlField(rec + 0x20C, 42) + trailing("line1"));
            line("line2 = " + tomlField(rec + 0x236, 42) + trailing("line2"));

            line("");
            line("[parameters]");
            for (const ParamSpec &p : schema::kParams)
            {
                if (p.kind == 1)
                {
                    const unsigned v = rd16(rec + p.off);
                    line(std::string(p.key) + " = { category = " + std::to_string(v >> 8) + ", number = " + std::to_string(v & 0xFF) + " }  # " + p.comment);
                }
                else
                {
                    const long long stored = p.size == 1 ? rec[p.off] : rd16(rec + p.off);
                    line(std::string(p.key) + " = " + shownValue(p, stored) + "  # " + p.comment);
                }
            }

            line("");
            line("[skills]");
            if (extras)
                line("point_budget = " + std::to_string(extras->budget) + trailing("point_budget"));
            for (int which = 0; which < 2; ++which)
            {
                const size_t at = which == 0 ? kOffenseAt : kDefenseAt;
                const char *const *rows = which == 0 ? schema::kOffenseRows : schema::kDefenseRows;
                std::string cells;
                for (int i = 0; i < 12; ++i)
                    cells += (i ? ", " : "") + std::string(rows[i]) + " = " + std::to_string(static_cast<int>(rec[at + static_cast<size_t>(i)]) - kSkillDelta);
                line(std::string(which == 0 ? "offense" : "defense") + " = { " + cells + " }" + trailing(which == 0 ? "offense" : "defense"));
            }

            line("");
            line("[moves]");
            line("# " + commentFor("moves"));
            if (extras)
                line("finisher_name = " + tomlField(extras->finisher, kFinisherSize) + trailing("finisher_name"));
            for (int j = 0; j < kMoveSlots; ++j)
            {
                const unsigned v = rd16(rec + 0x82 + 2 * static_cast<size_t>(j));
                const int number = static_cast<int>(v & 0xFFF);
                std::string parts = "id = " + std::to_string(number);
                std::string moveName;
                if (names && *names && number > 0 && number < kMoveIdMax && (*names)(number, moveName) && !moveName.empty())
                    parts += ", name = " + tomlString(moveName);
                if (v >> 14)
                    parts += ", variant = " + tomlString((v >> 14) == 1 ? "V1" : (v >> 14) == 2 ? "V2" : "V" + std::to_string(v >> 14));
                if ((v >> 12) & 3)
                    parts += ", mark = " + tomlString(((v >> 12) & 3) == 1 ? "S" : ((v >> 12) & 3) == 2 ? "C" : "M" + std::to_string((v >> 12) & 3));
                char key[16];
                std::snprintf(key, sizeof(key), "slot%02d", j);
                line(std::string(key) + " = { " + parts + " }");
            }

            line("");
            line("[cpu_logic]");
            line("# " + commentFor("cpu_logic"));
            {
                size_t at = kLogicAt;
                for (const auto &sit : schema::kSituations)
                {
                    std::string cols;
                    for (int c = 0; c < sit.cols; ++c)
                    {
                        std::string col;
                        int previous = 0;
                        for (int r = 0; r < sit.rows; ++r)
                        {
                            const int cumulative = r + 1 < sit.rows ? rec[at + static_cast<size_t>(r)] : 100;
                            col += (r ? ", " : "") + std::to_string(cumulative - previous);
                            previous = cumulative;
                        }
                        at += static_cast<size_t>(sit.rows - 1);
                        cols += (c ? ", [" : "[") + col + "]";
                    }
                    line(std::string(sit.key) + " = [" + cols + "]");
                }
                line("rest = \"" + hexOf(rec + kLogicAt + kLogicDecoded, kLogicSize - kLogicDecoded) + "\"" + trailing("rest"));
            }

            if (extras)
            {
                const uint8_t *app = extras->appearance;
                line("");
                line("[appearance]");
                line("# " + commentFor("appearance"));
                line("variant = " + std::to_string(rd16(app)) + trailing("variant"));
                constexpr size_t styles = sizeof(schema::kStyleNames) / sizeof(schema::kStyleNames[0]);
                line("style = " + (app[2] < styles ? tomlString(schema::kStyleNames[app[2]]) : std::to_string(app[2])) + trailing("style"));
                line("byte3 = " + std::to_string(app[3]) + trailing("byte3"));
                line("word4 = " + std::to_string(rd16(app + 4)) + trailing("word4"));
                line("rest = \"" + hexOf(app + kAppearanceRestAt, kAppearanceSize - kAppearanceRestAt) + "\"");
            }

            uint8_t pads[kPadBytes];
            size_t n = 0;
            bool any = false;
            for (const auto &pad : kPads)
                for (size_t i = 0; i < pad.size; ++i)
                {
                    pads[n] = rec[pad.at + i];
                    any = any || pads[n] != 0;
                    ++n;
                }
            if (any)
            {
                line("");
                line("[unmapped]");
                line("pad_bytes = \"" + hexOf(pads, kPadBytes) + "\"" + trailing("pad_bytes"));
            }
            return out;
        }

        // ---- reading the text -------------------------------------------------------------------------------------------------------

        using toml::value;

        const char *kindOf(const value &v)
        {
            if (v.is_boolean()) return "true or false";
            if (v.is_integer()) return "a number";
            if (v.is_string()) return "text";
            if (v.is_array()) return "a list";
            if (v.is_table()) return "a table";
            return "something else";
        }

        const value *find(const value &table, const std::string &key)
        {
            return table.contains(key) ? &table.at(key) : nullptr;
        }

        void checkKeys(const value &table, const std::vector<std::string> &allowed, const std::string &where)
        {
            std::vector<std::string> unknown;
            for (const auto &entry : table.as_table())
            {
                if (!entry.first.empty() && entry.first[0] == '_')
                    continue;
                if (std::find(allowed.begin(), allowed.end(), entry.first) == allowed.end())
                    unknown.push_back(entry.first);
            }
            if (!unknown.empty())
            {
                std::sort(unknown.begin(), unknown.end());
                fail((where.empty() ? std::string() : where + ": ") + "unknown key '" + unknown.front() + "' (keys starting with _ are ignored)");
            }
        }

        const value &needTable(const value &doc, const char *name)
        {
            const value *t = find(doc, name);
            if (!t)
                fail(std::string("the table [") + name + "] is missing");
            if (!t->is_table())
                fail(std::string(name) + " must be a table, not " + kindOf(*t));
            return *t;
        }

        long long intIn(const value &table, const std::string &key, long long lo, long long hi, const std::string &where)
        {
            const value *v = find(table, key);
            if (!v)
                fail(where + "." + key + " is missing");
            if (!v->is_integer())
                fail(where + "." + key + ": must be a whole number, not " + kindOf(*v));
            const long long n = v->as_integer();
            if (n < lo || n > hi)
                fail(where + "." + key + ": " + std::to_string(n) + " is out of range (" + std::to_string(lo) + " to " + std::to_string(hi) + ")");
            return n;
        }

        std::string shortText(const std::string &text)
        {
            // the first 24 characters, then "..." (Python: v[:24] + "...")
            size_t chars = 0, i = 0;
            for (; i < text.size(); ++i)
            {
                if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
                {
                    if (chars == 24)
                        break;
                    ++chars;
                }
            }
            return i >= text.size() ? text : text.substr(0, i) + "...";
        }

        // A text field of the file: a string or { hex = "..." } -> zero-padded bytes.
        Bytes textBytes(const value &v, size_t width, int maxChars, const char *label, const std::string &where)
        {
            if (v.is_table())
            {
                const value *hex = find(v, "hex");
                if (!hex || !hex->is_string() || v.as_table().size() != 1)
                    fail(where + ": a value that is not text must be { hex = \"...\" }");
                const std::string &h = hex->as_string();
                if (h.size() % 2 != 0)
                    fail(where + ": the hex value is not an even number of hexadecimal digits");
                Bytes raw;
                for (size_t i = 0; i < h.size(); i += 2)
                {
                    auto digit = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    const int hi = digit(h[i]), lo = digit(h[i + 1]);
                    if (hi < 0 || lo < 0)
                        fail(where + ": the hex value is not an even number of hexadecimal digits");
                    raw.push_back(static_cast<uint8_t>(hi * 16 + lo));
                }
                if (raw.size() > width)
                    fail(where + ": the hex value is " + std::to_string(raw.size()) + " bytes; the " + label + " field holds " + std::to_string(width));
                raw.resize(width, 0);
                return raw;
            }
            if (!v.is_string())
                fail(where + ": must be text or { hex = \"...\" }, not " + kindOf(v));
            std::vector<uint32_t> points;
            std::string why;
            if (!fpr::decodeUtf8(v.as_string(), points, &why))
                fail(where + ": " + why);
            if (static_cast<int>(points.size()) > maxChars)
                fail(where + ": '" + shortText(v.as_string()) + "' has " + std::to_string(points.size()) + " characters; the " + label + " field takes at most " +
                     std::to_string(maxChars));
            Bytes raw;
            if (!fpr::encodeGameText(points, raw, &why))
                fail(where + ": " + why);
            if (raw.size() > width)
                fail(where + ": '" + shortText(v.as_string()) + "' is " + std::to_string(raw.size()) + " bytes in Shift-JIS; the " + label + " field holds " +
                     std::to_string(width));
            raw.resize(width, 0);
            return raw;
        }

        Bytes hexBytes(const value *v, size_t nbytes, const std::string &where)
        {
            Bytes out;
            bool ok = v && v->is_string() && v->as_string().size() == 2 * nbytes;
            if (ok)
            {
                const std::string &h = v->as_string();
                for (size_t i = 0; i < h.size(); i += 2)
                {
                    auto digit = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    const int hi = digit(h[i]), lo = digit(h[i + 1]);
                    if (hi < 0 || lo < 0)
                    {
                        ok = false;
                        break;
                    }
                    out.push_back(static_cast<uint8_t>(hi * 16 + lo));
                }
            }
            if (!ok)
                fail(where + ": must be " + std::to_string(2 * nbytes) + " hexadecimal digits (" + std::to_string(nbytes) + " bytes)");
            return out;
        }

        int enumIndex(const value *v, const std::vector<std::string> &names, const std::string &where, const std::string &what)
        {
            std::string list;
            for (size_t i = 0; i < names.size(); ++i)
                list += (i ? ", " : "") + names[i];
            if (!v->is_string())
                fail(where + ": must be one of " + list + ", not " + kindOf(*v));
            const std::string word = lowerAscii(v->as_string());
            for (size_t i = 0; i < names.size(); ++i)
                if (lowerAscii(names[i]) == word)
                    return static_cast<int>(i);
            fail(where + ": '" + v->as_string() + "' is not " + what + " (" + list + ")");
        }

        std::vector<std::string> namesOf(const ParamSpec &p)
        {
            std::vector<std::string> out;
            for (int i = 0; i < p.nameCount; ++i)
                out.push_back(p.names[i]);
            return out;
        }

        template <size_t N>
        std::vector<std::string> listOf(const char *const (&items)[N])
        {
            return std::vector<std::string>(items, items + N);
        }

        long long parseParam(const ParamSpec &p, const value &table)
        {
            const std::string where = "parameters";
            const value *v = find(table, p.key);
            if (!v)
                fail(std::string("parameters.") + p.key + " is missing");
            if (p.kind == 1)
            {
                const std::string at = std::string("parameters.") + p.key;
                bool shape = v->is_table();
                if (shape)
                {
                    for (const auto &e : v->as_table())
                        if (e.first != "category" && e.first != "number")
                            shape = false;
                    shape = shape && v->contains("category") && v->contains("number");
                }
                if (!shape)
                    fail(at + ": must be { category = 0 to 10, number = 0 to 255 }");
                const long long c = intIn(*v, "category", 0, 10, at);
                const long long n = intIn(*v, "number", 0, 255, at);
                return (c << 8) | n;
            }
            if (p.unknown >= 0 && v->is_string())
            {
                if (lowerAscii(v->as_string()) == p.word)
                    return p.unknown;
                fail(std::string("parameters.") + p.key + ": the only word allowed is \"" + p.word + "\"");
            }
            if (p.names)
                return enumIndex(v, namesOf(p), std::string("parameters.") + p.key, "a value") + p.delta;
            const std::string orWord = p.word ? std::string(" or \"") + p.word + "\"" : std::string();
            if (!v->is_integer())
                fail(where + "." + p.key + ": must be a whole number" + orWord + ", not " + kindOf(*v));
            const long long n = v->as_integer();
            if (n < p.lo || n > p.hi)
                fail(where + "." + p.key + ": " + std::to_string(n) + " is out of range (" + std::to_string(p.lo) + " to " + std::to_string(p.hi) +
                     (p.word ? std::string(", or \"") + p.word + "\"" : std::string()) + ")");
            return n + p.delta;
        }

        std::vector<std::string> tableKeys(const char *table)
        {
            std::vector<std::string> keys;
            const std::string t = table;
            if (t == "name") keys = {"first", "last", "nickname", "flip", "space", "flags_raw"};
            else if (t == "profile") keys = {"line1", "line2"};
            else if (t == "parameters") for (const auto &p : schema::kParams) keys.push_back(p.key);
            else if (t == "skills") keys = {"point_budget", "offense", "defense"};
            else if (t == "moves")
            {
                keys.push_back("finisher_name");
                for (int i = 0; i < kMoveSlots; ++i)
                {
                    char key[16];
                    std::snprintf(key, sizeof(key), "slot%02d", i);
                    keys.push_back(key);
                }
            }
            else if (t == "cpu_logic")
            {
                for (const auto &s : schema::kSituations) keys.push_back(s.key);
                keys.push_back("rest");
            }
            else if (t == "appearance") keys = {"variant", "style", "byte3", "word4", "rest"};
            else if (t == "unmapped") keys = {"pad_bytes"};
            return keys;
        }

        struct Parsed
        {
            Bytes record;                       // 608 bytes
            bool whole = false;
            uint16_t budget = 380;
            Bytes finisher;                     // 40
            Bytes appearance;                   // 448
            std::vector<std::string> warnings;
        };

        Parsed parse(const std::string &fileText, const MoveNames *names, bool whole)
        {
            // a byte order mark (some editors add one) is not part of the file
            const bool bom = fileText.size() >= 3 && static_cast<unsigned char>(fileText[0]) == 0xEF && static_cast<unsigned char>(fileText[1]) == 0xBB &&
                             static_cast<unsigned char>(fileText[2]) == 0xBF;
            const std::string text = bom ? fileText.substr(3) : fileText;
            auto result = toml::try_parse_str(text);
            if (result.is_err())
            {
                std::string title, line;
                const auto &errors = result.unwrap_err();
                if (!errors.empty())
                {
                    title = errors.front().title();
                    if (!errors.front().locations().empty())
                        line = std::to_string(errors.front().locations().front().first.first_line_number());
                }
                const size_t colon = title.find(": ");
                if (colon != std::string::npos && title.compare(0, 6, "toml::") == 0)
                    title = title.substr(colon + 2);
                const size_t newline = title.find('\n');
                if (newline != std::string::npos)
                    title.resize(newline);
                fail("this is not valid TOML (" + (title.empty() ? std::string("syntax error") : title) + (line.empty() ? std::string() : " at line " + line) + ")");
            }
            const value doc = result.unwrap();
            if (!doc.is_table())
                fail("this is not valid TOML (the document is not a table)");

            Parsed out;
            out.whole = whole;
            Bytes &rec = out.record;
            rec.assign(kRecordSize, 0);

            std::vector<std::string> top = {"format", "version"};
            for (const char *t : {"name", "profile", "parameters", "skills", "moves", "cpu_logic", "appearance", "unmapped"})
                top.push_back(t);
            checkKeys(doc, top, std::string());
            {
                const value *format = find(doc, "format");
                if (!format || !format->is_string() || format->as_string() != kFormat)
                    fail(std::string("'format' must be \"") + kFormat + "\"");
                const value *version = find(doc, "version");
                if (!version || !version->is_integer() || version->as_integer() != kVersion)
                    fail("'version' must be " + std::to_string(kVersion) + " (the only version this game reads)");
            }
            for (const char *t : {"name", "profile", "parameters", "skills", "moves", "cpu_logic", "appearance", "unmapped"})
                if (const value *v = find(doc, t); v && !v->is_table())
                    fail(std::string(t) + " must be a table, not " + kindOf(*v));
            if (!whole && find(doc, "appearance"))
                fail("the table [appearance] is not part of a record-only text");

            // [name]
            {
                const value &t = needTable(doc, "name");
                checkKeys(t, tableKeys("name"), "name");
                for (const NameField &f : kNameFields)
                {
                    const value *v = find(t, f.key);
                    if (!v)
                        fail(std::string("name.") + f.key + " is missing");
                    const Bytes raw = textBytes(*v, f.width, f.chars, f.label, std::string("name.") + f.key);
                    std::copy(raw.begin(), raw.end(), rec.begin() + static_cast<std::ptrdiff_t>(f.off));
                }
                const value *flip = find(t, "flip");
                if (!flip)
                    fail("name.flip is missing");
                if (!flip->is_boolean())
                    fail(std::string("name.flip: must be true or false, not ") + kindOf(*flip));
                const value *space = find(t, "space");
                if (!space)
                    fail("name.space is missing");
                const int sep = enumIndex(space, listOf(schema::kSeparatorNames), "name.space", "a separator");
                int flags = (flip->as_boolean() ? 0x80 : 0) | sep;
                if (find(t, "flags_raw"))
                    flags = (static_cast<int>(intIn(t, "flags_raw", 0, 255, "name")) & 0x7C) | flags;
                rec[0x01] = static_cast<uint8_t>(flags);
            }

            // [profile]
            {
                const value &t = needTable(doc, "profile");
                checkKeys(t, tableKeys("profile"), "profile");
                for (const auto &line : {std::make_pair("line1", size_t(0x20C)), std::make_pair("line2", size_t(0x236))})
                {
                    const value *v = find(t, line.first);
                    if (!v)
                        fail(std::string("profile.") + line.first + " is missing");
                    const Bytes raw = textBytes(*v, 42, kProfileChars, "profile line", std::string("profile.") + line.first);
                    std::copy(raw.begin(), raw.end(), rec.begin() + static_cast<std::ptrdiff_t>(line.second));
                }
            }

            // [parameters]
            {
                const value &t = needTable(doc, "parameters");
                checkKeys(t, tableKeys("parameters"), "parameters");
                for (const ParamSpec &p : schema::kParams)
                {
                    const long long stored = parseParam(p, t);
                    if (p.size == 1)
                        rec[static_cast<size_t>(p.off)] = static_cast<uint8_t>(stored);
                    else
                        wr16(&rec[static_cast<size_t>(p.off)], static_cast<uint32_t>(stored));
                }
            }

            // [skills]
            {
                const value &t = needTable(doc, "skills");
                checkKeys(t, tableKeys("skills"), "skills");
                if (find(t, "point_budget"))
                {
                    if (!whole)
                        fail("skills.point_budget is not part of a record-only text");
                    out.budget = static_cast<uint16_t>(intIn(t, "point_budget", 0, 65535, "skills"));
                }
                for (int which = 0; which < 2; ++which)
                {
                    const char *key = which == 0 ? "offense" : "defense";
                    const size_t at = which == 0 ? kOffenseAt : kDefenseAt;
                    const char *const *rows = which == 0 ? schema::kOffenseRows : schema::kDefenseRows;
                    const value *rowTable = find(t, key);
                    if (!rowTable)
                        fail(std::string("skills.") + key + " is missing");
                    if (!rowTable->is_table())
                        fail(std::string("skills.") + key + ": must be a table of the 12 rows, not " + kindOf(*rowTable));
                    checkKeys(*rowTable, std::vector<std::string>(rows, rows + 12), std::string("skills.") + key);
                    for (int i = 0; i < 12; ++i)
                        rec[at + static_cast<size_t>(i)] = static_cast<uint8_t>(intIn(*rowTable, rows[i], 1, 10, std::string("skills.") + key) + kSkillDelta);
                }
            }

            // [moves]
            {
                const value &t = needTable(doc, "moves");
                checkKeys(t, tableKeys("moves"), "moves");
                out.finisher.assign(kFinisherSize, 0);
                if (const value *f = find(t, "finisher_name"))
                {
                    if (!whole)
                        fail("moves.finisher_name is not part of a record-only text");
                    out.finisher = textBytes(*f, kFinisherSize, kFinisherChars, "finisher name", "moves.finisher_name");
                }
                int marksS = 0, marksC = 0;
                for (int j = 0; j < kMoveSlots; ++j)
                {
                    char key[16];
                    std::snprintf(key, sizeof(key), "slot%02d", j);
                    const value *e = find(t, key);
                    if (!e)
                        continue;
                    const std::string where = std::string("moves.") + key;
                    if (!e->is_table())
                        fail(where + ": must be a table like { id = 6 }, not " + kindOf(*e));
                    if (find(*e, "move"))
                        fail(where + ": custom moves (move = \"<mod id>/<name>\") are not supported yet; give the move number as id");
                    checkKeys(*e, {"id", "name", "variant", "mark"}, where);
                    const long long number = intIn(*e, "id", 0, kMoveIdMax, where);
                    long long raw = number;
                    if (const value *variant = find(*e, "variant"))
                        raw |= static_cast<long long>(1 + enumIndex(variant, {"V1", "V2"}, where + ".variant", "a variant")) << 14;
                    if (const value *mark = find(*e, "mark"))
                    {
                        const int m = 1 + enumIndex(mark, {"S", "C"}, where + ".mark", "a mark");
                        raw |= static_cast<long long>(m) << 12;
                        marksS += m == 1;
                        marksC += m == 2;
                    }
                    if (const value *nm = find(*e, "name"))
                    {
                        if (!nm->is_string())
                            fail(where + ".name: must be text, not " + kindOf(*nm));
                        std::string known;
                        if (names && *names && number > 0 && number < kMoveIdMax && (*names)(static_cast<int>(number), known))
                        {
                            auto trimmed = [](std::string s) {
                                size_t b = 0, e2 = s.size();
                                while (b < e2 && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
                                while (e2 > b && std::isspace(static_cast<unsigned char>(s[e2 - 1]))) --e2;
                                return lowerAscii(s.substr(b, e2 - b));
                            };
                            if (trimmed(nm->as_string()) != trimmed(known))
                                out.warnings.push_back(where + ": the name '" + nm->as_string() + "' is not the name of move " + std::to_string(number) +
                                                       " in your game ('" + known + "')");
                        }
                    }
                    wr16(&rec[0x82 + 2 * static_cast<size_t>(j)], static_cast<uint32_t>(raw));
                }
                if (marksS > kMaxS)
                    fail("moves: " + std::to_string(marksS) + " moves are marked S; at most " + std::to_string(kMaxS) + " are allowed");
                if (marksC > kMaxC)
                    fail("moves: " + std::to_string(marksC) + " moves are marked C; only one finisher is allowed");
            }

            // [cpu_logic]
            {
                const value &t = needTable(doc, "cpu_logic");
                checkKeys(t, tableKeys("cpu_logic"), "cpu_logic");
                size_t at = kLogicAt;
                for (const auto &sit : schema::kSituations)
                {
                    const value *cols = find(t, sit.key);
                    if (!cols)
                        fail(std::string("cpu_logic.") + sit.key + " is missing");
                    const std::string where = std::string("cpu_logic.") + sit.key;
                    bool shape = cols->is_array() && static_cast<int>(cols->as_array().size()) == sit.cols;
                    if (shape)
                        for (const value &c : cols->as_array())
                            shape = shape && c.is_array();
                    if (!shape)
                        fail(where + ": must be a list of " + std::to_string(sit.cols) + " columns, each a list of " + std::to_string(sit.rows) + " percentages");
                    int ci = 0;
                    for (const value &c : cols->as_array())
                    {
                        ++ci;
                        bool whole2 = static_cast<int>(c.as_array().size()) == sit.rows;
                        if (whole2)
                            for (const value &x : c.as_array())
                                whole2 = whole2 && x.is_integer();
                        if (!whole2)
                            fail(where + ": column " + std::to_string(ci) + " must be " + std::to_string(sit.rows) + " whole numbers");
                        long long sum = 0;
                        bool inRange = true;
                        for (const value &x : c.as_array())
                        {
                            inRange = inRange && x.as_integer() >= 0 && x.as_integer() <= 100;
                            sum += x.as_integer();
                        }
                        if (!inRange)
                            fail(where + ": column " + std::to_string(ci) + " has a percentage outside 0 to 100");
                        if (sum != 100)
                            fail(where + ": column " + std::to_string(ci) + " adds up to " + std::to_string(sum) + ", not 100");
                        long long run = 0;
                        for (int r = 0; r + 1 < sit.rows; ++r)
                        {
                            run += c.as_array()[static_cast<size_t>(r)].as_integer();
                            rec[at++] = static_cast<uint8_t>(run);
                        }
                    }
                }
                if (!find(t, "rest"))
                    fail("cpu_logic.rest is missing");
                const Bytes rest = hexBytes(find(t, "rest"), kLogicSize - kLogicDecoded, "cpu_logic.rest");
                std::copy(rest.begin(), rest.end(), rec.begin() + static_cast<std::ptrdiff_t>(kLogicAt + kLogicDecoded));
            }

            // [unmapped]
            if (const value *u = find(doc, "unmapped"))
            {
                checkKeys(*u, tableKeys("unmapped"), "unmapped");
                if (find(*u, "pad_bytes"))
                {
                    const Bytes pads = hexBytes(find(*u, "pad_bytes"), kPadBytes, "unmapped.pad_bytes");
                    size_t n = 0;
                    for (const auto &pad : kPads)
                        for (size_t i = 0; i < pad.size; ++i)
                            rec[pad.at + i] = pads[n++];
                }
            }

            // [appearance]
            if (whole)
            {
                const value &t = needTable(doc, "appearance");
                checkKeys(t, tableKeys("appearance"), "appearance");
                out.appearance.assign(kAppearanceSize, 0);
                wr16(&out.appearance[0], static_cast<uint32_t>(intIn(t, "variant", 0, 65535, "appearance")));
                const value *style = find(t, "style");
                if (!style)
                    fail("appearance.style is missing");
                out.appearance[2] = static_cast<uint8_t>(enumIndex(style, listOf(schema::kStyleNames), "appearance.style", "a style"));
                out.appearance[3] = static_cast<uint8_t>(intIn(t, "byte3", 0, 255, "appearance"));
                wr16(&out.appearance[4], static_cast<uint32_t>(intIn(t, "word4", 0, 65535, "appearance")));
                if (!find(t, "rest"))
                    fail("appearance.rest is missing");
                const Bytes rest = hexBytes(find(t, "rest"), kAppearanceSize - kAppearanceRestAt, "appearance.rest");
                std::copy(rest.begin(), rest.end(), out.appearance.begin() + static_cast<std::ptrdiff_t>(kAppearanceRestAt));
            }
            return out;
        }

        Bytes assemble(const Parsed &p)
        {
            Bytes slot(kSlotSize, 0);
            wr16(&slot[0], 1);
            std::copy(p.record.begin(), p.record.end(), slot.begin() + static_cast<std::ptrdiff_t>(kRecordAt));
            wr16(&slot[kBudgetAt], p.budget);
            std::copy(p.finisher.begin(), p.finisher.end(), slot.begin() + static_cast<std::ptrdiff_t>(kFinisherAt));
            std::copy(p.appearance.begin(), p.appearance.end(), slot.begin() + static_cast<std::ptrdiff_t>(kAppearanceAt));
            return slot;
        }
    } // namespace

    bool textToSlot(const std::string &text, const MoveNames *names, Bytes &slot, std::vector<std::string> *warnings, std::string *error)
    {
        try
        {
            const Parsed p = parse(text, names, true);
            slot = assemble(p);
            if (warnings)
                *warnings = p.warnings;
            return true;
        }
        catch (const Failure &f)
        {
            if (error)
                *error = f.message;
        }
        catch (const std::exception &e)
        {
            if (error)
                *error = std::string("this is not a valid edit text (") + e.what() + ")";
        }
        return false;
    }

    bool textToRecord(const std::string &text, const MoveNames *names, Bytes &record, std::vector<std::string> *warnings, std::string *error)
    {
        try
        {
            const Parsed p = parse(text, names, false);
            record = p.record;
            if (warnings)
                *warnings = p.warnings;
            return true;
        }
        catch (const Failure &f)
        {
            if (error)
                *error = f.message;
        }
        catch (const std::exception &e)
        {
            if (error)
                *error = std::string("this is not a valid edit text (") + e.what() + ")";
        }
        return false;
    }

    bool slotToText(const Bytes &slot, const MoveNames *names, std::string &text, std::string *error)
    {
        try
        {
            if (slot.size() != kSlotSize)
                fail("a slot is " + std::to_string(kSlotSize) + " bytes, got " + std::to_string(slot.size()));
            if (rd16(slot.data()) == 0)
                fail("the slot is empty");
            Extras extras;
            extras.budget = rd16(slot.data() + kBudgetAt);
            extras.finisher = slot.data() + kFinisherAt;
            extras.appearance = slot.data() + kAppearanceAt;
            std::string made = emit(slot.data() + kRecordAt, &extras, names);
            Bytes back;
            std::string why;
            if (!textToSlot(made, nullptr, back, nullptr, &why))
                fail("the slot does not survive a round trip through the text (" + why + ")");
            Bytes want = slot;
            wr16(&want[0], 1);
            if (back != want)
                fail("the slot does not survive a round trip through the text (a value is outside what the editor allows)");
            text = std::move(made);
            return true;
        }
        catch (const Failure &f)
        {
            if (error)
                *error = f.message;
        }
        return false;
    }

    bool recordToText(const Bytes &record, const MoveNames *names, std::string &text, std::string *error)
    {
        try
        {
            if (record.size() != kRecordSize)
                fail("a wrestler record is " + std::to_string(kRecordSize) + " bytes, got " + std::to_string(record.size()));
            std::string made = emit(record.data(), nullptr, names);
            Bytes back;
            std::string why;
            if (!textToRecord(made, nullptr, back, nullptr, &why))
                fail("the record does not survive a round trip through the text (" + why + ")");
            if (back != record)
                fail("the record does not survive a round trip through the text");
            text = std::move(made);
            return true;
        }
        catch (const Failure &f)
        {
            if (error)
                *error = f.message;
        }
        return false;
    }

    uint16_t slotGroup(const Bytes &slot)
    {
        return slot.size() >= kGroupAt + 2 ? rd16(slot.data() + kGroupAt) : kNoGroup;
    }

    void setSlotGroup(Bytes &slot, uint16_t group)
    {
        if (slot.size() >= kGroupAt + 2)
            wr16(&slot[kGroupAt], group);
    }

    namespace
    {
        // FIRST and LAST as plain text (empty for a field that is not), in the order the game shows them.
        void shownParts(const Bytes &slot, std::string &a, std::string &b)
        {
            std::string parts[2];
            const size_t offs[2] = {kRecordAt + 0x02, kRecordAt + 0x12};
            const size_t widths[2] = {14, 18};
            for (int i = 0; i < 2; ++i)
            {
                bool isHex = false;
                std::string t = fpr::fieldTextOrHex(slot.data() + offs[i], widths[i], &isHex);
                parts[i] = isHex ? std::string() : t;
            }
            const bool flip = (slot[kRecordAt + 0x01] & 0x80) != 0;
            a = flip ? parts[1] : parts[0];
            b = flip ? parts[0] : parts[1];
        }
    }

    std::string editFileName(int slotIndex, const Bytes &slot)
    {
        std::string a, b;
        if (slot.size() == kSlotSize)
            shownParts(slot, a, b);
        std::string slug;
        bool dash = false;
        for (const char ch : a + " " + b)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            char lower = 0;
            if (c >= 'a' && c <= 'z') lower = static_cast<char>(c);
            else if (c >= 'A' && c <= 'Z') lower = static_cast<char>(c - 'A' + 'a');
            else if (c >= '0' && c <= '9') lower = static_cast<char>(c);
            if (lower)
            {
                if (dash && !slug.empty())
                    slug += '-';
                dash = false;
                slug += lower;
            }
            else
            {
                dash = true;
            }
        }
        if (slug.size() > 24)
            slug.resize(24);
        while (!slug.empty() && slug.back() == '-')
            slug.pop_back();
        char number[16];
        std::snprintf(number, sizeof(number), "%03d", slotIndex);
        return std::string(number) + (slug.empty() ? std::string() : "-" + slug) + ".edit.toml";
    }

    std::string displayName(const Bytes &slot)
    {
        if (slot.size() != kSlotSize)
            return std::string();
        std::string a, b;
        shownParts(slot, a, b);
        if (a.empty() || b.empty())
            return a + b;
        return a + " " + b;
    }

    std::vector<ParameterInfo> parameters()
    {
        std::vector<ParameterInfo> out;
        for (const auto &p : schema::kParams)
        {
            ParameterInfo info;
            info.key = p.key;
            info.voice = p.kind == 1;
            info.lo = p.lo;
            info.hi = p.hi;
            info.delta = p.delta;
            info.unknownStored = p.unknown;
            info.word = p.word ? p.word : "";
            for (int i = 0; i < p.nameCount; ++i)
                info.names.push_back(p.names[i]);
            info.offset = p.off;
            info.size = p.size;
            out.push_back(std::move(info));
        }
        return out;
    }
}
