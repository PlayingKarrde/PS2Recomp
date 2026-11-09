// The texture key tracker. See include/runtime/gs/gs_tex_keys.h for what it does and the rules for the keys.

#include "runtime/gs/gs_tex_keys.h"

#include "runtime/gs/gs_recording.h"
#include "runtime/gs/gs_texel.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace TexKeys
{
namespace
{
    const uint8_t kTab32[32] = {0, 1, 4, 5, 16, 17, 20, 21, 2, 3, 6, 7, 18, 19, 22, 23, 8, 9, 12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31};
    const uint8_t kTab16[32] = {0, 2, 8, 10, 1, 3, 9, 11, 4, 6, 12, 14, 5, 7, 13, 15, 16, 18, 24, 26, 17, 19, 25, 27, 20, 22, 28, 30, 21, 23, 29, 31};
    const uint8_t kTab16S[32] = {0, 2, 16, 18, 1, 3, 17, 19, 8, 10, 24, 26, 9, 11, 25, 27, 4, 6, 20, 22, 5, 7, 21, 23, 12, 14, 28, 30, 13, 15, 29, 31};

    inline uint64_t round64(uint64_t acc, uint64_t w)
    {
        acc ^= Rotl64(w * 0xC2B2AE3D27D4EB4Full, 31) * 0x9E3779B185EBCA87ull;
        return Rotl64(acc, 27) * 0x9E3779B185EBCA87ull + 0x85EBCA77C2B2AE63ull;
    }

    void stripe(uint64_t *lanes, const uint8_t *p)
    {
        for (int i = 0; i < 4; ++i)
        {
            uint64_t w;
            std::memcpy(&w, p + 8 * i, 8);
            lanes[i] = round64(lanes[i], w);
        }
    }

    constexpr uint32_t kTile = 16u;
}

// ------------------------------------------------------------------------------------------------
bool GeometryOf(uint32_t psm, Geom &g)
{
    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_T8H:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        g = {64u, 32u, 8u, 8u, 8u, kTab32};
        return true;
    case GS_PSM_CT16:
        g = {64u, 64u, 16u, 8u, 4u, kTab16};
        return true;
    case GS_PSM_CT16S:
        g = {64u, 64u, 16u, 8u, 4u, kTab16S};
        return true;
    case GS_PSM_T8:
        g = {128u, 64u, 16u, 16u, 8u, kTab32};
        return true;
    case GS_PSM_T4:
        g = {128u, 128u, 32u, 16u, 4u, kTab16};
        return true;
    default:
        return false;
    }
}

uint64_t Mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ull;
    x ^= x >> 33;
    return x;
}

uint64_t Combine(uint64_t h, uint64_t v)
{
    return Mix64(h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2)));
}

uint64_t HashBytes(const void *data, size_t size, uint64_t seed)
{
    return GSRec::Hash64(data, size, seed);
}

std::string KeyName(uint64_t key)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(key));
    return text;
}

Rect Unite(const Rect &a, const Rect &b)
{
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    return Rect{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

bool TextureDesc::indexed() const
{
    return GSTexel::isFourBitIndexedPsm(static_cast<uint8_t>(psm)) || GSTexel::isEightBitIndexedPsm(static_cast<uint8_t>(psm));
}

// ------------------------------------------------------------------------------------------------
void Stream::update(const uint8_t *p, size_t n)
{
    len += n;
    if (bufLen)
    {
        const size_t take = std::min<size_t>(n, 32u - bufLen);
        std::memcpy(buf + bufLen, p, take);
        bufLen += static_cast<uint32_t>(take);
        p += take;
        n -= take;
        if (bufLen < 32u)
            return;
        stripe(lane, buf);
        bufLen = 0;
    }
    while (n >= 32)
    {
        stripe(lane, p);
        p += 32;
        n -= 32;
    }
    if (n)
    {
        std::memcpy(buf, p, n);
        bufLen = static_cast<uint32_t>(n);
    }
}

uint64_t Stream::finish() const
{
    uint64_t l[4] = {lane[0], lane[1], lane[2], lane[3]};
    if (bufLen)
    {
        uint8_t last[32] = {};
        std::memcpy(last, buf, bufLen);
        stripe(l, last);
    }
    uint64_t h = len * 0x9E3779B185EBCA87ull;
    for (int i = 0; i < 4; ++i)
        h = Mix64(h ^ l[i]) + static_cast<uint64_t>(i);
    return Mix64(h ^ len);
}

uint64_t TransferRec::payload() const
{
    if (closed)
        return payloadCache;
    return Combine(Combine(stream.finish(), (static_cast<uint64_t>(dpsm) << 40) | (static_cast<uint64_t>(w) << 20) | h), 0x5041594Cull);
}

// ------------------------------------------------------------------------------------------------
Tracker::Tracker() : m_blockWriter(kBlocks, 0u) {}
Tracker::~Tracker() = default;

const TransferRec *Tracker::CurrentTransfer() const
{
    return m_cur < 0 ? nullptr : &m_recs[static_cast<size_t>(m_cur)];
}

void Tracker::releaseRecord(uint32_t index)
{
    TransferRec &r = m_recs[index];
    if (r.refs == 0 && r.inUse && static_cast<int>(index) != m_cur)
    {
        r.inUse = false;
        m_free.push_back(index);
    }
}

void Tracker::closeCurrent()
{
    if (m_cur < 0)
        return;
    const uint32_t index = static_cast<uint32_t>(m_cur);
    TransferRec &r = m_recs[index];
    r.payloadCache = r.payload();
    r.closed = true;
    m_cur = -1;
    if (m_onClose)
        m_onClose(r);
    releaseRecord(index);
}

void Tracker::BeginTransfer(const GSTransferCommand &c, uint64_t marker)
{
    const uint32_t bw = std::max<uint32_t>(c.bitbltbuf.dbw, 1u);
    auto forget = [&](uint32_t block)
    {
        const uint32_t old = m_blockWriter[block];
        if (old != 0u)
        {
            TransferRec &o = m_recs[old - 1u];
            if (o.refs > 0)
                --o.refs;
            m_blockWriter[block] = 0u;
            releaseRecord(old - 1u);
        }
    };
    if (c.direction == 2u)
    {
        // a copy inside GS memory: whatever it wrote is no longer what some upload wrote
        ForEachBlock(c.bitbltbuf.dpsm, c.bitbltbuf.dbp, bw, c.trxpos.dsax, c.trxpos.dsay, c.trxreg.rrw, c.trxreg.rrh,
                     [&](uint32_t block, int, int, int, int) { forget(block); });
        return;
    }
    if (c.direction != 0u)
        return;
    closeCurrent();
    uint32_t index;
    if (!m_free.empty())
    {
        index = m_free.back();
        m_free.pop_back();
    }
    else
    {
        index = static_cast<uint32_t>(m_recs.size());
        m_recs.emplace_back();
    }
    TransferRec &r = m_recs[index];
    r = TransferRec{};
    r.inUse = true;
    r.dbp = c.bitbltbuf.dbp;
    r.dbw = bw;
    r.dpsm = c.bitbltbuf.dpsm;
    r.x = c.trxpos.dsax;
    r.y = c.trxpos.dsay;
    r.w = c.trxreg.rrw;
    r.h = c.trxreg.rrh;
    r.marker = marker;
    r.seq = ++m_seq;
    m_cur = static_cast<int>(index);
    ForEachBlock(r.dpsm, r.dbp, r.dbw, static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.w), static_cast<int>(r.h),
                 [&](uint32_t block, int, int, int, int)
                 {
                     forget(block);
                     m_blockWriter[block] = index + 1u;
                     ++m_recs[index].refs;
                 });
}

void Tracker::UploadData(const uint8_t *data, uint32_t bytes)
{
    if (m_cur < 0)
        return;
    TransferRec &r = m_recs[static_cast<size_t>(m_cur)];
    r.stream.update(data, bytes);
    r.bytes += bytes;
}

void Tracker::WritePixel(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
{
    ForEachBlock(psm, base, std::max(bw, 1u), static_cast<int>(x), static_cast<int>(y), 1, 1,
                 [&](uint32_t block, int, int, int, int)
                 {
                     const uint32_t old = m_blockWriter[block];
                     if (old != 0u)
                     {
                         TransferRec &o = m_recs[old - 1u];
                         if (o.refs > 0)
                             --o.refs;
                         m_blockWriter[block] = 0u;
                         releaseRecord(old - 1u);
                     }
                 });
}

void Tracker::MemoryReset()
{
    closeCurrent();
    std::fill(m_blockWriter.begin(), m_blockWriter.end(), 0u);
    m_recs.clear();
    m_free.clear();
    m_cur = -1;
}

void Tracker::CollectWriters(uint32_t psm, uint32_t tbp, uint32_t tbw, int x0, int y0, int x1, int y1, std::vector<WriterInfo> &out, uint32_t &cells,
                             uint32_t &known) const
{
    ForEachBlock(psm, tbp, tbw, x0, y0, x1 - x0 + 1, y1 - y0 + 1,
                 [&](uint32_t block, int, int, int, int)
                 {
                     ++cells;
                     const uint32_t w = m_blockWriter[block];
                     if (w == 0u)
                         return;
                     ++known;
                     const TransferRec &r = recFor(w);
                     WriterEntry e;
                     e.payload = r.payload();
                     e.rel = static_cast<int32_t>(tbp) - static_cast<int32_t>(r.dbp);
                     e.dsax = static_cast<uint16_t>(r.x);
                     e.dsay = static_cast<uint16_t>(r.y);
                     for (WriterInfo &wi : out)
                         if (wi.entry == e)
                         {
                             ++wi.cells;
                             return;
                         }
                     WriterInfo wi;
                     wi.entry = e;
                     wi.cells = 1;
                     wi.dbw = r.dbw;
                     wi.dbp = r.dbp;
                     wi.dpsm = r.dpsm;
                     wi.w = r.w;
                     wi.h = r.h;
                     out.push_back(wi);
                 });
}

// ------------------------------------------------------------------------------------------------
LiveKey Tracker::KeyFor(const TextureDesc &t) const
{
    LiveKey key;
    Geom g;
    if (!GeometryOf(t.psm, g) || t.width == 0 || t.height == 0)
        return key;
    const uint32_t bw = std::max<uint32_t>(t.tbw, 1u);
    const uint32_t validW = std::min<uint32_t>(t.width, bw * 64u);
    key.validWidth = validW;
    const uint32_t anchorIndex = m_blockWriter[t.tbp & (kBlocks - 1u)];
    if (anchorIndex == 0u)
    {
        key.status = LiveKey::Status::NoWriter;
        return key;
    }
    const TransferRec &anchor = recFor(anchorIndex);
    key.anchorSeq = anchor.seq;
    key.anchorPayload = anchor.payload();

    // The writers: the anchor and the newer transfers that wrote other blocks of the valid area, each with the bounding box of
    // the blocks it wrote there.
    struct Acc
    {
        uint32_t record; // record index + 1
        Rect box;
        uint32_t cells;
    };
    std::vector<Acc> accs;
    bool tooMany = false;
    ForEachBlock(t.psm, t.tbp, bw, 0, 0, static_cast<int>(validW), static_cast<int>(t.height),
                 [&](uint32_t block, int bx0, int by0, int bx1, int by1)
                 {
                     if (tooMany)
                         return;
                     const uint32_t w = m_blockWriter[block];
                     if (w == 0u || recFor(w).seq < anchor.seq)
                         return;
                     ++key.cells;
                     const Rect cell{bx0, by0, std::min(bx1, static_cast<int>(validW) - 1), std::min(by1, static_cast<int>(t.height) - 1)};
                     for (Acc &a : accs)
                         if (a.record == w)
                         {
                             a.box = Unite(a.box, cell);
                             ++a.cells;
                             return;
                         }
                     if (accs.size() >= kMaxWriters)
                     {
                         tooMany = true;
                         return;
                     }
                     accs.push_back(Acc{w, cell, 1u});
                 });
    key.writers = static_cast<uint32_t>(accs.size());
    if (tooMany)
    {
        key.status = LiveKey::Status::Composed;
        return key;
    }

    // Pieces of one format uploaded at another buffer width are the pages of a canvas.
    for (const Acc &a : accs)
    {
        const TransferRec &r = recFor(a.record);
        if (r.dpsm == t.psm && r.dbw != bw)
        {
            key.status = LiveKey::Status::Composed;
            return key;
        }
    }

    // The replaced rectangle
    const Rect valid{0, 0, static_cast<int>(validW) - 1, static_cast<int>(t.height) - 1};
    Rect rect;
    std::vector<WriterEntry> entries;
    for (const Acc &a : accs)
    {
        const TransferRec &r = recFor(a.record);
        Rect footprint = a.box;
        const int32_t rel = static_cast<int32_t>(t.tbp) - static_cast<int32_t>(r.dbp);
        if (r.dpsm == t.psm && r.dbw == bw && rel >= 0 && rel % 32 == 0)
        {
            // The texture starts a whole number of pages into the transfer's buffer: its texel (u, v) is the buffer's pixel (px + u, py + v).
            const uint32_t pageIndex = static_cast<uint32_t>(rel) / 32u;
            const uint32_t perRow = std::max<uint32_t>((r.dbw * 64u) / g.pageW, 1u);
            const int px = static_cast<int>((pageIndex % perRow) * g.pageW), py = static_cast<int>((pageIndex / perRow) * g.pageH);
            footprint = Rect{static_cast<int>(r.x) - px, static_cast<int>(r.y) - py, static_cast<int>(r.x + r.w) - 1 - px, static_cast<int>(r.y + r.h) - 1 - py};
            footprint.x0 = std::max(footprint.x0, valid.x0);
            footprint.y0 = std::max(footprint.y0, valid.y0);
            footprint.x1 = std::min(footprint.x1, valid.x1);
            footprint.y1 = std::min(footprint.y1, valid.y1);
        }
        else
        {
            key.exact = false;
        }
        rect = Unite(rect, footprint);
        WriterEntry e;
        e.payload = r.payload();
        e.rel = rel;
        e.dsax = static_cast<uint16_t>(r.x);
        e.dsay = static_cast<uint16_t>(r.y);
        entries.push_back(e);
    }
    if (rect.empty())
    {
        key.status = LiveKey::Status::Empty;
        return key;
    }
    key.rect = rect;
    std::sort(entries.begin(), entries.end());
    std::vector<uint64_t> words;
    for (const WriterEntry &e : entries)
    {
        words.push_back(e.payload);
        words.push_back((static_cast<uint64_t>(static_cast<uint32_t>(e.rel)) << 32) | (static_cast<uint64_t>(e.dsax) << 16) | e.dsay);
    }
    words.push_back((static_cast<uint64_t>(static_cast<uint32_t>(rect.x0)) << 32) | static_cast<uint32_t>(rect.y0));
    const uint64_t seed = Combine(Combine(Combine(t.psm, 0x3333ull), t.tbw), (static_cast<uint64_t>(rect.width()) << 20) | static_cast<uint64_t>(rect.height()));
    key.k3i = HashBytes(words.data(), words.size() * 8u, seed);
    key.status = LiveKey::Status::Ok;
    return key;
}

// ------------------------------------------------------------------------------------------------
void ReadTexels(const uint8_t *vram, const TextureDesc &t, const GSTexaReg &texa, const Rect &rect, std::vector<uint8_t> &out)
{
    const bool indexed = t.indexed();
    const size_t bpp = indexed ? 1u : 4u;
    out.assign(static_cast<size_t>(rect.area()) * bpp, 0);
    if (rect.empty())
        return;
    Geom g;
    if (!GeometryOf(t.psm, g))
        return;
    const uint32_t bw = std::max<uint32_t>(t.tbw, 1u);
    GSMem::TexturePageCache cache;
    const size_t rw = static_cast<size_t>(rect.width());
    // One page of GS memory at a time (the page cache holds a single page).
    for (int cy = rect.y0; cy <= rect.y1;)
    {
        const int cyEnd = std::min(rect.y1, static_cast<int>((static_cast<uint32_t>(cy) / g.pageH + 1u) * g.pageH) - 1);
        for (int cx = rect.x0; cx <= rect.x1;)
        {
            const int cxEnd = std::min(rect.x1, static_cast<int>((static_cast<uint32_t>(cx) / g.pageW + 1u) * g.pageW) - 1);
            for (int v = cy; v <= cyEnd; ++v)
                for (int u = cx; u <= cxEnd; ++u)
                {
                    const uint32_t raw = GSMem::ReadTexture(cache, vram, t.psm, t.tbp, bw, static_cast<uint32_t>(u), static_cast<uint32_t>(v));
                    const size_t o = static_cast<size_t>(v - rect.y0) * rw + static_cast<size_t>(u - rect.x0);
                    if (indexed)
                    {
                        out[o] = static_cast<uint8_t>(GSTexel::isFourBitIndexedPsm(static_cast<uint8_t>(t.psm)) ? (raw & 0x0Fu) : (raw & 0xFFu));
                    }
                    else
                    {
                        uint32_t texel;
                        switch (t.psm)
                        {
                        case GS_PSM_CT32:
                        case GS_PSM_Z32:
                        case GS_PSM_CT24:
                        case GS_PSM_Z24:
                            texel = GSTexel::applyTexa(texa, static_cast<uint8_t>(t.psm), raw);
                            break;
                        case GS_PSM_CT16:
                        case GS_PSM_CT16S:
                        case GS_PSM_Z16:
                        case GS_PSM_Z16S:
                            texel = GSTexel::applyTexa(texa, static_cast<uint8_t>(t.psm), GSTexel::Rgba5551ToRgba8888(static_cast<uint16_t>(raw)));
                            break;
                        default:
                            texel = 0xFFFF00FFu;
                            break;
                        }
                        std::memcpy(&out[o * 4u], &texel, 4);
                    }
                }
            cx = cxEnd + 1;
        }
        cy = cyEnd + 1;
    }
}

std::bitset<256> UsedIndices(const std::vector<uint8_t> &indices)
{
    std::bitset<256> used;
    for (uint8_t b : indices)
        used.set(b);
    return used;
}

uint64_t ColouringKey(uint64_t k3i, const std::vector<uint8_t> &indices, const uint32_t *palette)
{
    const std::bitset<256> used = UsedIndices(indices);
    std::vector<uint32_t> entries;
    for (uint32_t i = 0; i < 256u; ++i)
        if (used.test(i))
        {
            entries.push_back(i);
            entries.push_back(palette[i]);
        }
    return HashBytes(entries.data(), entries.size() * 4u, k3i);
}

uint64_t FallbackKey(const uint8_t *vram, const TextureDesc &t, const GSTexaReg &texa, const Rect &reach, Rect &rectOut)
{
    rectOut = Rect{};
    if (reach.empty())
        return 0;
    const int w = static_cast<int>(t.width), h = static_cast<int>(t.height);
    const int tx0 = std::max(reach.x0, 0) / static_cast<int>(kTile), tx1 = std::min(reach.x1, w - 1) / static_cast<int>(kTile);
    const int ty0 = std::max(reach.y0, 0) / static_cast<int>(kTile), ty1 = std::min(reach.y1, h - 1) / static_cast<int>(kTile);
    const bool indexed = t.indexed();
    uint64_t hash = Combine(Combine(t.psm, 0x7469ull), indexed ? 1u : 0u);
    std::vector<uint8_t> tile;
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx)
        {
            const Rect r{tx * static_cast<int>(kTile), ty * static_cast<int>(kTile), std::min(tx * static_cast<int>(kTile) + static_cast<int>(kTile), w) - 1,
                         std::min(ty * static_cast<int>(kTile) + static_cast<int>(kTile), h) - 1};
            ReadTexels(vram, t, texa, r, tile);
            hash = HashBytes(tile.data(), tile.size(), Combine(hash, static_cast<uint64_t>(ty) * 64u + static_cast<uint64_t>(tx)));
            rectOut = Unite(rectOut, r);
        }
    return hash;
}
} // namespace TexKeys
