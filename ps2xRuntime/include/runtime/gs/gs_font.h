#pragma once

// FPWR: HD text (task t2; docs/formats/text.md describes how the game draws text, docs/formats/fprmod.md "Font packs" the files).
//
// How the game draws text, in short. The game keeps a bitmap font (fpr.fnt): one 32 x 32 picture of 16 grey levels per glyph. Text is
// not drawn into a canvas string by string. The game has a CACHE of glyph cells: 16 pages of 4 x 4 cells of 32 x 32 texels, one GS texture
// (a 512 x 512 PSMT4 image at TBP 0x3E00, TBW 8) whose texels the game fills with glyphs as strings need them and re-sends to the GS.
// A letter on screen is a quad (two triangles, sheared for the italic look) that shows the 28 x 28 texels inside one cell; its colour
// comes from the vertex colour (the cell's palette is white with an alpha ramp), and each letter is drawn twice: its outline version
// (the glyph grown by three pixels, "bold") in dark, and the plain glyph in front.
//
// HD text. A mod can ship a sharper picture for any glyph (docs/formats/fprmod.md). The renderer cannot ask the game which glyph sits
// in which cell, but it does not need to: it sees the cell's 512 bytes when the texture is made, and the game side (an override that
// wraps the glyph fetch routine at 0x138210) tells this file the bytes of every glyph it fetches, with its code and whether it is the
// outline version. So:
//   Registry   bitmap hash -> (glyph code, bold). Filled by the override (the EE thread), read by the renderer (the GS thread).
//   Atlas      the HD version of the 512 x 512 canvas: every cell is either the mod's picture of its glyph (or, for an outline cell with
//              no picture of its own, that glyph's picture grown by 3 x S pixels) or, for a glyph with no picture, the original cell
//              enlarged smoothly. It is one R8 picture of 512 S by 512 S pixels, a COVERAGE image: the renderer looks the coverage up
//              in the live palette (a ramp: the original's 16 grey levels are 16 positions on it), so colours, alpha and the outline
//              colour come out exactly as for the original glyph. The renderer draws the game's own quads from it (shader kind 3 of
//              gs_rep.frag.hlsl); nothing about the game's layout, shear, scale or draw order changes.
// With no font files, nothing here is created and the override does one atomic load per glyph fetch.
//
// Threads: Registry is thread-safe. Atlas::Compose and the rest of Atlas are called from the GS thread; its decoding thread is its own.
// Nothing here includes SDL, and no header that generated game code includes refers to this one.

#include "runtime/gs/gs_tex_pack.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace FontHD
{
    constexpr uint32_t kCell = 32;                 // a glyph cell, in pixels at scale 1
    constexpr uint32_t kCellsAcross = 16;          // cells per row of the canvas (and rows)
    constexpr uint32_t kCanvasSize = kCell * kCellsAcross; // 512
    constexpr uint32_t kCanvasTbp = 0x3E00u;       // where the game keeps the canvas in GS memory (a PSMT4 texture, TBW 8)
    constexpr uint32_t kCanvasTbw = 8;
    constexpr int kMaxScale = 8;
    constexpr const char *kFontPrefix = "assets/font/";
    constexpr uint32_t kBoldRadius = 3;            // the outline version of a glyph grows it by this many pixels (at scale 1)

    // ---- glyph codes and names (pure functions) ----
    // The game's glyph codes are Shift-JIS: 0x20-0x7F one byte; two bytes 0x8140-0x9FFC, 0xE040-0xEAFC, 0xF040-0xF9FC. fpr.fnt has a slot for
    // each (9,872 slots), found by the game's routine at 0x138A90, which SlotOfCode repeats.
    int SlotOfCode(uint32_t code);
    uint32_t CodeOfSlot(uint32_t slot); // 0 if out of range
    // The Unicode character of a code (0: none, e.g. the game's own symbols at 0xF040-0xF9FC), and the codes of a character (a
    // character can have more than one: the NEC duplicates of Shift-JIS).
    uint32_t UnicodeOfCode(uint32_t code);
    std::vector<uint32_t> CodesOfUnicode(uint32_t codePoint);

    struct FileName
    {
        uint32_t value = 0;     // the code (code names) or the Unicode code point (character names)
        bool unicode = false;   // u0041.png (a character) rather than c41.png (a game code)
        bool bold = false;      // u0041.bold.png: the outline version
    };
    // "c41.png", "C0041.PNG", "u0041.png", "u1f600.bold.png" (any case, 1 to 6 hex digits). False for anything else.
    bool ParseFileName(const std::string &fileName, FileName &out);
    // The scale S of a glyph picture (32 S by 32 S pixels, S from 1 to 8), or 0 with a one-line reason.
    int ScaleOf(uint32_t imgW, uint32_t imgH, std::string *reason = nullptr);

    // ---- glyph identity ----
    uint64_t BitmapHash(const uint8_t *packed512); // the 512 bytes of a glyph as the game holds them (two pixels per byte, left pixel low)
    constexpr uint32_t PackId(uint32_t code, bool bold) { return (code << 1) | (bold ? 1u : 0u); }
    constexpr uint32_t IdCode(uint32_t id) { return id >> 1; }
    constexpr bool IdBold(uint32_t id) { return (id & 1u) != 0u; }

    class Registry
    {
    public:
        Registry();
        ~Registry();
        Registry(const Registry &) = delete;
        Registry &operator=(const Registry &) = delete;
        // Called for every glyph bitmap the game fetches (a blank bitmap is ignored). Two glyphs with the same bitmap: the first one
        // reported keeps it.
        void Note(uint32_t code, bool bold, const uint8_t *packed512);
        bool Lookup(uint64_t bitmapHash, uint32_t &id) const;
        size_t Size() const;
        void Clear();
        // "code bold hash" lines, for tests and for checking the tool's decoding against what the game really fetched.
        std::vector<std::string> Lines() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
    Registry &Glyphs(); // the process-wide registry

    // True while something wants the game's glyph fetches reported (a font pack is on, or the author tools are). The override reads it.
    bool CaptureWanted();
    void SetCaptureWanted(bool wanted);

    // ---- the HD canvas ----
    struct AtlasStats
    {
        uint32_t files = 0;         // glyph files indexed
        uint32_t refused = 0;       // distinct refusals
        uint32_t decoded = 0;       // glyph pictures decoded
        uint32_t scale = 1;         // the atlas scale S
        uint64_t composes = 0;      // atlas versions made
        uint32_t lastHd = 0;        // cells of the last version that came from a mod's picture
        uint32_t lastFallback = 0;  // cells enlarged from the original
        uint32_t lastUnknown = 0;   // non-blank cells whose glyph the game never reported (enlarged from the original)
        uint64_t bytesAtlas = 0;    // the atlas picture's size now
    };

    class Atlas
    {
    public:
        // `source` lists and reads the files of the enabled mods and the dev pack (TexPack::FileSource). Indexes "assets/font/" and reads
        // the picture sizes (to fix the scale); starts the thread that decodes the pictures.
        explicit Atlas(std::shared_ptr<TexPack::FileSource> source);
        ~Atlas();
        Atlas(const Atlas &) = delete;
        Atlas &operator=(const Atlas &) = delete;

        bool empty() const;  // no glyph file at all
        // Changes whenever pictures may have changed: the index was rebuilt (reload), or the scale grew. Lock-free.
        uint64_t generation() const;
        uint32_t scale() const;
        AtlasStats stats() const;
        // Reads the file list and sizes again (a hot reload): forgets decoded pictures. Returns the number of glyph files.
        uint32_t reload();

        // The canvas the game uploaded: kCanvasSize x kCanvasSize palette indices, one byte per texel, row by row. Fills `pixels` (the HD
        // canvas, (kCanvasSize * S)^2 bytes of coverage 0..255 at 255 / 15 per original grey level) and returns true; false if nothing
        // is there to replace (no files).
        bool Compose(const uint8_t *indices, const std::vector<uint8_t> *&pixels);

        // The coverage picture the atlas uses for one glyph, at the atlas scale: the mod's picture, or (bold with no picture of its own)
        // the plain picture grown, or false when the mod has none. For tests and the author dump.
        bool GlyphPicture(uint32_t code, bool bold, std::vector<uint8_t> &out);

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };

    // ---- pictures (pure helpers, also used by the tests) ----
    // Coverage of a decoded glyph picture: the alpha when the picture is not entirely opaque, otherwise the brightness.
    void CoverageOfRgba(const uint8_t *rgba, uint32_t pixels, std::vector<uint8_t> &out);
    // The original cell enlarged S times with bilinear filtering (the 16 grey levels become coverage 0..255).
    void EnlargeOriginal(const uint8_t *indices32x32, uint32_t stride, uint32_t scale, std::vector<uint8_t> &out);
    // Resamples a square coverage picture from `from` pixels to `to` (bilinear; exact copy when equal).
    void ResizeSquare(const std::vector<uint8_t> &in, uint32_t from, uint32_t to, std::vector<uint8_t> &out);
    // Grows a square picture: every pixel becomes the largest value within `radius` pixels in x and y.
    void Dilate(const std::vector<uint8_t> &in, uint32_t size, uint32_t radius, std::vector<uint8_t> &out);
}
