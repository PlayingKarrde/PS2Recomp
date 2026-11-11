#include "save_import.h"

#include "data_pack.h"
#include "mini_json.h"
#include "mod_manager.h"
#include "mod_package.h"
#include "path_text.h"
#include "profile.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace ps2x::mods::import
{
    namespace fs = std::filesystem;
    using namespace fpr;

    namespace
    {
        constexpr const char *kBaseName = "my memory card"; // "source.base" of every pack made here: the player's own card, no path
        constexpr uint32_t kOffSortOrder = kRoster + 4;
        constexpr uint32_t kOffReferees = kRoster + 0x21A0;
        constexpr uint32_t kOffWrestlerGroup = kRoster + 0x4748;
        constexpr uint32_t kRosterSize = 0x2E1B8;
        constexpr uint32_t kOffEditSlots = kRoster + kRosterSize;
        constexpr int kEditSlots = 500;
        constexpr uint32_t kEditStride = 0x44C;

        std::string utf8(const fs::path &path)
        {
            const auto text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        fs::path fromUtf8(const std::string &text)
        {
            return fs::path(std::u8string(text.begin(), text.end()));
        }

        std::string withCommas(size_t n)
        {
            std::string digits = std::to_string(n), out;
            for (size_t i = 0; i < digits.size(); ++i)
            {
                if (i != 0 && (digits.size() - i) % 3 == 0)
                    out += ',';
                out += digits[i];
            }
            return out;
        }

        const char *plural(size_t n, const char *one, const char *many) { return n == 1 ? one : many; }

        std::string countPhrase(size_t n, const char *one, const char *many) { return std::to_string(n) + " " + plural(n, one, many); }

        // The bytes of a field up to its first zero.
        Bytes trimmed(const Bytes &image, size_t offset, size_t width)
        {
            size_t n = 0;
            while (n < width && image[offset + n] != 0)
                ++n;
            return Bytes(image.begin() + static_cast<std::ptrdiff_t>(offset), image.begin() + static_cast<std::ptrdiff_t>(offset + n));
        }

        // ---- the records, as tools/fprsave.py parse_save sees them --------------------------------------------------------------

        struct NameRecord
        {
            Bytes first, last, nick; // trimmed
            uint8_t flags = 0;
            bool operator==(const NameRecord &o) const { return first == o.first && last == o.last && nick == o.nick && flags == o.flags; }
        };

        NameRecord readName(const Bytes &image, int k)
        {
            const size_t o = kOffNames + static_cast<size_t>(k) * kNameStride;
            NameRecord r;
            r.flags = image[o];
            r.first = trimmed(image, o + 1, kWidthFirst);
            r.last = trimmed(image, o + 15, kWidthLast);
            r.nick = trimmed(image, o + 33, kWidthNickname);
            return r;
        }

        struct Record // an organisation (a = id word) or a group (a = organisation, kind)
        {
            uint16_t flag = 0;
            Bytes name, shortName;
            uint16_t a = 0, kind = 0;
            bool operator==(const Record &o) const { return flag == o.flag && name == o.name && shortName == o.shortName && a == o.a && kind == o.kind; }
        };

        Record readRecord(const Bytes &image, bool isOrg, int i)
        {
            const size_t o = (isOrg ? kOffOrgs : kOffGroups) + static_cast<size_t>(i) * (isOrg ? kOrgStride : kGroupStride);
            Record r;
            r.flag = readU16(image, o);
            r.name = trimmed(image, o + 2, kWidthOrgLong);
            r.shortName = trimmed(image, o + 28, kWidthOrgShort);
            r.a = readU16(image, o + 42);
            r.kind = isOrg ? 0 : readU16(image, o + 44);
            return r;
        }

        bool recordLive(const Bytes &image, bool isOrg, int i) { return isOrg ? orgLive(image, i) : groupLive(image, i); }

        // A deleted record that still holds data.
        bool recordDead(const Bytes &image, bool isOrg, int i)
        {
            const size_t stride = isOrg ? kOrgStride : kGroupStride;
            const size_t o = (isOrg ? kOffOrgs : kOffGroups) + static_cast<size_t>(i) * stride;
            if (readU16(image, o) != 0)
                return false;
            for (size_t k = 0; k < stride; ++k)
                if (image[o + k])
                    return true;
            return false;
        }

        // The list order of the records in use: the order array's entries, skipping records not in use.
        std::vector<int> orderOfLive(const Bytes &image, bool isOrg)
        {
            const uint32_t off = isOrg ? kOffOrgOrder : kOffGroupOrder;
            const int count = isOrg ? kOrgs : kGroups;
            std::vector<int> out;
            for (int k = 0; k < count; ++k)
            {
                const int x = image[off + static_cast<uint32_t>(k)];
                if (x < count && recordLive(image, isOrg, x))
                    out.push_back(x);
            }
            return out;
        }

        bool slotUsed(const Bytes &image, int slot) { return readU16(image, kOffEditSlots + static_cast<size_t>(slot) * kEditStride) != 0; }
        Bytes slotBytes(const Bytes &image, int slot)
        {
            const size_t o = kOffEditSlots + static_cast<size_t>(slot) * kEditStride;
            return Bytes(image.begin() + static_cast<std::ptrdiff_t>(o), image.begin() + static_cast<std::ptrdiff_t>(o + kEditStride));
        }

        // ---- JSON text, the way tools/fprsave.py dump_pack writes it ------------------------------------------------------------

        // A text field: plain text when it stores back to the same bytes, otherwise {"hex": "..."}.
        std::string textValue(const Bytes &image, size_t offset, size_t width)
        {
            bool isHex = false;
            const std::string t = fieldTextOrHex(image.data() + offset, width, &isHex);
            return isHex ? "{\"hex\": \"" + t + "\"}" : json::quote(t);
        }

        std::string intList(const std::vector<int> &values)
        {
            std::string out = "[";
            for (size_t i = 0; i < values.size(); ++i)
                out += (i ? ", " : "") + std::to_string(values[i]);
            return out + "]";
        }

        // `"key": {` lines `}` for a non-empty object, `"key": {}` for an empty one.
        std::string objectBlock(const std::string &key, const std::vector<std::string> &lines)
        {
            if (lines.empty())
                return " " + json::quote(key) + ": {}";
            std::string out = " " + json::quote(key) + ": {\n";
            for (size_t i = 0; i < lines.size(); ++i)
                out += "  " + lines[i] + (i + 1 < lines.size() ? ",\n" : "\n");
            return out + " }";
        }

        std::string toHex(const uint8_t *p, size_t n)
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

        std::string indexList(const std::vector<int> &values)
        {
            std::string out;
            for (size_t i = 0; i < values.size(); ++i)
                out += (i ? ", " : "") + std::to_string(values[i]);
            return out;
        }
    }

    bool isGameFolder(const std::string &folder)
    {
        return folder == kFolderUs || folder == kFolderJp || folder == kFolderEu;
    }

    const char *regionOf(const std::string &folder)
    {
        if (folder == kFolderUs) return "USA";
        if (folder == kFolderJp) return "Japan";
        if (folder == kFolderEu) return "Europe";
        return "";
    }

    // ---- the pack ----------------------------------------------------------------------------------------------------------

    bool extractRenames(const Bytes &save, const Bytes &base, bool withAppearance, const std::string &sourceFile, const std::string &sourceBase,
                        Extracted &out, std::string *error)
    {
        out = Extracted();
        if (save.size() != kSaveSize || base.size() != kSaveSize)
        {
            if (error)
                *error = "a save must be " + withCommas(kSaveSize) + " bytes";
            return false;
        }
        std::vector<std::string> blocks;
        blocks.push_back(" \"format\": \"fpr-rename\"");
        blocks.push_back(" \"version\": 1");
        blocks.push_back(" \"game\": " + json::quote(kGameName));
        blocks.push_back(" \"base_sha1\": " + json::quote(sha1Hex(base)));

        // wrestlers
        {
            std::vector<std::string> lines;
            for (int k = 0; k < kWrestlers; ++k)
            {
                if (readName(base, k) == readName(save, k))
                    continue;
                const size_t o = kOffNames + static_cast<size_t>(k) * kNameStride;
                const uint8_t flags = save[o];
                std::string e = "{\"first\": " + textValue(save, o + 1, kWidthFirst) + ", \"last\": " + textValue(save, o + 15, kWidthLast) +
                                ", \"nickname\": " + textValue(save, o + 33, kWidthNickname) + ", \"flip\": " + ((flags & 0x80) ? "true" : "false") +
                                ", \"space\": " + std::to_string(flags & 3);
                if (flags & 0x7C)
                    e += ", \"flags_raw\": " + std::to_string(flags);
                e += "}";
                lines.push_back(json::quote(std::to_string(k)) + ": " + e);
            }
            out.counts.wrestlers = lines.size();
            blocks.push_back(objectBlock("wrestlers", lines));
        }

        // organisations, then groups
        std::vector<int> created[2];
        for (int pass = 0; pass < 2; ++pass)
        {
            const bool isOrg = pass == 0;
            const int count = isOrg ? kOrgs : kGroups;
            const size_t stride = isOrg ? kOrgStride : kGroupStride;
            const size_t off0 = isOrg ? kOffOrgs : kOffGroups;
            std::vector<std::string> lines;
            for (int i = 0; i < count; ++i)
            {
                const bool inBase = recordLive(base, isOrg, i), inSave = recordLive(save, isOrg, i);
                if (!inBase && !inSave)
                    continue;
                const Record ra = inBase ? readRecord(base, isOrg, i) : Record();
                const Record rb = inSave ? readRecord(save, isOrg, i) : Record();
                if (inBase && inSave && ra == rb)
                    continue;
                if (!inSave)
                {
                    out.leftOut.push_back(std::string(isOrg ? "organisation" : "group") + " " + std::to_string(i) + " is in use in the base but deleted in the save");
                    continue;
                }
                const size_t o = off0 + static_cast<size_t>(i) * stride;
                std::string e = "{\"name\": " + textValue(save, o + 2, kWidthOrgLong) + ", \"short\": " + textValue(save, o + 28, kWidthOrgShort);
                if (!inBase)
                {
                    e += ", \"created\": true";
                    created[pass].push_back(i);
                    if (!isOrg)
                        e += ", \"org\": " + std::to_string(rb.a) + ", \"kind\": " + std::to_string(rb.kind);
                }
                else if (!isOrg)
                {
                    if (ra.a != rb.a)
                        e += ", \"org\": " + std::to_string(rb.a);
                    if (ra.kind != rb.kind)
                        e += ", \"kind\": " + std::to_string(rb.kind);
                }
                e += "}";
                lines.push_back(json::quote(std::to_string(i)) + ": " + e);
            }
            (isOrg ? out.counts.organisations : out.counts.groups) = lines.size();
            blocks.push_back(objectBlock(isOrg ? "organisations" : "groups", lines));
        }

        // The list order: written only when the plain rule (the base's order for what is still in use, then the new ones) would not give it.
        for (int pass = 0; pass < 2; ++pass)
        {
            const bool isOrg = pass == 0;
            const std::vector<int> theirs = orderOfLive(save, isOrg);
            std::vector<int> byDefault;
            for (const int x : orderOfLive(base, isOrg))
                if (recordLive(save, isOrg, x))
                    byDefault.push_back(x);
            std::vector<int> add = created[pass];
            std::sort(add.begin(), add.end());
            byDefault.insert(byDefault.end(), add.begin(), add.end());
            if (theirs != byDefault)
            {
                blocks.push_back(std::string(" ") + json::quote(isOrg ? "organisation_order" : "group_order") + ": " + intList(theirs));
                out.counts.orderLists = true;
            }
        }

        // notes about what a rename file cannot say
        for (int pass = 0; pass < 2; ++pass)
        {
            const bool isOrg = pass == 0;
            const int count = isOrg ? kOrgs : kGroups;
            std::vector<int> differing;
            for (int i = 0; i < count; ++i)
            {
                const bool a = recordDead(base, isOrg, i), b = recordDead(save, isOrg, i);
                if ((a || b) && (a != b || !(readRecord(base, isOrg, i) == readRecord(save, isOrg, i))))
                    differing.push_back(i);
            }
            if (!differing.empty())
                out.leftOut.push_back(std::to_string(differing.size()) + " deleted " + (isOrg ? "organisation" : "group") + " record(s) differ (" +
                                      indexList(differing) + "); the game never shows them");
        }
        {
            size_t moved = 0;
            for (int k = 0; k < kWrestlers; ++k)
                if (readU16(base, kOffWrestlerGroup + 2 * static_cast<size_t>(k)) != readU16(save, kOffWrestlerGroup + 2 * static_cast<size_t>(k)))
                    ++moved;
            if (moved)
                out.leftOut.push_back(std::to_string(moved) + " wrestler(s) belong to another group");
        }

        // appearance records
        {
            std::vector<std::string> lines;
            std::vector<int> slots;
            for (int k = 0; k < kWrestlers; ++k)
            {
                const size_t o = kOffAppearance + static_cast<size_t>(k) * kAppearanceStride;
                if (std::memcmp(base.data() + o, save.data() + o, kAppearanceStride) != 0)
                {
                    slots.push_back(k);
                    lines.push_back(json::quote(std::to_string(k)) + ": \"" + toHex(save.data() + o, kAppearanceStride) + "\"");
                }
            }
            if (withAppearance)
            {
                out.counts.appearance = lines.size();
                blocks.push_back(objectBlock("appearance", lines));
            }
            else if (!slots.empty())
            {
                out.leftOut.push_back("appearance records of slots " + indexList(slots) + " (colour/style edits; use --appearance)");
            }
        }

        blocks.push_back(" \"source\": {\"file\": " + json::quote(sourceFile) + ", \"base\": " + json::quote(sourceBase) + "}");

        std::string text = "{\n";
        for (size_t i = 0; i < blocks.size(); ++i)
            text += blocks[i] + (i + 1 < blocks.size() ? ",\n" : "\n");
        text += "}\n";
        out.json = std::move(text);
        return true;
    }

    bool extractEdits(const Bytes &save, const Bytes &base, const edit::MoveNames *names, Extracted &out, std::string *error)
    {
        out.edits.clear();
        out.counts.edits = 0;
        if (save.size() != kSaveSize || base.size() != kSaveSize)
        {
            if (error)
                *error = "a save must be " + withCommas(kSaveSize) + " bytes";
            return false;
        }
        std::map<Bytes, int> have;
        for (int s = 0; s < kEditSlots; ++s)
            if (slotUsed(base, s))
                ++have[slotBytes(base, s)];
        for (int s = 0; s < kEditSlots; ++s)
        {
            if (!slotUsed(save, s))
                continue;
            const Bytes slot = slotBytes(save, s);
            const auto it = have.find(slot);
            if (it != have.end() && it->second > 0)
            {
                --it->second; // the base has this wrestler already
                continue;
            }
            std::string text, why;
            if (!edit::slotToText(slot, names, text, &why))
            {
                out.leftOut.push_back("the created wrestler in edit slot " + std::to_string(s) + " cannot be written as text (" + why + ")");
                continue;
            }
            out.edits.push_back(EditPack{edit::editFileName(s, slot), std::move(text)});
        }
        out.counts.edits = out.edits.size();
        return true;
    }

    bool unmatchedChanges(const Bytes &save, const Bytes &base, const std::string &renamesJson, Unmatched &out, std::string *error)
    {
        return unmatchedChanges(save, base, renamesJson, std::vector<EditPack>(), out, error);
    }

    bool unmatchedChanges(const Bytes &save, const Bytes &base, const std::string &renamesJson, const std::vector<EditPack> &edits, Unmatched &out,
                          std::string *error)
    {
        out = Unmatched();
        RenamePack pack;
        std::string why;
        if (!parseRenamePack(renamesJson, pack, &why))
        {
            if (error)
                *error = "data/renames.json: " + why;
            return false;
        }
        ApplyOutcome outcome;
        if (!applyRenamePack(pack, base, nullptr, outcome))
        {
            if (error)
                *error = "data/renames.json: " + outcome.error;
            return false;
        }
        Bytes made = base;
        applyWrites(made, outcome.writes);

        std::vector<std::pair<std::string, size_t>> found;
        auto rawDiffers = [&](size_t from, size_t to) {
            return std::memcmp(save.data() + from, made.data() + from, to - from) != 0;
        };

        // the organisations and groups: record by record, ignoring deleted ones; and the list order of the records in use
        for (int pass = 0; pass < 2; ++pass)
        {
            const bool isOrg = pass == 0;
            const int count = isOrg ? kOrgs : kGroups;
            size_t records = 0;
            for (int i = 0; i < count; ++i)
            {
                const bool inSave = recordLive(save, isOrg, i), inMade = recordLive(made, isOrg, i);
                if (!inSave && !inMade)
                    continue;
                if (inSave != inMade || !(readRecord(save, isOrg, i) == readRecord(made, isOrg, i)))
                    ++records;
            }
            if (records)
                found.push_back({countPhrase(records, isOrg ? "organisation" : "group", isOrg ? "organisations" : "groups"), records});
            if (orderOfLive(save, isOrg) != orderOfLive(made, isOrg))
                found.push_back({std::string(isOrg ? "the organisation" : "the group") + " list order", 1});
        }

        size_t wrestlers = 0;
        for (int k = 0; k < kWrestlers; ++k)
            if (!(readName(save, k) == readName(made, k)))
                ++wrestlers;
        if (wrestlers)
            found.push_back({countPhrase(wrestlers, "wrestler name", "wrestler names"), wrestlers});

        size_t memberships = 0;
        for (int k = 0; k < kWrestlers; ++k)
            if (readU16(save, kOffWrestlerGroup + 2 * static_cast<size_t>(k)) != readU16(made, kOffWrestlerGroup + 2 * static_cast<size_t>(k)))
                ++memberships;
        if (memberships)
            found.push_back({countPhrase(memberships, "wrestler group", "wrestler groups"), memberships});

        size_t looks = 0;
        for (int k = 0; k < kWrestlers; ++k)
        {
            const size_t o = kOffAppearance + static_cast<size_t>(k) * kAppearanceStride;
            if (std::memcmp(save.data() + o, made.data() + o, kAppearanceStride) != 0)
                ++looks;
        }
        if (looks)
            found.push_back({countPhrase(looks, "appearance record", "appearance records"), looks});

        // The created wrestlers are matched as a set, not slot by slot: an edit takes the lowest free slot of the card it is applied to.
        size_t slots = 0;
        {
            std::map<Bytes, int> have;
            for (int s = 0; s < kEditSlots; ++s)
                if (slotUsed(made, s))
                    ++have[slotBytes(made, s)];
            for (const EditPack &e : edits)
            {
                Bytes slot;
                if (!edit::textToSlot(e.text, nullptr, slot, nullptr, nullptr))
                    continue; // the profile would refuse this file too: it carries nothing
                const uint16_t group = edit::slotGroup(slot);
                if (group != edit::kNoGroup && (group >= kGroups || !groupLive(made, group)))
                    edit::setSlotGroup(slot, edit::kNoGroup);
                ++have[slot];
            }
            for (int s = 0; s < kEditSlots; ++s)
            {
                if (!slotUsed(save, s))
                    continue;
                const auto it = have.find(slotBytes(save, s));
                if (it != have.end() && it->second > 0)
                    --it->second;
                else
                    ++slots;
            }
        }
        if (slots)
            found.push_back({countPhrase(slots, "created wrestler (edit slot)", "created wrestlers (edit slots)"), slots});

        const size_t editEnd = kOffEditSlots + static_cast<size_t>(kEditSlots) * kEditStride;
        const size_t namesEnd = kOffNames + static_cast<size_t>(kWrestlers) * kNameStride;
        const size_t appearanceEnd = kOffAppearance + static_cast<size_t>(kWrestlers) * kAppearanceStride;
        // The 827 wrestler ids at the start of the roster blob are the order the editor lists them in. Created wrestlers sit among the
        // built-ins there, so only a different order of the built-in ids counts (a pack cannot carry it).
        auto builtinOrder = [&](const Bytes &image) {
            std::vector<uint16_t> ids;
            for (size_t i = 0; i < 827; ++i)
            {
                const uint16_t id = readU16(image, kOffSortOrder + 2 * i);
                if (id < 0x2000)
                    ids.push_back(id);
            }
            return ids;
        };
        if (builtinOrder(save) != builtinOrder(made) || rawDiffers(namesEnd, kOffAppearance))
            found.push_back({"roster tables", 1});
        if (rawDiffers(kOffReferees, kOffWrestlerGroup))
            found.push_back({"referees", 1});
        if (rawDiffers(appearanceEnd, kOffEditSlots))
            found.push_back({"rings and other tables", 1});
        if (rawDiffers(editEnd, kDataEnd))
            found.push_back({"other records", 1});

        for (const auto &f : found)
        {
            out.areas.push_back(f.first);
            out.units += f.second;
        }
        return true;
    }

    // ---- the package -------------------------------------------------------------------------------------------------------

    std::string manifestToml(const PackInfo &info)
    {
        std::string out;
        out += "id = " + tomlString(info.id) + "\n";
        out += "name = " + tomlString(info.name) + "\n";
        out += "version = " + tomlString(info.version) + "\n";
        out += "authors = [" + tomlString(info.author) + "]\n";
        out += "description = " + tomlString(info.description) + "\n";
        out += "game = \"SLUS_217.02\"\n";
        out += "api = 1\n";
        out += "kind = \"data\"\n";
        return out;
    }

    std::string slugOf(const std::string &fileName)
    {
        const fs::path stemPath = fromUtf8(fileName).stem();
        const std::string stem = utf8(stemPath);
        std::string slug;
        bool pendingDash = false;
        for (const char ch : stem)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            char lower = 0;
            if (c >= 'a' && c <= 'z') lower = static_cast<char>(c);
            else if (c >= 'A' && c <= 'Z') lower = static_cast<char>(c - 'A' + 'a');
            else if (c >= '0' && c <= '9') lower = static_cast<char>(c);
            if (lower)
            {
                if (pendingDash && !slug.empty())
                    slug += '-';
                pendingDash = false;
                slug += lower;
            }
            else
            {
                pendingDash = true;
            }
        }
        if (slug.size() > 40)
            slug.resize(40);
        while (!slug.empty() && slug.back() == '-')
            slug.pop_back();
        return slug.empty() ? std::string("save") : slug;
    }

    std::string descriptionFor(const std::string &fileName, const Counts &counts)
    {
        std::vector<std::string> parts;
        if (counts.wrestlers) parts.push_back(countPhrase(counts.wrestlers, "wrestler", "wrestlers"));
        if (counts.organisations) parts.push_back(countPhrase(counts.organisations, "organisation", "organisations"));
        if (counts.groups) parts.push_back(countPhrase(counts.groups, "group", "groups"));
        if (counts.appearance) parts.push_back(countPhrase(counts.appearance, "appearance record", "appearance records"));
        if (counts.edits) parts.push_back(countPhrase(counts.edits, "created wrestler", "created wrestlers"));
        std::string list;
        for (size_t i = 0; i < parts.size(); ++i)
            list += (i == 0 ? "" : (i + 1 == parts.size() ? " and " : ", ")) + parts[i];
        return "Imported from " + shortenEnd(fileName, 120) + ": " + list + ". While it is on, the game plays from a copy of your memory card with these applied; your own card is not changed.";
    }

    // ---- the region gate ---------------------------------------------------------------------------------------------------

    GateResult checkSave(const SaveEntry &entry, const CrcTable &table)
    {
        GateResult r;
        const std::string where = entry.folder.empty() ? std::string("The save") : "The save in " + entry.folder;
        if (!entry.problem.empty())
        {
            r.line = entry.problem; // the reader's own one-line reason (a damaged folder on a card image)
            return r;
        }
        if (entry.save.empty())
        {
            r.line = entry.folder.empty() ? std::string("That file holds no save data.") : "The folder " + entry.folder + " holds no save file.";
            return r;
        }
        if (entry.save.size() != kSaveSize)
        {
            r.line = where + " is " + withCommas(entry.save.size()) + " bytes; a Fire Pro Wrestling Returns save is " + withCommas(kSaveSize) + " bytes.";
            return r;
        }
        const Integrity in = checkIntegrity(entry.save, table);
        if (!in.allOk())
        {
            r.line = where + " does not pass the game's own checks (" +
                     (in.badBlocks.empty() ? std::string("its parity data is wrong") : std::to_string(in.badBlocks.size()) + " of " + std::to_string(kBlocks) + " blocks are wrong")
                     + "), so it is damaged or not from this game.";
            return r;
        }
        if (!in.markersOk)
        {
            r.line = where + " is not from this version of the game (the game would show ERROR OLD VERSION SAVEDATA).";
            return r;
        }
        r.ok = true;
        return r;
    }

    // ---- the whole import --------------------------------------------------------------------------------------------------

    namespace
    {
        bool readFile(const fs::path &path, Bytes &out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            in.seekg(0, std::ios::end);
            const std::streamoff size = in.tellg();
            if (size < 0)
                return false;
            in.seekg(0, std::ios::beg);
            out.resize(static_cast<size_t>(size));
            if (size > 0)
                in.read(reinterpret_cast<char *>(out.data()), size);
            return static_cast<bool>(in) || in.eof();
        }

        Outcome refuse(Outcome o, const std::string &line)
        {
            o.ok = false;
            o.message = line;
            o.log.push_back("import: refused: " + line);
            return o;
        }

        // The player's own state without packs (see the header, step 3).
        bool loadBase(const Env &env, const CrcTable &table, Bytes &base, std::string &from, std::string &why)
        {
            std::error_code ec;
            if (!env.profileDir.empty() && profile::profileHasSave(env.profileDir))
            {
                if (!profile::playerSave(env.profileDir, table, base, &why))
                    return false;
                from = "the profile card without data packs";
                return true;
            }
            const fs::path save = env.baseCard0 / kSaveName / kSaveName;
            if (env.baseCard0.empty() || !fs::is_regular_file(save, ec))
            {
                why = "There is no save on your memory card to compare with. Save once in the game, then import.";
                return false;
            }
            if (!readFile(save, base) || base.size() != kSaveSize)
            {
                why = "The save on your own memory card is not a Fire Pro Wrestling Returns save (" + std::to_string(base.size()) + " bytes), so a save cannot be compared with it.";
                return false;
            }
            const Integrity in = checkIntegrity(base, table);
            if (!in.allOk() || !in.markersOk)
            {
                why = "The save on your own memory card does not pass the game's own checks, so a save cannot be compared with it.";
                return false;
            }
            from = "your memory card";
            return true;
        }
    }

    Outcome importEntries(const Env &env, const std::vector<SaveEntry> &entries, const std::string &sourceName, const std::string &chosenFolder)
    {
        Outcome o;
        if (!env.manager)
            return refuse(o, "Mods are not available in this build.");

        // 1. which of the entries could be this game's
        std::vector<const SaveEntry *> candidates;
        for (const SaveEntry &e : entries)
            if (e.folder.empty() || isGameFolder(e.folder))
                candidates.push_back(&e);
        if (entries.empty())
            return refuse(o, "There is no save in that file.");
        if (candidates.empty())
        {
            std::string names;
            size_t shown = 0;
            for (const SaveEntry &e : entries)
            {
                if (shown == 3)
                {
                    names += ", ...";
                    break;
                }
                names += (shown++ ? ", " : "") + e.folder;
            }
            return refuse(o, "That file has no Fire Pro Wrestling Returns save (its save folder is " + std::string(kFolderUs) + ", " + kFolderJp + " or " + kFolderEu +
                                 "). It holds: " + names + ".");
        }

        // 2. the integrity data needs the game's checksum table
        CrcTable loaded{};
        const CrcTable *table = env.crcTable;
        if (!table)
        {
            std::string why;
            if (env.loadCrcTable && env.loadCrcTable(loaded, &why))
                table = &loaded;
            else
                return refuse(o, "The game's checksum table could not be read (" + (why.empty() ? std::string("unknown reason") : why) + "), so a save cannot be checked.");
        }

        // 3. the region gate
        std::vector<const SaveEntry *> passing;
        std::string firstProblem;
        for (const SaveEntry *e : candidates)
        {
            const GateResult gate = checkSave(*e, *table);
            if (gate.ok)
                passing.push_back(e);
            else if (firstProblem.empty())
                firstProblem = gate.line;
        }
        if (passing.empty())
            return refuse(o, firstProblem);
        const SaveEntry *chosen = nullptr;
        if (!chosenFolder.empty())
        {
            for (const SaveEntry *e : passing)
                if (e->folder == chosenFolder)
                    chosen = e;
            if (!chosen)
                return refuse(o, "That file no longer holds a save in " + chosenFolder + ".");
        }
        else
        {
            // Identical saves under two folder names are one save.
            std::vector<const SaveEntry *> distinct;
            for (const SaveEntry *e : passing)
                if (std::none_of(distinct.begin(), distinct.end(), [&](const SaveEntry *d) { return d->save == e->save; }))
                    distinct.push_back(e);
            if (distinct.size() > 1)
            {
                o.needsChoice = true;
                for (const SaveEntry *e : distinct)
                    o.choices.push_back(Choice{e->folder, regionOf(e->folder)});
                std::string list;
                for (size_t i = 0; i < o.choices.size(); ++i)
                    list += (i ? ", " : "") + o.choices[i].folder;
                o.message = "That file holds " + std::to_string(distinct.size()) + " saves of the game (" + list + "). Which one do you want to import?";
                o.log.push_back("import: " + o.message);
                return o;
            }
            chosen = distinct.front();
        }
        o.region = regionOf(chosen->folder);
        o.log.push_back("import: " + sourceName + ": save " + (chosen->folder.empty() ? std::string("(no folder name)") : chosen->folder) + " from the " + chosen->format +
                        " reader, " + std::to_string(chosen->save.size()) + " bytes, passes the game's checks");

        // 4. the comparison base
        Bytes base;
        std::string baseFrom, why;
        if (!loadBase(env, *table, base, baseFrom, why))
            return refuse(o, why);
        o.log.push_back("import: comparing with " + baseFrom);

        // 5. the pack
        const std::string fileName = utf8(fromUtf8(sourceName).filename());
        Extracted pack;
        if (!extractRenames(chosen->save, base, true, fileName, kBaseName, pack, &why))
            return refuse(o, why);
        if (!extractEdits(chosen->save, base, nullptr, pack, &why))
            return refuse(o, why);
        o.counts = pack.counts;
        for (const std::string &note : pack.leftOut)
            o.log.push_back("import: left out: " + note);
        std::string tail;
        {
            Unmatched unmatched;
            if (!unmatchedChanges(chosen->save, base, pack.json, pack.edits, unmatched, &why))
                return refuse(o, "That save's changes cannot be applied to your card (" + why + ").");
            o.unmatched = unmatched;
            o.otherChanges = unmatched.any();
        }
        if (o.otherChanges)
        {
            std::string list;
            for (size_t i = 0; i < o.unmatched.areas.size(); ++i)
                list += (i ? ", " : "") + o.unmatched.areas[i];
            o.log.push_back("import: other changes in the save that a pack cannot carry yet: " + list);
            tail = " This save also has other changes that packs cannot carry yet.";
        }
        std::string regionNote;
        std::string kind;
        if (o.region == "Japan" || o.region == "Europe")
        {
            kind = o.region == "Japan" ? "Japanese" : "European";
            regionNote = " from a " + kind + " save";
        }
        if (pack.empty())
        {
            o.nothingToImport = true;
            o.message = "Nothing to import: that save has the same names, organisations, groups and looks as your card." + tail;
            o.log.push_back("import: " + o.message);
            return o;
        }

        PackInfo info;
        info.name = shortenEnd(utf8(fromUtf8(fileName).stem()), 60);
        for (char &c : info.name)
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
                c = ' ';
        if (info.name.empty())
            info.name = "Imported save";
        info.description = descriptionFor(fileName, pack.counts);
        const std::string slug = slugOf(fileName);
        std::string id = "import." + slug;
        int copy = 1; // the same file imported again gets a second id and a name that tells the two apart: "x (2)"
        for (int n = 2; env.manager->find(id) && n < 1000; ++n)
        {
            id = "import." + slug + "-" + std::to_string(n);
            copy = n;
        }
        if (copy > 1)
            info.name = shortenEnd(info.name, 56) + " (" + std::to_string(copy) + ")";
        info.id = id;
        std::string idWhy;
        if (!validModId(info.id, &idWhy))
            return refuse(o, "The mod id " + info.id + " is not valid (" + idWhy + ").");

        std::string packageError;
        std::vector<std::pair<std::string, std::string>> packageFiles = {{"mod.toml", manifestToml(info)}};
        if (pack.counts.renames())
            packageFiles.push_back({"data/renames.json", pack.json});
        for (const EditPack &e : pack.edits)
            packageFiles.push_back({"data/edits/" + e.file, e.text});
        const auto package = buildPackage(packageFiles, &packageError);
        if (!package)
            return refuse(o, "The pack could not be built (" + packageError + ").");
        fs::path scratch = env.scratchDir.empty() ? fs::temp_directory_path() / "fpwr-import" : env.scratchDir;
        std::error_code ec;
        fs::create_directories(scratch, ec);
        const fs::path packagePath = scratch / (info.id + ".fprmod");
        if (!writeBytesToFile(packagePath, *package))
            return refuse(o, "The pack could not be written to " + utf8(scratch) + ".");
        const Result installed = env.manager->install(packagePath);
        fs::remove(packagePath, ec);
        fs::remove(scratch, ec); // only if empty
        if (!installed.ok)
            return refuse(o, installed.message);
        o.modKey = installed.key.empty() ? info.id : installed.key;
        const Result enabled = env.manager->setEnabled(o.modKey, true);
        o.turnedOn = enabled.ok;
        o.ok = true;

        std::string parts;
        auto addPart = [&](size_t n, const char *one, const char *many) {
            if (n)
                parts += (parts.empty() ? "" : ", ") + countPhrase(n, one, many);
        };
        addPart(pack.counts.wrestlers, "wrestler", "wrestlers");
        addPart(pack.counts.organisations, "organisation", "organisations");
        addPart(pack.counts.groups, "group", "groups");
        addPart(pack.counts.appearance, "appearance record", "appearance records");
        addPart(pack.counts.edits, "created wrestler", "created wrestlers");
        o.message = "Imported " + parts + regionNote + ". ";
        o.message += o.turnedOn ? "Restart the game to use it." : "It is installed but could not be turned on: " + enabled.message;
        o.message += tail;
        // Lines for the pack's page in the Mods tab ("On your memory card:"), which has room for what the status line cannot say.
        o.notes.push_back("Imported from " + shortenEnd(fileName, 60) + (kind.empty() ? std::string() : " (a " + kind + " save: only the US release's layout has been checked)") + ".");
        if (o.otherChanges)
        {
            std::string list;
            for (size_t i = 0; i < o.unmatched.areas.size(); ++i)
                list += (i ? ", " : "") + o.unmatched.areas[i];
            o.notes.push_back("This save also has other changes that packs cannot carry yet: " + list + ".");
        }
        o.log.push_back("import: installed " + o.modKey + " (" + parts + ")" + (o.turnedOn ? ", turned on" : ", NOT turned on: " + enabled.message));
        return o;
    }

    Outcome importFile(const Env &env, const fs::path &file, const std::string &chosenFolder)
    {
        const auto started = std::chrono::steady_clock::now();
        auto elapsed = [&]() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(); };
        std::vector<SaveEntry> entries;
        std::string error;
        if (!readSaveContainer(file, entries, error))
        {
            Outcome o;
            o.message = error.empty() ? std::string("That file could not be read as a save.") : error;
            o.milliseconds = elapsed();
            o.log.push_back("import: " + utf8(file) + ": " + o.message);
            return o;
        }
        Outcome o = importEntries(env, entries, utf8(file), chosenFolder);
        o.milliseconds = elapsed();
        char text[64];
        std::snprintf(text, sizeof(text), "import: took %.1f ms", o.milliseconds);
        o.log.push_back(text);
        return o;
    }
}
