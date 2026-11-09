#pragma once

// FPWR: the texture census (task t1a; opt-in, off unless PS2X_TEX_CENSUS names a file). Since task t1b it reads the uploads that
// each texture was built from from the key tracker (gs_tex_keys.h), which the renderer also uses for the live texture keys; the
// dumper that t1a had here is now gs_tex_dump.h.
//
// What it is for. Texture packs (docs/mod-layer.md, step 2) name every game texture by a key, so the key was chosen from evidence.
// This module watches the GPU renderer (GSGpuBackend) and, for every distinct texture a scene draws, records how the game uses it and
// computes several candidate keys, so that a script can compare them across runs (which keys stay the same for the same picture,
// which collide, which split). t1b added the live key (gs_tex_keys.h, LiveKey) as extra columns, to compare it with the candidates.
//
// Switches (read once, when the backend is created; nothing here runs when it is not set, and the backend then holds no Census):
//   PS2X_TEX_CENSUS=<file>   write the census to <file> (CSV, one row per scene and texture) plus side files named <file>.tiles.csv,
//                            <file>.palettes.csv, <file>.uploads.csv. The scene is named by the first line of <file>.scene, which a
//                            test script rewrites at each step; a change closes the scene (its rows are written then).
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
// (The live key of t1b differs from k3_i in one way: it does not depend on what a scene reaches. See gs_tex_keys.h.)
//
// Block map. The tracker keeps, for each of the 16384 blocks of GS memory, which host-to-local transfer wrote it last, computed from
// the transfer's geometry. PS2X_TEX_CENSUS_VERIFY=1 compares that with what each transfer really changed in the memory image.
//
// Threads. Every call comes from the one thread that draws (under the backend's mutex), as the backend does. Nothing here includes
// SDL, and no header that generated game code includes refers to this one.

#include "runtime/gs/gs_gpu_math.h"
#include "runtime/gs/gs_tex_keys.h"
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
        const TexKeys::LiveKey *live = nullptr;    // the live key of this texture, when the renderer computed one (task t1b)
    };

    class Census
    {
    public:
        // Reads the switches above. Returns null when PS2X_TEX_CENSUS is not set. The tracker must outlive the census.
        static std::unique_ptr<Census> FromEnvironment(TexKeys::Tracker &tracker);
        ~Census();
        Census(const Census &) = delete;
        Census &operator=(const Census &) = delete;

        // Host-to-local transfers: BeginTransfer / BeforeUpload / AfterUpload surround the tracker's own calls (the tracker does the
        // bookkeeping of writers; these keep the census's tables and the verify mode).
        void BeginTransfer(const GSTransferCommand &command);
        void BeforeUpload(const uint8_t *vram);
        void AfterUpload(const uint8_t *vram);

        void Draw(const DrawInfo &draw);
        // Once per presentation marker: polls the scene file and writes periodic statistics.
        void Marker(uint64_t marker);
        // Closes the current scene, writes everything. The game's exit does not run destructors, so a test script ends a run by
        // writing the scene "end" and waiting for <file>.done instead.
        void Finish();

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
        Census();
    };
}
