#pragma once
// Development aid (task a3): serve a file of the game's archive FPR.ROM from a folder on the host.
//
// What it is for. Move data, texts and tables sit in files inside FPR.ROM. To try a one-byte change in one of them
// without writing a 800 MB archive, set PS2X_ROM_OVERRIDE_DIR=<folder>. A file inside the archive whose path exists under
// that folder (for example <folder>/yoneda/fprwaza.bin for the archive path yoneda/fprwaza.bin) is then served from the host
// file. Off by default: with the variable unset nothing in the disc path changes.
//
// How. The game finds a file through the archive's own directory (the scrambled table of contents in the first 22
// sectors, docs/formats/fpr-rom.md): each entry holds the file's start sector and its size in bytes. The game does not
// ask the host for either; it reads the directory with the same sceCdRead as all data. So this module
//   1. decodes the directory itself the first time the archive is read (the same key recovery as tools/inventory.py),
//   2. gives every replaced file a "virtual" place beyond the end of the archive (sector kVirtualSectorBase + k) and
//      rewrites that file's directory entry (start sector and size) in the bytes it hands to the game, scrambled with the
//      archive's own key, so the game believes the file lives there and has the new size,
//   3. serves reads of those virtual sectors from the host file (the last sector is padded with zeros).
// A replacement may therefore be larger or smaller than the original. What it cannot change is the game's own buffer for
// the file: the move table is loaded into a heap block of 0x180000 bytes with no size check (docs/formats/fprwaza.md), so a
// longer file would overrun it. For the files whose buffer we know, a larger replacement is refused with a log line.
//
// This is not the patch-mod format (docs/mod-layer.md "Patch mods"); it only replaces whole files.
//
// The functions below the line "Pure logic" have no global state and are what the unit tests exercise.
//
// Also in this pair of files: a small in-process logger that reads guest memory once per game frame and writes a CSV
// (PS2X_PROBE_FILE ...). tools/match_probe.py drives it. See the end of this header.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ps2x_rom_override
{
    constexpr uint32_t kSectorSize = 2048;
    // A replaced file lives at this sector (plus an offset) counted from the start of FPR.ROM. No real archive is that large
    // (0x1000000 sectors = 32 GB), and the CD layer's other pseudo files start right after the archive, far below.
    constexpr uint32_t kVirtualSectorBase = 0x01000000u;
    // The directory is scrambled from this byte on; the key repeats every 88 bytes (two interleaved cycles of 11).
    constexpr uint32_t kDirectoryStart = 0x80;
    constexpr uint32_t kKeyPeriod = 88;

    // ---- Pure logic ------------------------------------------------------------------------------------------------------

    struct TocFile
    {
        std::string path;         // lower-case, '/' between folders, e.g. "yoneda/fprwaza.bin"
        uint32_t sector = 0;      // first sector in the archive
        uint32_t size = 0;        // bytes
        uint32_t entryOffset = 0; // where this file's 12-byte entry (name word, sector, size) starts in the archive header
    };

    struct Toc
    {
        uint32_t headerSectors = 0;
        std::array<uint8_t, kKeyPeriod> key{}; // key byte for header offset 0x80 + i (i < 88)
        std::vector<TocFile> files;
        std::vector<std::string> notes; // oddities met while reading (not errors)
    };

    // `header` holds the first bytes of the archive as on disc. Needs the whole directory, i.e. u32@8 sectors.
    bool decodeToc(const uint8_t *header, size_t bytes, Toc &toc, std::string &error);

    // XOR key byte for the archive offset `pos` (>= kDirectoryStart).
    inline uint8_t keyByteAt(const Toc &toc, uint32_t pos)
    {
        return toc.key[(pos - kDirectoryStart) % kKeyPeriod];
    }

    // The largest size the game can take for a file, where the game's code tells us (docs/formats/fprwaza.md, "Where the
    // numbers live"). 0 = unknown.
    uint64_t knownLoadLimit(const std::string &lowerPath);

    struct Replacement
    {
        std::string path;
        std::filesystem::path host;
        uint32_t originalSector = 0;
        uint32_t originalSize = 0;
        uint32_t entryOffset = 0;
        uint32_t newSize = 0;
        uint32_t virtualSector = 0; // counted from the archive start (>= kVirtualSectorBase)
        uint32_t sectors = 0;       // sectors of the replacement
    };

    struct Plan
    {
        std::vector<Replacement> replacements;
        std::vector<std::string> log; // one line per decision, including every refusal
    };

    // Lower-case relative path (with '/') -> host file, for every regular file under `dir`.
    std::map<std::string, std::filesystem::path> scanFolder(const std::filesystem::path &dir);

    // Decide which files of the archive are replaced by which host files. Files that do not exist in the archive, are empty,
    // do not fit the 32-bit size, or exceed a known load limit are refused and named in the log.
    Plan makePlan(const Toc &toc, const std::map<std::string, std::filesystem::path> &hostFiles);

    // Rewrite the directory fields of the replaced files inside bytes the game is about to see. `archiveOffset` is where
    // dst[0] sits in the archive. Only whole fields inside [archiveOffset, archiveOffset + bytes) are changed.
    // Returns the number of fields rewritten.
    size_t patchDirectoryBytes(const Toc &toc, const Plan &plan, uint64_t archiveOffset, uint8_t *dst, size_t bytes);

    // If `sector` (counted from the archive start) lies in a replacement's virtual range, fill dst with the bytes from there
    // (zero past the end of the host file) and return true. `bytes` may be shorter than `sectors` * kSectorSize.
    bool readVirtualSectors(const Plan &plan, uint64_t sector, uint32_t sectors, uint8_t *dst, size_t bytes);

    // ---- The hook used by Support.h (readCdSectors) ----------------------------------------------------------------------

    // Is PS2X_ROM_OVERRIDE_DIR set? Read once. Cheap: call it on every disc read.
    bool enabled();

    // After the bytes of a registered file were read from the host: if it is FPR.ROM, patch the directory fields in them.
    // `baseLbn` is the file's first sector in the CD layer's numbering. `relativeSector` is the first sector read.
    void afterHostRead(const std::filesystem::path &hostPath, uint32_t baseLbn, uint64_t relativeSector,
                       uint8_t *dst, size_t bytes);

    // A read that no registered file contains: serve it when it is a replacement's virtual sector. Returns true if served.
    bool serveVirtual(uint32_t lbn, uint32_t sectors, uint8_t *dst, size_t bytes);

    // Tests: drop all state and use `dir` as if PS2X_ROM_OVERRIDE_DIR named it (an empty path turns the aid off).
    void resetForTests(const std::filesystem::path &dir);
    // Tests: the plan built from the archive that was read last (empty before any read).
    const Plan &currentPlanForTests();
}

// ---- The match probe (task a3) ---------------------------------------------------------------------------------------------
//
// A logger that runs on the game's own thread once per game frame (a wrapper on the frame-end function 0x136DB0, the one
// `i1` counts frames with), reads guest memory and appends rows to a CSV. It exists so that a script can watch per-wrestler
// fields change during a match without any memory-reading tool outside the process. Off unless PS2X_PROBE_FILE is set.
//
//   PS2X_PROBE_FILE=<csv>      where to write
//   PS2X_PROBE_FIELDS=<list>   comma-separated field specs (below); default "rec=+0x1AC:4,unit=+0x1B0:4"
//   PS2X_PROBE_BASE=<addr>     guest address of the pointer to the array of per-wrestler structs (default 0x37E658)
//   PS2X_PROBE_STRIDE=<bytes>  size of one struct (default 0x2A4)
//   PS2X_PROBE_COUNT=<n>       how many structs (default 8)
//   PS2X_PROBE_DUMP=<file>,<addr>,<len>[,ptr]
//                              once, PS2X_PROBE_DUMP_DELAY frames (default 600) after the wrestler array first exists
//                              (the game reads its move table after allocating it), write <len> bytes at <addr>
//                              (or, with "ptr", at the address stored in the word at <addr>) to <file>
//
// A field spec is  [name=]<where>[><deref>]:<width>  with <where> = +<offset in the wrestler's struct> or @<absolute guest
// address>, <deref> = an offset added to the 32-bit pointer read at <where> before the value is read, <width> = 1, 2 or 4.
// Numbers may be decimal or 0x hex. Example: "unit=+0x1B0:4,dur=+0x1B0>6:1" is the unit pointer and the byte 6 of the unit it
// points at (a unit's duration).
//
// The CSV is a change log: one row per wrestler in each frame in which any of its fields changed, and only the cells that
// changed are filled (an empty cell means "same as this wrestler's previous row"); a wrestler's first row has every cell.
//   frame,tick,w,<name>,<name>...   (values as 0x hex; widths over 4 and width 3 as plain hex bytes; "-" = not readable;
//                                    w = -1 is the row of the absolute @ fields)
// `frame` counts calls of the frame-end function since the start, `tick` is the runtime's VSync counter (host timing).
namespace ps2x_match_probe
{
    struct FieldSpec
    {
        std::string name;
        bool absolute = false;
        uint32_t where = 0;
        bool deref = false;
        uint32_t derefOffset = 0;
        uint32_t width = 4;
    };

    struct Config
    {
        std::string file;
        std::vector<FieldSpec> fields;
        uint32_t basePtrAddr = 0x0037E658u;
        uint32_t stride = 0x2A4u;
        uint32_t count = 8u;
        std::string dumpFile;
        uint32_t dumpAddr = 0;
        uint32_t dumpLen = 0;
        bool dumpIsPointer = false;
    };

    bool parseFieldSpecs(const std::string &text, std::vector<FieldSpec> &out, std::string &error);
    bool parseDumpSpec(const std::string &text, Config &cfg, std::string &error);

    // Turns the guest's memory into CSV rows. The game thread feeds it once per frame; the unit tests feed it a made-up RAM.
    class Sampler
    {
    public:
        using Sink = void (*)(void *user, const std::string &text);

        Sampler(const Config &cfg, Sink sink, void *user);
        // The CSV header line (with its newline); also written to the sink by the constructor.
        const std::string &header() const { return m_header; }
        // `ram` is the start of the guest's memory (ramBytes long). Returns the number of rows handed to the sink.
        size_t sample(const uint8_t *ram, uint32_t ramBytes, uint64_t frame, uint64_t tick);
        // True once the wrestler array pointer was non-zero in a sample (the first match has been set up).
        bool matchSeen() const { return m_matchSeen; }

    private:
        std::string readField(const FieldSpec &f, const uint8_t *ram, uint32_t ramBytes, uint32_t structBase) const;

        Config m_cfg;
        Sink m_sink;
        void *m_user;
        std::string m_header;
        std::vector<std::vector<std::string>> m_last; // per wrestler, then the absolute fields in the last slot
        bool m_matchSeen = false;
    };
}
