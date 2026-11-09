#include "MiniTest.h"

#include "runtime/gs/gs_png.h"
#include "runtime/gs/gs_tex_dump.h"
#include "runtime/gs/gs_tex_keys.h"
#include "runtime/gs/gs_tex_pack.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/ps2_gs_transfer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// Tests for texture replacement (task t1b): the PNG codec (gs_png.h), the key tracker and the replaced rectangle (gs_tex_keys.h), the
// naming and size rules and the store of texture packs (gs_tex_pack.h). The renderer and the shader are covered by the scripted game
// runs, not here.

namespace
{
    // PNG test vectors made with Python's zlib (level 9: dynamic Huffman blocks) and every row filter; see
    // baserom/out/tmp/t1b_make_png_vectors.py. 24 x 24 unless said.
#include "gs_tex_pack_vectors.inc"

    constexpr int kW = 24, kH = 24;

    GSTransferCommand upload(uint32_t dbp, uint32_t dbw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
    {
        GSTransferCommand c;
        c.bitbltbuf.dbp = dbp;
        c.bitbltbuf.dbw = static_cast<uint8_t>(dbw);
        c.bitbltbuf.dpsm = static_cast<uint8_t>(psm);
        c.trxpos.dsax = static_cast<uint16_t>(x);
        c.trxpos.dsay = static_cast<uint16_t>(y);
        c.trxreg.rrw = static_cast<uint16_t>(w);
        c.trxreg.rrh = static_cast<uint16_t>(h);
        c.direction = 0;
        return c;
    }

    // A host-to-local transfer of `bytes` filled with `fill` (plus the index so that two transfers never hash the same by accident).
    void doUpload(TexKeys::Tracker &tracker, const GSTransferCommand &c, uint8_t fill, uint64_t marker = 1)
    {
        tracker.BeginTransfer(c, marker);
        size_t pixelBytes = 1;
        if (c.bitbltbuf.dpsm == GS_PSM_CT32)
            pixelBytes = 4;
        else if (c.bitbltbuf.dpsm == GS_PSM_T4)
            pixelBytes = 0; // half a byte
        const size_t pixels = static_cast<size_t>(c.trxreg.rrw) * c.trxreg.rrh;
        const size_t bytes = pixelBytes ? pixels * pixelBytes : pixels / 2;
        std::vector<uint8_t> data(bytes);
        for (size_t i = 0; i < bytes; ++i)
            data[i] = static_cast<uint8_t>(fill + i * 7);
        // delivered in two chunks, to show that the hash does not depend on the chunking
        const size_t first = bytes / 3;
        if (first)
            tracker.UploadData(data.data(), static_cast<uint32_t>(first));
        tracker.UploadData(data.data() + first, static_cast<uint32_t>(bytes - first));
    }

    TexKeys::TextureDesc desc(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t w, uint32_t h)
    {
        TexKeys::TextureDesc d;
        d.tbp = tbp;
        d.tbw = tbw;
        d.psm = psm;
        d.width = w;
        d.height = h;
        return d;
    }

    std::vector<uint8_t> bytesOf(const uint8_t *data, size_t n)
    {
        return std::vector<uint8_t>(data, data + n);
    }

    // A FileSource that holds files in memory, with a provider and load order for each.
    class MemorySource final : public TexPack::FileSource
    {
    public:
        struct File
        {
            std::vector<uint8_t> bytes;
            std::string provider;
            uint32_t order = 0;
        };
        std::map<std::string, File> files;
        std::vector<std::pair<std::string, std::string>> problems;

        void add(const std::string &name, std::vector<uint8_t> bytes, const std::string &provider = "mod.a", uint32_t order = 0)
        {
            files[std::string(TexPack::kTexturePrefix) + name] = File{std::move(bytes), provider, order};
        }
        std::vector<Entry> list(const std::string &prefix) override
        {
            std::vector<Entry> out;
            for (const auto &f : files)
                if (f.first.compare(0, prefix.size(), prefix) == 0)
                    out.push_back(Entry{f.first, f.second.provider, f.second.order});
            return out;
        }
        bool read(const std::string &path, std::vector<uint8_t> &out, std::string &error) override
        {
            const auto it = files.find(path);
            if (it == files.end())
            {
                error = "no such file";
                return false;
            }
            out = it->second.bytes;
            return true;
        }
        void problem(const std::string &provider, const std::string &message) override { problems.emplace_back(provider, message); }
    };

    std::vector<uint8_t> rgbaPng(int w, int h, uint8_t seed, uint8_t alpha = 255)
    {
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < px.size(); i += 4)
        {
            px[i] = static_cast<uint8_t>(seed + i);
            px[i + 1] = static_cast<uint8_t>(seed * 3 + i);
            px[i + 2] = static_cast<uint8_t>(seed * 5);
            px[i + 3] = alpha;
        }
        return GSPng::Encode(static_cast<uint32_t>(w), static_cast<uint32_t>(h), 6, px);
    }

    std::vector<uint8_t> greyPng(int w, int h, uint8_t value)
    {
        std::vector<uint8_t> px(static_cast<size_t>(w) * h, value);
        return GSPng::Encode(static_cast<uint32_t>(w), static_cast<uint32_t>(h), 0, px);
    }

    std::string hex16(uint64_t key)
    {
        return TexKeys::KeyName(key);
    }
}

void register_gs_tex_pack_tests()
{
    MiniTest::Case("TexPng", [](TestCase &tc)
    {
        tc.Run("the encoder's output decodes to the same pixels (RGBA, grey, paletted)", [](TestCase &t)
        {
            std::vector<uint8_t> px(37 * 11 * 4);
            for (size_t i = 0; i < px.size(); ++i)
                px[i] = static_cast<uint8_t>((i * 37) ^ (i >> 3));
            const auto png = GSPng::Encode(37, 11, 6, px);
            GSPng::Image img;
            std::string error;
            t.IsTrue(GSPng::DecodeRgba(png.data(), png.size(), img, error), "RGBA decodes: " + error);
            t.IsTrue(img.width == 37 && img.height == 11 && img.pixels == px, "same pixels");
            GSPng::Info info;
            t.IsTrue(GSPng::ReadInfo(png.data(), png.size(), info, error) && info.width == 37 && info.height == 11 && info.colorType == 6, "header");

            std::vector<uint8_t> idx(29 * 5);
            for (size_t i = 0; i < idx.size(); ++i)
                idx[i] = static_cast<uint8_t>(i * 13);
            const auto grey = GSPng::Encode(29, 5, 0, idx);
            GSPng::Image gi;
            t.IsTrue(GSPng::DecodeIndex(grey.data(), grey.size(), gi, error) && gi.pixels == idx && gi.format == GSPng::Image::Format::Index8, "grey as index");
            std::vector<uint8_t> plte, trns;
            for (int i = 0; i < 256; ++i)
            {
                plte.insert(plte.end(), {static_cast<uint8_t>(i), static_cast<uint8_t>(255 - i), 7});
                trns.push_back(static_cast<uint8_t>(i));
            }
            const auto pal = GSPng::Encode(29, 5, 3, idx, plte, trns);
            GSPng::Image pi;
            t.IsTrue(GSPng::DecodeIndex(pal.data(), pal.size(), pi, error) && pi.pixels == idx && pi.wasPaletted, "paletted as index: the values are the palette indices");
            GSPng::Image pc;
            t.IsTrue(GSPng::DecodeRgba(pal.data(), pal.size(), pc, error), "paletted as colour");
            const uint8_t first = idx[0];
            t.IsTrue(pc.pixels[0] == first && pc.pixels[1] == 255 - first && pc.pixels[2] == 7 && pc.pixels[3] == first, "palette and tRNS applied");
        });

        tc.Run("zlib streams from another encoder: dynamic Huffman, every filter, all colour types", [](TestCase &t)
        {
            GSPng::Image img;
            std::string error;
            t.IsTrue(GSPng::DecodeRgba(kPngRgba8, sizeof(kPngRgba8), img, error), "RGBA8: " + error);
            bool same = img.width == kW && img.height == kH;
            for (int y = 0; same && y < kH; ++y)
                for (int x = 0; x < kW; ++x)
                {
                    const uint8_t *p = &img.pixels[(static_cast<size_t>(y) * kW + x) * 4];
                    if (p[0] != ((x * 10) & 255) || p[1] != ((y * 10) & 255) || p[2] != (((x ^ y) * 9) & 255) || p[3] != ((255 - 7 * x) & 255))
                        same = false;
                }
            t.IsTrue(same, "RGBA8 pixels (rows use filters 0 to 4)");

            GSPng::Image idx;
            t.IsTrue(GSPng::DecodeIndex(kPngPal8, sizeof(kPngPal8), idx, error), "paletted 8: " + error);
            same = idx.width == kW;
            for (int y = 0; same && y < kH; ++y)
                for (int x = 0; x < kW; ++x)
                    if (idx.pixels[static_cast<size_t>(y) * kW + x] != (x * 5 + y * 3) % 40)
                        same = false;
            t.IsTrue(same, "paletted 8: the indices");
            GSPng::Image col;
            t.IsTrue(GSPng::DecodeRgba(kPngPal8, sizeof(kPngPal8), col, error), "paletted 8 as colour");
            // pixel (1, 0): index 5: palette (30, 225, 15), alpha 255 - 30 = 225
            t.IsTrue(col.pixels[4] == 30 && col.pixels[5] == 225 && col.pixels[6] == 15 && col.pixels[7] == 225, "palette and tRNS");

            GSPng::Image p4;
            t.IsTrue(GSPng::DecodeIndex(kPngPal4, sizeof(kPngPal4), p4, error), "paletted 4: " + error);
            same = p4.width == 21 && p4.height == kH;
            for (int y = 0; same && y < kH; ++y)
                for (int x = 0; x < 21; ++x)
                    if (p4.pixels[static_cast<size_t>(y) * 21 + x] != (x + y) % 16)
                        same = false;
            t.IsTrue(same, "4-bit samples unpacked, rows end mid-byte");

            GSPng::Image g8;
            t.IsTrue(GSPng::DecodeIndex(kPngGrey8, sizeof(kPngGrey8), g8, error), "grey 8: " + error);
            same = true;
            for (int y = 0; same && y < kH; ++y)
                for (int x = 0; x < kW; ++x)
                    if (g8.pixels[static_cast<size_t>(y) * kW + x] != ((x * 11 + y * 7) & 255))
                        same = false;
            t.IsTrue(same, "grey 8 values");

            GSPng::Image c16;
            t.IsTrue(GSPng::DecodeRgba(kPngRgba16, sizeof(kPngRgba16), c16, error), "RGBA16: " + error);
            t.IsTrue(c16.pixels[0] == 0 && c16.pixels[1] == 0 && c16.pixels[2] == 77 && c16.pixels[3] == 200, "16 bit keeps the high byte");
            t.IsTrue(c16.pixels[4] == 10 && c16.pixels[5] == 0, "second pixel");
        });

        tc.Run("what it refuses, with a reason", [](TestCase &t)
        {
            GSPng::Image img;
            std::string error;
            t.IsFalse(GSPng::DecodeRgba(kPngInterlaced, sizeof(kPngInterlaced), img, error), "interlaced");
            t.IsTrue(error.find("interlac") != std::string::npos, "the reason names interlacing: " + error);

            std::vector<uint8_t> bad = bytesOf(kPngRgba8, sizeof(kPngRgba8));
            bad[bad.size() - 20] ^= 0x40; // inside IDAT or its checksum
            t.IsFalse(GSPng::DecodeRgba(bad.data(), bad.size(), img, error), "damaged data");
            t.IsFalse(error.empty(), "with a reason: " + error);

            std::vector<uint8_t> cut = bytesOf(kPngRgba8, sizeof(kPngRgba8));
            cut.resize(cut.size() / 2);
            t.IsFalse(GSPng::DecodeRgba(cut.data(), cut.size(), img, error), "truncated");
            t.IsTrue(error.find("short") != std::string::npos || error.find("cut") != std::string::npos, "says it is cut short: " + error);

            const uint8_t notPng[16] = {'G', 'I', 'F', '8', '9', 'a'};
            t.IsFalse(GSPng::DecodeRgba(notPng, sizeof(notPng), img, error), "not a PNG");
            t.IsTrue(error.find("not a PNG") != std::string::npos, error);

            // an RGBA picture is not an index image; 16-bit RGBA neither
            t.IsFalse(GSPng::DecodeIndex(kPngRgba8, sizeof(kPngRgba8), img, error), "RGBA as index");
            t.IsTrue(error.find("greyscale or paletted") != std::string::npos, error);
        });

        tc.Run("inflate: stored and fixed blocks, and the output bound", [](TestCase &t)
        {
            std::string error;
            std::vector<uint8_t> out;
            const uint8_t stored[] = {0x01, 0x03, 0x00, 0xFC, 0xFF, 'a', 'b', 'c'};
            t.IsTrue(GSPng::Inflate(stored, sizeof(stored), out, 100, error) && out == (std::vector<uint8_t>{'a', 'b', 'c'}), "a stored block");
            std::vector<uint8_t> text(5000);
            for (size_t i = 0; i < text.size(); ++i)
                text[i] = static_cast<uint8_t>("abcabcabd"[i % 9]);
            std::vector<uint8_t> packed;
            GSPng::DeflateFixed(text.data(), text.size(), packed);
            t.IsTrue(packed.size() < text.size() / 4, "the fixed-Huffman encoder compresses repeats");
            out.clear();
            t.IsTrue(GSPng::Inflate(packed.data(), packed.size(), out, text.size(), error) && out == text, "fixed block with matches round-trips");
            out.clear();
            t.IsFalse(GSPng::Inflate(packed.data(), packed.size(), out, 100, error), "more data than the bound is an error");
        });
    });

    MiniTest::Case("TexKeys", [](TestCase &tc)
    {
        tc.Run("the writer's rectangle is the replaced rectangle, and stale blocks are ignored", [](TestCase &t)
        {
            // a 64 x 64 T8 texture (TBW 2) of which a 48 x 48 image was uploaded, over an older 64 x 64 upload
            TexKeys::Tracker a;
            doUpload(a, upload(6720, 2, GS_PSM_T8, 0, 0, 64, 64), 1, 1);
            doUpload(a, upload(6720, 2, GS_PSM_T8, 0, 0, 48, 48), 50, 2);
            const TexKeys::LiveKey ka = a.KeyFor(desc(6720, 2, GS_PSM_T8, 64, 64));
            t.IsTrue(ka.status == TexKeys::LiveKey::Status::Ok, "keyed");
            t.IsTrue(ka.rect.x0 == 0 && ka.rect.y0 == 0 && ka.rect.width() == 48 && ka.rect.height() == 48, "the rectangle is the 48 x 48 image");
            t.IsTrue(ka.writers == 1 && ka.exact, "one exact writer; the older 64 x 64 upload is a leftover");

            TexKeys::Tracker b; // a fresh tracker that only saw the second upload
            doUpload(b, upload(6720, 2, GS_PSM_T8, 0, 0, 48, 48), 50, 2);
            const TexKeys::LiveKey kb = b.KeyFor(desc(6720, 2, GS_PSM_T8, 64, 64));
            t.Equals(ka.k3i, kb.k3i, "the leftover does not change the key");

            TexKeys::Tracker c; // the same image uploaded somewhere else in GS memory
            doUpload(c, upload(7744, 2, GS_PSM_T8, 0, 0, 48, 48), 50, 9);
            t.Equals(c.KeyFor(desc(7744, 2, GS_PSM_T8, 64, 64)).k3i, ka.k3i, "the key does not depend on where the texture sits");

            TexKeys::Tracker d; // other bytes, same place
            doUpload(d, upload(6720, 2, GS_PSM_T8, 0, 0, 48, 48), 51, 2);
            t.IsTrue(d.KeyFor(desc(6720, 2, GS_PSM_T8, 64, 64)).k3i != ka.k3i, "other bytes, other key");

            TexKeys::Tracker e; // a smaller image of the same bytes would be a different texture
            doUpload(e, upload(6720, 2, GS_PSM_T8, 0, 0, 32, 32), 50, 2);
            t.IsTrue(e.KeyFor(desc(6720, 2, GS_PSM_T8, 64, 64)).k3i != ka.k3i, "another size, another key");
        });

        tc.Run("no writer, memory reset, copies inside GS memory", [](TestCase &t)
        {
            TexKeys::Tracker a;
            t.IsTrue(a.KeyFor(desc(6720, 2, GS_PSM_T8, 64, 64)).status == TexKeys::LiveKey::Status::NoWriter, "nothing uploaded");
            doUpload(a, upload(6720, 2, GS_PSM_T8, 0, 0, 64, 64), 3);
            t.IsTrue(a.KeyFor(desc(6720, 2, GS_PSM_T8, 64, 64)).status == TexKeys::LiveKey::Status::Ok, "uploaded");
            GSTransferCommand copy = upload(6720, 2, GS_PSM_T8, 0, 0, 16, 16);
            copy.direction = 2;
            a.BeginTransfer(copy, 5);
            t.IsTrue(a.KeyFor(desc(6720, 2, GS_PSM_T8, 64, 64)).status == TexKeys::LiveKey::Status::NoWriter, "a copy inside GS memory makes the first block unknown");
            doUpload(a, upload(6720, 2, GS_PSM_T8, 0, 0, 64, 64), 3);
            a.MemoryReset();
            t.IsTrue(a.KeyFor(desc(6720, 2, GS_PSM_T8, 64, 64)).status == TexKeys::LiveKey::Status::NoWriter, "after a reset");
            doUpload(a, upload(6720, 2, GS_PSM_T8, 0, 0, 64, 64), 3);
            a.WritePixel(GS_PSM_T8, 6720, 2, 3, 3);
            t.IsTrue(a.KeyFor(desc(6720, 2, GS_PSM_T8, 64, 64)).status == TexKeys::LiveKey::Status::NoWriter, "a pixel written by the runtime");
        });

        tc.Run("a texture cut out of a blob uploaded as 32-bit colour covers the blocks that blob wrote", [](TestCase &t)
        {
            TexKeys::Tracker a;
            // a CT32 blob 64 wide, 64 high (two pages), and a 128 x 64 T8 texture (one page) in its second page
            doUpload(a, upload(6720, 1, GS_PSM_CT32, 0, 0, 64, 64), 9);
            const TexKeys::LiveKey k = a.KeyFor(desc(6720 + 32, 2, GS_PSM_T8, 128, 64));
            t.IsTrue(k.status == TexKeys::LiveKey::Status::Ok, "keyed");
            t.IsTrue(k.rect.x0 == 0 && k.rect.y0 == 0 && k.rect.width() == 128 && k.rect.height() == 64, "the whole page");
            t.IsFalse(k.exact, "block granularity is not exact");
            const TexKeys::LiveKey k2 = a.KeyFor(desc(6720, 2, GS_PSM_T8, 128, 64));
            t.IsTrue(k2.status == TexKeys::LiveKey::Status::Ok && k2.k3i != k.k3i, "the first page is another texture (another position in the blob)");
        });

        tc.Run("composed textures get no key: pages of a canvas, tiles", [](TestCase &t)
        {
            TexKeys::Tracker a;
            // a 512 x 512 T4 canvas (TBW 8) built from 128 x 128 T4 pages uploaded at buffer width 2
            doUpload(a, upload(15872, 2, GS_PSM_T4, 0, 0, 128, 128), 4);
            doUpload(a, upload(15904, 2, GS_PSM_T4, 0, 0, 128, 128), 5);
            const TexKeys::LiveKey k = a.KeyFor(desc(15872, 8, GS_PSM_T4, 512, 512));
            t.IsTrue(k.status == TexKeys::LiveKey::Status::Composed, "a canvas is composed");

            TexKeys::Tracker b; // 20 tiles of a CT32 texture
            for (uint32_t i = 0; i < 20; ++i)
                doUpload(b, upload(6720, 1, GS_PSM_CT32, (i % 4) * 16, (i / 4) * 16, 16, 16), static_cast<uint8_t>(i));
            t.IsTrue(b.KeyFor(desc(6720, 4, GS_PSM_CT32, 256, 256)).status == TexKeys::LiveKey::Status::Composed, "many writers");
        });

        tc.Run("two chunks, the valid width, and a texture in the middle of a buffer", [](TestCase &t)
        {
            TexKeys::Tracker a;
            // a 640 x 448 T8 image uploaded in two transfers of 640 x 256 and 640 x 192, declared 1024 x 512 (TBW 10)
            doUpload(a, upload(7744, 10, GS_PSM_T8, 0, 0, 640, 256), 1);
            doUpload(a, upload(7744, 10, GS_PSM_T8, 0, 256, 640, 192), 2);
            const TexKeys::LiveKey k = a.KeyFor(desc(7744, 10, GS_PSM_T8, 1024, 512));
            t.IsTrue(k.status == TexKeys::LiveKey::Status::Ok && k.writers == 2, "two writers");
            t.Equals(k.validWidth, 640u, "only 640 of the 1024 declared columns are real");
            t.IsTrue(k.rect.x0 == 0 && k.rect.y0 == 0 && k.rect.width() == 640 && k.rect.height() == 448, "the image is 640 x 448");

            // a 256 x 128 T8 image uploaded at buffer width 4; a 128 x 64 texture declared at its second page
            TexKeys::Tracker b;
            doUpload(b, upload(6720, 4, GS_PSM_T8, 0, 0, 256, 128), 3);
            const TexKeys::LiveKey whole = b.KeyFor(desc(6720, 4, GS_PSM_T8, 256, 128));
            t.IsTrue(whole.rect.width() == 256 && whole.rect.height() == 128, "the whole upload");
            const TexKeys::LiveKey inner = b.KeyFor(desc(6720 + 32, 4, GS_PSM_T8, 128, 64));
            t.IsTrue(inner.status == TexKeys::LiveKey::Status::Ok && inner.rect.width() == 128 && inner.rect.height() == 64, "a texture one page into the buffer, cut to its own size");
            t.IsTrue(inner.k3i != whole.k3i, "and a different key");
        });

        tc.Run("the block map agrees with what a transfer really changes in GS memory", [](TestCase &t)
        {
            // For several uploads of different formats and places: every 256-byte block whose bytes changed is one the tracker
            // credits to the transfer (the census's verify check).
            struct Case
            {
                uint32_t dbp, dbw, psm, x, y, w, h;
            };
            const Case cases[] = {{6720, 2, GS_PSM_T8, 0, 0, 48, 48},    {7744, 10, GS_PSM_T8, 0, 0, 640, 448},   {15872, 2, GS_PSM_T4, 0, 0, 128, 128},
                                  {8192, 1, GS_PSM_CT32, 0, 0, 64, 224}, {9000, 4, GS_PSM_CT16, 16, 8, 100, 50},     {10000, 4, GS_PSM_T8, 33, 17, 90, 70}};
            std::vector<uint8_t> vram(4u * 1024 * 1024, 0xEE);
            for (const Case &c : cases)
            {
                TexKeys::Tracker tracker;
                GSTransferCommand cmd = upload(c.dbp, c.dbw, c.psm, c.x, c.y, c.w, c.h);
                tracker.BeginTransfer(cmd, 1);
                std::vector<uint8_t> before = vram;
                GSTransferSnapshot state;
                GSTransfer::Begin(cmd, state);
                const size_t bits = c.psm == GS_PSM_CT32 ? 32 : (c.psm == GS_PSM_CT16 ? 16 : (c.psm == GS_PSM_T8 ? 8 : 4));
                const size_t bytes = static_cast<size_t>(c.w) * c.h * bits / 8;
                std::vector<uint8_t> data(bytes);
                for (size_t i = 0; i < bytes; ++i)
                    data[i] = static_cast<uint8_t>(i * 31 + 1);
                GSTransfer::UploadImage(vram.data(), cmd, state, data.data(), static_cast<uint32_t>(bytes));
                tracker.UploadData(data.data(), static_cast<uint32_t>(bytes));
                std::vector<bool> predicted(TexKeys::kBlocks, false);
                TexKeys::ForEachBlock(c.psm, c.dbp, c.dbw, static_cast<int>(c.x), static_cast<int>(c.y), static_cast<int>(c.w), static_cast<int>(c.h),
                                      [&](uint32_t block, int, int, int, int) { predicted[block] = true; });
                int changed = 0, outside = 0;
                for (uint32_t block = 0; block < TexKeys::kBlocks; ++block)
                    if (std::memcmp(&before[block * 256u], &vram[block * 256u], 256u) != 0)
                    {
                        ++changed;
                        if (!predicted[block])
                            ++outside;
                    }
                t.IsTrue(changed > 0, "the upload changed memory");
                t.Equals(outside, 0, "no changed block outside the prediction (psm " + std::to_string(c.psm) + ")");
                vram.assign(vram.size(), 0xEE);
            }
        });

        tc.Run("k3_c depends on the palette entries the indices use, and only on those", [](TestCase &t)
        {
            const std::vector<uint8_t> indices = {0, 1, 1, 2, 2, 2, 5, 0};
            std::vector<uint32_t> palette(256);
            for (uint32_t i = 0; i < 256; ++i)
                palette[i] = 0xFF000000u | (i * 0x010203u);
            const uint64_t base = TexKeys::ColouringKey(1234, indices, palette.data());
            std::vector<uint32_t> other = palette;
            other[9] ^= 0x00FFFFFFu; // an entry the part never uses
            t.Equals(TexKeys::ColouringKey(1234, indices, other.data()), base, "an unused entry changes nothing");
            other = palette;
            other[2] ^= 0x00001111u; // a used one
            t.IsTrue(TexKeys::ColouringKey(1234, indices, other.data()) != base, "a used entry changes the key");
            t.IsTrue(TexKeys::ColouringKey(1235, indices, palette.data()) != base, "and the index key is part of it");
        });

        tc.Run("ReadTexels returns what the transfer wrote (indices and colours)", [](TestCase &t)
        {
            std::vector<uint8_t> vram(4u * 1024 * 1024, 0);
            GSTransferCommand cmd = upload(6720, 2, GS_PSM_T8, 0, 0, 40, 30);
            GSTransferSnapshot state;
            GSTransfer::Begin(cmd, state);
            std::vector<uint8_t> data(40 * 30);
            for (size_t i = 0; i < data.size(); ++i)
                data[i] = static_cast<uint8_t>(i * 5);
            GSTransfer::UploadImage(vram.data(), cmd, state, data.data(), static_cast<uint32_t>(data.size()));
            std::vector<uint8_t> out;
            TexKeys::Rect r{0, 0, 39, 29};
            TexKeys::ReadTexels(vram.data(), desc(6720, 2, GS_PSM_T8, 64, 64), GSTexaReg{}, r, out);
            t.IsTrue(out == data, "the indices of the rectangle");
            TexKeys::Rect inner{5, 3, 20, 10};
            TexKeys::ReadTexels(vram.data(), desc(6720, 2, GS_PSM_T8, 64, 64), GSTexaReg{}, inner, out);
            bool same = out.size() == 16u * 8u;
            for (int y = 3; same && y <= 10; ++y)
                for (int x = 5; x <= 20; ++x)
                    if (out[static_cast<size_t>(y - 3) * 16 + static_cast<size_t>(x - 5)] != data[static_cast<size_t>(y) * 40 + static_cast<size_t>(x)])
                        same = false;
            t.IsTrue(same, "an inner rectangle");
        });

        tc.Run("alpha between PNG and the GS", [](TestCase &t)
        {
            for (uint32_t a = 0; a <= 0x80; ++a)
                t.Equals(static_cast<uint32_t>(TexDump::FromPngAlpha(TexDump::ToPngAlpha(a))), a, "round trip " + std::to_string(a));
            t.Equals(static_cast<uint32_t>(TexDump::ToPngAlpha(0x80)), 255u, "opaque");
        });
    });

    MiniTest::Case("TexPack", [](TestCase &tc)
    {
        tc.Run("file names", [](TestCase &t)
        {
            TexPack::ParsedName n;
            t.IsTrue(TexPack::ParseFileName("0123456789abcdef.png", n) && n.key == 0x0123456789abcdefull && !n.index, "plain");
            t.IsTrue(TexPack::ParseFileName("0123456789ABCDEF.idx.png", n) && n.key == 0x0123456789abcdefull && n.index, "index image, capitals");
            t.IsTrue(TexPack::ParseFileName("0123456789abcdef.PNG", n) && !n.index, "extension in capitals");
            t.IsFalse(TexPack::ParseFileName("0123456789abcde.png", n), "15 digits");
            t.IsFalse(TexPack::ParseFileName("0123456789abcdefg.png", n), "17 digits");
            t.IsFalse(TexPack::ParseFileName("0123456789abcdeg.png", n), "not hex");
            t.IsFalse(TexPack::ParseFileName("0123456789abcdef.jpg", n), "not a PNG");
            t.IsFalse(TexPack::ParseFileName("0123456789abcdef.pal.png", n), "the dumper's palette picture is not a pack file");
            t.IsFalse(TexPack::ParseFileName("0123456789abcdef__12345678.png", n), "nor is a numbered colouring");
            t.IsFalse(TexPack::ParseFileName("readme.txt", n), "other files");
        });

        tc.Run("the scale rule", [](TestCase &t)
        {
            std::string why;
            t.Equals(TexPack::ScaleOf(48, 48, 48, 48, &why), 1, "1x");
            t.Equals(TexPack::ScaleOf(96, 96, 48, 48, &why), 2, "2x");
            t.Equals(TexPack::ScaleOf(192, 192, 48, 48, &why), 4, "4x");
            t.Equals(TexPack::ScaleOf(384, 384, 48, 48, &why), 8, "8x");
            t.Equals(TexPack::ScaleOf(144, 96, 48, 32, &why), 3, "3x of a non-square rectangle");
            t.Equals(TexPack::ScaleOf(432, 432, 48, 48, &why), 0, "9x is too much");
            t.IsTrue(why.find("1 to 8") != std::string::npos, "says 1 to 8: " + why);
            t.Equals(TexPack::ScaleOf(100, 96, 48, 48, &why), 0, "not a whole number");
            t.IsTrue(why.find("100x96") != std::string::npos && why.find("48x48") != std::string::npos, "names both sizes: " + why);
            t.Equals(TexPack::ScaleOf(96, 144, 48, 48, &why), 0, "different in x and y");
            t.IsTrue(why.find("both directions") != std::string::npos, why);
            t.Equals(TexPack::ScaleOf(24, 24, 48, 48, &why), 0, "smaller than the original");
            t.Equals(TexPack::ScaleOf(0, 0, 48, 48, &why), 0, "empty");
            t.Equals(TexPack::ScaleOf(48, 48, 0, 0, &why), 0, "no rectangle");
        });

        tc.Run("the store: lookups, pending then ready, alpha conversion", [](TestCase &t)
        {
            auto src = std::make_shared<MemorySource>();
            const uint64_t key = 0x1111222233334444ull;
            src->add(hex16(key) + ".png", rgbaPng(8, 8, 17, 255));
            src->add("notes.txt", {1, 2, 3});
            TexPack::Store::Options opt;
            opt.prefetch = false;
            TexPack::Store store(src, opt);
            t.IsFalse(store.empty(), "one key");
            t.Equals(store.stats().files, 1u, "one file");
            t.Equals(store.stats().ignored, 1u, "one ignored");
            TexPack::Lookup none = store.find(0x99, 0, 4, 4, false, 0);
            t.IsTrue(none.state == TexPack::Lookup::State::None, "an unknown key");
            TexPack::Lookup first = store.find(key, 0, 4, 4, false, 0);
            t.IsTrue(first.state == TexPack::Lookup::State::Pending, "first ask: pending");
            store.waitIdle();
            TexPack::Lookup second = store.find(key, 0, 4, 4, false, 0);
            t.IsTrue(second.state == TexPack::Lookup::State::Ready && second.scale == 2 && second.image && second.image->width == 8, "second ask: ready at 2x");
            t.IsTrue(second.image && second.image->kind == TexPack::Kind::Rgba && second.image->pixels[3] == 0x80, "PNG alpha 255 is the GS's 0x80");
            TexPack::Lookup wrong = store.find(key, 0, 5, 5, false, 0);
            t.IsTrue(wrong.state == TexPack::Lookup::State::Refused, "a rectangle the picture does not fit: refused");
            t.IsFalse(src->problems.empty(), "the refusal went to the source");
            if (!src->problems.empty())
                t.IsTrue(src->problems[0].first == "mod.a" && src->problems[0].second.find("8x8") != std::string::npos, "naming the mod and the size: " + src->problems[0].second);
            t.Equals(store.stats().refused, 1u, "counted");
            store.find(key, 0, 5, 5, false, 0);
            t.Equals(src->problems.size(), static_cast<size_t>(1), "the same refusal is not repeated");
        });

        tc.Run("index images: palette bound, indexed textures only", [](TestCase &t)
        {
            auto src = std::make_shared<MemorySource>();
            const uint64_t good = 0xAAAA000000000001ull, big = 0xAAAA000000000002ull, plain = 0xAAAA000000000003ull;
            src->add(hex16(good) + ".idx.png", greyPng(8, 8, 5));
            src->add(hex16(big) + ".idx.png", greyPng(8, 8, 40));
            src->add(hex16(plain) + ".png", rgbaPng(8, 8, 1));
            TexPack::Store::Options opt;
            opt.prefetch = false;
            TexPack::Store store(src, opt);
            for (uint64_t k : {good, big, plain})
                store.find(k, 0, 4, 4, true, 16);
            store.waitIdle();
            const TexPack::Lookup ok = store.find(good, 0, 4, 4, true, 16);
            t.IsTrue(ok.state == TexPack::Lookup::State::Ready && ok.image && ok.image->kind == TexPack::Kind::Index && ok.image->maxIndex == 5, "an index image for a 16-entry palette");
            t.IsTrue(store.find(big, 0, 4, 4, true, 16).state == TexPack::Lookup::State::Refused, "index 40 does not exist in a 16-entry palette");
            t.IsTrue(store.find(big, 0, 4, 4, true, 256).state == TexPack::Lookup::State::Ready, "but it does in a 256-entry one");
            t.IsTrue(store.find(good, 0, 4, 4, false, 0).state == TexPack::Lookup::State::Refused, "an index image for a texture without a palette");
            t.IsTrue(store.find(plain, 0, 4, 4, true, 16).state == TexPack::Lookup::State::Ready, "a plain picture for an indexed texture is fine");
        });

        tc.Run("k3_c wins over k3_i, and a refused k3_c falls back", [](TestCase &t)
        {
            auto src = std::make_shared<MemorySource>();
            const uint64_t k3i = 0xBBBB000000000001ull, k3c = 0xBBBB000000000002ull;
            // k3_i is provided by a later mod than k3_c: the more specific key still wins
            src->add(hex16(k3c) + ".png", rgbaPng(8, 8, 3), "mod.early", 0);
            src->add(hex16(k3i) + ".png", rgbaPng(8, 8, 4), "mod.late", 1);
            TexPack::Store::Options opt;
            opt.prefetch = false;
            TexPack::Store store(src, opt);
            store.find(k3i, k3c, 4, 4, true, 16);
            store.waitIdle();
            TexPack::Lookup r = store.find(k3i, k3c, 4, 4, true, 16);
            t.IsTrue(r.state == TexPack::Lookup::State::Ready && r.colouring && r.image && r.image->provider == "mod.early", "k3_c wins whichever mod loads last");
            r = store.find(k3i, 0, 4, 4, true, 16);
            store.waitIdle();
            r = store.find(k3i, 0, 4, 4, true, 16);
            t.IsTrue(r.state == TexPack::Lookup::State::Ready && !r.colouring && r.image->provider == "mod.late", "without a colouring key: the k3_i file");
            // a k3_c file of the wrong size is refused, and the k3_i file is used
            auto src2 = std::make_shared<MemorySource>();
            src2->add(hex16(k3c) + ".png", rgbaPng(9, 9, 3), "mod.early", 0);
            src2->add(hex16(k3i) + ".png", rgbaPng(8, 8, 4), "mod.late", 1);
            TexPack::Store store2(src2, opt);
            store2.find(k3i, k3c, 4, 4, true, 16);
            store2.waitIdle();
            r = store2.find(k3i, k3c, 4, 4, true, 16);
            t.IsTrue(r.state == TexPack::Lookup::State::Pending || r.state == TexPack::Lookup::State::Ready, "falls back to k3_i (its picture may still be loading)");
            store2.waitIdle();
            r = store2.find(k3i, k3c, 4, 4, true, 16);
            t.IsTrue(r.state == TexPack::Lookup::State::Ready && !r.colouring && r.image->provider == "mod.late", "the bad k3_c file is skipped");
            t.IsFalse(src2->problems.empty(), "and reported");
            // a colouring key means nothing for a texture without a palette
            r = store.find(k3i, k3c, 4, 4, false, 0);
            store.waitIdle();
            r = store.find(k3i, k3c, 4, 4, false, 0);
            t.IsTrue(r.state == TexPack::Lookup::State::Ready && !r.colouring, "k3_c is for indexed textures only");
        });

        tc.Run("load order: the later mod's file wins, an index image beats a plain one inside one mod", [](TestCase &t)
        {
            const uint64_t key = 0xCCCC000000000001ull;
            TexPack::Store::Options opt;
            opt.prefetch = false;
            auto kindOf = [&](uint32_t plainOrder, uint32_t indexOrder) {
                auto src = std::make_shared<MemorySource>();
                src->add(hex16(key) + ".png", rgbaPng(8, 8, 1), "mod.p", plainOrder);
                src->add(hex16(key) + ".idx.png", greyPng(8, 8, 2), "mod.i", indexOrder);
                TexPack::Store store(src, opt);
                store.find(key, 0, 4, 4, true, 256);
                store.waitIdle();
                const TexPack::Lookup r = store.find(key, 0, 4, 4, true, 256);
                return r.image ? r.image->kind : TexPack::Kind::Rgba;
            };
            t.IsTrue(kindOf(0, 1) == TexPack::Kind::Index, "the index image comes from the later mod");
            t.IsTrue(kindOf(1, 0) == TexPack::Kind::Rgba, "the plain picture comes from the later mod");
            t.IsTrue(kindOf(2, 2) == TexPack::Kind::Index, "inside one mod the index image wins");
        });

        tc.Run("reload, prefetch and a small cache", [](TestCase &t)
        {
            auto src = std::make_shared<MemorySource>();
            for (uint64_t i = 1; i <= 6; ++i)
                src->add(hex16(0xDDDD000000000000ull + i) + ".png", rgbaPng(16, 16, static_cast<uint8_t>(i)));
            TexPack::Store::Options opt;
            opt.cacheBytes = 2560; // room for two pictures of 1024 bytes and a half (prefetch stops at half of it)
            TexPack::Store store(src, opt);
            store.waitIdle();
            t.IsTrue(store.stats().decoded >= 1 && store.stats().decoded < 6, "prefetch stops at half the cache");
            // every key can be asked for and eventually answered, however small the cache
            for (uint64_t i = 1; i <= 6; ++i)
            {
                const uint64_t key = 0xDDDD000000000000ull + i;
                TexPack::Lookup r = store.find(key, 0, 16, 16, false, 0);
                for (int tries = 0; tries < 50 && r.state == TexPack::Lookup::State::Pending; ++tries)
                {
                    store.waitIdle();
                    r = store.find(key, 0, 16, 16, false, 0);
                }
                t.IsTrue(r.state == TexPack::Lookup::State::Ready, "key " + std::to_string(i) + " ready");
            }
            t.IsTrue(store.stats().cacheBytes <= opt.cacheBytes, "the cache stayed within its budget");
            const uint64_t before = store.stats().generation;
            src->add(hex16(0xDDDD000000000009ull) + ".png", rgbaPng(16, 16, 9));
            t.Equals(store.reload(), 7u, "reload sees the new file");
            t.IsTrue(store.stats().generation != before && store.generation() == store.stats().generation, "the generation changed");
            auto empty = std::make_shared<MemorySource>();
            TexPack::Store none(empty, opt);
            t.IsTrue(none.empty(), "no files, empty");
        });
    });
}
