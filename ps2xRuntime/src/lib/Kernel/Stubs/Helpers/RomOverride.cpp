// Development aid (task a3): serve a file of FPR.ROM from a host folder, and log guest memory once per frame.
// See RomOverride.h for what both are for and how they work.
#include "RomOverride.h"

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>

namespace ps2x_rom_override
{
    namespace
    {
        uint32_t rd32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        }

        std::string lowerAscii(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        // Which of the 22 key bytes covers header offset 0x80 + p (tools/inventory.py key_slot): two interleaved cycles of 11.
        void keySlot(uint32_t p, uint32_t &half, uint32_t &index)
        {
            const uint32_t block = p / 8u;
            const uint32_t rem = p % 8u;
            half = rem / 4u;
            index = (7u * block + (rem % 4u)) % 11u;
        }

        // The bytes below 0x80 are plain; the rest is XORed with the key. Unused directory space is zero, so the most common
        // byte of each key slot is the key byte.
        void recoverKey(const uint8_t *header, uint32_t headerBytes, std::array<uint8_t, kKeyPeriod> &key)
        {
            static_assert(kKeyPeriod == 88, "two cycles of 11, each stepped every 8 bytes");
            uint32_t counts[2][11][256];
            std::memset(counts, 0, sizeof(counts));
            for (uint32_t p = 0; p + kDirectoryStart < headerBytes; ++p)
            {
                uint32_t half = 0, index = 0;
                keySlot(p, half, index);
                ++counts[half][index][header[kDirectoryStart + p]];
            }
            for (uint32_t i = 0; i < kKeyPeriod; ++i)
            {
                uint32_t half = 0, index = 0;
                keySlot(i, half, index);
                uint32_t best = 0;
                for (uint32_t v = 1; v < 256; ++v)
                {
                    if (counts[half][index][v] > counts[half][index][best])
                        best = v;
                }
                key[i] = static_cast<uint8_t>(best);
            }
        }

        struct Walker
        {
            const std::vector<uint8_t> &dec;
            Toc &toc;
            std::set<uint32_t> visited;

            bool inside(uint64_t off, uint64_t len) const { return off + len <= dec.size(); }

            // Reads the NUL-terminated name at `off`; false when it runs past the header.
            bool name(uint32_t off, std::string &out) const
            {
                out.clear();
                for (uint32_t i = off; i < dec.size(); ++i)
                {
                    if (dec[i] == 0)
                        return true;
                    out.push_back(static_cast<char>(dec[i]));
                    if (out.size() > 512)
                        return false;
                }
                return false;
            }

            void visit(uint32_t off, const std::string &path, bool isRoot, uint32_t depth)
            {
                static const char marker[] = {'.', 0, '.', '.', 0};
                if (depth > 16 || !visited.insert(off).second || !inside(off, 4))
                    return;
                const uint32_t headerCount = rd32(&dec[off]);
                uint32_t n = 0;
                if (!isRoot && headerCount >= 2 && headerCount < 100000 && inside(uint64_t(off) + 4 + 12ull * headerCount, 5) &&
                    std::memcmp(&dec[off + 4 + 12ull * headerCount], marker, 5) == 0)
                {
                    n = headerCount;
                }
                else
                {
                    // The root block's count word is wrong in this archive (docs/formats/fpr-rom.md): the real count is where the
                    // name pool starts, i.e. at the first ".\0..\0" that is 12-byte aligned from the table.
                    const uint64_t limit = std::min<uint64_t>(dec.size(), uint64_t(off) + 4 + 12ull * 20000 + 5);
                    uint64_t at = 0;
                    bool found = false;
                    for (uint64_t p = off + 4; p + 5 <= limit; ++p)
                    {
                        if (std::memcmp(&dec[p], marker, 5) == 0)
                        {
                            at = p;
                            found = true;
                            break;
                        }
                    }
                    if (!found || (at - off - 4) % 12 != 0)
                    {
                        toc.notes.push_back("no name pool for the directory block at 0x" + std::to_string(off));
                        return;
                    }
                    n = static_cast<uint32_t>((at - off - 4) / 12);
                }
                if (n < 2)
                    return;
                const uint32_t pool = 4 + 12 * n;
                for (uint32_t i = 2; i < n; ++i)
                {
                    const uint32_t entry = off + 4 + 12 * i;
                    if (!inside(entry, 12))
                        return;
                    const uint32_t nameWord = rd32(&dec[entry]);
                    const uint32_t a = rd32(&dec[entry + 4]);
                    const uint32_t b = rd32(&dec[entry + 8]);
                    const uint32_t nameOff = nameWord & 0x7FFFFFFFu;
                    std::string nm;
                    if (nameOff < pool || !name(off + nameOff, nm))
                    {
                        toc.notes.push_back("bad name offset in the directory block at 0x" + std::to_string(off));
                        continue;
                    }
                    if (nameWord & 0x80000000u)
                    {
                        visit(a * 128u, path + nm + "/", false, depth + 1);
                    }
                    else
                    {
                        TocFile f;
                        f.path = lowerAscii(path + nm);
                        f.sector = a;
                        f.size = b;
                        f.entryOffset = entry;
                        toc.files.push_back(std::move(f));
                    }
                }
            }
        };
    }

    bool decodeToc(const uint8_t *header, size_t bytes, Toc &toc, std::string &error)
    {
        toc = Toc();
        if (bytes < kDirectoryStart + 16 || std::memcmp(header, "ROM ", 4) != 0)
        {
            error = "not an FPR.ROM header (no 'ROM ' magic)";
            return false;
        }
        toc.headerSectors = rd32(header + 8);
        const uint64_t need = uint64_t(toc.headerSectors) * kSectorSize;
        if (toc.headerSectors == 0 || toc.headerSectors > 1024 || need > bytes)
        {
            error = "the header says " + std::to_string(toc.headerSectors) + " sectors but only " + std::to_string(bytes) +
                    " bytes were given";
            return false;
        }
        const uint32_t headerBytes = static_cast<uint32_t>(need);
        recoverKey(header, headerBytes, toc.key);
        std::vector<uint8_t> dec(header, header + headerBytes);
        for (uint32_t pos = kDirectoryStart; pos < headerBytes; ++pos)
            dec[pos] = static_cast<uint8_t>(dec[pos] ^ keyByteAt(toc, pos));
        Walker w{dec, toc, {}};
        w.visit(kDirectoryStart, std::string(), true, 0);
        if (toc.files.empty())
        {
            error = "the directory holds no files (wrong key, or not the game's archive)";
            return false;
        }
        return true;
    }

    uint64_t knownLoadLimit(const std::string &lowerPath)
    {
        // docs/formats/fprwaza.md "Where the numbers live": the move table is read into a heap block of 0x180000 bytes
        // (`lui a0,0x18` at 0x1473F4 and 0x148360) and the pose table into one of 0x540000 bytes (0x147370), and the reader
        // 0x13EAA0 has no capacity argument, so a longer file overruns the block.
        if (lowerPath == "yoneda/fprwaza.bin")
            return 0x180000ull;
        if (lowerPath == "yoneda/fprform.bin")
            return 0x540000ull;
        return 0;
    }

    std::map<std::string, std::filesystem::path> scanFolder(const std::filesystem::path &dir)
    {
        std::map<std::string, std::filesystem::path> out;
        std::error_code ec;
        if (dir.empty() || !std::filesystem::is_directory(dir, ec))
            return out;
        for (std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            std::error_code e2;
            if (!it->is_regular_file(e2) || e2)
                continue;
            const std::filesystem::path rel = std::filesystem::relative(it->path(), dir, e2);
            if (e2 || rel.empty())
                continue;
            std::string text = rel.generic_string();
            out.emplace(lowerAscii(text), it->path());
        }
        return out;
    }

    Plan makePlan(const Toc &toc, const std::map<std::string, std::filesystem::path> &hostFiles)
    {
        Plan plan;
        std::set<std::string> used;
        uint32_t nextSector = kVirtualSectorBase;
        for (const TocFile &f : toc.files)
        {
            const auto it = hostFiles.find(f.path);
            if (it == hostFiles.end())
                continue;
            used.insert(f.path);
            std::error_code ec;
            const uint64_t size = std::filesystem::file_size(it->second, ec);
            if (ec)
            {
                plan.log.push_back("REFUSED " + f.path + ": cannot read the size of " + it->second.string());
                continue;
            }
            if (size == 0)
            {
                plan.log.push_back("REFUSED " + f.path + ": the replacement file is empty");
                continue;
            }
            if (size > 0xFFFFFFFFull)
            {
                plan.log.push_back("REFUSED " + f.path + ": the replacement is larger than 4 GB");
                continue;
            }
            const uint64_t limit = knownLoadLimit(f.path);
            if (limit != 0 && size > limit)
            {
                plan.log.push_back("REFUSED " + f.path + ": " + std::to_string(size) + " bytes is larger than the game's load buffer for this file (" +
                                   std::to_string(limit) + " bytes); the game would overrun it");
                continue;
            }
            Replacement r;
            r.path = f.path;
            r.host = it->second;
            r.originalSector = f.sector;
            r.originalSize = f.size;
            r.entryOffset = f.entryOffset;
            r.newSize = static_cast<uint32_t>(size);
            r.sectors = static_cast<uint32_t>((size + kSectorSize - 1) / kSectorSize);
            r.virtualSector = nextSector;
            nextSector += r.sectors + 1; // one empty sector between files
            std::string line = "replacing " + f.path + " (" + std::to_string(f.size) + " -> " + std::to_string(size) + " bytes) with " +
                               it->second.string();
            if (limit == 0 && size > f.size)
                line += "; WARNING: larger than the original and the game's buffer for it is unknown";
            plan.log.push_back(std::move(line));
            plan.replacements.push_back(std::move(r));
        }
        for (const auto &kv : hostFiles)
        {
            if (!used.count(kv.first))
                plan.log.push_back("ignored " + kv.first + ": no such file in the archive");
        }
        return plan;
    }

    size_t patchDirectoryBytes(const Toc &toc, const Plan &plan, uint64_t archiveOffset, uint8_t *dst, size_t bytes)
    {
        size_t fields = 0;
        for (const Replacement &r : plan.replacements)
        {
            const uint32_t values[2] = {r.virtualSector, r.newSize};
            const uint32_t at[2] = {r.entryOffset + 4, r.entryOffset + 8};
            for (int k = 0; k < 2; ++k)
            {
                bool touched = false;
                for (uint32_t b = 0; b < 4; ++b)
                {
                    const uint64_t pos = uint64_t(at[k]) + b;
                    if (pos < archiveOffset || pos >= archiveOffset + bytes)
                        continue;
                    const uint8_t plain = static_cast<uint8_t>(values[k] >> (8 * b));
                    dst[pos - archiveOffset] = static_cast<uint8_t>(plain ^ keyByteAt(toc, static_cast<uint32_t>(pos)));
                    touched = true;
                }
                if (touched)
                    ++fields;
            }
        }
        return fields;
    }

    bool readVirtualSectors(const Plan &plan, uint64_t sector, uint32_t sectors, uint8_t *dst, size_t bytes)
    {
        (void)sectors;
        for (const Replacement &r : plan.replacements)
        {
            if (sector < r.virtualSector || sector >= uint64_t(r.virtualSector) + r.sectors)
                continue;
            std::memset(dst, 0, bytes);
            const uint64_t offset = (sector - r.virtualSector) * kSectorSize;
            std::ifstream in(r.host, std::ios::binary);
            if (!in.is_open())
                return true; // zeros: the host file vanished; the log has said what was planned
            in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            if (in.good())
                in.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(std::min<uint64_t>(bytes, r.newSize > offset ? r.newSize - offset : 0)));
            return true;
        }
        return false;
    }

    // ---- Global state behind the hook -------------------------------------------------------------------------------------

    namespace
    {
        struct State
        {
            std::mutex mutex;
            std::atomic<int> mode{-1}; // -1 not decided, 0 off, 1 on
            std::filesystem::path dir;
            bool built = false;
            bool failed = false;
            uint32_t baseLbn = 0;
            Toc toc;
            Plan plan;
            std::vector<uint8_t> served; // per replacement: has the game read it yet (for one log line)
        };

        State &state()
        {
            static State s;
            return s;
        }

        void say(const std::string &line)
        {
            std::fprintf(stderr, "[rom-override] %s\n", line.c_str());
            std::fflush(stderr);
        }

        void decideMode(State &s)
        {
            if (s.mode.load() != -1)
                return;
            std::lock_guard<std::mutex> lock(s.mutex);
            if (s.mode.load() != -1)
                return;
            const char *env = std::getenv("PS2X_ROM_OVERRIDE_DIR");
            if (env && env[0] != '\0')
            {
                s.dir = env;
                s.mode.store(1);
                say("PS2X_ROM_OVERRIDE_DIR=" + s.dir.string() + " (development aid; the game's files are replaced from this folder)");
            }
            else
            {
                s.mode.store(0);
            }
        }

        bool isFprRom(const std::filesystem::path &p)
        {
            return lowerAscii(p.filename().string()) == "fpr.rom";
        }

        // Called with the lock held, when FPR.ROM is first read.
        void build(State &s, const std::filesystem::path &romPath, uint32_t baseLbn)
        {
            s.built = true;
            s.baseLbn = baseLbn;
            std::ifstream in(romPath, std::ios::binary);
            uint8_t head[16] = {};
            if (!in.is_open() || !in.read(reinterpret_cast<char *>(head), sizeof(head)))
            {
                s.failed = true;
                say("cannot read the archive header of " + romPath.string());
                return;
            }
            const uint32_t sectors = rd32(head + 8);
            if (std::memcmp(head, "ROM ", 4) != 0 || sectors == 0 || sectors > 1024)
            {
                s.failed = true;
                say(romPath.string() + " is not the game's archive (no 'ROM ' header); the override is off");
                return;
            }
            std::vector<uint8_t> header(size_t(sectors) * kSectorSize);
            in.seekg(0, std::ios::beg);
            if (!in.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size())))
            {
                s.failed = true;
                say("cannot read the archive's directory");
                return;
            }
            std::string error;
            if (!decodeToc(header.data(), header.size(), s.toc, error))
            {
                s.failed = true;
                say("cannot decode the archive's directory: " + error + "; the override is off");
                return;
            }
            s.plan = makePlan(s.toc, scanFolder(s.dir));
            s.served.assign(s.plan.replacements.size(), 0);
            say("archive directory decoded: " + std::to_string(s.toc.files.size()) + " files");
            for (const std::string &line : s.plan.log)
                say(line);
            if (s.plan.replacements.empty())
                say("nothing to replace");
            else
                say(std::to_string(s.plan.replacements.size()) + " file(s) replaced");
        }
    }

    bool enabled()
    {
        State &s = state();
        if (s.mode.load(std::memory_order_relaxed) == -1)
            decideMode(s);
        return s.mode.load(std::memory_order_relaxed) == 1;
    }

    void afterHostRead(const std::filesystem::path &hostPath, uint32_t baseLbn, uint64_t relativeSector, uint8_t *dst, size_t bytes)
    {
        if (!enabled() || !isFprRom(hostPath))
            return;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.built)
            build(s, hostPath, baseLbn);
        if (s.failed || s.plan.replacements.empty())
            return;
        // Only the directory (the first sectors) holds the fields.
        if (relativeSector >= s.toc.headerSectors)
            return;
        patchDirectoryBytes(s.toc, s.plan, relativeSector * kSectorSize, dst, bytes);
    }

    bool serveVirtual(uint32_t lbn, uint32_t sectors, uint8_t *dst, size_t bytes)
    {
        if (!enabled())
            return false;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.built || s.failed || s.plan.replacements.empty() || lbn < s.baseLbn)
            return false;
        const uint64_t rel = uint64_t(lbn - s.baseLbn);
        if (!readVirtualSectors(s.plan, rel, sectors, dst, bytes))
            return false;
        for (size_t i = 0; i < s.plan.replacements.size(); ++i)
        {
            const Replacement &r = s.plan.replacements[i];
            if (rel >= r.virtualSector && rel < uint64_t(r.virtualSector) + r.sectors && i < s.served.size() && !s.served[i])
            {
                s.served[i] = 1;
                say("the game read " + r.path + " from " + r.host.string() + " (" + std::to_string(r.newSize) + " bytes, sector " +
                    std::to_string(rel - r.virtualSector) + " of " + std::to_string(r.sectors) + " first)");
            }
        }
        return true;
    }

    void resetForTests(const std::filesystem::path &dir)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.built = false;
        s.failed = false;
        s.baseLbn = 0;
        s.toc = Toc();
        s.plan = Plan();
        s.served.clear();
        s.dir = dir;
        s.mode.store(dir.empty() ? 0 : 1);
    }

    const Plan &currentPlanForTests()
    {
        return state().plan;
    }
}

// ---- The match probe -----------------------------------------------------------------------------------------------------

namespace ps2x_match_probe
{
    namespace
    {
        uint32_t rd32Local(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        }

        bool parseNumber(const std::string &text, uint32_t &out)
        {
            if (text.empty())
                return false;
            char *end = nullptr;
            const unsigned long long v = std::strtoull(text.c_str(), &end, 0);
            if (end == text.c_str() || *end != '\0' || v > 0xFFFFFFFFull)
                return false;
            out = static_cast<uint32_t>(v);
            return true;
        }

        std::vector<std::string> split(const std::string &text, char sep)
        {
            std::vector<std::string> out;
            size_t start = 0;
            for (;;)
            {
                const size_t at = text.find(sep, start);
                out.push_back(text.substr(start, at == std::string::npos ? std::string::npos : at - start));
                if (at == std::string::npos)
                    break;
                start = at + 1;
            }
            return out;
        }

        std::string trim(const std::string &s)
        {
            size_t a = 0, b = s.size();
            while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
                ++a;
            while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
                --b;
            return s.substr(a, b - a);
        }
    }

    bool parseFieldSpecs(const std::string &text, std::vector<FieldSpec> &out, std::string &error)
    {
        out.clear();
        for (const std::string &raw : split(text, ','))
        {
            std::string item = trim(raw);
            if (item.empty())
                continue;
            FieldSpec f;
            const size_t eq = item.find('=');
            if (eq != std::string::npos)
            {
                f.name = trim(item.substr(0, eq));
                item = trim(item.substr(eq + 1));
            }
            const size_t colon = item.rfind(':');
            if (colon == std::string::npos)
            {
                error = "field '" + raw + "': missing :<width>";
                return false;
            }
            uint32_t width = 0;
            if (!parseNumber(trim(item.substr(colon + 1)), width) || width == 0 || width > 256)
            {
                error = "field '" + raw + "': the width must be 1 to 256";
                return false;
            }
            f.width = width;
            std::string where = trim(item.substr(0, colon));
            const size_t gt = where.find('>');
            if (gt != std::string::npos)
            {
                if (!parseNumber(trim(where.substr(gt + 1)), f.derefOffset))
                {
                    error = "field '" + raw + "': bad offset after '>'";
                    return false;
                }
                f.deref = true;
                where = trim(where.substr(0, gt));
            }
            if (where.size() < 2 || (where[0] != '+' && where[0] != '@'))
            {
                error = "field '" + raw + "': start with +<offset> or @<address>";
                return false;
            }
            f.absolute = where[0] == '@';
            if (!parseNumber(trim(where.substr(1)), f.where))
            {
                error = "field '" + raw + "': bad number '" + where.substr(1) + "'";
                return false;
            }
            if (f.name.empty())
            {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "%c%X%s", f.absolute ? 'a' : 'f', f.where, f.deref ? "_p" : "");
                f.name = buf;
            }
            out.push_back(std::move(f));
        }
        if (out.empty())
        {
            error = "no fields";
            return false;
        }
        return true;
    }

    bool parseDumpSpec(const std::string &text, Config &cfg, std::string &error)
    {
        const std::vector<std::string> parts = split(text, ',');
        if (parts.size() < 3 || parts.size() > 4)
        {
            error = "PS2X_PROBE_DUMP is <file>,<addr>,<len>[,ptr]";
            return false;
        }
        if (parts.size() == 4)
        {
            if (trim(parts[3]) != "ptr")
            {
                error = "the fourth part of PS2X_PROBE_DUMP can only be 'ptr'";
                return false;
            }
            cfg.dumpIsPointer = true;
        }
        if (!parseNumber(trim(parts[1]), cfg.dumpAddr) || !parseNumber(trim(parts[2]), cfg.dumpLen) || cfg.dumpLen == 0)
        {
            error = "PS2X_PROBE_DUMP: bad address or length";
            return false;
        }
        cfg.dumpFile = trim(parts[0]);
        if (cfg.dumpFile.empty())
        {
            error = "PS2X_PROBE_DUMP: no file";
            return false;
        }
        return true;
    }

    Sampler::Sampler(const Config &cfg, Sink sink, void *user) : m_cfg(cfg), m_sink(sink), m_user(user)
    {
        m_header = "frame,tick,w";
        for (const FieldSpec &f : m_cfg.fields)
            m_header += "," + f.name;
        m_header += "\n";
        m_last.assign(m_cfg.count + 1, std::vector<std::string>(m_cfg.fields.size()));
        if (m_sink)
            m_sink(m_user, m_header);
    }

    std::string Sampler::readField(const FieldSpec &f, const uint8_t *ram, uint32_t ramBytes, uint32_t structBase) const
    {
        auto inRam = [&](uint64_t addr, uint32_t width) { return addr + width <= ramBytes; };
        uint64_t addr = f.absolute ? f.where : uint64_t(structBase) + f.where;
        addr &= 0x01FFFFFFull; // KSEG0/1 mirrors of the same RAM, as the runtime masks
        if (f.deref)
        {
            if (!inRam(addr, 4))
                return "-";
            const uint32_t ptr = rd32Local(ram + addr);
            if (ptr == 0)
                return "-";
            addr = (uint64_t(ptr) + f.derefOffset) & 0x01FFFFFFull;
        }
        if (!inRam(addr, f.width))
            return "-";
        char buf[16];
        if (f.width == 1 || f.width == 2 || f.width == 4)
        {
            uint32_t v = 0;
            for (uint32_t i = 0; i < f.width; ++i)
                v |= static_cast<uint32_t>(ram[addr + i]) << (8 * i);
            std::snprintf(buf, sizeof(buf), "0x%X", v);
            return buf;
        }
        std::string hex;
        hex.reserve(size_t(f.width) * 2);
        for (uint32_t i = 0; i < f.width; ++i)
        {
            std::snprintf(buf, sizeof(buf), "%02x", ram[addr + i]);
            hex += buf;
        }
        return hex;
    }

    size_t Sampler::sample(const uint8_t *ram, uint32_t ramBytes, uint64_t frame, uint64_t tick)
    {
        size_t rows = 0;
        // One row for one wrestler (or the absolute fields): only the cells that changed since this row's owner last had a row are
        // written, the others are left empty. The first row of an owner has every cell.
        auto emit = [&](int w, const std::vector<std::string> &values, std::vector<std::string> &last, bool absolute)
        {
            std::string cells;
            bool any = false;
            for (size_t i = 0; i < m_cfg.fields.size(); ++i)
            {
                cells += ",";
                if (m_cfg.fields[i].absolute != absolute)
                    continue;
                if (values[i] != last[i])
                {
                    cells += values[i];
                    last[i] = values[i];
                    any = true;
                }
            }
            if (!any)
                return;
            const std::string line = std::to_string(frame) + "," + std::to_string(tick) + "," + std::to_string(w) + cells + '\n';
            if (m_sink)
                m_sink(m_user, line);
            ++rows;
        };

        // Absolute fields: one row of their own whenever any of them changed.
        bool anyAbsolute = false;
        for (const FieldSpec &f : m_cfg.fields)
            anyAbsolute = anyAbsolute || f.absolute;
        if (anyAbsolute)
        {
            std::vector<std::string> values(m_cfg.fields.size());
            for (size_t i = 0; i < m_cfg.fields.size(); ++i)
            {
                if (m_cfg.fields[i].absolute)
                    values[i] = readField(m_cfg.fields[i], ram, ramBytes, 0);
            }
            emit(-1, values, m_last[m_cfg.count], true);
        }

        // Per-wrestler fields, once the array exists.
        uint32_t base = 0;
        if (uint64_t(m_cfg.basePtrAddr) + 4 <= ramBytes)
            base = rd32Local(ram + (m_cfg.basePtrAddr & 0x01FFFFFFu));
        base &= 0x01FFFFFFu;
        if (base == 0 || uint64_t(base) + uint64_t(m_cfg.count) * m_cfg.stride > ramBytes)
            return rows;
        m_matchSeen = true;
        for (uint32_t w = 0; w < m_cfg.count; ++w)
        {
            std::vector<std::string> values(m_cfg.fields.size());
            for (size_t i = 0; i < m_cfg.fields.size(); ++i)
            {
                if (!m_cfg.fields[i].absolute)
                    values[i] = readField(m_cfg.fields[i], ram, ramBytes, base + w * m_cfg.stride);
            }
            emit(static_cast<int>(w), values, m_last[w], false);
        }
        return rows;
    }
}

// ---- Wrapping the game's frame-end function with the probe -------------------------------------------------------------
//
// 0x136DB0 is the function the game calls at the end of every frame (and in its loading wait loops); `i1` counts frames with
// it (bringup.md "i1"). The wrapper samples before the original runs, i.e. the state at the end of this frame's update. The
// wrapper does not touch ctx->pc: the original sets it as it always does.
namespace
{
    constexpr uint32_t kFrameEndAddress = 0x00136DB0u;

    struct ProbeState
    {
        ps2x_match_probe::Config cfg;
        std::unique_ptr<ps2x_match_probe::Sampler> sampler;
        FILE *out = nullptr;
        uint64_t frame = 0;
        bool dumpDone = false;
        uint64_t seenFrame = 0;   // the frame in which the wrestler array first existed
        uint32_t dumpDelay = 600; // frames to wait after that before dumping (PS2X_PROBE_DUMP_DELAY)
        PS2Runtime::RecompiledFunction original = nullptr;
    };

    ProbeState g_probe;

    void probeSink(void *user, const std::string &text)
    {
        ProbeState *p = static_cast<ProbeState *>(user);
        std::fwrite(text.data(), 1, text.size(), p->out);
        std::fflush(p->out);
    }

    void probeDump(ProbeState &p, const uint8_t *rdram)
    {
        p.dumpDone = true;
        uint64_t addr = p.cfg.dumpAddr;
        if (p.cfg.dumpIsPointer)
        {
            uint32_t ptr = 0;
            if (addr + 4 <= PS2_RAM_SIZE)
                std::memcpy(&ptr, rdram + addr, 4);
            addr = ptr;
        }
        addr &= PS2_RAM_MASK;
        if (addr == 0 || addr + p.cfg.dumpLen > PS2_RAM_SIZE)
        {
            std::fprintf(stderr, "[match-probe] dump skipped: 0x%llX + %u is not in guest memory\n", (unsigned long long)addr, p.cfg.dumpLen);
            std::fflush(stderr);
            return;
        }
        FILE *f = std::fopen(p.cfg.dumpFile.c_str(), "wb");
        if (!f)
        {
            std::fprintf(stderr, "[match-probe] cannot write %s\n", p.cfg.dumpFile.c_str());
            std::fflush(stderr);
            return;
        }
        std::fwrite(rdram + addr, 1, p.cfg.dumpLen, f);
        std::fclose(f);
        std::fprintf(stderr, "[match-probe] frame %llu: dumped %u bytes from guest 0x%llX to %s\n", (unsigned long long)p.frame, p.cfg.dumpLen,
                     (unsigned long long)addr, p.cfg.dumpFile.c_str());
        std::fflush(stderr);
    }

    void frameEndWithProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ProbeState &p = g_probe;
        ++p.frame;
        const uint64_t tick = runtime ? runtime->eeScheduler().currentVSyncTick() : 0;
        p.sampler->sample(rdram, PS2_RAM_SIZE, p.frame, tick);
        if (!p.dumpDone && !p.cfg.dumpFile.empty() && p.sampler->matchSeen())
        {
            // The array exists from the frame the match block is allocated; the move table is read after that, while the game
            // waits in this very function. Dump once the read is long done.
            if (p.seenFrame == 0)
                p.seenFrame = p.frame;
            if (p.frame >= p.seenFrame + p.dumpDelay)
                probeDump(p, rdram);
        }
        p.original(rdram, ctx, runtime);
    }

    uint32_t envNumber(const char *name, uint32_t fallback)
    {
        const char *raw = std::getenv(name);
        if (!raw || raw[0] == '\0')
            return fallback;
        char *end = nullptr;
        const unsigned long long v = std::strtoull(raw, &end, 0);
        return (end == raw || *end != '\0' || v > 0xFFFFFFFFull) ? fallback : static_cast<uint32_t>(v);
    }

    void applyMatchProbe(PS2Runtime &runtime)
    {
        const char *file = std::getenv("PS2X_PROBE_FILE");
        if (!file || file[0] == '\0')
            return;
        ps2x_match_probe::Config &cfg = g_probe.cfg;
        cfg.file = file;
        const char *fields = std::getenv("PS2X_PROBE_FIELDS");
        std::string error;
        if (!ps2x_match_probe::parseFieldSpecs((fields && fields[0]) ? fields : "rec=+0x1AC:4,unit=+0x1B0:4", cfg.fields, error))
        {
            std::fprintf(stderr, "[match-probe] PS2X_PROBE_FIELDS: %s; the probe is off\n", error.c_str());
            std::fflush(stderr);
            return;
        }
        cfg.basePtrAddr = envNumber("PS2X_PROBE_BASE", cfg.basePtrAddr);
        cfg.stride = envNumber("PS2X_PROBE_STRIDE", cfg.stride);
        cfg.count = envNumber("PS2X_PROBE_COUNT", cfg.count);
        g_probe.dumpDelay = envNumber("PS2X_PROBE_DUMP_DELAY", g_probe.dumpDelay);
        if (cfg.stride == 0 || cfg.count == 0 || cfg.count > 64)
        {
            std::fprintf(stderr, "[match-probe] PS2X_PROBE_STRIDE/COUNT out of range; the probe is off\n");
            std::fflush(stderr);
            return;
        }
        if (const char *dump = std::getenv("PS2X_PROBE_DUMP"); dump && dump[0])
        {
            if (!ps2x_match_probe::parseDumpSpec(dump, cfg, error))
            {
                std::fprintf(stderr, "[match-probe] %s; the probe is off\n", error.c_str());
                std::fflush(stderr);
                return;
            }
        }
        if (!runtime.hasFunction(kFrameEndAddress))
        {
            std::fprintf(stderr, "[match-probe] no function at 0x%X; the probe is off\n", kFrameEndAddress);
            std::fflush(stderr);
            return;
        }
        g_probe.out = std::fopen(cfg.file.c_str(), "wb");
        if (!g_probe.out)
        {
            std::fprintf(stderr, "[match-probe] cannot write %s; the probe is off\n", cfg.file.c_str());
            std::fflush(stderr);
            return;
        }
        g_probe.sampler = std::make_unique<ps2x_match_probe::Sampler>(cfg, probeSink, &g_probe);
        g_probe.original = runtime.lookupFunction(kFrameEndAddress);
        runtime.replaceFunction(kFrameEndAddress, frameEndWithProbe);
        std::fprintf(stderr, "[match-probe] writing %s (%zu field(s), base ptr 0x%X, %u x 0x%X bytes)\n", cfg.file.c_str(), cfg.fields.size(),
                     cfg.basePtrAddr, cfg.count, cfg.stride);
        std::fflush(stderr);
    }
}

// Registered like the game's other overrides (game_overrides.h). This file is linked into the runtime because the CD code
// calls the ROM override above, so the registration is not dropped by the linker.
PS2_REGISTER_GAME_OVERRIDE("Match probe (task a3)", "SLUS_217.02", 0x00100008u, 0u, applyMatchProbe);
