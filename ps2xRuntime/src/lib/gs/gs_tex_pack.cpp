// Texture packs: the index of replacement pictures, the decoded-picture cache and the decoding thread. See
// include/runtime/gs/gs_tex_pack.h.

#include "runtime/gs/gs_tex_pack.h"

#include "runtime/gs/gs_png.h"
#include "runtime/gs/gs_tex_dump.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace TexPack
{
namespace
{
    std::string lower(std::string text)
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
}

bool ParseFileName(const std::string &fileName, ParsedName &out)
{
    const std::string name = lower(fileName);
    bool index = false;
    std::string stem;
    if (name.size() > 8 && name.compare(name.size() - 8, 8, ".idx.png") == 0)
    {
        index = true;
        stem = name.substr(0, name.size() - 8);
    }
    else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0)
    {
        stem = name.substr(0, name.size() - 4);
    }
    else
    {
        return false;
    }
    if (stem.size() != 16)
        return false;
    uint64_t key = 0;
    for (char c : stem)
    {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
        key = (key << 4) | static_cast<uint64_t>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    out.key = key;
    out.index = index;
    return true;
}

int ScaleOf(uint32_t imgW, uint32_t imgH, int rectW, int rectH, std::string *reason)
{
    auto fail = [&](const std::string &text)
    {
        if (reason)
            *reason = text;
        return 0;
    };
    if (rectW <= 0 || rectH <= 0)
        return fail("the texture has no replaced area");
    const std::string want = std::to_string(rectW) + "x" + std::to_string(rectH);
    const std::string have = std::to_string(imgW) + "x" + std::to_string(imgH);
    if (imgW % static_cast<uint32_t>(rectW) != 0 || imgH % static_cast<uint32_t>(rectH) != 0)
        return fail("is " + have + ", but the texture's replaced area is " + want + " and a picture must be that size times a whole number from 1 to " +
                    std::to_string(kMaxScale));
    const uint32_t sx = imgW / static_cast<uint32_t>(rectW), sy = imgH / static_cast<uint32_t>(rectH);
    if (sx != sy)
        return fail("is " + have + ", which is " + std::to_string(sx) + " times the width but " + std::to_string(sy) +
                    " times the height of the replaced area (" + want + "); the scale must be the same in both directions");
    if (sx < 1 || sx > static_cast<uint32_t>(kMaxScale))
        return fail("is " + have + ", " + std::to_string(sx) + " times the replaced area (" + want + "); the scale must be a whole number from 1 to " +
                    std::to_string(kMaxScale));
    return static_cast<int>(sx);
}

// ------------------------------------------------------------------------------------------------
namespace
{
    namespace fs = std::filesystem;

    class DirectorySource final : public FileSource
    {
    public:
        explicit DirectorySource(std::string root) : m_root(std::move(root)) {}

        std::vector<Entry> list(const std::string &prefix) override
        {
            std::vector<Entry> out;
            std::error_code ec;
            const fs::path dir = fs::path(m_root) / prefix;
            for (const auto &e : fs::directory_iterator(dir, ec))
            {
                if (!e.is_regular_file(ec))
                    continue;
                Entry entry;
                entry.path = prefix + e.path().filename().string();
                entry.provider = "folder";
                entry.order = 0;
                out.push_back(std::move(entry));
            }
            return out;
        }

        bool read(const std::string &path, std::vector<uint8_t> &out, std::string &error) override
        {
            std::ifstream in(fs::path(m_root) / path, std::ios::binary);
            if (!in)
            {
                error = "cannot open " + path;
                return false;
            }
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        }

        void problem(const std::string &provider, const std::string &message) override
        {
            (void)provider;
            (void)message;
        }

    private:
        std::string m_root;
    };
}

std::shared_ptr<FileSource> MakeDirectorySource(const std::string &root)
{
    return std::make_shared<DirectorySource>(root);
}

// ------------------------------------------------------------------------------------------------
// the dev pack

namespace
{
    std::atomic<bool> gAuthoring{false};

    fs::path pathFromUtf8(const std::string &text)
    {
        return fs::path(std::u8string(text.begin(), text.end()));
    }

    std::string utf8Name(const fs::path &path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    constexpr uint32_t kDevOrder = 0xFFFFFFFEu;
    constexpr const char *kFontPrefixDev = "assets/font/";

    // The sub-folder of the dev pack that holds the files of a listing prefix, or "" for a prefix the dev pack does not serve.
    const char *devSubfolder(const std::string &prefix)
    {
        if (prefix == kTexturePrefix)
            return "textures";
        if (prefix == kFontPrefixDev)
            return "font";
        return "";
    }

    // A change signature of a folder: file names, sizes and times combined without regard to the order the system lists them in.
    uint64_t folderSignature(const fs::path &root)
    {
        uint64_t sum = 0, count = 0;
        for (const char *sub : {"textures", "font"})
        {
            std::error_code ec;
            for (const auto &e : fs::directory_iterator(root / sub, ec))
            {
                std::error_code fec;
                if (!e.is_regular_file(fec))
                    continue;
                const std::string name = utf8Name(e.path().filename());
                uint64_t h = 1469598103934665603ull;
                for (char c : name)
                    h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
                h ^= static_cast<uint64_t>(e.file_size(fec)) * 0x9E3779B97F4A7C15ull;
                h ^= static_cast<uint64_t>(e.last_write_time(fec).time_since_epoch().count()) * 0xC2B2AE3D27D4EB4Full;
                h ^= static_cast<uint64_t>(sub[0]) << 56;
                sum += h * (h | 1ull);
                ++count;
            }
        }
        return sum ^ (count * 0x165667B19E3779F9ull);
    }
}

struct DevPackSource::Impl
{
    std::string folderText;
    fs::path folder;
    std::shared_ptr<FileSource> base;
    int pollMillis = 400;
    std::atomic<bool> changedFlag{false};
    bool stop = false;
    std::mutex m;
    std::condition_variable cv;
    std::thread worker;
    mutable std::mutex notesMutex;
    std::vector<std::string> notes;

    void loop()
    {
        uint64_t reported = folderSignature(folder);
        uint64_t observed = reported;
        std::unique_lock<std::mutex> lock(m);
        while (!stop)
        {
            cv.wait_for(lock, std::chrono::milliseconds(pollMillis), [&] { return stop; });
            if (stop)
                break;
            lock.unlock();
            const uint64_t sig = folderSignature(folder);
            lock.lock();
            if (sig != reported && sig == observed)
            {
                reported = sig;
                changedFlag.store(true, std::memory_order_release);
            }
            observed = sig;
        }
    }
};

DevPackSource::DevPackSource(std::string folder, std::shared_ptr<FileSource> base, int pollMillis, bool watch) : m(new Impl)
{
    m->folderText = std::move(folder);
    m->folder = pathFromUtf8(m->folderText);
    m->base = std::move(base);
    m->pollMillis = std::max(20, pollMillis);
    std::error_code ec;
    fs::create_directories(m->folder / "textures", ec);
    fs::create_directories(m->folder / "font", ec);
    if (watch)
        m->worker = std::thread([this] { m->loop(); });
}

DevPackSource::~DevPackSource()
{
    {
        std::lock_guard<std::mutex> lock(m->m);
        m->stop = true;
    }
    m->cv.notify_all();
    if (m->worker.joinable())
        m->worker.join();
}

std::vector<FileSource::Entry> DevPackSource::list(const std::string &prefix)
{
    std::vector<Entry> out = m->base ? m->base->list(prefix) : std::vector<Entry>();
    const char *sub = devSubfolder(prefix);
    if (sub[0] == '\0')
        return out;
    std::vector<Entry> mine;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(m->folder / sub, ec))
    {
        std::error_code fec;
        if (!e.is_regular_file(fec))
            continue;
        Entry entry;
        entry.path = prefix + utf8Name(e.path().filename());
        entry.provider = kProvider;
        entry.order = kDevOrder;
        mine.push_back(std::move(entry));
    }
    if (mine.empty())
        return out;
    out.erase(std::remove_if(out.begin(), out.end(), [&](const Entry &o)
    {
        for (const Entry &d : mine)
            if (d.path == o.path)
                return true;
        return false;
    }), out.end());
    out.insert(out.end(), mine.begin(), mine.end());
    return out;
}

bool DevPackSource::read(const std::string &path, std::vector<uint8_t> &out, std::string &error)
{
    for (const char *prefix : {kTexturePrefix, kFontPrefixDev})
    {
        const std::string p = prefix;
        if (path.compare(0, p.size(), p) == 0 && path.find('/', p.size()) == std::string::npos)
        {
            const fs::path file = m->folder / devSubfolder(p) / pathFromUtf8(path.substr(p.size()));
            std::error_code ec;
            if (fs::is_regular_file(file, ec))
            {
                std::ifstream in(file, std::ios::binary);
                if (!in)
                {
                    error = "cannot open " + path.substr(p.size());
                    return false;
                }
                out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                return true;
            }
        }
    }
    if (m->base)
        return m->base->read(path, out, error);
    error = "no such file";
    return false;
}

void DevPackSource::problem(const std::string &provider, const std::string &message)
{
    if (provider == kProvider)
    {
        std::lock_guard<std::mutex> lock(m->notesMutex);
        if (std::find(m->notes.begin(), m->notes.end(), message) == m->notes.end() && m->notes.size() < 24)
            m->notes.push_back(message);
        return;
    }
    if (m->base)
        m->base->problem(provider, message);
}

bool DevPackSource::changed()
{
    return m->changedFlag.exchange(false, std::memory_order_acq_rel);
}

const std::string &DevPackSource::folder() const
{
    return m->folderText;
}

uint32_t DevPackSource::fileCount() const
{
    uint32_t n = 0;
    for (const char *sub : {"textures", "font"})
    {
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(m->folder / sub, ec))
        {
            std::error_code fec;
            if (e.is_regular_file(fec))
                ++n;
        }
    }
    return n;
}

std::vector<std::string> DevPackSource::notes() const
{
    std::lock_guard<std::mutex> lock(m->notesMutex);
    return m->notes;
}

void DevPackSource::clearNotes()
{
    std::lock_guard<std::mutex> lock(m->notesMutex);
    m->notes.clear();
}

namespace
{
    std::mutex gDevPackMutex;
    std::shared_ptr<DevPackSource> gDevPackSlot;
}

void SetDevPack(std::shared_ptr<DevPackSource> pack)
{
    std::lock_guard<std::mutex> lock(gDevPackMutex);
    gDevPackSlot = std::move(pack);
}

std::shared_ptr<DevPackSource> DevPack()
{
    std::lock_guard<std::mutex> lock(gDevPackMutex);
    return gDevPackSlot;
}

void SetAuthoring(bool on)
{
    gAuthoring.store(on, std::memory_order_relaxed);
}

bool Authoring()
{
    return gAuthoring.load(std::memory_order_relaxed);
}

namespace
{
    std::mutex &defaultSourceMutex()
    {
        static std::mutex m;
        return m;
    }
    std::shared_ptr<FileSource> &defaultSourceSlot()
    {
        static std::shared_ptr<FileSource> slot;
        return slot;
    }
}

void SetDefaultSource(std::shared_ptr<FileSource> source)
{
    std::lock_guard<std::mutex> lock(defaultSourceMutex());
    defaultSourceSlot() = std::move(source);
}

std::shared_ptr<FileSource> DefaultSource()
{
    if (const char *dir = std::getenv("PS2X_TEX_PACK_DIR"))
        if (dir[0] != '\0')
            return MakeDirectorySource(dir);
    std::lock_guard<std::mutex> lock(defaultSourceMutex());
    return defaultSourceSlot();
}

// ------------------------------------------------------------------------------------------------
struct Store::Impl
{
    std::shared_ptr<FileSource> source;
    Options opt;
    mutable std::mutex mutex;
    mutable std::condition_variable cv, idleCv;

    struct File
    {
        std::string path, provider;
        uint32_t order = 0;
    };
    struct KeyFiles
    {
        bool hasRgba = false, hasIdx = false;
        File rgba, idx;
    };
    std::unordered_map<uint64_t, KeyFiles> index;
    uint32_t files = 0, ignored = 0, decoded = 0, refusedCount = 0;
    uint64_t readNs = 0, decodeNs = 0;
    uint64_t generation = 1;
    std::atomic<uint64_t> generationShared{1};

    struct CacheItem
    {
        std::shared_ptr<const Image> image;
        bool failed = false;
        bool queued = false;
        uint64_t lastUsed = 0;
        std::string provider;
    };
    std::unordered_map<std::string, CacheItem> cache; // by path
    std::deque<std::string> demand, prefetchQueue;
    uint64_t cacheBytes = 0, useClock = 0;
    std::set<std::string> reported; // refusal messages already sent on (path + text)

    std::thread worker;
    bool stop = false, busy = false;

    explicit Impl(std::shared_ptr<FileSource> src, const Options &o) : source(std::move(src)), opt(o) {}

    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        cv.notify_all();
        if (worker.joinable())
            worker.join();
    }

    // ---- the index
    uint32_t buildIndex()
    {
        std::vector<FileSource::Entry> list = source->list(kTexturePrefix);
        std::lock_guard<std::mutex> lock(mutex);
        index.clear();
        cache.clear();
        demand.clear();
        prefetchQueue.clear();
        reported.clear();
        cacheBytes = 0;
        files = ignored = decoded = refusedCount = 0;
        ++generation;
        generationShared.store(generation, std::memory_order_release);
        std::sort(list.begin(), list.end(), [](const FileSource::Entry &a, const FileSource::Entry &b) { return a.path < b.path; });
        for (const FileSource::Entry &e : list)
        {
            ParsedName parsed;
            if (!ParseFileName(baseName(e.path), parsed))
            {
                ++ignored;
                continue;
            }
            KeyFiles &k = index[parsed.key];
            File f{e.path, e.provider, e.order};
            // The same key and kind from two mods can not happen (one entry per path), but 0123.PNG and 0123.png can.
            if (parsed.index)
            {
                if (!k.hasIdx || f.order >= k.idx.order)
                    k.idx = f;
                k.hasIdx = true;
            }
            else
            {
                if (!k.hasRgba || f.order >= k.rgba.order)
                    k.rgba = f;
                k.hasRgba = true;
            }
            ++files;
            if (opt.prefetch)
                prefetchQueue.push_back(e.path);
        }
        cv.notify_all();
        return files;
    }

    void ensureWorker()
    {
        if (!worker.joinable())
            worker = std::thread([this] { run(); });
    }

    // ---- refusals (mutex held)
    void refuse(const std::string &path, const std::string &provider, const std::string &text)
    {
        const std::string message = baseName(path) + " " + text;
        if (reported.insert(path + "|" + text).second)
        {
            ++refusedCount;
            std::fprintf(stderr, "[tex:pack] refused %s (%s): %s\n", baseName(path).c_str(), provider.c_str(), text.c_str());
            source->problem(provider, message);
        }
    }

    // ---- decoding (worker thread)
    void decodeOne(const std::string &path, bool demanded)
    {
        ParsedName parsed;
        const Kind kind = (ParseFileName(baseName(path), parsed) && parsed.index) ? Kind::Index : Kind::Rgba;
        std::string provider;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = cache.find(path);
            if (it != cache.end() && (it->second.image || it->second.failed))
            {
                it->second.queued = false;
                return;
            }
            const KeyFiles *k = nullptr;
            auto idx = index.find(parsed.key);
            if (idx != index.end())
                k = &idx->second;
            if (k)
                provider = (kind == Kind::Index ? k->idx : k->rgba).provider;
            if (!demanded && cacheBytes >= opt.cacheBytes / 2u)
                return; // prefetch stops at half the cache
            if (it == cache.end())
                it = cache.emplace(path, CacheItem{}).first;
            it->second.queued = true;
            it->second.provider = provider;
        }
        std::vector<uint8_t> bytes;
        std::string error;
        auto image = std::make_shared<Image>();
        image->kind = kind;
        image->path = path;
        image->provider = provider;
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = source->read(path, bytes, error);
        const auto t1 = std::chrono::steady_clock::now();
        if (!ok)
        {
            error = "could not be read (" + error + ")";
        }
        else
        {
            GSPng::Image png;
            if (kind == Kind::Rgba)
                ok = GSPng::DecodeRgba(bytes.data(), bytes.size(), png, error);
            else
                ok = GSPng::DecodeIndex(bytes.data(), bytes.size(), png, error);
            if (ok)
            {
                image->width = png.width;
                image->height = png.height;
                image->pixels = std::move(png.pixels);
                if (kind == Kind::Rgba)
                {
                    // PNG alpha 0..255 to the GS's 0..0x80 (0x80 = opaque)
                    for (size_t i = 3; i < image->pixels.size(); i += 4)
                        image->pixels[i] = TexDump::FromPngAlpha(image->pixels[i]);
                }
                else
                {
                    uint8_t top = 0;
                    for (uint8_t v : image->pixels)
                        top = std::max(top, v);
                    image->maxIndex = top;
                }
            }
        }
        const auto t2 = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex);
        readNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        decodeNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
        auto it = cache.find(path);
        if (it == cache.end())
            return; // the index was rebuilt meanwhile
        it->second.queued = false;
        if (!ok)
        {
            it->second.failed = true;
            refuse(path, provider, error);
            return;
        }
        cacheBytes += image->pixels.size();
        it->second.image = std::move(image);
        it->second.lastUsed = demanded ? ++useClock : 0;
        ++decoded;
        evictLocked();
    }

    void evictLocked()
    {
        if (cacheBytes <= opt.cacheBytes)
            return;
        std::vector<std::pair<uint64_t, std::string>> order;
        for (const auto &e : cache)
            if (e.second.image)
                order.emplace_back(e.second.lastUsed, e.first);
        std::sort(order.begin(), order.end());
        const uint64_t target = opt.cacheBytes / 10u * 9u;
        for (const auto &o : order)
        {
            if (cacheBytes <= target)
                break;
            CacheItem &item = cache[o.second];
            cacheBytes -= item.image->pixels.size();
            item.image.reset();
        }
    }

    void run()
    {
        for (;;)
        {
            std::string path;
            bool demanded = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                busy = false;
                idleCv.notify_all();
                cv.wait(lock, [this] { return stop || !demand.empty() || (!prefetchQueue.empty() && cacheBytes < opt.cacheBytes / 2u); });
                if (stop)
                    return;
                if (!demand.empty())
                {
                    path = std::move(demand.front());
                    demand.pop_front();
                    demanded = true;
                }
                else
                {
                    path = std::move(prefetchQueue.front());
                    prefetchQueue.pop_front();
                }
                busy = true;
            }
            decodeOne(path, demanded);
        }
    }

    // ---- lookups (mutex held)
    // One file against the texture it would replace.
    void resolveFile(const File &file, Kind kind, int rectW, int rectH, bool indexed, uint32_t paletteSize, Lookup &out)
    {
        CacheItem &item = cache[file.path];
        if (item.failed)
        {
            out.state = Lookup::State::Refused;
            return;
        }
        if (!item.image)
        {
            if (!item.queued)
            {
                item.queued = true;
                item.provider = file.provider;
                demand.push_back(file.path);
                cv.notify_all();
            }
            out.state = Lookup::State::Pending;
            return;
        }
        const Image &img = *item.image;
        std::string reason;
        const int scale = ScaleOf(img.width, img.height, rectW, rectH, &reason);
        if (scale == 0)
        {
            refuse(file.path, file.provider, reason);
            out.state = Lookup::State::Refused;
            return;
        }
        if (kind == Kind::Index)
        {
            if (!indexed)
            {
                refuse(file.path, file.provider, "is an index image, but this texture has no palette; use a plain " + baseName(file.path).substr(0, 16) + ".png");
                out.state = Lookup::State::Refused;
                return;
            }
            if (img.maxIndex >= paletteSize)
            {
                refuse(file.path, file.provider,
                       "uses palette entry " + std::to_string(img.maxIndex) + ", but this texture's palette has only " + std::to_string(paletteSize) + " entries");
                out.state = Lookup::State::Refused;
                return;
            }
        }
        item.lastUsed = ++useClock;
        out.state = Lookup::State::Ready;
        out.image = item.image;
        out.scale = scale;
    }
};

Store::Store(std::shared_ptr<FileSource> source, const Options &options) : m(std::make_unique<Impl>(std::move(source), options))
{
    m->buildIndex();
    m->ensureWorker();
}

Store::~Store() = default;

uint32_t Store::reload()
{
    const uint32_t n = m->buildIndex();
    m->ensureWorker();
    return n;
}

bool Store::empty() const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    return m->index.empty();
}

uint64_t Store::generation() const
{
    return m->generationShared.load(std::memory_order_acquire);
}

Stats Store::stats() const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    Stats s;
    s.files = m->files;
    s.ignored = m->ignored;
    s.decoded = m->decoded;
    s.refused = m->refusedCount;
    s.pending = static_cast<uint32_t>(m->demand.size());
    s.cacheBytes = m->cacheBytes;
    s.generation = m->generation;
    s.readNanos = m->readNs;
    s.decodeNanos = m->decodeNs;
    return s;
}

Lookup Store::find(uint64_t k3i, uint64_t k3c, int rectW, int rectH, bool indexed, uint32_t paletteSize)
{
    std::lock_guard<std::mutex> lock(m->mutex);
    Lookup result;
    // 1. the colouring override: an RGBA picture named by k3_c, from any mod
    if (k3c != 0 && indexed)
    {
        const auto it = m->index.find(k3c);
        if (it != m->index.end() && it->second.hasRgba)
        {
            Lookup r;
            m->resolveFile(it->second.rgba, Kind::Rgba, rectW, rectH, indexed, paletteSize, r);
            if (r.state == Lookup::State::Ready || r.state == Lookup::State::Pending)
            {
                r.colouring = true;
                return r;
            }
            result = r; // refused: fall back to the k3_i files
        }
    }
    // 2. k3_i: the later mod wins; inside one mod an index image beats a plain one
    const auto it = m->index.find(k3i);
    if (k3i == 0 || it == m->index.end())
        return result;
    const Impl::KeyFiles &k = it->second;
    const Impl::File *first = nullptr, *second = nullptr;
    Kind firstKind = Kind::Rgba, secondKind = Kind::Rgba;
    if (k.hasIdx && k.hasRgba)
    {
        const bool idxFirst = k.idx.order >= k.rgba.order;
        first = idxFirst ? &k.idx : &k.rgba;
        firstKind = idxFirst ? Kind::Index : Kind::Rgba;
        second = idxFirst ? &k.rgba : &k.idx;
        secondKind = idxFirst ? Kind::Rgba : Kind::Index;
    }
    else if (k.hasIdx)
    {
        first = &k.idx;
        firstKind = Kind::Index;
    }
    else if (k.hasRgba)
    {
        first = &k.rgba;
    }
    if (!first)
        return result;
    Lookup r;
    m->resolveFile(*first, firstKind, rectW, rectH, indexed, paletteSize, r);
    if (r.state == Lookup::State::Refused && second)
    {
        Lookup r2;
        m->resolveFile(*second, secondKind, rectW, rectH, indexed, paletteSize, r2);
        if (r2.state != Lookup::State::Refused)
            return r2;
    }
    return r;
}

void Store::waitIdle() const
{
    std::unique_lock<std::mutex> lock(m->mutex);
    m->idleCv.wait(lock, [this] { return !m->busy && m->demand.empty() && (m->prefetchQueue.empty() || m->cacheBytes >= m->opt.cacheBytes / 2u); });
}
} // namespace TexPack
