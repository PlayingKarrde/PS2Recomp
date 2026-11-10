// Data packs (mod kind "data"): what a pack's files say and what applying them does to a save image.
//
// The first data file is data/renames.json, an "fpr-rename" version 1 document (the format of tools/fprsave.py renames; the rules
// are in docs/formats/fprmod.md "Data packs"). parseRenamePack reads and checks one; applyRenamePack turns it into a list of FIELD
// WRITES against a save image. A field is a named range of the save (fpr_save.h); the profile code (profile.h) owns fields through
// its ledger, so what a pack does has to be expressed field by field, not as one blob.
//
// applyRenamePack is the C++ twin of tools/fprsave.py apply_renames: for the same save and the same rename file it writes the same
// bytes (a unit test checks this on the community rename set). Where Python was lenient this is strict, and says so in one line.
//
// No SDL, no game code. Pure functions.
#pragma once

#include "fpr_save.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ps2x::mods
{
    using Bytes = std::vector<uint8_t>;

    // A rename file after parsing and checking: every text is already its zero-padded field bytes.
    struct RenamePack
    {
        struct Wrestler
        {
            int slot = 0;
            std::optional<Bytes> first, last, nickname; // 14, 18 and 24 bytes
            std::optional<bool> flip;
            std::optional<int> space;    // 0 blank, 1 dot, 2 equals sign, 3 nothing
            std::optional<int> flagsRaw; // the whole flags byte; flip and space override its bits
        };
        struct Record // an organisation or a group
        {
            int index = 0;
            std::optional<Bytes> name, shortName; // 26 and 14 bytes
            std::optional<int> org;               // groups only
            std::optional<int> kind;              // groups only: 0 Baby, 1 Neutral, 2 Heel
            bool created = false;                 // informational: whether the author's save added the record
        };

        std::vector<Wrestler> wrestlers; // by slot
        std::vector<Record> orgs;        // by index
        std::vector<Record> groups;
        std::optional<std::vector<int>> orgOrder, groupOrder;
        std::vector<std::pair<int, Bytes>> appearance; // slot, 448 bytes

        // Counts of what the file mentions, for the log.
        size_t entryCount() const { return wrestlers.size() + orgs.size() + groups.size() + appearance.size(); }
    };

    // Reads a rename file. False and *error (one line, no file name; the caller adds "data/renames.json: ") on any problem.
    bool parseRenamePack(const std::string &jsonText, RenamePack &out, std::string *error);

    // One field set to a value (a masked field: one byte, only its mask's bits count).
    struct FieldWrite
    {
        std::string key;
        Bytes value;
    };

    struct ApplyOutcome
    {
        bool ok = false;
        std::string error;                     // one line, set when !ok
        std::vector<FieldWrite> writes;        // in the order fprsave.py makes them; each key once
        std::vector<std::string> suppressed;   // keys the pack wanted to write but `frozen` held back
    };

    // What the pack does to `image` (not changed here). Decisions that depend on the save (is an organisation in use? where do the
    // new ones go in the list?) are made against `image` with the pack's own earlier writes in place, exactly as fprsave.py does.
    // A key in `frozen` is not written (and the later decisions see the old bytes): the profile code freezes fields the player
    // changed in the game.
    bool applyRenamePack(const RenamePack &pack, const Bytes &image, const std::set<std::string> *frozen, ApplyOutcome &out);

    // Writes the field writes into `image`.
    void applyWrites(Bytes &image, const std::vector<FieldWrite> &writes);
}
