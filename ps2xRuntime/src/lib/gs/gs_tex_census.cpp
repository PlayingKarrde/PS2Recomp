// The texture census (task t1a; since task t1b it shares the key tracker, gs_tex_keys.h, with the live texture keys). See
// include/runtime/gs/gs_tex_census.h for what it measures and the switches.
//
// Reading guide:
//   Census::Impl            instances (one per texture content), uploads, scenes, row output, the candidate keys
//   (the block map of uploads, the stream hash and the GS block layouts are in gs_tex_keys.cpp; the PNG writer in gs_png.cpp;
//    the dumper for texture authors, which task t1a had here, is gs_tex_dump.cpp)
//
// Everything is opt-in; the backend holds no Census unless PS2X_TEX_CENSUS names a file.

#include "runtime/gs/gs_tex_census.h"

#include "runtime/gs/gs_recording.h"
#include "runtime/gs/gs_texel.h"
#include "runtime/gs/ps2_gs_memory.h"

#include "runtime/gs/gs_tex_keys.h"
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TexCensus
{
namespace
{
    uint64_t nowNs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    inline uint64_t combine(uint64_t h, uint64_t v)
    {
        return TexKeys::Combine(h, v);
    }

    inline uint64_t hash64(const void *data, size_t size, uint64_t seed)
    {
        return TexKeys::HashBytes(data, size, seed);
    }

    void appendf(std::string &out, const char *fmt, ...)
    {
        char buffer[512];
        va_list args;
        va_start(args, fmt);
        const int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        if (n > 0)
            out.append(buffer, static_cast<size_t>(std::min<int>(n, sizeof(buffer) - 1)));
    }

    constexpr uint32_t kTile = 16u;
    constexpr uint32_t kTilesPerSide = 64u;

    using TexKeys::Rect;
    using TexKeys::Unite;
    using TexKeys::WriterEntry;
    using TexKeys::WriterInfo;

    // The texels the draws sample (see the header): like GSGpu::TextureReach without the one texel of guard on each side.
    Rect tightReach(const GSGpu::Setup &setup, uint64_t clampReg, uint32_t width, uint32_t height, bool bilinear)
    {
        Rect whole{0, 0, static_cast<int>(width) - 1, static_cast<int>(height) - 1};
        if (setup.count == 0 || width == 0u || height == 0u)
            return whole;
        const float size[2] = {static_cast<float>(width), static_cast<float>(height)};
        float lo[2] = {1.0e30f, 1.0e30f}, hi[2] = {-1.0e30f, -1.0e30f};
        for (uint32_t i = 0; i < setup.count; ++i)
        {
            const GSGpu::Vertex &v = setup.v[i];
            float c[2];
            if (setup.texelCoords)
            {
                c[0] = v.u;
                c[1] = v.v;
            }
            else
            {
                if (!(v.q > 0.0f))
                    return whole;
                c[0] = v.u / v.q * size[0];
                c[1] = v.v / v.q * size[1];
            }
            for (int k = 0; k < 2; ++k)
            {
                if (!(std::fabs(c[k]) < 1.0e6f))
                    return whole;
                lo[k] = std::min(lo[k], c[k]);
                hi[k] = std::max(hi[k], c[k]);
            }
        }
        const int extent[2] = {static_cast<int>(width), static_cast<int>(height)};
        const uint32_t mode[2] = {static_cast<uint32_t>(clampReg & 3u), static_cast<uint32_t>((clampReg >> 2) & 3u)};
        const int regionMin[2] = {static_cast<int>((clampReg >> 4) & 0x3FFu), static_cast<int>((clampReg >> 24) & 0x3FFu)};
        const int regionMax[2] = {static_cast<int>((clampReg >> 14) & 0x3FFu), static_cast<int>((clampReg >> 34) & 0x3FFu)};
        int first[2], last[2];
        for (int k = 0; k < 2; ++k)
        {
            int l, h;
            if (bilinear)
            {
                l = static_cast<int>(std::floor(lo[k] - 0.5f));
                h = static_cast<int>(std::floor(hi[k] - 0.5f)) + 1;
            }
            else
            {
                l = static_cast<int>(std::floor(lo[k]));
                h = static_cast<int>(std::ceil(hi[k])) - 1;
                if (h < l)
                    h = l;
            }
            first[k] = 0;
            last[k] = extent[k] - 1;
            switch (mode[k])
            {
            case 0u: // REPEAT
                if (l >= 0 && h <= extent[k] - 1)
                {
                    first[k] = l;
                    last[k] = h;
                }
                break;
            case 1u: // CLAMP
                first[k] = std::clamp(l, 0, extent[k] - 1);
                last[k] = std::clamp(h, 0, extent[k] - 1);
                break;
            case 2u: // REGION_CLAMP
                first[k] = std::clamp(std::min(std::max(l, regionMin[k]), regionMax[k]), 0, extent[k] - 1);
                last[k] = std::clamp(std::min(std::max(h, regionMin[k]), regionMax[k]), 0, extent[k] - 1);
                break;
            default:
                break;
            }
        }
        return Rect{first[0], first[1], last[0], last[1]};
    }

    struct Tile
    {
        std::vector<uint8_t> data; // 16 x 16 indices (indexed textures) or RGBA texels (4 bytes each)
    };

    struct Instance
    {
        uint64_t k1 = 0;
        uint32_t tbp = 0, tbw = 0, psm = 0, w = 0, h = 0, texa = 0;
        uint32_t cpsm = 0, csm = 0, csa = 0, cbp = 0;
        uint64_t clutHash = 0;
        bool indexed = false, four = false;
        int dynamic = 0; // 1: many different contents at one place (movie frames), 2: large colour texture
        uint64_t firstMarker = 0, lastMarker = 0;
        uint64_t draws = 0, sprites = 0, tris = 0, bilinear = 0, nearest = 0, fst = 0, stq = 0;
        uint32_t wmsMask = 0, wmtMask = 0;
        bool region = false;
        int regionMin[2] = {0, 0}, regionMax[2] = {0, 0};
        Rect guard, tight, lastGuard, lastTight;
        double magU[2] = {1.0e30, 0.0}, magV[2] = {1.0e30, 0.0}; // sprites: texels per GS pixel along each axis (min, max)
        double triDensity[2] = {1.0e30, 0.0};                    // triangles: sqrt(texel area / GS pixel area), min and max
        std::bitset<kTilesPerSide * kTilesPerSide> reached;
        std::unordered_map<uint32_t, Tile> tiles; // ty * 64 + tx
        std::vector<WriterInfo> writers;
        uint32_t cells = 0, cellsKnown = 0;
        uint32_t palette[256] = {};
        uint32_t paletteCount = 0;
        std::vector<WriterEntry> clutWriters;
        TexKeys::LiveKey live; // the key the renderer gave this texture (task t1b), if the renderer computed one
        bool hasLive = false;
    };
    struct UploadAgg
    {
        uint64_t count = 0, bytes = 0;
        uint32_t dpsm = 0, dbw = 0, w = 0, h = 0, firstDbp = 0;
        std::set<uint32_t> dbps;
        uint64_t firstMarker = 0, lastMarker = 0;
        std::string firstScene;
    };

    struct KeySet
    {
        uint64_t k1 = 0, k2b_i = 0, k2b_c = 0, k2t_i = 0, k2t_c = 0, k2s_i = 0, k2s_c = 0, k3_i = 0, k3_c = 0, k3m_i = 0, k3m_c = 0;
    };

} // namespace

// ------------------------------------------------------------------------------------------------
struct Census::Impl
{
    // ---- configuration
    std::string censusPath, scenePath;
    bool verify = false, statsOn = false;
    TexKeys::Tracker *tracker = nullptr; // owned by the backend; the census reads writers from it

    // ---- output
    std::FILE *csv = nullptr, *tilesCsv = nullptr, *palCsv = nullptr;
    uint64_t rowCounter = 0;
    std::string scene = "boot";
    bool finished = false;
    static const char *header()
    {
        return "row,scene,marker_first,marker_last,k1,tbp,tbw,psm,w,h,indexed,cpsm,csm,csa,cbp,clut_hash,texa,"
               "draws,sprite_draws,tri_draws,bilinear_draws,nearest_draws,fst_draws,stq_draws,wms_mask,wmt_mask,region,"
               "g_x0,g_y0,g_x1,g_y1,t_x0,t_y0,t_x1,t_y1,guard_area,tight_area,reached_tiles,"
               "mag_u_min,mag_u_max,mag_v_min,mag_v_max,tri_dens_min,tri_dens_max,dynamic,"
               "k2b_i,k2b_c,k2t_i,k2t_c,k2s_i,k2s_c,k3_i,k3_c,k3m_i,k3m_c,"
               "writers,cells,cells_known,main_payload,main_w,main_h,main_psm,main_dbw,main_dbp,main_rel,main_cells,clut_writer,clut_writers,"
               "used_entries,palette_n,"
               "live_status,live_k3_i,live_x0,live_y0,live_w,live_h,live_writers,live_exact";
    }

    // ---- uploads (the tracker keeps the block map; the census tabulates the transfers)
    std::unordered_map<uint64_t, UploadAgg> uploads;
    uint64_t uploadsDropped = 0; // distinct transfers not tabled once `uploads` is full
    // verify mode
    std::vector<uint8_t> predicted = std::vector<uint8_t>(TexKeys::kBlocks, 0u);
    std::vector<uint8_t> beforePages;
    GSGpu::PageSpan beforeSpan;
    uint64_t verifyChunks = 0, verifyChangedBlocks = 0, verifyViolations = 0, verifyPredicted = 0, verifyUnchangedPredicted = 0;
    std::vector<uint8_t> changedSeen = std::vector<uint8_t>(TexKeys::kBlocks, 0u);

    // ---- instances
    std::unordered_map<uint64_t, std::unique_ptr<Instance>> instances;
    std::unordered_map<uint64_t, uint32_t> slotCounts;

    // ---- cost
    uint64_t costNs = 0, drawCalls = 0, uploadCalls = 0, lastStatsMarker = 0, curMarker = 0;
    // where the cost goes (printed with PS2X_TEX_CENSUS_STATS=1 and when a scene closes)
    uint64_t costBeginNs = 0, costAfterNs = 0, costCreateNs = 0, costGrowNs = 0, costPollNs = 0, costCloseNs = 0, instancesCreated = 0, growths = 0, tilesCaptured = 0;
    uint64_t scenesClosed = 0;

    ~Impl()
    {
        for (std::FILE *f : {csv, tilesCsv, palCsv})
            if (f)
                std::fclose(f);
    }

    // ---------------------------------------------------------------- uploads
    // Called by the tracker when a transfer is complete.
    void transferClosed(const TexKeys::TransferRec &r)
    {
        const uint64_t payload = r.payload();
        auto found = uploads.find(payload);
        if (found == uploads.end())
        {
            // A movie uploads thousands of different frames and strips: keep the table (and the file written from it) bounded.
            if (uploads.size() >= 300000u)
            {
                ++uploadsDropped;
                return;
            }
            found = uploads.emplace(payload, UploadAgg{}).first;
        }
        UploadAgg &a = found->second;
        if (a.count == 0)
        {
            a.dpsm = r.dpsm;
            a.dbw = r.dbw;
            a.w = r.w;
            a.h = r.h;
            a.firstDbp = r.dbp;
            a.firstMarker = r.marker;
            a.firstScene = scene;
        }
        ++a.count;
        a.bytes += r.bytes;
        a.lastMarker = curMarker;
        if (a.dbps.size() < 8)
            a.dbps.insert(r.dbp);
    }

    void beginTransfer(const GSTransferCommand &c)
    {
        if (!verify)
            return;
        std::fill(predicted.begin(), predicted.end(), 0);
        const TexKeys::TransferRec *r = tracker->CurrentTransfer();
        if (r)
            TexKeys::ForEachBlock(r->dpsm, r->dbp, r->dbw, static_cast<int>(r->x), static_cast<int>(r->y), static_cast<int>(r->w), static_cast<int>(r->h),
                                  [&](uint32_t block, int, int, int, int) { predicted[block] = 1; });
        std::fill(changedSeen.begin(), changedSeen.end(), 0);
        for (uint8_t p : predicted)
            verifyPredicted += p;
        (void)c;
    }


    // ---------------------------------------------------------------- instances
    static uint32_t readTexel(GSMem::TexturePageCache &cache, const uint8_t *vram, const Instance &I, const GSTexaReg &texa, uint32_t u, uint32_t v)
    {
        const uint32_t raw = GSMem::ReadTexture(cache, vram, I.psm, I.tbp, I.tbw, u, v);
        switch (I.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return GSTexel::applyTexa(texa, static_cast<uint8_t>(I.psm), raw);
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return GSTexel::applyTexa(texa, static_cast<uint8_t>(I.psm), GSTexel::Rgba5551ToRgba8888(static_cast<uint16_t>(raw)));
        case GS_PSM_T8:
        case GS_PSM_T8H:
            return raw & 0xFFu;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return raw & 0x0Fu;
        default:
            return 0xFFFF00FFu;
        }
    }

    void captureTiles(Instance &I, const Rect &r, const DrawInfo &d)
    {
        GSMem::TexturePageCache cache;
        for (int ty = r.y0 / static_cast<int>(kTile); ty <= r.y1 / static_cast<int>(kTile); ++ty)
            for (int tx = r.x0 / static_cast<int>(kTile); tx <= r.x1 / static_cast<int>(kTile); ++tx)
            {
                const uint32_t key = static_cast<uint32_t>(ty) * kTilesPerSide + static_cast<uint32_t>(tx);
                if (I.tiles.count(key))
                    continue;
                Tile t;
                t.data.assign(I.indexed ? 256u : 1024u, 0);
                for (uint32_t v = 0; v < kTile; ++v)
                    for (uint32_t u = 0; u < kTile; ++u)
                    {
                        const uint32_t x = static_cast<uint32_t>(tx) * kTile + u, y = static_cast<uint32_t>(ty) * kTile + v;
                        if (x >= I.w || y >= I.h)
                            continue;
                        const uint32_t value = readTexel(cache, d.vram, I, d.state->texa, x, y);
                        if (I.indexed)
                            t.data[v * kTile + u] = static_cast<uint8_t>(value);
                        else
                            std::memcpy(&t.data[(v * kTile + u) * 4u], &value, 4);
                    }
                I.tiles.emplace(key, std::move(t));
                ++tilesCaptured;
            }
    }

    Instance *instanceFor(const DrawInfo &d)
    {
        auto it = instances.find(d.k1);
        if (it != instances.end())
            return it->second.get();
        auto inst = std::make_unique<Instance>();
        Instance &I = *inst;
        const GSDrawState &s = *d.state;
        const GSTex0Reg &t = s.context.tex0;
        I.k1 = d.k1;
        I.tbp = t.tbp0;
        I.tbw = t.tbw;
        I.psm = t.psm;
        I.w = d.width;
        I.h = d.height;
        I.texa = static_cast<uint32_t>(s.texa.ta0) | (static_cast<uint32_t>(s.texa.ta1) << 8) | (s.texa.aem ? 1u << 16 : 0u);
        I.four = GSTexel::isFourBitIndexedPsm(t.psm);
        I.indexed = I.four || GSTexel::isEightBitIndexedPsm(t.psm);
        I.firstMarker = d.marker;
        if (d.live)
        {
            I.live = *d.live;
            I.hasLive = true;
        }
        if (I.indexed)
        {
            I.cpsm = t.cpsm;
            I.csm = t.csm;
            I.csa = t.csa;
            I.cbp = t.cbp;
            I.paletteCount = I.four ? 16u : 256u;
            for (uint32_t i = 0; i < 256u; ++i)
                I.palette[i] = GSTexel::LookupClutEntry(d.clut->data(), s.texa, static_cast<uint8_t>(i % I.paletteCount), t.cpsm, t.csm, t.csa, t.psm);
            std::vector<uint32_t> pal(I.palette, I.palette + I.paletteCount);
            I.clutHash = hash64(pal.data(), pal.size() * 4u, I.paletteCount);
            // who wrote the CLUT: 16 x 16 entries for 8 bit textures, 8 x 2 for 4 bit (CSM1 layout)
            std::vector<WriterInfo> cw;
            uint32_t cells = 0, known = 0;
            tracker->CollectWriters(t.cpsm, t.cbp, 1, 0, 0, I.four ? 8 : 16, I.four ? 2 : 16, cw, cells, known);
            for (const WriterInfo &w : cw)
                I.clutWriters.push_back(w.entry);
        }
        // movie frames: many different contents at one place, in a non-indexed format
        const uint64_t slot = combine(combine(combine(I.tbp, I.tbw), combine(I.psm, I.w)), I.h);
        const uint32_t seen = ++slotCounts[slot];
        if (!I.indexed && seen > 32u)
            I.dynamic = 1;
        Instance *raw = inst.get();
        instances.emplace(d.k1, std::move(inst));
        return raw;
    }

    void draw(const DrawInfo &d)
    {
        if (!d.state || !d.setup || !d.vram || !d.clut)
            return;
        const uint64_t tCreate = nowNs();
        const size_t before = instances.size();
        Instance &I = *instanceFor(d);
        if (instances.size() != before)
        {
            costCreateNs += nowNs() - tCreate;
            ++instancesCreated;
        }
        const GSDrawState &s = *d.state;
        const GSGpu::Setup &setup = *d.setup;
        I.lastMarker = d.marker;
        ++I.draws;
        const bool sprite = s.prim.type == GS_PRIM_SPRITE;
        sprite ? ++I.sprites : ++I.tris;
        s.linearFilter ? ++I.bilinear : ++I.nearest;
        s.prim.fst ? ++I.fst : ++I.stq;
        const uint64_t clamp = s.context.clamp;
        I.wmsMask |= 1u << (clamp & 3u);
        I.wmtMask |= 1u << ((clamp >> 2) & 3u);
        if (!I.region && ((clamp & 3u) >= 2u || ((clamp >> 2) & 3u) >= 2u))
        {
            I.region = true;
            I.regionMin[0] = static_cast<int>((clamp >> 4) & 0x3FFu);
            I.regionMax[0] = static_cast<int>((clamp >> 14) & 0x3FFu);
            I.regionMin[1] = static_cast<int>((clamp >> 24) & 0x3FFu);
            I.regionMax[1] = static_cast<int>((clamp >> 34) & 0x3FFu);
        }
        if (sprite && setup.count >= 3 && setup.texelCoords)
        {
            float x0 = setup.v[0].x, x1 = x0, y0 = setup.v[0].y, y1 = y0, u0 = setup.v[0].u, u1 = u0, v0 = setup.v[0].v, v1 = v0;
            for (uint32_t i = 1; i < setup.count; ++i)
            {
                x0 = std::min(x0, setup.v[i].x);
                x1 = std::max(x1, setup.v[i].x);
                y0 = std::min(y0, setup.v[i].y);
                y1 = std::max(y1, setup.v[i].y);
                u0 = std::min(u0, setup.v[i].u);
                u1 = std::max(u1, setup.v[i].u);
                v0 = std::min(v0, setup.v[i].v);
                v1 = std::max(v1, setup.v[i].v);
            }
            const double pw = (x1 - x0) / std::max(1u, d.scaleX), ph = (y1 - y0) / std::max(1u, d.scaleY);
            if (pw > 0.0)
            {
                const double mu = (u1 - u0) / pw;
                I.magU[0] = std::min(I.magU[0], mu);
                I.magU[1] = std::max(I.magU[1], mu);
            }
            if (ph > 0.0)
            {
                const double mv = (v1 - v0) / ph;
                I.magV[0] = std::min(I.magV[0], mv);
                I.magV[1] = std::max(I.magV[1], mv);
            }
        }
        else if (!sprite && setup.count == 3)
        {
            double px[3], py[3], tu[3], tv[3];
            bool ok = true;
            for (int i = 0; i < 3; ++i)
            {
                px[i] = setup.v[i].x / std::max(1u, d.scaleX);
                py[i] = setup.v[i].y / std::max(1u, d.scaleY);
                if (setup.texelCoords)
                {
                    tu[i] = setup.v[i].u;
                    tv[i] = setup.v[i].v;
                }
                else if (setup.v[i].q > 0.0f)
                {
                    tu[i] = setup.v[i].u / setup.v[i].q * I.w;
                    tv[i] = setup.v[i].v / setup.v[i].q * I.h;
                }
                else
                    ok = false;
            }
            if (ok)
            {
                const double areaPix = std::fabs((px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]));
                const double areaTex = std::fabs((tu[1] - tu[0]) * (tv[2] - tv[0]) - (tu[2] - tu[0]) * (tv[1] - tv[0]));
                if (areaPix > 1.0e-3)
                {
                    const double density = std::sqrt(areaTex / areaPix);
                    I.triDensity[0] = std::min(I.triDensity[0], density);
                    I.triDensity[1] = std::max(I.triDensity[1], density);
                }
            }
        }
        const GSGpu::TexelRect gr = GSGpu::TextureReach(setup, clamp, I.w, I.h);
        const Rect G{gr.x0, gr.y0, gr.x1, gr.y1};
        const Rect T = tightReach(setup, clamp, I.w, I.h, s.linearFilter);
        if (G == I.lastGuard && T == I.lastTight)
            return;
        const uint64_t tGrow = nowNs();
        struct GrowTimer
        {
            uint64_t &sink;
            uint64_t start;
            uint64_t &count;
            ~GrowTimer()
            {
                sink += nowNs() - start;
                ++count;
            }
        } growTimer{costGrowNs, tGrow, growths};
        I.lastGuard = G;
        I.lastTight = T;
        I.tight = Unite(I.tight, T);
        if (I.dynamic == 0 && !I.indexed && G.area() > 65536)
            I.dynamic = 2;
        const Rect grown = Unite(I.guard, G);
        const bool guardGrew = !(grown == I.guard);
        I.guard = grown;
        if (I.dynamic != 0 || G.empty())
            return;
        if (guardGrew)
            captureTiles(I, I.guard, d);
        // tiles this draw reaches for the first time: who wrote their blocks
        for (int ty = G.y0 / static_cast<int>(kTile); ty <= G.y1 / static_cast<int>(kTile); ++ty)
            for (int tx = G.x0 / static_cast<int>(kTile); tx <= G.x1 / static_cast<int>(kTile); ++tx)
            {
                const size_t bit = static_cast<size_t>(ty) * kTilesPerSide + static_cast<size_t>(tx);
                if (I.reached.test(bit))
                    continue;
                I.reached.set(bit);
                const int x0 = std::max(tx * static_cast<int>(kTile), 0), y0 = std::max(ty * static_cast<int>(kTile), 0);
                const int x1 = std::min(tx * static_cast<int>(kTile) + static_cast<int>(kTile) - 1, static_cast<int>(I.w) - 1);
                const int y1 = std::min(ty * static_cast<int>(kTile) + static_cast<int>(kTile) - 1, static_cast<int>(I.h) - 1);
                tracker->CollectWriters(I.psm, I.tbp, I.tbw, x0, y0, x1, y1, I.writers, I.cells, I.cellsKnown);
            }
    }

    // ---------------------------------------------------------------- keys
    // Raw values of the rectangle, row by row: one byte per texel (indices) or four (RGBA).
    static void gather(const Instance &I, const Rect &r, std::vector<uint8_t> &buf, std::bitset<256> &used)
    {
        const size_t bpp = I.indexed ? 1u : 4u;
        buf.assign(static_cast<size_t>(r.area()) * bpp, 0);
        size_t o = 0;
        for (int y = r.y0; y <= r.y1; ++y)
            for (int tx = r.x0 / static_cast<int>(kTile); tx <= r.x1 / static_cast<int>(kTile); ++tx)
            {
                const int xs = std::max(r.x0, tx * static_cast<int>(kTile)), xe = std::min(r.x1, tx * static_cast<int>(kTile) + static_cast<int>(kTile) - 1);
                const auto it = I.tiles.find(static_cast<uint32_t>(y / static_cast<int>(kTile)) * kTilesPerSide + static_cast<uint32_t>(tx));
                const size_t n = static_cast<size_t>(xe - xs + 1) * bpp;
                if (it != I.tiles.end())
                    std::memcpy(&buf[o], &it->second.data[(static_cast<size_t>(y % kTile) * kTile + static_cast<size_t>(xs % kTile)) * bpp], n);
                o += n;
            }
        if (I.indexed)
            for (uint8_t b : buf)
                used.set(b);
    }

    static uint64_t formatSeed(const Instance &I, int rw, int rh)
    {
        return combine(combine(I.psm, static_cast<uint64_t>(rw)), static_cast<uint64_t>(rh));
    }

    // The key with the colouring: the resolved CLUT entries the indices use.
    static uint64_t withColours(const Instance &I, uint64_t indexKey, const std::bitset<256> &used)
    {
        if (!I.indexed)
            return indexKey;
        std::vector<uint32_t> entries;
        for (uint32_t i = 0; i < 256u; ++i)
            if (used.test(i))
            {
                entries.push_back(i);
                entries.push_back(I.palette[i]);
            }
        return hash64(entries.data(), entries.size() * 4u, indexKey);
    }

    static uint64_t withColoursAll(const Instance &I, uint64_t indexKey)
    {
        return I.indexed ? hash64(I.palette, I.paletteCount * 4u, indexKey) : indexKey;
    }

    void computeKeys(const Instance &I, KeySet &k, uint32_t &usedEntries, std::bitset<256> &usedGuard)
    {
        k.k1 = I.k1;
        std::vector<uint8_t> buf;
        std::bitset<256> used;
        if (!I.guard.empty())
        {
            gather(I, I.guard, buf, used);
            k.k2b_i = hash64(buf.data(), buf.size(), formatSeed(I, I.guard.x1 - I.guard.x0 + 1, I.guard.y1 - I.guard.y0 + 1));
            k.k2b_c = withColours(I, k.k2b_i, used);
            usedGuard = used;
        }
        if (!I.tight.empty() && I.guard.contains(I.tight))
        {
            used.reset();
            gather(I, I.tight, buf, used);
            k.k2t_i = hash64(buf.data(), buf.size(), formatSeed(I, I.tight.x1 - I.tight.x0 + 1, I.tight.y1 - I.tight.y0 + 1));
            k.k2t_c = withColours(I, k.k2t_i, used);
        }
        // the 16 x 16 tiles some draw reaches, and the indices used inside them
        {
            used.reset();
            uint64_t h = combine(combine(I.psm, 0x7469ull), I.indexed ? 1u : 0u);
            for (size_t bit = 0; bit < I.reached.size(); ++bit)
            {
                if (!I.reached.test(bit))
                    continue;
                const auto it = I.tiles.find(static_cast<uint32_t>(bit));
                if (it == I.tiles.end())
                    continue;
                h = hash64(it->second.data.data(), it->second.data.size(), combine(h, bit));
                if (I.indexed)
                    markUsed(I, it->second, static_cast<uint32_t>(bit), used);
            }
            k.k2s_i = h;
            k.k2s_c = withColours(I, h, used);
            usedEntries = static_cast<uint32_t>(used.count());
            usedGuard = used;
        }
        // upload-based keys
        if (!I.writers.empty())
        {
            std::vector<WriterInfo> w = I.writers;
            std::sort(w.begin(), w.end(), [](const WriterInfo &a, const WriterInfo &b) { return a.entry < b.entry; });
            const uint64_t seed = combine(combine(I.psm, 0x3333ull), I.tbw);
            std::vector<uint64_t> words;
            for (const WriterInfo &x : w)
            {
                words.push_back(x.entry.payload);
                words.push_back((static_cast<uint64_t>(static_cast<uint32_t>(x.entry.rel)) << 32) | (static_cast<uint64_t>(x.entry.dsax) << 16) | x.entry.dsay);
            }
            k.k3_i = hash64(words.data(), words.size() * 8u, seed);
            k.k3_c = withColours(I, k.k3_i, usedGuard);
            const WriterInfo *main = &w[0];
            for (const WriterInfo &x : w)
                if (x.cells > main->cells)
                    main = &x;
            const uint64_t single[2] = {main->entry.payload,
                                        (static_cast<uint64_t>(static_cast<uint32_t>(main->entry.rel)) << 32) | (static_cast<uint64_t>(main->entry.dsax) << 16) | main->entry.dsay};
            k.k3m_i = hash64(single, sizeof(single), seed);
            k.k3m_c = withColours(I, k.k3m_i, usedGuard);
        }
    }

    // ---------------------------------------------------------------- rows
    static void markUsed(const Instance &I, const Tile &tile, uint32_t tileKey, std::bitset<256> &used)
    {
        const uint32_t tx = tileKey % kTilesPerSide, ty = tileKey / kTilesPerSide;
        for (uint32_t v = 0; v < kTile; ++v)
            for (uint32_t u = 0; u < kTile; ++u)
                if (tx * kTile + u < I.w && ty * kTile + v < I.h)
                    used.set(tile.data[v * kTile + u]);
    }

    static const WriterInfo *mainWriter(const Instance &I)
    {
        const WriterInfo *main = nullptr;
        for (const WriterInfo &x : I.writers)
            if (!main || x.cells > main->cells || (x.cells == main->cells && x.entry < main->entry))
                main = &x;
        return main;
    }

    std::string formatRow(uint64_t id, const Instance &I, const KeySet &k, uint32_t usedEntries) const
    {
        std::string s;
        appendf(s, "%llu,%s,%llu,%llu,%016llx,%u,%u,%u,%u,%u,%d,%u,%u,%u,%u,%016llx,%u,", static_cast<unsigned long long>(id), scene.c_str(),
                static_cast<unsigned long long>(I.firstMarker), static_cast<unsigned long long>(I.lastMarker), static_cast<unsigned long long>(I.k1), I.tbp,
                I.tbw, I.psm, I.w, I.h, I.indexed ? 1 : 0, I.cpsm, I.csm, I.csa, I.cbp, static_cast<unsigned long long>(I.clutHash), I.texa);
        appendf(s, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,", static_cast<unsigned long long>(I.draws), static_cast<unsigned long long>(I.sprites),
                static_cast<unsigned long long>(I.tris), static_cast<unsigned long long>(I.bilinear), static_cast<unsigned long long>(I.nearest),
                static_cast<unsigned long long>(I.fst), static_cast<unsigned long long>(I.stq), I.wmsMask, I.wmtMask);
        if (I.region)
            appendf(s, "%d:%d:%d:%d,", I.regionMin[0], I.regionMax[0], I.regionMin[1], I.regionMax[1]);
        else
            s += "-,";
        appendf(s, "%d,%d,%d,%d,%d,%d,%d,%d,%lld,%lld,%u,", I.guard.x0, I.guard.y0, I.guard.x1, I.guard.y1, I.tight.x0, I.tight.y0, I.tight.x1, I.tight.y1,
                static_cast<long long>(I.guard.area()), static_cast<long long>(I.tight.area()), static_cast<uint32_t>(I.reached.count()));
        appendf(s, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,", I.magU[0] > 1.0e29 ? 0.0 : I.magU[0], I.magU[1], I.magV[0] > 1.0e29 ? 0.0 : I.magV[0], I.magV[1],
                I.triDensity[0] > 1.0e29 ? 0.0 : I.triDensity[0], I.triDensity[1], I.dynamic);
        for (uint64_t key : {k.k2b_i, k.k2b_c, k.k2t_i, k.k2t_c, k.k2s_i, k.k2s_c, k.k3_i, k.k3_c, k.k3m_i, k.k3m_c})
            appendf(s, "%016llx,", static_cast<unsigned long long>(key));
        const WriterInfo *main = mainWriter(I);
        appendf(s, "%u,%u,%u,", static_cast<uint32_t>(I.writers.size()), I.cells, I.cellsKnown);
        if (main)
            appendf(s, "%016llx,%u,%u,%u,%u,%u,%d,%u,", static_cast<unsigned long long>(main->entry.payload), main->w, main->h, main->dpsm, main->dbw, main->dbp,
                    main->entry.rel, main->cells);
        else
            s += "0,0,0,0,0,0,0,0,";
        appendf(s, "%016llx,%u,%u,%u", static_cast<unsigned long long>(I.clutWriters.empty() ? 0ull : I.clutWriters[0].payload),
                static_cast<uint32_t>(I.clutWriters.size()), usedEntries, I.paletteCount);
        // the live key (task t1b): the key the renderer gave this texture when it was created
        if (I.hasLive)
            appendf(s, ",%d,%016llx,%d,%d,%d,%d,%u,%d", static_cast<int>(I.live.status), static_cast<unsigned long long>(I.live.k3i), I.live.rect.x0, I.live.rect.y0,
                    I.live.rect.width(), I.live.rect.height(), I.live.writers, I.live.exact ? 1 : 0);
        else
            s += ",-1,0,0,0,0,0,0,0";
        return s;
    }

    void writeSidecars(uint64_t id, const Instance &I, const std::bitset<256> &usedGuard)
    {
        (void)usedGuard;
        if (tilesCsv && !I.tight.empty() && I.dynamic == 0)
        {
            // The tiles some draw reaches, hashed over the part of the tile inside the tight reach only: the tile around the edge of
            // an image also holds whatever GS memory held beyond it, which says nothing about the texture. `cov` is that part,
            // x0:y0:x1:y1 inside the tile; two rows can only be compared on a tile where their `cov` is the same.
            std::string s;
            for (int ty = I.tight.y0 / static_cast<int>(kTile); ty <= I.tight.y1 / static_cast<int>(kTile); ++ty)
                for (int tx = I.tight.x0 / static_cast<int>(kTile); tx <= I.tight.x1 / static_cast<int>(kTile); ++tx)
                {
                    const uint32_t key = static_cast<uint32_t>(ty) * kTilesPerSide + static_cast<uint32_t>(tx);
                    if (!I.reached.test(key))
                        continue;
                    const auto it = I.tiles.find(key);
                    if (it == I.tiles.end())
                        continue;
                    const Tile &t = it->second;
                    const int tileX = tx * static_cast<int>(kTile), tileY = ty * static_cast<int>(kTile);
                    const int cx0 = std::max(I.tight.x0, tileX) - tileX, cx1 = std::min(I.tight.x1, tileX + static_cast<int>(kTile) - 1) - tileX;
                    const int cy0 = std::max(I.tight.y0, tileY) - tileY, cy1 = std::min(I.tight.y1, tileY + static_cast<int>(kTile) - 1) - tileY;
                    std::vector<uint32_t> covered; // the texels as colours (for indexed textures: the indices go to `indices`)
                    std::vector<uint8_t> indices;
                    std::bitset<256> seenIndex;
                    std::unordered_set<uint32_t> seenColour;
                    for (int y = cy0; y <= cy1; ++y)
                        for (int x = cx0; x <= cx1; ++x)
                        {
                            if (I.indexed)
                            {
                                const uint8_t v = t.data[static_cast<size_t>(y) * kTile + static_cast<size_t>(x)];
                                indices.push_back(v);
                                covered.push_back(I.palette[v]);
                                seenIndex.set(v);
                            }
                            else
                            {
                                uint32_t px;
                                std::memcpy(&px, &t.data[(static_cast<size_t>(y) * kTile + static_cast<size_t>(x)) * 4u], 4);
                                covered.push_back(px);
                                if (seenColour.size() < 255u)
                                    seenColour.insert(px);
                            }
                        }
                    const uint64_t rgbaHash = hash64(covered.data(), covered.size() * 4u, 0);
                    const uint64_t rawHash = I.indexed ? hash64(indices.data(), indices.size(), 0) : rgbaHash;
                    const uint32_t distinct = I.indexed ? static_cast<uint32_t>(seenIndex.count()) : static_cast<uint32_t>(seenColour.size());
                    appendf(s, "%llu,%d,%d,%016llx,%016llx,%u,%d:%d:%d:%d\n", static_cast<unsigned long long>(id), tx, ty, static_cast<unsigned long long>(rawHash),
                            static_cast<unsigned long long>(rgbaHash), distinct, cx0, cy0, cx1, cy1);
                }
            std::fwrite(s.data(), 1, s.size(), tilesCsv);
        }
        if (palCsv && I.indexed && !I.tight.empty() && I.dynamic == 0)
        {
            // how many texels of the tight reach use each palette entry
            std::vector<uint64_t> hist(256, 0);
            for (int y = I.tight.y0; y <= I.tight.y1; ++y)
                for (int x = I.tight.x0; x <= I.tight.x1; ++x)
                {
                    const auto it = I.tiles.find(static_cast<uint32_t>(y / static_cast<int>(kTile)) * kTilesPerSide + static_cast<uint32_t>(x / static_cast<int>(kTile)));
                    if (it != I.tiles.end())
                        ++hist[it->second.data[static_cast<size_t>(y % kTile) * kTile + static_cast<size_t>(x % kTile)]];
                }
            std::string s;
            appendf(s, "%llu,%016llx,%u,", static_cast<unsigned long long>(id), static_cast<unsigned long long>(I.clutHash), I.paletteCount);
            for (uint32_t i = 0; i < I.paletteCount; ++i)
                appendf(s, "%s%08x", i ? " " : "", I.palette[i]);
            s += ",";
            for (uint32_t i = 0; i < I.paletteCount; ++i)
                appendf(s, "%s%llu", i ? " " : "", static_cast<unsigned long long>(hist[i]));
            s += "\n";
            std::fwrite(s.data(), 1, s.size(), palCsv);
        }
    }

    // ---------------------------------------------------------------- scenes
    void writeUploads()
    {
        std::FILE *f = std::fopen((censusPath + ".uploads.csv").c_str(), "w");
        if (!f)
            return;
        std::fprintf(f, "payload,uploads,bytes,dpsm,dbw,rrw,rrh,distinct_dbp,first_dbp,first_marker,last_marker,first_scene\n");
        for (const auto &e : uploads)
        {
            const UploadAgg &a = e.second;
            std::fprintf(f, "%016llx,%llu,%llu,%u,%u,%u,%u,%zu,%u,%llu,%llu,%s\n", static_cast<unsigned long long>(e.first), static_cast<unsigned long long>(a.count),
                         static_cast<unsigned long long>(a.bytes), a.dpsm, a.dbw, a.w, a.h, a.dbps.size(), a.firstDbp, static_cast<unsigned long long>(a.firstMarker),
                         static_cast<unsigned long long>(a.lastMarker), a.firstScene.c_str());
        }
        std::fclose(f);
    }

    void closeScene(bool final = false)
    {
        const uint64_t tClose = nowNs();
        std::vector<Instance *> list;
        list.reserve(instances.size());
        for (auto &e : instances)
            list.push_back(e.second.get());
        std::sort(list.begin(), list.end(), [](const Instance *a, const Instance *b) { return a->firstMarker != b->firstMarker ? a->firstMarker < b->firstMarker : a->k1 < b->k1; });
        for (Instance *p : list)
        {
            const Instance &I = *p;
            KeySet keys;
            keys.k1 = I.k1;
            uint32_t usedEntries = 0;
            std::bitset<256> usedGuard;
            if (I.dynamic == 0)
                computeKeys(I, keys, usedEntries, usedGuard);
            const uint64_t id = rowCounter++;
            const std::string row = formatRow(id, I, keys, usedEntries);
            if (csv)
                std::fprintf(csv, "%s\n", row.c_str());
            writeSidecars(id, I, usedGuard);
        }
        instances.clear();
        slotCounts.clear();
        ++scenesClosed;
        for (std::FILE *f : {csv, tilesCsv, palCsv})
            if (f)
                std::fflush(f);
        if (final)
            writeUploads();
        if (verify)
            std::fprintf(stderr,
                         "[tex:census] verify: %llu chunks, %llu blocks predicted, %llu blocks changed, %llu changed blocks outside the prediction (%.4f%%)\n",
                         static_cast<unsigned long long>(verifyChunks), static_cast<unsigned long long>(verifyPredicted),
                         static_cast<unsigned long long>(verifyChangedBlocks), static_cast<unsigned long long>(verifyViolations),
                         verifyChangedBlocks ? 100.0 * static_cast<double>(verifyViolations) / static_cast<double>(verifyChangedBlocks) : 0.0);
        costCloseNs += nowNs() - tClose;
        std::fprintf(stderr, "[tex:census] scene %s closed: %zu rows so far, %zu upload payloads, census cost %.1f ms in %llu draw calls; closing this scene took %.1f ms\n",
                     scene.c_str(), static_cast<size_t>(rowCounter), uploads.size(), static_cast<double>(costNs) / 1.0e6, static_cast<unsigned long long>(drawCalls),
                     static_cast<double>(nowNs() - tClose) / 1.0e6);
    }

    void markDone()
    {
        if (std::FILE *f = std::fopen((censusPath + ".done").c_str(), "wb"))
            std::fclose(f);
    }

    void pollScene(uint64_t marker)
    {
        (void)marker;
        std::FILE *f = std::fopen(scenePath.c_str(), "rb");
        if (!f)
            return;
        char line[64] = {};
        const size_t n = std::fread(line, 1, sizeof(line) - 1, f);
        std::fclose(f);
        std::string name(line, n);
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' '))
            name.pop_back();
        for (char &c : name)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
                c = '_';
        if (name.empty() || name == scene)
            return;
        closeScene();
        scene = name;
        if (scene == "end")
        {
            closeScene(true);
            markDone();
        }
    }
};
// ------------------------------------------------------------------------------------------------
Census::Census() : m(std::make_unique<Impl>()) {}

Census::~Census()
{
    Finish();
}

std::unique_ptr<Census> Census::FromEnvironment(TexKeys::Tracker &tracker)
{
    const char *censusEnv = std::getenv("PS2X_TEX_CENSUS");
    if (!censusEnv || censusEnv[0] == '\0')
        return nullptr;
    std::unique_ptr<Census> c(new Census());
    Impl &m = *c->m;
    m.tracker = &tracker;
    m.censusPath = censusEnv;
    m.scenePath = m.censusPath + ".scene";
    const char *stats = std::getenv("PS2X_TEX_CENSUS_STATS");
    m.statsOn = stats && stats[0] == '1';
    const char *verify = std::getenv("PS2X_TEX_CENSUS_VERIFY");
    m.verify = verify && verify[0] == '1';
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m.censusPath).parent_path(), ec);
    std::filesystem::remove(m.censusPath + ".done", ec);
    m.csv = std::fopen(m.censusPath.c_str(), "w");
    m.tilesCsv = std::fopen((m.censusPath + ".tiles.csv").c_str(), "w");
    m.palCsv = std::fopen((m.censusPath + ".palettes.csv").c_str(), "w");
    if (!m.csv)
    {
        std::fprintf(stderr, "[tex:census] cannot write %s\n", m.censusPath.c_str());
        return nullptr;
    }
    std::fprintf(m.csv, "%s\n", Impl::header());
    if (m.tilesCsv)
        std::fprintf(m.tilesCsv, "row,tx,ty,raw_hash,rgba_hash,distinct,cov\n");
    if (m.palCsv)
        std::fprintf(m.palCsv, "row,clut_hash,n,palette,hist\n");
    Impl *impl = &m;
    tracker.SetCloseCallback([impl](const TexKeys::TransferRec &r) { impl->transferClosed(r); });
    std::fprintf(stderr, "[tex:census] on: census %s, scene file %s%s\n", m.censusPath.c_str(), m.scenePath.c_str(), m.verify ? ", verifying the block map" : "");
    return c;
}

void Census::BeginTransfer(const GSTransferCommand &command)
{
    if (command.direction != 0u)
        return;
    const uint64_t t0 = nowNs();
    m->beginTransfer(command);
    m->costBeginNs += nowNs() - t0;
    m->costNs += nowNs() - t0;
}

void Census::BeforeUpload(const uint8_t *vram)
{
    Impl &s = *m;
    if (!s.verify || !vram)
        return;
    const TexKeys::TransferRec *r = s.tracker->CurrentTransfer();
    if (!r)
        return;
    s.beforeSpan = GSGpu::BufferPageSpan(r->dpsm, r->dbp, r->dbw, r->x, r->y, r->w, r->h);
    s.beforePages.resize(static_cast<size_t>(s.beforeSpan.count) * 8192u);
    for (uint32_t i = 0; i < s.beforeSpan.count; ++i)
        std::memcpy(&s.beforePages[static_cast<size_t>(i) * 8192u], vram + static_cast<size_t>((s.beforeSpan.first + i) & 511u) * 8192u, 8192u);
}

void Census::AfterUpload(const uint8_t *vram)
{
    Impl &s = *m;
    const uint64_t t0 = nowNs();
    ++s.uploadCalls;
    if (s.verify && vram && !s.beforePages.empty())
    {
        ++s.verifyChunks;
        for (uint32_t i = 0; i < s.beforeSpan.count; ++i)
        {
            const uint32_t page = (s.beforeSpan.first + i) & 511u;
            for (uint32_t b = 0; b < 32u; ++b)
            {
                const uint32_t block = page * 32u + b;
                if (std::memcmp(&s.beforePages[static_cast<size_t>(i) * 8192u + b * 256u], vram + static_cast<size_t>(block) * 256u, 256u) != 0)
                {
                    if (!s.changedSeen[block])
                    {
                        s.changedSeen[block] = 1;
                        ++s.verifyChangedBlocks;
                        if (!s.predicted[block])
                            ++s.verifyViolations;
                    }
                }
            }
        }
        s.beforePages.clear();
    }
    s.costAfterNs += nowNs() - t0;
    s.costNs += nowNs() - t0;
}

void Census::Draw(const DrawInfo &draw)
{
    const uint64_t t0 = nowNs();
    m->curMarker = draw.marker;
    m->draw(draw);
    ++m->drawCalls;
    m->costNs += nowNs() - t0;
}

void Census::Marker(uint64_t marker)
{
    Impl &s = *m;
    s.curMarker = marker;
    const uint64_t t0 = nowNs();
    if ((marker & 7u) == 0u)
    {
        const uint64_t tPoll = nowNs();
        s.pollScene(marker);
        s.costPollNs += nowNs() - tPoll;
    }
    if (s.statsOn && marker - s.lastStatsMarker >= 600u)
    {
        s.lastStatsMarker = marker;
        std::fprintf(stderr, "[tex:census] marker %llu: %zu instances live, %llu rows, cost %.1f ms (%llu draws, %llu uploads)\n",
                     static_cast<unsigned long long>(marker), s.instances.size(), static_cast<unsigned long long>(s.rowCounter), static_cast<double>(s.costNs) / 1.0e6,
                     static_cast<unsigned long long>(s.drawCalls), static_cast<unsigned long long>(s.uploadCalls));
        std::fprintf(stderr,
                     "[tex:census]   cost split ms: transfers begin %.1f, upload data %.1f, new instances %.1f (%llu), growth %.1f (%llu growths, %llu tiles read), scene poll %.1f, scene closes %.1f\n",
                     static_cast<double>(s.costBeginNs) / 1.0e6, static_cast<double>(s.costAfterNs) / 1.0e6, static_cast<double>(s.costCreateNs) / 1.0e6,
                     static_cast<unsigned long long>(s.instancesCreated), static_cast<double>(s.costGrowNs) / 1.0e6, static_cast<unsigned long long>(s.growths),
                     static_cast<unsigned long long>(s.tilesCaptured), static_cast<double>(s.costPollNs) / 1.0e6, static_cast<double>(s.costCloseNs) / 1.0e6);
    }
    s.costNs += nowNs() - t0;
}

void Census::Finish()
{
    Impl &s = *m;
    if (s.finished)
        return;
    s.finished = true;
    s.closeScene(true);
    s.markDone();
    for (std::FILE *f : {s.csv, s.tilesCsv, s.palCsv})
        if (f)
            std::fflush(f);
}
} // namespace TexCensus
