#include "data_pack.h"

#include "mini_json.h"

#include <algorithm>

namespace ps2x::mods
{
    using json::Value;
    using namespace fpr;

    namespace
    {
        bool failWith(std::string *error, std::string message)
        {
            if (error)
                *error = std::move(message);
            return false;
        }

        // The text of a value for an error message: short, and quoted.
        std::string shown(const std::string &text)
        {
            std::string out = "'";
            size_t chars = 0;
            for (size_t i = 0; i < text.size(); ++i)
            {
                if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
                {
                    if (chars == 24)
                    {
                        out += "...";
                        break;
                    }
                    ++chars;
                }
                out += text[i];
            }
            return out + "'";
        }

        bool underscore(const std::string &key) { return !key.empty() && key[0] == '_'; }

        // Every member name of `object` must be in `allowed` (names starting with _ are comments and are skipped).
        bool checkKeys(const Value &object, std::initializer_list<const char *> allowed, const std::string &where, std::string *error)
        {
            for (const auto &member : object.members)
            {
                if (underscore(member.first))
                    continue;
                bool known = false;
                for (const char *name : allowed)
                    if (member.first == name)
                        known = true;
                if (!known)
                {
                    std::string list;
                    for (const char *name : allowed)
                        list += (list.empty() ? "" : ", ") + std::string(name);
                    return failWith(error, where + ": unknown key " + shown(member.first) + " (this entry takes: " + list + "; keys starting with _ are ignored)");
                }
            }
            return true;
        }

        // "12" -> 12; digits only, in [0, limit).
        bool parseIndexKey(const std::string &key, int limit, const std::string &what, int &out, std::string *error)
        {
            int v = 0;
            bool ok = !key.empty() && key.size() <= 4;
            for (const char c : key)
            {
                if (c < '0' || c > '9')
                    ok = false;
                else
                    v = v * 10 + (c - '0');
            }
            if (!ok)
                return failWith(error, what + " " + shown(key) + " is not a number");
            if (v >= limit)
                return failWith(error, what + " " + std::to_string(v) + " is out of range (0 to " + std::to_string(limit - 1) + ")");
            out = v;
            return true;
        }

        bool parseHex(const std::string &text, Bytes &out)
        {
            if (text.size() % 2 != 0)
                return false;
            out.clear();
            auto digit = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            for (size_t i = 0; i < text.size(); i += 2)
            {
                const int hi = digit(text[i]), lo = digit(text[i + 1]);
                if (hi < 0 || lo < 0)
                    return false;
                out.push_back(static_cast<uint8_t>(hi * 16 + lo));
            }
            return true;
        }

        // A name field: text (full-width in the save) or {"hex": "..."} (the bytes as they are).
        bool parseText(const Value &v, uint32_t width, int maxChars, const char *label, const std::string &where, Bytes &out, std::string *error)
        {
            if (v.isString())
            {
                std::vector<uint32_t> points;
                std::string why;
                if (!decodeUtf8(v.text, points, &why))
                    return failWith(error, where + ": " + why);
                if (static_cast<int>(points.size()) > maxChars)
                    return failWith(error, where + ": " + shown(v.text) + " has " + std::to_string(points.size()) + " characters; the " + label +
                                               " field takes at most " + std::to_string(maxChars));
                Bytes bytes;
                if (!encodeGameText(points, bytes, &why))
                    return failWith(error, where + ": " + why);
                if (bytes.size() > width)
                    return failWith(error, where + ": " + shown(v.text) + " is " + std::to_string(bytes.size()) + " bytes in Shift-JIS; the " + label +
                                               " field holds " + std::to_string(width));
                bytes.resize(width, 0);
                out = std::move(bytes);
                return true;
            }
            if (v.isObject())
            {
                const Value *hex = v.find("hex");
                if (!hex || !hex->isString() || v.members.size() != 1)
                    return failWith(error, where + ": a value that is not text must be {\"hex\": \"...\"}");
                Bytes bytes;
                if (!parseHex(hex->text, bytes))
                    return failWith(error, where + ": the hex value is not an even number of hexadecimal digits");
                if (bytes.size() > width)
                    return failWith(error, where + ": the hex value is " + std::to_string(bytes.size()) + " bytes; the " + label + " field holds " +
                                               std::to_string(width));
                bytes.resize(width, 0);
                out = std::move(bytes);
                return true;
            }
            return failWith(error, where + ": must be text or {\"hex\": \"...\"}, not " + v.describe());
        }

        bool parseInt(const Value &v, int lo, int hi, const std::string &where, int &out, std::string *error)
        {
            if (!v.isInt())
                return failWith(error, where + ": must be a whole number, not " + v.describe());
            if (v.integer < lo || v.integer > hi)
                return failWith(error, where + ": " + std::to_string(v.integer) + " is out of range (" + std::to_string(lo) + " to " + std::to_string(hi) + ")");
            out = static_cast<int>(v.integer);
            return true;
        }

        bool parseRecords(const Value *section, bool isOrg, std::vector<RenamePack::Record> &out, std::string *error)
        {
            if (!section)
                return true;
            const char *name = isOrg ? "organisations" : "groups";
            const char *word = isOrg ? "organisation" : "group";
            if (!section->isObject())
                return failWith(error, std::string(name) + ": must be an object of numbers, not " + section->describe());
            const int limit = isOrg ? kOrgs : kGroups;
            for (const auto &member : section->members)
            {
                if (underscore(member.first))
                    continue;
                const std::string where = std::string(name) + "." + member.first;
                RenamePack::Record rec;
                if (!parseIndexKey(member.first, limit, std::string(name) + " " + word, rec.index, error))
                    return false;
                for (const auto &seen : out)
                    if (seen.index == rec.index)
                        return failWith(error, where + ": " + word + " " + std::to_string(rec.index) + " appears twice");
                const Value &e = member.second;
                if (!e.isObject())
                    return failWith(error, where + ": must be an object, not " + e.describe());
                if (isOrg ? !checkKeys(e, {"name", "short", "created"}, where, error) : !checkKeys(e, {"name", "short", "org", "kind", "created"}, where, error))
                    return false;
                if (const Value *v = e.find("name"))
                {
                    Bytes b;
                    if (!parseText(*v, kWidthOrgLong, kCharsName, "name", where + ".name", b, error))
                        return false;
                    rec.name = std::move(b);
                }
                if (const Value *v = e.find("short"))
                {
                    Bytes b;
                    if (!parseText(*v, kWidthOrgShort, kCharsShort, "short name", where + ".short", b, error))
                        return false;
                    rec.shortName = std::move(b);
                }
                if (!isOrg)
                {
                    if (const Value *v = e.find("org"))
                    {
                        int n = 0;
                        if (!parseInt(*v, 0, kOrgs - 1, where + ".org", n, error))
                            return false;
                        rec.org = n;
                    }
                    if (const Value *v = e.find("kind"))
                    {
                        int n = 0;
                        if (!parseInt(*v, 0, 2, where + ".kind", n, error))
                            return false;
                        rec.kind = n;
                    }
                }
                if (const Value *v = e.find("created"))
                {
                    if (!v->isBool())
                        return failWith(error, where + ".created: must be true or false, not " + v->describe());
                    rec.created = v->boolean;
                }
                out.push_back(std::move(rec));
            }
            std::sort(out.begin(), out.end(), [](const RenamePack::Record &a, const RenamePack::Record &b) { return a.index < b.index; });
            return true;
        }

        bool parseOrder(const Value *v, bool isOrg, std::optional<std::vector<int>> &out, std::string *error)
        {
            if (!v)
                return true;
            const char *name = isOrg ? "organisation_order" : "group_order";
            const int limit = isOrg ? kOrgs : kGroups;
            if (!v->isArray())
                return failWith(error, std::string(name) + ": must be a list of numbers, not " + v->describe());
            std::vector<int> list;
            std::vector<bool> seen(static_cast<size_t>(limit), false);
            for (size_t i = 0; i < v->items.size(); ++i)
            {
                int n = 0;
                if (!parseInt(v->items[i], 0, limit - 1, std::string(name) + "[" + std::to_string(i) + "]", n, error))
                    return false;
                if (seen[static_cast<size_t>(n)])
                    return failWith(error, std::string(name) + ": " + std::to_string(n) + " is listed twice");
                seen[static_cast<size_t>(n)] = true;
                list.push_back(n);
            }
            out = std::move(list);
            return true;
        }
    }

    bool parseRenamePack(const std::string &jsonText, RenamePack &out, std::string *error)
    {
        out = RenamePack();
        Value doc;
        std::string why;
        if (!json::parse(jsonText, doc, &why))
            return failWith(error, "this is not valid JSON (" + why + ")");
        if (!doc.isObject())
            return failWith(error, std::string("the document must be an object, not ") + doc.describe());
        const Value *format = doc.find("format");
        if (!format || !format->isString() || format->text != "fpr-rename")
            return failWith(error, "'format' must be \"fpr-rename\"");
        const Value *version = doc.find("version");
        if (!version || !version->isInt() || version->integer != 1)
            return failWith(error, "'version' must be 1 (the only version this game reads)");
        for (const auto &member : doc.members)
        {
            if (underscore(member.first))
                continue;
            static const char *known[] = {"format", "version", "game", "base_sha1", "source", "wrestlers", "organisations", "groups",
                                          "organisation_order", "group_order", "appearance"};
            if (std::find_if(std::begin(known), std::end(known), [&](const char *k) { return member.first == k; }) == std::end(known))
                return failWith(error, "unknown key " + shown(member.first) + " (keys starting with _ are ignored)");
        }
        if (const Value *v = doc.find("game"))
            if (!v->isString())
                return failWith(error, std::string("'game' must be text, not ") + v->describe());
        if (const Value *v = doc.find("base_sha1"))
            if (!v->isString())
                return failWith(error, std::string("'base_sha1' must be text, not ") + v->describe());

        RenamePack pack;
        if (const Value *section = doc.find("wrestlers"))
        {
            if (!section->isObject())
                return failWith(error, std::string("wrestlers: must be an object of slot numbers, not ") + section->describe());
            for (const auto &member : section->members)
            {
                if (underscore(member.first))
                    continue;
                const std::string where = "wrestlers." + member.first;
                RenamePack::Wrestler w;
                if (!parseIndexKey(member.first, kWrestlers, "wrestler slot", w.slot, error))
                    return false;
                for (const auto &seen : pack.wrestlers)
                    if (seen.slot == w.slot)
                        return failWith(error, where + ": slot " + std::to_string(w.slot) + " appears twice");
                const Value &e = member.second;
                if (!e.isObject())
                    return failWith(error, where + ": must be an object, not " + e.describe());
                if (!checkKeys(e, {"first", "last", "nickname", "flip", "space", "flags_raw"}, where, error))
                    return false;
                struct TextField
                {
                    const char *name;
                    std::optional<Bytes> RenamePack::Wrestler::*member;
                    uint32_t width;
                    int chars;
                    const char *label;
                };
                static const TextField fields[] = {{"first", &RenamePack::Wrestler::first, kWidthFirst, kCharsFirst, "FIRST"},
                                                   {"last", &RenamePack::Wrestler::last, kWidthLast, kCharsLast, "LAST"},
                                                   {"nickname", &RenamePack::Wrestler::nickname, kWidthNickname, kCharsNickname, "NICKNAME"}};
                for (const TextField &f : fields)
                {
                    if (const Value *v = e.find(f.name))
                    {
                        Bytes b;
                        if (!parseText(*v, f.width, f.chars, f.label, where + "." + f.name, b, error))
                            return false;
                        w.*(f.member) = std::move(b);
                    }
                }
                if (const Value *v = e.find("flip"))
                {
                    if (!v->isBool())
                        return failWith(error, where + ".flip: must be true or false, not " + v->describe());
                    w.flip = v->boolean;
                }
                if (const Value *v = e.find("space"))
                {
                    int n = 0;
                    if (!parseInt(*v, 0, 3, where + ".space", n, error))
                        return false;
                    w.space = n;
                }
                if (const Value *v = e.find("flags_raw"))
                {
                    int n = 0;
                    if (!parseInt(*v, 0, 255, where + ".flags_raw", n, error))
                        return false;
                    w.flagsRaw = n;
                }
                pack.wrestlers.push_back(std::move(w));
            }
            std::sort(pack.wrestlers.begin(), pack.wrestlers.end(), [](const RenamePack::Wrestler &a, const RenamePack::Wrestler &b) { return a.slot < b.slot; });
        }
        if (!parseRecords(doc.find("organisations"), true, pack.orgs, error) || !parseRecords(doc.find("groups"), false, pack.groups, error))
            return false;
        if (!parseOrder(doc.find("organisation_order"), true, pack.orgOrder, error) || !parseOrder(doc.find("group_order"), false, pack.groupOrder, error))
            return false;
        if (const Value *section = doc.find("appearance"))
        {
            if (!section->isObject())
                return failWith(error, std::string("appearance: must be an object of slot numbers, not ") + section->describe());
            for (const auto &member : section->members)
            {
                if (underscore(member.first))
                    continue;
                const std::string where = "appearance." + member.first;
                int slot = 0;
                if (!parseIndexKey(member.first, kWrestlers, "appearance slot", slot, error))
                    return false;
                for (const auto &seen : pack.appearance)
                    if (seen.first == slot)
                        return failWith(error, where + ": slot " + std::to_string(slot) + " appears twice");
                if (!member.second.isString())
                    return failWith(error, where + ": must be the record as hexadecimal text, not " + member.second.describe());
                Bytes bytes;
                if (!parseHex(member.second.text, bytes))
                    return failWith(error, where + ": not an even number of hexadecimal digits");
                if (bytes.size() != kAppearanceStride)
                    return failWith(error, where + ": the record must be " + std::to_string(kAppearanceStride) + " bytes, not " + std::to_string(bytes.size()));
                pack.appearance.emplace_back(slot, std::move(bytes));
            }
            std::sort(pack.appearance.begin(), pack.appearance.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        }
        out = std::move(pack);
        return true;
    }

    // ---- applying --------------------------------------------------------------------------------------------------------

    bool applyRenamePack(const RenamePack &pack, const Bytes &image, const std::set<std::string> *frozen, ApplyOutcome &out)
    {
        out = ApplyOutcome();
        if (image.size() != kSaveSize)
        {
            out.error = "the save is " + std::to_string(image.size()) + " bytes, not " + std::to_string(kSaveSize);
            return false;
        }
        Bytes work = image;
        auto emit = [&](const std::string &key, Bytes value) {
            if (frozen && frozen->count(key))
            {
                out.suppressed.push_back(key);
                return;
            }
            FieldRange range;
            if (!fieldRange(key, range))
                return; // cannot happen: every key is built from a checked index
            writeField(work, range, value);
            out.writes.push_back(FieldWrite{key, std::move(value)});
        };
        auto fail = [&](std::string message) {
            out.error = std::move(message);
            out.writes.clear();
            return false;
        };
        auto u16 = [](int v) { return Bytes{static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF)}; };

        // 1. wrestlers: name parts, then the flags byte as three masked fields (fprsave.py reads and rewrites the one byte;
        //    flip owns bit 7, space bits 0-1, and bits 2-6 only matter when the file gives the whole byte).
        for (const auto &w : pack.wrestlers)
        {
            const std::string base = "w" + std::to_string(w.slot);
            if (w.first) emit(base + ".first", *w.first);
            if (w.last) emit(base + ".last", *w.last);
            if (w.nickname) emit(base + ".nick", *w.nickname);
            if (w.flip || w.flagsRaw)
                emit(base + ".flip", Bytes{static_cast<uint8_t>(w.flip ? (*w.flip ? 0x80 : 0x00) : (*w.flagsRaw & 0x80))});
            if (w.space || w.flagsRaw)
                emit(base + ".space", Bytes{static_cast<uint8_t>(w.space ? *w.space : (*w.flagsRaw & 0x03))});
            if (w.flagsRaw)
                emit(base + ".flags", Bytes{static_cast<uint8_t>(*w.flagsRaw & 0x7C)});
        }

        // 2. organisations, then groups. A record that is not in use in the save is ADDED, the way the game's Create Org. / Create
        //    Group does it (the whole record is written, so every byte of it is owned by a field).
        std::vector<int> added[2];
        for (int pass = 0; pass < 2; ++pass)
        {
            const bool isOrg = pass == 0;
            const auto &records = isOrg ? pack.orgs : pack.groups;
            const char *word = isOrg ? "organisation" : "group";
            for (const auto &e : records)
            {
                const std::string base = std::string(isOrg ? "o" : "g") + std::to_string(e.index);
                const bool wasLive = isOrg ? orgLive(image, e.index) : groupLive(image, e.index);
                if (!wasLive)
                {
                    if (!e.name || !e.shortName)
                        return fail(std::string(word) + " " + std::to_string(e.index) + " is not in this save, so the pack must give both a name and a short name");
                    if (!isOrg && !e.org)
                        return fail("group " + std::to_string(e.index) + " is not in this save, so the pack must give its organisation (org)");
                    emit(base + ".live", u16(1));
                    if (isOrg)
                        emit(base + ".id", u16(0xFFFF));
                    added[pass].push_back(e.index);
                }
                if (e.name) emit(base + ".name", *e.name);
                if (e.shortName) emit(base + ".short", *e.shortName);
                if (!isOrg)
                {
                    if (e.org)
                    {
                        if (!orgLive(work, *e.org))
                            return fail("group " + std::to_string(e.index) + ": organisation " + std::to_string(*e.org) + " is not in use in this save");
                        emit(base + ".org", u16(*e.org));
                    }
                    std::optional<int> kind = e.kind;
                    if (!kind && !wasLive)
                        kind = 1; // the game's default for a new group: Neutral
                    if (kind)
                        emit(base + ".kind", u16(*kind));
                }
            }
        }

        // 3. the list order arrays: rewritten when the file gives an order or adds records (new ones go last).
        for (int pass = 0; pass < 2; ++pass)
        {
            const bool isOrg = pass == 0;
            const int count = isOrg ? kOrgs : kGroups;
            const uint32_t offset = isOrg ? kOffOrgOrder : kOffGroupOrder;
            const char *word = isOrg ? "organisation" : "group";
            auto live = [&](const Bytes &img, int i) { return isOrg ? orgLive(img, i) : groupLive(img, i); };
            std::vector<int> used;
            for (int i = 0; i < count; ++i)
                if (live(work, i))
                    used.push_back(i);
            std::vector<int> prefix;
            const auto &given = isOrg ? pack.orgOrder : pack.groupOrder;
            if (given)
            {
                prefix = *given;
                std::vector<int> sorted = prefix;
                std::sort(sorted.begin(), sorted.end());
                if (sorted != used)
                {
                    std::string list;
                    for (const int i : used)
                        list += (list.empty() ? "" : ", ") + std::to_string(i);
                    return fail(std::string(isOrg ? "organisation_order" : "group_order") + " must list each " + word + " in use exactly once (in use: " + list + ")");
                }
            }
            else if (!added[pass].empty())
            {
                std::vector<bool> seen(static_cast<size_t>(count), false);
                for (int k = 0; k < count; ++k)
                {
                    const int x = image[offset + static_cast<uint32_t>(k)];
                    if (x < count && live(image, x) && live(work, x))
                    {
                        if (seen[static_cast<size_t>(x)])
                            return fail(std::string("the save's ") + word + " list order lists " + std::to_string(x) + " twice");
                        seen[static_cast<size_t>(x)] = true;
                        prefix.push_back(x);
                    }
                }
                for (const int a : added[pass])
                    prefix.push_back(a);
            }
            else
            {
                continue;
            }
            std::vector<bool> inPrefix(static_cast<size_t>(count), false);
            for (const int x : prefix)
                inPrefix[static_cast<size_t>(x)] = true;
            Bytes value;
            for (const int x : prefix)
                value.push_back(static_cast<uint8_t>(x));
            for (int i = 0; i < count; ++i)
                if (!inPrefix[static_cast<size_t>(i)])
                    value.push_back(static_cast<uint8_t>(i));
            if (value.size() != static_cast<size_t>(count))
                return fail(std::string("the ") + word + " list order does not come to " + std::to_string(count) + " entries");
            emit(isOrg ? "o.order" : "g.order", std::move(value));
        }

        // 4. appearance records.
        for (const auto &a : pack.appearance)
            emit("a" + std::to_string(a.first), a.second);

        out.ok = true;
        return true;
    }

    void applyWrites(Bytes &image, const std::vector<FieldWrite> &writes)
    {
        for (const auto &w : writes)
        {
            FieldRange range;
            if (fieldRange(w.key, range))
                writeField(image, range, w.value);
        }
    }
}
