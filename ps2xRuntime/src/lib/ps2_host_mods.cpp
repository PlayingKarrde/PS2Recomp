#include "ps2_host_mods.h"

#include "runtime/gs/gs_tex_pack.h"

#if defined(PS2X_HAS_MODS)
#include "edit_pack.h"
#include "fpr_save.h"
#include "hot_load.h"
#include "mod_manager.h"
#include "profile.h"
#include "save_import.h"
#include "ps2_runtime.h"
#include "ps2_settings.h"
#include "runtime/ps2_card_activity.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>

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

        // The game's override found the places Load Now needs (hotLoadInstalled); the Mods tab offers the button only then.
        std::atomic<bool> g_hotLoadInstalled{false};

        // What selectCard decided, for the Mods tab and "Keep this card".
        struct CardState
        {
            std::mutex mutex;
            fs::path prefFolder;
            fs::path profileDir; // <pref folder>/profile ("" without a pref folder)
            fs::path baseCard0;  // the base card's slot 1 root, as the runtime had it before any redirect
            std::string line;
            bool usingProfile = false; // this session plays from the profile card
            fs::path elfPath;          // the game executable: the save importer reads the checksum table from it
            std::optional<ps2x::mods::fpr::CrcTable> crcTable; // read once, when the first save is imported
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

    namespace
    {
        // One data pack as the profile builder wants it: its identity and the text of its data files, read through the manager (on or
        // off, live or session: the caller chose which packs). The one place that knows which files a data pack holds, shared by the
        // start of a session (selectCard) and Load Now.
        ps2x::mods::profile::PackInput readPackInput(ps2x::mods::Manager *m, const std::string &id)
        {
            ps2x::mods::profile::PackInput pack;
            pack.id = id;
            const auto info = m->find(id);
            if (!info)
                return pack;
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
            ps2x::mods::edit::readEditFiles(*m, id, pack); // data/edits/*.edit.toml: created wrestlers (task e2)
            return pack;
        }

        // What the profile builder takes besides the packs and the cards, the same at a session start and at Load Now (one place, so the two
        // cannot differ). Nothing yet.
        void extendProfileConfig(ps2x::mods::profile::Config &)
        {
        }

        // The game's checksum table, read from its executable once (the importer and Load Now both need it).
        bool crcTableCached(ps2x::mods::fpr::CrcTable &table, std::string *why)
        {
            std::lock_guard<std::mutex> lock(card().mutex);
            if (!card().crcTable)
            {
                ps2x::mods::fpr::CrcTable read{};
                if (card().elfPath.empty())
                {
                    if (why)
                        *why = "the game's executable is not known yet";
                    return false;
                }
                if (!ps2x::mods::fpr::crcTableFromElf(card().elfPath, read, why))
                    return false;
                card().crcTable = read;
            }
            table = *card().crcTable;
            return true;
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
            card().elfPath = fromUtf8(elfPath);
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
            config.packs.push_back(readPackInput(m, id));
        }
        if (config.profileDir.empty())
            return; // no preference folder: the base card, exactly as without mods

        // Which card, and (when it is the profile) building it: mods/profile.h select(). The checksum table is read from the executable
        // only if the profile is going to be used.
        config.loadCrcTable = [elfPath](fpr::CrcTable &table, std::string *why) { return fpr::crcTableFromElf(fromUtf8(elfPath), table, why); };
        extendProfileConfig(config);

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
            return "Memory card: profile card until Load Now or the next start. You chose Back to my card: your own card is used from then, and the profile card stays on disk.";
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
        const bool recorded = ps2x::mods::profile::recordOwnCard(profileDir, &message);
        std::printf("[mods] back to my card: %s\n", message.c_str());
        std::fflush(stdout);
        // The choice is a change a load can apply (task h1): the Mods tab now offers Load Now for it.
        if (recorded)
            message += " Load Now uses it at once, at the title screen or the main menu.";
        return message;
    }

    bool canImportSave()
    {
        return manager() != nullptr;
    }

    DropOutcome importSave(const std::string &utf8Path, const std::string &folder)
    {
        namespace imp = ps2x::mods::import;
        DropOutcome outcome;
        ps2x::mods::Manager *m = manager();
        if (!m)
        {
            outcome.message = "Mods are not available.";
            return outcome;
        }
        imp::Env env;
        env.manager = m;
        {
            std::lock_guard<std::mutex> lock(card().mutex);
            env.profileDir = card().profileDir;
            env.baseCard0 = card().baseCard0;
            env.scratchDir = (card().prefFolder.empty() ? fs::temp_directory_path() : card().prefFolder) / "import-tmp";
        }
        // The checksum table is read from the player's own executable, once, the first time a save is imported.
        env.loadCrcTable = [](ps2x::mods::fpr::CrcTable &table, std::string *why) { return crcTableCached(table, why); };
        const imp::Outcome result = imp::importFile(env, fromUtf8(utf8Path), folder);
        outcome.handled = true;
        outcome.ok = result.ok;
        outcome.message = result.message;
        outcome.modKey = result.modKey;
        // The importer ends its report with "Restart the game to use it."; where the game can load the card again that is not the only way.
        static const std::string restartOnly = "Restart the game to use it.";
        if (result.ok && g_hotLoadInstalled.load() && outcome.message.size() >= restartOnly.size() &&
            outcome.message.compare(outcome.message.size() - restartOnly.size(), restartOnly.size(), restartOnly) == 0)
            outcome.message.replace(outcome.message.size() - restartOnly.size(), restartOnly.size(), "Use Load Now to apply it, or restart the game.");
        if (result.needsChoice)
        {
            for (const imp::Choice &c : result.choices)
            {
                outcome.folders.push_back(c.folder);
                outcome.choices.push_back(c.region); // the buttons are short; the question's text names the folders
            }
            outcome.path = utf8Path;
        }
        if (result.ok)
            for (const std::string &note : result.notes)
                m->addInfoNote(result.modKey, note);
        for (const std::string &line : result.log)
            std::printf("[mods] %s\n", line.c_str());
        std::printf("[mods] imported %s: %s\n", utf8Path.c_str(), result.message.c_str());
        std::fflush(stdout);
        return outcome;
    }

    DropOutcome handleDropped(const std::string &utf8Path)
    {
        std::string extension = utf8(fromUtf8(utf8Path).extension()); // u8string: a file name with letters outside the system's code page is fine
        for (char &c : extension)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        if (extension == ".fprmod")
            return installDropped(utf8Path);
        return importSave(utf8Path, std::string());
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
        outcome.modKey = result.ok ? result.key : std::string();
        std::printf("[mods] dropped %s: %s\n", utf8Path.c_str(), result.message.c_str());
        std::fflush(stdout);
        return outcome;
    }

    // ---- Load Now (task h1) --------------------------------------------------------------------------------------------------
    namespace
    {
        namespace hot = ps2x::mods::hotload;

        hot::Controller &controller()
        {
            static hot::Controller c;
            return c;
        }

        uint64_t nowNs()
        {
            return ps2x_host::cardSteadyNowNs();
        }

        // Changes a load can apply: the data packs that are on differ from what the game has loaded, or the player's choice of card
        // (the profile, or "Back to my card") differs from the card in use.
        bool loadPending()
        {
            ps2x::mods::Manager *m = manager();
            fs::path profileDir;
            bool usingProfile = false;
            {
                std::lock_guard<std::mutex> lock(card().mutex);
                profileDir = card().profileDir;
                usingProfile = card().usingProfile;
            }
            if (!m || profileDir.empty())
                return false;
            if (m->dataChangesPending())
                return true;
            return ps2x::mods::profile::planCard(profileDir, liveDataPacks(m)).useProfile != usingProfile;
        }

        const char *whereText(int where)
        {
            return where == 1 ? "the title screen" : (where == 2 ? "the main menu" : "an unknown screen");
        }

        // The steps of a load, bound to the real manager, card and runtime. (mods/hot_load.h has the order and the rules.)
        hot::Controller::Hooks hooks()
        {
            hot::Controller::Hooks h;
            h.cardBusy = []() -> std::string {
                const ps2x_host::CardWriteStatus status = ps2x_host::cardWriteStatus();
                if (status.writeHandlesOpen > 0)
                    return "the game is writing to the memory card. Try again in a moment.";
                return std::string();
            };
            h.plan = [](hot::Plan &plan, std::string &why) {
                ps2x::mods::Manager *m = manager();
                hot::PlanInput in;
                in.manager = m;
                {
                    std::lock_guard<std::mutex> lock(card().mutex);
                    in.profileDir = card().profileDir;
                    in.baseCard0 = card().baseCard0;
                }
                in.baseCard1 = ps2x::mods::profile::slot2Root(in.baseCard0);
                in.readPack = [m](const std::string &id) { return readPackInput(m, id); };
                in.loadCrcTable = [](ps2x::mods::fpr::CrcTable &table, std::string *reason) { return crcTableCached(table, reason); };
                in.extendConfig = [](ps2x::mods::profile::Config &config) { extendProfileConfig(config); };
                hot::PlanResult result = hot::makePlan(in);
                for (const std::string &line : result.plan.log)
                    std::printf("[mods] %s\n", line.c_str());
                if (!result.ok)
                {
                    why = result.refusal;
                    return false;
                }
                plan = std::move(result.plan);
                return true;
            };
            h.switchCard = [](const hot::Plan &plan) {
                PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
                const std::string before = utf8(paths.mcRoot);
                if (paths.mcRoot.lexically_normal() == plan.root0.lexically_normal())
                {
                    std::printf("[mods] Load Now: card root stays %s\n", before.c_str());
                    std::fflush(stdout);
                    return;
                }
                paths.mcRoot = plan.root0;
                PS2Runtime::setIoPaths(paths);
                std::printf("[mods] Load Now: card root %s -> %s\n", before.c_str(), utf8(PS2Runtime::getIoPaths().mcRoot).c_str());
                std::fflush(stdout);
            };
            h.commit = [](const hot::Plan &plan) {
                ps2x::mods::Manager *m = manager();
                if (!m)
                    return;
                m->commitDataSession(plan.data);
                // The notes on each pack's page describe the profile as it was built just now.
                for (const auto &report : plan.packs)
                {
                    m->clearNotes(report.id);
                    if (!report.error.empty())
                        m->addRuntimeNote(report.id, report.error);
                    for (const std::string &note : report.notes)
                        m->addInfoNote(report.id, note);
                }
                {
                    std::lock_guard<std::mutex> lock(card().mutex);
                    card().usingProfile = plan.useProfile;
                    card().line = plan.line;
                }
                std::printf("[mods] Load Now: %s\n", plan.line.c_str());
                for (const auto &report : plan.packs)
                    std::printf("[mods]   %s: %zu fields written, %zu kept (changed by you), %zu overridden by a later pack%s%s\n", report.id.c_str(),
                                report.written, report.kept, report.overridden, report.error.empty() ? "" : "; NOT USED: ", report.error.c_str());
                std::fflush(stdout);
            };
            return h;
        }
    }

    LoadOffer loadOffer()
    {
        LoadOffer out;
        if (!g_hotLoadInstalled.load() || !manager())
            return out;
        {
            std::lock_guard<std::mutex> lock(card().mutex);
            if (card().profileDir.empty())
                return out;
        }
        const hot::Controller::Offer offer = controller().offer(loadPending());
        out.shown = offer.shown;
        out.enabled = offer.enabled;
        out.unsaved = offer.unsaved;
        out.note = offer.note;
        out.help = offer.help;
        out.question = offer.question;
        return out;
    }

    std::string requestLoad(bool *accepted)
    {
        if (accepted)
            *accepted = false;
        if (!g_hotLoadInstalled.load() || !manager())
            return "Load Now is not available in this build.";
        const hot::Controller::Requested result = controller().request(loadPending(), nowNs());
        if (accepted)
            *accepted = result.ok;
        std::printf("[mods] Load Now %s: %s\n", result.ok ? "requested" : "not requested", result.message.c_str());
        std::fflush(stdout);
        return result.message;
    }

    void hotLoadGuestStopped()
    {
        controller().guestStopped(nowNs());
    }

    void hotLoadGuestResumed()
    {
        controller().guestResumed();
    }

    void hotLoadTick()
    {
        if (controller().expire(nowNs()))
        {
            std::printf("[mods] Load Now: %s\n", controller().message().c_str());
            std::fflush(stdout);
        }
    }

    bool takeLoadResult(std::string &message)
    {
        return controller().takeResult(message);
    }

    void hotLoadInstalled()
    {
        g_hotLoadInstalled.store(true);
    }

    void hotLoadSafePoint(int where, bool unsaved)
    {
        controller().noteSafePoint(static_cast<hot::Where>(where), unsaved, nowNs());
    }

    bool hotLoadRequested()
    {
        return controller().requested();
    }

    bool hotLoadBegin(int where)
    {
        const auto started = std::chrono::steady_clock::now();
        const bool go = controller().begin(static_cast<hot::Where>(where), nowNs(), hooks());
        if (go)
        {
            std::printf("[mods] Load Now: starting the game's loader at %s (%.1f ms to prepare)\n", whereText(where),
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        }
        else if (controller().state() == hot::Controller::State::Refused)
        {
            std::printf("[mods] %s\n", controller().message().c_str());
        }
        std::fflush(stdout);
        return go;
    }

    void hotLoadEnd(uint8_t *roster, size_t rosterSize, uint8_t *slots, size_t slotsSize)
    {
        hot::GuestView view;
        view.roster = roster;
        view.rosterSize = rosterSize;
        view.slots = slots;
        view.slotsSize = slotsSize;
        controller().end(view, hooks());
        if (controller().state() == hot::Controller::State::Loaded)
            std::printf("[mods] Load Now: the game's memory matches the card (%zu unused edit slots had leftovers and were cleared)\n", controller().scrubbedSlots());
        else
            std::printf("[mods] Load Now: %s\n", controller().message().c_str());
        std::fflush(stdout);
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
    bool canImportSave() { return false; }
    DropOutcome importSave(const std::string &, const std::string &) { return installDropped(std::string()); }
    DropOutcome handleDropped(const std::string &path) { return installDropped(path); }
    LoadOffer loadOffer() { return LoadOffer(); }
    std::string requestLoad(bool *accepted)
    {
        if (accepted)
            *accepted = false;
        return "This build has no mod manager.";
    }
    void hotLoadGuestStopped() {}
    void hotLoadGuestResumed() {}
    void hotLoadTick() {}
    bool takeLoadResult(std::string &) { return false; }
    void hotLoadInstalled() {}
    void hotLoadSafePoint(int, bool) {}
    bool hotLoadRequested() { return false; }
    bool hotLoadBegin(int) { return false; }
    void hotLoadEnd(uint8_t *, size_t, uint8_t *, size_t) {}
#endif
}
