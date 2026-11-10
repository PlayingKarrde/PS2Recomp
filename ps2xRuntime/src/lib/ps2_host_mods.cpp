#include "ps2_host_mods.h"

#include "runtime/gs/gs_tex_pack.h"

#if defined(PS2X_HAS_MODS)
#include "fpr_save.h"
#include "mod_manager.h"
#include "profile.h"
#include "ps2_runtime.h"
#include "ps2_settings.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>

namespace ps2x_host_mods
{
#if defined(PS2X_HAS_MODS)
    namespace
    {
        namespace fs = std::filesystem;

        struct Holder
        {
            std::once_flag once;
            std::unique_ptr<ps2x::mods::Manager> manager;
        };

        Holder &holder()
        {
            static Holder h;
            return h;
        }

        // What selectCard decided, for the Mods tab and "Keep this card".
        struct CardState
        {
            std::mutex mutex;
            fs::path prefFolder;
            fs::path profileDir; // <pref folder>/profile ("" without a pref folder)
            fs::path baseCard0;  // the base card's slot 1 root, as the runtime had it before any redirect
            std::string line;
            bool usingProfile = false; // this session plays from the profile card
        };

        CardState &card()
        {
            static CardState c;
            return c;
        }

        fs::path fromUtf8(const std::string &text)
        {
            return fs::path(std::u8string(text.begin(), text.end()));
        }

        std::string utf8(const fs::path &path)
        {
            const auto text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        // The mods' files for the texture pack store: lists and reads through the manager's session lookup, and hands the store's
        // complaints back to the manager as notes on the mod that provided the file (the Mods tab shows them).
        class ModTextureSource final : public TexPack::FileSource
        {
        public:
            explicit ModTextureSource(ps2x::mods::Manager *manager) : m_manager(manager) {}

            std::vector<Entry> list(const std::string &prefix) override
            {
                std::vector<Entry> out;
                for (const auto &file : m_manager->sessionFiles(prefix))
                    out.push_back(Entry{file.path, file.modId, file.order});
                return out;
            }

            bool read(const std::string &path, std::vector<uint8_t> &bytes, std::string &error) override
            {
                auto data = m_manager->open(path);
                if (!data)
                {
                    error = "the mod no longer has the file";
                    return false;
                }
                bytes = std::move(*data);
                return true;
            }

            void problem(const std::string &provider, const std::string &message) override
            {
                m_manager->addRuntimeNote(provider, message);
            }

        private:
            ps2x::mods::Manager *m_manager;
        };
    }

    void initialize()
    {
        Holder &h = holder();
        std::call_once(h.once, [&h]() {
            std::string settingsPath = ps2x_settings::path();
            if (settingsPath.empty())
                settingsPath = ps2x_settings::defaultPath();
            fs::path prefFolder = settingsPath.empty() ? fs::path() : fromUtf8(settingsPath).parent_path();
            {
                std::lock_guard<std::mutex> lock(card().mutex);
                card().prefFolder = prefFolder;
            }
            fs::path modsDir;
            if (const char *env = std::getenv("PS2X_MODS_DIR"); env && env[0] != '\0')
                modsDir = fromUtf8(env);
            else if (!prefFolder.empty())
                modsDir = prefFolder / "mods";
            if (modsDir.empty())
            {
                std::printf("[mods] no folder for mods (no preference folder); the mod manager is off\n");
                std::fflush(stdout);
                return;
            }

            ps2x::mods::Manager::Config config;
            config.modsDir = modsDir;
            config.statePath = (prefFolder.empty() ? modsDir.parent_path() : prefFolder) / "mods.toml";
            auto manager = std::make_unique<ps2x::mods::Manager>();
            if (!manager->init(config))
            {
                std::fprintf(stderr, "[mods] the mod manager could not start: %s\n", manager->initError().c_str());
                return;
            }
            for (const std::string &note : manager->notes())
                std::fprintf(stderr, "[mods] %s\n", note.c_str());

            const auto mods = manager->list();
            size_t valid = 0;
            std::string active;
            for (const auto &mod : mods)
            {
                if (mod.valid)
                    ++valid;
                if (mod.active)
                    active += (active.empty() ? "" : ", ") + mod.id + " " + mod.version;
            }
            std::printf("[mods] folder %s; state %s; %zu mods (%zu working); active: %s\n", utf8(config.modsDir).c_str(),
                        utf8(config.statePath).c_str(), mods.size(), valid, active.empty() ? "none" : active.c_str());
            for (const auto &mod : mods)
                if (!mod.valid)
                    std::printf("[mods] %s is not usable: %s\n", mod.file.c_str(), mod.error.c_str());
            std::fflush(stdout);
            h.manager = std::move(manager);
            std::shared_ptr<TexPack::FileSource> source = std::make_shared<ModTextureSource>(h.manager.get());

            // The dev pack (task t2, docs/modding-textures.md): a folder of loose pictures that acts like a mod loaded after every other one.
            // The author tools (the key tracker without a pack, the dump button, watching the folder) are on when PS2X_AUTHORING=1, when the
            // overlay's Texture Tools setting is on (it reaches this variable) or when the folder already holds files; PS2X_AUTHORING=0 turns
            // them off whatever the folder holds.
            fs::path devFolder;
            if (const char *env = std::getenv("PS2X_DEV_PACK"); env && env[0] != '\0')
                devFolder = fromUtf8(env);
            else if (!prefFolder.empty())
                devFolder = prefFolder / "dev-pack";
            if (!devFolder.empty())
            {
                const char *flag = std::getenv("PS2X_AUTHORING");
                bool authoring = flag && (flag[0] == '1' || flag[0] == 'o' || flag[0] == 'O');
                const bool forcedOff = flag && flag[0] == '0';
                std::error_code ec;
                uint32_t looseFiles = 0;
                for (const char *sub : {"textures", "font"})
                    for (const auto &entry : fs::directory_iterator(devFolder / sub, ec))
                    {
                        std::error_code fec;
                        if (entry.is_regular_file(fec))
                            ++looseFiles;
                    }
                if (!authoring && !forcedOff && looseFiles > 0)
                    authoring = true;
                if (forcedOff)
                    authoring = false;
                auto pack = std::make_shared<TexPack::DevPackSource>(utf8(devFolder), source, 400, authoring);
                TexPack::SetDevPack(pack);
                TexPack::SetAuthoring(authoring);
                source = pack;
                std::printf("[mods] dev pack folder %s (%u files); author tools %s\n", utf8(devFolder).c_str(), looseFiles, authoring ? "on" : "off");
                std::fflush(stdout);
            }
            TexPack::SetDefaultSource(source);
        });
    }

    ps2x::mods::Manager *manager()
    {
        return holder().manager.get();
    }

    std::string modsFolder()
    {
        ps2x::mods::Manager *m = manager();
        return m ? utf8(m->modsDir()) : std::string();
    }

    std::shared_ptr<TexPack::FileSource> texturePackSource()
    {
        ps2x::mods::Manager *m = manager();
        return m ? std::make_shared<ModTextureSource>(m) : nullptr;
    }

    std::vector<std::string> themeStyleSheets()
    {
        ps2x::mods::Manager *m = manager();
        return m ? m->sessionThemeStylesheets() : std::vector<std::string>();
    }

    bool readVirtualFile(const std::string &name, std::vector<uint8_t> &bytes)
    {
        ps2x::mods::Manager *m = manager();
        if (!m)
            return false;
        auto data = m->readVirtual(name);
        if (!data)
            return false;
        bytes = std::move(*data);
        return true;
    }

    namespace
    {
        // The data packs that are on now (what the next start would use), not the ones the running game started with.
        size_t liveDataPacks(ps2x::mods::Manager *m)
        {
            size_t count = 0;
            for (const auto &mod : m->list())
                if (mod.valid && mod.active && mod.kind == ps2x::mods::Kind::Data)
                    ++count;
            return count;
        }
    }

    void selectCard(const std::string &elfPath)
    {
        ps2x::mods::Manager *m = manager();
        if (!m)
            return;
        namespace profile = ps2x::mods::profile;
        namespace fpr = ps2x::mods::fpr;
        const PS2Runtime::IoPaths io = PS2Runtime::getIoPaths();
        profile::Config config;
        {
            std::lock_guard<std::mutex> lock(card().mutex);
            if (!card().prefFolder.empty())
                card().profileDir = card().prefFolder / "profile";
            card().baseCard0 = io.mcRoot;
            card().usingProfile = false;
            config.profileDir = card().profileDir;
            card().line = profile::baseCardLine();
        }
        config.baseCard0 = io.mcRoot;
        config.baseCard1 = profile::slot2Root(io.mcRoot);

        // The data packs that were on when the game started, in load order.
        for (const std::string &id : m->sessionActive())
        {
            const auto info = m->find(id);
            if (!info || info->kind != ps2x::mods::Kind::Data)
                continue;
            profile::PackInput pack;
            pack.id = info->id;
            pack.name = info->name;
            pack.version = info->version;
            pack.hasFile = m->hasFile(id, "data/renames.json");
            if (pack.hasFile)
            {
                std::string why;
                const auto bytes = m->readFile(id, "data/renames.json", 16ull * 1024 * 1024, &why);
                if (bytes)
                    pack.renamesJson.assign(bytes->begin(), bytes->end());
                else
                    pack.readError = why.empty() ? "the file could not be read" : why;
            }
            config.packs.push_back(std::move(pack));
        }
        if (config.profileDir.empty())
            return; // no preference folder: the base card, exactly as without mods

        // Which card, and (when it is the profile) building it: mods/profile.h select(). The checksum table is read from the executable
        // only if the profile is going to be used.
        config.loadCrcTable = [elfPath](fpr::CrcTable &table, std::string *why) { return fpr::crcTableFromElf(fromUtf8(elfPath), table, why); };

        profile::Selection selection;
        try
        {
            selection = profile::select(config);
        }
        catch (const std::exception &e)
        {
            // select() does not throw by design; if something still does, the game plays from the base card as if no pack were on.
            std::fprintf(stderr, "[mods] the profile card could not be prepared (%s); using your own card\n", e.what());
            std::lock_guard<std::mutex> lock(card().mutex);
            card().line = "Memory card: your own card (the profile card could not be prepared).";
            return;
        }
        for (const std::string &line : selection.log)
            std::printf("[mods] %s\n", line.c_str());
        if (!selection.useProfile)
        {
            // The base card: nothing under the profile folder was made or touched.
            std::lock_guard<std::mutex> lock(card().mutex);
            card().line = selection.line;
            std::fflush(stdout);
            return;
        }
        const profile::Result &result = selection.result;
        for (size_t i = 0; i < result.packs.size(); ++i)
        {
            const profile::PackReport &report = result.packs[i];
            if (!report.error.empty())
                m->addRuntimeNote(report.id, report.error);
            for (const std::string &note : report.notes)
                m->addInfoNote(report.id, note);
        }
        {
            std::lock_guard<std::mutex> lock(card().mutex);
            card().line = selection.line;
            card().usingProfile = true;
        }
        // The game plays from the profile card from here on. mc1 follows by the card code's own rule (the folder next to mc0).
        PS2Runtime::IoPaths redirected = io;
        redirected.mcRoot = result.root0;
        PS2Runtime::setIoPaths(redirected);

        std::printf("[mods] %s (%.1f ms, profile %s)\n", selection.line.c_str(), result.milliseconds, utf8(config.profileDir).c_str());
        for (const profile::PackReport &report : result.packs)
        {
            std::printf("[mods]   %s: %zu fields written, %zu kept (changed by you), %zu overridden by a later pack%s%s\n", report.id.c_str(), report.written,
                        report.kept, report.overridden, report.error.empty() ? "" : "; NOT USED: ", report.error.c_str());
            for (const std::string &note : report.notes)
                std::printf("[mods]     %s\n", note.c_str());
        }
        std::fflush(stdout);
    }

    std::string cardLine()
    {
        fs::path profileDir;
        std::string line;
        bool usingProfile = false;
        {
            std::lock_guard<std::mutex> lock(card().mutex);
            profileDir = card().profileDir;
            line = card().line;
            usingProfile = card().usingProfile;
        }
        // "Back to my card" was pressed during this session: the card in use does not change until the next start, so say what will happen.
        ps2x::mods::Manager *m = manager();
        if (m && usingProfile && !profileDir.empty() && liveDataPacks(m) == 0 && ps2x::mods::profile::choseOwnCard(profileDir))
            return "Memory card: profile card until the next start. You chose Back to my card: your own card is used from then, and the profile card stays on disk.";
        return line;
    }

    bool canKeepCard()
    {
        std::lock_guard<std::mutex> lock(card().mutex);
        return !card().profileDir.empty() && ps2x::mods::profile::profileHasSave(card().profileDir);
    }

    std::string keepCard()
    {
        fs::path profileDir, base;
        {
            std::lock_guard<std::mutex> lock(card().mutex);
            profileDir = card().profileDir;
            base = card().baseCard0;
        }
        if (profileDir.empty() || base.empty())
            return "There is no profile card.";
        const auto result = ps2x::mods::profile::keepCard(profileDir, base);
        std::printf("[mods] keep this card: %s\n", result.message.c_str());
        std::fflush(stdout);
        return result.message;
    }

    bool canBackToOwnCard()
    {
        ps2x::mods::Manager *m = manager();
        fs::path profileDir;
        {
            std::lock_guard<std::mutex> lock(card().mutex);
            profileDir = card().profileDir;
        }
        if (!m || profileDir.empty() || liveDataPacks(m) != 0)
            return false;
        return ps2x::mods::profile::profileHasAnySave(profileDir) && !ps2x::mods::profile::choseOwnCard(profileDir);
    }

    std::string backToOwnCard()
    {
        if (!canBackToOwnCard())
            return "Back to my card is not available now (a data pack is on, or there is no profile save).";
        fs::path profileDir;
        {
            std::lock_guard<std::mutex> lock(card().mutex);
            profileDir = card().profileDir;
        }
        std::string message;
        ps2x::mods::profile::recordOwnCard(profileDir, &message);
        std::printf("[mods] back to my card: %s\n", message.c_str());
        std::fflush(stdout);
        return message;
    }

    DropOutcome installDropped(const std::string &utf8Path)
    {
        DropOutcome outcome;
        ps2x::mods::Manager *m = manager();
        if (!m)
        {
            outcome.message = "Mods are not available.";
            return outcome;
        }
        const ps2x::mods::Result result = m->install(fromUtf8(utf8Path));
        outcome.handled = true;
        outcome.ok = result.ok;
        outcome.message = result.message;
        std::printf("[mods] dropped %s: %s\n", utf8Path.c_str(), result.message.c_str());
        std::fflush(stdout);
        return outcome;
    }
#else
    void initialize() {}
    ps2x::mods::Manager *manager() { return nullptr; }
    std::string modsFolder() { return std::string(); }
    std::shared_ptr<TexPack::FileSource> texturePackSource() { return nullptr; }
    std::vector<std::string> themeStyleSheets() { return {}; }
    bool readVirtualFile(const std::string &, std::vector<uint8_t> &) { return false; }
    void selectCard(const std::string &) {}
    std::string cardLine() { return std::string(); }
    bool canKeepCard() { return false; }
    std::string keepCard() { return std::string(); }
    bool canBackToOwnCard() { return false; }
    std::string backToOwnCard() { return std::string(); }
    DropOutcome installDropped(const std::string &)
    {
        DropOutcome outcome;
        outcome.message = "This build has no mod manager.";
        return outcome;
    }
#endif
}
