#pragma once
// Load-time checks for replaced model files of FPR.ROM (task md6; docs/mod-layer.md "ROM files and model mods").
//
// What it is for. A mod (or the development folder PS2X_ROM_OVERRIDE_DIR) can replace a whole file of the game's archive. The
// arena, crowd and corner-post models live in aikoh/r/*.all files (docs/formats/fpr-model.md). The game's model loader
// (model_init 0x13FCB0) checks nothing: a file that does not parse is drawn from garbage, a mesh with too many triangles breaks the
// picture, and a file that needs more heap than is free hangs the game while a match loads. So before such a file is served, it is
// read here and compared with the game's own copy of the same file, with the limits measured in task md4:
//
//   - the container and every model blob parse (offsets and counts inside the file, triangle corners inside the mesh);
//   - the pair layout is the original's: same number of pairs, each pair the same kind, each model the same mesh count
//     (the game's depth layers are per mesh index);
//   - no mesh has more than 7,281 triangles (one DMA tag per mesh, its size a 16-bit field at 0x12F860) or 65,535 vertices;
//   - the estimated extra heap over the original stays under 6.0 MB (refused) / 3.0 MB (a warning). 6.66 MB is free in a
//     one-on-one match in arena 0, the only case measured;
//   - more than 16,000 extra triangles gets a warning about speed: with the native transform (task md5, on by default) the x16 hall
//     (+15,600) ran at 59.7 frames/s, and the GS side is estimated to top out near 19,000 drawn triangles a frame.
//
// The functions have no global state; rom_override_tests.cpp exercises them with made-up files.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2x_fpr_model
{
    constexpr uint32_t kMaxTrianglesPerMesh = 7281; // 9n + 4 quad words must fit the 16-bit DMA size (0x12F860)
    constexpr uint32_t kMaxVerticesPerMesh = 65535; // u16 indices
    constexpr uint32_t kMaxMeshes = 128;            // model_init's limit
    constexpr int64_t kHeapWarnBytes = 3000000;
    constexpr int64_t kHeapRefuseBytes = 6000000;
    constexpr int64_t kSpeedWarnTriangles = 16000; // with the native transform (md5) x16 (+15,600) ran at 59.7 frames/s

    enum class PairKind : uint8_t
    {
        Empty,
        Picture, // a bare VPF0 picture
        TexturePack,
        Model,
        Other,
    };

    const char *pairKindName(PairKind kind);

    struct MeshInfo
    {
        std::string name;
        uint32_t vertices = 0;
        uint32_t triangles = 0;
    };

    struct PairInfo
    {
        PairKind kind = PairKind::Empty;
        std::vector<MeshInfo> meshes; // models only
    };

    struct FileInfo
    {
        uint64_t bytes = 0;
        std::vector<PairInfo> pairs;
    };

    // Is this archive path one of the model containers (aikoh/r/*.all)? `lowerPath` is lower case with '/'.
    bool isModelContainer(const std::string &lowerPath);

    // Reads a .all container and its model blobs. False with a one-line reason when it does not parse.
    bool parseContainer(const uint8_t *data, size_t bytes, FileInfo &out, std::string &error);

    // What the file costs on the game's heap: the file itself (loaded whole) plus, per mesh, the triangle packets (TRILIST,
    // 288 bytes per triangle + 128) and the transformed vertices (TRIBUF, 32 bytes per vertex), each block rounded up to 0x40
    // with a 0x40 header. Matches the md4 runs within 5% (x4: 1.08 MB estimated, 1.03 measured; x16: 5.32 / 5.08).
    int64_t heapCost(const FileInfo &info);

    uint64_t triangleCount(const FileInfo &info);

    struct Verdict
    {
        bool refused = false;
        std::vector<std::string> problems; // why it is refused (each one line)
        std::vector<std::string> warnings; // allowed, but worth saying
        int64_t extraHeap = 0;             // over the original
        int64_t extraTriangles = 0;
    };

    // Checks a replacement of `lowerPath` against the game's own file. `original` may be empty (it could not be read): then only the
    // replacement's own rules are checked.
    Verdict checkReplacement(const std::string &lowerPath, const uint8_t *original, size_t originalBytes, const uint8_t *replacement,
                             size_t replacementBytes);

    // "1.08 MB" style text.
    std::string megabytes(int64_t bytes);
}
