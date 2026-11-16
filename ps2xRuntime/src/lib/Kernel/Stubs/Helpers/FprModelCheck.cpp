// Load-time checks for replaced model files of FPR.ROM (task md6). See FprModelCheck.h; the layout is docs/formats/fpr-model.md and
// mirrors tools/fprmodel.py (classify, parse_container, parse_model).
#include "FprModelCheck.h"

#include <cstdio>
#include <cstring>

namespace ps2x_fpr_model
{
    namespace
    {
        constexpr uint32_t kHeaderBytes = 0x18;
        constexpr uint32_t kMeshBytes = 0x5C;
        constexpr uint32_t kVertexBytes = 0x30;
        constexpr uint32_t kNameBytes = 16;

        uint32_t rd32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        }

        uint16_t rd16(const uint8_t *p)
        {
            return static_cast<uint16_t>(p[0] | (p[1] << 8));
        }

        std::string cstr(const uint8_t *p, size_t n)
        {
            std::string s;
            for (size_t i = 0; i < n && p[i]; ++i)
                s.push_back((p[i] >= 0x20 && p[i] < 0x7F) ? static_cast<char>(p[i]) : '?');
            return s;
        }

        std::string hex(uint64_t v)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
            return buf;
        }

        std::string withCommas(int64_t v)
        {
            const bool neg = v < 0;
            std::string digits = std::to_string(neg ? -v : v);
            std::string out;
            for (size_t i = 0; i < digits.size(); ++i)
            {
                if (i && (digits.size() - i) % 3 == 0)
                    out.push_back(',');
                out.push_back(digits[i]);
            }
            return neg ? "-" + out : out;
        }

        PairKind classify(const uint8_t *b, size_t n)
        {
            if (n == 0)
                return PairKind::Empty;
            if (n >= 4 && std::memcmp(b, "VPF0", 4) == 0)
                return PairKind::Picture;
            if (n >= 8)
            {
                const uint32_t first = rd32(b);
                if (first && first % 4 == 0 && first <= 0x100 && uint64_t(first) + 4 <= n && std::memcmp(b + first, "VPF0", 4) == 0)
                    return PairKind::TexturePack;
            }
            if (n >= kHeaderBytes && rd32(b + 4) == kHeaderBytes && rd32(b) <= kMaxMeshes)
                return PairKind::Model;
            return PairKind::Other;
        }

        bool parseModel(const uint8_t *b, size_t n, std::vector<MeshInfo> &meshes, std::string &error)
        {
            const uint32_t count = rd32(b);
            if (count == 0)
            {
                error = "has no meshes";
                return false;
            }
            const uint32_t ntex = rd32(b + 8), texOff = rd32(b + 12);
            if (uint64_t(kHeaderBytes) + uint64_t(count) * kMeshBytes > n || uint64_t(texOff) + uint64_t(ntex) * kNameBytes > n)
            {
                error = "its tables run past the end of the model";
                return false;
            }
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t *r = b + kHeaderBytes + size_t(i) * kMeshBytes;
                MeshInfo m;
                m.name = cstr(r + 4, kNameBytes);
                const int16_t parent = static_cast<int16_t>(rd16(r));
                m.vertices = rd32(r + 0x3C);
                const uint32_t vOff = rd32(r + 0x44);
                m.triangles = rd32(r + 0x48);
                const uint32_t iOff = rd32(r + 0x50);
                const std::string label = "mesh " + std::to_string(i) + " '" + m.name + "'";
                if (i == 0 ? parent != -1 : (parent < 0 || parent >= static_cast<int>(i)))
                {
                    error = label + (i == 0 ? " must be the root (parent -1)" : " names a parent that does not come before it");
                    return false;
                }
                if (uint64_t(vOff) + uint64_t(m.vertices) * kVertexBytes > n || uint64_t(iOff) + uint64_t(m.triangles) * 6 > n)
                {
                    error = label + " runs past the end of the model";
                    return false;
                }
                for (uint64_t k = 0; k < uint64_t(m.triangles) * 3; ++k)
                {
                    if (rd16(b + iOff + k * 2) >= m.vertices)
                    {
                        error = label + " has a triangle corner past its last vertex";
                        return false;
                    }
                }
                meshes.push_back(std::move(m));
            }
            return true;
        }

        int64_t block(int64_t bytes)
        {
            return ((bytes + 0x3F) / 0x40) * 0x40 + 0x40;
        }
    }

    const char *pairKindName(PairKind kind)
    {
        switch (kind)
        {
        case PairKind::Empty: return "empty";
        case PairKind::Picture: return "picture";
        case PairKind::TexturePack: return "texture pack";
        case PairKind::Model: return "model";
        default: return "unknown data";
        }
    }

    bool isModelContainer(const std::string &lowerPath)
    {
        static const std::string prefix = "aikoh/r/";
        return lowerPath.size() > prefix.size() + 4 && lowerPath.compare(0, prefix.size(), prefix) == 0 &&
               lowerPath.compare(lowerPath.size() - 4, 4, ".all") == 0 && lowerPath.find('/', prefix.size()) == std::string::npos;
    }

    bool parseContainer(const uint8_t *data, size_t bytes, FileInfo &out, std::string &error)
    {
        out = FileInfo();
        out.bytes = bytes;
        if (bytes < 16)
        {
            error = "the file is too short to be a .all container";
            return false;
        }
        const uint32_t first = rd32(data);
        if (first % 8 || first < 8 || first > 0x400 || first > bytes)
        {
            error = "the start of the file is not a pair table (first offset " + hex(first) + ")";
            return false;
        }
        const uint32_t slots = first / 8;
        for (uint32_t i = 0; i < slots; ++i)
        {
            const uint32_t off = rd32(data + i * 8), size = rd32(data + i * 8 + 4);
            PairInfo pair;
            if (size == 0)
            {
                if (off != 0 && off != bytes)
                {
                    error = "pair " + std::to_string(i) + " has size 0 but offset " + hex(off);
                    return false;
                }
                out.pairs.push_back(pair);
                continue;
            }
            if (off < first || uint64_t(off) + size > bytes)
            {
                error = "pair " + std::to_string(i) + " (offset " + hex(off) + ", size " + hex(size) + ") lies outside the file";
                return false;
            }
            pair.kind = classify(data + off, size);
            if (pair.kind == PairKind::Model)
            {
                std::string why;
                if (!parseModel(data + off, size, pair.meshes, why))
                {
                    error = "the model in pair " + std::to_string(i) + " " + why;
                    return false;
                }
            }
            out.pairs.push_back(std::move(pair));
        }
        return true;
    }

    int64_t heapCost(const FileInfo &info)
    {
        int64_t cost = static_cast<int64_t>(info.bytes);
        for (const PairInfo &p : info.pairs)
        {
            for (const MeshInfo &m : p.meshes)
                cost += block(288ll * m.triangles + 128) + block(32ll * m.vertices);
        }
        return cost;
    }

    uint64_t triangleCount(const FileInfo &info)
    {
        uint64_t n = 0;
        for (const PairInfo &p : info.pairs)
        {
            for (const MeshInfo &m : p.meshes)
                n += m.triangles;
        }
        return n;
    }

    std::string megabytes(int64_t bytes)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / 1e6);
        return buf;
    }

    Verdict checkReplacement(const std::string &lowerPath, const uint8_t *original, size_t originalBytes, const uint8_t *replacement,
                             size_t replacementBytes)
    {
        Verdict v;
        (void)lowerPath;
        FileInfo repl;
        std::string error;
        if (!parseContainer(replacement, replacementBytes, repl, error))
        {
            v.refused = true;
            v.problems.push_back("the file does not read as a model file: " + error + ".");
            return v;
        }
        for (size_t i = 0; i < repl.pairs.size(); ++i)
        {
            const PairInfo &p = repl.pairs[i];
            if (p.meshes.size() > kMaxMeshes)
                v.problems.push_back("the model in pair " + std::to_string(i) + " has " + std::to_string(p.meshes.size()) +
                                     " meshes; the game allows at most " + std::to_string(kMaxMeshes) + ".");
            for (size_t k = 0; k < p.meshes.size(); ++k)
            {
                const MeshInfo &m = p.meshes[k];
                if (m.triangles > kMaxTrianglesPerMesh)
                    v.problems.push_back("mesh " + std::to_string(k) + " '" + m.name + "' has " + withCommas(m.triangles) +
                                         " triangles; the game can draw at most " + withCommas(kMaxTrianglesPerMesh) + " per mesh.");
                if (m.vertices > kMaxVerticesPerMesh)
                    v.problems.push_back("mesh " + std::to_string(k) + " '" + m.name + "' has " + withCommas(m.vertices) +
                                         " vertices; at most " + withCommas(kMaxVerticesPerMesh) + " per mesh.");
            }
        }

        FileInfo orig;
        const bool haveOriginal = original && originalBytes && parseContainer(original, originalBytes, orig, error);
        if (!haveOriginal)
        {
            v.warnings.push_back("the game's own copy could not be read, so the layout and heap use were not compared.");
        }
        else
        {
            if (orig.pairs.size() != repl.pairs.size())
            {
                v.problems.push_back("it has " + std::to_string(repl.pairs.size()) + " pairs; the game's file has " +
                                     std::to_string(orig.pairs.size()) + ".");
            }
            else
            {
                for (size_t i = 0; i < orig.pairs.size(); ++i)
                {
                    const PairInfo &a = orig.pairs[i], &b = repl.pairs[i];
                    if (a.kind != b.kind)
                        v.problems.push_back("pair " + std::to_string(i) + " is " + pairKindName(b.kind) + "; in the game's file it is " +
                                             pairKindName(a.kind) + ".");
                    else if (a.kind == PairKind::Model && a.meshes.size() != b.meshes.size())
                        v.problems.push_back("the model in pair " + std::to_string(i) + " has " + std::to_string(b.meshes.size()) +
                                             " meshes; the game's has " + std::to_string(a.meshes.size()) +
                                             " (keep the number and order of the meshes: the depth layers go by mesh number).");
                }
            }
            v.extraHeap = heapCost(repl) - heapCost(orig);
            v.extraTriangles = static_cast<int64_t>(triangleCount(repl)) - static_cast<int64_t>(triangleCount(orig));
            if (v.extraHeap > kHeapRefuseBytes)
                v.problems.push_back("it needs about " + megabytes(v.extraHeap) + " more memory than the game's file; at most " +
                                     megabytes(kHeapRefuseBytes) + " (about 6.66 MB is free in a one-on-one match).");
            else if (v.extraHeap > kHeapWarnBytes)
                v.warnings.push_back("it needs about " + megabytes(v.extraHeap) +
                                     " more memory than the game's file; matches with more wrestlers or a cage may not have that much free.");
            if (v.extraTriangles > kSpeedWarnTriangles)
                v.warnings.push_back("it adds " + withCommas(v.extraTriangles) + " triangles; the game may run below 60 frames per second.");
        }
        v.refused = !v.problems.empty();
        return v;
    }
}
