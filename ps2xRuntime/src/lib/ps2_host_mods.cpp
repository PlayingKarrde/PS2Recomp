#include "ps2_host_mods.h"

#include "runtime/gs/gs_tex_pack.h"

#if defined(PS2X_HAS_MODS)
#include "mod_manager.h"
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
    DropOutcome installDropped(const std::string &)
    {
        DropOutcome outcome;
        outcome.message = "This build has no mod manager.";
        return outcome;
    }
#endif
}
