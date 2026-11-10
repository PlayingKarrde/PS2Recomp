#include "profile.h"

#include "data_pack.h"
#include "mini_json.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <set>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ps2x::mods::profile
{
    namespace fs = std::filesystem;
    using fpr::FieldRange;

    namespace
    {
        std::string utf8(const fs::path &path)
        {
            const auto text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        std::string lowerAscii(std::string text)
        {
            for (char &c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return text;
        }

        bool readFileBytes(const fs::path &path, Bytes &out)
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

        std::string toHex(const Bytes &bytes)
        {
            static const char digits[] = "0123456789abcdef";
            std::string out;
            out.reserve(bytes.size() * 2);
            for (const uint8_t b : bytes)
            {
                out += digits[b >> 4];
                out += digits[b & 15];
            }
            return out;
        }

        bool fromHex(const std::string &text, Bytes &out)
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

        // ---- the ledger ---------------------------------------------------------------------------------------------

        struct LedgerEntry
        {
            std::string pack;
            std::string version;
            Bytes before;
            Bytes written;
            bool operator==(const LedgerEntry &o) const { return pack == o.pack && version == o.version && before == o.before && written == o.written; }
        };
        using LedgerFields = std::map<std::string, LedgerEntry>;

        // The ledger as text, one field per line, in file order.
        std::string ledgerText(const LedgerFields &fields, const std::string &saveSha)
        {
            std::vector<std::pair<std::pair<uint32_t, int>, const std::string *>> order;
            for (const auto &entry : fields)
            {
                FieldRange range;
                if (fpr::fieldRange(entry.first, range))
                    order.push_back({{range.offset, static_cast<int>(range.mask)}, &entry.first});
            }
            std::sort(order.begin(), order.end());
            std::string out = "{\n \"format\": \"fpr-ledger\",\n \"version\": 1,\n \"save_sha256\": " + json::quote(saveSha) + ",\n \"fields\": {\n";
            for (size_t i = 0; i < order.size(); ++i)
            {
                const std::string &key = *order[i].second;
                const LedgerEntry &e = fields.at(key);
                out += "  " + json::quote(key) + ": {\"pack\": " + json::quote(e.pack) + ", \"version\": " + json::quote(e.version) +
                       ", \"before\": \"" + toHex(e.before) + "\", \"written\": \"" + toHex(e.written) + "\"}";
                out += i + 1 < order.size() ? ",\n" : "\n";
            }
            out += " }\n}\n";
            return out;
        }

        // Reads a ledger text. False with *error when it is not a ledger; entries that do not make sense are skipped (and counted).
        bool parseLedger(const std::string &text, LedgerFields &out, std::string &saveSha, size_t &skipped, std::string *error)
        {
            out.clear();
            saveSha.clear();
            skipped = 0;
            json::Value doc;
            std::string why;
            if (!json::parse(text, doc, &why))
            {
                if (error)
                    *error = "not valid JSON (" + why + ")";
                return false;
            }
            const json::Value *format = doc.find("format");
            const json::Value *version = doc.find("version");
            const json::Value *fields = doc.find("fields");
            if (!doc.isObject() || !format || !format->isString() || format->text != "fpr-ledger" || !version || !version->isInt() || version->integer != 1 ||
                !fields || !fields->isObject())
            {
                if (error)
                    *error = "not an fpr-ledger version 1 document";
                return false;
            }
            if (const json::Value *sha = doc.find("save_sha256"))
                if (sha->isString())
                    saveSha = sha->text;
            for (const auto &member : fields->members)
            {
                FieldRange range;
                const json::Value &e = member.second;
                const json::Value *pack = e.find("pack");
                const json::Value *ver = e.find("version");
                const json::Value *before = e.find("before");
                const json::Value *written = e.find("written");
                LedgerEntry entry;
                if (!fpr::fieldRange(member.first, range) || !e.isObject() || !pack || !pack->isString() || !ver || !ver->isString() || !before ||
                    !before->isString() || !written || !written->isString() || !fromHex(before->text, entry.before) ||
                    !fromHex(written->text, entry.written) || entry.before.size() != range.length || entry.written.size() != range.length)
                {
                    ++skipped;
                    continue;
                }
                entry.pack = pack->text;
                entry.version = ver->text;
                out[member.first] = std::move(entry);
            }
            return true;
        }

        // ---- files --------------------------------------------------------------------------------------------------

        bool syncAndClose(FILE *f)
        {
            bool ok = std::fflush(f) == 0;
#if defined(_WIN32)
            ok = ok && _commit(_fileno(f)) == 0;
#else
            ok = ok && fsync(fileno(f)) == 0;
#endif
            return std::fclose(f) == 0 && ok;
        }

        FILE *openForWrite(const fs::path &path)
        {
#if defined(_WIN32)
            return _wfopen(path.c_str(), L"wb");
#else
            return std::fopen(path.c_str(), "wb");
#endif
        }

        bool isScratchName(const fs::path &name)
        {
            const std::string n = lowerAscii(utf8(name));
            return n.size() >= 6 && (n.compare(n.size() - 6, 6, ".mctmp") == 0 || (n.size() >= 4 && n.compare(n.size() - 4, 4, ".tmp") == 0));
        }

        // Copies every file of `from` into `to` (created), each through a temporary file and a rename, the main save last.
        bool copyCardFolder(const fs::path &from, const fs::path &to, std::string *error)
        {
            std::error_code ec;
            fs::create_directories(to, ec);
            std::vector<fs::path> names;
            for (fs::directory_iterator it(from, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            {
                std::error_code typeError;
                if (it->is_regular_file(typeError) && !isScratchName(it->path().filename()))
                    names.push_back(it->path().filename());
            }
            if (ec)
            {
                if (error)
                    *error = "the folder " + utf8(from) + " could not be read (" + ec.message() + ")";
                return false;
            }
            std::stable_sort(names.begin(), names.end(), [](const fs::path &a, const fs::path &b) {
                return (utf8(a) == fpr::kSaveName ? 1 : 0) < (utf8(b) == fpr::kSaveName ? 1 : 0);
            });
            for (const fs::path &name : names)
            {
                Bytes bytes;
                if (!readFileBytes(from / name, bytes) ||
                    !writeFileAtomic(to / name, std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size())))
                {
                    if (error)
                        *error = "the file " + utf8(name) + " could not be copied to " + utf8(to);
                    return false;
                }
            }
            return true;
        }

        std::string stamp()
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char text[32];
            std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
            return text;
        }

        const char *plural(size_t n, const char *one, const char *many) { return n == 1 ? one : many; }

        // A finished or interrupted two-file update: see the header. Looks at ledger.json.new and settles it.
        void settleInterruptedUpdate(const fs::path &profileDir, const fs::path &profileSave, std::vector<std::string> &log)
        {
            const fs::path pending = profileDir / "ledger.json.new";
            std::error_code ec;
            if (!fs::exists(pending, ec))
                return;
            bool promote = false;
            Bytes ledgerBytes, saveBytes;
            if (readFileBytes(pending, ledgerBytes) && readFileBytes(profileSave, saveBytes))
            {
                LedgerFields fields;
                std::string sha;
                size_t skipped = 0;
                if (parseLedger(std::string(ledgerBytes.begin(), ledgerBytes.end()), fields, sha, skipped, nullptr) && !sha.empty())
                    promote = sha == fpr::sha256Hex(saveBytes);
            }
            if (promote)
            {
                fs::rename(pending, profileDir / "ledger.json", ec);
                log.push_back(ec ? "an interrupted profile update could not be finished: " + ec.message() : "finished an interrupted profile update (the save was written, the ledger was not)");
            }
            else
            {
                fs::remove(pending, ec);
                log.push_back("dropped an interrupted profile update (the save was not replaced)");
            }
        }
    }

    bool writeFileAtomic(const fs::path &target, const std::string &bytes)
    {
        std::error_code ec;
        if (target.has_parent_path())
            fs::create_directories(target.parent_path(), ec);
        fs::path temp = target;
        temp += ".tmp";
        FILE *f = openForWrite(temp);
        if (!f)
            return false;
        bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        ok = syncAndClose(f) && ok;
        if (!ok)
        {
            fs::remove(temp, ec);
            return false;
        }
        fs::rename(temp, target, ec);
        if (ec)
        {
            std::error_code ignored;
            fs::remove(temp, ignored);
            return false;
        }
        return true;
    }

    fs::path slot2Root(const fs::path &slot1Root)
    {
        fs::path root = slot1Root.lexically_normal();
        const fs::path parent = root.parent_path();
        const std::string leaf = utf8(root.filename());
        if (lowerAscii(leaf) == "mc0")
            return (parent / "mc1").lexically_normal();
        if (leaf.empty())
            return (root / "mc1").lexically_normal();
        return (parent / fs::path(std::u8string(leaf.begin(), leaf.end()) + u8"_slot1")).lexically_normal();
    }

    std::string baseCardLine()
    {
        return "Memory card: your own card (no data packs are on).";
    }

    bool profileHasSave(const fs::path &profileDir)
    {
        std::error_code ec;
        const fs::path save = profileDir / "mc0" / fpr::kSaveName / fpr::kSaveName;
        return fs::is_regular_file(save, ec) && fs::file_size(save, ec) == fpr::kSaveSize;
    }

    // ---- building the profile -----------------------------------------------------------------------------------------

    Result prepare(const Config &cfg)
    {
        const auto started = std::chrono::steady_clock::now();
        Result r;
        r.root0 = cfg.profileDir / "mc0";
        const fs::path root1 = cfg.profileDir / "mc1";
        const fs::path profileSave = r.root0 / fpr::kSaveName / fpr::kSaveName;
        const fs::path ledgerPath = cfg.profileDir / "ledger.json";
        r.packs.resize(cfg.packs.size());
        for (size_t i = 0; i < cfg.packs.size(); ++i)
            r.packs[i].id = cfg.packs[i].id;
        const std::string cardWord = std::to_string(cfg.packs.size()) + " data " + plural(cfg.packs.size(), "pack", "packs");

        auto finish = [&]() {
            r.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            return r;
        };
        auto stop = [&](const std::string &reason) {
            r.problem = reason;
            r.line = "Memory card: profile card with " + cardWord + " on. " + reason;
            for (PackReport &p : r.packs)
                if (p.error.empty())
                    p.notes.push_back(reason);
            r.log.push_back("profile: " + reason);
            return finish();
        };

        std::error_code ec;
        fs::create_directories(r.root0, ec);
        fs::create_directories(root1, ec);
        if (!fs::is_directory(r.root0, ec))
            return stop("The profile folder " + utf8(r.root0) + " cannot be created, so no data pack was applied.");

        settleInterruptedUpdate(cfg.profileDir, profileSave, r.log);

        // 1. a profile slot with no save gets the base card's files
        bool copied = false;
        for (int slot = 0; slot < 2; ++slot)
        {
            const fs::path from = (slot == 0 ? cfg.baseCard0 : cfg.baseCard1) / fpr::kSaveName;
            const fs::path to = (slot == 0 ? r.root0 : root1) / fpr::kSaveName;
            if (!fs::is_regular_file(to / fpr::kSaveName, ec) && fs::is_regular_file(from / fpr::kSaveName, ec))
            {
                std::string why;
                if (!copyCardFolder(from, to, &why))
                    return stop("The base card's save could not be copied to the profile (" + why + "), so no data pack was applied.");
                copied = true;
                r.log.push_back("profile: copied the base card's slot " + std::to_string(slot + 1) + " save to the profile");
            }
        }

        // 2. no save anywhere: nothing to apply to
        if (!fs::is_regular_file(profileSave, ec))
        {
            r.line = "Memory card: profile card with " + cardWord + " on, but it has no save yet.";
            for (PackReport &p : r.packs)
                p.notes.push_back("No save to apply to yet: save once in the game, then restart.");
            r.log.push_back("profile: no save on the base card or the profile; nothing to apply");
            return finish();
        }
        r.saveFound = true;

        Bytes P;
        if (!readFileBytes(profileSave, P) || P.size() != fpr::kSaveSize)
            return stop("The save on the profile card is not a Fire Pro Wrestling Returns save (" + std::to_string(P.size()) + " bytes), so no data pack was applied.");
        if (!cfg.crcTable)
            return stop("The game's checksum table could not be read (" + (cfg.crcTableError.empty() ? std::string("unknown reason") : cfg.crcTableError) +
                        "), so no data pack was applied.");
        const fpr::CrcTable &table = *cfg.crcTable;
        {
            const fpr::Integrity in = fpr::checkIntegrity(P, table);
            if (!in.allOk())
                return stop("The save on the profile card does not pass the game's own checks, so no data pack was applied.");
            if (!in.markersOk)
                return stop("The save on the profile card is not from this version of the game, so no data pack was applied.");
        }

        // The ledger.
        LedgerFields ledger;
        {
            Bytes text;
            if (readFileBytes(ledgerPath, text))
            {
                std::string sha, why;
                size_t skipped = 0;
                if (!parseLedger(std::string(text.begin(), text.end()), ledger, sha, skipped, &why))
                {
                    fs::path bad = ledgerPath;
                    bad += ".bad";
                    std::error_code renameError;
                    fs::rename(ledgerPath, bad, renameError);
                    ledger.clear();
                    r.log.push_back("profile: the ledger could not be read (" + why + "); starting a new one" + (renameError ? "" : ", the old file is kept as ledger.json.bad"));
                    for (PackReport &p : r.packs)
                        p.notes.push_back("The profile's ledger could not be read, so earlier changes by data packs are now part of the card.");
                }
                else if (skipped)
                {
                    r.log.push_back("profile: " + std::to_string(skipped) + " ledger entries could not be understood and were dropped");
                }
            }
        }

        // 3. split the ledger's fields into the packs' (owned: reverted to "before" in R) and the player's (frozen)
        Bytes R = P;
        std::set<std::string> frozen;
        size_t reverted = 0;
        for (const auto &entry : ledger)
        {
            FieldRange range;
            fpr::fieldRange(entry.first, range);
            if (fpr::readField(P, range) == entry.second.written)
            {
                fpr::writeField(R, range, entry.second.before);
                ++reverted;
            }
            else
            {
                frozen.insert(entry.first);
            }
        }

        // 4. apply every pack in load order
        Bytes W = R;
        std::map<std::string, size_t> provider;                 // field -> the last pack that wrote it
        std::vector<std::vector<std::string>> suppressedBy(cfg.packs.size());
        std::vector<std::vector<std::string>> wroteBy(cfg.packs.size());
        for (size_t i = 0; i < cfg.packs.size(); ++i)
        {
            const PackInput &pack = cfg.packs[i];
            PackReport &report = r.packs[i];
            if (!pack.readError.empty())
            {
                report.error = "data/renames.json: " + pack.readError;
                continue;
            }
            if (!pack.hasFile)
            {
                report.notes.push_back("This data pack has no data/renames.json, so it changes nothing.");
                continue;
            }
            RenamePack parsed;
            std::string why;
            if (!parseRenamePack(pack.renamesJson, parsed, &why))
            {
                report.error = "data/renames.json: " + why;
                continue;
            }
            ApplyOutcome outcome;
            if (!applyRenamePack(parsed, W, &frozen, outcome))
            {
                report.error = "data/renames.json: " + outcome.error;
                continue;
            }
            applyWrites(W, outcome.writes);
            for (const FieldWrite &w : outcome.writes)
            {
                provider[w.key] = i;
                wroteBy[i].push_back(w.key);
            }
            suppressedBy[i] = outcome.suppressed;
        }

        // 5. the new ledger, and the notes
        LedgerFields next;
        for (const auto &p : provider)
        {
            FieldRange range;
            fpr::fieldRange(p.first, range);
            next[p.first] = LedgerEntry{cfg.packs[p.second].id, cfg.packs[p.second].version, fpr::readField(R, range), fpr::readField(W, range)};
        }
        std::map<std::string, size_t> keptBy;                   // frozen field -> the last pack that wanted it
        for (size_t i = 0; i < cfg.packs.size(); ++i)
            for (const std::string &key : suppressedBy[i])
                keptBy[key] = i;
        for (const auto &k : keptBy)
        {
            // The player's change stays in the ledger as it was, so it is still recognised next time; only the pack named changes.
            LedgerEntry e = ledger.at(k.first);
            e.pack = cfg.packs[k.second].id;
            e.version = cfg.packs[k.second].version;
            next[k.first] = std::move(e);
            ++r.packs[k.second].kept;
        }
        for (const auto &p : provider)
            ++r.packs[p.second].written;
        for (size_t i = 0; i < cfg.packs.size(); ++i)
            for (const std::string &key : wroteBy[i])
                if (provider.at(key) != i)
                    ++r.packs[i].overridden;
        for (size_t i = 0; i < cfg.packs.size(); ++i)
        {
            PackReport &report = r.packs[i];
            if (!report.error.empty())
                continue;
            if (report.written)
                report.notes.push_back("Changes " + std::to_string(report.written) + plural(report.written, " field", " fields") + " on the profile card.");
        }
        {
            for (size_t i = 0; i < cfg.packs.size(); ++i)
            {
                size_t shown = 0, total = 0;
                for (const auto &k : keptBy)
                {
                    if (k.second != i)
                        continue;
                    ++total;
                    if (shown < 12)
                    {
                        r.packs[i].notes.push_back("Kept your change to " + fpr::describeField(k.first) + ".");
                        ++shown;
                    }
                }
                if (total > shown)
                    r.packs[i].notes.push_back("And " + std::to_string(total - shown) + " more changes of yours were kept.");
            }
        }

        // 6. write: only when something changed
        bool dataChanged = !std::equal(P.begin(), P.begin() + static_cast<std::ptrdiff_t>(fpr::kDataEnd), W.begin());
        if (dataChanged)
            fpr::fixIntegrity(W, table);
        const bool ledgerChanged = next != ledger;
        if (dataChanged)
        {
            const std::string sha = fpr::sha256Hex(W);
            if (!writeFileAtomic(cfg.profileDir / "ledger.json.new", ledgerText(next, sha)))
                return stop("The profile's ledger could not be written, so no data pack was applied.");
            if (cfg.failpoint == 1)
                return finish();
            if (!writeFileAtomic(profileSave, std::string(reinterpret_cast<const char *>(W.data()), W.size())))
            {
                std::error_code removeError;
                fs::remove(cfg.profileDir / "ledger.json.new", removeError);
                return stop("The profile's save could not be written, so no data pack was applied.");
            }
            if (cfg.failpoint == 2)
                return finish();
            fs::rename(cfg.profileDir / "ledger.json.new", ledgerPath, ec);
            if (ec)
                r.log.push_back("profile: the ledger could not be put in place (" + ec.message() + ")");
            r.imageChanged = true;
        }
        else if (ledgerChanged)
        {
            if (!writeFileAtomic(ledgerPath, ledgerText(next, fpr::sha256Hex(P))))
                r.log.push_back("profile: the ledger could not be written");
        }
        r.applied = true;
        r.line = "Memory card: profile card with " + cardWord + " on (your own card is not changed).";
        r.log.push_back("profile: " + std::to_string(cfg.packs.size()) + " data " + plural(cfg.packs.size(), "pack", "packs") + ", " + std::to_string(next.size()) +
                        " fields owned (" + std::to_string(reverted) + " reverted first, " + std::to_string(frozen.size()) + " held by the player), save " +
                        (dataChanged ? "rewritten" : "unchanged") + (copied ? ", base card copied in" : ""));
        return finish();
    }

    // ---- Keep this card -----------------------------------------------------------------------------------------------

    KeepResult keepCard(const fs::path &profileDir, const fs::path &baseCard0)
    {
        KeepResult result;
        std::error_code ec;
        const fs::path from = profileDir / "mc0" / fpr::kSaveName;
        if (!profileHasSave(profileDir))
        {
            result.message = "The profile card has no save to keep yet. Save once in the game first.";
            return result;
        }
        const fs::path to = baseCard0 / fpr::kSaveName;
        if (fs::is_directory(to, ec))
        {
            fs::path backup = profileDir / ("base-backup-" + stamp());
            for (int n = 2; fs::exists(backup, ec) && n < 100; ++n)
                backup = profileDir / ("base-backup-" + stamp() + "-" + std::to_string(n));
            std::string why;
            if (!copyCardFolder(to, backup / fpr::kSaveName, &why))
            {
                result.message = "Nothing was changed: your own card's files could not be saved first (" + why + ").";
                return result;
            }
            result.backup = backup;
        }
        std::string why;
        if (!copyCardFolder(from, to, &why))
        {
            result.message = "Your own card could not be written (" + why + ")." + (result.backup.empty() ? std::string() : " Its files are saved in " + utf8(result.backup) + ".");
            return result;
        }
        result.ok = true;
        result.message = std::string("Copied the profile card's save to your own card.") +
                         (result.backup.empty() ? "" : " The files it replaced are in " + utf8(result.backup) + ".") + " Restart the game to play from it.";
        return result;
    }
}
