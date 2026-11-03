#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_log.h"
#include <atomic>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace GSInternal;

namespace
{
    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0) & 0xFF) >> 3;
        uint32_t g = ((c >> 8) & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    u32 Rgba5551ToRgba8888(u16 c)
    {
        u32 r = ((c >> 0) & 0x1F) << 3;
        u32 g = ((c >> 5) & 0x1F) << 3;
        u32 b = ((c >> 10) & 0x1F) << 3;
        u32 a = ((c >> 15) & 0x01) << 7;

        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};

    int wrapTextureCoordinate(int coordinate,
                              int textureSize,
                              uint8_t mode,
                              uint16_t regionMin,
                              uint16_t regionMax)
    {
        switch (mode & 0x3u)
        {
        case 0: // REPEAT
            return static_cast<int>(static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(textureSize - 1));
        case 1: // CLAMP
            return clampInt(coordinate, 0, textureSize - 1);
        case 2: // REGION_CLAMP
            return std::min(std::max(coordinate, static_cast<int>(regionMin)), static_cast<int>(regionMax));
        case 3: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(regionMin)) | static_cast<uint32_t>(regionMax));
        default:
            return coordinate;
        }
    }

    bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct PixelWriteMask
    {
        bool writeRgb = true;
        bool writeAlpha = true;
        bool writeDepth = true;

        bool writesFramebuffer() const
        {
            return writeRgb || writeAlpha;
        }

        bool writesAnything() const
        {
            return writesFramebuffer() || writeDepth;
        }
    };

    PixelWriteMask classifyAlphaTest(uint64_t testReg, uint8_t alpha, uint8_t framePsm)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, true, false};
        case 2: // ZB_ONLY
            return {false, false, true};
        case 3: // RGB_ONLY
            // RGB_ONLY is only distinct for RGBA32. The GS treats it as
            // FB_ONLY for RGB24 and RGBA16 framebuffers.
            if (framePsm == GS_PSM_CT32)
                return {true, false, false};
            return {true, true, false};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    bool passesDestinationAlphaTest(uint64_t testReg, uint8_t framePsm, uint32_t rawFramebufferPixel)
    {
        const bool date = ((testReg >> 14) & 0x1u) != 0u;
        if (!date)
            return true;

        const bool datm = ((testReg >> 15) & 0x1u) != 0u;
        switch (framePsm)
        {
        case GS_PSM_CT32:
            return (((rawFramebufferPixel >> 31) & 0x1u) != 0u) == datm;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return (((rawFramebufferPixel >> 15) & 0x1u) != 0u) == datm;
        case GS_PSM_CT24:
            // RGB24 has no destination alpha, so DATE always passes.
            return true;
        default:
            return true;
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        // CSM1 swaps address bits 3 and 4. Preserve the remaining bits:
        // 16-bit CLUTs expose a ninth address bit through CSA[4].
        return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    bool isFourBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    bool isEightBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H;
    }

    // FPWR: integer bilinear filter. Each RGBA8888 texel is spread into four 16-bit lanes of a
    // uint64_t so all channels are weighted at once; weights are 8-bit fractions (0..256).
    uint64_t spreadRgba(uint32_t c)
    {
        uint64_t x = c;
        x = (x | (x << 16)) & 0x0000FFFF0000FFFFull;
        x = (x | (x << 8)) & 0x00FF00FF00FF00FFull;
        return x;
    }

    uint32_t packRgba(uint64_t x)
    {
        x &= 0x00FF00FF00FF00FFull;
        x = (x | (x >> 8)) & 0x0000FFFF0000FFFFull;
        x = (x | (x >> 16)) & 0x00000000FFFFFFFFull;
        return static_cast<uint32_t>(x);
    }

    uint32_t bilinearRgba(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, uint32_t fx, uint32_t fy)
    {
        constexpr uint64_t kHalf = 0x0080008000800080ull;
        const uint64_t top = ((spreadRgba(c00) * (256u - fx) + spreadRgba(c10) * fx + kHalf) >> 8) & 0x00FF00FF00FF00FFull;
        const uint64_t bottom = ((spreadRgba(c01) * (256u - fx) + spreadRgba(c11) * fx + kHalf) >> 8) & 0x00FF00FF00FF00FFull;
        return packRgba((top * (256u - fy) + bottom * fy + kHalf) >> 8);
    }

    // FPWR: worker threads that rasterize in row bands, either the rows of one large primitive
    // or a whole queue of primitives (see DrawBatch). Bands never share a framebuffer
    // pixel. PS2X_GS_THREADS sets the total thread
    // count including the caller (1 disables the pool).
    class RasterPool
    {
    public:
        using BandFunc = std::function<void(int, int, unsigned)>;
        static constexpr int kBandRows = 8;

        static RasterPool &instance()
        {
            // Never destroyed: joining threads during static destruction can hang at exit.
            static RasterPool *pool = new RasterPool();
            return *pool;
        }

        unsigned workerCount() const { return static_cast<unsigned>(m_threads.size()); }

        // Calls fn(firstRow, lastRow, slot) over [firstRow, lastRow]. Slot 0 is the caller.
        void run(int firstRow, int lastRow, const BandFunc &fn)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_fn = &fn;
                m_nextRow.store(firstRow, std::memory_order_relaxed);
                m_lastRow = lastRow;
                m_busyWorkers = static_cast<unsigned>(m_threads.size());
                ++m_generation;
            }
            m_wake.notify_all();
            work(0u);
            std::unique_lock<std::mutex> lock(m_mutex);
            m_done.wait(lock, [this] { return m_busyWorkers == 0u; });
            m_fn = nullptr;
        }

    private:
        RasterPool()
        {
            unsigned total = std::thread::hardware_concurrency() / 2u;
            if (const char *env = std::getenv("PS2X_GS_THREADS"))
                total = static_cast<unsigned>(std::strtoul(env, nullptr, 10));
            total = std::clamp(total, 1u, 16u);
            for (unsigned slot = 1u; slot < total; ++slot)
            {
                m_threads.emplace_back([this, slot] { threadMain(slot); });
                m_threads.back().detach();
            }
        }

        void work(unsigned slot)
        {
            for (;;)
            {
                const int first = m_nextRow.fetch_add(kBandRows, std::memory_order_relaxed);
                if (first > m_lastRow)
                    return;
                (*m_fn)(first, std::min(first + kBandRows - 1, m_lastRow), slot);
            }
        }

        void threadMain(unsigned slot)
        {
            uint64_t seen = 0u;
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_wake.wait(lock, [&] { return m_generation != seen; });
                    seen = m_generation;
                }
                work(slot);
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (--m_busyWorkers == 0u)
                        m_done.notify_one();
                }
            }
        }

        std::vector<std::thread> m_threads;
        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::condition_variable m_done;
        const BandFunc *m_fn = nullptr;
        std::atomic<int> m_nextRow{0};
        int m_lastRow = 0;
        unsigned m_busyWorkers = 0u;
        uint64_t m_generation = 0u;
    };

    // Primitives smaller than this many pixels (bounding box) are drawn on the calling thread.
    constexpr uint64_t kParallelMinPixels = 16384u;
}

namespace
{
    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;
        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }
        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        return {
            static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu),
            static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu)};
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        return {
            (pmode64 & 0x1ull) != 0ull,
            (pmode64 & 0x2ull) != 0ull,
            ((pmode64 >> 5) & 0x1ull) != 0ull,
            ((pmode64 >> 6) & 0x1ull) != 0ull,
            ((pmode64 >> 7) & 0x1ull) != 0ull,
            static_cast<uint8_t>((pmode64 >> 8) & 0xFFu)};
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode2)
    {
        return {(smode2 & 0x1ull) != 0ull, ((smode2 >> 1) & 0x1ull) != 0ull};
    }

    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        if (pixels.empty() || width == 0u || height < 2u)
            return;
        const std::vector<uint8_t> source = pixels;
        for (uint32_t y = 0; y < height; ++y)
        {
            uint32_t sourceY = ((y >> 1u) << 1u) + (oddField ? 1u : 0u);
            if (sourceY >= height)
                sourceY = height - 1u;
            std::memcpy(pixels.data() + y * kHostFrameWidth * 4u,
                        source.data() + sourceY * kHostFrameWidth * 4u,
                        width * 4u);
        }
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
                row[x * 4u + 3u] = 255u;
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
            {
                if (row[x * 4u] != 0u || row[x * 4u + 1u] != 0u || row[x * 4u + 2u] != 0u)
                    ++count;
            }
        }
        return count;
    }
}

// ---------------------------------------------------------------------------------------------
// FPWR: GS census. Opt-in: does nothing unless PS2X_GS_CENSUS=<file path> is set (one cached
// check per call site). It lists which GS features the game really uses, so a GPU renderer
// can be scoped: every primitive, transfer, CLUT load, clear, VRAM peek and presentation that
// reaches this backend is counted on the calling thread, in submission order, under the
// backend's own mutex. About every 5 s (PS2X_GS_CENSUS_INTERVAL=<s> changes that) one JSON line is
// appended to the file with the counts since the previous line (see tools/gs_census.py, which decodes
// the keys). The runner leaves through std::_Exit, so the destructor and atexit hooks below never run
// in the game: the last line written is the last full interval.
//
// Besides feature counts it keeps, for each 8 KiB VRAM page, what wrote it last (host upload,
// local-to-local copy, draw to frame, draw to Z, clear, direct write). That finds the things a
// GPU renderer cannot do cheaply: textures or CLUTs read from pages a draw or clear wrote
// (render to texture), reads of the primitive's own target (feedback), a frame that overlaps
// its Z buffer, and a buffer drawn in a different pixel format than it was last written in.
// Page sets come from bounding boxes, so overlaps are page-level and can over-report.
namespace
{
    bool gsCensusEnabled()
    {
        static const bool enabled = []
        {
            const char *env = std::getenv("PS2X_GS_CENSUS");
            return env && env[0] != '\0';
        }();
        return enabled;
    }

    constexpr uint64_t kCensusNoKey = ~0ull;
    constexpr uint32_t kCensusPages = 512u; // 4 MiB of GS local memory in 8 KiB pages
    constexpr double kCensusDefaultInterval = 5.0; // seconds between lines (PS2X_GS_CENSUS_INTERVAL overrides)
    constexpr uint8_t kCensusExamplesPerKey = 2u;
    constexpr uint32_t kCensusExamplesPerCategory = 600u;

    struct CensusCount
    {
        uint64_t n = 0;
        uint64_t area = 0;
    };
    using CensusTable = std::unordered_map<uint64_t, CensusCount>;

    // What wrote a page last. The writer mask in the tables has bit (1 << kind) set per kind.
    enum CensusWriter : uint8_t
    {
        kWriterNone = 0,   // never written since start
        kWriterUpload = 1, // host to local transfer
        kWriterCopy = 2,   // local to local transfer (destination)
        kWriterDraw = 3,   // draw into the frame buffer
        kWriterDrawZ = 4,  // draw into the Z buffer
        kWriterClear = 5,  // ClearFramebuffer
        kWriterDirect = 6, // WriteVram
    };
    constexpr uint32_t kCensusDrawnMask = (1u << kWriterDraw) | (1u << kWriterDrawZ) | (1u << kWriterClear);

    // Tables. The first kCensusStateTables are filled from one key per primitive (kCensusNoKey
    // when not applicable) and also make up the pipeline-state hashes; the rest are events.
    // Every table holds key -> {count, area}; the key layouts are documented in
    // tools/gs_census.py, which must match.
    enum CensusTableId : unsigned
    {
        kTabPrim,
        kTabFrame,
        kTabFbmsk,
        kTabZbuf,
        kTabTest,
        kTabAlpha,
        kTabFba,
        kTabColclamp,
        kTabDither,
        kTabScanmsk,
        kTabFog,
        kTabTex0,
        kTabTex1,
        kTabClamp,
        kTabTexa,
        kTabFilter,
        kTabPersp,
        kTabScissor,
        kTabOffset,
        kTabRtFrame,
        kTabRtZ,
        kCensusStateTables,
        kTabTexSrc = kCensusStateTables,
        kTabRttTex,
        kTabFeedback,
        kTabFzOverlap,
        kTabReinterp,
        kTabUpload,
        kTabUploadBytes,
        kTabL2L,
        kTabL2H,
        kTabClutCalls,
        kTabClutLoads,
        kTabClutSrc,
        kTabDirect,
        kTabClear,
        kTabClearColor,
        kTabTexFmt,
        kTabUploadRect,
        kTabAreaHist,
        kTabClobber,
        kTabPrimsPerMark,
        kTabStatesFull,
        kTabStatesCoarse,
        kCensusTables
    };
    const char *const kCensusTableNames[] = {
        "prim", "frame", "fbmsk", "zbuf", "test", "alpha", "fba", "colclamp", "dither", "scanmsk", "fog",
        "tex0", "tex1", "clamp", "texa", "filter", "persp", "scissor", "offset", "rt_frame", "rt_z",
        "tex_src", "rtt_tex", "feedback", "fz_overlap", "reinterp", "upload", "upload_bytes", "l2l", "l2h",
        "clut_calls", "clut_loads", "clut_src", "direct", "clear", "clear_color", "tex_fmt", "upload_rect", "area_hist", "clobber", "prims_per_mark",
        "states_full", "states_coarse"};
    static_assert(sizeof(kCensusTableNames) / sizeof(kCensusTableNames[0]) == kCensusTables,
                  "one name per census table");

    struct CensusPageSet
    {
        uint64_t w[kCensusPages / 64u] = {};
        void set(uint32_t page) { page &= kCensusPages - 1u; w[page >> 6u] |= 1ull << (page & 63u); }
        bool empty() const
        {
            uint64_t any = 0;
            for (uint64_t v : w)
                any |= v;
            return any == 0;
        }
        bool intersects(const CensusPageSet &o) const
        {
            uint64_t any = 0;
            for (unsigned i = 0; i < kCensusPages / 64u; ++i)
                any |= w[i] & o.w[i];
            return any != 0;
        }
        uint32_t count() const
        {
            uint32_t n = 0;
            for (uint64_t v : w)
                n += static_cast<uint32_t>(std::popcount(v));
            return n;
        }
        template <typename F>
        void forEach(F &&f) const
        {
            for (unsigned i = 0; i < kCensusPages / 64u; ++i)
                for (uint64_t bits = w[i]; bits != 0; bits &= bits - 1)
                    f(i * 64u + static_cast<uint32_t>(std::countr_zero(bits)));
        }
    };

    struct CensusPage
    {
        uint8_t kind = kWriterNone;
        uint8_t psm = 0;
        uint8_t fbw = 0;
        uint16_t base = 0; // fbp / zbp for draws, destination page for transfers
        uint32_t seq = 0;  // census sequence number of the write
    };

    // Page size in pixels for a pixel storage format (8 KiB pages).
    void censusPageDims(uint32_t psm, uint32_t &w, uint32_t &h)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            w = 64u;
            h = 64u;
            return;
        case GS_PSM_T8:
            w = 128u;
            h = 64u;
            return;
        case GS_PSM_T4:
            w = 128u;
            h = 128u;
            return;
        default: // 32-bit layouts, including T8H/T4HL/T4HH
            w = 64u;
            h = 32u;
            return;
        }
    }

    // Pages with the same number share an address layout and swizzle.
    uint32_t censusLayout(uint32_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
        case GS_PSM_Z16:
            return 1u;
        case GS_PSM_CT16S:
        case GS_PSM_Z16S:
            return 2u;
        case GS_PSM_T8:
            return 3u;
        case GS_PSM_T4:
            return 4u;
        default:
            return 0u; // CT32, CT24, Z32, Z24 and the 32-bit sub-field formats
        }
    }

    bool censusIndexed(uint32_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T4 || psm == GS_PSM_T8H || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    // Pages touched by the pixel rectangle [x0,x1) x [y0,y1) of a buffer at `baseBlock` (256 byte
    // units) that is `widthPx` wide. A base that is not page aligned also touches one page more.
    void censusRectPages(CensusPageSet &out, uint32_t baseBlock, uint32_t widthPx, uint32_t psm,
                         int x0, int y0, int x1, int y1)
    {
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        if (x1 <= x0 || y1 <= y0)
            return;
        uint32_t pw, ph;
        censusPageDims(psm, pw, ph);
        const uint32_t perRow = std::max(1u, (widthPx + pw - 1u) / pw);
        const uint32_t basePage = baseBlock >> 5u;
        const uint32_t px0 = static_cast<uint32_t>(x0) / pw, px1 = static_cast<uint32_t>(x1 - 1) / pw;
        const uint32_t py0 = static_cast<uint32_t>(y0) / ph, py1 = static_cast<uint32_t>(y1 - 1) / ph;
        uint32_t highest = 0;
        for (uint32_t py = py0; py <= py1; ++py)
            for (uint32_t px = px0; px <= px1; ++px)
            {
                const uint32_t page = basePage + py * perRow + px;
                out.set(page);
                highest = std::max(highest, page);
            }
        if ((baseBlock & 31u) != 0u)
            out.set(highest + 1u);
    }

    uint64_t censusMix(uint64_t h, uint64_t v)
    {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
        return h;
    }

    // The same state with the constants a GPU shader would take as uniforms removed: what is
    // left decides which pipeline variant is needed.
    uint64_t censusCoarseWord(unsigned table, uint64_t key)
    {
        if (key == kCensusNoKey)
            return key;
        switch (table)
        {
        case kTabPrim:
            return key & ~(1ull << 9); // CTXT only picks a register set
        case kTabFrame:
            return key & (63ull | (1ull << 12)); // psm and "any mask", not the width
        case kTabFbmsk:
        case kTabFog:
        case kTabScissor:
        case kTabOffset:
        case kTabRtFrame:
        case kTabRtZ:
            return kCensusNoKey;
        case kTabTest:
            return key & ~0xFF0ull; // AREF is a constant
        case kTabAlpha:
            return key & 0xFFull; // FIX is a constant
        case kTabDither:
            return key & 1ull;
        case kTabTex0: // psm, tcc, tfx, cpsm, csm; not size, width or CSA
            return key & (63ull | (1ull << 14) | (3ull << 15) | (0xFull << 17) | (1ull << 21));
        case kTabTex1:
            return key & 0x1FFull;
        case kTabClamp:
            return key & 0xFull;
        case kTabTexa:
            return (key >> 8) & 1ull; // AEM
        default:
            return key;
        }
    }

    std::atomic<uint64_t> s_censusDrawnInBatches{0}; // primitives the drawer thread drew from batches

    class GsCensus
    {
    public:
        static GsCensus &instance()
        {
            static GsCensus *census = new GsCensus(); // never destroyed: used from atexit and other threads
            return *census;
        }

        // ---- hooks, called by GSCpuBackend. `who` is the calling backend; only the first one
        // counts (Present makes a scratch backend on the presenter thread). ----

        void primitive(const void *who, const GSPrimitiveBatch &b)
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (!accept(who))
                return;
            const auto begin = std::chrono::steady_clock::now();
            countPrimitive(b);
            m_iv.nanos += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
        }

        // Called after the primitive was queued (dropped ones included) or before it is drawn
        // directly. `pending` is the backend's own count of primitives in its unkicked queue,
        // read under the backend mutex; lines are only written from hooks that hold that mutex,
        // so each line can check the census against the backend's own queue accounting.
        void path(const void *who, bool queued, size_t pending)
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (!accept(who))
                return;
            if (queued)
                ++m_iv.queued;
            else
            {
                ++m_iv.direct;
                ++m_cum.direct;
            }
            if ((++m_tickCounter & 255u) == 0u)
                tick(false, false, static_cast<int64_t>(pending));
        }

        void dropped(const void *who)
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (!accept(who))
                return;
            ++m_iv.dropped;
            ++m_cum.dropped;
        }

        void kick(const void *who, size_t prims)
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (!accept(who))
                return;
            ++m_iv.kicks;
            m_cum.kickedPrims += prims;
        }

        void transfer(const void *who, const GSTransferCommand &cmd)
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (!accept(who))
                return;
            ++m_seq;
            const GSBitBltBuf &bb = cmd.bitbltbuf;
            const uint32_t rrw = cmd.trxreg.rrw, rrh = cmd.trxreg.rrh;
            const uint64_t pixels = static_cast<uint64_t>(rrw) * rrh;
            if (cmd.direction == 0u)
            {
                m_uploadKey = bb.dpsm | (static_cast<uint64_t>(bb.dbw) << 6);
                add(kTabUpload, m_uploadKey, 1, pixels);
                add(kTabUploadRect, bb.dpsm | (static_cast<uint64_t>(rrw & 0xFFF) << 6) | (static_cast<uint64_t>(rrh & 0xFFF) << 18), 1, pixels);
                CensusPageSet dst;
                censusRectPages(dst, bb.dbp, std::max<uint32_t>(bb.dbw, 1u) * 64u, bb.dpsm,
                                cmd.trxpos.dsax, cmd.trxpos.dsay, cmd.trxpos.dsax + rrw, cmd.trxpos.dsay + rrh);
                clobberCheck(dst, bb.dpsm, false, bb, rrw, rrh, pixels);
                markPages(dst, kWriterUpload, bb.dpsm, bb.dbw, bb.dbp >> 5u);
            }
            else if (cmd.direction == 2u)
            {
                CensusPageSet src, dst;
                censusRectPages(src, bb.sbp, std::max<uint32_t>(bb.sbw, 1u) * 64u, bb.spsm,
                                cmd.trxpos.ssax, cmd.trxpos.ssay, cmd.trxpos.ssax + rrw, cmd.trxpos.ssay + rrh);
                const uint32_t mask = writerMask(src);
                const uint64_t key = bb.spsm | (static_cast<uint64_t>(bb.dpsm) << 6) | (static_cast<uint64_t>(mask) << 12) |
                                     (static_cast<uint64_t>(rrw) << 19) | (static_cast<uint64_t>(rrh) << 31);
                add(kTabL2L, key, 1, pixels);
                if ((mask & kCensusDrawnMask) != 0u && exampleOk(kTabL2L, key))
                {
                    char buf[512];
                    std::snprintf(buf, sizeof(buf),
                                  "{\"cat\":\"l2l\",\"t\":%.2f,\"spsm\":%u,\"dpsm\":%u,\"sbp\":%u,\"sbw\":%u,\"dbp\":%u,\"dbw\":%u,"
                                  "\"w\":%u,\"h\":%u,\"src_pages\":[%u,%u],%s}",
                                  elapsed(), bb.spsm, bb.dpsm, bb.sbp, bb.sbw, bb.dbp, bb.dbw, rrw, rrh,
                                  firstPage(src), src.count(), writerInfo(src).c_str());
                    pushExample(kTabL2L, key, buf);
                }
                censusRectPages(dst, bb.dbp, std::max<uint32_t>(bb.dbw, 1u) * 64u, bb.dpsm,
                                cmd.trxpos.dsax, cmd.trxpos.dsay, cmd.trxpos.dsax + rrw, cmd.trxpos.dsay + rrh);
                clobberCheck(dst, bb.dpsm, true, bb, rrw, rrh, pixels);
                markPages(dst, kWriterCopy, bb.dpsm, bb.dbw, bb.dbp >> 5u);
            }
            else if (cmd.direction == 1u)
            {
                CensusPageSet src;
                censusRectPages(src, bb.sbp, std::max<uint32_t>(bb.sbw, 1u) * 64u, bb.spsm,
                                cmd.trxpos.ssax, cmd.trxpos.ssay, cmd.trxpos.ssax + rrw, cmd.trxpos.ssay + rrh);
                const uint32_t mask = writerMask(src);
                const uint64_t key = bb.spsm | (static_cast<uint64_t>(mask) << 6) | (static_cast<uint64_t>(rrw) << 13) |
                                     (static_cast<uint64_t>(rrh) << 25);
                add(kTabL2H, key, 1, pixels);
                if ((mask & kCensusDrawnMask) != 0u && exampleOk(kTabL2H, key))
                {
                    char buf[512];
                    std::snprintf(buf, sizeof(buf),
                                  "{\"cat\":\"l2h\",\"t\":%.2f,\"spsm\":%u,\"sbp\":%u,\"sbw\":%u,\"w\":%u,\"h\":%u,"
                                  "\"src_pages\":[%u,%u],%s}",
                                  elapsed(), bb.spsm, bb.sbp, bb.sbw, rrw, rrh, firstPage(src), src.count(),
                                  writerInfo(src).c_str());
                    pushExample(kTabL2H, key, buf);
                }
            }
        }

        void uploadChunk(const void *who, uint32_t sizeBytes)
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (accept(who))
                add(kTabUploadBytes, m_uploadKey, 1, sizeBytes);
        }

        void clutCall(const void *who, const GSTex0Reg &t)
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (!accept(who))
                return;
            const uint64_t key = t.cld | (static_cast<uint64_t>(t.psm == GS_PSM_T4 || t.psm == GS_PSM_T4HL || t.psm == GS_PSM_T4HH) << 3) |
                                 (static_cast<uint64_t>(t.cpsm & 0xF) << 4) | (static_cast<uint64_t>(t.csm & 1) << 8);
            add(kTabClutCalls, key, 1, 0);
        }

        void clutLoad(const void *who, const GSTex0Reg &t, const GSTexClutReg &tc)
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (!accept(who))
                return;
            ++m_seq;
            const bool four = t.psm == GS_PSM_T4 || t.psm == GS_PSM_T4HL || t.psm == GS_PSM_T4HH;
            const uint32_t entries = four ? 16u : 256u;
            const bool sixteen = t.cpsm == GS_PSM_CT16 || t.cpsm == GS_PSM_CT16S;
            add(kTabClutLoads,
                (t.cpsm & 0xF) | (static_cast<uint64_t>(t.csm & 1) << 4) | (static_cast<uint64_t>(t.cld & 7) << 5) |
                    (static_cast<uint64_t>(four) << 8),
                1, entries);
            // The pages the load reads.
            CensusPageSet src;
            if (t.csm == 0u)
            {
                const uint32_t bytes = entries * (sixteen ? 2u : 4u);
                const uint32_t first = (t.cbp * 256u) >> 13u, last = (t.cbp * 256u + bytes - 1u) >> 13u;
                for (uint32_t p = first; p <= last; ++p)
                    src.set(p);
            }
            else
            {
                const uint32_t cou = static_cast<uint32_t>(tc.cou) * 16u;
                censusRectPages(src, t.cbp, std::max<uint32_t>(tc.cbw, 1u) * 64u, t.cpsm,
                                static_cast<int>(cou), tc.cov, static_cast<int>(cou + entries), tc.cov + 1);
            }
            const uint32_t mask = writerMask(src);
            uint32_t writerPsm = 0, writerSeq = 0;
            src.forEach([&](uint32_t p) {
                if (m_pages[p].kind != kWriterNone && m_pages[p].seq >= writerSeq)
                {
                    writerSeq = m_pages[p].seq;
                    writerPsm = m_pages[p].psm;
                }
            });
            const uint64_t key = mask | (static_cast<uint64_t>(t.cpsm & 0xF) << 8) | (static_cast<uint64_t>(t.csm & 1) << 12) |
                                 (static_cast<uint64_t>(writerPsm & 63) << 13);
            add(kTabClutSrc, key, 1, entries);
            if ((mask & kCensusDrawnMask) != 0u && exampleOk(kTabClutSrc, key))
            {
                char buf[512];
                std::snprintf(buf, sizeof(buf),
                              "{\"cat\":\"rtt_clut\",\"t\":%.2f,\"cbp\":%u,\"cpsm\":%u,\"csm\":%u,\"csa\":%u,\"cld\":%u,"
                              "\"tex_psm\":%u,\"cbw\":%u,\"cou\":%u,\"cov\":%u,\"pages\":[%u,%u],%s}",
                              elapsed(), t.cbp, t.cpsm, t.csm, t.csa, t.cld, t.psm, tc.cbw, tc.cou, tc.cov,
                              firstPage(src), src.count(), writerInfo(src).c_str());
                pushExample(kTabClutSrc, key, buf);
            }
        }

        void clear(const void *who, const GSContext &ctx, uint32_t rgba)
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (!accept(who))
                return;
            ++m_seq;
            const uint32_t x0 = ctx.scissor.x0, x1 = std::max<uint32_t>(x0, ctx.scissor.x1);
            const uint32_t y0 = ctx.scissor.y0, y1 = std::max<uint32_t>(y0, ctx.scissor.y1);
            const uint64_t area = static_cast<uint64_t>(x1 - x0 + 1u) * (y1 - y0 + 1u);
            add(kTabClear,
                ctx.frame.fbp | (static_cast<uint64_t>(ctx.frame.fbw) << 9) | (static_cast<uint64_t>(ctx.frame.psm & 63) << 15) |
                    (static_cast<uint64_t>(ctx.frame.fbmsk != 0u) << 21),
                1, area);
            add(kTabClearColor, rgba, 1, area);
            if (ctx.frame.fbw != 0u)
            {
                CensusPageSet dst;
                censusRectPages(dst, ctx.frame.fbp << 5u, ctx.frame.fbw * 64u, ctx.frame.psm,
                                static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 + 1u), static_cast<int>(y1 + 1u));
                markPages(dst, kWriterClear, ctx.frame.psm, ctx.frame.fbw, ctx.frame.fbp);
            }
        }

        void direct(const void *who, bool write, uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (!accept(who))
                return;
            add(kTabDirect, (write ? 1u : 0u) | (static_cast<uint64_t>(psm & 63u) << 1), 1, 1);
            if (write)
            {
                ++m_seq;
                CensusPageSet dst;
                censusRectPages(dst, base, std::max<uint32_t>(bw, 1u) * 64u, psm, static_cast<int>(x), static_cast<int>(y),
                                static_cast<int>(x + 1u), static_cast<int>(y + 1u));
                markPages(dst, kWriterDirect, psm, bw, base >> 5u);
            }
        }

        // `mark` is a guest VBlank marker (called under the backend mutex, `pending` valid); otherwise
        // the host presenter showed a frame (no mutex held, `pending` unused).
        void present(const void *who, const GSPresentationRequest &r, bool mark, size_t pending)
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (!accept(who))
                return;
            uint64_t h = 0x1234567ull;
            const uint64_t regs[6] = {r.pmode, r.smode2, r.dispfb1, r.display1, r.dispfb2, r.display2};
            for (uint64_t v : regs)
                h = censusMix(h, v);
            PresentEntry &e = m_present[h];
            std::memcpy(e.regs, regs, sizeof(regs));
            if (mark)
            {
                ++e.marks;
                ++m_iv.marks;
                add(kTabPrimsPerMark, std::min<uint64_t>(m_primsSinceMark, 0xFFFFFu), 1, 0);
                m_primsSinceMark = 0;
                tick(false, false, static_cast<int64_t>(pending));
            }
            else
            {
                ++e.presents;
                ++m_iv.presents;
            }
        }

        // At shutdown: after the drawer has drained, so drawn counts are complete.
        void finalize(const void *who)
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (accept(who))
                tick(true, true, -1);
        }

        void finalizeAtExit()
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (m_file && !m_finalDone)
                tick(true, true, -1);
        }

    private:
        struct PresentEntry
        {
            uint64_t regs[6] = {};
            uint64_t marks = 0;
            uint64_t presents = 0;
        };
        struct Interval
        {
            uint64_t submit = 0, queued = 0, direct = 0, dropped = 0, empty = 0, area = 0, nanos = 0;
            uint64_t marks = 0, presents = 0, kicks = 0;
        };
        struct Cumulative
        {
            uint64_t submitted = 0, direct = 0, dropped = 0, kickedPrims = 0;
        };

        bool accept(const void *who)
        {
            if (!m_owner)
            {
                m_owner = who;
                start();
            }
            return who == m_owner && m_file != nullptr;
        }

        void start()
        {
            const char *path = std::getenv("PS2X_GS_CENSUS");
            m_file = path ? std::fopen(path, "ab") : nullptr;
            if (!m_file)
            {
                std::fprintf(stderr, "[gs:census] cannot open PS2X_GS_CENSUS file\n");
                return;
            }
            if (const char *env = std::getenv("PS2X_GS_CENSUS_INTERVAL"))
                m_interval = std::max(0.2, std::atof(env));
            m_t0 = std::chrono::steady_clock::now();
            m_last = m_t0;
            m_unix0 = unixNow();
            std::atexit([] { GsCensus::instance().finalizeAtExit(); });
        }

        static double unixNow()
        {
            return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        }
        double elapsed() const
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_t0).count();
        }

        void add(unsigned table, uint64_t key, uint64_t n, uint64_t area)
        {
            CensusCount &c = m_tables[table][key];
            c.n += n;
            c.area += area;
        }

        // Writer kinds present in a page set, as a bit mask over CensusWriter.
        uint32_t writerMask(const CensusPageSet &set) const
        {
            uint32_t mask = 0;
            set.forEach([&](uint32_t p) { mask |= 1u << m_pages[p].kind; });
            return mask;
        }

        static uint32_t firstPage(const CensusPageSet &set)
        {
            uint32_t first = 0;
            bool found = false;
            set.forEach([&](uint32_t p) {
                if (!found)
                {
                    first = p;
                    found = true;
                }
            });
            return first;
        }

        // JSON members describing the most recent drawn-to writer in the set.
        std::string writerInfo(const CensusPageSet &set) const
        {
            const CensusPage *best = nullptr;
            uint32_t bestPage = 0;
            set.forEach([&](uint32_t p) {
                const CensusPage &pg = m_pages[p];
                if (((1u << pg.kind) & kCensusDrawnMask) != 0u && (!best || pg.seq > best->seq))
                {
                    best = &pg;
                    bestPage = p;
                }
            });
            char buf[200];
            if (!best)
                return "\"writer\":null";
            std::snprintf(buf, sizeof(buf), "\"writer\":{\"kind\":%u,\"psm\":%u,\"base\":%u,\"fbw\":%u,\"page\":%u,\"age\":%u}",
                          best->kind, best->psm, best->base, best->fbw, bestPage, m_seq - best->seq);
            return buf;
        }

        void markPages(const CensusPageSet &set, uint8_t kind, uint32_t psm, uint32_t fbw, uint32_t base)
        {
            set.forEach([&](uint32_t p) {
                CensusPage &pg = m_pages[p];
                pg.kind = kind;
                pg.psm = static_cast<uint8_t>(psm);
                pg.fbw = static_cast<uint8_t>(fbw);
                pg.base = static_cast<uint16_t>(base);
                pg.seq = m_seq;
            });
        }

        // At most a few examples per key in each 15 s of the run, so every scene can show its own.
        bool exampleOk(unsigned table, uint64_t key)
        {
            const uint32_t bucket = static_cast<uint32_t>(elapsed() / 15.0);
            if (bucket != m_exampleBucket)
            {
                m_exampleBucket = bucket;
                m_exampleCounts.clear();
            }
            uint8_t &n = m_exampleCounts[censusMix(table, key)];
            if (n >= kCensusExamplesPerKey || m_categoryExamples[table] >= kCensusExamplesPerCategory)
                return false;
            ++n;
            ++m_categoryExamples[table];
            return true;
        }

        // Adds the table name and key so the summary tool can match an example to its table row.
        void pushExample(unsigned table, uint64_t key, const char *json)
        {
            std::string s = json;
            s.insert(1, std::string("\"table\":\"") + kCensusTableNames[table] + "\",\"key\":" + std::to_string(key) + ",");
            m_examples.push_back(std::move(s));
        }

        void countPrimitive(const GSPrimitiveBatch &b)
        {
            const GSDrawState &s = b.state;
            const GSContext &c = s.context;
            const GSPrimReg &p = s.prim;
            const GSTex0Reg &t = c.tex0;
            ++m_seq;
            ++m_primsSinceMark;
            ++m_iv.submit;
            ++m_cum.submitted;

            // Bounding box in window pixels, clipped to the scissor. A triangle's box is larger
            // than its coverage; the area column is the box, as asked.
            const int ofx = c.xyoffset.ofx >> 4, ofy = c.xyoffset.ofy >> 4;
            const unsigned vc = std::min<unsigned>(b.vertexCount, 3u);
            float minX = b.vertices[0].x, maxX = minX, minY = b.vertices[0].y, maxY = minY;
            for (unsigned i = 1; i < vc; ++i)
            {
                minX = std::min(minX, b.vertices[i].x);
                maxX = std::max(maxX, b.vertices[i].x);
                minY = std::min(minY, b.vertices[i].y);
                maxY = std::max(maxY, b.vertices[i].y);
            }
            int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
            uint64_t area = 0;
            if (std::isfinite(minX) && std::isfinite(maxX) && std::isfinite(minY) && std::isfinite(maxY) &&
                std::fabs(minX) < 1.0e6f && std::fabs(maxX) < 1.0e6f && std::fabs(minY) < 1.0e6f && std::fabs(maxY) < 1.0e6f)
            {
                bx0 = static_cast<int>(std::floor(minX)) - ofx;
                by0 = static_cast<int>(std::floor(minY)) - ofy;
                bx1 = static_cast<int>(std::ceil(maxX)) - ofx;
                by1 = static_cast<int>(std::ceil(maxY)) - ofy;
                if (bx1 <= bx0)
                    bx1 = bx0 + 1;
                if (by1 <= by0)
                    by1 = by0 + 1;
                bx0 = std::max(bx0, static_cast<int>(c.scissor.x0));
                by0 = std::max(by0, static_cast<int>(c.scissor.y0));
                bx1 = std::min(bx1, static_cast<int>(c.scissor.x1) + 1);
                by1 = std::min(by1, static_cast<int>(c.scissor.y1) + 1);
                if (bx1 > bx0 && by1 > by0)
                    area = static_cast<uint64_t>(bx1 - bx0) * static_cast<uint64_t>(by1 - by0);
                else
                    bx1 = bx0, by1 = by0;
            }
            m_iv.area += area;
            if (area == 0)
                ++m_iv.empty;
            add(kTabAreaHist, static_cast<uint64_t>(p.type & 7) | (static_cast<uint64_t>(std::bit_width(area)) << 3), 1, area);

            // Keys.
            uint64_t k[kCensusStateTables];
            for (uint64_t &v : k)
                v = kCensusNoKey;
            const bool zmask = c.zbuf.zmask;
            const bool zte = ((c.test >> 16) & 1u) != 0u;
            k[kTabPrim] = static_cast<uint64_t>(p.type & 7) | (static_cast<uint64_t>(p.iip) << 3) | (static_cast<uint64_t>(p.tme) << 4) |
                          (static_cast<uint64_t>(p.fge) << 5) | (static_cast<uint64_t>(p.abe) << 6) | (static_cast<uint64_t>(p.aa1) << 7) |
                          (static_cast<uint64_t>(p.fst) << 8) | (static_cast<uint64_t>(p.ctxt) << 9) | (static_cast<uint64_t>(p.fix) << 10) |
                          (static_cast<uint64_t>(s.pabe) << 11);
            k[kTabFrame] = static_cast<uint64_t>(c.frame.psm & 63) | (static_cast<uint64_t>(c.frame.fbw & 63) << 6) |
                           (static_cast<uint64_t>(c.frame.fbmsk != 0u) << 12);
            if (c.frame.fbmsk != 0u)
                k[kTabFbmsk] = c.frame.fbmsk;
            k[kTabZbuf] = static_cast<uint64_t>(c.zbuf.psm & 63) | (static_cast<uint64_t>(zmask) << 6);
            k[kTabTest] = c.test & 0x7FFFFull;
            if (p.abe)
                k[kTabAlpha] = (c.alpha & 0xFFull) | (((c.alpha >> 32) & 0xFFull) << 8);
            k[kTabFba] = c.fba & 1ull;
            k[kTabColclamp] = s.colclamp & 1ull;
            k[kTabDither] = (s.dthe & 1ull) != 0u ? (1ull | ((s.dimx & 0xFFFFFFFFFFFFull) << 1)) : 0ull;
            k[kTabScanmsk] = s.scanmsk & 3ull;
            if (p.fge)
                k[kTabFog] = static_cast<uint64_t>(s.fogR) | (static_cast<uint64_t>(s.fogG) << 8) | (static_cast<uint64_t>(s.fogB) << 16);
            k[kTabScissor] = static_cast<uint64_t>(c.scissor.x0 & 0x7FF) | (static_cast<uint64_t>(c.scissor.x1 & 0x7FF) << 11) |
                             (static_cast<uint64_t>(c.scissor.y0 & 0x7FF) << 22) | (static_cast<uint64_t>(c.scissor.y1 & 0x7FF) << 33);
            k[kTabOffset] = static_cast<uint64_t>(c.xyoffset.ofx) | (static_cast<uint64_t>(c.xyoffset.ofy) << 16);
            k[kTabRtFrame] = static_cast<uint64_t>(c.frame.fbp & 0x1FF) | (static_cast<uint64_t>(c.frame.fbw & 63) << 9) |
                             (static_cast<uint64_t>(c.frame.psm & 63) << 15);
            k[kTabRtZ] = static_cast<uint64_t>(c.zbuf.zbp & 0x1FF) | (static_cast<uint64_t>(c.zbuf.psm & 63) << 9) |
                         (static_cast<uint64_t>(zmask) << 15) | (static_cast<uint64_t>(zte) << 16);

            const bool indexed = censusIndexed(t.psm);
            if (p.tme)
            {
                k[kTabTex0] = static_cast<uint64_t>(t.psm & 63) | (static_cast<uint64_t>(t.tw & 15) << 6) | (static_cast<uint64_t>(t.th & 15) << 10) |
                              (static_cast<uint64_t>(t.tcc & 1) << 14) | (static_cast<uint64_t>(t.tfx & 3) << 15) |
                              (indexed ? (static_cast<uint64_t>(t.cpsm & 15) << 17) | (static_cast<uint64_t>(t.csm & 1) << 21) |
                                             (static_cast<uint64_t>(t.csa & 31) << 22)
                                       : 0ull) |
                              (static_cast<uint64_t>(t.tbw & 63) << 27);
                const uint64_t t1 = c.tex1;
                k[kTabTex1] = (t1 & 0x1FFull) | (((t1 >> 19) & 3ull) << 9) | (((t1 >> 32) & 0xFFFull) << 11) |
                              (static_cast<uint64_t>(c.miptbp1 != 0u) << 23) | (static_cast<uint64_t>(c.miptbp2 != 0u) << 24);
                const uint64_t cl = c.clamp;
                const uint64_t wms = cl & 3ull, wmt = (cl >> 2) & 3ull;
                uint64_t ck = wms | (wmt << 2);
                if (wms >= 2u)
                    ck |= (((cl >> 4) & 0x3FFull) << 4) | (((cl >> 14) & 0x3FFull) << 14);
                if (wmt >= 2u)
                    ck |= (((cl >> 24) & 0x3FFull) << 24) | (((cl >> 34) & 0x3FFull) << 34);
                k[kTabClamp] = ck;
                const bool texaUsed = t.psm == GS_PSM_CT24 || t.psm == GS_PSM_CT16 || t.psm == GS_PSM_CT16S ||
                                      (indexed && (t.cpsm == GS_PSM_CT24 || t.cpsm == GS_PSM_CT16 || t.cpsm == GS_PSM_CT16S));
                if (texaUsed)
                    k[kTabTexa] = static_cast<uint64_t>(s.texa.ta0) | (static_cast<uint64_t>(s.texa.aem) << 8) |
                                  (static_cast<uint64_t>(s.texa.ta1) << 9);
                const uint64_t mmag = (t1 >> 5) & 1ull, mmin = (t1 >> 6) & 7ull;
                k[kTabFilter] = static_cast<uint64_t>(s.linearFilter) | (mmag << 1) | (mmin << 2);
                bool qVaries = false;
                if (!p.fst)
                {
                    const float q0 = b.vertices[0].q;
                    qVaries = std::fabs(q0 - 1.0f) > 1.0e-6f;
                    for (unsigned i = 1; i < vc; ++i)
                        qVaries |= std::fabs(b.vertices[i].q - q0) > 1.0e-4f * std::fabs(q0);
                }
                k[kTabPersp] = static_cast<uint64_t>(!p.fst) | (static_cast<uint64_t>(qVaries) << 1);
            }

            // Pages this primitive reads and writes.
            const uint32_t fbw = std::max<uint32_t>(c.frame.fbw, 1u);
            CensusPageSet frameSet, zSet;
            const bool writesFrame = c.frame.fbmsk != 0xFFFFFFFFu && area != 0;
            const bool writesZ = !zmask && area != 0;
            if (writesFrame)
                censusRectPages(frameSet, c.frame.fbp << 5u, fbw * 64u, c.frame.psm, bx0, by0, bx1, by1);
            if (writesZ)
                censusRectPages(zSet, c.zbuf.zbp << 5u, fbw * 64u, c.zbuf.psm, bx0, by0, bx1, by1);

            if (p.tme && area != 0)
                textureHazards(b, bx0, by0, bx1, by1, area, frameSet, zSet);

            // A frame drawn in another pixel format than its pages were last written in.
            if (writesFrame)
                reinterpretCheck(frameSet, c.frame.psm, false, area, c.frame.fbp, c.frame.fbw, bx0, by0, bx1, by1);
            if (writesZ)
                reinterpretCheck(zSet, c.zbuf.psm, true, area, c.zbuf.zbp, c.frame.fbw, bx0, by0, bx1, by1);

            // A frame buffer that overlaps its Z buffer, when depth is in use.
            if (!zmask || zte)
            {
                const int rows = std::max<int>(c.scissor.y1 + 1, 1);
                const uint64_t ck = static_cast<uint64_t>(c.frame.fbp & 0x1FF) | (static_cast<uint64_t>(fbw & 63) << 9) |
                                    (static_cast<uint64_t>(c.frame.psm & 63) << 15) | (static_cast<uint64_t>(c.zbuf.zbp & 0x1FF) << 21) |
                                    (static_cast<uint64_t>(c.zbuf.psm & 63) << 30) | (static_cast<uint64_t>(rows & 0x7FF) << 36);
                auto found = m_fzCache.find(ck);
                if (found == m_fzCache.end())
                {
                    CensusPageSet fs, zs;
                    censusRectPages(fs, c.frame.fbp << 5u, fbw * 64u, c.frame.psm, 0, 0, static_cast<int>(fbw * 64u), rows);
                    censusRectPages(zs, c.zbuf.zbp << 5u, fbw * 64u, c.zbuf.psm, 0, 0, static_cast<int>(fbw * 64u), rows);
                    found = m_fzCache.emplace(ck, fs.intersects(zs)).first;
                }
                if (found->second)
                    add(kTabFzOverlap, ck | (static_cast<uint64_t>(zmask) << 47) | (static_cast<uint64_t>(zte) << 48), 1, area);
            }

            if (writesFrame)
                markPages(frameSet, kWriterDraw, c.frame.psm, c.frame.fbw, c.frame.fbp);
            if (writesZ)
                markPages(zSet, kWriterDrawZ, c.zbuf.psm, c.frame.fbw, c.zbuf.zbp);

            // Count into the state tables, one run of identical states at a time.
            if (m_hasPending && std::memcmp(k, m_pendKeys, sizeof(k)) == 0)
            {
                ++m_pendN;
                m_pendArea += area;
            }
            else
            {
                commitPending();
                std::memcpy(m_pendKeys, k, sizeof(k));
                m_pendN = 1;
                m_pendArea = area;
                m_hasPending = true;
            }
        }

        // The texels a primitive can sample, as a rectangle [u0,u1) x [v0,v1): the box of its vertex
        // coordinates plus a texel of margin for the filter, then cut down by the wrap mode (clamping
        // limits it to the texture or the clamp region; repeating keeps it only if it does not wrap).
        // Whole texture when the coordinates cannot be worked out.
        void textureFootprint(const GSPrimitiveBatch &b, int &u0, int &v0, int &u1, int &v1) const
        {
            const GSContext &c = b.state.context;
            const GSTex0Reg &t = c.tex0;
            const int texW = 1 << std::min<uint32_t>(t.tw, 10u), texH = 1 << std::min<uint32_t>(t.th, 10u);
            u0 = 0;
            v0 = 0;
            u1 = texW;
            v1 = texH;
            const unsigned vc = std::min<unsigned>(b.vertexCount, 3u);
            float minU = 1.0e30f, maxU = -1.0e30f, minV = 1.0e30f, maxV = -1.0e30f;
            for (unsigned i = 0; i < vc; ++i)
            {
                const GSVertex &v = b.vertices[i];
                float u, w;
                if (b.state.prim.fst)
                {
                    u = static_cast<float>(v.u) / 16.0f;
                    w = static_cast<float>(v.v) / 16.0f;
                }
                else
                {
                    if (std::fabs(v.q) < 1.0e-8f)
                        return;
                    u = v.s / v.q * static_cast<float>(texW);
                    w = v.t / v.q * static_cast<float>(texH);
                }
                if (!std::isfinite(u) || !std::isfinite(w) || std::fabs(u) > 1.0e6f || std::fabs(w) > 1.0e6f)
                    return;
                minU = std::min(minU, u);
                maxU = std::max(maxU, u);
                minV = std::min(minV, w);
                maxV = std::max(maxV, w);
            }
            const uint64_t cl = c.clamp;
            auto narrow = [](float lo, float hi, int size, uint64_t mode, int regionMin, int regionMax, int &out0, int &out1)
            {
                int a = static_cast<int>(std::floor(lo)) - 1;
                int e = static_cast<int>(std::ceil(hi)) + 1; // exclusive
                switch (mode & 3u)
                {
                case 1: // CLAMP
                    a = std::clamp(a, 0, size - 1);
                    e = std::clamp(e, a + 1, size);
                    break;
                case 2: // REGION_CLAMP
                    regionMax = std::min(regionMax, size - 1);
                    regionMin = std::min(regionMin, regionMax);
                    a = std::clamp(a, regionMin, regionMax);
                    e = std::clamp(e, a + 1, regionMax + 1);
                    break;
                case 0: // REPEAT: narrow only when no wrap happens
                    if (a >= 0 && e <= size)
                        break;
                    [[fallthrough]];
                default: // REGION_REPEAT, or a repeat that wraps: the whole texture
                    a = 0;
                    e = size;
                    break;
                }
                out0 = a;
                out1 = e;
            };
            narrow(minU, maxU, texW, cl & 3u, static_cast<int>((cl >> 4) & 0x3FFu), static_cast<int>((cl >> 14) & 0x3FFu), u0, u1);
            narrow(minV, maxV, texH, (cl >> 2) & 3u, static_cast<int>((cl >> 24) & 0x3FFu), static_cast<int>((cl >> 34) & 0x3FFu), v0, v1);
        }

        // Texture reads: where the pages came from, render to texture, feedback.
        void textureHazards(const GSPrimitiveBatch &b, int bx0, int by0, int bx1, int by1, uint64_t area,
                            const CensusPageSet &frameSet, const CensusPageSet &zSet)
        {
            const GSContext &c = b.state.context;
            const GSTex0Reg &t = c.tex0;
            const uint32_t texW = 1u << std::min<uint32_t>(t.tw, 10u);
            int fu0, fv0, fu1, fv1;
            textureFootprint(b, fu0, fv0, fu1, fv1);
            CensusPageSet texSet;
            // The row stride is TBW (TW only wraps coordinates), so a 1024 wide texture with TBW 10 lies on a 640 wide grid.
            censusRectPages(texSet, t.tbp0, t.tbw != 0u ? t.tbw * 64u : texW, t.psm, fu0, fv0, fu1, fv1);
            const uint32_t mask = writerMask(texSet);
            add(kTabTexSrc, mask | (static_cast<uint64_t>(t.psm & 63) << 8), 1, area);

            // Pages written by a transfer in another pixel format than the texture is read in.
            {
                uint32_t seen[4], seenPage[4];
                unsigned seenCount = 0;
                texSet.forEach([&](uint32_t p) {
                    const CensusPage &pg = m_pages[p];
                    if ((pg.kind != kWriterUpload && pg.kind != kWriterCopy && pg.kind != kWriterDirect) || pg.psm == t.psm)
                        return;
                    const uint32_t combo = pg.kind | (static_cast<uint32_t>(pg.psm) << 3);
                    for (unsigned i = 0; i < seenCount; ++i)
                        if (seen[i] == combo)
                            return;
                    if (seenCount < 4u)
                    {
                        seenPage[seenCount] = p;
                        seen[seenCount++] = combo;
                    }
                });
                for (unsigned i = 0; i < seenCount; ++i)
                {
                    const uint32_t writerKind = seen[i] & 7u, writerPsm = seen[i] >> 3;
                    const uint64_t key = static_cast<uint64_t>(t.psm & 63) | (static_cast<uint64_t>(writerPsm & 63) << 6) |
                                         (static_cast<uint64_t>(writerKind) << 12) |
                                         (static_cast<uint64_t>(censusLayout(t.psm) != censusLayout(writerPsm)) << 15);
                    add(kTabTexFmt, key, 1, area);
                    if (exampleOk(kTabTexFmt, key))
                    {
                        const CensusPage &pg = m_pages[seenPage[i]];
                        char buf[700];
                        std::snprintf(buf, sizeof(buf),
                                      "{\"cat\":\"tex_fmt\",\"t\":%.2f,\"prim\":{\"type\":%u,\"fst\":%u},"
                                      "\"tex\":{\"tbp0\":%u,\"tbw\":%u,\"psm\":%u,\"tw\":%u,\"th\":%u,\"cbp\":%u,\"cpsm\":%u},"
                                      "\"footprint\":[%d,%d,%d,%d],\"pages\":{\"total\":%u,\"first\":%u},"
                                      "\"writer\":{\"kind\":%u,\"psm\":%u,\"base\":%u,\"fbw\":%u,\"page\":%u,\"age\":%u}}",
                                      elapsed(), b.state.prim.type, b.state.prim.fst, t.tbp0, t.tbw, t.psm, t.tw, t.th, t.cbp, t.cpsm,
                                      fu0, fv0, fu1, fv1, texSet.count(), firstPage(texSet), pg.kind, pg.psm, pg.base, pg.fbw,
                                      seenPage[i], m_seq - pg.seq);
                        pushExample(kTabTexFmt, key, buf);
                    }
                }
            }

            if ((mask & kCensusDrawnMask) != 0u)
            {
                // Render to texture: report the most recently drawn page.
                const CensusPage *best = nullptr;
                uint32_t bestPage = 0, drawnPages = 0;
                texSet.forEach([&](uint32_t p) {
                    const CensusPage &pg = m_pages[p];
                    if (((1u << pg.kind) & kCensusDrawnMask) == 0u)
                        return;
                    ++drawnPages;
                    if (!best || pg.seq > best->seq)
                    {
                        best = &pg;
                        bestPage = p;
                    }
                });
                const uint64_t key = static_cast<uint64_t>(t.psm & 63) | (static_cast<uint64_t>(best->psm & 63) << 6) |
                                     (static_cast<uint64_t>(best->kind) << 12) |
                                     (static_cast<uint64_t>(censusLayout(t.psm) != censusLayout(best->psm)) << 15);
                add(kTabRttTex, key, 1, area);
                if (exampleOk(kTabRttTex, key))
                {
                    char buf[1000];
                    std::snprintf(buf, sizeof(buf),
                                  "{\"cat\":\"rtt_tex\",\"t\":%.2f,\"prim\":{\"type\":%u,\"abe\":%u,\"fst\":%u,\"ctxt\":%u},"
                                  "\"tex\":{\"tbp0\":%u,\"tbw\":%u,\"psm\":%u,\"tw\":%u,\"th\":%u,\"cbp\":%u,\"cpsm\":%u,\"csm\":%u},"
                                  "\"footprint\":[%d,%d,%d,%d],"
                                  "\"pages\":{\"total\":%u,\"drawn\":%u,\"first\":%u},"
                                  "\"writer\":{\"kind\":%u,\"psm\":%u,\"base\":%u,\"fbw\":%u,\"page\":%u,\"age\":%u},"
                                  "\"frame\":{\"fbp\":%u,\"fbw\":%u,\"psm\":%u},\"bbox\":[%d,%d,%d,%d]}",
                                  elapsed(), b.state.prim.type, b.state.prim.abe, b.state.prim.fst, b.state.prim.ctxt, t.tbp0, t.tbw,
                                  t.psm, t.tw, t.th, t.cbp, t.cpsm, t.csm, fu0, fv0, fu1, fv1, texSet.count(), drawnPages,
                                  firstPage(texSet), best->kind, best->psm, best->base, best->fbw, bestPage, m_seq - best->seq,
                                  c.frame.fbp, c.frame.fbw, c.frame.psm, bx0, by0, bx1, by1);
                    pushExample(kTabRttTex, key, buf);
                }
            }

            // Feedback: the texture shares pages with what this primitive writes.
            const bool fbFeedback = texSet.intersects(frameSet);
            const bool zFeedback = texSet.intersects(zSet);
            if (fbFeedback || zFeedback)
            {
                const uint64_t key = (fbFeedback ? 1ull : 0ull) | (zFeedback ? 2ull : 0ull) | (static_cast<uint64_t>(t.psm & 63) << 2) |
                                     (static_cast<uint64_t>(c.frame.psm & 63) << 8);
                add(kTabFeedback, key, 1, area);
                if (exampleOk(kTabFeedback, key))
                {
                    char buf[800];
                    std::snprintf(buf, sizeof(buf),
                                  "{\"cat\":\"feedback\",\"t\":%.2f,\"prim\":{\"type\":%u,\"abe\":%u,\"fst\":%u},"
                                  "\"tex\":{\"tbp0\":%u,\"tbw\":%u,\"psm\":%u,\"tw\":%u,\"th\":%u},\"footprint\":[%d,%d,%d,%d],"
                                  "\"tex_pages\":[%u,%u],"
                                  "\"frame\":{\"fbp\":%u,\"fbw\":%u,\"psm\":%u},\"z\":{\"zbp\":%u,\"psm\":%u,\"zmsk\":%u},"
                                  "\"bbox\":[%d,%d,%d,%d],\"with_frame\":%u,\"with_z\":%u}",
                                  elapsed(), b.state.prim.type, b.state.prim.abe, b.state.prim.fst, t.tbp0, t.tbw, t.psm, t.tw, t.th,
                                  fu0, fv0, fu1, fv1, firstPage(texSet), texSet.count(), c.frame.fbp, c.frame.fbw, c.frame.psm,
                                  c.zbuf.zbp, c.zbuf.psm, static_cast<unsigned>(c.zbuf.zmask), bx0, by0, bx1, by1, fbFeedback, zFeedback);
                    pushExample(kTabFeedback, key, buf);
                }
            }
        }

        void reinterpretCheck(const CensusPageSet &set, uint32_t newPsm, bool isZ, uint64_t area, uint32_t base, uint32_t fbw,
                              int bx0, int by0, int bx1, int by1)
        {
            uint32_t seen[8], seenPage[8];
            unsigned seenCount = 0;
            set.forEach([&](uint32_t p) {
                const CensusPage &pg = m_pages[p];
                if (pg.kind == kWriterNone || pg.psm == newPsm)
                    return;
                const uint32_t combo = pg.kind | (static_cast<uint32_t>(pg.psm) << 3);
                for (unsigned i = 0; i < seenCount; ++i)
                    if (seen[i] == combo)
                        return;
                if (seenCount < 8u)
                {
                    seenPage[seenCount] = p;
                    seen[seenCount++] = combo;
                }
            });
            for (unsigned i = 0; i < seenCount; ++i)
            {
                const uint32_t oldKind = seen[i] & 7u, oldPsm = seen[i] >> 3;
                const uint64_t key = oldKind | (static_cast<uint64_t>(oldPsm) << 3) | (static_cast<uint64_t>(newPsm & 63) << 9) |
                                     (static_cast<uint64_t>(isZ) << 15) |
                                     (static_cast<uint64_t>(censusLayout(oldPsm) != censusLayout(newPsm)) << 16);
                add(kTabReinterp, key, 1, area);
                if (exampleOk(kTabReinterp, key))
                {
                    const CensusPage &pg = m_pages[seenPage[i]];
                    char buf[600];
                    std::snprintf(buf, sizeof(buf),
                                  "{\"cat\":\"reinterp\",\"t\":%.2f,\"new\":{\"is_z\":%u,\"psm\":%u,\"base\":%u,\"fbw\":%u},"
                                  "\"bbox\":[%d,%d,%d,%d],\"pages\":{\"total\":%u,\"first\":%u},"
                                  "\"old\":{\"kind\":%u,\"psm\":%u,\"base\":%u,\"fbw\":%u,\"page\":%u,\"age\":%u}}",
                                  elapsed(), isZ, newPsm, base, fbw, bx0, by0, bx1, by1, set.count(), firstPage(set), pg.kind, pg.psm,
                                  pg.base, pg.fbw, seenPage[i], m_seq - pg.seq);
                    pushExample(kTabReinterp, key, buf);
                }
            }
        }

        // A transfer into pages a draw or clear wrote last (a texture upload into a render target's memory).
        void clobberCheck(const CensusPageSet &dst, uint32_t dpsm, bool isCopy, const GSBitBltBuf &bb, uint32_t rrw, uint32_t rrh,
                          uint64_t pixels)
        {
            uint32_t seen[4], seenPage[4];
            unsigned seenCount = 0;
            dst.forEach([&](uint32_t p) {
                const CensusPage &pg = m_pages[p];
                if (((1u << pg.kind) & kCensusDrawnMask) == 0u)
                    return;
                const uint32_t combo = pg.kind | (static_cast<uint32_t>(pg.psm) << 3);
                for (unsigned i = 0; i < seenCount; ++i)
                    if (seen[i] == combo)
                        return;
                if (seenCount < 4u)
                {
                    seenPage[seenCount] = p;
                    seen[seenCount++] = combo;
                }
            });
            for (unsigned i = 0; i < seenCount; ++i)
            {
                const uint32_t oldKind = seen[i] & 7u, oldPsm = seen[i] >> 3;
                const uint64_t key = static_cast<uint64_t>(dpsm & 63) | (static_cast<uint64_t>(oldKind) << 6) |
                                     (static_cast<uint64_t>(oldPsm & 63) << 9) |
                                     (static_cast<uint64_t>(censusLayout(oldPsm) != censusLayout(dpsm)) << 15) |
                                     (static_cast<uint64_t>(isCopy) << 16);
                add(kTabClobber, key, 1, pixels);
                if (exampleOk(kTabClobber, key))
                {
                    const CensusPage &pg = m_pages[seenPage[i]];
                    char buf[600];
                    std::snprintf(buf, sizeof(buf),
                                  "{\"cat\":\"clobber\",\"t\":%.2f,\"copy\":%u,\"dpsm\":%u,\"dbp\":%u,\"dbw\":%u,\"w\":%u,\"h\":%u,"
                                  "\"pages\":{\"total\":%u,\"first\":%u},"
                                  "\"old\":{\"kind\":%u,\"psm\":%u,\"base\":%u,\"fbw\":%u,\"page\":%u,\"age\":%u}}",
                                  elapsed(), isCopy, dpsm, bb.dbp, bb.dbw, rrw, rrh, dst.count(), firstPage(dst), pg.kind, pg.psm,
                                  pg.base, pg.fbw, seenPage[i], m_seq - pg.seq);
                    pushExample(kTabClobber, key, buf);
                }
            }
        }

        void commitPending()
        {
            if (!m_hasPending)
                return;
            uint64_t full = 0x9E3779B97F4A7C15ull, coarse = 0x243F6A8885A308D3ull;
            for (unsigned i = 0; i < kCensusStateTables; ++i)
            {
                if (m_pendKeys[i] == kCensusNoKey)
                    continue;
                add(i, m_pendKeys[i], m_pendN, m_pendArea);
                // The target and scissor tables are bindings, not pipeline state.
                if (i != kTabScissor && i != kTabOffset && i != kTabRtFrame && i != kTabRtZ)
                    full = censusMix(censusMix(full, i), m_pendKeys[i]);
                const uint64_t cw = censusCoarseWord(i, m_pendKeys[i]);
                if (cw != kCensusNoKey)
                    coarse = censusMix(censusMix(coarse, i), cw);
            }
            add(kTabStatesFull, full, m_pendN, m_pendArea);
            add(kTabStatesCoarse, coarse, m_pendN, m_pendArea);
            if (m_seenFull.insert(full).second || m_seenCoarse.insert(coarse).second)
            {
                // Describe each state the first time it is seen, so the summary tool can decode it.
                std::string s = "[" + std::to_string(full) + "," + std::to_string(coarse);
                for (unsigned i = 0; i < kCensusStateTables; ++i)
                    s += "," + (m_pendKeys[i] == kCensusNoKey ? std::string("-1") : std::to_string(m_pendKeys[i]));
                m_newStates.push_back(s + "]");
            }
            m_hasPending = false;
            m_pendN = 0;
            m_pendArea = 0;
        }

        void tick(bool force, bool final, int64_t pending)
        {
            const auto now = std::chrono::steady_clock::now();
            if (!force && std::chrono::duration<double>(now - m_last).count() < m_interval)
                return;
            commitPending();
            std::string out;
            out.reserve(1u << 16);
            char buf[512];
            const double t = std::chrono::duration<double>(now - m_t0).count();
            const double dt = std::chrono::duration<double>(now - m_last).count();
            const uint64_t drawn = s_censusDrawnInBatches.load(std::memory_order_relaxed);
            std::snprintf(buf, sizeof(buf),
                          "{\"v\":1,\"t\":%.3f,\"dt\":%.3f,\"unix\":%.3f,\"t0\":%.3f,\"final\":%s,"
                          "\"cum\":{\"submitted\":%llu,\"drawn_batches\":%llu,\"direct\":%llu,\"dropped\":%llu,"
                          "\"kicked_prims\":%llu,\"pending\":%lld},"
                          "\"n\":{\"submit\":%llu,\"area\":%llu,\"queued\":%llu,\"direct\":%llu,\"dropped\":%llu,\"empty\":%llu,"
                          "\"drawn_batches\":%llu,\"marks\":%llu,\"presents\":%llu,\"kicks\":%llu,\"census_ns\":%llu}",
                          t, dt, unixNow(), m_unix0, final ? "true" : "false",
                          static_cast<unsigned long long>(m_cum.submitted), static_cast<unsigned long long>(drawn),
                          static_cast<unsigned long long>(m_cum.direct), static_cast<unsigned long long>(m_cum.dropped),
                          static_cast<unsigned long long>(m_cum.kickedPrims), static_cast<long long>(pending),
                          static_cast<unsigned long long>(m_iv.submit), static_cast<unsigned long long>(m_iv.area),
                          static_cast<unsigned long long>(m_iv.queued), static_cast<unsigned long long>(m_iv.direct),
                          static_cast<unsigned long long>(m_iv.dropped), static_cast<unsigned long long>(m_iv.empty),
                          static_cast<unsigned long long>(drawn - m_drawnAtLast), static_cast<unsigned long long>(m_iv.marks),
                          static_cast<unsigned long long>(m_iv.presents), static_cast<unsigned long long>(m_iv.kicks),
                          static_cast<unsigned long long>(m_iv.nanos));
            out += buf;
            out += ",\"tab\":{";
            bool firstTable = true;
            for (unsigned i = 0; i < kCensusTables; ++i)
            {
                if (m_tables[i].empty())
                    continue;
                out += firstTable ? "\"" : ",\"";
                firstTable = false;
                out += kCensusTableNames[i];
                out += "\":[";
                bool firstRow = true;
                for (const auto &row : m_tables[i])
                {
                    std::snprintf(buf, sizeof(buf), "%s[%llu,%llu,%llu]", firstRow ? "" : ",",
                                  static_cast<unsigned long long>(row.first), static_cast<unsigned long long>(row.second.n),
                                  static_cast<unsigned long long>(row.second.area));
                    firstRow = false;
                    out += buf;
                }
                out += "]";
                m_tables[i].clear();
            }
            out += "},\"present\":[";
            bool firstPresent = true;
            for (const auto &row : m_present)
            {
                const PresentEntry &e = row.second;
                std::snprintf(buf, sizeof(buf), "%s[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]", firstPresent ? "" : ",",
                              static_cast<unsigned long long>(e.regs[0]), static_cast<unsigned long long>(e.regs[1]),
                              static_cast<unsigned long long>(e.regs[2]), static_cast<unsigned long long>(e.regs[3]),
                              static_cast<unsigned long long>(e.regs[4]), static_cast<unsigned long long>(e.regs[5]),
                              static_cast<unsigned long long>(e.marks), static_cast<unsigned long long>(e.presents));
                firstPresent = false;
                out += buf;
            }
            m_present.clear();
            out += "],\"new_states\":[";
            for (size_t i = 0; i < m_newStates.size(); ++i)
            {
                if (i != 0)
                    out += ",";
                out += m_newStates[i];
            }
            m_newStates.clear();
            out += "],\"ex\":[";
            for (size_t i = 0; i < m_examples.size(); ++i)
            {
                if (i != 0)
                    out += ",";
                out += m_examples[i];
            }
            m_examples.clear();
            out += "]}\n";
            std::fwrite(out.data(), 1, out.size(), m_file);
            std::fflush(m_file);
            m_iv = Interval{};
            m_drawnAtLast = drawn;
            m_last = now;
            if (final)
                m_finalDone = true;
        }

        std::mutex m_lock;
        std::FILE *m_file = nullptr;
        const void *m_owner = nullptr;
        std::chrono::steady_clock::time_point m_t0{}, m_last{};
        double m_unix0 = 0.0;
        double m_interval = kCensusDefaultInterval;
        bool m_finalDone = false;
        uint32_t m_tickCounter = 0;
        uint32_t m_seq = 0;
        uint64_t m_primsSinceMark = 0;
        uint64_t m_uploadKey = 0;
        uint64_t m_drawnAtLast = 0;
        Interval m_iv{};
        Cumulative m_cum{};
        CensusTable m_tables[kCensusTables];
        bool m_hasPending = false;
        uint64_t m_pendKeys[kCensusStateTables] = {};
        uint64_t m_pendN = 0, m_pendArea = 0;
        std::unordered_set<uint64_t> m_seenFull, m_seenCoarse;
        std::vector<std::string> m_newStates, m_examples;
        std::unordered_map<uint64_t, uint8_t> m_exampleCounts;
        uint32_t m_categoryExamples[kCensusTables] = {};
        uint32_t m_exampleBucket = 0;
        std::unordered_map<uint64_t, PresentEntry> m_present;
        std::unordered_map<uint64_t, bool> m_fzCache;
        CensusPage m_pages[kCensusPages];
    };
}

GSCpuBackend::GSCpuBackend()
{
    using namespace GSMem;
    static std::once_flag lookupTablesOnce;
    std::call_once(lookupTablesOnce, []()
                   { InitLookupTables(); });
    for (size_t i = 0; i < kPsmHandlerCount; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_readVramFuncs[i] = ReadCT32;
            m_writeVramFuncs[i] = WriteCT32;
            break;
        case GS_PSM_CT24:
            m_readVramFuncs[i] = ReadCT24;
            m_writeVramFuncs[i] = WriteCT24;
            break;
        case GS_PSM_CT16:
            m_readVramFuncs[i] = ReadCT16;
            m_writeVramFuncs[i] = WriteCT16;
            break;
        case GS_PSM_CT16S:
            m_readVramFuncs[i] = ReadCT16S;
            m_writeVramFuncs[i] = WriteCT16S;
            break;
        case GS_PSM_T8:
            m_readVramFuncs[i] = ReadP8;
            m_writeVramFuncs[i] = WriteP8;
            break;
        case GS_PSM_T8H:
            m_readVramFuncs[i] = ReadP8H;
            m_writeVramFuncs[i] = WriteP8H;
            break;
        case GS_PSM_T4:
            m_readVramFuncs[i] = ReadP4;
            m_writeVramFuncs[i] = WriteP4;
            break;
        case GS_PSM_T4HH:
            m_readVramFuncs[i] = ReadP4HH;
            m_writeVramFuncs[i] = WriteP4HH;
            break;
        case GS_PSM_T4HL:
            m_readVramFuncs[i] = ReadP4HL;
            m_writeVramFuncs[i] = WriteP4HL;
            break;
        case GS_PSM_Z32:
            m_readVramFuncs[i] = ReadZ32;
            m_writeVramFuncs[i] = WriteZ32;
            break;
        case GS_PSM_Z24:
            m_readVramFuncs[i] = ReadZ24;
            m_writeVramFuncs[i] = WriteZ24;
            break;
        case GS_PSM_Z16:
            m_readVramFuncs[i] = ReadZ16;
            m_writeVramFuncs[i] = WriteZ16;
            break;
        case GS_PSM_Z16S:
            m_readVramFuncs[i] = ReadZ16S;
            m_writeVramFuncs[i] = WriteZ16S;
            break;
        default:
            m_readVramFuncs[i] = ReadNull;
            m_writeVramFuncs[i] = WriteNull;
            break;
        }
    }
    Reset();
}

void GSCpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    if (vram && vramSize < GSMem::MEMORY_SIZE)
        throw std::invalid_argument("GS CPU backend requires at least 4 MiB of VRAM");

    std::lock_guard<std::mutex> lock(m_mutex);
    FlushQueueUnlocked();
    m_vram = vram;
    m_vramSize = vramSize;
    ResetUnlocked();
}

void GSCpuBackend::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    FlushQueueUnlocked();
    ResetUnlocked();
}

void GSCpuBackend::ResetUnlocked()
{
    m_clut.fill(0u);
    m_clutCbp.fill(0u);
    m_texturePageCache.Invalidate();
    m_transfer = {};
    m_transfer.direction = 3u;
    m_transferState = {};
    m_transferState.direction = 3u;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
}

namespace
{
    constexpr size_t kMaxQueuedPrimitives = 16384u;
}

void GSCpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || batch.vertexCount == 0u)
        return;
    const bool census = gsCensusEnabled();
    if (census)
        GsCensus::instance().primitive(this, batch);
    if (QueuePrimitive(batch))
    {
        if (census)
            GsCensus::instance().path(this, true, m_pending.prims.size());
        if (m_pending.prims.size() >= kMaxQueuedPrimitives)
            KickQueueUnlocked();
        return;
    }
    if (census)
        GsCensus::instance().path(this, false, m_pending.prims.size());
    FlushQueueUnlocked();
    DrawPrimitive(batch);
}

thread_local GSCpuBackend::BandContext *GSCpuBackend::t_band = nullptr;
thread_local const uint16_t *GSCpuBackend::t_clut = nullptr;

namespace
{
    bool deferredDrawingEnabled()
    {
        static const bool enabled = []
        {
            const char *env = std::getenv("PS2X_GS_DEFER");
            return !(env && env[0] == '0');
        }();
        return enabled;
    }

    // Page size in pixels for a pixel storage format (8 KiB pages).
    void psmPageSize(uint32_t psm, uint32_t &width, uint32_t &height)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            width = 64u;
            height = 64u;
            return;
        case GS_PSM_T8:
            width = 128u;
            height = 64u;
            return;
        case GS_PSM_T4:
            width = 128u;
            height = 128u;
            return;
        default: // 32-bit layouts, including T8H/T4HL/T4HH
            width = 64u;
            height = 32u;
            return;
        }
    }

    // Bytes spanned by a buffer of `width` x `height` pixels starting at `baseBytes`. When the
    // base is not page aligned (textures), the span is extended by one page.
    template <typename Range>
    void addBufferRange(std::vector<Range> &out, uint32_t baseBytes, uint32_t width, uint32_t height,
                        uint32_t psm, uint32_t key)
    {
        constexpr uint32_t kPage = GSMem::TexturePageCache::kPageSize;
        uint32_t pageW, pageH;
        psmPageSize(psm, pageW, pageH);
        const uint32_t pagesX = std::max(1u, (width + pageW - 1u) / pageW);
        const uint32_t pagesY = std::max(1u, (height + pageH - 1u) / pageH);
        const uint32_t begin = baseBytes & ~(kPage - 1u);
        uint64_t end = static_cast<uint64_t>(begin) + static_cast<uint64_t>(pagesX) * pagesY * kPage;
        if ((baseBytes & (kPage - 1u)) != 0u)
            end += kPage;
        const uint64_t memSize = GSMem::MEMORY_SIZE;
        if (begin >= memSize)
            return;
        if (end > memSize)
        {
            out.push_back({begin, static_cast<uint32_t>(memSize), key});
            out.push_back({0u, static_cast<uint32_t>(std::min<uint64_t>(end - memSize, memSize)), key});
            return;
        }
        out.push_back({begin, static_cast<uint32_t>(end), key});
    }

    template <typename Range>
    bool rangesOverlap(const Range &a, const Range &b)
    {
        return a.begin < b.end && b.begin < a.end;
    }
}

bool GSCpuBackend::QueuePrimitive(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    const bool sprite = state.prim.type == GS_PRIM_SPRITE;
    const bool triangle = state.prim.type == GS_PRIM_TRIANGLE || state.prim.type == GS_PRIM_TRISTRIP ||
                          state.prim.type == GS_PRIM_TRIFAN;
    if (!deferredDrawingEnabled() || RasterPool::instance().workerCount() == 0u || (!sprite && !triangle))
        return false;

    // Rows the primitive can touch; a superset is fine, each thread clips to its own rows.
    const float ofy = static_cast<float>(ctx.xyoffset.ofy >> 4);
    const int vertexCount = sprite ? 2 : 3;
    float minYf = batch.vertices[0].y, maxYf = batch.vertices[0].y;
    for (int i = 1; i < vertexCount; ++i)
    {
        minYf = std::min(minYf, batch.vertices[i].y);
        maxYf = std::max(maxYf, batch.vertices[i].y);
    }
    if (!std::isfinite(minYf) || !std::isfinite(maxYf))
        return false;
    const int minY = std::max(static_cast<int>(std::floor(minYf - ofy)) - 1, static_cast<int>(ctx.scissor.y0));
    const int maxY = std::min(static_cast<int>(std::ceil(maxYf - ofy)) + 1, static_cast<int>(ctx.scissor.y1));
    if (minY > maxY)
    {
        if (gsCensusEnabled())
            GsCensus::instance().dropped(this);
        return true; // outside the scissor rows: draws nothing
    }

    // VRAM this primitive writes (frame, depth) and reads (texture).
    thread_local std::vector<VramRange> writes, reads;
    writes.clear();
    reads.clear();
    const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
    const uint32_t rows = static_cast<uint32_t>(ctx.scissor.y1) + 1u;
    if (ctx.frame.fbmsk != 0xFFFFFFFFu)
        addBufferRange(writes, ctx.frame.fbp * GSMem::TexturePageCache::kPageSize, fbw * 64u, rows, ctx.frame.psm,
                       0x80000000u | ((ctx.frame.fbp & 0x1FFu) << 14) | ((fbw & 0x3Fu) << 6) | (ctx.frame.psm & 0x3Fu));
    if (!ctx.zbuf.zmask)
        addBufferRange(writes, ctx.zbuf.zbp * GSMem::TexturePageCache::kPageSize, fbw * 64u, rows, ctx.zbuf.psm,
                       0x80000000u | ((ctx.zbuf.zbp & 0x1FFu) << 14) | ((fbw & 0x3Fu) << 6) | (ctx.zbuf.psm & 0x3Fu));
    if (state.prim.tme)
    {
        const auto &tex = ctx.tex0;
        const uint32_t texW = 1u << std::min<uint32_t>(tex.tw, 10u);
        const uint32_t texH = 1u << std::min<uint32_t>(tex.th, 10u);
        addBufferRange(reads, tex.tbp0 * 256u, std::max<uint32_t>(tex.tbw * 64u, texW), texH, tex.psm, 0u);
    }

    bool selfOverlap = false;
    for (size_t i = 0; i < writes.size(); ++i)
    {
        for (size_t j = i + 1; j < writes.size(); ++j)
            selfOverlap |= writes[i].key != writes[j].key && rangesOverlap(writes[i], writes[j]);
        for (const VramRange &r : reads)
            selfOverlap |= rangesOverlap(writes[i], r);
    }
    if (selfOverlap)
        return false; // feedback or aliasing within one primitive: draw it on its own, as before

    bool hazard = false;
    for (const VramRange &w : writes)
    {
        for (const VramRange &q : m_pending.writes)
            hazard |= q.key != w.key && rangesOverlap(q, w);
        for (const VramRange &q : m_pending.reads)
            hazard |= rangesOverlap(q, w);
    }
    for (const VramRange &r : reads)
        for (const VramRange &q : m_pending.writes)
            hazard |= rangesOverlap(q, r);
    if (hazard)
        KickQueueUnlocked();

    auto remember = [](std::vector<VramRange> &list, const VramRange &range)
    {
        for (const VramRange &existing : list)
            if (existing.begin == range.begin && existing.end == range.end && existing.key == range.key)
                return;
        list.push_back(range);
    };
    for (const VramRange &w : writes)
        remember(m_pending.writes, w);
    for (const VramRange &r : reads)
        remember(m_pending.reads, r);
    if (m_pending.cluts.empty() || m_clutChanged)
    {
        m_pending.cluts.push_back(m_clut);
        m_clutChanged = false;
    }
    m_pending.prims.push_back({batch, minY, maxY, static_cast<uint32_t>(m_pending.cluts.size() - 1u), m_queueEpoch});
    return true;
}

void GSCpuBackend::KickQueueUnlocked()
{
    if (m_pending.prims.empty())
        return;
    if (gsCensusEnabled())
        GsCensus::instance().kick(this, m_pending.prims.size());

    // Give the batch its own copy of every page it reads, so uploads after this point need not
    // wait for it. Pages that batches in flight still write (render to texture) are copied once
    // those batches are done.
    WaitIfInFlight(m_pending.reads, false);
    constexpr uint32_t kPage = GSMem::TexturePageCache::kPageSize;
    for (const VramRange &r : m_pending.reads)
        for (uint32_t page = r.begin / kPage; page * kPage < r.end; ++page)
        {
            std::vector<SavedPage> &history = m_pending.savedPages[page];
            if (history.empty() || history.back().epoch != m_queueEpoch)
                history.push_back({m_queueEpoch, std::vector<uint8_t>(m_vram + page * kPage, m_vram + (page + 1u) * kPage)});
        }

    auto batch = std::make_unique<Batch>(std::move(m_pending));
    m_pending = Batch{};
    m_clutChanged = true;
    m_queueEpoch = 0u;
    m_queueSizeAtTransfer = 0u;
    {
        std::lock_guard<std::mutex> lock(m_drawMutex);
        if (!m_drawer.joinable())
            m_drawer = std::thread([this] { DrawerMain(); });
        m_inFlight.push_back(std::move(batch));
        ++m_batchesKicked;
    }
    m_drawWake.notify_one();
}

void GSCpuBackend::MarkPresentation(const GSPresentationRequest &request)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram)
        return;
    if (gsCensusEnabled())
        GsCensus::instance().present(this, request, true, m_pending.prims.size());
    KickQueueUnlocked();
    auto marker = std::make_unique<Batch>();
    marker->present = true;
    marker->request = request;
    // The displayed buffers count as read until the copy is taken, so an upload into them waits.
    const auto addDisplay = [&](uint64_t dispfb, uint64_t display)
    {
        const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
        const uint32_t fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
        const uint32_t psm = static_cast<uint32_t>((dispfb >> 15) & 0x1Fu);
        const uint32_t height = static_cast<uint32_t>((display >> 44) & 0x7FFu) + 1u;
        addBufferRange(marker->reads, fbp * 8192u, std::max(fbw, 1u) * 64u, height, psm, 0u);
    };
    addDisplay(request.dispfb1, request.display1);
    addDisplay(request.dispfb2, request.display2);
    {
        std::lock_guard<std::mutex> drawLock(m_drawMutex);
        if (!m_drawer.joinable())
            m_drawer = std::thread([this] { DrawerMain(); });
        m_inFlight.push_back(std::move(marker));
        ++m_batchesKicked;
    }
    m_drawWake.notify_one();
}

uint64_t GSCpuBackend::KickedBatches() const
{
    std::lock_guard<std::mutex> lock(m_drawMutex);
    return m_batchesKicked;
}

void GSCpuBackend::WaitForBatches(uint64_t count) const
{
    std::unique_lock<std::mutex> lock(m_drawMutex);
    m_drawDone.wait(lock, [&] { return m_batchesDrawn >= count; });
}

void GSCpuBackend::FlushQueueUnlocked()
{
    KickQueueUnlocked();
    WaitForBatches(KickedBatches());
}

void GSCpuBackend::WaitIfInFlight(const std::vector<VramRange> &ranges, bool includeReads)
{
    bool overlap = false;
    {
        std::lock_guard<std::mutex> lock(m_drawMutex);
        for (const std::unique_ptr<Batch> &batch : m_inFlight)
            for (const VramRange &range : ranges)
            {
                for (const VramRange &w : batch->writes)
                    overlap |= rangesOverlap(range, w);
                if (includeReads)
                    for (const VramRange &r : batch->reads)
                        overlap |= rangesOverlap(range, r);
            }
    }
    if (overlap)
        WaitForBatches(KickedBatches());
}

void GSCpuBackend::DrawerMain()
{
    for (;;)
    {
        Batch *batch = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_drawMutex);
            m_drawWake.wait(lock, [&] { return m_drawerStop || !m_inFlight.empty(); });
            if (m_inFlight.empty())
                return;
            batch = m_inFlight.front().get();
        }
        if (batch->present)
        {
            m_presentScratch.assign(m_vram, m_vram + m_vramSize);
            std::lock_guard<std::mutex> lock(m_presentMutex);
            m_presentVram.swap(m_presentScratch);
            m_presentRequest = batch->request;
            m_hasPresentVram = true;
        }
        else
        {
            DrawBatch(*batch);
        }
        {
            std::lock_guard<std::mutex> lock(m_drawMutex);
            m_inFlight.pop_front();
            ++m_batchesDrawn;
        }
        m_drawDone.notify_all();
    }
}

GSCpuBackend::~GSCpuBackend()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        KickQueueUnlocked();
    }
    WaitForBatches(KickedBatches());
    if (gsCensusEnabled())
        GsCensus::instance().finalize(this);
    {
        std::lock_guard<std::mutex> lock(m_drawMutex);
        m_drawerStop = true;
    }
    m_drawWake.notify_all();
    if (m_drawer.joinable())
        m_drawer.join();
}

void GSCpuBackend::DrawBatch(const Batch &batch)
{
    if (gsCensusEnabled())
        s_censusDrawnInBatches.fetch_add(batch.prims.size(), std::memory_order_relaxed);
    RasterPool &pool = RasterPool::instance();
    const size_t slots = pool.workerCount() + 1u;
    if (m_workerPageCaches.size() < slots)
        m_workerPageCaches.resize(slots);
    if (m_workerTexelCaches.size() < slots)
        m_workerTexelCaches.resize(slots);

    int firstRow = batch.prims.front().minY;
    int lastRow = batch.prims.front().maxY;
    for (const QueuedPrimitive &q : batch.prims)
    {
        firstRow = std::min(firstRow, q.minY);
        lastRow = std::max(lastRow, q.maxY);
    }

    const bool savedPages = !batch.savedPages.empty();
    pool.run(firstRow, lastRow, [&](int bandFirst, int bandLast, unsigned slot)
             {
        GSMem::TexturePageCache &pageCache = m_workerPageCaches[slot];
        BandContext band{bandFirst, bandLast, &pageCache, &m_workerTexelCaches[slot], &batch, 0u};
        pageCache.SetSource(savedPages ? &SavedPageSource : nullptr, &band);
        t_band = &band;
        for (const QueuedPrimitive &q : batch.prims)
        {
            if (q.maxY < bandFirst || q.minY > bandLast)
                continue;
            if (q.epoch != band.epoch)
            {
                band.epoch = q.epoch;
                pageCache.Invalidate();
            }
            t_clut = batch.cluts[q.clut].data();
            DrawPrimitive(q.batch);
        }
        t_clut = nullptr;
        t_band = nullptr;
        pageCache.SetSource(nullptr, nullptr); });
}

const uint8_t *GSCpuBackend::SavedPageSource(const void *context, uint32_t pageBase)
{
    // The content a primitive of this epoch saw is the oldest copy saved at or after its epoch.
    const BandContext &band = *static_cast<const BandContext *>(context);
    const auto found = band.batch->savedPages.find(pageBase / GSMem::TexturePageCache::kPageSize);
    if (found == band.batch->savedPages.end())
        return nullptr;
    for (const SavedPage &saved : found->second)
        if (saved.epoch >= band.epoch)
            return saved.bytes.data();
    return nullptr;
}

void GSCpuBackend::CollectUploadRange(const GSTransferCommand &command, std::vector<VramRange> &out) const
{
    const auto &buf = command.bitbltbuf;
    addBufferRange(out, buf.dbp * 256u,
                   std::max<uint32_t>(std::max<uint32_t>(buf.dbw, 1u) * 64u, command.trxpos.dsax + command.trxreg.rrw),
                   command.trxpos.dsay + command.trxreg.rrh, buf.dpsm, 0u);
}

bool GSCpuBackend::PrepareUpload(const GSTransferCommand &command)
{
    // Before a host-to-GS upload. Waits for batches being drawn that touch the destination.
    // Returns true when the queue had to be flushed; otherwise saves the pages queued primitives
    // read that this upload will overwrite, and starts a new epoch.
    thread_local std::vector<VramRange> dest;
    dest.clear();
    CollectUploadRange(command, dest);
    WaitIfInFlight(dest, false);
    if (m_pending.prims.empty())
        return false;
    for (const VramRange &d : dest)
        for (const VramRange &w : m_pending.writes)
            if (rangesOverlap(d, w))
            {
                FlushQueueUnlocked();
                return true;
            }

    constexpr uint32_t kPage = GSMem::TexturePageCache::kPageSize;
    bool saved = false;
    for (const VramRange &d : dest)
    {
        for (uint32_t page = d.begin / kPage; page * kPage < d.end; ++page)
        {
            const VramRange pageRange{page * kPage, (page + 1u) * kPage, 0u};
            bool read = false;
            for (const VramRange &r : m_pending.reads)
                read |= rangesOverlap(pageRange, r);
            if (!read)
                continue;
            std::vector<SavedPage> &history = m_pending.savedPages[page];
            if (!history.empty() && history.back().epoch == m_queueEpoch)
                continue; // already saved for this epoch
            history.push_back({m_queueEpoch, std::vector<uint8_t>(m_vram + page * kPage, m_vram + (page + 1u) * kPage)});
            saved = true;
        }
    }
    if (saved)
        ++m_queueEpoch;
    m_queueSizeAtTransfer = m_pending.prims.size();
    return false;
}

void GSCpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || (!isFourBitIndexedPsm(tex0.psm) && !isEightBitIndexedPsm(tex0.psm)))
        return;
    if (gsCensusEnabled())
        GsCensus::instance().clutCall(this, tex0);

    switch (tex0.cld)
    {
    case 0u:
    case 6u:
    case 7u:
        return;
    case 1u:
        break;
    case 2u:
        m_clutCbp[0] = tex0.cbp;
        break;
    case 3u:
        m_clutCbp[1] = tex0.cbp;
        break;
    case 4u:
        if (m_clutCbp[0] == tex0.cbp)
            return;
        m_clutCbp[0] = tex0.cbp;
        break;
    case 5u:
        if (m_clutCbp[1] == tex0.cbp)
            return;
        m_clutCbp[1] = tex0.cbp;
        break;
    default:
        return;
    }

    {
        // Queued primitives keep the CLUT they were queued with (Batch::cluts), so a load only
        // has to wait for primitives that write the memory it reads.
        thread_local std::vector<VramRange> source;
        source.clear();
        addBufferRange(source, tex0.cbp * 256u, std::max<uint32_t>(texclut.cbw, 1u) * 64u,
                       static_cast<uint32_t>(texclut.cov) + 16u, tex0.cpsm, 0u);
        WaitIfInFlight(source, false);
        bool overlap = false;
        for (const VramRange &r : source)
            for (const VramRange &w : m_pending.writes)
                overlap |= rangesOverlap(r, w);
        if (overlap)
            FlushQueueUnlocked();
    }
    if (gsCensusEnabled())
        GsCensus::instance().clutLoad(this, tex0, texclut);
    LoadClutUnlocked(tex0, texclut);
    m_clutChanged = true;
}

void GSCpuBackend::LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const bool fourBit = isFourBitIndexedPsm(tex0.psm);
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
    if (!sixteenBit && !thirtyTwoBit)
        return;

    const uint32_t entryCount = fourBit ? 16u : 256u;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;

    const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
    const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;

    for (uint32_t entry = firstEntry; entry < entryCount; ++entry)
    {
        uint32_t sourceX = 0u;
        uint32_t sourceY = 0u;
        uint32_t sourceWidth = 1u;

        if (tex0.csm == 0u)
        {
            const uint32_t sourceIndex = swizzleClutIndexCSM1(entry);
            sourceX = sourceIndex & 0x0Fu;
            sourceY = sourceIndex >> 4u;
        }
        else
        {
            sourceWidth = texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            sourceX = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
            sourceY = static_cast<uint32_t>(texclut.cov);
        }

        const uint32_t raw = ReadTextureVramUnlocked(tex0.cpsm, tex0.cbp, sourceWidth, sourceX, sourceY); 
        const uint32_t destination = (loadCsm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (sixteenBit)
        {
            m_clut[destination] = static_cast<uint16_t>(raw);
        }
        else
        {
            m_clut[destination] = static_cast<uint16_t>(raw & 0xFFFFu);
            m_clut[destination + 256u] = static_cast<uint16_t>(raw >> 16u);
        }
    }
}

void GSCpuBackend::Flush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    KickQueueUnlocked();
}

void GSCpuBackend::TextureFlush()
{
    // Queued primitives need no flush here: their texture reads are already ordered against
    // writes by the overlap checks, and raster threads start each flush with empty page caches.
    std::lock_guard<std::mutex> lock(m_mutex);
    m_texturePageCache.Invalidate();
}

void GSCpuBackend::Sync(GSSyncReason reason)
{
    // FINISH needs no wait: anything that reads the result waits for it. Other syncs wait without
    // holding m_mutex, so the EE can keep queuing meanwhile.
    uint64_t kicked;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        KickQueueUnlocked();
        if (reason == GSSyncReason::Finish)
            return;
        kicked = KickedBatches();
    }
    WaitForBatches(kicked);
}

uint32_t GSCpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (gsCensusEnabled())
        GsCensus::instance().direct(this, false, psm, base, bw, x, y);
    const_cast<GSCpuBackend *>(this)->FlushQueueUnlocked();
    return ReadVramUnlocked(psm, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (!m_vram)
        return 0u;
    return m_readVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
{
    if (!m_vram)
        return 0u;

    return GSMem::ReadTexture(m_texturePageCache, m_vram, psm, base, bw, x, y);
}

void GSCpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (gsCensusEnabled())
        GsCensus::instance().direct(this, true, psm, base, bw, x, y);
    FlushQueueUnlocked();
    WriteVramUnlocked(psm, base, bw, x, y, value);
}

void GSCpuBackend::WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (!m_vram)
        return;
    m_writeVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y, value);
}

void GSCpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    // Waits only for what was queued before the call; later batches may be drawing into other
    // buffers while the copy is taken.
    uint64_t kicked;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const_cast<GSCpuBackend *>(this)->KickQueueUnlocked();
        kicked = KickedBatches();
    }
    WaitForBatches(kicked);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || m_vramSize == 0u)
    {
        out.clear();
        return;
    }
    out.resize(m_vramSize);
    std::memcpy(out.data(), m_vram, m_vramSize);
}

GSTransferSnapshot GSCpuBackend::GetTransferSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    GSTransferSnapshot result = m_transferState;
    result.localToHostPendingBytes = m_localToHostReadPos < m_localToHostBuffer.size()
                                         ? m_localToHostBuffer.size() - m_localToHostReadPos
                                         : 0u;
    return result;
}

void GSCpuBackend::DrawPrimitive(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex < 64u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << ")"
                      << " uv0=(" << (batch.vertices[0].u >> 4) << "," << (batch.vertices[0].v >> 4) << ")"
                      << " stq0=(" << batch.vertices[0].s << "," << batch.vertices[0].t << "," << batch.vertices[0].q << ")"
                      << " v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << ")"
                      << " uv1=(" << (batch.vertices[1].u >> 4) << "," << (batch.vertices[1].v >> 4) << ")"
                      << " stq1=(" << batch.vertices[1].s << "," << batch.vertices[1].t << "," << batch.vertices[1].q << ")"
                      << " v2=(" << batch.vertices[2].x << "," << batch.vertices[2].y << ")"
                      << " uv2=(" << (batch.vertices[2].u >> 4) << "," << (batch.vertices[2].v >> 4) << ")"
                      << " stq2=(" << batch.vertices[2].s << "," << batch.vertices[2].t << "," << batch.vertices[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(batch.vertices[0].r) << ","
                      << static_cast<uint32_t>(batch.vertices[0].g) << ","
                      << static_cast<uint32_t>(batch.vertices[0].b) << ","
                      << static_cast<uint32_t>(batch.vertices[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(batch.vertices[1].r) << ","
                      << static_cast<uint32_t>(batch.vertices[1].g) << ","
                      << static_cast<uint32_t>(batch.vertices[1].b) << ","
                      << static_cast<uint32_t>(batch.vertices[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(batch.vertices[2].r) << ","
                      << static_cast<uint32_t>(batch.vertices[2].g) << ","
                      << static_cast<uint32_t>(batch.vertices[2].b) << ","
                      << static_cast<uint32_t>(batch.vertices[2].a) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((state.prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });

    switch (state.prim.type)
    {
    case GS_PRIM_SPRITE:
        DrawSprite(batch);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        DrawTriangle(batch);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        DrawLine(batch);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = batch.vertices[0];
        const auto &ctx = state.context;
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        WritePixel(state, px, py, static_cast<u32>(v.z), v.r, v.g, v.b, v.a, v.fog);
        break;
    }
    default:
        break;
    }
}

void GSCpuBackend::WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog)
{
    const auto &ctx = state.context;
    if (x < ctx.scissor.x0 || x > ctx.scissor.x1 || y < ctx.scissor.y0 || y > ctx.scissor.y1)
        return;

    if (state.prim.fge)
    {
        const uint32_t inverseFog = 255u - fog;
        auto applyFog = [&](uint8_t input, uint8_t fogColor) -> uint8_t
        {
            return static_cast<uint8_t>(((static_cast<uint32_t>(fog) * input) >> 8) + ((inverseFog * fogColor) >> 8));
        };

        r = applyFog(r, state.fogR);
        g = applyFog(g, state.fogG);
        b = applyFog(b, state.fogB);
    }

    const u32 fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const u32 fbw = std::max<u32>(ctx.frame.fbw, 1u);
    const u32 fpsm = ctx.frame.psm;
    const u32 zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const u32 zpsm = ctx.zbuf.psm;

    const PixelWriteMask writeMask = classifyAlphaTest(ctx.test, a, static_cast<uint8_t>(fpsm));
    if (!writeMask.writesAnything())
    {
        return;
    }

    // TEST.ZTE (bit 16) = 0 turns the depth test off: every pixel passes, as with ZTST = ALWAYS
    // (PCSX2 does the same). FPR draws its in-match HUD with ZTE = 0 and ZTST = GEQUAL.
    const bool depthTestEnabled = ((ctx.test >> 16) & 1u) != 0u;
    const uint32_t ztestMethod = depthTestEnabled ? static_cast<uint32_t>((ctx.test >> 17) & 3u) : 1u;
    const bool alphaBlendEnabled = state.prim.abe;
    const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && fpsm == GS_PSM_CT32;
    const bool destinationAlphaTestNeedsRead = ((ctx.test >> 14) & 0x1u) != 0u && (fpsm == GS_PSM_CT32 || fpsm == GS_PSM_CT16 || fpsm == GS_PSM_CT16S);

    // small optimization, avoid reading the framebuffer for simple draws
    // TODO: only one address lookup for rmw
    const bool frmw = destinationAlphaTestNeedsRead || (writeMask.writesFramebuffer() && ((ctx.frame.fbmsk != 0) || alphaBlendEnabled || preserveDestinationAlpha));

    u32 rawFramebufferPixel = 0;
    u32 fbrgba = 0;
    if (frmw)
    {
        rawFramebufferPixel = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
        fbrgba = rawFramebufferPixel;

        if (bitsPerPixel(fpsm) == 16)
        {
            fbrgba = Rgba5551ToRgba8888(fbrgba);
        }
        else if (fpsm == GS_PSM_CT24)
        {
            // The GS supplies 0x80 as destination alpha for RGB24 blending.
            fbrgba |= 0x80000000u;
        }
    }

    if (!passesDestinationAlphaTest(ctx.test, static_cast<uint8_t>(fpsm), rawFramebufferPixel))
    {
        return;
    }

    bool zpass = false;
    uint32_t storedZ = 0u;
    switch (ztestMethod)
    {
    case 0:
        zpass = false;
        break;
    case 1:
        zpass = true;
        break;
    case 2:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) >= storedZ;
        break;
    case 3:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) > storedZ;
        break;
    }

    if (!zpass)
    {
        return;
    }

    if (writeMask.writesFramebuffer())
    {
        const u8 srcR = r;
        const u8 srcG = g;
        const u8 srcB = b;

        if (state.prim.abe)
        {
            uint8_t dr = fbrgba & 0xFF;
            uint8_t dg = (fbrgba >> 8) & 0xFF;
            uint8_t db = (fbrgba >> 16) & 0xFF;
            uint8_t da = (fbrgba >> 24) & 0xFF;

            // PABE disables alpha blending when the source alpha MSB is clear.
            if (!(state.pabe && (a & 0x80u) == 0u))
            {
                uint64_t alphaReg = ctx.alpha;
                uint8_t asel = alphaReg & 3;
                uint8_t bsel = (alphaReg >> 2) & 3;
                uint8_t csel = (alphaReg >> 4) & 3;
                uint8_t dsel = (alphaReg >> 6) & 3;
                uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

                auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
                {
                    if (sel == 0)
                        return cs;
                    if (sel == 1)
                        return cd;
                    return 0;
                };
                int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                           : fix;

                r = clampU8(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr));
                g = clampU8(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg));
                b = clampU8(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db));
            }
            else
            {
                r = srcR;
                g = srcG;
                b = srcB;
            }
        }

        if (writeMask.writeAlpha && (ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24)
        {
            a = static_cast<uint8_t>(a | 0x80u);
        }

        u32 pixel = pack32(r, g, b, a);

        if (ctx.frame.fbmsk != 0)
        {
            pixel = (pixel & ~ctx.frame.fbmsk) | (fbrgba & ctx.frame.fbmsk);
        }

        if (preserveDestinationAlpha)
        {
            pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
        }

        // format conversion
        if (bitsPerPixel(fpsm) == 16)
        {
            pixel = Rgba8888ToRgba5551(pixel);
        }

        WriteVramUnlocked(fpsm, fbp, fbw, x, y, pixel);
    }

    if (writeMask.writeDepth && !ctx.zbuf.zmask)
    {
        WriteVramUnlocked(zpsm, zbp, fbw, x, y, z);
    }
}

uint32_t GSCpuBackend::LookupCLUT(const GSDrawState &state,
                                  uint8_t index,
                                  uint8_t cpsm,
                                  uint8_t csm,
                                  uint8_t csa,
                                  uint8_t sourcePsm)
{
    const bool sixteenBit = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t clutBase = (static_cast<uint32_t>(csa) & csaMask) << 4u;
    const uint32_t sourceIndex = isFourBitIndexedPsm(sourcePsm)
                                     ? (static_cast<uint32_t>(index) & 0x0Fu)
                                     : static_cast<uint32_t>(index);

    const uint16_t *clut = t_clut ? t_clut : m_clut.data();
    uint32_t clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
    if (!sixteenBit && csm == 0u && isEightBitIndexedPsm(sourcePsm))
    {
        const uint32_t block = std::min((sourceIndex & 0xF0u) + clutBase, 240u);
        clutIndex = block + (sourceIndex & 0x0Fu);
    }

    switch (cpsm)
    {
    case GS_PSM_CT32:
    {
        const uint32_t raw = static_cast<uint32_t>(clut[clutIndex]) | (static_cast<uint32_t>(clut[clutIndex + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, raw);
    }
    case GS_PSM_CT24:
    {
        const uint32_t raw = static_cast<uint32_t>(clut[clutIndex]) | (static_cast<uint32_t>(clut[clutIndex + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, raw & 0x00FFFFFFu);
    }
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        return applyTexa(state.texa, cpsm, Rgba5551ToRgba8888(clut[clutIndex]));
    default:
        break;
    }

    return 0xFFFF00FFu;
}

uint32_t GSCpuBackend::DecodeTexel(const GSDrawState &state, int u, int v, GSMem::TexturePageCache &pageCache)
{
    const auto &tex = state.context.tex0;
    const u32 out = m_vram ? GSMem::ReadTexture(pageCache, m_vram, tex.psm, tex.tbp0, tex.tbw, u, v) : 0u;

    switch (tex.psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_Z32:
    case GS_PSM_CT24:
    case GS_PSM_Z24:
        return applyTexa(state.texa, tex.psm, out);
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return applyTexa(state.texa, tex.psm, Rgba5551ToRgba8888(out));
    case GS_PSM_T8:
    case GS_PSM_T8H:
    case GS_PSM_T4:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        return LookupCLUT(state, static_cast<u8>(out), tex.cpsm, tex.csm, tex.csa, tex.psm);
    }

    return 0xFFFF00FFu;
}

uint32_t GSCpuBackend::FetchTexel(const GSDrawState &state, int u, int v, GSMem::TexturePageCache &pageCache, bool storeTexels)
{
    TexelCache &cache = ActiveTexelCache();
    if (!cache.active || u < 0 || v < 0 || u >= cache.width || v >= cache.height)
        return DecodeTexel(state, u, v, pageCache);

    const size_t index = static_cast<size_t>(v) * static_cast<size_t>(cache.width) + static_cast<size_t>(u);
    if (cache.stamps[index] == cache.generation)
        return cache.texels[index];

    const uint32_t texel = DecodeTexel(state, u, v, pageCache);
    if (storeTexels)
    {
        cache.texels[index] = texel;
        cache.stamps[index] = cache.generation;
    }
    return texel;
}

void GSCpuBackend::PrepareTexelCache(const GSPrimitiveBatch &batch, bool decodeFootprint)
{
    // The cache is valid for one primitive only: VRAM, CLUT and TEXA can change between them.
    const GSDrawState &state = batch.state;
    TexelCache &cache = ActiveTexelCache();
    cache.active = false;
    if (!state.prim.tme)
        return;

    const uint64_t clamp = state.context.clamp;
    const uint8_t wrapU = static_cast<uint8_t>(clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
    // REGION_CLAMP / REGION_REPEAT can produce coordinates outside the texture; sample those directly.
    if (wrapU >= 2u || wrapV >= 2u)
        return;

    const int texW = state.textureWidth;
    const int texH = state.textureHeight;
    if (texW <= 0 || texH <= 0)
        return;
    const size_t texelCount = static_cast<size_t>(texW) * static_cast<size_t>(texH);
    if (cache.texels.size() < texelCount)
    {
        cache.texels.resize(texelCount);
        cache.stamps.assign(texelCount, 0u);
        cache.generation = 0u;
    }
    if (++cache.generation == 0u)
    {
        std::fill(cache.stamps.begin(), cache.stamps.end(), 0u);
        cache.generation = 1u;
    }
    cache.width = texW;
    cache.height = texH;
    cache.active = true;

    if (!decodeFootprint)
        return; // texels are decoded on first use

    // Decode every texel the primitive can sample, so worker threads only read the cache.
    // Interpolated texel coordinates stay inside the range spanned by the vertices (for STQ, as
    // long as all Q have the same sign). The margin covers bilinear neighbours and rounding.
    const bool sprite = state.prim.type == GS_PRIM_SPRITE;
    const int vertexCount = sprite ? 2 : 3;
    float minU = 0.0f, maxU = 0.0f, minV = 0.0f, maxV = 0.0f;
    bool wholeTexture = false;
    for (int i = 0; i < vertexCount; ++i)
    {
        const GSVertex &vtx = batch.vertices[i];
        float u, v;
        if (state.prim.fst)
        {
            u = static_cast<float>(vtx.u) / 16.0f;
            v = static_cast<float>(vtx.v) / 16.0f;
        }
        else
        {
            if ((vtx.q < 0.0f) != (batch.vertices[0].q < 0.0f))
                wholeTexture = true;
            const float q = fabsQ(vtx.q);
            u = vtx.s / q * static_cast<float>(texW);
            v = vtx.t / q * static_cast<float>(texH);
        }
        if (!std::isfinite(u) || !std::isfinite(v))
            wholeTexture = true;
        minU = (i == 0) ? u : std::min(minU, u);
        maxU = (i == 0) ? u : std::max(maxU, u);
        minV = (i == 0) ? v : std::min(minV, v);
        maxV = (i == 0) ? v : std::max(maxV, v);
    }

    int u0 = 0, u1 = texW - 1, v0 = 0, v1 = texH - 1;
    if (!wholeTexture && maxU - minU < static_cast<float>(texW) && maxV - minV < static_cast<float>(texH))
    {
        u0 = static_cast<int>(std::floor(minU)) - 2;
        u1 = static_cast<int>(std::ceil(maxU)) + 2;
        v0 = static_cast<int>(std::floor(minV)) - 2;
        v1 = static_cast<int>(std::ceil(maxV)) + 2;
        if (u1 - u0 + 1 >= texW)
        {
            u0 = 0;
            u1 = texW - 1;
        }
        if (v1 - v0 + 1 >= texH)
        {
            v0 = 0;
            v1 = texH - 1;
        }
    }

    // CLAMP maps everything outside the texture onto its edge, so only the inside needs decoding.
    // Afterwards distinct rows v map to distinct wrapped rows, so threads never write the same texel.
    if (wrapU == 1u)
    {
        u0 = clampInt(u0, 0, texW - 1);
        u1 = clampInt(u1, 0, texW - 1);
    }
    if (wrapV == 1u)
    {
        v0 = clampInt(v0, 0, texH - 1);
        v1 = clampInt(v1, 0, texH - 1);
    }

    const uint16_t regionMinU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
    const uint16_t regionMaxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
    const uint16_t regionMinV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
    const uint16_t regionMaxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);
    auto decodeRows = [&](int firstRow, int lastRow, GSMem::TexturePageCache &pageCache)
    {
        for (int v = firstRow; v <= lastRow; ++v)
        {
            const int wv = wrapTextureCoordinate(v, texH, wrapV, regionMinV, regionMaxV);
            for (int u = u0; u <= u1; ++u)
            {
                const int wu = wrapTextureCoordinate(u, texW, wrapU, regionMinU, regionMaxU);
                FetchTexel(state, wu, wv, pageCache, true);
            }
        }
    };

    RasterPool &pool = RasterPool::instance();
    const uint64_t texelArea = static_cast<uint64_t>(u1 - u0 + 1) * static_cast<uint64_t>(v1 - v0 + 1);
    if (pool.workerCount() == 0u || texelArea < kParallelMinPixels)
    {
        decodeRows(v0, v1, m_texturePageCache);
        return;
    }
    if (m_workerPageCaches.size() < pool.workerCount() + 1u)
        m_workerPageCaches.resize(pool.workerCount() + 1u);
    for (GSMem::TexturePageCache &pageCache : m_workerPageCaches)
        pageCache.Invalidate();
    pool.run(v0, v1, [&](int bandFirst, int bandLast, unsigned slot)
             { decodeRows(bandFirst, bandLast, m_workerPageCaches[slot]); });
}

void GSCpuBackend::RasterizeRows(int firstRow, int lastRow, uint64_t pixelArea, const GSPrimitiveBatch &batch, const RowBandFunc &fn)
{
    if (t_band)
    {
        // Drawing a queued primitive on a raster thread: only this thread's rows.
        firstRow = std::max(firstRow, t_band->first);
        lastRow = std::min(lastRow, t_band->last);
        if (firstRow > lastRow)
            return;
        PrepareTexelCache(batch, false);
        fn(firstRow, lastRow, *t_band->pageCache, true);
        return;
    }
    if (firstRow > lastRow)
        return;

    RasterPool &pool = RasterPool::instance();
    const bool parallel = pool.workerCount() > 0u && pixelArea >= kParallelMinPixels &&
                          lastRow - firstRow >= RasterPool::kBandRows;
    PrepareTexelCache(batch, parallel);
    if (!parallel)
    {
        fn(firstRow, lastRow, m_texturePageCache, true);
        return;
    }

    if (m_workerPageCaches.size() < pool.workerCount() + 1u)
        m_workerPageCaches.resize(pool.workerCount() + 1u);
    for (GSMem::TexturePageCache &pageCache : m_workerPageCaches)
        pageCache.Invalidate();

    pool.run(firstRow, lastRow, [&](int bandFirst, int bandLast, unsigned slot)
             { fn(bandFirst, bandLast, m_workerPageCaches[slot], false); });
}

uint32_t GSCpuBackend::SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v,
                                     GSMem::TexturePageCache &pageCache, bool storeTexels)
{
    const auto &ctx = state.context;

    const int texW = state.textureWidth;
    const int texH = state.textureHeight;
    const uint64_t clamp = ctx.clamp;
    const uint8_t wrapU = static_cast<uint8_t>(clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
    const uint16_t minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
    const uint16_t maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
    const uint16_t minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
    const uint16_t maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);

    float texUf, texVf;
    if (state.prim.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(texW);
        texVf = t * invQ * static_cast<float>(texH);
    }

    if (!state.linearFilter)
    {
        const int pu = wrapTextureCoordinate(static_cast<int>(texUf), texW, wrapU, minU, maxU);
        const int pv = wrapTextureCoordinate(static_cast<int>(texVf), texH, wrapV, minV, maxV);
        return FetchTexel(state, pu, pv, pageCache, storeTexels);
    }

    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const float floorU = std::floor(sampleU);
    const float floorV = std::floor(sampleV);
    const int u0 = static_cast<int>(floorU);
    const int v0 = static_cast<int>(floorV);
    const uint32_t fx = std::min(static_cast<uint32_t>((sampleU - floorU) * 256.0f), 256u);
    const uint32_t fy = std::min(static_cast<uint32_t>((sampleV - floorV) * 256.0f), 256u);

    const int wu0 = wrapTextureCoordinate(u0, texW, wrapU, minU, maxU);
    const int wu1 = wrapTextureCoordinate(u0 + 1, texW, wrapU, minU, maxU);
    const int wv0 = wrapTextureCoordinate(v0, texH, wrapV, minV, maxV);
    const int wv1 = wrapTextureCoordinate(v0 + 1, texH, wrapV, minV, maxV);

    return bilinearRgba(FetchTexel(state, wu0, wv0, pageCache, storeTexels),
                        FetchTexel(state, wu1, wv0, pageCache, storeTexels),
                        FetchTexel(state, wu0, wv1, pageCache, storeTexels),
                        FetchTexel(state, wu1, wv1, pageCache, storeTexels),
                        fx, fy);
}

void GSCpuBackend::DrawSprite(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;
    u32 z1 = static_cast<u32>(v1.z);

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
        return;

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

    const uint64_t alphaReg = ctx.alpha;
    const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);

    uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    if (state.prim.tme)
    {
        const auto &tex = ctx.tex0;
        const int texW = state.textureWidth;
        const int texH = state.textureHeight;

        float u0f, v0f, u1f, v1f;
        if (state.prim.fst)
        {
            u0f = static_cast<float>(v0.u >> 4);
            v0f = static_cast<float>(v0.v >> 4);
            u1f = static_cast<float>(v1.u >> 4);
            v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }

        float spriteW = static_cast<float>(spanX);
        float spriteH = static_cast<float>(spanY);
        if (spriteW < 1.0f)
            spriteW = 1.0f;
        if (spriteH < 1.0f)
            spriteH = 1.0f;

        const uint64_t area = static_cast<uint64_t>(drawX1 - drawX0 + 1) * static_cast<uint64_t>(drawY1 - drawY0 + 1);
        RasterizeRows(drawY0, drawY1, area, batch, [&](int firstRow, int lastRow, GSMem::TexturePageCache &pageCache, bool storeTexels)
        {
            for (int y = firstRow; y <= lastRow; ++y)
            {
                float ty = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
                float texVf = v0f + (v1f - v0f) * ty;

                for (int x = drawX0; x <= drawX1; ++x)
                {
                    float tx = (static_cast<float>(x - unclippedX0) + 0.5f) / spriteW;
                    float texUf = u0f + (u1f - u0f) * tx;
                    uint32_t texel = 0xFFFF00FFu;
                    if (state.prim.fst)
                    {
                        const int fixedU = static_cast<int>((texUf * 16.0f) + 0.5f);
                        const int fixedV = static_cast<int>((texVf * 16.0f) + 0.5f);
                        const uint16_t sampleU = static_cast<uint16_t>(clampInt(fixedU, 0, 0xFFFF));
                        const uint16_t sampleV = static_cast<uint16_t>(clampInt(fixedV, 0, 0xFFFF));
                        texel = SampleTexture(state, 0.0f, 0.0f, 1.0f, sampleU, sampleV, pageCache, storeTexels);
                    }
                    else
                    {
                        texel = SampleTexture(state, texUf / static_cast<float>(texW), texVf / static_cast<float>(texH), 1.0f, 0u, 0u,
                                              pageCache, storeTexels);
                    }

                    uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                    uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                    uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                    uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                    const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                    WritePixel(state, x, y, z1, color.r, color.g, color.b, color.a, v1.fog);
                }
            }
        });
    }
    else
    {
        const uint64_t area = static_cast<uint64_t>(drawX1 - drawX0 + 1) * static_cast<uint64_t>(drawY1 - drawY0 + 1);
        RasterizeRows(drawY0, drawY1, area, batch, [&](int firstRow, int lastRow, GSMem::TexturePageCache &, bool)
        {
            for (int y = firstRow; y <= lastRow; ++y)
                for (int x = drawX0; x <= drawX1; ++x)
                    WritePixel(state, x, y, z1, r, g, b, a, v1.fog);
        });
    }
}

void GSCpuBackend::DrawTriangle(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const GSVertex &v2 = batch.vertices[2];
    const auto &ctx = state.context;

    // Window coordinates in the GS's own 12.4 fixed point.
    const int64_t ix0 = std::lround(v0.x * 16.0f) - ctx.xyoffset.ofx;
    const int64_t iy0 = std::lround(v0.y * 16.0f) - ctx.xyoffset.ofy;
    const int64_t ix1 = std::lround(v1.x * 16.0f) - ctx.xyoffset.ofx;
    const int64_t iy1 = std::lround(v1.y * 16.0f) - ctx.xyoffset.ofy;
    const int64_t ix2 = std::lround(v2.x * 16.0f) - ctx.xyoffset.ofx;
    const int64_t iy2 = std::lround(v2.y * 16.0f) - ctx.xyoffset.ofy;

    float fx0 = static_cast<float>(ix0) / 16.0f;
    float fy0 = static_cast<float>(iy0) / 16.0f;
    float fx1 = static_cast<float>(ix1) / 16.0f;
    float fy1 = static_cast<float>(iy1) / 16.0f;
    float fx2 = static_cast<float>(ix2) / 16.0f;
    float fy2 = static_cast<float>(iy2) / 16.0f;

    int minX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
    int maxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
    int minY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    int maxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));

    minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
    maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
    minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
    maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);

    const int64_t idenom = (iy1 - iy2) * (ix0 - ix2) + (ix2 - ix1) * (iy0 - iy2);
    if (idenom == 0)
        return;

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
    const float winding = (idenom < 0) ? -1.0f : 1.0f;
    const float invAbsDenom = 1.0f / std::fabs(denom);
    constexpr float kEdgeEpsilon = 1.0e-4f;

    // Coverage is decided exactly, in integers, with the top-left fill rule: a pixel exactly on an
    // edge shared by two triangles belongs to just one of them. Otherwise it is drawn twice, which
    // shows wherever the triangles blend. Each edge value is a*X + b*Y + c, positive inside.
    const int64_t iwinding = (idenom < 0) ? -1 : 1;
    const int64_t a0 = (iy1 - iy2) * iwinding, b0 = (ix2 - ix1) * iwinding;
    const int64_t a1 = (iy2 - iy0) * iwinding, b1 = (ix0 - ix2) * iwinding;
    const int64_t a2 = (iy0 - iy1) * iwinding, b2 = (ix1 - ix0) * iwinding;
    // A zero value counts as inside only on a left edge (a > 0) or a top edge (a == 0, b > 0).
    const int64_t bias0 = (a0 > 0 || (a0 == 0 && b0 > 0)) ? 0 : -1;
    const int64_t bias1 = (a1 > 0 || (a1 == 0 && b1 > 0)) ? 0 : -1;
    const int64_t bias2 = (a2 > 0 || (a2 == 0 && b2 > 0)) ? 0 : -1;
    const int64_t absDenom = idenom * iwinding;

    // Bounding box area overestimates the triangle; it only decides whether to use threads.
    const uint64_t area = (maxX >= minX && maxY >= minY)
                              ? static_cast<uint64_t>(maxX - minX + 1) * static_cast<uint64_t>(maxY - minY + 1) / 2u
                              : 0u;
    // Each barycentric weight is linear in px along a row: w = slope * px + intercept.
    // Solving w >= -kEdgeEpsilon for all three gives the row's covered span, so only that
    // span is visited. It is padded by a pixel on each side and every pixel still goes through
    // the exact per-pixel test below, so the pixels drawn are the same as a full bounding-box scan.
    const float slope0 = (fy1 - fy2) * winding * invAbsDenom;
    const float slope1 = (fy2 - fy0) * winding * invAbsDenom;
    const float slope2 = -slope0 - slope1;
    auto rowSpan = [&](float py, int &spanX0, int &spanX1) -> bool
    {
        const float intercept0 = ((fy1 - fy2) * -fx2 + (fx2 - fx1) * (py - fy2)) * winding * invAbsDenom;
        const float intercept1 = ((fy2 - fy0) * -fx2 + (fx0 - fx2) * (py - fy2)) * winding * invAbsDenom;
        const float intercept2 = 1.0f - intercept0 - intercept1;
        if (!std::isfinite(slope2) || !std::isfinite(intercept2) || !std::isfinite(intercept0))
        {
            // Non-finite weights would slip through the per-pixel test; scan the whole row as before.
            spanX0 = minX;
            spanX1 = maxX;
            return true;
        }
        float lo = -std::numeric_limits<float>::infinity();
        float hi = std::numeric_limits<float>::infinity();
        const float slopes[3] = {slope0, slope1, slope2};
        const float intercepts[3] = {intercept0, intercept1, intercept2};
        for (int i = 0; i < 3; ++i)
        {
            const float bound = (-kEdgeEpsilon - intercepts[i]);
            if (slopes[i] > 0.0f)
                lo = std::max(lo, bound / slopes[i]);
            else if (slopes[i] < 0.0f)
                hi = std::min(hi, bound / slopes[i]);
            else if (intercepts[i] < -kEdgeEpsilon)
                return false;
        }
        if (!(lo <= hi + 2.0f) || lo > static_cast<float>(maxX) + 2.0f || hi < static_cast<float>(minX) - 2.0f)
            return false;
        // The padding absorbs rounding differences against the per-pixel test.
        spanX0 = lo > static_cast<float>(minX) ? static_cast<int>(std::floor(lo)) - 1 : minX;
        spanX1 = hi < static_cast<float>(maxX) ? static_cast<int>(std::ceil(hi)) + 1 : maxX;
        spanX0 = std::max(spanX0, minX);
        spanX1 = std::min(spanX1, maxX);
        return spanX0 <= spanX1;
    };

    RasterizeRows(minY, maxY, area, batch, [&](int firstRow, int lastRow, GSMem::TexturePageCache &pageCache, bool storeTexels)
    {
        for (int y = firstRow; y <= lastRow; ++y)
        {
            // The GS samples at the integer pixel position, not at a pixel centre.
            float py = static_cast<float>(y);
            int spanX0, spanX1;
            const bool rowHit = rowSpan(py, spanX0, spanX1);
            if (!rowHit)
                continue;
            const int64_t iy = static_cast<int64_t>(y) * 16;
            for (int x = spanX0; x <= spanX1; ++x)
            {
                const int64_t ipx = static_cast<int64_t>(x) * 16;
                const int64_t e0 = a0 * (ipx - ix2) + b0 * (iy - iy2);
                const int64_t e1 = a1 * (ipx - ix2) + b1 * (iy - iy2);
                const int64_t e2 = a2 * (ipx - ix0) + b2 * (iy - iy0);
                if (e0 + bias0 < 0 || e1 + bias1 < 0 || e2 + bias2 < 0)
                    continue;

                float w0 = static_cast<float>(static_cast<double>(e0) / static_cast<double>(absDenom));
                float w1 = static_cast<float>(static_cast<double>(e1) / static_cast<double>(absDenom));
                float w2 = 1.0f - w0 - w1;

                double z = v0.z * w0 + v1.z * w1 + v2.z * w2;

                uint8_t r, g, b, a;
                if (state.prim.iip)
                {
                    r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                    g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                    b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                    a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
                }
                else
                {
                    r = v2.r;
                    g = v2.g;
                    b = v2.b;
                    a = v2.a;
                }

                if (state.prim.tme)
                {
                    float is, it, iq;
                    uint16_t iu, iv;
                    if (state.prim.fst)
                    {
                        iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                        iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                        is = 0.0f;
                        it = 0.0f;
                        iq = 1.0f;
                    }
                    else
                    {
                        // The GS DDA interpolates the homogeneous S, T and Q
                        // values. Texel coordinates are calculated from S/Q and
                        // T/Q only after interpolation.
                        is = v0.s * w0 + v1.s * w1 + v2.s * w2;
                        it = v0.t * w0 + v1.t * w1 + v2.t * w2;
                        iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
                        iu = 0;
                        iv = 0;
                    }

                    uint32_t texel = SampleTexture(state, is, it, iq, iu, iv, pageCache, storeTexels);

                    uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                    uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                    uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                    uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                    const auto &tex = ctx.tex0;
                    const uint8_t shadeR = r;
                    const uint8_t shadeG = g;
                    const uint8_t shadeB = b;
                    const uint8_t shadeA = a;
                    const TextureCombineResult color = combineTexture(tex, shadeR, shadeG, shadeB, shadeA, tr, tg, tb, ta);

                    r = color.r;
                    g = color.g;
                    b = color.b;
                    a = color.a;
                }

                const uint8_t fog = clampU8(static_cast<int>(v0.fog * w0 + v1.fog * w1 + v2.fog * w2));
                WritePixel(state, x, y, static_cast<u32>(z + 0.5), r, g, b, a, fog);
            }
        }
    });
}

void GSCpuBackend::DrawLine(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (state.prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        double z = (v0.z + (v1.z - v0.z) * t);
        const uint8_t fog = clampU8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * t));
        WritePixel(state, x0, y0, static_cast<u32>(z), r, g, b, a, fog);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}

void GSCpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (gsCensusEnabled())
        GsCensus::instance().transfer(this, command);
    if (command.direction != 0u)
        FlushQueueUnlocked();
    else
        PrepareUpload(command);
    m_transfer = command;
    m_transferState.x = command.trxpos.dsax;
    m_transferState.y = command.trxpos.dsay;
    m_transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * static_cast<uint32_t>(command.trxreg.rrh);
    m_transferState.copiedPixels = 0u;
    m_transferState.direction = command.direction;
    m_transferState.localToHostPendingBytes = 0u;

    if (command.direction == 2u)
        PerformLocalToLocalTransfer();
    else if (command.direction == 1u)
        PerformLocalToHostTransfer();
}

void GSCpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (gsCensusEnabled())
        GsCensus::instance().uploadChunk(this, sizeBytes);
    if (m_pending.prims.size() != m_queueSizeAtTransfer)
    {
        PrepareUpload(m_transfer); // primitives were queued since the transfer started
    }
    else
    {
        thread_local std::vector<VramRange> dest;
        dest.clear();
        CollectUploadRange(m_transfer, dest);
        WaitIfInFlight(dest, false); // batches kicked since the transfer started
    }
    UploadImageUnlocked(data, sizeBytes);
}

void GSCpuBackend::UploadImageUnlocked(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes == 0u || !m_vram || m_transferState.direction != 0u)
        return;
    if (m_transfer.trxreg.rrw == 0u || m_transfer.trxreg.rrh == 0u || m_transferState.totalPixels == 0u)
        return;

    const uint32_t dbp = m_transfer.bitbltbuf.dbp;
    const uint32_t dbw = std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u);
    const uint8_t dpsm = m_transfer.bitbltbuf.dpsm;
    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t dsax = m_transfer.trxpos.dsax;
    uint32_t offset = 0u;

    auto advancePixel = [&](uint32_t count)
    {
        const uint32_t totalPixels = m_transferState.totalPixels;
        m_transferState.copiedPixels =
            std::min<uint32_t>(totalPixels, m_transferState.copiedPixels + count);

        if (m_transferState.copiedPixels >= totalPixels)
        {
            m_transferState.direction = 3u;
            m_transferState.totalPixels = 0u;
            return;
        }

        m_transferState.x = dsax + (m_transferState.copiedPixels % rrw);
        m_transferState.y = m_transfer.trxpos.dsay + (m_transferState.copiedPixels / rrw);
    };

    // Fast path: write the rest of the current row (or as much of it as the data covers) in one
    // span. 4-bit formats need the span to start on a byte, i.e. an even pixel count so far in a
    // transfer whose rows hold whole bytes; anything else falls through to the per-pixel code.
    uint32_t bytesPerPixel = 0u;
    switch (dpsm)
    {
    case GS_PSM_CT32:
    case GS_PSM_Z32:
        bytesPerPixel = 4u;
        break;
    case GS_PSM_CT24:
    case GS_PSM_Z24:
        bytesPerPixel = 3u;
        break;
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        bytesPerPixel = 2u;
        break;
    case GS_PSM_T8:
    case GS_PSM_T8H:
        bytesPerPixel = 1u;
        break;
    default:
        break;
    }
    const bool fourBit = dpsm == GS_PSM_T4 || dpsm == GS_PSM_T4HL || dpsm == GS_PSM_T4HH;

    while (offset < sizeBytes && m_transferState.direction == 0u)
    {
        const uint32_t copied = m_transferState.copiedPixels;
        const uint32_t column = copied % rrw;
        const uint32_t rowLeft = std::min(rrw - column, m_transferState.totalPixels - copied);
        uint32_t count = 0u;
        uint32_t bytes = 0u;
        if (bytesPerPixel != 0u)
        {
            count = std::min(rowLeft, (sizeBytes - offset) / bytesPerPixel);
            bytes = count * bytesPerPixel;
        }
        else if (fourBit && (rrw & 1u) == 0u && (copied & 1u) == 0u)
        {
            count = std::min(rowLeft, (sizeBytes - offset) * 2u);
            bytes = (count + 1u) / 2u;
        }
        if (count != 0u &&
            GSMem::WriteSpan(m_vram, dpsm, dbp, dbw, dsax + column, m_transfer.trxpos.dsay + copied / rrw, data + offset, count))
        {
            offset += bytes;
            advancePixel(count);
            continue;
        }

        switch (dpsm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        {
            if (sizeBytes - offset < 4u)
                return;
            uint32_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 4u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            if (sizeBytes - offset < 3u)
                return;
            const uint32_t value = static_cast<uint32_t>(data[offset]) |
                                   (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
                                   (static_cast<uint32_t>(data[offset + 2u]) << 16u);
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 3u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        {
            if (sizeBytes - offset < 2u)
                return;
            uint16_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 2u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_T8:
        case GS_PSM_T8H:
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, data[offset++]);
            advancePixel(1u);
            break;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            const uint8_t packed = data[offset++];
            const uint32_t firstPixel = m_transferState.copiedPixels;
            WriteVramUnlocked(dpsm, dbp, dbw,
                              dsax + (firstPixel % rrw),
                              m_transfer.trxpos.dsay + (firstPixel / rrw),
                              packed & 0x0Fu);
            if (firstPixel + 1u < m_transferState.totalPixels)
            {
                const uint32_t secondPixel = firstPixel + 1u;
                WriteVramUnlocked(dpsm, dbp, dbw,
                                  dsax + (secondPixel % rrw),
                                  m_transfer.trxpos.dsay + (secondPixel / rrw),
                                  (packed >> 4u) & 0x0Fu);
            }
            advancePixel(std::min<uint32_t>(2u, m_transferState.totalPixels - firstPixel));
            break;
        }
        default:
            return;
        }
    }
}

void GSCpuBackend::PerformLocalToLocalTransfer()
{
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t total = rrw * rrh;
    if (total == 0u)
    {
        m_transferState.direction = 3u;
        return;
    }

    for (uint32_t pixel = 0; pixel < total; ++pixel)
    {
        uint32_t x = pixel % rrw;
        uint32_t y = pixel / rrw;
        if ((m_transfer.trxpos.dir & 0x2u) != 0u)
            x = rrw - x - 1u;
        if ((m_transfer.trxpos.dir & 0x1u) != 0u)
            y = rrh - y - 1u;

        const uint32_t value = ReadVramUnlocked(m_transfer.bitbltbuf.spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u),
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        WriteVramUnlocked(m_transfer.bitbltbuf.dpsm,
                          m_transfer.bitbltbuf.dbp,
                          std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u),
                          x + m_transfer.trxpos.dsax,
                          y + m_transfer.trxpos.dsay,
                          value);
    }

    m_transferState.copiedPixels = total;
    m_transferState.direction = 3u;
}

void GSCpuBackend::PerformLocalToHostTransfer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t sbw = std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u);
    const uint8_t spsm = m_transfer.bitbltbuf.spsm;
    const uint32_t bpp = static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm)));
    const uint32_t total = rrw * rrh;
    m_localToHostBuffer.reserve((static_cast<size_t>(total) * bpp + 7u) / 8u);

    for (uint32_t pixel = 0u; pixel < total; ++pixel)
    {
        const uint32_t x = pixel % rrw;
        const uint32_t y = pixel / rrw;
        const uint32_t value = ReadVramUnlocked(spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                sbw,
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 24u));
            break;
        case 24:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            break;
        case 16:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            break;
        case 8:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            break;
        case 4:
        {
            if ((pixel & 1u) != 0u)
                break;
            uint32_t next = 0u;
            if (pixel + 1u < total)
            {
                const uint32_t nextPixel = pixel + 1u;
                const uint32_t nextX = nextPixel % rrw;
                const uint32_t nextY = nextPixel / rrw;
                next = ReadVramUnlocked(spsm, m_transfer.bitbltbuf.sbp, sbw,
                                        nextX + m_transfer.trxpos.ssax,
                                        nextY + m_transfer.trxpos.ssay);
            }
            m_localToHostBuffer.push_back(static_cast<uint8_t>((value & 0x0Fu) | ((next & 0x0Fu) << 4u)));
            break;
        }
        default:
            break;
        }
    }

    m_transferState.copiedPixels = total;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size();
}

uint32_t GSCpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    FlushQueueUnlocked();
    if (!dst || maxBytes == 0u || m_localToHostReadPos >= m_localToHostBuffer.size())
        return 0u;
    const size_t count = std::min<size_t>(maxBytes, m_localToHostBuffer.size() - m_localToHostReadPos);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, count);
    m_localToHostReadPos += count;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size() - m_localToHostReadPos;
    return static_cast<uint32_t>(count);
}

bool GSCpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (gsCensusEnabled())
        GsCensus::instance().clear(this, context, rgba);
    FlushQueueUnlocked();
    if (!m_vram || context.frame.fbw == 0u)
        return false;

    const uint32_t x0 = context.scissor.x0;
    const uint32_t x1 = std::max<uint32_t>(x0, context.scissor.x1);
    const uint32_t y0 = context.scissor.y0;
    const uint32_t y1 = std::max<uint32_t>(y0, context.scissor.y1);
    uint8_t r = static_cast<uint8_t>(rgba);
    uint8_t g = static_cast<uint8_t>(rgba >> 8u);
    uint8_t b = static_cast<uint8_t>(rgba >> 16u);
    uint8_t a = static_cast<uint8_t>(rgba >> 24u);
    if ((context.fba & 1ull) != 0ull && context.frame.psm != GS_PSM_CT24)
        a |= 0x80u;

    const uint32_t fbp = GSInternal::framePageBaseToBlock(context.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(context.frame.fbw, 1u);
    if (context.frame.psm == GS_PSM_CT32 || context.frame.psm == GS_PSM_CT24)
    {
        const uint32_t source = static_cast<uint32_t>(r) |
                                (static_cast<uint32_t>(g) << 8u) |
                                (static_cast<uint32_t>(b) << 16u) |
                                (static_cast<uint32_t>(a) << 24u);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint32_t pixel = source;
                if (context.frame.fbmsk != 0u)
                {
                    const uint32_t old = ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y);
                    pixel = (pixel & ~context.frame.fbmsk) | (old & context.frame.fbmsk);
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }

    if (context.frame.psm == GS_PSM_CT16 || context.frame.psm == GS_PSM_CT16S)
    {
        const uint16_t source = encodeFramePixelPSMCT16(r, g, b, a);
        const uint16_t mask = static_cast<uint16_t>(context.frame.fbmsk);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint16_t pixel = source;
                if (mask != 0u)
                {
                    const uint16_t old = static_cast<uint16_t>(ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y));
                    pixel = static_cast<uint16_t>((pixel & ~mask) | (old & mask));
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }
    return false;
}

bool GSCpuBackend::CopyFrameToHostRgba(const GSFrameReg &frame,
                                       uint32_t width,
                                       uint32_t height,
                                       std::vector<uint8_t> &outPixels,
                                       bool preserveAlpha,
                                       bool useLocalMemoryLayout,
                                       bool frameBaseIsPages,
                                       uint32_t sourceOriginX,
                                       uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
        return false;

    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
    const uint32_t baseBytes = frameBaseIsPages ? frame.fbp * 8192u : frame.fbp * 256u;
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbw = frame.fbw ? frame.fbw : kHostFrameWidth / 64u;
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t stride = fbw * 64u * bytesPerPixel;

    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *dst = outPixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t sx = sourceOriginX + x;
            const uint32_t sy = sourceOriginY + y;
            if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
            {
                uint32_t color = 0u;
                if (useLocalMemoryLayout)
                    color = ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy);
                else
                {
                    const uint32_t pixelBytes = frame.psm == GS_PSM_CT24 ? 3u : 4u;
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * pixelBytes;
                    if (offset + pixelBytes > m_vramSize)
                        return false;
                    color = m_vram[offset] | (static_cast<uint32_t>(m_vram[offset + 1u]) << 8u) |
                            (static_cast<uint32_t>(m_vram[offset + 2u]) << 16u);
                    if (pixelBytes == 4u)
                        color |= static_cast<uint32_t>(m_vram[offset + 3u]) << 24u;
                }
                dst[x * 4u] = static_cast<uint8_t>(color);
                dst[x * 4u + 1u] = static_cast<uint8_t>(color >> 8u);
                dst[x * 4u + 2u] = static_cast<uint8_t>(color >> 16u);
                dst[x * 4u + 3u] = preserveAlpha && frame.psm != GS_PSM_CT24 ? static_cast<uint8_t>(color >> 24u) : 255u;
            }
            else if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
            {
                uint16_t color = 0u;
                if (useLocalMemoryLayout)
                    color = static_cast<uint16_t>(ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy));
                else
                {
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * 2u;
                    if (offset + 2u > m_vramSize)
                        return false;
                    std::memcpy(&color, m_vram + offset, sizeof(color));
                }
                const uint32_t r = color & 31u;
                const uint32_t g = (color >> 5u) & 31u;
                const uint32_t b = (color >> 10u) & 31u;
                dst[x * 4u] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
                dst[x * 4u + 3u] = preserveAlpha ? ((color & 0x8000u) ? 0x80u : 0u) : 255u;
            }
            else
            {
                outPixels.clear();
                return false;
            }
        }
    }
    return true;
}

PresentationFrame GSCpuBackend::Present(const GSPresentationRequest &request)
{
    // Snapshot local memory under the backend lock, then perform the expensive
    // display conversion without holding the producer-side raster lock.
    thread_local GSCpuBackend snapshotBackend;
    {
        // FPWR: show VRAM as of the latest guest VBlankStart (see MarkPresentation).
        std::lock_guard<std::mutex> lock(m_presentMutex);
        if (m_hasPresentVram)
        {
            if (gsCensusEnabled())
                GsCensus::instance().present(this, m_presentRequest, false, 0);
            snapshotBackend.Initialize(m_presentVram.data(), static_cast<uint32_t>(m_presentVram.size()));
            return snapshotBackend.PresentFromLocalMemory(m_presentRequest);
        }
    }

    if (gsCensusEnabled())
        GsCensus::instance().present(this, request, false, 0);
    thread_local std::vector<uint8_t> snapshot;
    SnapshotVram(snapshot);
    if (snapshot.empty())
        return {};

    snapshotBackend.Initialize(snapshot.data(), static_cast<uint32_t>(snapshot.size()));
    return snapshotBackend.PresentFromLocalMemory(request);
}

PresentationFrame GSCpuBackend::PresentFromLocalMemory(const GSPresentationRequest &request)
{
    PresentationFrame result{};
    const GSPmodeState pmode = decodePmode(request.pmode);
    const GSSmode2State smode2 = decodeSMode2(request.smode2);
    const bool fieldMode = smode2.interlaced && !smode2.frameMode;
    const bool oddField = (request.vsyncTick & 1ull) != 0ull;
    const GSFrameReg displayFrame1 = decodeDisplayFrame(request.dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(request.dispfb2);
    const GSDisplayReadOrigin origin1 = decodeDisplayReadOrigin(request.dispfb1);
    const GSDisplayReadOrigin origin2 = decodeDisplayReadOrigin(request.dispfb2);
    uint32_t width1 = 0u, height1 = 0u, width2 = 0u, height2 = 0u;
    decodeDisplaySize(request.display1, width1, height1);
    decodeDisplaySize(request.display2, width2, height2);
    const bool valid1 = pmode.enableCrt1 && hasDisplaySetup(request.display1, displayFrame1);
    const bool valid2 = pmode.enableCrt2 && hasDisplaySetup(request.display2, displayFrame2);
    if (!valid1 && !valid2)
        return result;

    auto copySource = [&](const GSFrameReg &displayFrame,
                          const GSDisplayReadOrigin &origin,
                          uint32_t width,
                          uint32_t height,
                          bool allowPreferred,
                          bool preserveAlpha,
                          GSFrameReg &selected,
                          std::vector<uint8_t> &pixels,
                          bool &usedPreferred) -> bool
    {
        selected = displayFrame;
        pixels.clear();
        usedPreferred = false;
        if (allowPreferred && request.hasPreferredSource && request.preferredDestFbp == displayFrame.fbp &&
            (request.preferredSource.fbw != 0u || request.preferredSource.fbp != displayFrame.fbp) &&
            CopyFrameToHostRgba(request.preferredSource, width, height, pixels, preserveAlpha, true, false, 0u, 0u))
        {
            selected = request.preferredSource;
            usedPreferred = true;
        }
        if (pixels.empty() && !CopyFrameToHostRgba(displayFrame, width, height, pixels, preserveAlpha, true, true, origin.x, origin.y))
            return false;

        if (!usedPreferred && displayFrame.fbp == 0u && countNonBlackPixels(pixels, width, height) == 0u)
        {
            for (const GSFrameReg &candidate : request.contextFrames)
            {
                if (candidate.fbp == selected.fbp && candidate.fbw == selected.fbw && candidate.psm == selected.psm)
                    continue;
                std::vector<uint8_t> candidatePixels;
                if (!CopyFrameToHostRgba(candidate, width, height, candidatePixels, preserveAlpha, true, true, 0u, 0u))
                    continue;
                if (countNonBlackPixels(candidatePixels, width, height) == 0u)
                    continue;
                selected = candidate;
                pixels.swap(candidatePixels);
                break;
            }
        }
        return true;
    };

    if (valid1 && valid2)
    {
        GSFrameReg selected1{}, selected2{};
        std::vector<uint8_t> crt1, crt2;
        bool preferred1 = false, preferred2 = false;
        if (copySource(displayFrame1, origin1, width1, height1, false, true, selected1, crt1, preferred1) &&
            copySource(displayFrame2, origin2, width2, height2, false, true, selected2, crt2, preferred2))
        {
            result.width = std::max(width1, width2);
            result.height = std::max(height1, height2);
            result.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            const uint8_t bgR = static_cast<uint8_t>(request.bgcolor);
            const uint8_t bgG = static_cast<uint8_t>(request.bgcolor >> 8u);
            const uint8_t bgB = static_cast<uint8_t>(request.bgcolor >> 16u);
            for (uint32_t y = 0; y < result.height; ++y)
                for (uint32_t x = 0; x < result.width; ++x)
                {
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    dst[0] = bgR;
                    dst[1] = bgG;
                    dst[2] = bgB;
                    dst[3] = pmode.alp;
                }
            if (!pmode.slbg)
                for (uint32_t y = 0; y < height2; ++y)
                    std::memcpy(result.pixels.data() + y * kHostFrameWidth * 4u, crt2.data() + y * kHostFrameWidth * 4u, width2 * 4u);
            for (uint32_t y = 0; y < height1; ++y)
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t *src = crt1.data() + (y * kHostFrameWidth + x) * 4u;
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    const uint32_t factor = pmode.mmod ? pmode.alp : std::min<uint32_t>(255u, static_cast<uint32_t>(src[3]) * 2u);
                    dst[0] = blendPresentationChannel(src[0], dst[0], factor);
                    dst[1] = blendPresentationChannel(src[1], dst[1], factor);
                    dst[2] = blendPresentationChannel(src[2], dst[2], factor);
                    dst[3] = pmode.amod ? dst[3] : src[3];
                }
            normalizePresentationAlpha(result.pixels, result.width, result.height);
            if (fieldMode)
                applyFieldPresentation(result.pixels, result.width, result.height, oddField);
            result.displayFbp = displayFrame1.fbp;
            result.sourceFbp = selected1.fbp;
            return result;
        }
    }

    const GSFrameReg &displayFrame = valid1 ? displayFrame1 : displayFrame2;
    const GSDisplayReadOrigin &origin = valid1 ? origin1 : origin2;
    result.width = valid1 ? width1 : width2;
    result.height = valid1 ? height1 : height2;
    GSFrameReg selected = displayFrame;
    if (!copySource(displayFrame, origin, result.width, result.height, true, false, selected, result.pixels, result.usedPreferred))
        return {};
    if (fieldMode)
        applyFieldPresentation(result.pixels, result.width, result.height, oddField);
    normalizePresentationAlpha(result.pixels, result.width, result.height);
    result.displayFbp = displayFrame.fbp;
    result.sourceFbp = selected.fbp;
    return result;
}
