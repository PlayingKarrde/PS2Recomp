// Importing a community save as a data pack (task s4, part B; docs/formats/fprmod.md "Importing a save").
//
// The player drops a save file on the game window (or picks one with the Mods tab's "Import a save..." button). This turns it into an
// installed, switched-on data pack without Python:
//
//   1. the container is read (save_container.h: .max .psu .xps/.sps .cbs .psv, a .ps2 card image, a raw save, a folder);
//   2. the REGION GATE: the save's folder must be BASLUS-21702, BISLPM-66082 or BESLES-55041 (or the file has no folder name at all, a
//      raw save), it must be exactly 913,892 bytes, and its integrity data (888 block checksums, parity, final value) and the two
//      version markers must verify with the game's own checksum code. Anything else is refused with ONE line. The Japanese and PAL
//      bodies are UNVERIFIED (nobody has checked them against our layout); the integrity gate is the safety net;
//   3. the COMPARISON BASE: the player's own state without packs. That is the profile's save with every ledger field put back to its
//      "before" bytes when a profile has a save (profile::playerSave), otherwise the save on the base card. With neither, refuse;
//   4. the PACK: everything the renames layer understands (names, organisations, groups, their list orders, appearance records) that
//      differs from the base, as data/renames.json. It is the same document `fprsave.py renames --appearance` writes (a unit test
//      compares them byte for byte), packed with a mod.toml the way `fprsave.py mkmod` + `pack_mod.py` do. The package is installed
//      through the mod manager and turned on, so the Mods tab says "Restart the game to apply";
//   5. the REPORT: what was imported (counts) and what was not (bytes that differ outside the fields a pack carries: created wrestlers,
//      group membership, referees, rings...), as "This save also has other changes that packs cannot carry yet."
//
// No SDL and no game code. The host (ps2_host_mods.cpp) fills in `Env` and calls importFile; the unit tests do the same with a made-up
// checksum table and a temporary folder.
#pragma once

#include "fpr_save.h"
#include "save_container.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ps2x::mods
{
    class Manager;
}

namespace ps2x::mods::import
{
    using Bytes = std::vector<uint8_t>;

    // The three save folder names of the game's regions.
    constexpr const char *kFolderUs = "BASLUS-21702";
    constexpr const char *kFolderJp = "BISLPM-66082";
    constexpr const char *kFolderEu = "BESLES-55041";
    bool isGameFolder(const std::string &folder);
    // "USA", "Japan" or "Europe" for a game folder, "" otherwise.
    const char *regionOf(const std::string &folder);

    // ---- the pack ----------------------------------------------------------------------------------------------------------
    struct Counts
    {
        size_t wrestlers = 0, organisations = 0, groups = 0, appearance = 0;
        bool orderLists = false; // organisation_order / group_order are in the file
        size_t total() const { return wrestlers + organisations + groups + appearance; }
    };

    struct Extracted
    {
        std::string json;                  // data/renames.json
        Counts counts;
        std::vector<std::string> leftOut;  // changes a rename file cannot say (tools/fprsave.py prints these as "left out: ...")
        bool empty() const { return counts.total() == 0; }
    };

    // The rename set `save` carries relative to `base` (tools/fprsave.py extract_renames + dump_pack, appearance records included when
    // `withAppearance`): the very same bytes, with "source" naming `sourceFile` and `sourceBase`. Both images must be kSaveSize bytes.
    bool extractRenames(const Bytes &save, const Bytes &base, bool withAppearance, const std::string &sourceFile, const std::string &sourceBase,
                        Extracted &out, std::string *error);

    // Differences between `save` and what the pack in `renamesJson` makes of `base`, in the parts of the save that matter: the data a pack
    // cannot carry. The header, the saved screen state and options (bytes 0 to 0x111E0), the text after a name's terminator, deleted
    // records and the tail of a list order are not counted. `areas` names where the rest is ("12 edit slots", "3 wrestler groups").
    struct Unmatched
    {
        size_t units = 0;                  // differing items, all areas together
        std::vector<std::string> areas;    // "12 edit slots", ...
        bool any() const { return units != 0; }
    };
    bool unmatchedChanges(const Bytes &save, const Bytes &base, const std::string &renamesJson, Unmatched &out, std::string *error);

    // ---- the package -------------------------------------------------------------------------------------------------------
    struct PackInfo
    {
        std::string id;
        std::string name;
        std::string version = "1.0.0";
        std::string author = "Unknown author";
        std::string description;
    };
    // mod.toml, written the way tools/fprsave.py mkmod writes it.
    std::string manifestToml(const PackInfo &info);
    // "GameFAQs Save (16161).max" -> "gamefaqs-save-16161": the lower-case letters and digits of a file name's stem, runs of anything
    // else made one dash, at most 40 characters; "save" when nothing is left.
    std::string slugOf(const std::string &fileName);
    // The sentence a pack carries as its description.
    std::string descriptionFor(const std::string &fileName, const Counts &counts);

    // ---- the region gate ---------------------------------------------------------------------------------------------------
    struct GateResult
    {
        bool ok = false;
        std::string line; // when !ok: one line for the player
    };
    // Size, integrity and version markers of one save from a container.
    GateResult checkSave(const SaveEntry &entry, const fpr::CrcTable &table);

    // ---- the whole import --------------------------------------------------------------------------------------------------
    struct Env
    {
        Manager *manager = nullptr;
        std::filesystem::path profileDir;   // <pref folder>/profile ("" when there is no pref folder)
        std::filesystem::path baseCard0;    // the player's own card, slot 1 (its mc0 folder)
        std::filesystem::path scratchDir;   // where the package is written before it is installed (made and cleaned up here)
        const fpr::CrcTable *crcTable = nullptr;
        // Used when crcTable is null: reads the checksum table (from the player's own executable).
        std::function<bool(fpr::CrcTable &, std::string *)> loadCrcTable;
    };

    struct Choice
    {
        std::string folder;  // "BASLUS-21702"
        std::string region;  // "USA"
    };

    struct Outcome
    {
        bool ok = false;               // a pack was installed
        std::string message;           // what to show the player (one to three sentences, at most two lines of the Mods tab); a refusal is one line
        std::vector<std::string> notes; // when a pack was installed: lines for its page in the Mods tab (where it came from, what was left out)
        std::string modKey;            // the installed mod's id
        bool turnedOn = false;
        bool nothingToImport = false;  // the save carries no change a pack can hold (not an error)
        bool needsChoice = false;      // several saves of the game are in the file: call again with `choices[i].folder`
        std::vector<Choice> choices;
        Counts counts;
        bool otherChanges = false;
        Unmatched unmatched;
        std::string region;            // "USA", "Japan" or "Europe"
        std::vector<std::string> log;  // lines for the log
        double milliseconds = 0;       // importFile: how long it took, reading included
    };

    // Imports the save(s) in `entries` (what readSaveContainer found in `sourceName`). `chosenFolder` picks one when the file holds
    // several saves of the game (leave it empty the first time).
    Outcome importEntries(const Env &env, const std::vector<SaveEntry> &entries, const std::string &sourceName, const std::string &chosenFolder);
    // Reads `file` (save_container.h) and imports it.
    Outcome importFile(const Env &env, const std::filesystem::path &file, const std::string &chosenFolder = std::string());
}
