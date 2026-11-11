#include "edit_pack.h"

#include "edit_text.h"
#include "fpr_save.h"
#include "mod_manager.h"
#include "mod_package.h"

#include <algorithm>
#include <set>

namespace ps2x::mods::edit
{
    namespace
    {
        std::string lowerAscii(std::string text)
        {
            for (char &c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return text;
        }
    }

    std::vector<Placement> planSlots(const std::vector<bool> &used, const std::vector<SlotClaim> &claims, const std::vector<Wanted> &wanted)
    {
        std::vector<Placement> out(wanted.size());
        std::set<int> given;     // slots handed to an edit in this run
        std::set<int> reserved;  // slots held for a kept edit
        auto isUsed = [&](int k) { return k >= 0 && k < static_cast<int>(used.size()) && used[static_cast<size_t>(k)]; };

        // 1. edits the ledger already knows
        for (size_t i = 0; i < wanted.size(); ++i)
        {
            for (const SlotClaim &c : claims)
            {
                if (c.pack != wanted[i].pack || c.file != wanted[i].file || c.slot < 0 || c.slot >= fpr::kEditSlotCount)
                    continue;
                if (given.count(c.slot) || reserved.count(c.slot))
                    continue; // another edit already has it (cannot happen with one claim per slot and per edit)
                if (c.held)
                {
                    out[i] = Placement{true, c.slot, true};
                    reserved.insert(c.slot);
                }
                else if (!isUsed(c.slot))
                {
                    out[i] = Placement{true, c.slot, false};
                    given.insert(c.slot);
                }
                break;
            }
        }
        // 2. new edits take the lowest free slot
        for (size_t i = 0; i < wanted.size(); ++i)
        {
            if (out[i].placed)
                continue;
            for (int k = 0; k < fpr::kEditSlotCount; ++k)
            {
                if (isUsed(k) || given.count(k) || reserved.count(k))
                    continue;
                // a slot the ledger holds for an edit that is not asked for now is free: it was put back to its "before" bytes already
                out[i] = Placement{true, k, false};
                given.insert(k);
                break;
            }
        }
        return out;
    }

    bool isEditFileName(const std::string &name)
    {
        static const std::string suffix = ".edit.toml";
        const std::string lower = lowerAscii(name);
        return lower.size() > suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    void readEditFiles(const Manager &manager, const std::string &id, profile::PackInput &pack)
    {
        const auto info = manager.find(id);
        if (!info || !info->valid)
            return;
        const PackageResult opened = readPackage(info->path);
        if (!opened.package)
            return;
        static const std::string folder = "data/edits/";
        std::vector<std::string> names;
        for (const PackageEntry &entry : opened.package->entries)
        {
            if (entry.isDirectory || entry.name.compare(0, folder.size(), folder) != 0)
                continue;
            const std::string rest = entry.name.substr(folder.size());
            if (rest.find('/') != std::string::npos || !isEditFileName(rest))
                continue;
            names.push_back(entry.name);
        }
        std::sort(names.begin(), names.end());
        size_t taken = 0;
        for (const std::string &path : names)
        {
            profile::EditFile file;
            file.name = path.substr(folder.size());
            if (taken == static_cast<size_t>(fpr::kEditSlotCount))
            {
                file.readError = "a data pack can hold at most " + std::to_string(fpr::kEditSlotCount) + " edits, the number of edit slots";
                pack.edits.push_back(std::move(file));
                break;
            }
            ++taken;
            std::string why;
            const auto bytes = manager.readFile(id, path, kMaxEditBytes, &why);
            if (bytes)
                file.text.assign(bytes->begin(), bytes->end());
            else
                file.readError = why.empty() ? "the file could not be read" : why;
            pack.edits.push_back(std::move(file));
        }
    }
}
