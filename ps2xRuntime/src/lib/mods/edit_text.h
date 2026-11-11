// A created wrestler ("edit") as a text file, and back: the C++ twin of the text half of tools/fpredit.py.
//
// The main form of an edit is a TOML file, <name>.edit.toml, with format = "fpr-edit" and version = 1 (docs/mod-layer.md "Edit format: text
// first"; the keys, ranges and rules are in docs/formats/edit-text.md). The game's save holds an edit in one of 500 slots of 0x44C bytes
// (docs/formats/edit-save.md "Edit slots"): a used flag, the 0x260-byte wrestler record that built-in wrestlers use too, a point budget, a
// finisher name and an appearance record. textToSlot reads a file and gives those 1,100 bytes; slotToText writes the file for a slot.
//
// The two directions are exact inverses on everything the text can say: slotToText only returns text that textToSlot turns back into
// the very same bytes (it checks), and a unit test compares this code with fpredit.py byte for byte on real and made-up slots. The keys,
// ranges and comments come from edit_schema.inc, which fpredit.py generates from its own schema.
//
// No game data here: move names, which the text may carry as a readable check, are looked up through a function the caller gives (the
// host may read them from the player's own files); with none given the names are left out and not checked.
//
// No SDL, no game code. Pure functions; toml11 stays inside the .cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ps2x::mods::edit
{
    using Bytes = std::vector<uint8_t>;

    constexpr const char *kFormat = "fpr-edit";
    constexpr int kVersion = 1;

    constexpr int kSlotCount = 500;
    constexpr size_t kSlotSize = 0x44C;     // the slot in the save
    constexpr size_t kRecordSize = 0x260;   // the wrestler record inside it, at slot offset 2
    constexpr size_t kRecordAt = 2;
    constexpr size_t kGroupAt = kRecordAt + 0x4A; // the group word, 0xFFFF = none
    constexpr size_t kBudgetAt = 0x262, kFinisherAt = 0x264, kAppearanceAt = 0x28C;
    constexpr size_t kFinisherSize = 40, kAppearanceSize = 448;
    constexpr uint16_t kNoGroup = 0xFFFF;

    // The game's move name for a move id (1 to 2000), or false when unknown. Optional.
    using MoveNames = std::function<bool(int id, std::string &name)>;

    // Text -> the 1,100 bytes of a used slot. False with *error (one line, no file name) when the text is not a valid edit. Move names that
    // do not match `names` are not errors: they come back in *warnings (one line each).
    bool textToSlot(const std::string &text, const MoveNames *names, Bytes &slot, std::vector<std::string> *warnings, std::string *error);
    // Text without [appearance] and the slot extras -> the 608 bytes of a wrestler record (the built-in wrestlers' records).
    bool textToRecord(const std::string &text, const MoveNames *names, Bytes &record, std::vector<std::string> *warnings, std::string *error);

    // A used slot (1,100 bytes) -> text. False with *error when the slot holds something the text cannot say and read back identically
    // (a value the game's editor would not allow); the importer reports that wrestler as one it could not carry.
    bool slotToText(const Bytes &slot, const MoveNames *names, std::string &text, std::string *error);
    // A 608-byte record -> text (no slot extras).
    bool recordToText(const Bytes &record, const MoveNames *names, std::string &text, std::string *error);

    // The group word of a slot, and setting it.
    uint16_t slotGroup(const Bytes &slot);
    void setSlotGroup(Bytes &slot, uint16_t group);

    // The name a pack's edit file for the wrestler in slot `slotIndex` gets: "000-karl-gotch.edit.toml" (the number is the slot the
    // wrestler came from; the words are the name the game shows, lower-case ASCII letters and digits, at most 24 characters).
    std::string editFileName(int slotIndex, const Bytes &slot);
    // The text name of the wrestler as the game shows it (FIRST LAST, or LAST FIRST with Flip), for notes: "Karl Gotch". Empty when the
    // name is not plain text.
    std::string displayName(const Bytes &slot);

    // ---- what the schema holds, for tests ----
    struct ParameterInfo
    {
        std::string key;
        bool voice = false;
        int lo = 0, hi = 0, delta = 0;
        long long unknownStored = -1; // the stored value the word stands for, or -1
        std::string word;             // "unknown", "none" or ""
        std::vector<std::string> names;
        int offset = 0, size = 0;     // in the record
    };
    std::vector<ParameterInfo> parameters();
}
