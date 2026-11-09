#include "mod_manager.h"

#include "mod_state.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>

namespace ps2x::mods
{
    namespace fs = std::filesystem;

    namespace
    {
        std::string utf8(const fs::path &path)
        {
            const auto text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        fs::path fromUtf8(const std::string &text)
        {
            return fs::path(std::u8string(text.begin(), text.end()));
        }

        std::string lowerAscii(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        bool hasModExtension(const fs::path &path)
        {
            return lowerAscii(utf8(path.extension())) == ".fprmod";
        }

        // Only these top-level folders of a package are visible through open() / providers(): they are where the kinds of
        // mod keep their content. Everything else (mod.toml, the manager's pictures) is for the manager.
        bool isContentPath(const std::string &path)
        {
            const size_t slash = path.find('/');
            if (slash == std::string::npos)
                return false;
            const std::string top = path.substr(0, slash);
            return top == "assets" || top == "data" || top == "patch" || top == "code" || top == "theme";
        }

        bool contains(const std::vector<std::string> &list, const std::string &item)
        {
            return std::find(list.begin(), list.end(), item) != list.end();
        }

        std::string joinAuthors(const std::vector<std::string> &authors)
        {
            std::string out;
            for (const std::string &a : authors)
                out += (out.empty() ? "" : ", ") + a;
            return out;
        }

        // Writes `text` to `target` through a temporary file and a rename.
        bool writeFileAtomically(const fs::path &target, const std::string &text)
        {
            std::error_code ec;
            if (target.has_parent_path())
                fs::create_directories(target.parent_path(), ec);
            fs::path temp = target;
            temp += ".tmp";
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                out.write(text.data(), static_cast<std::streamsize>(text.size()));
                out.flush();
                if (!out)
                    return false;
            }
            fs::rename(temp, target, ec);
            if (ec)
            {
                std::error_code ignored;
                fs::remove(temp, ignored);
                return false;
            }
            return true;
        }
    }

    // ---- internal types ---------------------------------------------------------------------------------------

    struct Manager::Slot
    {
        ModInfo info;       // everything that does not change while the file stays the same
        Manifest manifest;  // valid slots only
        std::vector<PackageEntry> entries;
        bool enabled = false;
        bool active = false;
        std::string problem;
        std::map<std::string, std::string> settingValues;
    };

    struct Manager::State
    {
        StateFile file; // as last read or written: the order, enabled flags and settings of every mod the manager knows
    };

    struct Manager::Session
    {
        struct Mod
        {
            std::string id;
            std::string version;
            fs::path path;
        };
        std::vector<Mod> mods; // active mods in load order
        std::map<std::string, std::vector<size_t>> providers; // content path -> indices into `mods`, ascending
        std::vector<std::string> stylesheets;
        std::string signature;
    };

    Manager::Manager() : m_state(std::make_unique<State>()), m_session(std::make_unique<Session>()) {}
    Manager::~Manager() = default;

    // ---- scanning and state -------------------------------------------------------------------------------------

    bool Manager::init(const Config &config)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = config;
        m_ready = false;
        m_initError.clear();
        m_notes.clear();
        m_state->file = StateFile();
        m_slots.clear();

        std::error_code ec;
        fs::create_directories(m_config.modsDir, ec);
        if (!fs::is_directory(m_config.modsDir, ec))
        {
            m_initError = "The mods folder " + utf8(m_config.modsDir) + " cannot be created or read.";
            return false;
        }

        std::error_code existsError;
        if (!m_config.statePath.empty() && fs::exists(m_config.statePath, existsError))
        {
            std::string content;
            bool readOk = false;
            {
                // Closed again before anything below renames the file (Windows will not rename an open file).
                std::ifstream in(m_config.statePath, std::ios::binary);
                std::ostringstream buffer;
                buffer << in.rdbuf();
                content = buffer.str();
                readOk = static_cast<bool>(in) || in.eof();
            }
            std::string error;
            if (!readOk || !parseStateFile(content, m_state->file, &error))
            {
                // Set the unreadable file aside rather than overwrite it with the next change.
                fs::path bad = m_config.statePath;
                bad += ".bad";
                std::error_code renameError;
                fs::rename(m_config.statePath, bad, renameError);
                m_notes.push_back((error.empty() ? std::string("The mod state file cannot be read.") : error) + " Starting with no mods turned on" +
                                  (renameError ? "." : "; the old file is kept as " + utf8(bad.filename()) + "."));
                m_state->file = StateFile();
            }
        }

        scanLocked();
        commitSessionLocked();
        m_ready = true;
        return true;
    }

    bool Manager::ready() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_ready;
    }

    std::vector<std::string> Manager::notes() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_notes;
    }

    void Manager::rescan()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_ready)
            return;
        scanLocked();
    }

    void Manager::scanLocked()
    {
        // A rescan starts from what the manager holds now (order, on or off, settings), not from the file as it was last read:
        // nothing may have been written yet, and a mod that appeared since must go after the ones already in the order.
        // Entries for mods that are not installed are kept after those (a mod that comes back finds its settings).
        if (!m_slots.empty())
        {
            StateFile current = buildStateLocked();
            for (const StateMod &old : m_state->file.mods)
            {
                const bool known = std::any_of(current.mods.begin(), current.mods.end(), [&](const StateMod &m) { return m.id == old.id; });
                if (!known)
                    current.mods.push_back(old);
            }
            m_state->file = std::move(current);
        }
        m_listingFailed = false;
        std::vector<fs::path> files;
        std::error_code ec;
        fs::directory_iterator it(m_config.modsDir, ec);
        for (; !ec && it != fs::directory_iterator(); it.increment(ec))
        {
            std::error_code typeError;
            if (it->is_regular_file(typeError) && hasModExtension(it->path()))
                files.push_back(it->path());
        }
        if (ec)
        {
            m_listingFailed = true;
            m_notes.push_back("The mods folder could not be listed completely: " + ec.message() + ".");
        }
        std::sort(files.begin(), files.end(), [](const fs::path &a, const fs::path &b) { return utf8(a.filename()) < utf8(b.filename()); });

        std::vector<std::unique_ptr<Slot>> valid;
        std::vector<std::unique_ptr<Slot>> invalid;
        for (const fs::path &file : files)
        {
            auto slot = std::make_unique<Slot>();
            ModInfo &info = slot->info;
            info.file = utf8(file.filename());
            info.path = file;
            std::error_code sizeError;
            info.fileSize = fs::file_size(file, sizeError);
            PackageResult read = readPackage(file, m_config.rules, m_config.limits);
            if (read.package)
            {
                const std::string id = read.package->manifest.id;
                const std::string name = read.package->manifest.name;
                const std::string version = read.package->manifest.version;
                bool duplicate = false;
                for (const auto &other : valid)
                    if (other->info.id == id)
                        duplicate = true;
                if (duplicate)
                {
                    read.package.reset();
                    read.error = "Another file already provides the mod '" + id + "'.";
                    read.hints.id = id;
                    read.hints.name = name;
                    read.hints.version = version;
                }
            }
            if (read.package)
            {
                slot->manifest = read.package->manifest;
                slot->entries = read.package->entries;
                const Manifest &m = slot->manifest;
                info.valid = true;
                info.key = m.id;
                info.id = m.id;
                info.name = m.name;
                info.version = m.version;
                info.authors = joinAuthors(m.authors);
                info.description = m.description;
                info.kind = m.kind;
                info.depends = m.depends;
                info.conflicts = m.conflicts;
                info.settings = m.settings;
                info.thumbnail = m.thumbnail;
                info.screenshots = m.screenshots;
                info.stylesheets = m.stylesheets;
                valid.push_back(std::move(slot));
            }
            else
            {
                info.valid = false;
                info.error = read.error;
                info.key = "file:" + info.file;
                info.id = read.hints.id;
                info.name = !read.hints.name.empty() ? read.hints.name : info.file;
                info.version = read.hints.version;
                invalid.push_back(std::move(slot));
            }
        }

        // Load order: what the state file lists first, in its order; then the mods it has not seen, by file name.
        std::vector<std::unique_ptr<Slot>> ordered;
        for (const StateMod &entry : m_state->file.mods)
        {
            for (auto it2 = valid.begin(); it2 != valid.end(); ++it2)
            {
                if ((*it2)->info.id == entry.id)
                {
                    ordered.push_back(std::move(*it2));
                    valid.erase(it2);
                    break;
                }
            }
        }
        for (auto &slot : valid)
            ordered.push_back(std::move(slot));

        for (auto &slot : ordered)
        {
            const StateMod *saved = nullptr;
            for (const StateMod &entry : m_state->file.mods)
                if (entry.id == slot->info.id)
                    saved = &entry;
            slot->enabled = saved && saved->enabled;
            slot->settingValues.clear();
            for (const SettingDecl &decl : slot->manifest.settings)
            {
                std::string value = decl.defaultValue;
                if (saved)
                {
                    const auto found = saved->settings.find(decl.key);
                    if (found != saved->settings.end() && settingValueValid(decl, found->second.text))
                        value = found->second.text;
                }
                slot->settingValues[decl.key] = value;
            }
        }

        m_slots.clear();
        for (auto &slot : ordered)
            m_slots.push_back(std::move(slot));
        for (auto &slot : invalid)
            m_slots.push_back(std::move(slot));

        normalizeOrderLocked();
        recomputeLocked();
    }

    // A mod's requirements against the mods that are active so far (in load order). "" when they all hold.
    std::string Manager::requirementProblemLocked(const Slot &slot, const std::vector<std::string> &activeIds) const
    {
        auto nameOf = [&](const std::string &id) {
            const Slot *other = slotLocked(id);
            return other ? other->info.name : id;
        };
        for (const Dependency &dep : slot.manifest.depends)
        {
            const Slot *other = slotLocked(dep.id);
            if (!other || !other->info.valid)
                return "Needs the mod '" + dep.id + "', which is not installed.";
            if (!dep.constraint.empty() && !satisfies(other->manifest.parsedVersion, dep.constraint))
                return "Needs " + other->info.name + " " + dep.constraint + ", but version " + other->info.version + " is installed.";
            if (!contains(activeIds, dep.id))
                return other->enabled ? "Needs " + other->info.name + ", which is on but cannot be used."
                                      : "Needs " + other->info.name + " to be turned on.";
        }
        for (const std::string &conflict : slot.manifest.conflicts)
            if (contains(activeIds, conflict))
                return "Conflicts with " + nameOf(conflict) + ", which is on.";
        for (const std::string &activeId : activeIds)
        {
            const Slot *other = slotLocked(activeId);
            if (other && contains(other->manifest.conflicts, slot.info.id))
                return "Conflicts with " + other->info.name + ", which is on.";
        }
        return std::string();
    }

    void Manager::recomputeLocked()
    {
        std::vector<std::string> activeIds;
        for (auto &slot : m_slots)
        {
            slot->active = false;
            slot->problem.clear();
            if (!slot->info.valid || !slot->enabled)
                continue;
            slot->problem = requirementProblemLocked(*slot, activeIds);
            if (slot->problem.empty())
            {
                slot->active = true;
                activeIds.push_back(slot->info.id);
            }
        }
        // A mod that is off: why it could not be turned on now.
        for (auto &slot : m_slots)
            if (slot->info.valid && !slot->enabled)
                slot->problem = requirementProblemLocked(*slot, activeIds);
    }

    // True when, in the given order of slots, every enabled mod loads after the enabled mods it depends on.
    bool Manager::orderOkLocked(const std::vector<size_t> &order) const
    {
        std::map<std::string, size_t> position;
        for (size_t i = 0; i < order.size(); ++i)
            position[m_slots[order[i]]->info.id] = i;
        for (size_t i = 0; i < order.size(); ++i)
        {
            const Slot &slot = *m_slots[order[i]];
            if (!slot.enabled)
                continue;
            for (const Dependency &dep : slot.manifest.depends)
            {
                const auto found = position.find(dep.id);
                if (found == position.end())
                    continue;
                const Slot *other = slotLocked(dep.id);
                if (other && other->enabled && found->second > i)
                    return false;
            }
        }
        return true;
    }

    // Moves a mod down to just after the last of its (enabled) dependencies, until every enabled mod loads after what it needs.
    void Manager::normalizeOrderLocked()
    {
        size_t validCount = 0;
        while (validCount < m_slots.size() && m_slots[validCount]->info.valid)
            ++validCount;
        for (int pass = 0; pass < 200; ++pass)
        {
            bool changed = false;
            for (size_t i = 0; i < validCount && !changed; ++i)
            {
                const Slot &slot = *m_slots[i];
                if (!slot.enabled)
                    continue;
                size_t lastDependency = i;
                for (const Dependency &dep : slot.manifest.depends)
                {
                    for (size_t j = i + 1; j < validCount; ++j)
                        if (m_slots[j]->info.id == dep.id && m_slots[j]->enabled)
                            lastDependency = std::max(lastDependency, j);
                }
                if (lastDependency > i)
                {
                    std::unique_ptr<Slot> moved = std::move(m_slots[i]);
                    m_slots.erase(m_slots.begin() + static_cast<long>(i));
                    m_slots.insert(m_slots.begin() + static_cast<long>(lastDependency), std::move(moved));
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
    }

    StateFile Manager::buildStateLocked() const
    {
        StateFile file;
        for (const auto &slot : m_slots)
        {
            if (!slot->info.valid)
                continue;
            StateMod mod;
            mod.id = slot->info.id;
            mod.enabled = slot->enabled;
            for (const SettingDecl &decl : slot->manifest.settings)
            {
                StateValue value;
                value.text = slot->settingValues.count(decl.key) ? slot->settingValues[decl.key] : decl.defaultValue;
                value.kind = decl.type == SettingDecl::Type::Bool ? StateValue::Kind::Bool
                                                                   : (decl.type == SettingDecl::Type::Int ? StateValue::Kind::Int : StateValue::Kind::String);
                mod.settings[decl.key] = value;
            }
            file.mods.push_back(std::move(mod));
        }
        return file;
    }

    bool Manager::saveStateLocked()
    {
        StateFile file = buildStateLocked();
        if (m_listingFailed)
        {
            // The folder could not be read completely: keep what the file said about mods we did not see.
            for (const StateMod &old : m_state->file.mods)
            {
                const bool known = std::any_of(file.mods.begin(), file.mods.end(), [&](const StateMod &m) { return m.id == old.id; });
                if (!known)
                    file.mods.push_back(old);
            }
        }
        m_state->file = file;
        if (m_config.statePath.empty())
            return true;
        return writeFileAtomically(m_config.statePath, formatStateFile(file));
    }

    // ---- queries --------------------------------------------------------------------------------------------------

    Manager::Slot *Manager::slotLocked(const std::string &key)
    {
        for (auto &slot : m_slots)
            if (slot->info.key == key)
                return slot.get();
        return nullptr;
    }

    const Manager::Slot *Manager::slotLocked(const std::string &key) const
    {
        for (const auto &slot : m_slots)
            if (slot->info.key == key)
                return slot.get();
        return nullptr;
    }

    ModInfo Manager::infoLocked(const Slot &slot) const
    {
        ModInfo info = slot.info;
        info.enabled = slot.enabled;
        info.active = slot.active;
        info.problem = slot.problem;
        info.settingValues = slot.settingValues;
        info.position = 0;
        if (slot.info.valid)
        {
            int position = 0;
            for (const auto &other : m_slots)
            {
                if (!other->info.valid)
                    break;
                ++position;
                if (other.get() == &slot)
                {
                    info.position = position;
                    break;
                }
            }
            info.inSession = std::any_of(m_session->mods.begin(), m_session->mods.end(), [&](const Session::Mod &m) { return m.id == slot.info.id; });
        }
        return info;
    }

    std::vector<ModInfo> Manager::list() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<ModInfo> out;
        for (const auto &slot : m_slots)
            out.push_back(infoLocked(*slot));
        return out;
    }

    std::optional<ModInfo> Manager::find(const std::string &key) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot *slot = slotLocked(key);
        if (!slot)
            return std::nullopt;
        return infoLocked(*slot);
    }

    fs::path Manager::modsDir() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_config.modsDir;
    }

    fs::path Manager::statePath() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_config.statePath;
    }

    fs::path Manager::trashDir() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_config.modsDir / ".trash";
    }

    // ---- changes --------------------------------------------------------------------------------------------------

    Result Manager::setEnabled(const std::string &key, bool enabled)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Result result;
        Slot *slot = slotLocked(key);
        if (!slot)
        {
            result.message = "That mod is not installed.";
            return result;
        }
        if (!slot->info.valid)
        {
            result.message = "This mod cannot be used: " + slot->info.error;
            return result;
        }
        if (slot->enabled == enabled)
        {
            result.ok = true;
            result.message = enabled ? slot->info.name + " is already on." : slot->info.name + " is already off.";
            return result;
        }
        if (enabled)
        {
            std::vector<std::string> activeIds;
            for (const auto &other : m_slots)
                if (other->active)
                    activeIds.push_back(other->info.id);
            const std::string problem = requirementProblemLocked(*slot, activeIds);
            if (!problem.empty())
            {
                result.message = "Cannot turn on " + slot->info.name + ". " + problem;
                return result;
            }
            slot->enabled = true;
            normalizeOrderLocked();
        }
        else
        {
            for (const auto &other : m_slots)
            {
                if (!other->active || other.get() == slot)
                    continue;
                for (const Dependency &dep : other->manifest.depends)
                {
                    if (dep.id == slot->info.id)
                    {
                        result.message = "Cannot turn off " + slot->info.name + ": " + other->info.name + " needs it. Turn " + other->info.name + " off first.";
                        return result;
                    }
                }
            }
            slot->enabled = false;
        }
        recomputeLocked();
        saveStateLocked();
        result.ok = true;
        result.message = slot->info.name + (enabled ? " is on." : " is off.");
        return result;
    }

    Result Manager::move(const std::string &key, int delta)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Result result;
        Slot *slot = slotLocked(key);
        if (!slot || !slot->info.valid)
        {
            result.message = "Only a working mod can be moved.";
            return result;
        }
        size_t validCount = 0;
        while (validCount < m_slots.size() && m_slots[validCount]->info.valid)
            ++validCount;
        size_t index = 0;
        while (index < validCount && m_slots[index].get() != slot)
            ++index;
        const long target = static_cast<long>(index) + (delta < 0 ? -1 : 1);
        if (delta == 0 || target < 0 || target >= static_cast<long>(validCount))
        {
            result.message = delta < 0 ? slot->info.name + " is already first in the load order." : slot->info.name + " is already last in the load order.";
            return result;
        }
        std::vector<size_t> trial(validCount);
        for (size_t i = 0; i < validCount; ++i)
            trial[i] = i;
        std::swap(trial[index], trial[static_cast<size_t>(target)]);
        if (!orderOkLocked(trial))
        {
            const Slot &neighbour = *m_slots[static_cast<size_t>(target)];
            result.message = delta < 0 ? slot->info.name + " needs " + neighbour.info.name + ", so it has to load after it."
                                       : neighbour.info.name + " needs " + slot->info.name + ", so " + slot->info.name + " has to load before it.";
            return result;
        }
        std::swap(m_slots[index], m_slots[static_cast<size_t>(target)]);
        recomputeLocked();
        saveStateLocked();
        result.ok = true;
        result.message = slot->info.name + (delta < 0 ? " now loads earlier." : " now loads later, so it wins over mods above it.");
        return result;
    }

    Result Manager::install(const fs::path &file)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Result result;
        if (!m_ready)
        {
            result.message = "The mod manager is not running.";
            return result;
        }
        std::error_code ec;
        if (!hasModExtension(file))
        {
            result.message = "That is not a mod file. Mods end in .fprmod.";
            return result;
        }
        if (!fs::is_regular_file(file, ec))
        {
            result.message = "That file cannot be found.";
            return result;
        }
        for (const auto &slot : m_slots)
        {
            std::error_code same;
            if (fs::equivalent(slot->info.path, file, same) && !same)
            {
                result.message = "That mod is already installed.";
                return result;
            }
        }
        PackageResult read = readPackage(file, m_config.rules, m_config.limits);
        if (!read.package)
        {
            result.message = "Not installed: " + read.error;
            return result;
        }
        const Manifest &manifest = read.package->manifest;
        for (const auto &slot : m_slots)
        {
            if (slot->info.valid && slot->info.id == manifest.id)
            {
                result.message = "Not installed: the mod '" + manifest.id + "' is already installed (version " + slot->info.version +
                                 "). Uninstall it first to install another version.";
                return result;
            }
        }

        // The file name: the one it has, or the mod's id if that name is taken.
        const std::string base = utf8(file.filename());
        fs::path destination = m_config.modsDir / fromUtf8(base);
        if (fs::exists(destination, ec))
        {
            destination = m_config.modsDir / fromUtf8(manifest.id + ".fprmod");
            for (int n = 2; fs::exists(destination, ec) && n < 100; ++n)
                destination = m_config.modsDir / fromUtf8(manifest.id + "-" + std::to_string(n) + ".fprmod");
            if (fs::exists(destination, ec))
            {
                result.message = "Not installed: the mods folder already has files with every name this mod could take.";
                return result;
            }
        }
        fs::path partial = destination;
        partial += ".part";
        fs::copy_file(file, partial, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            std::error_code ignored;
            fs::remove(partial, ignored);
            result.message = "Not installed: the file could not be copied into the mods folder (" + ec.message() + ").";
            return result;
        }
        fs::rename(partial, destination, ec);
        if (ec)
        {
            std::error_code ignored;
            fs::remove(partial, ignored);
            result.message = "Not installed: the copy could not be put in place (" + ec.message() + ").";
            return result;
        }

        scanLocked();
        saveStateLocked();
        result.ok = true;
        result.key = manifest.id;
        result.message = "Installed " + manifest.name + " " + manifest.version + ". Turn it on to use it.";
        return result;
    }

    Result Manager::uninstall(const std::string &key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Result result;
        Slot *slot = slotLocked(key);
        if (!slot)
        {
            result.message = "That mod is not installed.";
            return result;
        }
        for (const auto &other : m_slots)
        {
            if (!other->active || other.get() == slot || !slot->info.valid)
                continue;
            for (const Dependency &dep : other->manifest.depends)
            {
                if (dep.id == slot->info.id)
                {
                    result.message = "Cannot uninstall " + slot->info.name + ": " + other->info.name + " needs it. Turn " + other->info.name + " off first.";
                    return result;
                }
            }
        }
        const std::string name = slot->info.name;
        const fs::path source = slot->info.path;
        const fs::path trash = m_config.modsDir / ".trash";
        std::error_code ec;
        fs::create_directories(trash, ec);
        fs::path destination = trash / source.filename();
        if (fs::exists(destination, ec))
        {
            const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            destination = trash / fromUtf8(utf8(source.stem()) + "." + std::to_string(stamp) + utf8(source.extension()));
            for (int n = 2; fs::exists(destination, ec) && n < 100; ++n)
                destination = trash / fromUtf8(utf8(source.stem()) + "." + std::to_string(stamp) + "-" + std::to_string(n) + utf8(source.extension()));
        }
        fs::rename(source, destination, ec);
        if (ec)
        {
            result.message = "Cannot uninstall " + name + ": the file could not be moved to the .trash folder (" + ec.message() + ").";
            return result;
        }
        for (auto it = m_slots.begin(); it != m_slots.end(); ++it)
        {
            if (it->get() == slot)
            {
                m_slots.erase(it);
                break;
            }
        }
        recomputeLocked();
        saveStateLocked();
        result.ok = true;
        result.message = "Removed " + name + ". The file is in the mods folder's .trash folder.";
        return result;
    }

    Result Manager::setSetting(const std::string &key, const std::string &settingKey, const std::string &value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Result result;
        Slot *slot = slotLocked(key);
        if (!slot || !slot->info.valid)
        {
            result.message = "That mod is not installed.";
            return result;
        }
        const SettingDecl *decl = nullptr;
        for (const SettingDecl &d : slot->manifest.settings)
            if (d.key == settingKey)
                decl = &d;
        if (!decl)
        {
            result.message = "This mod has no setting '" + settingKey + "'.";
            return result;
        }
        if (!settingValueValid(*decl, value))
        {
            result.message = "'" + value + "' is not a valid value for " + decl->label + ".";
            return result;
        }
        slot->settingValues[settingKey] = value;
        saveStateLocked();
        result.ok = true;
        const std::string shown = decl->type == SettingDecl::Type::Bool ? (value == "true" ? "on" : "off") : value;
        result.message = decl->label + " is " + shown + ".";
        return result;
    }

    // ---- the session ----------------------------------------------------------------------------------------------

    std::string Manager::signatureLocked() const
    {
        std::string signature;
        for (const auto &slot : m_slots)
            if (slot->active)
                signature += slot->info.id + "@" + slot->info.version + ";";
        return signature;
    }

    void Manager::commitSessionLocked()
    {
        auto session = std::make_unique<Session>();
        for (const auto &slot : m_slots)
        {
            if (!slot->active)
                continue;
            Session::Mod mod;
            mod.id = slot->info.id;
            mod.version = slot->info.version;
            mod.path = slot->info.path;
            const size_t index = session->mods.size();
            session->mods.push_back(std::move(mod));
            for (const PackageEntry &entry : slot->entries)
                if (!entry.isDirectory && isContentPath(entry.name))
                    session->providers[entry.name].push_back(index);
            if (slot->manifest.kind == Kind::Theme)
                for (const std::string &sheet : slot->manifest.stylesheets)
                    session->stylesheets.push_back(virtualName(slot->info.id, sheet));
        }
        session->signature = signatureLocked();
        m_session = std::move(session);
    }

    void Manager::commitSession()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        commitSessionLocked();
    }

    bool Manager::restartNeeded() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return signatureLocked() != m_session->signature;
    }

    std::vector<std::string> Manager::sessionActive() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> ids;
        for (const Session::Mod &mod : m_session->mods)
            ids.push_back(mod.id);
        return ids;
    }

    // ---- reading mod files ----------------------------------------------------------------------------------------

    std::optional<std::vector<uint8_t>> Manager::open(const std::string &path) const
    {
        std::vector<fs::path> candidates; // last provider first
        uint64_t maxBytes = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_session->providers.find(path);
            if (found == m_session->providers.end())
                return std::nullopt;
            for (auto it = found->second.rbegin(); it != found->second.rend(); ++it)
                candidates.push_back(m_session->mods[*it].path);
            maxBytes = m_config.limits.maxEntryBytes;
        }
        for (const fs::path &package : candidates)
        {
            // If the winner's file has gone since the start, the mod below it still provides the path.
            if (auto bytes = readPackageEntry(package, path, maxBytes))
                return bytes;
        }
        return std::nullopt;
    }

    std::vector<std::string> Manager::providers(const std::string &path) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> ids;
        const auto found = m_session->providers.find(path);
        if (found != m_session->providers.end())
            for (const size_t index : found->second)
                ids.push_back(m_session->mods[index].id);
        return ids;
    }

    std::vector<PathConflict> Manager::conflicts() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<PathConflict> out;
        for (const auto &entry : m_session->providers)
        {
            if (entry.second.size() < 2)
                continue;
            PathConflict conflict;
            conflict.path = entry.first;
            for (const size_t index : entry.second)
                conflict.providers.push_back(m_session->mods[index].id);
            out.push_back(std::move(conflict));
        }
        return out;
    }

    std::optional<std::vector<uint8_t>> Manager::readFile(const std::string &modId, const std::string &path, uint64_t maxBytes,
                                                          std::string *error) const
    {
        fs::path package;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const Slot *slot = slotLocked(modId);
            if (!slot || !slot->info.valid)
            {
                if (error)
                    *error = "That mod is not installed.";
                return std::nullopt;
            }
            std::string why;
            if (!isSafeRelativePath(path, &why))
            {
                if (error)
                    *error = "Not a package path: " + why + ".";
                return std::nullopt;
            }
            const bool listed = std::any_of(slot->entries.begin(), slot->entries.end(),
                                            [&](const PackageEntry &entry) { return !entry.isDirectory && entry.name == path; });
            if (!listed)
            {
                if (error)
                    *error = "The package has no file '" + path + "'.";
                return std::nullopt;
            }
            package = slot->info.path;
        }
        return readPackageEntry(package, path, maxBytes, error);
    }

    std::optional<std::vector<uint8_t>> Manager::readVirtual(const std::string &name) const
    {
        if (name.compare(0, 4, "mod:") != 0)
            return std::nullopt;
        const size_t slash = name.find('/', 4);
        if (slash == std::string::npos)
            return std::nullopt;
        return readFile(name.substr(4, slash - 4), name.substr(slash + 1));
    }

    std::vector<std::string> Manager::sessionThemeStylesheets() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_session->stylesheets;
    }
}
