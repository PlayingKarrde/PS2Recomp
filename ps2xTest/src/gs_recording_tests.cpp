#include "MiniTest.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_recording.h"

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// Tests for the GS recording format (gs_recording.h): hashes, one record of every type written and
// read back, shared states and blobs, a file that was cut short, and a recording replayed into a
// second backend that has to end with the same VRAM and the same picture.

namespace
{
    using namespace GSRec;

    // Hands out different numbers, so a field that is swapped with another or dropped shows up.
    struct Counter
    {
        uint32_t n;
        explicit Counter(uint32_t start) : n(start) {}
        uint8_t u8() { return static_cast<uint8_t>(++n); }
        uint16_t u16() { return static_cast<uint16_t>(++n * 257u); }
        uint32_t u32() { return ++n * 65537u; }
        uint64_t u64() { return (static_cast<uint64_t>(++n) << 40) | (static_cast<uint64_t>(++n) << 8) | n; }
        bool flag() { return (++n & 1u) != 0u; }
    };

    GSContext makeContext(Counter &c)
    {
        GSContext x{};
        x.frame = {c.u32(), c.u32(), c.u8(), c.u32()};
        x.scissor = {c.u16(), c.u16(), c.u16(), c.u16()};
        x.tex0 = {c.u32(), c.u8(), c.u8(), c.u8(), c.u8(), c.u8(), c.u8(), c.u32(), c.u8(), c.u8(), c.u8(), c.u8()};
        x.xyoffset = {c.u16(), c.u16()};
        x.zbuf = {c.u32(), c.u8(), c.flag()};
        x.tex1 = c.u64();
        x.miptbp1 = c.u64();
        x.miptbp2 = c.u64();
        x.clamp = c.u64();
        x.alpha = c.u64();
        x.test = c.u64();
        x.fba = c.u64();
        return x;
    }

    GSDrawState makeState(uint32_t seed)
    {
        Counter c(seed * 1000u);
        GSDrawState s{};
        s.context = makeContext(c);
        s.prim.type = GS_PRIM_TRISTRIP;
        s.prim.iip = c.flag();
        s.prim.tme = c.flag();
        s.prim.fge = c.flag();
        s.prim.abe = c.flag();
        s.prim.aa1 = c.flag();
        s.prim.fst = c.flag();
        s.prim.ctxt = c.flag();
        s.prim.fix = c.flag();
        s.texa = {c.u8(), c.flag(), c.u8()};
        s.texclut = {c.u8(), c.u8(), c.u16()};
        s.pabe = c.flag();
        s.scanmsk = c.u64();
        s.dimx = c.u64();
        s.dthe = c.u64();
        s.colclamp = c.u64();
        s.fogR = c.u8();
        s.fogG = c.u8();
        s.fogB = c.u8();
        s.textureWidth = c.u16();
        s.textureHeight = c.u16();
        s.linearFilter = c.flag();
        return s;
    }

    GSVertex makeVertex(uint32_t seed, bool exactZ)
    {
        Counter c(seed * 77u);
        GSVertex v{};
        v.x = static_cast<float>(c.u16()) / 16.0f;
        v.y = static_cast<float>(c.u16()) / 16.0f;
        v.z = exactZ ? static_cast<double>(c.u32() & 0xFFFFFFu) : 1099511627777.0 + 1.0 / 3.0; // needs an f64
        v.r = c.u8();
        v.g = c.u8();
        v.b = c.u8();
        v.a = c.u8();
        v.q = 0.25f + static_cast<float>(seed);
        v.s = 0.5f + static_cast<float>(seed);
        v.t = -0.125f - static_cast<float>(seed);
        v.u = c.u16();
        v.v = c.u16();
        v.fog = c.u8();
        return v;
    }

    GSPresentationRequest makeRequest(uint32_t seed)
    {
        Counter c(seed * 31u);
        GSPresentationRequest r{};
        r.pmode = c.u64();
        r.smode2 = c.u64();
        r.dispfb1 = c.u64();
        r.display1 = c.u64();
        r.dispfb2 = c.u64();
        r.display2 = c.u64();
        r.bgcolor = c.u64();
        r.vsyncTick = c.u64();
        r.contextFrames[0] = {c.u32(), c.u32(), c.u8(), c.u32()};
        r.contextFrames[1] = {c.u32(), c.u32(), c.u8(), c.u32()};
        r.preferredSource = {c.u32(), c.u32(), c.u8(), c.u32()};
        r.preferredDestFbp = c.u32();
        r.hasPreferredSource = c.flag();
        return r;
    }

    GSTransferCommand makeTransfer(uint32_t seed)
    {
        Counter c(seed * 13u);
        GSTransferCommand t{};
        t.bitbltbuf = {c.u32(), c.u8(), c.u8(), c.u32(), c.u8(), c.u8()};
        t.trxpos = {c.u16(), c.u16(), c.u16(), c.u16(), c.u8()};
        t.trxreg = {c.u16(), c.u16()};
        t.direction = c.u32() & 3u;
        return t;
    }

    // Text for comparing structures field by field.
    std::string text(const GSFrameReg &f)
    {
        std::ostringstream o;
        o << f.fbp << ',' << f.fbw << ',' << +f.psm << ',' << f.fbmsk;
        return o.str();
    }
    std::string text(const GSTex0Reg &t)
    {
        std::ostringstream o;
        o << t.tbp0 << ',' << +t.tbw << ',' << +t.psm << ',' << +t.tw << ',' << +t.th << ',' << +t.tcc << ',' << +t.tfx << ','
          << t.cbp << ',' << +t.cpsm << ',' << +t.csm << ',' << +t.csa << ',' << +t.cld;
        return o.str();
    }
    std::string text(const GSTexClutReg &t)
    {
        std::ostringstream o;
        o << +t.cbw << ',' << +t.cou << ',' << t.cov;
        return o.str();
    }
    std::string text(const GSContext &x)
    {
        std::ostringstream o;
        o << text(x.frame) << '|' << x.scissor.x0 << ',' << x.scissor.x1 << ',' << x.scissor.y0 << ',' << x.scissor.y1 << '|' << text(x.tex0)
          << '|' << x.xyoffset.ofx << ',' << x.xyoffset.ofy << '|' << x.zbuf.zbp << ',' << +x.zbuf.psm << ',' << x.zbuf.zmask << '|' << x.tex1 << ','
          << x.miptbp1 << ',' << x.miptbp2 << ',' << x.clamp << ',' << x.alpha << ',' << x.test << ',' << x.fba;
        return o.str();
    }
    std::string text(const GSDrawState &s)
    {
        std::ostringstream o;
        o << text(s.context) << '|' << +s.prim.type << ',' << s.prim.iip << s.prim.tme << s.prim.fge << s.prim.abe << s.prim.aa1 << s.prim.fst
          << s.prim.ctxt << s.prim.fix << '|' << +s.texa.ta0 << ',' << s.texa.aem << ',' << +s.texa.ta1 << '|' << text(s.texclut) << '|' << s.pabe
          << '|' << s.scanmsk << ',' << s.dimx << ',' << s.dthe << ',' << s.colclamp << '|' << +s.fogR << ',' << +s.fogG << ',' << +s.fogB << '|'
          << s.textureWidth << ',' << s.textureHeight << ',' << s.linearFilter;
        return o.str();
    }
    std::string text(const GSVertex &v)
    {
        std::ostringstream o;
        o.precision(17);
        o << v.x << ',' << v.y << ',' << v.z << ',' << +v.r << ',' << +v.g << ',' << +v.b << ',' << +v.a << ',' << v.q << ',' << v.s << ',' << v.t
          << ',' << v.u << ',' << v.v << ',' << +v.fog;
        return o.str();
    }
    std::string text(const GSPrimitiveBatch &b)
    {
        std::ostringstream o;
        o << +b.vertexCount << '|' << text(b.state);
        for (unsigned i = 0; i < 3; ++i)
            o << '|' << text(b.vertices[i]);
        return o.str();
    }
    std::string text(const GSTransferCommand &t)
    {
        std::ostringstream o;
        o << t.bitbltbuf.sbp << ',' << +t.bitbltbuf.sbw << ',' << +t.bitbltbuf.spsm << ',' << t.bitbltbuf.dbp << ',' << +t.bitbltbuf.dbw << ','
          << +t.bitbltbuf.dpsm << '|' << t.trxpos.ssax << ',' << t.trxpos.ssay << ',' << t.trxpos.dsax << ',' << t.trxpos.dsay << ',' << +t.trxpos.dir
          << '|' << t.trxreg.rrw << ',' << t.trxreg.rrh << '|' << t.direction;
        return o.str();
    }
    std::string text(const GSPresentationRequest &r)
    {
        std::ostringstream o;
        o << r.pmode << ',' << r.smode2 << ',' << r.dispfb1 << ',' << r.display1 << ',' << r.dispfb2 << ',' << r.display2 << ',' << r.bgcolor << ','
          << r.vsyncTick << '|' << text(r.contextFrames[0]) << '|' << text(r.contextFrames[1]) << '|' << text(r.preferredSource) << '|'
          << r.preferredDestFbp << ',' << r.hasPreferredSource;
        return o.str();
    }

    std::vector<uint8_t> pattern(size_t size, uint32_t seed)
    {
        std::vector<uint8_t> data(size);
        for (size_t i = 0; i < size; ++i)
            data[i] = static_cast<uint8_t>((i * 7u + seed) & 0xFFu);
        return data;
    }

    constexpr uint32_t kVramBytes = 4u * 1024u * 1024u;

    // Everything the tests write, so the reading tests can look for it.
    struct Sample
    {
        std::vector<uint8_t> vram;
        BackendState state;
        GSPrimitiveBatch triangle, strip, sprite, again;
        GSTex0Reg tex0;
        GSTexClutReg texclut;
        GSTransferCommand transfer;
        std::vector<uint8_t> blobA, blobB, l2h;
        GSPresentationRequest request;
        GSContext clearContext;
        std::vector<uint8_t> file;
    };

    Sample writeSample()
    {
        Sample s;
        s.vram.assign(kVramBytes, 0u);
        for (uint32_t page : {0u, 7u, 300u, 511u})
        {
            const auto data = pattern(kPageSize, page);
            std::memcpy(s.vram.data() + static_cast<size_t>(page) * kPageSize, data.data(), kPageSize);
        }
        Counter c(5);
        for (uint16_t &v : s.state.clut)
            v = c.u16();
        s.state.clutCbp = {c.u32(), c.u32()};
        s.state.pageCacheBase = 7u * kPageSize;
        s.state.pageCacheBytes = pattern(kPageSize, 99);
        s.state.transfer = makeTransfer(3);
        s.state.transferState.x = c.u32();
        s.state.transferState.y = c.u32();
        s.state.transferState.totalPixels = c.u32();
        s.state.transferState.copiedPixels = c.u32();
        s.state.transferState.direction = 0;
        s.state.transferState.localToHostPendingBytes = 4242;
        s.state.localToHost = pattern(21, 4);
        s.state.localToHostReadPos = 9;

        s.triangle.vertexCount = 3;
        s.triangle.state = makeState(1);
        s.triangle.vertices = {makeVertex(1, true), makeVertex(2, false), makeVertex(3, true)};
        // The next triangle of a strip: two vertices of the last one, one new, in another order.
        s.strip = s.triangle;
        s.strip.vertices = {s.triangle.vertices[2], makeVertex(4, true), s.triangle.vertices[1]};
        // A sprite in another state: two vertices; the third slot stays a default vertex.
        s.sprite.vertexCount = 2;
        s.sprite.state = makeState(2);
        s.sprite.state.prim.type = GS_PRIM_SPRITE;
        s.sprite.vertices[0] = makeVertex(5, true);
        s.sprite.vertices[1] = makeVertex(6, true);
        // Back to the first state.
        s.again = s.triangle;
        s.again.vertices = {makeVertex(7, true), s.sprite.vertices[1], makeVertex(8, false)};

        s.tex0 = makeContext(c).tex0;
        s.texclut = {c.u8(), c.u8(), c.u16()};
        s.transfer = makeTransfer(9);
        s.blobA = pattern(100, 1);
        s.blobB = pattern(5000, 2);
        s.l2h = pattern(5, 6);
        s.request = makeRequest(1);
        Counter c2(900);
        s.clearContext = makeContext(c2);

        Writer w;
        w.openMemory();
        w.writeStart(s.vram.data(), kVramBytes, s.state);
        w.submit(s.triangle);
        w.submit(s.strip);
        w.submit(s.sprite);
        w.submit(s.again);
        w.loadClut(s.tex0, s.texclut);
        w.beginTransfer(s.transfer);
        w.uploadImage(s.blobA.data(), static_cast<uint32_t>(s.blobA.size()));
        w.uploadImage(s.blobA.data(), static_cast<uint32_t>(s.blobA.size())); // the same data again
        w.uploadImage(s.blobB.data(), static_cast<uint32_t>(s.blobB.size()));
        w.flush();
        w.textureFlush();
        w.sync(GSSyncReason::Presentation);
        w.liveFrame(0, 640, 448, 0x1122334455667788ull); // from another thread, normally
        w.liveDisplay(0, 0x99AABBCCDDEEFF00ull);
        w.marker(0, s.request, 123456789ull);
        w.clearFramebuffer(s.clearContext, 0xAABBCCDDu, true);
        w.writeVram(GS_PSM_CT16, 11, 12, 13, 14, 15);
        w.reset();
        w.readVram(GS_PSM_T8, 21, 22, 23, 24, 25);
        w.consumeLocalToHost(64, s.l2h.data(), static_cast<uint32_t>(s.l2h.size()));
        w.finish(1);
        s.file = w.memory();
        return s;
    }

    // Reads the next record, failing the test if there is none.
    bool nextIs(TestCase &t, Reader &r, Record &rec, RecordType type, const char *what)
    {
        const Reader::Status status = r.next(rec);
        if (status != Reader::Status::Ok)
        {
            t.Fail(std::string(what) + ": no record (status " + std::to_string(static_cast<int>(status)) + ")");
            return false;
        }
        if (rec.type != type)
        {
            t.Fail(std::string(what) + ": got " + RecordName(rec.type) + " instead of " + RecordName(type));
            return false;
        }
        return true;
    }
}

void register_gs_recording_tests()
{
    MiniTest::Case("GSRecording", [](TestCase &tc)
    {
        tc.Run("Hash64 is XXH64", [](TestCase &t)
        {
            // Values from an independent XXH64 (the empty one and "abc" are the published ones).
            std::vector<uint8_t> pat(1000);
            for (size_t i = 0; i < pat.size(); ++i)
                pat[i] = static_cast<uint8_t>((i * 7u + 3u) & 255u);
            t.Equals(Hash64(nullptr, 0), 0xEF46DB3751D8E999ull, "empty input");
            t.Equals(Hash64("abc", 3), 0x44BC2CF5AD770999ull, "abc");
            t.Equals(Hash64(pat.data(), 1000), 0x5F235FA033F1A3FBull, "1000 bytes");
            t.Equals(Hash64(pat.data(), 1000, 12345), 0x365C39A0C5A4C88Eull, "1000 bytes with a seed");
            t.Equals(Hash64(pat.data(), 37), 0xE32EF63802F5A3FDull, "37 bytes: one stripe, 8-byte, 4-byte and 1-byte steps");
            t.Equals(Hash64(pat.data(), 13, 7), 0xB54DBCA188B7E03Aull, "13 bytes with a seed");
            t.Equals(Hash64(pat.data(), 5), 0xC7608EFDDB7051FEull, "5 bytes");
        });

        tc.Run("HashFrame covers the picture and not the gaps between rows", [](TestCase &t)
        {
            std::vector<uint8_t> a(8 * 4 * 3, 1), b(8 * 4 * 3, 1);
            // 2 x 2 pixels in a 3-pixel-wide buffer; the third pixel of each row is padding.
            b[2 * 4 + 0] = 200; // padding in row 0: not part of the picture
            t.Equals(HashFrame(a.data(), 2, 2, 12), HashFrame(b.data(), 2, 2, 12), "padding does not count");
            b[1 * 4 + 3] = 9; // inside the picture
            t.IsTrue(HashFrame(a.data(), 2, 2, 12) != HashFrame(b.data(), 2, 2, 12), "a pixel counts");
            t.IsTrue(HashFrame(a.data(), 2, 2, 12) != HashFrame(a.data(), 1, 4, 12), "the size counts");
        });

        tc.Run("one record of every type round-trips", [](TestCase &t)
        {
            const Sample s = writeSample();
            Reader r;
            std::string error;
            t.IsTrue(r.openMemory(s.file.data(), s.file.size(), &error), "open: " + error);
            t.Equals(r.version(), 1u, "version");
            t.Equals(r.vramSize(), kVramBytes, "VRAM size");

            std::vector<uint8_t> vram;
            BackendState st;
            t.IsTrue(r.readStart(vram, st, &error), "start state: " + error);
            t.IsTrue(vram == s.vram, "VRAM image");
            t.IsTrue(st.clut == s.state.clut, "CLUT");
            t.Equals(st.clutCbp[0], s.state.clutCbp[0], "CBP 0");
            t.Equals(st.clutCbp[1], s.state.clutCbp[1], "CBP 1");
            t.Equals(st.pageCacheBase, s.state.pageCacheBase, "page cache base");
            t.IsTrue(st.pageCacheBytes == s.state.pageCacheBytes, "page cache bytes");
            t.Equals(text(st.transfer), text(s.state.transfer), "transfer command");
            t.Equals(st.transferState.x, s.state.transferState.x, "transfer x");
            t.Equals(st.transferState.y, s.state.transferState.y, "transfer y");
            t.Equals(st.transferState.totalPixels, s.state.transferState.totalPixels, "transfer total");
            t.Equals(st.transferState.copiedPixels, s.state.transferState.copiedPixels, "transfer copied");
            t.Equals(st.transferState.direction, s.state.transferState.direction, "transfer direction");
            t.Equals(st.transferState.localToHostPendingBytes, s.state.transferState.localToHostPendingBytes, "transfer pending bytes");
            t.IsTrue(st.localToHost == s.state.localToHost, "local-to-host bytes");
            t.Equals(st.localToHostReadPos, s.state.localToHostReadPos, "local-to-host position");

            Record rec;
            if (nextIs(t, r, rec, RT_Submit, "triangle"))
                t.Equals(text(rec.batch), text(s.triangle), "triangle (exact z, f64 z, state)");
            if (nextIs(t, r, rec, RT_Submit, "strip"))
                t.Equals(text(rec.batch), text(s.strip), "strip triangle sharing vertices");
            if (nextIs(t, r, rec, RT_Submit, "sprite"))
                t.Equals(text(rec.batch), text(s.sprite), "sprite with two vertices and another state");
            if (nextIs(t, r, rec, RT_Submit, "again"))
            {
                t.Equals(text(rec.batch), text(s.again), "triangle in the first state again");
                t.Equals(rec.stateId, 0u, "it used state 0");
            }
            if (nextIs(t, r, rec, RT_LoadClut, "LoadClut"))
            {
                t.Equals(text(rec.tex0), text(s.tex0), "LoadClut tex0");
                t.Equals(text(rec.texclut), text(s.texclut), "LoadClut texclut");
            }
            if (nextIs(t, r, rec, RT_BeginTransfer, "BeginTransfer"))
                t.Equals(text(rec.transfer), text(s.transfer), "BeginTransfer");
            if (nextIs(t, r, rec, RT_UploadImage, "UploadImage A"))
                t.IsTrue(rec.size == s.blobA.size() && std::memcmp(rec.data, s.blobA.data(), rec.size) == 0, "upload A data");
            if (nextIs(t, r, rec, RT_UploadImage, "UploadImage A again"))
            {
                t.IsTrue(rec.size == s.blobA.size() && std::memcmp(rec.data, s.blobA.data(), rec.size) == 0, "upload A data, second time");
                t.Equals(rec.blobId, 0u, "the second upload of the same data is the same blob");
            }
            if (nextIs(t, r, rec, RT_UploadImage, "UploadImage B"))
            {
                t.IsTrue(rec.size == s.blobB.size() && std::memcmp(rec.data, s.blobB.data(), rec.size) == 0, "upload B data");
                t.Equals(rec.blobId, 1u, "new data is a new blob");
            }
            nextIs(t, r, rec, RT_Flush, "Flush");
            nextIs(t, r, rec, RT_TextureFlush, "TextureFlush");
            if (nextIs(t, r, rec, RT_Sync, "Sync"))
                t.Equals(static_cast<int>(rec.syncReason), static_cast<int>(GSSyncReason::Presentation), "sync reason");
            if (nextIs(t, r, rec, RT_Marker, "Marker"))
            {
                t.Equals(rec.marker, 0u, "marker index");
                t.Equals(rec.hostNs, 123456789ull, "marker time");
                t.Equals(text(rec.request), text(s.request), "presentation request");
            }
            if (nextIs(t, r, rec, RT_LiveFrame, "LiveFrame"))
            {
                t.Equals(rec.marker, 0u, "live frame marker");
                t.Equals(rec.width, 640u, "live frame width");
                t.Equals(rec.height, 448u, "live frame height");
                t.Equals(rec.hash, 0x1122334455667788ull, "live frame hash");
            }
            if (nextIs(t, r, rec, RT_LiveDisplay, "LiveDisplay"))
            {
                t.Equals(rec.marker, 0u, "live display marker");
                t.Equals(rec.hash, 0x99AABBCCDDEEFF00ull, "live display hash");
            }
            if (nextIs(t, r, rec, RT_ClearFramebuffer, "ClearFramebuffer"))
            {
                t.Equals(text(rec.context), text(s.clearContext), "clear context");
                t.Equals(rec.rgba, 0xAABBCCDDu, "clear colour");
                t.IsTrue(rec.result, "clear result");
            }
            if (nextIs(t, r, rec, RT_WriteVram, "WriteVram"))
                t.Equals(std::to_string(rec.psm) + "," + std::to_string(rec.base) + "," + std::to_string(rec.bw) + "," + std::to_string(rec.x) + "," +
                             std::to_string(rec.y) + "," + std::to_string(rec.value),
                         std::string("2,11,12,13,14,15"), "WriteVram fields");
            nextIs(t, r, rec, RT_Reset, "Reset");
            if (nextIs(t, r, rec, RT_ReadVram, "ReadVram"))
                t.Equals(std::to_string(rec.psm) + "," + std::to_string(rec.base) + "," + std::to_string(rec.bw) + "," + std::to_string(rec.x) + "," +
                             std::to_string(rec.y) + "," + std::to_string(rec.value),
                         std::string("19,21,22,23,24,25"), "ReadVram fields");
            if (nextIs(t, r, rec, RT_ConsumeLocalToHost, "ConsumeLocalToHost"))
            {
                t.Equals(rec.maxBytes, 64u, "consume max");
                t.IsTrue(rec.size == s.l2h.size() && std::memcmp(rec.data, s.l2h.data(), rec.size) == 0, "consume data");
            }
            t.IsTrue(r.next(rec) == Reader::Status::End, "End record");
            t.Equals(rec.markerCount, 1u, "marker count in End");
            t.Equals(r.position(), r.fileSize(), "the reader used the whole file");
        });

        tc.Run("states, blobs and vertices are shared", [](TestCase &t)
        {
            const Sample s = writeSample();
            // A fresh writer, so its counters can be looked at directly.
            Writer w2;
            w2.openMemory();
            w2.writeStart(s.vram.data(), kVramBytes, s.state);
            w2.submit(s.triangle);
            w2.submit(s.triangle); // identical: the state and all three vertices are shared
            w2.submit(s.sprite);
            w2.submit(s.again);
            w2.uploadImage(s.blobA.data(), 100);
            w2.uploadImage(s.blobA.data(), 100);
            w2.finish(0);
            const WriterStats &st = w2.stats();
            t.Equals(st.statesDefined, 2ull, "two different states");
            t.Equals(st.statesReused, 1ull, "going back to the first state reuses it");
            t.Equals(st.blobsDefined, 1ull, "one blob");
            t.Equals(st.blobsReused, 1ull, "second upload reuses it");
            t.IsTrue(st.verticesReused >= 3, "the repeated triangle costs no vertices");
            t.Equals(st.records[RT_Submit], 4ull, "four Submit records");
            // The identical triangle is two bytes: type and header.
            t.Equals(st.bytes[RT_Submit] < 4 * (2 + 3 * 38), true, "Submit records are compact");
        });

        tc.Run("a file cut short reads up to the cut and reports it", [](TestCase &t)
        {
            const Sample s = writeSample();
            // Find where the Marker record starts by reading, then cut inside it and before it.
            Reader whole;
            whole.openMemory(s.file.data(), s.file.size());
            std::vector<uint8_t> vram;
            BackendState st;
            whole.readStart(vram, st);
            Record rec;
            size_t markerStart = 0;
            for (;;)
            {
                const size_t before = whole.position();
                if (whole.next(rec) != Reader::Status::Ok)
                    break;
                if (rec.type == RT_Marker)
                {
                    markerStart = before;
                    break;
                }
            }
            t.IsTrue(markerStart != 0, "found the marker");

            for (size_t cut : {markerStart, markerStart + 1, markerStart + 40, s.file.size() - 1})
            {
                Reader r;
                std::string error;
                t.IsTrue(r.openMemory(s.file.data(), cut, &error), "open a cut file");
                t.IsTrue(r.readStart(vram, st, &error), "start state of a cut file: " + error);
                unsigned records = 0;
                Reader::Status status;
                while ((status = r.next(rec)) == Reader::Status::Ok)
                    ++records;
                t.IsTrue(status == Reader::Status::Truncated, "a cut file ends with Truncated, cut at " + std::to_string(cut));
                if (cut >= markerStart)
                    t.IsTrue(records >= 12, "everything before the cut is read");
            }

            Reader bad;
            std::string error;
            std::vector<uint8_t> junk = s.file;
            junk[0] = 'X';
            t.IsFalse(bad.openMemory(junk.data(), junk.size(), &error), "bad magic is refused");
            junk = s.file;
            junk[8] = 2;
            t.IsFalse(bad.openMemory(junk.data(), junk.size(), &error), "another version is refused");
        });

        tc.Run("a recording replays into a second backend with the same VRAM and picture", [](TestCase &t)
        {
            // Run a short scene on backend A, writing each call to a recording as well; then play the
            // recording into backend B. (Drawing is immediate in the tests, PS2X_GS_DEFER=0.)
            std::vector<uint8_t> vramA(kVramBytes, 0u), vramB(kVramBytes, 0u);
            GSCpuBackend a;
            a.Initialize(vramA.data(), kVramBytes);
            Writer w;
            w.openMemory();
            w.writeStart(vramA.data(), kVramBytes, BackendState{});

            GSDrawState base{};
            base.context.frame = {0u, 10u, GS_PSM_CT32, 0u};
            base.context.scissor = {0, 639, 0, 447};
            base.context.zbuf = {140u, GS_PSM_Z24, true};
            base.prim.type = GS_PRIM_SPRITE;
            base.prim.abe = true;
            base.context.alpha = 0x44ull; // (Cs-Cd)*As + Cd
            base.context.tex0 = {0x2000u, 1, GS_PSM_CT32, 4, 4, 1, 0, 0, 0, 0, 0, 0};
            base.textureWidth = 16;
            base.textureHeight = 16;

            auto sprite = [&](bool textured, float x0, float y0, float x1, float y1, uint8_t red)
            {
                GSPrimitiveBatch b{};
                b.vertexCount = 2;
                b.state = base;
                b.state.prim.tme = textured;
                b.state.prim.fst = true;
                b.vertices[0].x = x0;
                b.vertices[0].y = y0;
                b.vertices[1].x = x1;
                b.vertices[1].y = y1;
                b.vertices[0].u = 0;
                b.vertices[0].v = 0;
                b.vertices[1].u = 16 * 16;
                b.vertices[1].v = 16 * 16;
                for (unsigned i = 0; i < 2; ++i) // a sprite has two vertices; the third slot stays default
                {
                    b.vertices[i].r = red;
                    b.vertices[i].g = 0x80;
                    b.vertices[i].b = 0x40;
                    b.vertices[i].a = 0x80;
                }
                return b;
            };

            // A texture upload, then one sprite with it and two without.
            const std::vector<uint8_t> texture = pattern(16 * 16 * 4, 17);
            GSTransferCommand upload{};
            upload.bitbltbuf.dbp = 0x2000u;
            upload.bitbltbuf.dbw = 1;
            upload.bitbltbuf.dpsm = GS_PSM_CT32;
            upload.trxreg = {16, 16};
            upload.direction = 0;
            a.BeginTransfer(upload);
            w.beginTransfer(upload);
            a.UploadImage(texture.data(), static_cast<uint32_t>(texture.size()));
            w.uploadImage(texture.data(), static_cast<uint32_t>(texture.size()));
            for (const GSPrimitiveBatch &b : {sprite(true, 10, 10, 60, 50, 0xFF), sprite(false, 40, 30, 100, 90, 0x20), sprite(false, 0, 0, 640, 8, 0x90)})
            {
                a.Submit(b);
                w.submit(b);
            }
            a.TextureFlush();
            w.textureFlush();

            GSPresentationRequest request{};
            request.pmode = 1;
            request.dispfb1 = 0x0 | (10ull << 9);
            request.display1 = (447ull << 44) | (639ull << 32);
            a.MarkPresentation(request);
            w.marker(0, request, 0);
            a.Sync(GSSyncReason::Presentation);
            w.sync(GSSyncReason::Presentation);
            const PresentationFrame frameA = a.Present(request);
            w.finish(1);

            // Play it into B.
            Reader r;
            std::string error;
            t.IsTrue(r.openMemory(w.memory().data(), w.memory().size(), &error), "open: " + error);
            std::vector<uint8_t> startVram;
            BackendState start;
            t.IsTrue(r.readStart(startVram, start, &error), "start: " + error);
            vramB = startVram;
            GSCpuBackend b;
            b.Initialize(vramB.data(), kVramBytes);
            b.ImportRecordingState(start);
            Record rec;
            Reader::Status status;
            PresentationFrame frameB;
            while ((status = r.next(rec)) == Reader::Status::Ok)
            {
                switch (rec.type)
                {
                case RT_BeginTransfer: b.BeginTransfer(rec.transfer); break;
                case RT_UploadImage: b.UploadImage(rec.data, rec.size); break;
                case RT_Submit: b.Submit(rec.batch); break;
                case RT_TextureFlush: b.TextureFlush(); break;
                case RT_Marker: b.MarkPresentation(rec.request); break;
                case RT_Sync:
                    b.Sync(rec.syncReason);
                    frameB = b.Present(request);
                    break;
                default: t.Fail(std::string("unexpected ") + RecordName(rec.type)); break;
                }
            }
            t.IsTrue(status == Reader::Status::End, "reached End");
            std::vector<uint8_t> finalA, finalB;
            a.SnapshotVram(finalA);
            b.SnapshotVram(finalB);
            t.IsTrue(finalA == finalB, "VRAM is identical after the replay");
            t.IsTrue(static_cast<bool>(frameA) && static_cast<bool>(frameB), "both backends presented a picture");
            t.Equals(frameA.width, frameB.width, "picture width");
            t.Equals(frameA.height, frameB.height, "picture height");
            t.Equals(HashFrame(frameA.pixels.data(), frameA.width, frameA.height, GSCpuBackend::kPresentationStride * 4u),
                     HashFrame(frameB.pixels.data(), frameB.width, frameB.height, GSCpuBackend::kPresentationStride * 4u), "picture hash");
            t.Equals(GSCpuBackend::HashPresentedPages(finalA.data(), request), GSCpuBackend::HashPresentedPages(finalB.data(), request),
                     "displayed pages hash");
            // The scene drew something, so the test is not comparing two empty buffers.
            uint32_t nonZero = 0;
            for (uint8_t v : finalA)
                nonZero += v != 0u;
            t.IsTrue(nonZero > 1000u, "the scene wrote pixels");
        });
    });
}
