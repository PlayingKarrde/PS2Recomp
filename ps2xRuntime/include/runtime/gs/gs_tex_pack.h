#pragma once

// FPWR: texture packs, the replacement images of the enabled mods (task t1b; format in docs/formats/fprmod.md "Texture packs").
//
// A texture pack is a mod of kind "assets" whose package holds, under assets/textures/, PNG files named by texture keys
// (gs_tex_keys.h, written by the dumper gs_tex_dump.h):
//   <key>.png       an RGBA replacement picture, for any colouring of the texture
//   <key>.idx.png   an index image (8-bit greyscale or paletted PNG whose values are the original palette indices); the renderer
//                   looks the colours up in the texture's live palette, so one image serves every colouring
// A key is the live key k3_i, or k3_c when the file is for one colouring of an indexed texture only (the k3_c RGBA file wins over the
// k3_i files whichever mod provides them). The picture's size must be the replaced rectangle's size times a whole number S from 1 to
// 8, the same in both directions. Among files with the same key the one from the mod that loads last wins (an .idx.png beats a
// .png inside one mod).
//
// This file holds the part that needs no GPU and no mod manager, so it can be unit-tested:
//   FileSource    where the files come from (the mod manager's virtual lookup in the game, a folder in tests and the replayer)
//   Store         the index of the files, the decoded-image cache and the thread that decodes
// The renderer (gs_gpu_backend.cpp) asks the store for the image of a texture when it creates it; the store never blocks it: an image
// that is not decoded yet is "pending" (the renderer draws the original this time and asks again), and a worker thread decodes it.
//
// Threads: Store's functions may be called from any thread; the worker is the store's own. A FileSource must allow reads from the
// worker thread. Nothing here includes SDL, and no header that generated game code includes refers to this one.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace TexPack
{
    constexpr const char *kTexturePrefix = "assets/textures/";
    constexpr int kMaxScale = 8;

    // ---- names and the size rule (pure functions) ----
    struct ParsedName
    {
        uint64_t key = 0;
        bool index = false; // <key>.idx.png
    };
    // "0123456789abcdef.png" or "0123456789ABCDEF.idx.png" (any case); false for anything else.
    bool ParseFileName(const std::string &fileName, ParsedName &out);
    // The scale S of a picture of imgW x imgH for a replaced rectangle of rectW x rectH, or 0 with a one-line reason.
    int ScaleOf(uint32_t imgW, uint32_t imgH, int rectW, int rectH, std::string *reason = nullptr);

    // ---- where the files come from ----
    class FileSource
    {
    public:
        struct Entry
        {
            std::string path;     // "assets/textures/<name>"
            std::string provider; // the mod's id
            uint32_t order = 0;   // the provider's place in the load order; larger loads later and wins
        };
        virtual ~FileSource() = default;
        // The files under `prefix` that the enabled mods provide, one entry per path (the last provider's).
        virtual std::vector<Entry> list(const std::string &prefix) = 0;
        // Reads one of them. Called from the store's worker thread.
        virtual bool read(const std::string &path, std::vector<uint8_t> &out, std::string &error) = 0;
        // A file was refused or could not be used: one line for the player, about the mod `provider`.
        virtual void problem(const std::string &provider, const std::string &message) { (void)provider; (void)message; }
    };

    // The files of a folder: <root>/assets/textures/*. Provider "folder", order 0. For tests and the replayer.
    std::shared_ptr<FileSource> MakeDirectorySource(const std::string &root);

    // The DEV PACK (task t2; docs/modding-textures.md): a folder of loose files that acts like a mod loaded after every other one, so an author
    // sees an edited picture at once, with no packing and no restart. Its layout is the mod's `assets/` folder without that name:
    //     <folder>/textures/<key>.png, <key>.idx.png      texture pictures
    //     <folder>/font/<glyph>.png                       HD glyphs
    // Its files are listed under the same paths a mod's would have ("assets/textures/...", "assets/font/..."), with provider "dev-pack" and the
    // highest order, and they replace a mod's file of the same path. Everything else comes from `base` (the enabled mods; may be null).
    // A thread polls the folder (every 400 ms, and takes a change only once the files have stopped changing for a poll, since editors write
    // in two steps); changed() tells the renderer, once per change, that it should read the packs again.
    class DevPackSource final : public FileSource
    {
    public:
        static constexpr const char *kProvider = "dev-pack";
        // `watch`: start the polling thread (the author tools are on). Without it the folder is still read, once, at the start.
        DevPackSource(std::string folder, std::shared_ptr<FileSource> base, int pollMillis = 400, bool watch = true);
        ~DevPackSource() override;
        DevPackSource(const DevPackSource &) = delete;
        DevPackSource &operator=(const DevPackSource &) = delete;

        std::vector<Entry> list(const std::string &prefix) override;
        bool read(const std::string &path, std::vector<uint8_t> &out, std::string &error) override;
        void problem(const std::string &provider, const std::string &message) override;

        // True once after the folder's files changed (any thread). The first call after construction is false.
        bool changed();
        const std::string &folder() const;
        // The number of files in the folder now (not cached: reads the folder).
        uint32_t fileCount() const;
        // Complaints about dev-pack files (refused pictures), at most 24, newest last.
        std::vector<std::string> notes() const;
        // Forget the notes (before a reload).
        void clearNotes();

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };

    // Whether the author tools are on for this run: the texture tracker runs even with no pack, the dev pack is watched and the "Dump
    // textures" button works. Set by the host from PS2X_AUTHORING=1, the overlay's Texture tools setting or a dev-pack folder that has files.
    void SetAuthoring(bool on);
    bool Authoring();
    // The dev pack the host made (also the default source's outermost layer), so the renderer can ask it whether files changed.
    void SetDevPack(std::shared_ptr<DevPackSource> pack);
    std::shared_ptr<DevPackSource> DevPack();

    // The source the renderer uses. The game's host registers the mod manager's (ps2_host_mods.cpp) before the renderer is made;
    // PS2X_TEX_PACK_DIR=<folder> (a test aid, and how the replayer gets a pack) names a folder in its place. Null when neither is set.
    void SetDefaultSource(std::shared_ptr<FileSource> source);
    std::shared_ptr<FileSource> DefaultSource();

    // ---- the store ----
    // Ramp: a coverage picture (R8, 0..255) that the renderer looks up on the texture's live palette as a ramp: 255 is the palette's last
    // entry, 0 its first, in between the colours blend. The HD text atlas (gs_font.h) is one; it never comes from a file.
    enum class Kind : uint8_t { Rgba, Index, Ramp };

    // A decoded replacement picture. Rgba pixels are 4 bytes each with the alpha already in the GS's scale (0x80 = opaque); Index
    // pixels are the raw palette indices.
    struct Image
    {
        Kind kind = Kind::Rgba;
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> pixels;
        uint8_t maxIndex = 0; // Index: the largest value present
        std::string path, provider;
    };

    struct Lookup
    {
        enum class State : uint8_t
        {
            None,    // no file for this texture
            Pending, // there is one; it is being read and decoded
            Ready,   // `image` and `scale` are set
            Refused  // there is one, and it cannot be used (the reason went to FileSource::problem and the log)
        };
        State state = State::None;
        std::shared_ptr<const Image> image;
        int scale = 0;
        bool colouring = false; // the file is a k3_c override
    };

    struct Stats
    {
        uint32_t files = 0;          // texture files indexed
        uint32_t ignored = 0;        // files under assets/textures/ whose names are not keys
        uint32_t decoded = 0;        // pictures decoded so far
        uint32_t refused = 0;        // distinct refusals
        uint32_t pending = 0;        // pictures waiting for the worker
        uint64_t cacheBytes = 0;     // decoded pictures held
        uint64_t generation = 0;     // changes when the index is rebuilt
        uint64_t readNanos = 0;      // time the worker spent reading files from the source (the zip) ...
        uint64_t decodeNanos = 0;    // ... and decoding PNGs, in all
    };

    class Store
    {
    public:
        struct Options
        {
            uint64_t cacheBytes = 192ull << 20; // decoded pictures kept in memory (PS2X_TEX_PACK_CACHE_MB)
            bool prefetch = true;               // decode pictures in the background before they are asked for (PS2X_TEX_PACK_PREFETCH=0 turns off)
        };
        Store(std::shared_ptr<FileSource> source, const Options &options);
        ~Store();
        Store(const Store &) = delete;
        Store &operator=(const Store &) = delete;

        // Reads the file list again (a hot reload): forgets decoded pictures and refusals. Returns the number of texture files.
        uint32_t reload();
        bool empty() const;
        Stats stats() const;
        // Changes whenever the index is rebuilt; a texture that was looked up under an older one asks again. Lock-free.
        uint64_t generation() const;

        // The replacement for a texture whose live keys are `k3i` and `k3c` (0 = none) and whose replaced rectangle is rectW x rectH.
        // `indexed`: the texture has a palette, so an index image fits (and `paletteSize`, 16 or 256, bounds its values).
        Lookup find(uint64_t k3i, uint64_t k3c, int rectW, int rectH, bool indexed, uint32_t paletteSize);

        // Waits until the worker has nothing left to decode (tests).
        void waitIdle() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}
