#include "ps2_host_mods.h"

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
