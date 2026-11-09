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

void Dumper::Seen(const Sighting &sighting)
{
    m->seen(sighting);
}

uint32_t Dumper::FilesWritten() const
{
    return m->files;
}
} // namespace TexDump
