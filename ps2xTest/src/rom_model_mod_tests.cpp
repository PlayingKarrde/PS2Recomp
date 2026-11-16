#include "MiniTest.h"
#include "Stubs/Helpers/FprModelCheck.h"
#include "Stubs/Helpers/RomOverride.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Tests for task md6 (docs/mod-layer.md "ROM files and model mods"): the model checks of FprModelCheck.{h,cpp} and the planning of
// replacements offered by mods and the development folder (RomOverride.h, makePlan with candidates). The .all files here are made up
// from the layout in docs/formats/fpr-model.md (not by calling the code under test); nothing is game data.

namespace
{
    namespace fm = ps2x_fpr_model;
    namespace ro = ps2x_rom_override;
    using Bytes = std::vector<uint8_t>;

    void put32(Bytes &b, size_t off, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            b[off + i] = static_cast<uint8_t>(v >> (8 * i));
    }

    void put16(Bytes &b, size_t off, uint16_t v)
    {
        b[off] = static_cast<uint8_t>(v);
        b[off + 1] = static_cast<uint8_t>(v >> 8);
    }

    void putF(Bytes &b, size_t off, float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        put32(b, off, u);
    }

    size_t align(size_t n, size_t a)
    {
        return (n + a - 1) / a * a;
    }

    struct MeshSpec
    {
        uint32_t vertices;
        uint32_t triangles;
    };

    // A model blob: header, mesh table, one texture name, the indices, then the vertices on a 16-byte boundary.
    Bytes model(const std::vector<MeshSpec> &meshes)
    {
        const size_t n = meshes.size();
        const size_t texOff = 0x18 + n * 0x5C;
        size_t idx = texOff + 16;
        std::vector<size_t> idxAt, vtxAt;
        for (const MeshSpec &m : meshes)
        {
            idxAt.push_back(idx);
            idx += size_t(m.triangles) * 6;
        }
        size_t vtx = align(idx, 16);
        const size_t vtxStart = vtx;
        for (const MeshSpec &m : meshes)
        {
            vtxAt.push_back(vtx);
            vtx += size_t(m.vertices) * 0x30;
        }
        Bytes b(vtx, 0);
        put32(b, 0, static_cast<uint32_t>(n));
        put32(b, 4, 0x18);
        put32(b, 8, 1);
        put32(b, 12, static_cast<uint32_t>(texOff));
        put32(b, 16, static_cast<uint32_t>(vtxStart));
        put32(b, 20, static_cast<uint32_t>(texOff + 16));
        std::memcpy(&b[texOff], "T_test.tga", 10);
        uint32_t firstV = 0, firstT = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const size_t r = 0x18 + i * 0x5C;
            put16(b, r, i == 0 ? 0xFFFF : 0);
            put16(b, r + 2, static_cast<uint16_t>(i));
            const std::string name = "Mesh" + std::to_string(i);
            std::memcpy(&b[r + 4], name.data(), name.size());
            putF(b, r + 0x2C, 1.0f); // quaternion w
            putF(b, r + 0x30, 1.0f);
            putF(b, r + 0x34, 1.0f);
            putF(b, r + 0x38, 1.0f);
            put32(b, r + 0x3C, meshes[i].vertices);
            put32(b, r + 0x40, firstV);
            put32(b, r + 0x44, static_cast<uint32_t>(vtxAt[i]));
            put32(b, r + 0x48, meshes[i].triangles);
            put32(b, r + 0x4C, firstT);
            put32(b, r + 0x50, static_cast<uint32_t>(idxAt[i]));
            firstV += meshes[i].vertices;
            firstT += meshes[i].triangles;
            for (uint32_t k = 0; k < meshes[i].triangles * 3; ++k)
                put16(b, idxAt[i] + size_t(k) * 2, static_cast<uint16_t>((k / 3 + k % 3) % meshes[i].vertices));
            for (uint32_t v = 0; v < meshes[i].vertices; ++v)
            {
                const size_t at = vtxAt[i] + size_t(v) * 0x30;
                putF(b, at, static_cast<float>(v));
                putF(b, at + 12, 1.0f);
                putF(b, at + 24, 1.0f);
                put32(b, at + 44, 128);
            }
        }
        return b;
    }

    // A texture pack: an offset table of 0x40 bytes, then one tiny VPF0 picture.
    Bytes texpack()
    {
        Bytes b(0x80, 0);
        put32(b, 0, 0x40);
        std::memcpy(&b[0x40], "VPF0", 4);
        return b;
    }

    // A .all container: 16 pairs, blobs on 0x40 boundaries, padded to 0x40.
    Bytes container(const std::vector<Bytes> &blobs)
    {
        const size_t slots = 16;
        Bytes out(slots * 8, 0);
        for (size_t i = 0; i < blobs.size(); ++i)
        {
            if (blobs[i].empty())
                continue;
            out.resize(align(out.size(), 0x40), 0);
            put32(out, i * 8, static_cast<uint32_t>(out.size()));
            put32(out, i * 8 + 4, static_cast<uint32_t>(blobs[i].size()));
            out.insert(out.end(), blobs[i].begin(), blobs[i].end());
        }
        out.resize(align(out.size(), 0x40), 0);
        return out;
    }

    // The "game's" arena: a hall of three meshes with its texture pack, a crowd of one mesh with its texture pack.
    Bytes arena(const std::vector<MeshSpec> &hall)
    {
        return container({model(hall), texpack(), model({{8, 6}}), texpack()});
    }

    const std::vector<MeshSpec> kHall = {{30, 20}, {20, 10}, {12, 8}};

    fm::Verdict check(const Bytes &original, const Bytes &replacement)
    {
        return fm::checkReplacement("aikoh/r/arena00.all", original.data(), original.size(), replacement.data(), replacement.size());
    }

    bool anyContains(const std::vector<std::string> &lines, const std::string &needle)
    {
        for (const std::string &l : lines)
            if (l.find(needle) != std::string::npos)
                return true;
        return false;
    }

    bool anyNote(const ro::Plan &plan, const std::string &source, bool problem, const std::string &needle)
    {
        for (const ro::PlanNote &n : plan.notes)
            if (n.source == source && n.problem == problem && n.text.find(needle) != std::string::npos)
                return true;
        return false;
    }

    ro::Candidate modFile(const std::string &id, const std::string &path, const Bytes &bytes)
    {
        ro::Candidate c;
        c.path = path;
        c.source = id;
        auto data = std::make_shared<const Bytes>(bytes);
        c.read = [data]() { return data; };
        return c;
    }

    ro::Toc toc(const Bytes &arenaBytes)
    {
        ro::Toc t;
        t.files.push_back(ro::TocFile{"aikoh/r/arena00.all", 100, static_cast<uint32_t>(arenaBytes.size()), 0x90});
        t.files.push_back(ro::TocFile{"yoneda/other.bin", 300, 100, 0x9C});
        return t;
    }

    ro::OriginalReader reader(const Bytes &arenaBytes)
    {
        return [arenaBytes](const ro::TocFile &f, std::vector<uint8_t> &bytes)
        {
            if (f.path != "aikoh/r/arena00.all")
                return false;
            bytes = arenaBytes;
            return true;
        };
    }
}

void register_rom_model_mod_tests()
{
    MiniTest::Case("RomModelMods", [](TestCase &tc)
    {
        tc.Run("a made-up arena parses; the heap estimate follows the md4 formula", [](TestCase &t)
        {
            const Bytes a = container({model({{3, 1}}), texpack()});
            fm::FileInfo info;
            std::string error;
            t.IsTrue(fm::parseContainer(a.data(), a.size(), info, error), "parses: " + error);
            t.Equals(info.pairs.size(), static_cast<size_t>(16), "16 pairs");
            t.IsTrue(info.pairs[0].kind == fm::PairKind::Model && info.pairs[1].kind == fm::PairKind::TexturePack &&
                         info.pairs[2].kind == fm::PairKind::Empty,
                     "kinds: model, texture pack, empty");
            // file + TRILIST block(288*1+128 = 416 -> 448 + 64) + TRIBUF block(32*3 = 96 -> 128 + 64)
            t.Equals(fm::heapCost(info), static_cast<int64_t>(a.size() + 512 + 192), "file + 512 + 192");
            t.IsTrue(fm::isModelContainer("aikoh/r/arena00.all") && fm::isModelContainer("aikoh/r/post12.all"), "model containers");
            t.IsFalse(fm::isModelContainer("yoneda/fprwaza.bin") || fm::isModelContainer("aikoh/r/sub/x.all") ||
                          fm::isModelContainer("aikoh/all.eff"),
                      "other files are not");
        });

        tc.Run("the game's own file passes with nothing to say", [](TestCase &t)
        {
            const Bytes a = arena(kHall);
            const fm::Verdict v = check(a, a);
            t.IsFalse(v.refused, "not refused");
            t.IsTrue(v.problems.empty() && v.warnings.empty(), "no problems or warnings");
            t.Equals(v.extraHeap, static_cast<int64_t>(0), "no extra heap");
        });

        tc.Run("a file that does not parse is refused", [](TestCase &t)
        {
            const Bytes a = arena(kHall);
            Bytes junk(4096, 0xAB);
            fm::Verdict v = check(a, junk);
            t.IsTrue(v.refused && anyContains(v.problems, "does not read as a model file"), "junk refused");
            Bytes bad = a;
            // the first triangle corner of the hall's first mesh: point it past the mesh's 30 vertices
            const uint32_t hallOff = bad[0] | (bad[1] << 8) | (bad[2] << 16) | (uint32_t(bad[3]) << 24);
            const uint32_t idxOff = hallOff + 0x18 + 3 * 0x5C + 16;
            put16(bad, idxOff, 30);
            v = check(a, bad);
            t.IsTrue(v.refused && anyContains(v.problems, "past its last vertex"), "a corner past the last vertex is refused");
            Bytes outside = a;
            put32(outside, 4, 0x7FFFFF00); // pair 0's size runs past the end
            v = check(a, outside);
            t.IsTrue(v.refused && anyContains(v.problems, "lies outside the file"), "a pair outside the file is refused");
        });

        tc.Run("the layout must stay: mesh count and pair kinds", [](TestCase &t)
        {
            const Bytes a = arena(kHall);
            fm::Verdict v = check(a, arena({{30, 20}, {20, 10}}));
            t.IsTrue(v.refused && anyContains(v.problems, "keep the number and order"), "a missing mesh is refused");
            v = check(a, container({model(kHall), model({{3, 1}}), model({{8, 6}}), texpack()}));
            t.IsTrue(v.refused && anyContains(v.problems, "pair 1 is model; in the game's file it is texture pack"), "a pair of another kind is refused");
            v = check(a, arena({{300, 400}, {20, 10}, {12, 8}}));
            t.IsFalse(v.refused, "more vertices and triangles in a mesh are fine");
            t.Equals(v.extraTriangles, static_cast<int64_t>(380), "380 extra triangles");
        });

        tc.Run("7,281 triangles in a mesh pass, 7,282 are refused", [](TestCase &t)
        {
            const Bytes a = arena(kHall);
            fm::Verdict v = check(a, arena({{3000, 7281}, {20, 10}, {12, 8}}));
            t.IsFalse(v.refused, "7,281 accepted");
            t.IsFalse(anyContains(v.warnings, "below 60 frames"), "no speed warning for 7,261 extra triangles");
            v = check(a, arena({{3000, 6000}, {3000, 6000}, {3000, 6000}}));
            t.IsTrue(anyContains(v.warnings, "below 60 frames"), "over 16,000 extra triangles: a speed warning");
            v = check(a, arena({{3000, 7282}, {20, 10}, {12, 8}}));
            t.IsTrue(v.refused && anyContains(v.problems, "mesh 0 'Mesh0' has 7,282 triangles; the game can draw at most 7,281 per mesh."),
                     "7,282 refused with the mesh named");
        });

        tc.Run("heap: over 3 MB extra warns, over 6 MB refuses", [](TestCase &t)
        {
            const Bytes a = arena(kHall);
            // two meshes of 6,000 triangles: about 3.5 MB of packets
            fm::Verdict v = check(a, arena({{3000, 6000}, {3000, 6000}, {12, 8}}));
            t.IsFalse(v.refused, "3.5 MB is allowed");
            t.IsTrue(v.extraHeap > fm::kHeapWarnBytes && v.extraHeap < fm::kHeapRefuseBytes, "between the two limits: " + fm::megabytes(v.extraHeap));
            t.IsTrue(anyContains(v.warnings, "more memory than the game's file"), "with a memory warning");
            // three meshes of 7,200: about 6.3 MB
            v = check(a, arena({{3000, 7200}, {3000, 7200}, {3000, 7200}}));
            t.IsTrue(v.refused && anyContains(v.problems, "more memory than the game's file; at most 6.00 MB"), "over 6 MB refused: " + fm::megabytes(v.extraHeap));
        });

        tc.Run("a plan from mods: the later mod wins; a refused file falls back to the mod below; notes for both", [](TestCase &t)
        {
            const Bytes game = arena(kHall);
            const Bytes good = arena({{40, 30}, {20, 10}, {12, 8}});
            const Bytes better = arena({{50, 40}, {20, 10}, {12, 8}});
            const Bytes broken(2048, 0x11);
            const ro::Toc tc0 = toc(game);

            std::vector<ro::Candidate> c = {modFile("a.first", "aikoh/r/arena00.all", good), modFile("b.second", "aikoh/r/arena00.all", better)};
            ro::Plan plan = ro::makePlan(tc0, c, reader(game));
            t.Equals(plan.replacements.size(), static_cast<size_t>(1), "one replacement");
            if (!plan.replacements.empty())
            {
                const ro::Replacement &r = plan.replacements[0];
                t.Equals(r.source, std::string("b.second"), "the later mod wins");
                t.IsTrue(r.data && *r.data == better, "served from its bytes in memory");
                t.Equals(r.newSize, static_cast<uint32_t>(better.size()), "its size");
            }
            t.IsTrue(anyNote(plan, "b.second", false, "Replaces aikoh/r/arena00.all"), "the winner says what it replaces");
            t.IsTrue(anyNote(plan, "b.second", false, "also in a.first; this mod loads later and wins"), "and who it beat");
            t.IsTrue(anyNote(plan, "a.first", false, "also in b.second, which loads later and wins"), "the loser is told");

            c = {modFile("a.first", "aikoh/r/arena00.all", good), modFile("b.second", "aikoh/r/arena00.all", broken)};
            plan = ro::makePlan(tc0, c, reader(game));
            t.IsTrue(plan.replacements.size() == 1 && plan.replacements[0].source == "a.first", "a refused winner falls back to the mod below");
            t.IsTrue(anyNote(plan, "b.second", true, "not used, the file does not read as a model file"), "the refusal is a problem note");
            t.IsTrue(anyContains(plan.log, "REFUSED aikoh/r/arena00.all from the mod b.second"), "and a log line");

            c = {modFile("b.second", "aikoh/r/arena00.all", broken)};
            plan = ro::makePlan(tc0, c, reader(game));
            t.IsTrue(plan.replacements.empty(), "with no other offer the game keeps its own file");
        });

        tc.Run("a plan: unchecked files are taken with a note; unknown paths are named; dev folder wins and only warns", [](TestCase &t)
        {
            const Bytes game = arena(kHall);
            const ro::Toc tc0 = toc(game);
            std::vector<ro::Candidate> c = {modFile("a.mod", "yoneda/other.bin", Bytes(500, 7)), modFile("a.mod", "nope/missing.bin", Bytes(5, 1))};
            ro::Plan plan = ro::makePlan(tc0, c, reader(game));
            t.Equals(plan.replacements.size(), static_cast<size_t>(1), "the unchecked file is taken");
            t.IsTrue(anyNote(plan, "a.mod", false, "yoneda/other.bin: not checked; the game's buffer for this file is unknown and this file is larger"),
                     "with a 'not checked' note");
            t.IsTrue(anyNote(plan, "a.mod", true, "nope/missing.bin: there is no such file in the game's archive"), "an unknown path is named");
            c = {modFile("a.mod", "yoneda/other.bin", Bytes())};
            plan = ro::makePlan(tc0, c, reader(game));
            t.IsTrue(plan.replacements.empty() && anyNote(plan, "a.mod", true, "empty"), "an empty file is refused");

            // the development folder: a file over the per-mesh limit is used, with a warning, and wins over the mod
            const ro::Plan devPlan = [&]() {
                std::vector<ro::Candidate> cc = {modFile("a.mod", "aikoh/r/arena00.all", arena({{40, 30}, {20, 10}, {12, 8}}))};
                ro::Candidate dev;
                dev.path = "aikoh/r/arena00.all";
                dev.dev = true;
                auto bytes = std::make_shared<const Bytes>(arena({{3000, 7300}, {20, 10}, {12, 8}}));
                // a dev candidate reads its host file; this one has none, so stand the host read in with a temporary file
                std::error_code ec;
                const auto dir = std::filesystem::temp_directory_path(ec) / "ps2x_md6_dev";
                std::filesystem::create_directories(dir / "aikoh" / "r", ec);
                dev.host = dir / "aikoh" / "r" / "arena00.all";
                {
                    std::FILE *f = std::fopen(dev.host.string().c_str(), "wb");
                    if (f)
                    {
                        std::fwrite(bytes->data(), 1, bytes->size(), f);
                        std::fclose(f);
                    }
                }
                cc.push_back(dev);
                ro::Plan p = ro::makePlan(tc0, cc, reader(game));
                std::filesystem::remove_all(dir, ec);
                return p;
            }();
            t.IsTrue(devPlan.replacements.size() == 1 && devPlan.replacements[0].source.empty() && !devPlan.replacements[0].host.empty(),
                     "the development folder's file wins");
            t.IsTrue(anyContains(devPlan.log, "would be refused in a mod: mesh 0 'Mesh0' has 7,300 triangles"), "with a warning only");
            t.IsFalse(anyNote(devPlan, "a.mod", false, "Replaces"), "the mod below it replaces nothing");
        });

        tc.Run("a replacement held in memory is served by sector, padded with zeros", [](TestCase &t)
        {
            ro::Plan plan;
            ro::Replacement r;
            Bytes data(5000);
            for (size_t i = 0; i < data.size(); ++i)
                data[i] = static_cast<uint8_t>(i * 3);
            r.data = std::make_shared<const Bytes>(data);
            r.newSize = 5000;
            r.sectors = 3;
            r.virtualSector = ro::kVirtualSectorBase;
            plan.replacements.push_back(r);
            Bytes got(2048, 0xEE);
            t.IsTrue(ro::readVirtualSectors(plan, ro::kVirtualSectorBase + 2, 1, got.data(), got.size()), "the last sector is served");
            t.IsTrue(std::equal(data.begin() + 4096, data.end(), got.begin()), "with the file's tail");
            t.IsTrue(got[5000 - 4096] == 0 && got[2047] == 0, "then zeros");
        });
    });
}
