#pragma once

// FPWR: GS recording. A recording holds everything the game sent to the GS renderer
// (GSCpuBackend) for a short clip, so the clip can be played back later without the game: the
// standalone program ps2x_gs_replay does that, and it is the test bench for a future GPU renderer.
//
// What is in this file: the on-disk format (documented below and in docs/formats/gs-recording.md),
// a Writer used by the recorder inside GSCpuBackend, a Reader used by the replayer and the tests,
// and the 64-bit hash used for the check values. Nothing here depends on the rest of the runtime
// except the plain GS types in gs_types.h.
//
// ---------------------------------------------------------------------------------------------
// FORMAT, version 1. All integers are little-endian and written field by field (no struct dumps),
// so a recording made on Windows replays on Linux. f32/f64 are IEEE-754 bit patterns stored as
// u32/u64. bool is one byte, 0 or 1. The file is a header followed by records. Every record is one
// type byte followed by its payload. There are no record lengths: a reader that meets a type it does
// not know must stop (that is what the version number is for).
//
//   Header (20 bytes): char magic[8] = "PS2XGSRC"; u32 version (= 1); u32 flags (= 0); u32 vramSize.
//
//   Start state. It is exactly the first two records, and replay needs it before anything else:
//   0x02 StartVram     u32 pageSize (8192); u32 pageCount (= vramSize / pageSize);
//                      u8 usedBitmap[(pageCount + 7) / 8] (bit i%8 of byte i/8 set = page i is not
//                      all zero); then pageSize bytes for each set bit, in page order.
//   0x03 StartBackend  u16 clut[512]; u32 clutCbp[2]; u32 pageCacheBase (0xFFFFFFFF = empty) and,
//                      when it is not empty, u8 pageCache[8192]; TransferCommand transfer;
//                      u32 x, y, totalPixels, copiedPixels, direction; u64 localToHostPendingBytes
//                      (the backend's transfer-in-progress state); u64 localToHostReadPos;
//                      u32 localToHostSize; u8 localToHost[localToHostSize].
//
//   Tables (they only fill in what later records refer to):
//   0x10 DrawState     DrawState (162 bytes). Defines state number N (N = how many were defined
//                      before) and makes it the current state.
//   0x11 UseState      u32 id. Makes an earlier state current.
//   0x12 Blob          u32 size; u8 data[size]. Defines blob number N (counted the same way).
//
//   Calls into the backend, in the order the game made them:
//   0x20 Submit        u8 header: bits 0-1 = vertex count (1..3); bits 2-3, 4-5, 6-7 = how vertex
//                      0, 1, 2 is given: 0 = new vertex follows, 1..3 = same as vertex 0..2 of the
//                      previous Submit (nothing follows). The slots the previous Submit did not use
//                      count as default vertices. Then, per new vertex: u8 flags (bit 0: z is an
//                      f32, else an f64); f32 x, y; z; u8 r, g, b, a; f32 q, s, t; u16 u, v; u8 fog.
//                      The state is the current one. (Fields are a vertex in GSVertex order.)
//   0x21 LoadClut      Tex0 (18 bytes); u8 cbw, cou; u16 cov
//   0x22 BeginTransfer TransferCommand (29 bytes)
//   0x23 UploadImage   u32 blob. The blob is the data and its size.
//   0x24 Flush         (nothing)
//   0x25 TextureFlush  (nothing)
//   0x26 Sync          u8 reason (GSSyncReason)
//   0x27 Marker        u32 index (0, 1, 2, ... in the file); u64 hostNs (time since the recording
//                      started; information only); PresentationRequest (108 bytes)
//   0x28 ClearFramebuffer  Context (105 bytes); u32 rgba; u8 result
//   0x29 WriteVram     u8 psm; u32 base, bw, x, y, value
//   0x2A Reset         (nothing; also stands for Initialize)
//   0x2B ReadVram      u8 psm; u32 base, bw, x, y; u32 returned
//   0x2C ConsumeLocalToHost  u32 maxBytes; u32 returned; u8 data[returned]
//
//   Check values taken while the game ran (they may appear anywhere after their marker):
//   0x30 LiveFrame     u32 marker; u32 width, height; u64 hash. What Present() returned for that
//                      marker (width = height = 0 and hash 0 when it returned no picture).
//   0x31 LiveDisplay   u32 marker; u64 hash. Hash of the VRAM pages of the displayed buffers at the
//                      marker (GSCpuBackend::HashPresentedPages).
//
//   0x01 End           u32 markerCount. A file without it was cut short and is still readable.
//
// Compound fields, all in this order:
//   Context (105): frame {u32 fbp, u32 fbw, u8 psm, u32 fbmsk}; scissor {u16 x0, x1, y0, y1};
//     tex0 {u32 tbp0; u8 tbw, psm, tw, th, tcc, tfx; u32 cbp; u8 cpsm, csm, csa, cld};
//     xyoffset {u16 ofx, ofy}; zbuf {u32 zbp; u8 psm; bool zmask}; u64 tex1, miptbp1, miptbp2,
//     clamp, alpha, test, fba.
//   Tex0 (18): the tex0 group above.
//   DrawState (162): Context; prim {u8 type; bool iip, tme, fge, abe, aa1, fst, ctxt, fix};
//     texa {u8 ta0; bool aem; u8 ta1}; texclut {u8 cbw, cou; u16 cov}; bool pabe;
//     u64 scanmsk, dimx, dthe, colclamp; u8 fogR, fogG, fogB; u16 textureWidth, textureHeight;
//     bool linearFilter.
//   TransferCommand (29): bitbltbuf {u32 sbp; u8 sbw, spsm; u32 dbp; u8 dbw, dpsm};
//     trxpos {u16 ssax, ssay, dsax, dsay; u8 dir}; trxreg {u16 rrw, rrh}; u32 direction.
//   PresentationRequest (108): u64 pmode, smode2, dispfb1, display1, dispfb2, display2, bgcolor,
//     vsyncTick; contextFrames[2], preferredSource (each {u32 fbp, fbw; u8 psm; u32 fbmsk});
//     u32 preferredDestFbp; bool hasPreferredSource.
//
// Hash (Hash64): XXH64, seed 0 unless said. Blob: Hash64(data, size, seed = size). Frame:
//   h = (width << 32) | height; then for each row, h = Hash64(row, width * 4, h) (row = the RGBA
//   bytes of that row; the row stride is not part of the hash). Pages: h = 0; for each range of
//   whole pages, h = Hash64(range bytes, h).

#include "runtime/gs/gs_types.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace GSRec
{
    constexpr uint32_t kVersion = 1u;
    constexpr uint32_t kHeaderBytes = 20u;
    constexpr uint32_t kPageSize = 8192u;
    constexpr uint32_t kDrawStateBytes = 162u;
    constexpr uint64_t kNoMarker = ~0ull;

    enum RecordType : uint8_t
    {
        RT_Invalid = 0x00,
        RT_End = 0x01,
        RT_StartVram = 0x02,
        RT_StartBackend = 0x03,
        RT_DrawState = 0x10,
        RT_UseState = 0x11,
        RT_Blob = 0x12,
        RT_Submit = 0x20,
        RT_LoadClut = 0x21,
        RT_BeginTransfer = 0x22,
        RT_UploadImage = 0x23,
        RT_Flush = 0x24,
        RT_TextureFlush = 0x25,
        RT_Sync = 0x26,
        RT_Marker = 0x27,
        RT_ClearFramebuffer = 0x28,
        RT_WriteVram = 0x29,
        RT_Reset = 0x2A,
        RT_ReadVram = 0x2B,
        RT_ConsumeLocalToHost = 0x2C,
        RT_LiveFrame = 0x30,
        RT_LiveDisplay = 0x31,
    };

    const char *RecordName(uint8_t type);

    // XXH64 (own implementation, checked against the published value for empty input in the tests).
    uint64_t Hash64(const void *data, size_t size, uint64_t seed = 0);
    // Hash of a width x height RGBA picture whose rows are `strideBytes` apart (see above).
    uint64_t HashFrame(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t strideBytes);

    // The part of the backend's state that is not in GS memory but that later calls depend on.
    struct BackendState
    {
        std::array<uint16_t, 512> clut{};
        std::array<uint32_t, 2> clutCbp{};
        uint32_t pageCacheBase = 0xFFFFFFFFu; // the backend's one-page texture read cache; may be stale
        std::vector<uint8_t> pageCacheBytes;  // kPageSize bytes when pageCacheBase is not 0xFFFFFFFF
        GSTransferCommand transfer{};
        GSTransferSnapshot transferState{};
        std::vector<uint8_t> localToHost;
        uint64_t localToHostReadPos = 0;
    };

    struct WriterStats
    {
        uint64_t records[256] = {};
        uint64_t bytes[256] = {};
        uint64_t statesDefined = 0, statesReused = 0;
        uint64_t blobsDefined = 0, blobsReused = 0, blobBytesDefined = 0, blobBytesReused = 0;
        uint64_t verticesNew = 0, verticesReused = 0;
        uint64_t bytesWritten = 0;
        uint64_t markersWithSubmits = 0; // markers that had at least one Submit since the one before
        uint64_t lastMarkerNs = 0;
    };

    class Writer
    {
    public:
        Writer();
        ~Writer();
        Writer(const Writer &) = delete;
        Writer &operator=(const Writer &) = delete;

        bool open(const std::string &path); // writes the header
        void openMemory();                  // collects the file in memory (tests)
        bool isOpen() const { return m_open; }
        const std::vector<uint8_t> &memory() const { return m_mem; } // after finish(), memory mode

        // Start state; must be the first thing written after open().
        void writeStart(const uint8_t *vram, uint32_t vramSize, const BackendState &state);

        // The calls below are made by one thread at a time (the backend holds its mutex).
        void submit(const GSPrimitiveBatch &batch);
        void loadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
        void beginTransfer(const GSTransferCommand &command);
        void uploadImage(const uint8_t *data, uint32_t size);
        void flush();
        void textureFlush();
        void sync(GSSyncReason reason);
        void marker(uint32_t index, const GSPresentationRequest &request, uint64_t hostNs);
        void clearFramebuffer(const GSContext &context, uint32_t rgba, bool result);
        void writeVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
        void reset();
        void readVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t returned);
        void consumeLocalToHost(uint32_t maxBytes, const uint8_t *data, uint32_t returned);

        // Check values from other threads (the presenter and the drawer). They are queued and
        // written by the thread that makes the calls above, at its next marker or at finish().
        bool wantsFrame(uint64_t marker) const; // false if this marker was reported already or the file is closed
        void liveFrame(uint64_t marker, uint32_t width, uint32_t height, uint64_t hash);
        void liveDisplay(uint64_t marker, uint64_t hash);

        void drainLive();                  // write the queued check values
        void finish(uint32_t markerCount); // End record, flush, close; later check values are dropped
        const WriterStats &stats() const { return m_stats; }
        std::string summary() const; // a few lines for the log: counts and bytes per record type

    private:
        struct LiveItem
        {
            uint8_t type;
            uint32_t marker, width, height;
            uint64_t hash;
        };
        uint8_t *reserve(size_t n);
        void commit(uint8_t *start, uint8_t *end); // start[0] is the record type
        void writeOut();
        void emitState(const uint8_t *encoded);

        FILE *m_file = nullptr;
        bool m_open = false;
        bool m_memory = false;
        std::vector<uint8_t> m_buf;
        size_t m_len = 0;
        std::vector<uint8_t> m_mem;
        WriterStats m_stats;

        // states
        std::vector<std::array<uint8_t, kDrawStateBytes>> m_states;
        std::unordered_multimap<uint64_t, uint32_t> m_stateIndex;
        int64_t m_currentState = -1;
        // blobs
        std::unordered_map<uint64_t, uint32_t> m_blobIds;
        uint32_t m_blobCount = 0;
        // previous Submit's vertices, as encoded (flags byte first), for the "same as" codes
        std::array<std::array<uint8_t, 40>, 3> m_prevVertex{};
        std::array<uint8_t, 3> m_prevVertexLen{};

        mutable std::mutex m_liveMutex;
        std::vector<LiveItem> m_live;
        bool m_liveOpen = false;
        std::atomic<uint64_t> m_lastFrameMarker{kNoMarker};
        uint64_t m_submitsSinceMarker = 0;
    };

    // One decoded record. Tables (DrawState, UseState, Blob) are resolved by the Reader and never shown.
    struct Record
    {
        RecordType type = RT_Invalid;
        GSPrimitiveBatch batch{};   // RT_Submit, with its state
        uint32_t stateId = 0;       // RT_Submit
        GSTex0Reg tex0{};           // RT_LoadClut
        GSTexClutReg texclut{};     // RT_LoadClut
        GSTransferCommand transfer{}; // RT_BeginTransfer
        uint32_t blobId = 0;        // RT_UploadImage
        const uint8_t *data = nullptr; // RT_UploadImage (the blob), RT_ConsumeLocalToHost; both live as long as the Reader
        uint32_t size = 0;          // bytes at data
        GSSyncReason syncReason = GSSyncReason::Finish; // RT_Sync
        uint32_t marker = 0;        // RT_Marker, RT_LiveFrame, RT_LiveDisplay
        uint64_t hostNs = 0;        // RT_Marker
        GSPresentationRequest request{}; // RT_Marker
        GSContext context{};        // RT_ClearFramebuffer
        uint32_t rgba = 0;          // RT_ClearFramebuffer
        bool result = false;        // RT_ClearFramebuffer
        uint32_t psm = 0, base = 0, bw = 0, x = 0, y = 0, value = 0; // RT_WriteVram, RT_ReadVram (value = returned)
        uint32_t maxBytes = 0;      // RT_ConsumeLocalToHost (size = bytes returned)
        uint32_t width = 0, height = 0; // RT_LiveFrame
        uint64_t hash = 0;          // RT_LiveFrame, RT_LiveDisplay
        uint32_t markerCount = 0;   // RT_End
    };

    class Reader
    {
    public:
        enum class Status
        {
            Ok,
            End,       // the End record
            Truncated, // the data ended inside a record, or before End
            Corrupt,   // an unknown record type or an impossible value
        };

        bool openFile(const std::string &path, std::string *error = nullptr); // reads the whole file
        bool openMemory(const uint8_t *data, size_t size, std::string *error = nullptr); // keeps the pointer
        uint32_t version() const { return m_version; }
        uint32_t vramSize() const { return m_vramSize; }

        // The first two records. `vram` is resized to vramSize.
        bool readStart(std::vector<uint8_t> &vram, BackendState &state, std::string *error = nullptr);
        Status next(Record &record);
        size_t position() const { return m_pos; }
        size_t fileSize() const { return m_size; }
        uint32_t statesDefined() const { return static_cast<uint32_t>(m_states.size()); }
        uint32_t blobsDefined() const { return static_cast<uint32_t>(m_blobs.size()); }

    private:
        std::vector<uint8_t> m_owned;
        const uint8_t *m_data = nullptr;
        size_t m_size = 0;
        size_t m_pos = 0;
        uint32_t m_version = 0;
        uint32_t m_vramSize = 0;
        std::vector<GSDrawState> m_states;
        uint32_t m_currentState = 0;
        bool m_hasState = false;
        struct BlobRef
        {
            size_t offset;
            uint32_t size;
        };
        std::vector<BlobRef> m_blobs;
        std::array<GSVertex, 3> m_prevVertex{};
        std::vector<uint8_t> m_scratch;
    };
}
