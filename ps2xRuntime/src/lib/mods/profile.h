// Profile cards: how data packs reach the game (docs/mod-layer.md "Data packs: save profiles"; docs/formats/fprmod.md "Data packs").
//
// A profile card is a copy of the player's own memory card with the active data packs applied. The game plays from the copy, and
// the player's own card (the "base card") is never written while a data pack is on. The profile lives in the pref folder:
//
//     <pref folder>/profile/mc0/BASLUS-21702/...   card slot 1 (the card root the game is pointed at)
//     <pref folder>/profile/mc1/...                card slot 2
//     <pref folder>/profile/ledger.json            which pack wrote which field, and the bytes before and after
//     <pref folder>/profile/base-backup-<date-time>/   the base card's files, saved before "Keep this card" overwrites them
//
// prepare() runs once at the start of a session that has at least one active data pack, before the game's first card call:
//   1. a profile slot with no save gets the base card's files for that slot;
//   2. with no save at all there is nothing to apply to (the player saves once in the game, then restarts);
//   3. every data pack's fields are applied in load order, using the ledger: a field still holding what the ledger says a pack wrote
//      belongs to the packs (the last pack that provides it wins, and if none does any more it gets its "before" bytes back); a
//      field that holds something else was changed by the player in the game and is kept, and noted; a field no pack has written
//      before is written and its current bytes become its "before";
//   4. the integrity data is recomputed.
// The profile save and the ledger are written so that a crash at any point leaves either the old pair or the new pair (the ledger
// is first written as ledger.json.new together with the SHA-256 of the save it belongs to; whichever file the next session finds,
// it finishes or abandons that update).
//
// Which card the game plays from (task s3; docs/mod-layer.md "Leaving profiles"), decided by select() at the start of a session:
//   - at least one data pack active: the profile card;
//   - no data pack, but the profile has a save and the player has not chosen "Back to my card": the profile card still, with every
//     field the packs wrote put back to its "before" bytes (what prepare() does with an empty pack list; the player's own changes stay);
//   - no data pack and the player chose "Back to my card", or there is no profile save at all: the base card, and nothing under the
//     profile folder is touched.
// The choice is one small file of its own, <profile>/card.json ({"format": "fpr-card", "version": 1, "use": "base"}), written through a
// temporary file and a rename. It is removed at the start of any session that has a data pack active, so the profile is used again and
// "stay on the profile" is the rule again after that pack goes off.
//
// No SDL, no game code and no Manager here: the host (ps2_host_mods.cpp) reads the packs out of the manager and hands them in, so the
// unit tests drive this with a temporary folder and a made-up checksum table.
#pragma once

#include "fpr_save.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ps2x::mods::profile
{
    // One active data pack, as the host read it.
    struct PackInput
    {
        std::string id;
        std::string name;
        std::string version;
        bool hasFile = false;      // the package holds data/renames.json
        std::string renamesJson;   // its text
        std::string readError;     // set when the file is there but could not be read
    };

    struct Config
    {
        std::filesystem::path profileDir;  // <pref folder>/profile
        std::filesystem::path baseCard0;   // the base card's slot 1 root (its mc0 folder)
        std::filesystem::path baseCard1;   // slot 2 (mc1)
        std::vector<PackInput> packs;      // the session's active data packs, in load order (the last one wins)
        const fpr::CrcTable *crcTable = nullptr; // null: the checksum table could not be read, so no save can be checked or fixed
        std::string crcTableError;         // why not
        // select() only: reads the checksum table when (and only when) the profile is going to be used, so a start that uses the base
        // card does not read the executable. Used when crcTable is null.
        std::function<bool(fpr::CrcTable &, std::string *)> loadCrcTable;
        // Tests only: stop at a point as if the process had died there. 0 none; 1 after ledger.json.new is written; 2 after the save
        // is replaced but before ledger.json is.
        // 3 (select() only): after the card choice was cleared, before the profile is prepared.
        int failpoint = 0;
    };

    struct PackReport
    {
        std::string id;
        std::string error;                 // the pack could not be used; one line
        size_t written = 0;                // fields the pack provides now
        size_t kept = 0;                   // fields it wanted to write but the player had changed
        size_t overridden = 0;             // fields a later pack provides instead
        std::vector<std::string> notes;    // lines for the pack's page in the Mods tab
    };

    struct Result
    {
        std::filesystem::path root0;       // the card root to give the game: <profile>/mc0
        bool saveFound = false;            // the profile has a save the packs could apply to
        bool applied = false;              // the packs were applied
        bool imageChanged = false;         // the profile's save was rewritten
        std::string line;                  // "Memory card: ..." for the Mods tab
        std::string problem;               // why nothing was applied, when that is so
        std::vector<PackReport> packs;     // one per input pack, same order
        std::vector<std::string> log;      // lines for the log
        double milliseconds = 0;
    };

    // Builds or updates the profile and returns what the host needs. Never throws; a problem becomes Result::problem and the packs'
    // notes, and the profile card is still the one to use.
    Result prepare(const Config &config);

    // The Mods tab line when the base card is in use because there is no profile save (no data pack is on).
    std::string baseCardLine();
    // The line when the player chose "Back to my card" and no data pack is on.
    std::string ownCardChoiceLine();

    // ---- which card (task s3) ----
    enum class CardWhy
    {
        NoProfile,       // no data pack and no save on the profile: the base card, nothing made
        DataPacksOn,     // at least one data pack is active: the profile
        StayedOnProfile, // no data pack, the profile has a save and the player did not leave it: the profile
        ChoseOwnCard,    // no data pack, the profile has a save, and the player chose "Back to my card": the base card
    };
    struct CardPlan
    {
        bool useProfile = false;
        CardWhy why = CardWhy::NoProfile;
        bool recordUnreadable = false; // card.json exists but could not be understood (treated as no choice)
    };
    // True when the profile has a save in either card slot (a save the player would not see on the base card).
    bool profileHasAnySave(const std::filesystem::path &profileDir);
    // The recorded choice: true when <profile>/card.json says "Back to my card" (an absent or unreadable record is no choice; *unreadable
    // is set for the second).
    bool choseOwnCard(const std::filesystem::path &profileDir, bool *unreadable = nullptr);
    // The rule above, reading the disk and changing nothing.
    CardPlan planCard(const std::filesystem::path &profileDir, size_t activePacks);
    // Records "Back to my card" (needs a save on the profile: false with *message saying so otherwise). One crash-safe write.
    bool recordOwnCard(const std::filesystem::path &profileDir, std::string *message);
    // Forgets the choice (the file is removed). True when there is no record afterwards.
    bool clearOwnCardChoice(const std::filesystem::path &profileDir);

    struct Selection
    {
        bool useProfile = false;           // point the card root at result.root0
        CardWhy why = CardWhy::NoProfile;
        std::string line;                  // "Memory card: ..." for the Mods tab
        Result result;                     // meaningful when useProfile
        std::vector<std::string> log;      // lines for the log (this and result.log)
    };
    // The session's start: decides the card (planCard), clears the choice when a data pack is on, and builds or updates the profile
    // when it is the one to use. Never throws. With a base card nothing under the profile folder is made, read or changed.
    Selection select(const Config &config);

    struct KeepResult
    {
        bool ok = false;
        std::string message;               // one sentence for the player
        std::filesystem::path backup;      // where the base card's files went (empty if there were none)
    };
    // True when the profile has a save ("Keep this card" has something to copy).
    bool profileHasSave(const std::filesystem::path &profileDir);
    // Copies the profile's slot 1 save to the base card, after saving the base card's own files to
    // <profileDir>/base-backup-<date-time>/. Each file is written through a temporary file and a rename.
    KeepResult keepCard(const std::filesystem::path &profileDir, const std::filesystem::path &baseCard0);

    // ---- helpers the tests and the host share ----
    // Writes `bytes` to `target` through target + ".tmp" and a rename, flushed to disk. False if it could not (the old file is kept).
    bool writeFileAtomic(const std::filesystem::path &target, const std::string &bytes);
    // Slot 2's root for a slot 1 root, the way the card code derives it (mc0 -> mc1 next to it).
    std::filesystem::path slot2Root(const std::filesystem::path &slot1Root);
}
