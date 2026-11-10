// HD text: glyph codes and names, the registry of glyph bitmaps and the HD canvas. See include/runtime/gs/gs_font.h.

#include "runtime/gs/gs_font.h"

#include "runtime/gs/gs_png.h"
#include "runtime/gs/gs_tex_keys.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace FontHD
{
namespace
{
#include "gs_font_sjis.inc"

    // The ranges of two-byte codes the game's font has slots for (the table at 0x325DC0 of SLUS_217.02): lead byte range and the number of
    // slots before it. Every lead byte has 188 trail bytes (0x40-0x7E and 0x80-0xFC).
    struct Range
    {
        uint32_t lo, hi;
    };
    constexpr Range kRanges[3] = {{0x8100u, 0x9FFFu}, {0xE000u, 0xEAFFu}, {0xF000u, 0xF9FFu}};
    constexpr uint32_t kAsciiSlots = 96;
    constexpr uint32_t kTrailSlots = 188;

    uint32_t rangeCount(const Range &r)
    {
        return ((r.hi >> 8) - (r.lo >> 8) + 1u) * kTrailSlots;
    }

    std::string lowerCase(std::string text)
    {
        for (char &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    std::string baseName(const std::string &path)
    {
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    std::atomic<bool> gCapture{false};
}

int SlotOfCode(uint32_t code)
{
    if (code >= 0x20u && code < 0x80u)
        return static_cast<int>(code - 0x20u);
    uint32_t base = kAsciiSlots;
    for (const Range &r : kRanges)
    {
        if (code < r.lo || code > r.hi)
        {
            base += rangeCount(r);
            continue;
        }
        const uint32_t trail = code & 0xFFu;
        if (trail < 0x40u || trail == 0x7Fu || trail >= 0xFDu)
            return -1;
        const uint32_t leadIndex = (code >> 8) - (r.lo >> 8);
        return static_cast<int>(base + leadIndex * kTrailSlots + (trail - 0x40u) - (trail >= 0x80u ? 1u : 0u));
    }
    return -1;
}

uint32_t CodeOfSlot(uint32_t slot)
{
    if (slot < kAsciiSlots)
        return slot + 0x20u;
    uint32_t rest = slot - kAsciiSlots;
    for (const Range &r : kRanges)
    {
        const uint32_t count = rangeCount(r);
        if (rest >= count)
        {
            rest -= count;
            continue;
        }
        const uint32_t lead = (r.lo >> 8) + rest / kTrailSlots;
        const uint32_t t = rest % kTrailSlots;
        return (lead << 8) | (0x40u + t + (t >= 63u ? 1u : 0u));
    }
    return 0;
}

uint32_t UnicodeOfCode(uint32_t code)
{
    const int slot = SlotOfCode(code);
    return slot < 0 ? 0u : kSlotUnicode[slot];
}

std::vector<uint32_t> CodesOfUnicode(uint32_t codePoint)
{
    static const std::unordered_map<uint32_t, std::vector<uint32_t>> reverse = []
    {
        std::unordered_map<uint32_t, std::vector<uint32_t>> map;
        for (uint32_t slot = 0; slot < kSlotCount; ++slot)
            if (kSlotUnicode[slot] != 0u)
                map[kSlotUnicode[slot]].push_back(CodeOfSlot(slot));
        return map;
    }();
    const auto it = reverse.find(codePoint);
    return it == reverse.end() ? std::vector<uint32_t>() : it->second;
}

bool ParseFileName(const std::string &fileName, FileName &out)
{
    std::string name = lowerCase(fileName);
    if (name.size() < 5 || name.compare(name.size() - 4, 4, ".png") != 0)
        return false;
    name.resize(name.size() - 4);
    bool bold = false;
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".bold") == 0)
    {
        bold = true;
        name.resize(name.size() - 5);
    }
    if (name.size() < 2 || name.size() > 7 || (name[0] != 'c' && name[0] != 'u'))
        return false;
    uint32_t value = 0;
    for (size_t i = 1; i < name.size(); ++i)
    {
        const char c = name[i];
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
        value = (value << 4) | static_cast<uint32_t>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    out.value = value;
    out.unicode = name[0] == 'u';
    out.bold = bold;
    return true;
}

int ScaleOf(uint32_t imgW, uint32_t imgH, std::string *reason)
{
    if (imgW == imgH && imgW >= kCell && imgW % kCell == 0 && imgW / kCell <= static_cast<uint32_t>(kMaxScale))
        return static_cast<int>(imgW / kCell);
    if (reason)
        *reason = "is " + std::to_string(imgW) + "x" + std::to_string(imgH) + ", but a glyph picture must be 32 S by 32 S pixels with S a whole number from 1 to 8 (32, 64, 96 ... 256 pixels square)";
    return 0;
}

uint64_t BitmapHash(const uint8_t *packed512)
{
    return TexKeys::HashBytes(packed512, 512, 0x666F6E74ull); // "font"
}

// ---------------------------------------------------------------------------------------------------------------------
// the registry

struct Registry::Impl
{
    mutable std::mutex mutex;
    std::unordered_map<uint64_t, uint32_t> byHash;
};

Registry::Registry() : m(new Impl) {}
Registry::~Registry() = default;

void Registry::Note(uint32_t code, bool bold, const uint8_t *packed512)
{
    bool blank = true;
    for (int i = 0; i < 512; ++i)
        if (packed512[i])
        {
            blank = false;
            break;
        }
    if (blank)
        return;
    const uint64_t hash = BitmapHash(packed512);
    std::lock_guard<std::mutex> lock(m->mutex);
    const bool added = m->byHash.emplace(hash, PackId(code, bold)).second; // the first glyph reported keeps a bitmap
    // PS2X_FONT_REGISTRY=<file>: every glyph bitmap the game fetches, once, as "code outline hex" (the 512 bytes in hex), so a tool can
    // check its own decoding of fpr.fnt (tools/fontpack.py) against what the game really holds.
    static std::FILE *file = []() -> std::FILE *
    {
        const char *path = std::getenv("PS2X_FONT_REGISTRY");
        return path && path[0] != '\0' ? std::fopen(path, "w") : nullptr;
    }();
    if (added && file)
    {
        std::fprintf(file, "%04x %d ", code, bold ? 1 : 0);
        for (int i = 0; i < 512; ++i)
            std::fprintf(file, "%02x", packed512[i]);
        std::fprintf(file, "\n");
        std::fflush(file);
    }
}

bool Registry::Lookup(uint64_t bitmapHash, uint32_t &id) const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    const auto it = m->byHash.find(bitmapHash);
    if (it == m->byHash.end())
        return false;
    id = it->second;
    return true;
}

size_t Registry::Size() const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    return m->byHash.size();
}

void Registry::Clear()
{
    std::lock_guard<std::mutex> lock(m->mutex);
    m->byHash.clear();
}

std::vector<std::string> Registry::Lines() const
{
    std::vector<std::string> lines;
    std::lock_guard<std::mutex> lock(m->mutex);
    for (const auto &e : m->byHash)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%04x %u %016llx", IdCode(e.second), IdBold(e.second) ? 1u : 0u, static_cast<unsigned long long>(e.first));
        lines.emplace_back(buf);
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

Registry &Glyphs()
{
    static Registry registry;
    return registry;
}

bool CaptureWanted()
{
    return gCapture.load(std::memory_order_relaxed);
}

void SetCaptureWanted(bool wanted)
{
    gCapture.store(wanted, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------------
// picture helpers

void CoverageOfRgba(const uint8_t *rgba, uint32_t pixels, std::vector<uint8_t> &out)
{
    out.resize(pixels);
    bool transparent = false;
    for (uint32_t i = 0; i < pixels; ++i)
        if (rgba[i * 4 + 3] != 255)
        {
            transparent = true;
            break;
        }
    for (uint32_t i = 0; i < pixels; ++i)
    {
        const uint8_t *p = rgba + i * 4;
        out[i] = transparent ? p[3] : static_cast<uint8_t>((p[0] * 299u + p[1] * 587u + p[2] * 114u + 500u) / 1000u);
    }
}

void EnlargeOriginal(const uint8_t *indices, uint32_t stride, uint32_t scale, std::vector<uint8_t> &out)
{
    const uint32_t size = kCell * scale;
    out.resize(static_cast<size_t>(size) * size);
    // sample positions along one axis: the original pixel to the left and the weight of the right one
    std::vector<int> i0(size), i1(size);
    std::vector<float> w1(size);
    for (uint32_t x = 0; x < size; ++x)
    {
        const float f = (static_cast<float>(x) + 0.5f) / static_cast<float>(scale) - 0.5f;
        const float fl = std::floor(f);
        i0[x] = std::max(0, static_cast<int>(fl));
        i1[x] = std::min(static_cast<int>(kCell) - 1, static_cast<int>(fl) + 1);
        if (static_cast<int>(fl) < 0)
            i0[x] = 0;
        w1[x] = f - fl;
        if (f < 0.0f)
            w1[x] = 0.0f;
        if (f >= static_cast<float>(kCell) - 1.0f)
            w1[x] = 0.0f;
    }
    for (uint32_t y = 0; y < size; ++y)
    {
        const uint8_t *r0 = indices + static_cast<size_t>(i0[y]) * stride;
        const uint8_t *r1 = indices + static_cast<size_t>(i1[y]) * stride;
        const float wy = w1[y];
        uint8_t *dst = out.data() + static_cast<size_t>(y) * size;
        for (uint32_t x = 0; x < size; ++x)
        {
            const float wx = w1[x];
            const float a = r0[i0[x]] * (1.0f - wx) + r0[i1[x]] * wx;
            const float b = r1[i0[x]] * (1.0f - wx) + r1[i1[x]] * wx;
            const float v = (a * (1.0f - wy) + b * wy) * (255.0f / 15.0f);
            dst[x] = static_cast<uint8_t>(std::min(255.0f, v + 0.5f));
        }
    }
}

void ResizeSquare(const std::vector<uint8_t> &in, uint32_t from, uint32_t to, std::vector<uint8_t> &out)
{
    if (from == to)
    {
        out = in;
        return;
    }
    out.resize(static_cast<size_t>(to) * to);
    std::vector<int> i0(to), i1(to);
    std::vector<float> w1(to);
    const float ratio = static_cast<float>(from) / static_cast<float>(to);
    for (uint32_t x = 0; x < to; ++x)
    {
        const float f = (static_cast<float>(x) + 0.5f) * ratio - 0.5f;
        const float fl = std::floor(f);
        const int a = static_cast<int>(fl);
        i0[x] = std::min(std::max(a, 0), static_cast<int>(from) - 1);
        i1[x] = std::min(std::max(a + 1, 0), static_cast<int>(from) - 1);
        w1[x] = f - fl;
    }
    for (uint32_t y = 0; y < to; ++y)
    {
        const uint8_t *r0 = in.data() + static_cast<size_t>(i0[y]) * from;
        const uint8_t *r1 = in.data() + static_cast<size_t>(i1[y]) * from;
        for (uint32_t x = 0; x < to; ++x)
        {
            const float wx = w1[x];
            const float a = r0[i0[x]] * (1.0f - wx) + r0[i1[x]] * wx;
            const float b = r1[i0[x]] * (1.0f - wx) + r1[i1[x]] * wx;
            out[static_cast<size_t>(y) * to + x] = static_cast<uint8_t>(std::min(255.0f, a * (1.0f - w1[y]) + b * w1[y] + 0.5f));
        }
    }
}

namespace
{
    // The largest value in a window of 2 * radius + 1 along a line of `n` values (edges count as 0), by a monotonic queue: O(n).
    void slidingMax(const uint8_t *in, size_t inStride, uint8_t *out, size_t outStride, uint32_t n, uint32_t radius)
    {
        std::deque<uint32_t> q; // indices with decreasing values
        uint32_t next = 0;      // next index to push
        for (uint32_t x = 0; x < n; ++x)
        {
            const uint32_t last = std::min(n - 1u, x + radius);
            while (next <= last)
            {
                const uint8_t v = in[next * inStride];
                while (!q.empty() && in[q.back() * inStride] <= v)
                    q.pop_back();
                q.push_back(next);
                ++next;
            }
            const uint32_t first = x > radius ? x - radius : 0u;
            while (!q.empty() && q.front() < first)
                q.pop_front();
            out[x * outStride] = q.empty() ? 0 : in[q.front() * inStride];
        }
    }
}

void Dilate(const std::vector<uint8_t> &in, uint32_t size, uint32_t radius, std::vector<uint8_t> &out)
{
    std::vector<uint8_t> tmp(in.size());
    for (uint32_t y = 0; y < size; ++y)
        slidingMax(in.data() + static_cast<size_t>(y) * size, 1, tmp.data() + static_cast<size_t>(y) * size, 1, size, radius);
    out.resize(in.size());
    for (uint32_t x = 0; x < size; ++x)
        slidingMax(tmp.data() + x, size, out.data() + x, size, size, radius);
}

// ---------------------------------------------------------------------------------------------------------------------
// the atlas

namespace
{
    struct FileRef
    {
        std::string path, provider;
        uint32_t order = 0;
        // Within one mod: 2 = named by code (c8260.png), 1 = by its own character (uff21.png), 0 = folded from ASCII
        // (u0041.png also standing for the full-width A that names use). A higher rank wins; a later mod wins whatever the rank.
        uint8_t rank = 0;
        uint32_t scale = 0;    // from the picture's header; 0 until read
        bool refused = false;
    };
}

struct Atlas::Impl
{
    std::shared_ptr<TexPack::FileSource> source;
    mutable std::mutex mutex;
    std::unordered_map<uint32_t, FileRef> index; // by glyph id
    uint32_t scale = 1;
    std::atomic<uint64_t> generation{1};
    AtlasStats stats;
    std::unordered_set<std::string> reported;

    // decoded pictures, at the picture's own scale
    struct Picture
    {
        uint32_t scale = 1;
        std::vector<uint8_t> coverage;
    };
    std::unordered_map<uint32_t, std::shared_ptr<const Picture>> decoded;
    std::unordered_set<uint32_t> failed;
    uint64_t decodedBytes = 0; // the decoded pictures held; the prefetch stops at kPrefetchBytes (the rest is decoded when a glyph is first shown)
    static constexpr uint64_t kPrefetchBytes = 160ull << 20;
    // pictures at the atlas scale (resized, or the outline version grown from the plain one)
    std::unordered_map<uint32_t, std::shared_ptr<const std::vector<uint8_t>>> atScale;

    // the HD canvas, kept between versions: only the cells whose bitmap changed are redrawn
    std::vector<uint8_t> hd;
    struct Cell
    {
        uint64_t hash = 0; // 0: empty or unknown yet
        bool valid = false;
    };
    Cell cells[kCellsAcross * kCellsAcross];

    // the decoding thread
    std::thread worker;
    std::mutex wmutex;
    std::condition_variable wcond;
    std::deque<uint32_t> queue;
    bool stop = false;
    uint64_t workGeneration = 0;

    explicit Impl(std::shared_ptr<TexPack::FileSource> s) : source(std::move(s)) {}

    void note(const std::string &provider, const std::string &line)
    {
        if (!reported.insert(provider + "|" + line).second)
            return;
        std::fprintf(stderr, "[font:pack] %s\n", line.c_str());
        if (source)
            source->problem(provider, line);
    }

    // Indexes assets/font/ and reads each picture's size (the header of the PNG) to fix the atlas scale.
    uint32_t rebuild()
    {
        std::unordered_map<uint32_t, FileRef> fresh;
        uint32_t files = 0, refused = 0;
        uint32_t maxScale = 1;
        std::vector<TexPack::FileSource::Entry> entries = source ? source->list(kFontPrefix) : std::vector<TexPack::FileSource::Entry>();
        auto offer = [&](uint32_t id, const TexPack::FileSource::Entry &e, uint8_t rank)
        {
            auto it = fresh.find(id);
            if (it != fresh.end())
            {
                const FileRef &cur = it->second;
                if (cur.order > e.order || (cur.order == e.order && cur.rank >= rank))
                    return;
            }
            FileRef ref;
            ref.path = e.path;
            ref.provider = e.provider;
            ref.order = e.order;
            ref.rank = rank;
            fresh[id] = ref;
        };
        for (const auto &e : entries)
        {
            FileName name;
            if (!ParseFileName(baseName(e.path), name))
                continue;
            if (name.unicode)
            {
                for (uint32_t code : CodesOfUnicode(name.value))
                    offer(PackId(code, name.bold), e, 1);
                // ASCII ! to ~ also stands for its full-width form (U+FF01..U+FF5E), which wrestler names and most menus use,
                // unless a file names the full-width character or code itself.
                if (name.value >= 0x21u && name.value <= 0x7Eu)
                    for (uint32_t code : CodesOfUnicode(name.value + 0xFEE0u))
                        offer(PackId(code, name.bold), e, 0);
            }
            else
            {
                if (SlotOfCode(name.value) < 0)
                {
                    note(e.provider, baseName(e.path) + " is not a glyph code the game's font has (codes are 0x20-0x7F and the Shift-JIS two-byte codes)");
                    continue;
                }
                offer(PackId(name.value, name.bold), e, 2);
            }
        }
        // sizes: each distinct file once
        std::unordered_map<std::string, uint32_t> sizes;
        for (auto &kv : fresh)
        {
            FileRef &ref = kv.second;
            auto found = sizes.find(ref.path);
            if (found == sizes.end())
            {
                uint32_t sc = 0;
                std::vector<uint8_t> bytes;
                std::string error;
                if (source && source->read(ref.path, bytes, error))
                {
                    GSPng::Info info;
                    if (GSPng::ReadInfo(bytes.data(), bytes.size(), info, error))
                    {
                        std::string reason;
                        const int s = ScaleOf(info.width, info.height, &reason);
                        if (s > 0)
                            sc = static_cast<uint32_t>(s);
                        else
                            note(ref.provider, baseName(ref.path) + " " + reason);
                    }
                    else
                    {
                        note(ref.provider, baseName(ref.path) + ": " + error);
                    }
                }
                else
                {
                    note(ref.provider, baseName(ref.path) + ": " + (error.empty() ? "cannot be read" : error));
                }
                found = sizes.emplace(ref.path, sc).first;
            }
            ref.scale = found->second;
            if (ref.scale == 0)
            {
                ref.refused = true;
                ++refused;
            }
            else
            {
                ++files;
                maxScale = std::max(maxScale, ref.scale);
            }
        }
        // drop the refused ones: they play no part
        for (auto it = fresh.begin(); it != fresh.end();)
            it = it->second.refused ? fresh.erase(it) : std::next(it);
        std::lock_guard<std::mutex> lock(mutex);
        index = std::move(fresh);
        scale = maxScale;
        decoded.clear();
        failed.clear();
        atScale.clear();
        decodedBytes = 0;
        std::memset(cells, 0, sizeof(cells));
        hd.assign(static_cast<size_t>(kCanvasSize) * kCanvasSize * scale * scale, 0);
        stats.files = files;
        stats.refused = refused;
        stats.scale = scale;
        stats.decoded = 0;
        stats.bytesAtlas = hd.size();
        generation.fetch_add(1, std::memory_order_relaxed);
        return files;
    }

    // Decodes one glyph picture (any thread; the caller holds no lock).
    std::shared_ptr<const Picture> decodeFile(const FileRef &ref)
    {
        std::vector<uint8_t> bytes;
        std::string error;
        if (!source || !source->read(ref.path, bytes, error))
            return nullptr;
        GSPng::Image img;
        if (!GSPng::DecodeRgba(bytes.data(), bytes.size(), img, error))
        {
            note(ref.provider, baseName(ref.path) + ": " + error);
            return nullptr;
        }
        std::string reason;
        const int s = ScaleOf(img.width, img.height, &reason);
        if (s <= 0)
            return nullptr;
        auto pic = std::make_shared<Picture>();
        pic->scale = static_cast<uint32_t>(s);
        CoverageOfRgba(img.pixels.data(), img.width * img.height, pic->coverage);
        return pic;
    }

    std::shared_ptr<const Picture> picture(uint32_t id)
    {
        FileRef ref;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto d = decoded.find(id);
            if (d != decoded.end())
                return d->second;
            if (failed.count(id))
                return nullptr;
            const auto it = index.find(id);
            if (it == index.end())
                return nullptr;
            ref = it->second;
        }
        std::shared_ptr<const Picture> pic = decodeFile(ref);
        std::lock_guard<std::mutex> lock(mutex);
        if (index.find(id) == index.end() || index[id].path != ref.path)
            return pic; // the index changed meanwhile: do not keep it
        if (!pic)
        {
            failed.insert(id);
            return nullptr;
        }
        decoded[id] = pic;
        decodedBytes += pic->coverage.size();
        ++stats.decoded;
        return pic;
    }

    // The picture of a glyph at the atlas scale; null when the mods have none (and, for the outline version, none of the plain glyph either).
    std::shared_ptr<const std::vector<uint8_t>> glyphAtScale(uint32_t id)
    {
        uint32_t s;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = atScale.find(id);
            if (it != atScale.end())
                return it->second;
            s = scale;
        }
        std::shared_ptr<const std::vector<uint8_t>> result;
        if (std::shared_ptr<const Picture> pic = picture(id))
        {
            if (pic->scale == s)
            {
                result = std::shared_ptr<const std::vector<uint8_t>>(pic, &pic->coverage); // the same bytes, no second copy
            }
            else
            {
                auto v = std::make_shared<std::vector<uint8_t>>();
                ResizeSquare(pic->coverage, kCell * pic->scale, kCell * s, *v);
                result = v;
            }
        }
        else if (IdBold(id))
        {
            // no outline picture of its own: the plain glyph grown by three original pixels
            if (std::shared_ptr<const Picture> plain = picture(PackId(IdCode(id), false)))
            {
                std::vector<uint8_t> big;
                ResizeSquare(plain->coverage, kCell * plain->scale, kCell * s, big);
                auto v = std::make_shared<std::vector<uint8_t>>();
                Dilate(big, kCell * s, kBoldRadius * s, *v);
                result = v;
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (scale == s)
            atScale[id] = result;
        return result;
    }

    void workerLoop()
    {
        for (;;)
        {
            uint32_t id = 0;
            {
                std::unique_lock<std::mutex> lock(wmutex);
                wcond.wait(lock, [&] { return stop || !queue.empty(); });
                if (stop)
                    return;
                id = queue.front();
                queue.pop_front();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (decodedBytes >= kPrefetchBytes)
                {
                    std::lock_guard<std::mutex> wlock(wmutex);
                    queue.clear();
                    continue;
                }
            }
            glyphAtScale(id);
        }
    }

    void startPrefetch()
    {
        std::vector<uint32_t> ids;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ids.reserve(index.size());
            for (const auto &kv : index)
                if (!IdBold(kv.first))
                    ids.push_back(kv.first);
        }
        std::sort(ids.begin(), ids.end());
        std::lock_guard<std::mutex> lock(wmutex);
        queue.assign(ids.begin(), ids.end());
        wcond.notify_one();
    }
};

Atlas::Atlas(std::shared_ptr<TexPack::FileSource> source) : m(new Impl(std::move(source)))
{
    m->rebuild();
    m->worker = std::thread([this] { m->workerLoop(); });
    m->startPrefetch();
}

Atlas::~Atlas()
{
    {
        std::lock_guard<std::mutex> lock(m->wmutex);
        m->stop = true;
    }
    m->wcond.notify_all();
    if (m->worker.joinable())
        m->worker.join();
}

bool Atlas::empty() const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    return m->index.empty();
}

uint64_t Atlas::generation() const
{
    return m->generation.load(std::memory_order_relaxed);
}

uint32_t Atlas::scale() const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    return m->scale;
}

AtlasStats Atlas::stats() const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    return m->stats;
}

uint32_t Atlas::reload()
{
    {
        std::lock_guard<std::mutex> lock(m->wmutex);
        m->queue.clear();
    }
    const uint32_t n = m->rebuild();
    m->startPrefetch();
    return n;
}

bool Atlas::GlyphPicture(uint32_t code, bool bold, std::vector<uint8_t> &out)
{
    const auto p = m->glyphAtScale(PackId(code, bold));
    if (!p)
        return false;
    out = *p;
    return true;
}

bool Atlas::Compose(const uint8_t *indices, const std::vector<uint8_t> *&pixels)
{
    uint32_t s;
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        if (m->index.empty())
            return false;
        s = m->scale;
    }
    const uint32_t size = kCell * s;
    const size_t atlasStride = static_cast<size_t>(kCanvasSize) * s;
    uint32_t hdCells = 0, fallback = 0, unknown = 0;
    std::vector<uint8_t> original;
    for (uint32_t cy = 0; cy < kCellsAcross; ++cy)
    {
        for (uint32_t cx = 0; cx < kCellsAcross; ++cx)
        {
            const uint8_t *cell = indices + static_cast<size_t>(cy) * kCell * kCanvasSize + static_cast<size_t>(cx) * kCell;
            uint8_t packed[512];
            bool blank = true;
            for (uint32_t y = 0; y < kCell; ++y)
                for (uint32_t x = 0; x < kCell; x += 2)
                {
                    const uint8_t b = static_cast<uint8_t>((cell[y * kCanvasSize + x] & 15u) | ((cell[y * kCanvasSize + x + 1] & 15u) << 4));
                    packed[y * 16 + x / 2] = b;
                    blank = blank && b == 0;
                }
            Impl::Cell &state = m->cells[cy * kCellsAcross + cx];
            const uint64_t hash = blank ? 0u : BitmapHash(packed);
            uint8_t *dst = m->hd.data() + static_cast<size_t>(cy) * size * atlasStride + static_cast<size_t>(cx) * size;
            if (state.valid && state.hash == hash)
            {
                // unchanged since the last version; count what it is for the statistics only
                if (hash != 0u)
                {
                    uint32_t id;
                    if (Glyphs().Lookup(hash, id) && m->glyphAtScale(id))
                        ++hdCells;
                    else
                        ++fallback;
                }
                continue;
            }
            state.hash = hash;
            state.valid = true;
            if (blank)
            {
                for (uint32_t y = 0; y < size; ++y)
                    std::memset(dst + static_cast<size_t>(y) * atlasStride, 0, size);
                continue;
            }
            std::shared_ptr<const std::vector<uint8_t>> pic;
            uint32_t id = 0;
            if (Glyphs().Lookup(hash, id))
                pic = m->glyphAtScale(id);
            else
                ++unknown;
            const uint8_t *src;
            if (pic)
            {
                src = pic->data();
                ++hdCells;
            }
            else
            {
                EnlargeOriginal(cell, kCanvasSize, s, original);
                src = original.data();
                ++fallback;
            }
            for (uint32_t y = 0; y < size; ++y)
                std::memcpy(dst + static_cast<size_t>(y) * atlasStride, src + static_cast<size_t>(y) * size, size);
        }
    }
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        ++m->stats.composes;
        m->stats.lastHd = hdCells;
        m->stats.lastFallback = fallback;
        m->stats.lastUnknown = unknown;
    }
    pixels = &m->hd;
    return true;
}
}

// The game side's entry points (the override at 0x138210 in src/overrides/fpwr_overrides.cpp declares them itself).
namespace ps2x_gs
{
    bool fontCaptureWanted()
    {
        return FontHD::CaptureWanted();
    }

    void fontNoteGlyph(uint32_t code, uint32_t bold, const uint8_t *packed512)
    {
        FontHD::Glyphs().Note(code, bold != 0u, packed512);
    }
}
