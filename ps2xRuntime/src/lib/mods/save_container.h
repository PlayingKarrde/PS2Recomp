// Readers for the files that PS2 saves are shared in, so the save importer (task s4) can take a community save without Python:
//
//   raw save file    the save as the game writes it (913,892 bytes for Fire Pro Wrestling Returns)
//   folder           a card folder that someone already extracted (the save file, icon.sys, view.ico)
//   .max             Action Replay MAX ("Ps2PowerSave" + LZARI compression)
//   .psu             EMS / uLaunchELF (no magic number; recognised by its first three directory entries)
//   .xps / .sps      X-Port / SharkPort ("\x0d\0\0\0SharkPortSave")
//   .cbs             CodeBreaker ("CFU\0", RC4 + zlib)
//   .psv             PS3 export ("\0VSP", PS2 type only)
//   .ps2             a whole memory card image (PCSX2), 512-byte pages or 528-byte pages with ECC; read-only
//
// The format is found from the bytes, never from the extension. The one exception is the refusal of a PS3 ".vme" card, which is
// found by its layout (a 0x80-byte header in front of a card image) and, failing that, by its extension: the player gets the same
// "convert it first" line either way. The readers follow mymcplus (the formats' reference tool) and the tests compare against it.
//
// This file knows nothing about the game's save layout, region or integrity data: it only turns a container into the files of its
// save folders. Deciding which folder is Fire Pro Returns, and whether its bytes are a save, is the caller's job (see fpr_save.h).
//
// Dependencies: the C++ standard library, and miniz (for .cbs) inside the .cpp only. No SDL, no game code, no game data.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps2x::mods
{
    // One file of a save folder.
    struct SaveFile
    {
        std::string name;
        std::vector<uint8_t> data;
    };

    // One save folder found in a container.
    struct SaveEntry
    {
        // The folder's name on the card, e.g. "BASLUS-21702", as the bytes stored there (a few Japanese games use Shift-JIS; only this
        // game's three ASCII names matter to the importer). For a bare raw save file it is the file's own name when that looks like
        // a save folder name (B + A/E/I/K + 10 more characters), otherwise the name of the directory the file is in when that does,
        // otherwise "".
        std::string folder;

        // The folder's main save file: the file named like the folder; failing that, the largest file that is not icon.sys and not
        // an .ico. Empty when the folder holds no file at all. The caller checks its size (913,892 bytes for this game).
        std::vector<uint8_t> save;

        // The name of that file inside the folder. For a bare raw save file it is the file's own name ("" from the in-memory reader
        // when no name was given).
        std::string saveName;

        // Every other regular file of the folder (icon.sys, view.ico, ...), in the container's order.
        std::vector<SaveFile> others;

        // Which reader produced it: "raw", "folder", "max", "psu", "sps", "cbs", "psv" or "ps2".
        std::string format;

        // Empty normally. Set (to a one-line message ending in a full stop) when this folder of a card image could not be read
        // completely, for example because its cluster chain is broken: `save` and `others` are then empty. The other folders of the
        // card are still returned. No other format sets it (a damaged file of those fails as a whole).
        std::string problem;
    };

    // Reads a save container (see the list at the top). `out` is cleared first and then holds one entry per save folder:
    //   - raw save file: one entry (format "raw");
    //   - a directory with no sub-folders: one entry for it (format "folder"), made of its regular files;
    //   - a directory with sub-folders (a PCSX2 folder memory card, or a pile of extracted saves): one entry for each sub-folder
    //     (format "folder"), made of that sub-folder's regular files, in name order; files beside the sub-folders are ignored;
    //   - .max .psu .sps/.xps .cbs .psv: one entry;
    //   - .ps2 card image: one entry for every folder in the card's root directory, whatever game it belongs to, in directory
    //     order (the caller picks the folder of this game and, if several are, asks the player).
    // Returns false with a one-line message in `error` (no line break, no path) when the file is unreadable, not a container, damaged,
    // or a kind that is refused on purpose (.vme). It never throws and never writes anything. A card with no save folders succeeds
    // with an empty `out`.
    bool readSaveContainer(const std::filesystem::path &path, std::vector<SaveEntry> &out, std::string &error);

    // The same for a file that is already in memory. `nameHint` is the file's name (used only for a raw save's folder name and for
    // the ".vme" fallback); it may be empty. The unit tests use this one.
    bool readSaveContainerBytes(const std::vector<uint8_t> &bytes, const std::string &nameHint, std::vector<SaveEntry> &out, std::string &error);

    // Names the container's kind from its bytes without reading it ("raw", "max", "psu", "sps", "cbs", "psv", "ps2", "vme"), or ""
    // when nothing matches. Handy for a message such as "this is a .max file".
    std::string saveContainerKind(const std::vector<uint8_t> &bytes, const std::string &nameHint);
} // namespace ps2x::mods
