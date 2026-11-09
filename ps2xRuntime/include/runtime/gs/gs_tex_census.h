#pragma once

// FPWR: the texture census and dumper (task t1a; opt-in, off unless an environment variable names a file).
//
// What it is for. Texture packs (docs/mod-layer.md, step 2) will name every game texture by a key, so the key has to be chosen
// from evidence. This module watches the GPU renderer (GSGpuBackend) and, for every distinct texture a scene draws, records how
// the game uses it and computes several candidate keys, so that a script can compare them across runs (which keys stay the same
// for the same picture, which collide, which split). It can also write every texture as a PNG named by one of the keys.
//
// Switches (read once, when the backend is created; nothing here runs when none is set, and the backend then holds no Census):
//   PS2X_TEX_CENSUS=<file>   write the census to <file> (CSV, one row per scene and texture) plus side files named <file>.tiles.csv,
//                            <file>.palettes.csv, <file>.uploads.csv. The scene is named by the first line of <file>.scene, which a
//                            test script rewrites at each step; a change closes the scene (its rows are written then).
//   PS2X_TEX_DUMP=<folder>   write each new texture as <folder>/<key>.png (decoded over the guard reach rectangle of the scene that
//                            closed it; the PNG's alpha is the GS alpha scaled so 0x80 becomes 255; for indexed textures also
//                            <key>_idx.png, the index image as an 8-bit paletted PNG whose pixel values are the palette indices
//                            and whose palette is the live CLUT of the first colouring, and <key>_pal.png, that palette as 8 x 8
//                            swatches in 16 columns; further colourings of the same key are <key>__<8 hex of the CLUT hash>.png, up
//                            to eight) and an index <folder>/textures.csv. A key seen again with a larger rectangle overwrites its
//                            files. Implies the census (its file defaults to <folder>/census.csv).
//   PS2X_TEX_DUMP_KEY=<name> the key that names the files: k1, k2b_i, k2b_c, k2t_i, k2t_c, k2s_i, k2s_c, k3_i, k3_c, k3m_i
//                            (default) or k3m_c. Falls back to k2b_i for a texture without an upload to tie it to.
//   PS2X_TEX_DUMP_MAX=<n>    stop dumping after n files (default 40000).
//   PS2X_TEX_CENSUS_STATS=1  print the time the census itself took (stderr) every 600 markers and at the end.
//   PS2X_TEX_CENSUS_VERIFY=1 check the block map of uploads (see below) against the bytes the transfers really changed.
//
// What a "texture" is here. The GPU backend's texture cache names a texture by a content hash K1 (its location, format, size,
// CLUT and the bytes of every page it could live in). One K1 is one "instance"; the census keeps one row per instance and scene.
// For each instance it records the format, the declared size, the CLUT, how the draws sample it (primitive kinds, filter, wrap
// modes, texel coordinates) and the rectangle of texels they reach, as the union (bounding box) over the draws of the scene:
//   guard reach  what the GPU renderer decodes (GSGpu::TextureReach: the UV extent plus one texel each side, narrowed or widened by
//                the wrap mode);
//   tight reach  the texels the draws sample: floor(lo) .. ceil(hi) - 1 for nearest, one more texel for bilinear.
//
// The candidate keys (64-bit hashes, XXH64, written in hex):
//   k1     the texture cache's content hash: location + bytes of all pages of the declared size. The baseline.
//   k2b_i  the raw texels (indices for T4/T8, colours otherwise) inside the guard reach rectangle, with the format class and the
//          rectangle size; independent of where the texture sits in GS memory. "_i": indices only.
//   k2b_c  the same, plus the resolved CLUT entries the indices use (the colouring). Equal to k2b_i for textures without a CLUT.
//   k2t_*  as k2b_*, over the tight reach rectangle.
//   k2s_*  as k2b_*, over the 16 x 16 tiles some draw reaches (tile aligned, not the gaps of the bounding box).
//   k3_i   upload-based: the payload hashes of the host-to-local transfers that wrote the 256-byte blocks of the texels the draws
//          reach (with where the texture starts relative to each transfer). k3_c adds the colouring as k2b_c does.
//   k3m_*  as k3_*, but only the transfer that wrote most of the reached blocks.
// A transfer's payload hash covers its bytes (independent of how the DMA chunks them), the destination format and the size of its
// rectangle, not where it was sent to.
//
// Block map. The census keeps, for each of the 16384 blocks of GS memory, which host-to-local transfer wrote it last, computed from
// the transfer's geometry (the GS block and page layouts, in the .cpp). PS2X_TEX_CENSUS_VERIFY=1 compares that with what each
// transfer really changed in the memory image.
//
// Threads. Every call comes from the one thread that draws (under the backend's mutex), as the backend does. PNG files are
// encoded and written on a worker thread; when the "end" scene is closed the worker writes <file>.done once its queue is empty.
// Nothing here includes SDL, and no header that generated game code includes refers to this one.

#include "runtime/gs/gs_gpu_math.h"
#include "runtime/gs/gs_types.h"

#include <array>
#include <cstdint>
#include <memory>

namespace TexCensus
{
    // What the backend tells the census about one textured primitive, after the texture is resolved and decoded.
    struct DrawInfo
    {
        uint64_t k1 = 0;                           // the texture cache's content hash of the texture this draw used
        const GSDrawState *state = nullptr;
        const GSGpu::Setup *setup = nullptr;       // the primitive's vertices (texel coordinates or S, T, Q)
        uint32_t width = 1, height = 1;            // the texture as declared (TW, TH)
        uint32_t scaleX = 1, scaleY = 1;           // target pixels per GS pixel along each axis
        const uint8_t *vram = nullptr;             // the GS memory image
        const std::array<uint16_t, 512> *clut = nullptr; // the CLUT buffer as the draw sees it
        uint64_t marker = 0;                       // the presentation marker (VBlank) count
    };

    class Census
    {
    public:
        // Reads the switches above. Returns null when none is set.
        static std::unique_ptr<Census> FromEnvironment();
        ~Census();
        Census(const Census &) = delete;
        Census &operator=(const Census &) = delete;

        // Host-to-local transfers (direction 0 only). BeginTransfer starts a transfer and marks its blocks as written by it;
        // BeforeUpload / AfterUpload surround each GSTransfer::UploadImage call (the data arrives in chunks).
        void BeginTransfer(const GSTransferCommand &command);
        void BeforeUpload(const uint8_t *vram);
        void AfterUpload(const uint8_t *vram, const uint8_t *data, uint32_t bytes);
        // The memory image was replaced or reset: nothing is known about who wrote it.
        void MemoryReset();

        void Draw(const DrawInfo &draw);
        // Once per presentation marker: polls the scene file and writes periodic statistics.
        void Marker(uint64_t marker);
        // Closes the current scene, writes everything and stops the worker. The game's exit does not run destructors, so a test
        // script ends a run by writing the scene "end" and waiting for <file>.done instead.
        void Finish();

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
        Census();
    };
}
