#include "runtime/gs/gs_backend_probe.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

// See gs_backend_probe.h. Everything here is for measuring; the wrapped backend does the work.

namespace
{
    enum Kind : unsigned
    {
        kSubmit,
        kLoadClut,
        kBeginTransfer,
        kUploadImage,
        kFlush,
        kTextureFlush,
        kSync,
        kPresent,
        kClear,
        kConsume,
        kReadVram,
        kWriteVram,
        kSnapshot,
        kMark,
        kOther, // Initialize, Reset, GetTransferSnapshot
        kKindCount
    };

    const char *const kKindName[kKindCount] = {"Submit", "LoadClut", "BeginTransfer", "UploadImage", "Flush", "TextureFlush", "Sync",
                                               "Present", "Clear", "Consume", "ReadVram", "WriteVram", "Snapshot", "Mark", "Other"};

    uint64_t nowNs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // Which kinds the calling thread has already registered (bit per kind).
    thread_local uint32_t g_threadRegistered = 0;
    thread_local int g_isGameThread = -1; // -1 not decided yet; the game's thread is the one named "GameThread"

    // The name a thread gave itself (ThreadNaming::SetCurrentThreadName), or "thread N".
    std::string currentThreadLabel()
    {
        char text[96] = {};
#if defined(_WIN32)
        using GetDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
        static const auto getDescription = reinterpret_cast<GetDescriptionFn>(::GetProcAddress(::GetModuleHandleW(L"Kernel32.dll"), "GetThreadDescription"));
        if (getDescription)
        {
            PWSTR wide = nullptr;
            if (SUCCEEDED(getDescription(::GetCurrentThread(), &wide)) && wide)
            {
                std::wcstombs(text, wide, sizeof(text) - 1);
                ::LocalFree(wide);
            }
        }
#elif defined(__linux__)
        pthread_getname_np(pthread_self(), text, sizeof(text));
#endif
        if (text[0] != '\0')
            return text;
        char fallback[48];
        std::snprintf(fallback, sizeof(fallback), "thread %zu", std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000u);
        return fallback;
    }
}

struct GSBackendProbe::Impl
{
    std::unique_ptr<GSRasterBackend> inner;
    std::string label;
    uint64_t startNs = nowNs();
    std::atomic<uint64_t> lastReportNs{nowNs()};
    std::atomic<uint64_t> markers{0};

    struct Stat
    {
        std::atomic<uint64_t> calls{0}, ns{0}, maxNs{0};
    };
    std::array<Stat, kKindCount> stats;

    // PS2X_GS_MARKER_LOG=<file>: one CSV row per VBlank marker (what the game thread spent in each kind of call since
    // the previous marker, and how far apart the markers were). The cumulative counters below are never reset, so the
    // per-second report and the log do not disturb each other.
    std::array<std::atomic<uint64_t>, kKindCount> gameNs{};
    std::array<std::atomic<uint64_t>, kKindCount> gameCalls{};
    std::atomic<uint64_t> uploadBytes{0};
    std::FILE *markerLog = nullptr;
    uint64_t markerSeq = 0, markerPrevNs = 0;
    std::array<uint64_t, kKindCount> prevGameNs{}, prevGameCalls{};
    uint64_t prevUploadBytes = 0;
    unsigned stallMs = 0; // PS2X_TEST_GS_STALL_MS: sleep this long after every marker, to slow the game on purpose (a test of the measure)
    std::string stallFile; // PS2X_TEST_GS_STALL_FILE: ... or the number of ms is read from this file (polled every 10 markers)
    unsigned stallPoll = 0;

    std::mutex threadsMutex;
    std::array<std::string, kKindCount> threads; // names of the threads that called this kind, comma separated

    void registerThread(Kind kind)
    {
        if ((g_threadRegistered >> kind) & 1u)
            return;
        g_threadRegistered |= 1u << kind;
        const std::string name = currentThreadLabel();
        std::lock_guard<std::mutex> lock(threadsMutex);
        std::string &list = threads[kind];
        if (list.find(name) == std::string::npos)
            list += (list.empty() ? "" : ",") + name;
    }

    void add(Kind kind, uint64_t ns)
    {
        Stat &s = stats[kind];
        s.calls.fetch_add(1, std::memory_order_relaxed);
        s.ns.fetch_add(ns, std::memory_order_relaxed);
        uint64_t seen = s.maxNs.load(std::memory_order_relaxed);
        while (ns > seen && !s.maxNs.compare_exchange_weak(seen, ns, std::memory_order_relaxed))
        {
        }
        registerThread(kind);
        if (g_isGameThread < 0)
            g_isGameThread = currentThreadLabel().find("GameThread") != std::string::npos ? 1 : 0;
        if (g_isGameThread == 1)
        {
            gameNs[kind].fetch_add(ns, std::memory_order_relaxed);
            gameCalls[kind].fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t now = nowNs();
        uint64_t last = lastReportNs.load(std::memory_order_relaxed);
        if (now - last >= 1000000000ull && lastReportNs.compare_exchange_strong(last, now))
            report(now);
    }

    // Times the rest of the enclosing scope as one call of `kind`.
    struct Scope
    {
        Impl *impl;
        Kind kind;
        uint64_t start;
        Scope(Impl *i, Kind k) : impl(i), kind(k), start(nowNs()) {}
        ~Scope() { impl->add(kind, nowNs() - start); }
    };

    void logMarker(uint64_t startNs, uint64_t markNs)
    {
        uint64_t sum[kKindCount];
        uint64_t calls = 0, total = 0;
        for (unsigned k = 0; k < kKindCount; ++k)
        {
            const uint64_t ns = gameNs[k].load(std::memory_order_relaxed), n = gameCalls[k].load(std::memory_order_relaxed);
            sum[k] = ns - prevGameNs[k];
            if (k != kMark)
            {
                calls += n - prevGameCalls[k];
                total += sum[k];
            }
            prevGameNs[k] = ns;
            prevGameCalls[k] = n;
        }
        const uint64_t up = uploadBytes.load(std::memory_order_relaxed);
        const uint64_t other = total - sum[kSubmit] - sum[kBeginTransfer] - sum[kUploadImage] - sum[kLoadClut] - sum[kFlush] - sum[kSync];
        std::fprintf(markerLog, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                     static_cast<unsigned long long>(markerSeq), static_cast<unsigned long long>(startNs),
                     static_cast<unsigned long long>(markerSeq ? (startNs - markerPrevNs) / 1000u : 0u), static_cast<unsigned long long>(markNs / 1000u),
                     static_cast<unsigned long long>(total / 1000u), static_cast<unsigned long long>(sum[kSubmit] / 1000u),
                     static_cast<unsigned long long>(sum[kBeginTransfer] / 1000u), static_cast<unsigned long long>(sum[kUploadImage] / 1000u),
                     static_cast<unsigned long long>(sum[kLoadClut] / 1000u), static_cast<unsigned long long>(sum[kFlush] / 1000u),
                     static_cast<unsigned long long>(sum[kSync] / 1000u), static_cast<unsigned long long>(other / 1000u),
                     static_cast<unsigned long long>(calls), static_cast<unsigned long long>((up - prevUploadBytes) / 1024u));
        if ((markerSeq % 30u) == 0u)
            std::fflush(markerLog);
        prevUploadBytes = up;
        markerPrevNs = startNs;
        ++markerSeq;
    }

    void report(uint64_t now)
    {
        const uint64_t frames = markers.exchange(0);
        std::string line;
        char head[160];
        uint64_t totalCalls = 0, totalNs = 0;
        std::string parts;
        for (unsigned k = 0; k < kKindCount; ++k)
        {
            Stat &s = stats[k];
            const uint64_t calls = s.calls.exchange(0), ns = s.ns.exchange(0), maxNs = s.maxNs.exchange(0);
            if (calls == 0)
                continue;
            totalCalls += calls;
            totalNs += ns;
            std::string threadList;
            {
                std::lock_guard<std::mutex> lock(threadsMutex);
                threadList = threads[k];
            }
            char part[200];
            std::snprintf(part, sizeof(part), " | %s %llu %.3fms max %.3f [%s]", kKindName[k], static_cast<unsigned long long>(calls),
                          static_cast<double>(ns) / 1e6 / static_cast<double>(frames ? frames : 1), static_cast<double>(maxNs) / 1e6,
                          threadList.c_str());
            parts += part;
        }
        const double perFrame = frames ? 1.0 / static_cast<double>(frames) : 0.0;
        std::snprintf(head, sizeof(head), "[gs:stats] backend=%s t=%.1fs markers=%llu calls/frame=%.0f backend_ms/frame=%.3f", label.c_str(),
                      static_cast<double>(now - startNs) / 1e9, static_cast<unsigned long long>(frames),
                      static_cast<double>(totalCalls) * perFrame, static_cast<double>(totalNs) / 1e6 * perFrame);
        line = head + parts;
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }
};


GSBackendProbe::GSBackendProbe(std::unique_ptr<GSRasterBackend> inner, const char *label) : m_impl(std::make_unique<Impl>())
{
    m_impl->inner = std::move(inner);
    m_impl->label = label ? label : "?";
    if (const char *path = std::getenv("PS2X_GS_MARKER_LOG"))
        if (path[0] != '\0')
        {
            m_impl->markerLog = std::fopen(path, "w");
            if (m_impl->markerLog)
                std::fprintf(m_impl->markerLog, "marker,t_ns,gap_us,mark_us,game_us,submit_us,begin_us,upload_us,clut_us,flush_us,sync_us,other_us,calls,upload_kb\n");
        }
    if (const char *stall = std::getenv("PS2X_TEST_GS_STALL_MS"))
        m_impl->stallMs = static_cast<unsigned>(std::strtoul(stall, nullptr, 10));
    if (const char *stallFile = std::getenv("PS2X_TEST_GS_STALL_FILE"))
        m_impl->stallFile = stallFile;
}

GSBackendProbe::~GSBackendProbe()
{
    if (m_impl && m_impl->markerLog)
        std::fclose(m_impl->markerLog);
}

GSRasterBackend *GSBackendProbe::Inner() const
{
    return m_impl->inner.get();
}

void GSBackendProbe::TimedMark(const std::function<void()> &doMark)
{
    m_impl->markers.fetch_add(1, std::memory_order_relaxed);
    const uint64_t start = nowNs();
    {
        Impl::Scope t(m_impl.get(), kMark);
        doMark();
    }
    if (m_impl->markerLog)
        m_impl->logMarker(start, nowNs() - start);
    if (!m_impl->stallFile.empty() && (m_impl->stallPoll++ % 10u) == 0u)
    {
        // PS2X_TEST_GS_STALL_FILE=<file>: the stall in ms is read from the file (0 or no file: none), so a test can start
        // slowing the game at a chosen moment instead of from the start
        unsigned value = 0;
        if (std::FILE *f = std::fopen(m_impl->stallFile.c_str(), "r"))
        {
            if (std::fscanf(f, "%u", &value) != 1)
                value = 0;
            std::fclose(f);
        }
        m_impl->stallMs = value;
    }
    if (m_impl->stallMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(m_impl->stallMs));
}

void GSBackendProbe::Initialize(uint8_t *vram, uint32_t vramSize)
{
    Impl::Scope t(m_impl.get(), kOther);
    m_impl->inner->Initialize(vram, vramSize);
}

void GSBackendProbe::Reset()
{
    Impl::Scope t(m_impl.get(), kOther);
    m_impl->inner->Reset();
}

void GSBackendProbe::Submit(const GSPrimitiveBatch &batch)
{
    Impl::Scope t(m_impl.get(), kSubmit);
    m_impl->inner->Submit(batch);
}

void GSBackendProbe::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    Impl::Scope t(m_impl.get(), kLoadClut);
    m_impl->inner->LoadClut(tex0, texclut);
}

void GSBackendProbe::BeginTransfer(const GSTransferCommand &command)
{
    Impl::Scope t(m_impl.get(), kBeginTransfer);
    m_impl->inner->BeginTransfer(command);
}

void GSBackendProbe::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Impl::Scope t(m_impl.get(), kUploadImage);
    m_impl->uploadBytes.fetch_add(sizeBytes, std::memory_order_relaxed);
    m_impl->inner->UploadImage(data, sizeBytes);
}

void GSBackendProbe::Flush()
{
    Impl::Scope t(m_impl.get(), kFlush);
    m_impl->inner->Flush();
}

void GSBackendProbe::TextureFlush()
{
    Impl::Scope t(m_impl.get(), kTextureFlush);
    m_impl->inner->TextureFlush();
}

void GSBackendProbe::Sync(GSSyncReason reason)
{
    Impl::Scope t(m_impl.get(), kSync);
    m_impl->inner->Sync(reason);
}

PresentationFrame GSBackendProbe::Present(const GSPresentationRequest &request)
{
    Impl::Scope t(m_impl.get(), kPresent);
    return m_impl->inner->Present(request);
}

bool GSBackendProbe::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    Impl::Scope t(m_impl.get(), kClear);
    return m_impl->inner->ClearFramebuffer(context, rgba);
}

uint32_t GSBackendProbe::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    Impl::Scope t(m_impl.get(), kConsume);
    return m_impl->inner->ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSBackendProbe::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    Impl::Scope t(m_impl.get(), kReadVram);
    return m_impl->inner->ReadVram(psm, base, bw, x, y);
}

void GSBackendProbe::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    Impl::Scope t(m_impl.get(), kWriteVram);
    m_impl->inner->WriteVram(psm, base, bw, x, y, value);
}

void GSBackendProbe::SnapshotVram(std::vector<uint8_t> &out) const
{
    Impl::Scope t(m_impl.get(), kSnapshot);
    m_impl->inner->SnapshotVram(out);
}

GSTransferSnapshot GSBackendProbe::GetTransferSnapshot() const
{
    Impl::Scope t(m_impl.get(), kOther);
    return m_impl->inner->GetTransferSnapshot();
}
