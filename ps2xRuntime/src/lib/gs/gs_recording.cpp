#include "runtime/gs/gs_recording.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>

// See gs_recording.h for the format. Every compound field is described once, by a "codec"
// function that is used both to write and to read it, so the two directions cannot drift apart:
// Enc (writing) and Dec (reading) offer the same calls, and the codec calls them in file order.

namespace GSRec
{
    namespace
    {
        // ---------------------------------------------------------------------------------
        // Little-endian access, independent of the host byte order.
        inline void put16(uint8_t *p, uint16_t v)
        {
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(v >> 8);
        }
        inline void put32(uint8_t *p, uint32_t v)
        {
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(v >> 8);
            p[2] = static_cast<uint8_t>(v >> 16);
            p[3] = static_cast<uint8_t>(v >> 24);
        }
        inline void put64(uint8_t *p, uint64_t v)
        {
            put32(p, static_cast<uint32_t>(v));
            put32(p + 4, static_cast<uint32_t>(v >> 32));
        }
        inline uint16_t get16(const uint8_t *p)
        {
            return static_cast<uint16_t>(p[0] | (p[1] << 8));
        }
        inline uint32_t get32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }
        inline uint64_t get64(const uint8_t *p)
        {
            return static_cast<uint64_t>(get32(p)) | (static_cast<uint64_t>(get32(p + 4)) << 32);
        }

        struct Enc
        {
            static constexpr bool kReading = false;
            uint8_t *p;
            void u8(const uint8_t &v) { *p++ = v; }
            void b(const bool &v) { *p++ = v ? 1u : 0u; }
            void u16(const uint16_t &v) { put16(p, v); p += 2; }
            void u32(const uint32_t &v) { put32(p, v); p += 4; }
            void u64(const uint64_t &v) { put64(p, v); p += 8; }
            void f32(const float &v) { put32(p, std::bit_cast<uint32_t>(v)); p += 4; }
            void f64(const double &v) { put64(p, std::bit_cast<uint64_t>(v)); p += 8; }
            template <class E> void en(const E &v) { *p++ = static_cast<uint8_t>(v); }
        };

        struct Dec
        {
            static constexpr bool kReading = true;
            const uint8_t *p;
            const uint8_t *end;
            bool ok = true;
            bool need(size_t n)
            {
                if (static_cast<size_t>(end - p) >= n)
                    return true;
                ok = false;
                p = end;
                return false;
            }
            void u8(uint8_t &v) { v = need(1) ? *p++ : 0u; }
            void b(bool &v) { v = need(1) && *p++ != 0u; }
            void u16(uint16_t &v) { if (need(2)) { v = get16(p); p += 2; } else v = 0; }
            void u32(uint32_t &v) { if (need(4)) { v = get32(p); p += 4; } else v = 0; }
            void u64(uint64_t &v) { if (need(8)) { v = get64(p); p += 8; } else v = 0; }
            void f32(float &v) { uint32_t t; u32(t); v = std::bit_cast<float>(t); }
            void f64(double &v) { uint64_t t; u64(t); v = std::bit_cast<double>(t); }
            template <class E> void en(E &v) { uint8_t t; u8(t); v = static_cast<E>(t); }
        };

        // ---------------------------------------------------------------------------------
        // Compound fields. T is the type with or without const (writing takes const objects).
        template <class C, class T> void codecFrame(C &c, T &f)
        {
            c.u32(f.fbp);
            c.u32(f.fbw);
            c.u8(f.psm);
            c.u32(f.fbmsk);
        }

        template <class C, class T> void codecTex0(C &c, T &t)
        {
            c.u32(t.tbp0);
            c.u8(t.tbw);
            c.u8(t.psm);
            c.u8(t.tw);
            c.u8(t.th);
            c.u8(t.tcc);
            c.u8(t.tfx);
            c.u32(t.cbp);
            c.u8(t.cpsm);
            c.u8(t.csm);
            c.u8(t.csa);
            c.u8(t.cld);
        }

        template <class C, class T> void codecTexClut(C &c, T &t)
        {
            c.u8(t.cbw);
            c.u8(t.cou);
            c.u16(t.cov);
        }

        template <class C, class T> void codecContext(C &c, T &x)
        {
            codecFrame(c, x.frame);
            c.u16(x.scissor.x0);
            c.u16(x.scissor.x1);
            c.u16(x.scissor.y0);
            c.u16(x.scissor.y1);
            codecTex0(c, x.tex0);
            c.u16(x.xyoffset.ofx);
            c.u16(x.xyoffset.ofy);
            c.u32(x.zbuf.zbp);
            c.u8(x.zbuf.psm);
            c.b(x.zbuf.zmask);
            c.u64(x.tex1);
            c.u64(x.miptbp1);
            c.u64(x.miptbp2);
            c.u64(x.clamp);
            c.u64(x.alpha);
            c.u64(x.test);
            c.u64(x.fba);
        }

        template <class C, class T> void codecDrawState(C &c, T &s)
        {
            codecContext(c, s.context);
            c.en(s.prim.type);
            c.b(s.prim.iip);
            c.b(s.prim.tme);
            c.b(s.prim.fge);
            c.b(s.prim.abe);
            c.b(s.prim.aa1);
            c.b(s.prim.fst);
            c.b(s.prim.ctxt);
            c.b(s.prim.fix);
            c.u8(s.texa.ta0);
            c.b(s.texa.aem);
            c.u8(s.texa.ta1);
            codecTexClut(c, s.texclut);
            c.b(s.pabe);
            c.u64(s.scanmsk);
            c.u64(s.dimx);
            c.u64(s.dthe);
            c.u64(s.colclamp);
            c.u8(s.fogR);
            c.u8(s.fogG);
            c.u8(s.fogB);
            c.u16(s.textureWidth);
            c.u16(s.textureHeight);
            c.b(s.linearFilter);
        }

        template <class C, class T> void codecTransfer(C &c, T &t)
        {
            c.u32(t.bitbltbuf.sbp);
            c.u8(t.bitbltbuf.sbw);
            c.u8(t.bitbltbuf.spsm);
            c.u32(t.bitbltbuf.dbp);
            c.u8(t.bitbltbuf.dbw);
            c.u8(t.bitbltbuf.dpsm);
            c.u16(t.trxpos.ssax);
            c.u16(t.trxpos.ssay);
            c.u16(t.trxpos.dsax);
            c.u16(t.trxpos.dsay);
            c.u8(t.trxpos.dir);
            c.u16(t.trxreg.rrw);
            c.u16(t.trxreg.rrh);
            c.u32(t.direction);
        }

        template <class C, class T> void codecRequest(C &c, T &r)
        {
            c.u64(r.pmode);
            c.u64(r.smode2);
            c.u64(r.dispfb1);
            c.u64(r.display1);
            c.u64(r.dispfb2);
            c.u64(r.display2);
            c.u64(r.bgcolor);
            c.u64(r.vsyncTick);
            codecFrame(c, r.contextFrames[0]);
            codecFrame(c, r.contextFrames[1]);
            codecFrame(c, r.preferredSource);
            c.u32(r.preferredDestFbp);
            c.b(r.hasPreferredSource);
        }

        // ---------------------------------------------------------------------------------
        // Vertices. Written compactly but exactly: z is stored as an f32 when that loses nothing.
        constexpr size_t kMaxVertexBytes = 38;

        size_t encodeVertex(uint8_t *out, const GSVertex &v)
        {
            const float zf = static_cast<float>(v.z);
            const bool z32 = static_cast<double>(zf) == v.z; // false for NaN and out-of-range values
            Enc e{out};
            e.u8(z32 ? 1u : 0u);
            e.f32(v.x);
            e.f32(v.y);
            if (z32)
                e.f32(zf);
            else
                e.f64(v.z);
            e.u8(v.r);
            e.u8(v.g);
            e.u8(v.b);
            e.u8(v.a);
            e.f32(v.q);
            e.f32(v.s);
            e.f32(v.t);
            e.u16(v.u);
            e.u16(v.v);
            e.u8(v.fog);
            return static_cast<size_t>(e.p - out);
        }

        void decodeVertex(Dec &d, GSVertex &v)
        {
            uint8_t flags = 0;
            d.u8(flags);
            d.f32(v.x);
            d.f32(v.y);
            if ((flags & 1u) != 0u)
            {
                float zf;
                d.f32(zf);
                v.z = static_cast<double>(zf);
            }
            else
            {
                d.f64(v.z);
            }
            d.u8(v.r);
            d.u8(v.g);
            d.u8(v.b);
            d.u8(v.a);
            d.f32(v.q);
            d.f32(v.s);
            d.f32(v.t);
            d.u16(v.u);
            d.u16(v.v);
            d.u8(v.fog);
        }

        constexpr uint8_t kHeaderMagic[8] = {'P', 'S', '2', 'X', 'G', 'S', 'R', 'C'};
    }

    const char *RecordName(uint8_t type)
    {
        switch (type)
        {
        case RT_End: return "End";
        case RT_StartVram: return "StartVram";
        case RT_StartBackend: return "StartBackend";
        case RT_DrawState: return "DrawState";
        case RT_UseState: return "UseState";
        case RT_Blob: return "Blob";
        case RT_Submit: return "Submit";
        case RT_LoadClut: return "LoadClut";
        case RT_BeginTransfer: return "BeginTransfer";
        case RT_UploadImage: return "UploadImage";
        case RT_Flush: return "Flush";
        case RT_TextureFlush: return "TextureFlush";
        case RT_Sync: return "Sync";
        case RT_Marker: return "Marker";
        case RT_ClearFramebuffer: return "ClearFramebuffer";
        case RT_WriteVram: return "WriteVram";
        case RT_Reset: return "Reset";
        case RT_ReadVram: return "ReadVram";
        case RT_ConsumeLocalToHost: return "ConsumeLocalToHost";
        case RT_LiveFrame: return "LiveFrame";
        case RT_LiveDisplay: return "LiveDisplay";
        default: return "?";
        }
    }

    // ---------------------------------------------------------------------------------------
    // XXH64
    namespace
    {
        constexpr uint64_t P1 = 0x9E3779B185EBCA87ull;
        constexpr uint64_t P2 = 0xC2B2AE3D27D4EB4Full;
        constexpr uint64_t P3 = 0x165667B19E3779F9ull;
        constexpr uint64_t P4 = 0x85EBCA77C2B2AE63ull;
        constexpr uint64_t P5 = 0x27D4EB2F165667C5ull;
        inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
        inline uint64_t xround(uint64_t acc, uint64_t input)
        {
            acc += input * P2;
            acc = rotl(acc, 31);
            return acc * P1;
        }
        inline uint64_t xmerge(uint64_t acc, uint64_t val)
        {
            acc ^= xround(0, val);
            return acc * P1 + P4;
        }
    }

    uint64_t Hash64(const void *data, size_t len, uint64_t seed)
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        const uint8_t *const end = p + len;
        uint64_t h;
        if (len >= 32)
        {
            const uint8_t *const limit = end - 32;
            uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
            do
            {
                v1 = xround(v1, get64(p));
                v2 = xround(v2, get64(p + 8));
                v3 = xround(v3, get64(p + 16));
                v4 = xround(v4, get64(p + 24));
                p += 32;
            } while (p <= limit);
            h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
            h = xmerge(h, v1);
            h = xmerge(h, v2);
            h = xmerge(h, v3);
            h = xmerge(h, v4);
        }
        else
        {
            h = seed + P5;
        }
        h += static_cast<uint64_t>(len);
        while (p + 8 <= end)
        {
            h ^= xround(0, get64(p));
            h = rotl(h, 27) * P1 + P4;
            p += 8;
        }
        if (p + 4 <= end)
        {
            h ^= static_cast<uint64_t>(get32(p)) * P1;
            h = rotl(h, 23) * P2 + P3;
            p += 4;
        }
        while (p < end)
        {
            h ^= static_cast<uint64_t>(*p) * P5;
            h = rotl(h, 11) * P1;
            ++p;
        }
        h ^= h >> 33;
        h *= P2;
        h ^= h >> 29;
        h *= P3;
        h ^= h >> 32;
        return h;
    }

    uint64_t HashFrame(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t strideBytes)
    {
        uint64_t h = (static_cast<uint64_t>(width) << 32) | height;
        for (uint32_t y = 0; y < height; ++y)
            h = Hash64(rgba + static_cast<size_t>(y) * strideBytes, static_cast<size_t>(width) * 4u, h);
        return h;
    }

    // ---------------------------------------------------------------------------------------
    // Writer
    Writer::Writer()
    {
        m_buf.resize(1u << 20);
        uint8_t def[kMaxVertexBytes + 2];
        const size_t n = encodeVertex(def, GSVertex{});
        for (size_t i = 0; i < 3; ++i)
        {
            std::memcpy(m_prevVertex[i].data(), def, n);
            m_prevVertexLen[i] = static_cast<uint8_t>(n);
        }
    }

    Writer::~Writer()
    {
        if (m_file)
        {
            writeOut();
            std::fclose(m_file);
            m_file = nullptr;
        }
    }

    bool Writer::open(const std::string &path)
    {
        m_file = std::fopen(path.c_str(), "wb");
        if (!m_file)
            return false;
        std::setvbuf(m_file, nullptr, _IOFBF, 1u << 16);
        m_open = true;
        m_liveOpen = true;
        uint8_t header[kHeaderBytes];
        std::memcpy(header, kHeaderMagic, 8);
        put32(header + 8, kVersion);
        put32(header + 12, 0u);
        put32(header + 16, 0u); // vramSize, filled in by writeStart
        std::memcpy(m_buf.data(), header, kHeaderBytes);
        m_len = kHeaderBytes;
        return true;
    }

    void Writer::openMemory()
    {
        m_memory = true;
        m_open = true;
        m_liveOpen = true;
        uint8_t header[kHeaderBytes] = {};
        std::memcpy(header, kHeaderMagic, 8);
        put32(header + 8, kVersion);
        std::memcpy(m_buf.data(), header, kHeaderBytes);
        m_len = kHeaderBytes;
    }

    void Writer::writeOut()
    {
        if (m_len == 0)
            return;
        m_stats.bytesWritten += m_len;
        if (m_memory)
            m_mem.insert(m_mem.end(), m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(m_len));
        else if (m_file)
            std::fwrite(m_buf.data(), 1, m_len, m_file);
        m_len = 0;
    }

    uint8_t *Writer::reserve(size_t n)
    {
        if (m_len + n > m_buf.size())
        {
            writeOut();
            if (n > m_buf.size())
                m_buf.resize(n);
        }
        return m_buf.data() + m_len;
    }

    void Writer::commit(uint8_t *start, uint8_t *end)
    {
        const size_t n = static_cast<size_t>(end - start);
        m_len += n;
        m_stats.records[start[0]]++;
        m_stats.bytes[start[0]] += n;
    }

    void Writer::writeStart(const uint8_t *vram, uint32_t vramSize, const BackendState &state)
    {
        // The header was written by open(); fix up the vramSize now that it is known. It is still in
        // the buffer because nothing has been written yet.
        put32(m_buf.data() + 16, vramSize);

        const uint32_t pageCount = vramSize / kPageSize;
        const uint32_t bitmapBytes = (pageCount + 7u) / 8u;
        std::vector<uint8_t> bitmap(bitmapBytes, 0u);
        for (uint32_t page = 0; page < pageCount; ++page)
        {
            const uint8_t *src = vram + static_cast<size_t>(page) * kPageSize;
            uint64_t any = 0;
            for (uint32_t i = 0; i < kPageSize; i += 8)
                any |= get64(src + i);
            if (any != 0u)
            {
                bitmap[page >> 3] = static_cast<uint8_t>(bitmap[page >> 3] | (1u << (page & 7u)));
            }
        }
        {
            uint8_t *start = reserve(1 + 8 + bitmapBytes);
            uint8_t *p = start;
            *p++ = RT_StartVram;
            put32(p, kPageSize);
            put32(p + 4, pageCount);
            p += 8;
            std::memcpy(p, bitmap.data(), bitmapBytes);
            p += bitmapBytes;
            commit(start, p);
        }
        for (uint32_t page = 0; page < pageCount; ++page)
        {
            if ((bitmap[page >> 3] & (1u << (page & 7u))) == 0u)
                continue;
            uint8_t *start = reserve(kPageSize);
            std::memcpy(start, vram + static_cast<size_t>(page) * kPageSize, kPageSize);
            m_len += kPageSize;
            m_stats.bytes[RT_StartVram] += kPageSize;
        }

        const bool hasPage = state.pageCacheBase != 0xFFFFFFFFu && state.pageCacheBytes.size() >= kPageSize;
        const size_t max = 1 + 1024 + 8 + 4 + (hasPage ? kPageSize : 0) + 29 + 20 + 8 + 8 + 4 + state.localToHost.size();
        uint8_t *start = reserve(max);
        Enc e{start};
        e.u8(RT_StartBackend);
        for (uint16_t v : state.clut)
            e.u16(v);
        e.u32(state.clutCbp[0]);
        e.u32(state.clutCbp[1]);
        e.u32(hasPage ? state.pageCacheBase : 0xFFFFFFFFu);
        if (hasPage)
        {
            std::memcpy(e.p, state.pageCacheBytes.data(), kPageSize);
            e.p += kPageSize;
        }
        codecTransfer(e, state.transfer);
        e.u32(state.transferState.x);
        e.u32(state.transferState.y);
        e.u32(state.transferState.totalPixels);
        e.u32(state.transferState.copiedPixels);
        e.u32(state.transferState.direction);
        e.u64(static_cast<uint64_t>(state.transferState.localToHostPendingBytes));
        e.u64(state.localToHostReadPos);
        e.u32(static_cast<uint32_t>(state.localToHost.size()));
        if (!state.localToHost.empty())
        {
            std::memcpy(e.p, state.localToHost.data(), state.localToHost.size());
            e.p += state.localToHost.size();
        }
        commit(start, e.p);
    }

    void Writer::emitState(const uint8_t *encoded)
    {
        const uint64_t h = Hash64(encoded, kDrawStateBytes);
        const auto range = m_stateIndex.equal_range(h);
        for (auto it = range.first; it != range.second; ++it)
        {
            if (std::memcmp(m_states[it->second].data(), encoded, kDrawStateBytes) == 0)
            {
                uint8_t *start = reserve(5);
                start[0] = RT_UseState;
                put32(start + 1, it->second);
                commit(start, start + 5);
                m_currentState = it->second;
                ++m_stats.statesReused;
                return;
            }
        }
        const uint32_t id = static_cast<uint32_t>(m_states.size());
        m_states.emplace_back();
        std::memcpy(m_states.back().data(), encoded, kDrawStateBytes);
        m_stateIndex.emplace(h, id);
        uint8_t *start = reserve(1 + kDrawStateBytes);
        start[0] = RT_DrawState;
        std::memcpy(start + 1, encoded, kDrawStateBytes);
        commit(start, start + 1 + kDrawStateBytes);
        m_currentState = id;
        ++m_stats.statesDefined;
    }

    void Writer::submit(const GSPrimitiveBatch &batch)
    {
        unsigned count = batch.vertexCount;
        if (count == 0u)
            return;
        if (count > 3u)
            count = 3u;

        uint8_t encodedState[kDrawStateBytes];
        {
            Enc e{encodedState};
            codecDrawState(e, batch.state);
        }
        if (m_currentState < 0 || std::memcmp(m_states[static_cast<size_t>(m_currentState)].data(), encodedState, kDrawStateBytes) != 0)
            emitState(encodedState);

        uint8_t cur[3][kMaxVertexBytes + 2];
        uint8_t curLen[3];
        static const GSVertex kDefaultVertex{};
        for (unsigned i = 0; i < 3u; ++i)
            curLen[i] = static_cast<uint8_t>(encodeVertex(cur[i], i < count ? batch.vertices[i] : kDefaultVertex));

        uint8_t header = static_cast<uint8_t>(count);
        uint8_t reuse[3] = {0, 0, 0};
        for (unsigned i = 0; i < count; ++i)
        {
            for (unsigned j = 0; j < 3u; ++j)
            {
                if (curLen[i] == m_prevVertexLen[j] && std::memcmp(cur[i], m_prevVertex[j].data(), curLen[i]) == 0)
                {
                    reuse[i] = static_cast<uint8_t>(j + 1u);
                    break;
                }
            }
            header = static_cast<uint8_t>(header | (reuse[i] << (2u + 2u * i)));
        }

        ++m_submitsSinceMarker;
        uint8_t *start = reserve(2 + 3 * kMaxVertexBytes);
        uint8_t *p = start;
        *p++ = RT_Submit;
        *p++ = header;
        for (unsigned i = 0; i < count; ++i)
        {
            if (reuse[i] != 0u)
            {
                ++m_stats.verticesReused;
                continue;
            }
            std::memcpy(p, cur[i], curLen[i]);
            p += curLen[i];
            ++m_stats.verticesNew;
        }
        commit(start, p);

        for (unsigned i = 0; i < 3u; ++i)
        {
            std::memcpy(m_prevVertex[i].data(), cur[i], curLen[i]);
            m_prevVertexLen[i] = curLen[i];
        }
    }

    void Writer::loadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
    {
        uint8_t *start = reserve(1 + 18 + 4);
        Enc e{start};
        e.u8(RT_LoadClut);
        codecTex0(e, tex0);
        codecTexClut(e, texclut);
        commit(start, e.p);
    }

    void Writer::beginTransfer(const GSTransferCommand &command)
    {
        uint8_t *start = reserve(1 + 29);
        Enc e{start};
        e.u8(RT_BeginTransfer);
        codecTransfer(e, command);
        commit(start, e.p);
    }

    void Writer::uploadImage(const uint8_t *data, uint32_t size)
    {
        const uint64_t h = Hash64(data, size, size);
        uint32_t id;
        const auto found = m_blobIds.find(h);
        if (found != m_blobIds.end())
        {
            id = found->second;
            ++m_stats.blobsReused;
            m_stats.blobBytesReused += size;
        }
        else
        {
            id = m_blobCount++;
            m_blobIds.emplace(h, id);
            uint8_t *start = reserve(5 + static_cast<size_t>(size));
            start[0] = RT_Blob;
            put32(start + 1, size);
            std::memcpy(start + 5, data, size);
            commit(start, start + 5 + size);
            ++m_stats.blobsDefined;
            m_stats.blobBytesDefined += size;
        }
        uint8_t *start = reserve(5);
        start[0] = RT_UploadImage;
        put32(start + 1, id);
        commit(start, start + 5);
    }

    void Writer::flush()
    {
        uint8_t *start = reserve(1);
        start[0] = RT_Flush;
        commit(start, start + 1);
    }

    void Writer::textureFlush()
    {
        uint8_t *start = reserve(1);
        start[0] = RT_TextureFlush;
        commit(start, start + 1);
    }

    void Writer::sync(GSSyncReason reason)
    {
        uint8_t *start = reserve(2);
        start[0] = RT_Sync;
        start[1] = static_cast<uint8_t>(reason);
        commit(start, start + 2);
    }

    void Writer::marker(uint32_t index, const GSPresentationRequest &request, uint64_t hostNs)
    {
        uint8_t *start = reserve(1 + 4 + 8 + 108);
        Enc e{start};
        e.u8(RT_Marker);
        e.u32(index);
        e.u64(hostNs);
        codecRequest(e, request);
        commit(start, e.p);
        if (m_submitsSinceMarker != 0u)
            ++m_stats.markersWithSubmits;
        m_submitsSinceMarker = 0;
        m_stats.lastMarkerNs = hostNs;
        drainLive();
        // A marker is once per VBlank: hand the data to the OS so a killed game leaves a usable file.
        writeOut();
        if (m_file)
            std::fflush(m_file);
    }

    void Writer::clearFramebuffer(const GSContext &context, uint32_t rgba, bool result)
    {
        uint8_t *start = reserve(1 + 105 + 4 + 1);
        Enc e{start};
        e.u8(RT_ClearFramebuffer);
        codecContext(e, context);
        e.u32(rgba);
        e.b(result);
        commit(start, e.p);
    }

    void Writer::writeVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
    {
        uint8_t *start = reserve(1 + 1 + 20);
        Enc e{start};
        e.u8(RT_WriteVram);
        const uint8_t psm8 = static_cast<uint8_t>(psm);
        e.u8(psm8);
        e.u32(base);
        e.u32(bw);
        e.u32(x);
        e.u32(y);
        e.u32(value);
        commit(start, e.p);
    }

    void Writer::reset()
    {
        uint8_t *start = reserve(1);
        start[0] = RT_Reset;
        commit(start, start + 1);
    }

    void Writer::readVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t returned)
    {
        uint8_t *start = reserve(1 + 1 + 20);
        Enc e{start};
        e.u8(RT_ReadVram);
        const uint8_t psm8 = static_cast<uint8_t>(psm);
        e.u8(psm8);
        e.u32(base);
        e.u32(bw);
        e.u32(x);
        e.u32(y);
        e.u32(returned);
        commit(start, e.p);
    }

    void Writer::consumeLocalToHost(uint32_t maxBytes, const uint8_t *data, uint32_t returned)
    {
        uint8_t *start = reserve(1 + 8 + static_cast<size_t>(returned));
        Enc e{start};
        e.u8(RT_ConsumeLocalToHost);
        e.u32(maxBytes);
        e.u32(returned);
        if (returned != 0u)
        {
            std::memcpy(e.p, data, returned);
            e.p += returned;
        }
        commit(start, e.p);
    }

    bool Writer::wantsFrame(uint64_t marker) const
    {
        return marker != kNoMarker && marker != m_lastFrameMarker.load(std::memory_order_relaxed);
    }

    void Writer::liveFrame(uint64_t marker, uint32_t width, uint32_t height, uint64_t hash)
    {
        std::lock_guard<std::mutex> lock(m_liveMutex);
        if (!m_liveOpen)
            return;
        m_lastFrameMarker.store(marker, std::memory_order_relaxed);
        m_live.push_back({RT_LiveFrame, static_cast<uint32_t>(marker), width, height, hash});
    }

    void Writer::liveDisplay(uint64_t marker, uint64_t hash)
    {
        std::lock_guard<std::mutex> lock(m_liveMutex);
        if (!m_liveOpen)
            return;
        m_live.push_back({RT_LiveDisplay, static_cast<uint32_t>(marker), 0u, 0u, hash});
    }

    void Writer::drainLive()
    {
        std::vector<LiveItem> items;
        {
            std::lock_guard<std::mutex> lock(m_liveMutex);
            items.swap(m_live);
        }
        for (const LiveItem &item : items)
        {
            uint8_t *start = reserve(1 + 4 + 4 + 4 + 8);
            Enc e{start};
            e.u8(item.type);
            e.u32(item.marker);
            if (item.type == RT_LiveFrame)
            {
                e.u32(item.width);
                e.u32(item.height);
            }
            e.u64(item.hash);
            commit(start, e.p);
        }
    }

    void Writer::finish(uint32_t markerCount)
    {
        if (!m_open)
            return;
        {
            std::lock_guard<std::mutex> lock(m_liveMutex);
            m_liveOpen = false;
        }
        drainLive();
        uint8_t *start = reserve(5);
        start[0] = RT_End;
        put32(start + 1, markerCount);
        commit(start, start + 5);
        writeOut();
        if (m_file)
        {
            std::fclose(m_file);
            m_file = nullptr;
        }
        m_open = false;
    }

    std::string Writer::summary() const
    {
        std::string out;
        char line[512];
        std::snprintf(line, sizeof(line), "[gs:rec]   %llu bytes in the file; %llu markers over %.2f s, %llu of them with primitives\n",
                      static_cast<unsigned long long>(m_stats.bytesWritten),
                      static_cast<unsigned long long>(m_stats.records[RT_Marker]),
                      static_cast<double>(m_stats.lastMarkerNs) / 1.0e9,
                      static_cast<unsigned long long>(m_stats.markersWithSubmits));
        out += line;
        for (unsigned type = 0; type < 256; ++type)
        {
            if (m_stats.records[type] == 0u && m_stats.bytes[type] == 0u)
                continue;
            std::snprintf(line, sizeof(line), "[gs:rec]   0x%02X %-18s %10llu records %12llu bytes\n", type, RecordName(static_cast<uint8_t>(type)),
                          static_cast<unsigned long long>(m_stats.records[type]), static_cast<unsigned long long>(m_stats.bytes[type]));
            out += line;
        }
        std::snprintf(line, sizeof(line),
                      "[gs:rec]   states: %llu defined, %llu reused; blobs: %llu defined (%llu bytes), %llu reused (%llu bytes saved); vertices: %llu new, %llu same as before\n",
                      static_cast<unsigned long long>(m_stats.statesDefined), static_cast<unsigned long long>(m_stats.statesReused),
                      static_cast<unsigned long long>(m_stats.blobsDefined), static_cast<unsigned long long>(m_stats.blobBytesDefined),
                      static_cast<unsigned long long>(m_stats.blobsReused), static_cast<unsigned long long>(m_stats.blobBytesReused),
                      static_cast<unsigned long long>(m_stats.verticesNew), static_cast<unsigned long long>(m_stats.verticesReused));
        out += line;
        return out;
    }

    // ---------------------------------------------------------------------------------------
    // Reader
    bool Reader::openFile(const std::string &path, std::string *error)
    {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
        {
            if (error)
                *error = "cannot open " + path;
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size < 0)
        {
            std::fclose(f);
            if (error)
                *error = "cannot get the size of " + path;
            return false;
        }
        m_owned.resize(static_cast<size_t>(size));
        const size_t got = size > 0 ? std::fread(m_owned.data(), 1, m_owned.size(), f) : 0;
        std::fclose(f);
        if (got != m_owned.size())
        {
            if (error)
                *error = "short read of " + path;
            return false;
        }
        return openMemory(m_owned.data(), m_owned.size(), error);
    }

    bool Reader::openMemory(const uint8_t *data, size_t size, std::string *error)
    {
        m_data = data;
        m_size = size;
        m_pos = 0;
        m_states.clear();
        m_blobs.clear();
        m_hasState = false;
        m_prevVertex = {};
        if (size < kHeaderBytes || std::memcmp(data, kHeaderMagic, 8) != 0)
        {
            if (error)
                *error = "not a GS recording (bad magic)";
            return false;
        }
        m_version = get32(data + 8);
        m_vramSize = get32(data + 16);
        if (m_version != kVersion)
        {
            if (error)
                *error = "unsupported recording version " + std::to_string(m_version);
            return false;
        }
        if (m_vramSize == 0u || (m_vramSize % kPageSize) != 0u || m_vramSize > (64u << 20))
        {
            if (error)
                *error = "implausible VRAM size " + std::to_string(m_vramSize);
            return false;
        }
        m_pos = kHeaderBytes;
        return true;
    }

    bool Reader::readStart(std::vector<uint8_t> &vram, BackendState &state, std::string *error)
    {
        auto fail = [&](const char *why)
        {
            if (error)
                *error = why;
            return false;
        };
        Dec d{m_data + m_pos, m_data + m_size};
        uint8_t type = 0;
        d.u8(type);
        if (!d.ok || type != RT_StartVram)
            return fail("the first record is not StartVram");
        uint32_t pageSize = 0, pageCount = 0;
        d.u32(pageSize);
        d.u32(pageCount);
        if (!d.ok || pageSize != kPageSize || static_cast<uint64_t>(pageCount) * pageSize != m_vramSize)
            return fail("StartVram does not match the header");
        const size_t bitmapBytes = (pageCount + 7u) / 8u;
        if (!d.need(bitmapBytes))
            return fail("StartVram is cut short");
        const uint8_t *bitmap = d.p;
        d.p += bitmapBytes;
        vram.assign(m_vramSize, 0u);
        for (uint32_t page = 0; page < pageCount; ++page)
        {
            if ((bitmap[page >> 3] & (1u << (page & 7u))) == 0u)
                continue;
            if (!d.need(kPageSize))
                return fail("StartVram is cut short");
            std::memcpy(vram.data() + static_cast<size_t>(page) * kPageSize, d.p, kPageSize);
            d.p += kPageSize;
        }

        d.u8(type);
        if (!d.ok || type != RT_StartBackend)
            return fail("the second record is not StartBackend");
        for (uint16_t &v : state.clut)
            d.u16(v);
        d.u32(state.clutCbp[0]);
        d.u32(state.clutCbp[1]);
        d.u32(state.pageCacheBase);
        state.pageCacheBytes.clear();
        if (state.pageCacheBase != 0xFFFFFFFFu)
        {
            if (!d.need(kPageSize))
                return fail("StartBackend is cut short");
            state.pageCacheBytes.assign(d.p, d.p + kPageSize);
            d.p += kPageSize;
        }
        codecTransfer(d, state.transfer);
        d.u32(state.transferState.x);
        d.u32(state.transferState.y);
        d.u32(state.transferState.totalPixels);
        d.u32(state.transferState.copiedPixels);
        d.u32(state.transferState.direction);
        uint64_t pending = 0;
        d.u64(pending);
        state.transferState.localToHostPendingBytes = static_cast<size_t>(pending);
        d.u64(state.localToHostReadPos);
        uint32_t l2h = 0;
        d.u32(l2h);
        if (!d.ok || !d.need(l2h))
            return fail("StartBackend is cut short");
        state.localToHost.assign(d.p, d.p + l2h);
        d.p += l2h;
        if (!d.ok)
            return fail("StartBackend is cut short");
        m_pos = static_cast<size_t>(d.p - m_data);
        return true;
    }

    Reader::Status Reader::next(Record &r)
    {
        for (;;)
        {
            if (m_pos >= m_size)
                return Status::Truncated;
            Dec d{m_data + m_pos, m_data + m_size};
            uint8_t type = 0;
            d.u8(type);
            r.type = static_cast<RecordType>(type);
            switch (type)
            {
            case RT_End:
                d.u32(r.markerCount);
                if (!d.ok)
                    return Status::Truncated;
                m_pos = static_cast<size_t>(d.p - m_data);
                return Status::End;

            case RT_DrawState:
            {
                GSDrawState s{};
                codecDrawState(d, s);
                if (!d.ok)
                    return Status::Truncated;
                m_states.push_back(s);
                m_currentState = static_cast<uint32_t>(m_states.size() - 1u);
                m_hasState = true;
                m_pos = static_cast<size_t>(d.p - m_data);
                continue;
            }
            case RT_UseState:
            {
                uint32_t id = 0;
                d.u32(id);
                if (!d.ok)
                    return Status::Truncated;
                if (id >= m_states.size())
                    return Status::Corrupt;
                m_currentState = id;
                m_hasState = true;
                m_pos = static_cast<size_t>(d.p - m_data);
                continue;
            }
            case RT_Blob:
            {
                uint32_t size = 0;
                d.u32(size);
                if (!d.ok || !d.need(size))
                    return Status::Truncated;
                m_blobs.push_back({static_cast<size_t>(d.p - m_data), size});
                d.p += size;
                m_pos = static_cast<size_t>(d.p - m_data);
                continue;
            }

            case RT_Submit:
            {
                uint8_t header = 0;
                d.u8(header);
                const unsigned count = header & 3u;
                if (!d.ok)
                    return Status::Truncated;
                if (count == 0u || !m_hasState)
                    return Status::Corrupt;
                r.batch = GSPrimitiveBatch{};
                r.batch.vertexCount = static_cast<uint8_t>(count);
                r.batch.state = m_states[m_currentState];
                r.stateId = m_currentState;
                std::array<GSVertex, 3> cur{};
                for (unsigned i = 0; i < count; ++i)
                {
                    const unsigned code = (header >> (2u + 2u * i)) & 3u;
                    if (code == 0u)
                        decodeVertex(d, cur[i]);
                    else
                        cur[i] = m_prevVertex[code - 1u];
                    r.batch.vertices[i] = cur[i];
                }
                for (unsigned i = count; i < 3u; ++i)
                    if (((header >> (2u + 2u * i)) & 3u) != 0u)
                        return Status::Corrupt;
                if (!d.ok)
                    return Status::Truncated;
                m_prevVertex = cur; // slots past the count are default vertices
                m_pos = static_cast<size_t>(d.p - m_data);
                return Status::Ok;
            }
            case RT_LoadClut:
                r.tex0 = {};
                r.texclut = {};
                codecTex0(d, r.tex0);
                codecTexClut(d, r.texclut);
                break;
            case RT_BeginTransfer:
                r.transfer = {};
                codecTransfer(d, r.transfer);
                break;
            case RT_UploadImage:
            {
                d.u32(r.blobId);
                if (!d.ok)
                    return Status::Truncated;
                if (r.blobId >= m_blobs.size())
                    return Status::Corrupt;
                r.data = m_data + m_blobs[r.blobId].offset;
                r.size = m_blobs[r.blobId].size;
                break;
            }
            case RT_Flush:
            case RT_TextureFlush:
            case RT_Reset:
                break;
            case RT_Sync:
            {
                uint8_t reason = 0;
                d.u8(reason);
                r.syncReason = static_cast<GSSyncReason>(reason);
                break;
            }
            case RT_Marker:
                r.request = {};
                d.u32(r.marker);
                d.u64(r.hostNs);
                codecRequest(d, r.request);
                break;
            case RT_ClearFramebuffer:
                r.context = {};
                codecContext(d, r.context);
                d.u32(r.rgba);
                d.b(r.result);
                break;
            case RT_WriteVram:
            case RT_ReadVram:
            {
                uint8_t psm = 0;
                d.u8(psm);
                r.psm = psm;
                d.u32(r.base);
                d.u32(r.bw);
                d.u32(r.x);
                d.u32(r.y);
                d.u32(r.value);
                break;
            }
            case RT_ConsumeLocalToHost:
                d.u32(r.maxBytes);
                d.u32(r.size);
                if (!d.ok || !d.need(r.size))
                    return Status::Truncated;
                r.data = d.p;
                d.p += r.size;
                break;
            case RT_LiveFrame:
                d.u32(r.marker);
                d.u32(r.width);
                d.u32(r.height);
                d.u64(r.hash);
                break;
            case RT_LiveDisplay:
                d.u32(r.marker);
                d.u64(r.hash);
                break;
            default:
                return Status::Corrupt;
            }
            if (!d.ok)
                return Status::Truncated;
            m_pos = static_cast<size_t>(d.p - m_data);
            return Status::Ok;
        }
    }
}
