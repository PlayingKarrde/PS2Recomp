// The Fire Pro Wrestling Returns (SLUS-21702) save file, as far as data packs need it: the layout of the fields a rename pack
// writes, the game's integrity data (block checksums, parity block, final value), and the text encoding of names.
//
// This is the C++ twin of tools/fprsave.py and tools/save_checksums.py. Where they and this file disagree, the game's own code is
// the authority; the layout and the evidence for it are in docs/formats/edit-save.md. The unit tests check that the bytes written
// here equal the bytes fprsave.py writes for the same inputs.
//
// No SDL, no game code, no game data: the CRC table is read at run time from the player's own copy of the game executable (and
// checked against a SHA-256 fingerprint, as save_checksums.py does), never stored in the source.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps2x::mods::fpr
{
    constexpr const char *kGameName = "SLUS-21702";
    constexpr const char *kSaveName = "BASLUS-21702"; // the card folder and the main save file in it

    // ---- the file --------------------------------------------------------------------------------------------------------
    constexpr size_t kSaveSize = 0xDF1E4;
    constexpr size_t kBlockSize = 0x400;
    constexpr size_t kBlocks = 888;
    constexpr size_t kDataEnd = kBlockSize * kBlocks; // 0xDE000: everything before it is data
    constexpr size_t kTableOff = 0xDE000;             // 888 x u32: the checksum of each block
    constexpr size_t kParityOff = 0xDEDE0;            // 1024 bytes: the XOR of all the blocks
    constexpr size_t kFinalOff = 0xDF1E0;             // u32: the checksum of the parity block
    constexpr uint32_t kSettingsMarkerOff = 0x1C, kSettingsMarker = 0x111C4;
    constexpr uint32_t kRosterMarkerOff = 0x111E0, kRosterMarker = 0x2E1B8;

    // ---- the roster blob (file offsets) ----------------------------------------------------------------------------------
    constexpr uint32_t kRoster = 0x111E0;
    constexpr int kWrestlers = 327;
    constexpr int kOrgs = 50;
    constexpr uint32_t kOrgStride = 44;
    constexpr uint32_t kOffOrgs = kRoster + 0x067A;
    constexpr uint32_t kOffOrgOrder = kRoster + 0x0F12;
    constexpr int kGroups = 100;
    constexpr uint32_t kGroupStride = 46;
    constexpr uint32_t kOffGroups = kRoster + 0x0F44;
    constexpr uint32_t kOffGroupOrder = kRoster + 0x213C;
    constexpr uint32_t kOffNames = kRoster + 0x49D6;
    constexpr uint32_t kNameStride = 57;
    constexpr uint32_t kOffAppearance = kRoster + 0x92A6;
    constexpr uint32_t kAppearanceStride = 448;

    // Text field widths in bytes, and the most characters each takes (the game's own limits for wrestlers; one short of the field
    // for organisations and groups, which no save has ever filled).
    constexpr uint32_t kWidthFirst = 14, kWidthLast = 18, kWidthNickname = 24, kWidthOrgLong = 26, kWidthOrgShort = 14;
    constexpr int kCharsFirst = 7, kCharsLast = 9, kCharsNickname = 12, kCharsName = 12, kCharsShort = 6;

    // ---- integrity -------------------------------------------------------------------------------------------------------
    using CrcTable = std::array<uint32_t, 256>;

    // The game's checksum (function 0x1C09A0): crc = (crc << 8) ^ T[(crc >> 24) ^ byte], start 0, no final XOR.
    uint32_t crc(const uint8_t *data, size_t size, const CrcTable &table);

    // True when the table's SHA-256 is the one of the NTSC-U v1.01 executable (the only version the layout is verified for).
    bool crcTableFingerprintOk(const CrcTable &table);
    // Reads the 256-entry table from the game executable (an ELF32; virtual address 0x32E8D0) and checks its fingerprint.
    bool crcTableFromElf(const std::filesystem::path &elf, CrcTable &out, std::string *error);

    struct Integrity
    {
        bool sizeOk = false;
        size_t blocksOk = 0;
        std::vector<size_t> badBlocks;
        bool parityOk = false;
        bool finalOk = false;
        bool markersOk = false; // the two layout markers (settings 0x111C4 at 0x1C, roster 0x2E1B8 at 0x111E0)
        bool allOk() const { return sizeOk && badBlocks.empty() && parityOk && finalOk; }
    };
    Integrity checkIntegrity(const std::vector<uint8_t> &image, const CrcTable &table);
    // Rewrites the block checksums, the parity block and the final value from the data. `image` must be kSaveSize bytes.
    void fixIntegrity(std::vector<uint8_t> &image, const CrcTable &table);

    // ---- SHA-256 (the CRC table's fingerprint, and the profile's save fingerprint) -----------------------------------------
    std::string sha256Hex(const uint8_t *data, size_t size);
    inline std::string sha256Hex(const std::vector<uint8_t> &bytes) { return sha256Hex(bytes.data(), bytes.size()); }

    // SHA-1 (only the "base_sha1" line of a rename file, which names the save a pack was made against; not a security check).
    std::string sha1Hex(const uint8_t *data, size_t size);
    inline std::string sha1Hex(const std::vector<uint8_t> &bytes) { return sha1Hex(bytes.data(), bytes.size()); }

    // ---- text ------------------------------------------------------------------------------------------------------------
    // The game's name bytes to code points, strictly: false when a byte sequence is not one code page 932 character (the same
    // characters Python's "cp932" codec decodes, for the ones that encode back to the same bytes; see fieldTextOrHex).
    bool decodeGameBytes(const uint8_t *bytes, size_t size, std::vector<uint32_t> &out);
    // UTF-8 for code points.
    std::string encodeUtf8(const std::vector<uint32_t> &codePoints);
    // What a rename file holds for a text field (tools/fprsave.py field_value): the bytes up to the first zero, as plain text when
    // that stores back to exactly the same bytes (full-width ASCII shown as ASCII, a full-width space as a space), otherwise the same
    // text without the ASCII folding, otherwise nothing (*isHex is set and the return value is the bytes as lower-case hexadecimal).
    std::string fieldTextOrHex(const uint8_t *field, size_t width, bool *isHex);

    // UTF-8 to code points. False (with *error) on a byte sequence that is not valid UTF-8.
    bool decodeUtf8(const std::string &utf8, std::vector<uint32_t> &out, std::string *error = nullptr);
    // What the game's name fields hold: printable ASCII becomes the full-width form (a space becomes U+3000), then the text is
    // encoded as Windows-31J ("Shift-JIS", code page 932). False when a character has no code in that page.
    bool encodeGameText(const std::vector<uint32_t> &codePoints, std::vector<uint8_t> &out, std::string *error = nullptr);

    // ---- fields ----------------------------------------------------------------------------------------------------------
    // A field is a named range of the save that a pack can own. Keys (stable; the profile's ledger stores them):
    //   w<slot>.first | .last | .nick    a wrestler's name parts (FIRST, LAST, NICKNAME)
    //   w<slot>.flip | .space | .flags   bit 7, bits 0-1 and bits 2-6 of the name record's flags byte
    //   o<i>.live | .name | .short | .id    an organisation's record (in use, long name, short name, id word)
    //   g<i>.live | .name | .short | .org | .kind    a group's record
    //   o.order | g.order                the list order arrays
    //   a<slot>                          a wrestler's appearance record
    struct FieldRange
    {
        uint32_t offset = 0;
        uint32_t length = 0;
        uint8_t mask = 0xFF; // length 1 only: the bits of that byte the field owns
    };
    bool fieldRange(const std::string &key, FieldRange &out);
    // The field as the player would say it: "wrestler 12's FIRST name", "the group list order".
    std::string describeField(const std::string &key);
    // The bytes of the field (a masked field: the one byte, AND the mask).
    std::vector<uint8_t> readField(const std::vector<uint8_t> &image, const FieldRange &range);
    // Writes the field (a masked field changes only its bits). `value` must have range.length bytes.
    void writeField(std::vector<uint8_t> &image, const FieldRange &range, const std::vector<uint8_t> &value);

    inline uint16_t readU16(const std::vector<uint8_t> &image, size_t offset)
    {
        return static_cast<uint16_t>(image[offset] | (image[offset + 1] << 8));
    }
    inline bool orgLive(const std::vector<uint8_t> &image, int index) { return readU16(image, kOffOrgs + static_cast<size_t>(index) * kOrgStride) != 0; }
    inline bool groupLive(const std::vector<uint8_t> &image, int index) { return readU16(image, kOffGroups + static_cast<size_t>(index) * kGroupStride) != 0; }
}
