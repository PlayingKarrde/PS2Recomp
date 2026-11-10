#pragma once

// FPWR: the texture dumper for texture authors (task t1b; opt-in, off unless PS2X_TEX_DUMP names a folder).
//
// What it does. Every texture the GPU renderer creates is given its live key (gs_tex_keys.h). The dumper writes the REPLACED
// RECTANGLE of each new key as a PNG named by the key, exactly the part of the texture that a replacement image stands for, so an
// author edits what the game will replace and the file names are the names a texture pack uses (docs/formats/fprmod.md):
//   <k3_i>.png       RGBA, at the original size (an indexed texture is shown in the colours of the first colouring seen)
//   <k3_i>.idx.png   indexed textures only: an 8-bit paletted PNG whose pixel values are the original palette indices (the file a
//                    pack's HD index image replaces) and whose palette is the live palette of the first colouring seen
//   <k3_i>.pal.png   indexed textures only: that palette as 8 x 8 swatches in 16 columns (to look at; not a pack file)
//   <k3_c>.png       indexed textures only: the RGBA picture of a further colouring of the same texture (up to PS2X_TEX_DUMP_COLOURINGS
//                    of them per key, default 4); named by the colouring's own key, so it is already a "this colouring only" override
//   textures.csv     one row per file group: key, key type, the texture's place and format, the rectangle, how many writers made it,
//                    and the class: "ok", or why it is not worth replacing (composed: a text canvas or tiled texture; movie: a
//                    non-indexed slot that showed more than 32 different contents)
//   slots.csv        the slots (TBP, TBW, PSM, size) classed as movie frames, for tools that join them with textures.csv
// A file that already exists in the folder is not written again, so a folder can collect textures over many runs. The files are
// game imagery: keep the folder under baserom/ (gitignored); tools/texpack.py turns a chosen set of them into a pack source.
//
// Switches (read once, when the backend is created):
//   PS2X_TEX_DUMP=<folder>            the folder (created if needed)
//   PS2X_TEX_DUMP_MAX=<n>             stop after n files (default 40000)
//   PS2X_TEX_DUMP_COLOURINGS=<n>      further colourings kept per indexed key (default 4, 0 = none)
//
// Files are written on the calling thread (the GS thread) as textures appear: about a millisecond for a part, tens of milliseconds
// for a full-screen background, so a dumping run is slower, and nothing is lost when the game exits without running destructors.
// Nothing here includes SDL, and no header that generated game code includes refers to this one.

#include "runtime/gs/gs_tex_keys.h"
#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace TexDump
{
    // What the renderer tells the dumper about a texture it has just created and keyed.
    struct Sighting
    {
        uint64_t k1 = 0;                 // the texture cache's content hash of this instance
        TexKeys::TextureDesc desc;
        TexKeys::LiveKey key;            // as the tracker answered; for a fallback key `rect` is the tile rectangle
        uint64_t primary = 0;            // the key the files are named by (key.k3i, or the fallback key); 0: no key
        bool fallback = false;           // `primary` is a k2s_i (the texture had no writer)
        uint64_t k3c = 0;                // the colouring key (indexed textures with a small rectangle), else 0
        uint64_t clutHash = 0;           // indexed textures: a hash of the resolved palette
        const uint32_t *palette = nullptr; // indexed textures: the 256 resolved entries (r | g << 8 | b << 16 | a << 24)
        uint32_t paletteCount = 0;       // 16 or 256
        const std::vector<uint8_t> *texels = nullptr; // TexKeys::ReadTexels of the rectangle, if the caller has it already
        const uint8_t *vram = nullptr;   // the GS memory image (to read the texels when `texels` is null)
        GSTexaReg texa{};
        uint64_t marker = 0;
    };

    // A picture for a contact sheet: RGBA8, straight alpha (PNG scale).
    struct Tile
    {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };
    // A PNG of the tiles in a grid (`columns` per row, each fitted into a `cell` x `cell` square on a grey checkerboard). False if it cannot be written.
    bool WriteContactSheet(const std::string &path, const std::vector<Tile> &tiles, uint32_t cell = 96, uint32_t columns = 16);

    class Dumper
    {
    public:
        // Reads the switches above; null when PS2X_TEX_DUMP is not set.
        static std::unique_ptr<Dumper> FromEnvironment();
        // The one-off dump of the F1 panel's and the Mods tab's "Dump textures on this screen" button: the pictures go to `imageDir`, the
        // two csv files to `csvDir`, and the pictures of the files written are kept for a contact sheet (TakePictures). Null if the folders
        // cannot be made.
        static std::unique_ptr<Dumper> Create(const std::string &imageDir, const std::string &csvDir, uint32_t maxFiles = 4000, uint32_t colourings = 0);
        std::vector<Tile> TakePictures();
        ~Dumper();
        Dumper(const Dumper &) = delete;
        Dumper &operator=(const Dumper &) = delete;

        void Seen(const Sighting &sighting);
        uint32_t FilesWritten() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
        Dumper();
    };

    // The PNG alpha of a GS alpha (0x80 = opaque), and back. Round trip: FromPngAlpha(ToPngAlpha(a)) == a for a in 0..0x80.
    inline uint8_t ToPngAlpha(uint32_t gsAlpha) { return gsAlpha >= 0x80u ? 255u : static_cast<uint8_t>(gsAlpha * 2u); }
    inline uint8_t FromPngAlpha(uint32_t pngAlpha) { return static_cast<uint8_t>((pngAlpha + 1u) >> 1); }
}
