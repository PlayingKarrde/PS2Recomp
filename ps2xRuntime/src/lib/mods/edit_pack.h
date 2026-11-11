// Created wrestlers in data packs: which edit slot each edit file gets, and how the host hands a pack's edit files to the profile builder.
//
// A data pack can carry edit files, data/edits/<name>.edit.toml (docs/formats/edit-text.md). When the profile card is built (profile.h),
// each edit file becomes one created wrestler in one of the save's 500 edit slots. The ledger keeps one field per slot, "e<slot>" (the
// whole 0x44C bytes), with the pack's id and the edit file's name, so an edit keeps its slot from one session to the next.
//
// planSlots decides the slots. Its rules (docs/formats/fprmod.md "Created wrestlers"):
//   - An edit that has a slot in the ledger (same pack id, same file name) keeps it, as long as the slot is free once the packs' own
//     wrestlers are taken out of the card, or the player changed that wrestler in the game (then it is kept as the player's: the slot is
//     "held" and the edit is not written again).
//   - A new edit takes the lowest slot that is free: never one the player uses, never one held for a kept edit, never one already given.
//   - With no free slot left the edit is skipped; the others are not affected.
// Pure functions and no game data: the unit tests drive them directly.
#pragma once

#include "profile.h"

#include <string>
#include <vector>

namespace ps2x::mods
{
    class Manager;
}

namespace ps2x::mods::edit
{
    // A slot the ledger gave to an edit earlier.
    struct SlotClaim
    {
        int slot = 0;
        std::string pack;   // the pack's id
        std::string file;   // the edit file's name
        bool held = false;  // the wrestler in the slot is no longer what the pack wrote (the player changed or deleted it in the game)
    };
    struct Wanted
    {
        std::string pack;
        std::string file;
    };
    struct Placement
    {
        bool placed = false;  // the edit has a slot
        int slot = -1;
        bool kept = false;    // the slot is held: the player's version stays, nothing is written
    };

    // `used[k]` says slot k holds a wrestler in the card once the packs' own wrestlers are taken out (the player's own ones). `claims` are the
    // ledger's slots, `wanted` the edit files of the active packs in load order. One Placement per wanted edit, in the same order.
    std::vector<Placement> planSlots(const std::vector<bool> &used, const std::vector<SlotClaim> &claims, const std::vector<Wanted> &wanted);

    // True for "<anything>.edit.toml" (capitals ignored).
    bool isEditFileName(const std::string &name);

    // Fills pack.edits from the installed package of the mod `id` (on or off): its data/edits/*.edit.toml files, in name order. Nothing is
    // changed when the mod has none. At most kEditSlotCount files are read (a pack cannot hold more wrestlers than the save has slots) and
    // each at most kMaxEditBytes; a file that cannot be read is listed with its readError.
    constexpr uint64_t kMaxEditBytes = 256 * 1024;
    void readEditFiles(const Manager &manager, const std::string &id, profile::PackInput &pack);
}
