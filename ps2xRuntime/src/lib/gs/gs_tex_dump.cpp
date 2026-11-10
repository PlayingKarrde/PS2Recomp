// The texture dumper for texture authors. See include/runtime/gs/gs_tex_dump.h.

#include "runtime/gs/gs_tex_dump.h"

#include "runtime/gs/gs_png.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace TexDump
{
namespace
{
    namespace fs = std::filesystem;

    bool writeFile(const std::string &path, const std::vector<uint8_t> &bytes)
    {
        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        std::fclose(f);
        return ok;
    }

    // RGBA8 for a PNG from `texels` as the renderer decodes them (alpha as the GS sees it).
    void toRgbaPng(const TexKeys::TextureDesc &desc, const std::vector<uint8_t> &texels, const uint32_t *palette, std::vector<uint8_t> &rgba)
    {
        const bool indexed = desc.indexed();
        const size_t count = indexed ? texels.size() : texels.size() / 4u;
        rgba.resize(count * 4u);
        for (size_t i = 0; i < count; ++i)
        {
            uint32_t texel;
            if (indexed)
                texel = palette[texels[i]];
            else
                std::memcpy(&texel, &texels[i * 4u], 4);
            rgba[i * 4u + 0] = static_cast<uint8_t>(texel & 0xFFu);
            rgba[i * 4u + 1] = static_cast<uint8_t>((texel >> 8) & 0xFFu);
            rgba[i * 4u + 2] = static_cast<uint8_t>((texel >> 16) & 0xFFu);
            rgba[i * 4u + 3] = ToPngAlpha(texel >> 24);
        }
    }

    uint64_t slotOf(const TexKeys::TextureDesc &d)
    {
        return TexKeys::Combine(TexKeys::Combine(TexKeys::Combine(d.tbp, d.tbw), TexKeys::Combine(d.psm, d.width)), d.height);
    }
}

struct Dumper::Impl
{
    std::string dir;
    uint32_t maxFiles = 40000, files = 0, colouringsMax = 4;
    std::FILE *csv = nullptr, *slotsCsv = nullptr;
    bool keepPictures = false;
    std::vector<Tile> pictures;

    struct KeyState
    {
        uint32_t colourings = 0;
        std::unordered_set<uint64_t> cluts;
    };
    std::unordered_map<uint64_t, KeyState> keys;
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> slotKeys; // non-indexed slot -> keys seen there
    std::unordered_set<uint64_t> movieSlots;
    std::unordered_set<uint64_t> composedSlots; // one row each
    uint64_t rows = 0;

    ~Impl()
    {
        if (csv)
            std::fclose(csv);
        if (slotsCsv)
            std::fclose(slotsCsv);
    }

    std::string path(const std::string &name) const { return dir + "/" + name; }

    void row(const std::string &file, const Sighting &s, const char *cls, bool existed)
    {
        if (!csv)
            return;
        const TexKeys::LiveKey &k = s.key;
        std::fprintf(csv, "%s,%s,%s,%016llx,%u,%u,%u,%u,%u,%d,%d,%d,%d,%d,%u,%d,%d,%s,%016llx,%016llx,%d,%llu\n", file.c_str(),
                     s.primary ? TexKeys::KeyName(s.primary).c_str() : "-", s.fallback ? "k2s_i" : "k3_i", static_cast<unsigned long long>(s.k1), s.desc.tbp,
                     s.desc.tbw, s.desc.psm, s.desc.width, s.desc.height, s.desc.indexed() ? 1 : 0, k.rect.x0, k.rect.y0, k.rect.width(), k.rect.height(), k.writers,
                     k.exact ? 1 : 0, static_cast<int>(k.status), cls, static_cast<unsigned long long>(s.clutHash), static_cast<unsigned long long>(s.k3c),
                     existed ? 1 : 0, static_cast<unsigned long long>(s.marker));
        std::fflush(csv);
        ++rows;
    }

    void seen(const Sighting &s)
    {
        const uint64_t slot = slotOf(s.desc);
        if (s.key.status == TexKeys::LiveKey::Status::Composed)
        {
            if (composedSlots.insert(slot).second)
                row("", s, "composed", false);
            return;
        }
        if (s.primary == 0 || s.key.rect.empty())
        {
            if (composedSlots.insert(slot).second)
                row("", s, "nokey", false);
            return;
        }
        const bool indexed = s.desc.indexed();
        if (!indexed)
        {
            // A non-indexed slot that shows more than 32 different contents is a movie.
            if (movieSlots.count(slot))
                return;
            auto &set = slotKeys[slot];
            set.insert(s.primary);
            if (set.size() > 32)
            {
                movieSlots.insert(slot);
                if (slotsCsv)
                {
                    std::fprintf(slotsCsv, "%u,%u,%u,%u,%u,movie\n", s.desc.tbp, s.desc.tbw, s.desc.psm, s.desc.width, s.desc.height);
                    std::fflush(slotsCsv);
                }
                return;
            }
        }
        if (files >= maxFiles)
            return;
        const std::string name = TexKeys::KeyName(s.primary);
        KeyState &state = keys[s.primary];
        const bool first = state.cluts.empty();
        if (!first)
        {
            // Another colouring of a texture already dumped: its own RGBA picture, named by the colouring's key.
            if (!indexed || s.k3c == 0 || !s.palette || state.cluts.count(s.clutHash) || state.colourings >= colouringsMax)
                return;
            state.cluts.insert(s.clutHash);
            ++state.colourings;
            std::vector<uint8_t> local;
            const std::vector<uint8_t> *texels = s.texels;
            if (!texels)
            {
                TexKeys::ReadTexels(s.vram, s.desc, s.texa, s.key.rect, local);
                texels = &local;
            }
            const std::string file = TexKeys::KeyName(s.k3c) + ".png";
            std::error_code ec;
            const bool existed = fs::exists(path(file), ec);
            if (!existed)
            {
                std::vector<uint8_t> rgba;
                toRgbaPng(s.desc, *texels, s.palette, rgba);
                if (writeFile(path(file), GSPng::Encode(static_cast<uint32_t>(s.key.rect.width()), static_cast<uint32_t>(s.key.rect.height()), 6, rgba)))
                    ++files;
            }
            row(file, s, "colouring", existed);
            return;
        }
        state.cluts.insert(s.clutHash);
        std::error_code ec;
        const std::string file = name + ".png";
        const bool existed = fs::exists(path(file), ec);
        if (!existed)
        {
            std::vector<uint8_t> local;
            const std::vector<uint8_t> *texels = s.texels;
            if (!texels)
            {
                TexKeys::ReadTexels(s.vram, s.desc, s.texa, s.key.rect, local);
                texels = &local;
            }
            const uint32_t w = static_cast<uint32_t>(s.key.rect.width()), h = static_cast<uint32_t>(s.key.rect.height());
            std::vector<uint8_t> rgba;
            toRgbaPng(s.desc, *texels, s.palette, rgba);
            if (writeFile(path(file), GSPng::Encode(w, h, 6, rgba)))
                ++files;
            if (keepPictures && pictures.size() < 4096)
            {
                Tile tile;
                tile.w = w;
                tile.h = h;
                tile.rgba = rgba;
                pictures.push_back(std::move(tile));
            }
            if (indexed && s.palette)
            {
                std::vector<uint8_t> plte, trns;
                for (uint32_t i = 0; i < s.paletteCount; ++i)
                {
                    plte.push_back(static_cast<uint8_t>(s.palette[i] & 0xFFu));
                    plte.push_back(static_cast<uint8_t>((s.palette[i] >> 8) & 0xFFu));
                    plte.push_back(static_cast<uint8_t>((s.palette[i] >> 16) & 0xFFu));
                    trns.push_back(ToPngAlpha(s.palette[i] >> 24));
                }
                if (writeFile(path(name + ".idx.png"), GSPng::Encode(w, h, 3, *texels, plte, trns)))
                    ++files;
                std::vector<uint8_t> swatches(static_cast<size_t>(16u * 8u) * ((s.paletteCount / 16u) * 8u) * 4u, 0);
                const size_t sw = 16u * 8u;
                for (uint32_t i = 0; i < s.paletteCount; ++i)
                    for (uint32_t yy = 0; yy < 8u; ++yy)
                        for (uint32_t xx = 0; xx < 8u; ++xx)
                        {
                            uint8_t *px = &swatches[((static_cast<size_t>(i / 16u) * 8u + yy) * sw + (i % 16u) * 8u + xx) * 4u];
                            px[0] = static_cast<uint8_t>(s.palette[i] & 0xFFu);
                            px[1] = static_cast<uint8_t>((s.palette[i] >> 8) & 0xFFu);
                            px[2] = static_cast<uint8_t>((s.palette[i] >> 16) & 0xFFu);
                            px[3] = ToPngAlpha(s.palette[i] >> 24);
                        }
                if (writeFile(path(name + ".pal.png"), GSPng::Encode(static_cast<uint32_t>(sw), (s.paletteCount / 16u) * 8u, 6, swatches)))
                    ++files;
            }
        }
        row(file, s, "ok", existed);
    }
};

Dumper::Dumper() : m(std::make_unique<Impl>()) {}
Dumper::~Dumper() = default;

std::unique_ptr<Dumper> Dumper::FromEnvironment()
{
    const char *env = std::getenv("PS2X_TEX_DUMP");
    if (!env || env[0] == '\0')
        return nullptr;
    std::unique_ptr<Dumper> d(new Dumper());
    Impl &m = *d->m;
    m.dir = env;
    while (!m.dir.empty() && (m.dir.back() == '/' || m.dir.back() == '\\'))
        m.dir.pop_back();
    if (const char *max = std::getenv("PS2X_TEX_DUMP_MAX"))
        if (std::atol(max) > 0)
            m.maxFiles = static_cast<uint32_t>(std::atol(max));
    if (const char *col = std::getenv("PS2X_TEX_DUMP_COLOURINGS"))
        if (col[0] != '\0')
            m.colouringsMax = static_cast<uint32_t>(std::max(0l, std::atol(col)));
    std::error_code ec;
    fs::create_directories(m.dir, ec);
    const bool newCsv = !fs::exists(m.dir + "/textures.csv", ec);
    m.csv = std::fopen((m.dir + "/textures.csv").c_str(), "a");
    m.slotsCsv = std::fopen((m.dir + "/slots.csv").c_str(), "a");
    if (!m.csv)
    {
        std::fprintf(stderr, "[tex:dump] cannot write in %s\n", m.dir.c_str());
        return nullptr;
    }
    if (newCsv)
    {
        std::fprintf(m.csv, "file,key,keytype,k1,tbp,tbw,psm,w,h,indexed,x,y,rw,rh,writers,exact,status,class,clut_hash,k3_c,existed,marker\n");
        if (m.slotsCsv)
            std::fprintf(m.slotsCsv, "tbp,tbw,psm,w,h,class\n");
    }
    std::fprintf(stderr, "[tex:dump] on: writing the replaced rectangle of each texture to %s (named by k3_i)\n", m.dir.c_str());
    return d;
}

std::unique_ptr<Dumper> Dumper::Create(const std::string &imageDir, const std::string &csvDir, uint32_t maxFiles, uint32_t colourings)
{
    std::unique_ptr<Dumper> d(new Dumper());
    Impl &m = *d->m;
    m.dir = imageDir;
    while (!m.dir.empty() && (m.dir.back() == '/' || m.dir.back() == '\\'))
        m.dir.pop_back();
    m.maxFiles = maxFiles;
    m.colouringsMax = colourings;
    m.keepPictures = true;
    std::error_code ec;
    fs::create_directories(m.dir, ec);
    fs::create_directories(csvDir, ec);
    m.csv = std::fopen((csvDir + "/textures.csv").c_str(), "w");
    if (!m.csv)
        return nullptr;
    std::fprintf(m.csv, "file,key,keytype,k1,tbp,tbw,psm,w,h,indexed,x,y,rw,rh,writers,exact,status,class,clut_hash,k3_c,existed,marker\n");
    return d;
}

std::vector<Tile> Dumper::TakePictures()
{
    return std::move(m->pictures);
}

bool WriteContactSheet(const std::string &path, const std::vector<Tile> &tiles, uint32_t cell, uint32_t columns)
{
    if (tiles.empty() || cell < 8 || columns == 0)
        return false;
    const uint32_t rows = static_cast<uint32_t>((tiles.size() + columns - 1) / columns);
    const uint32_t pitch = cell + 2;
    const uint32_t W = columns * pitch + 2, H = rows * pitch + 2;
    std::vector<uint8_t> sheet(static_cast<size_t>(W) * H * 4, 0);
    for (size_t i = 0; i < sheet.size(); i += 4)
    {
        sheet[i] = sheet[i + 1] = sheet[i + 2] = 24;
        sheet[i + 3] = 255;
    }
    for (size_t t = 0; t < tiles.size(); ++t)
    {
        const Tile &tile = tiles[t];
        if (tile.w == 0 || tile.h == 0 || tile.rgba.size() < static_cast<size_t>(tile.w) * tile.h * 4u)
            continue;
        const uint32_t ox = 2 + static_cast<uint32_t>(t % columns) * pitch, oy = 2 + static_cast<uint32_t>(t / columns) * pitch;
        const float scale = std::min(static_cast<float>(cell) / static_cast<float>(tile.w), static_cast<float>(cell) / static_cast<float>(tile.h));
        const uint32_t dw = std::max(1u, static_cast<uint32_t>(static_cast<float>(tile.w) * scale)), dh = std::max(1u, static_cast<uint32_t>(static_cast<float>(tile.h) * scale));
        for (uint32_t y = 0; y < cell; ++y)
            for (uint32_t x = 0; x < cell; ++x)
            {
                uint8_t *px = &sheet[(static_cast<size_t>(oy + y) * W + ox + x) * 4u];
                const uint8_t checker = (((x >> 3) + (y >> 3)) & 1u) ? 70 : 52;
                float r = checker, g = checker, b = checker;
                if (x < dw && y < dh)
                {
                    // the box of source pixels under this thumbnail pixel
                    const uint32_t sx0 = static_cast<uint32_t>(static_cast<float>(x) / scale), sy0 = static_cast<uint32_t>(static_cast<float>(y) / scale);
                    const uint32_t sx1 = std::min(tile.w, std::max(sx0 + 1, static_cast<uint32_t>(static_cast<float>(x + 1) / scale)));
                    const uint32_t sy1 = std::min(tile.h, std::max(sy0 + 1, static_cast<uint32_t>(static_cast<float>(y + 1) / scale)));
                    float cr = 0, cg = 0, cb = 0, ca = 0;
                    uint32_t n = 0;
                    for (uint32_t sy = sy0; sy < sy1; ++sy)
                        for (uint32_t sx = sx0; sx < sx1; ++sx)
                        {
                            const uint8_t *sp = &tile.rgba[(static_cast<size_t>(sy) * tile.w + sx) * 4u];
                            cr += sp[0] * sp[3];
                            cg += sp[1] * sp[3];
                            cb += sp[2] * sp[3];
                            ca += sp[3];
                            ++n;
                        }
                    if (n > 0 && ca > 0)
                    {
                        const float a = ca / (255.0f * static_cast<float>(n));
                        r = (cr / ca) * a + r * (1.0f - a);
                        g = (cg / ca) * a + g * (1.0f - a);
                        b = (cb / ca) * a + b * (1.0f - a);
                    }
                }
                px[0] = static_cast<uint8_t>(r + 0.5f);
                px[1] = static_cast<uint8_t>(g + 0.5f);
                px[2] = static_cast<uint8_t>(b + 0.5f);
                px[3] = 255;
            }
    }
    return writeFile(path, GSPng::Encode(W, H, 6, sheet));
}

void Dumper::Seen(const Sighting &sighting)
{
    m->seen(sighting);
}

uint32_t Dumper::FilesWritten() const
{
    return m->files;
}
} // namespace TexDump
