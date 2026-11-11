// The mod manager: which mods are installed, which are on, in what order, what each one's settings are, and the read-only
// virtual file lookup that the kinds of mod use to find a mod's files. Design: docs/mod-layer.md. Package format:
// docs/formats/fprmod.md.
//
// The manager has no SDL and no UI in its interface and no game code; the overlay's Mods tab (src/lib/ui/ui_mods.*) and the
// host (ps2_host_mods.*) are its clients, and the unit tests drive it directly with a temporary folder.
//
// ---- Files ----
//   <mods folder>/*.fprmod     the installed packages (the folder is scanned, not recursed; ".trash" is skipped)
//   <mods folder>/.trash/      where "uninstall" puts a package (moved, never deleted)
//   <state file> (mods.toml)   which mods are on, their order and their settings (mod_state.h)
//
// ---- Two views of the mod set ----
//   The LIVE set is what the player has chosen: list() and every setter work on it, and the state file follows each change
//   at once (written to a temporary file and renamed over the old one).
//   The SESSION set is the live set as it was when the manager started (init() / commitSession()). Every kind of mod takes
//   effect when the game starts, so open(), providers(), conflicts() and sessionThemeStylesheets() answer from the session
//   set, and restartNeeded() says whether the live set has moved away from it.
//
// ---- Rules (all enforced here; the messages are plain English for the player) ----
//   - A mod with a bad manifest or package is listed with its error and can do nothing.
//   - A mod is ACTIVE when it is on and its requirements hold: every dependency is active (and its version fits), and no
//     active mod conflicts with it (checked in load order, so the earlier mod wins).
//   - It cannot be turned on while a requirement fails; it cannot be turned off while an active mod needs it; it cannot be
//     uninstalled while an active mod needs it.
//   - Load order: later wins. A mod always loads after the mods it depends on (turning one on moves it down if needed,
//     moving it up past a dependency is refused).
//
// Thread safety: every public function takes one internal lock, so any thread may call them. The UI uses them from the main
// thread; open() is meant for the game thread of later kinds.
#pragma once

#include "mod_manifest.h"
#include "mod_package.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ps2x::mods
{
    struct StateFile; // mod_state.h

    // The outcome of an action. `message` is one sentence for the player either way.
    struct Result
    {
        bool ok = false;
        std::string message;
        std::string key; // install: the key of the new mod
    };

    // One installed package, as the list shows it.
    struct ModInfo
    {
        std::string key;  // the mod's id when its manifest is valid, else "file:<file name>"
        std::string file; // file name in the mods folder
        std::filesystem::path path;
        uint64_t fileSize = 0;

        bool valid = false;
        std::string error; // why it is not valid ("" when valid)

        // From the manifest, or best-effort hints when it is not valid (then only id, name and version may be set).
        std::string id;
        std::string name; // never empty: falls back to the file name
        std::string version;
        std::string authors; // joined with ", "
        std::string description;
        Kind kind = Kind::Assets;
        std::vector<Dependency> depends;
        std::vector<std::string> conflicts;
        std::vector<SettingDecl> settings;
        std::map<std::string, std::string> settingValues; // key -> value, defaults filled in
        std::string thumbnail;
        std::vector<std::string> screenshots;
        std::vector<std::string> stylesheets;

        bool enabled = false;  // the player's choice
        bool active = false;   // on and every requirement holds
        int position = 0;      // 1-based place in the load order (valid mods only)
        // For an enabled mod that is not active: why. For a disabled valid mod: why it cannot be turned on now ("" if it can).
        // For an invalid mod: empty (see `error`).
        std::string problem;
        // True when the running game loaded this mod at its start (it is in the session set).
        bool inSession = false;
    };

    struct PathConflict
    {
        std::string path;
        std::vector<std::string> providers; // mod ids in load order; the last one wins
    };

    // One content file of the session, as the winning mod provides it.
    struct SessionFile
    {
        std::string path;
        std::string modId;   // the last session mod that has the path
        uint32_t order = 0;  // that mod's place in the session's load order (0 = loads first); the later one wins
    };

    class Manager
    {
    public:
        struct Config
        {
            std::filesystem::path modsDir;   // created if missing
            std::filesystem::path statePath; // mods.toml; its folder is created if missing
            ManifestRules rules;             // the game and api a mod must name
            Limits limits;
        };

        Manager();
        ~Manager();
        Manager(const Manager &) = delete;
        Manager &operator=(const Manager &) = delete;

        // Reads the state file, scans the mods folder and takes the session snapshot. Returns false (and message()) only when the
        // folder cannot be created or read at all; a bad package is not a failure, it is a list entry.
        bool init(const Config &config);
        bool ready() const;
        const std::string &initError() const { return m_initError; }
        // Things worth a log line that were noticed on the way (an unreadable state file that was set aside, a folder that
        // could not be listed). Not errors: the manager works either way.
        std::vector<std::string> notes() const;

        // Scans the folder again (a file was added or removed by hand). The session set is kept.
        void rescan();

        std::vector<ModInfo> list() const;
        std::optional<ModInfo> find(const std::string &key) const;
        std::filesystem::path modsDir() const;
        std::filesystem::path statePath() const;
        std::filesystem::path trashDir() const;

        // ---- changes ----
        Result setEnabled(const std::string &key, bool enabled);
        // delta -1: one place earlier (loads sooner, wins less); +1: one place later (wins more).
        Result move(const std::string &key, int delta);
        // Copies a .fprmod into the mods folder after checking it. The new mod starts off, last in the order.
        Result install(const std::filesystem::path &file);
        // Moves the package to <mods folder>/.trash and forgets the mod's state.
        Result uninstall(const std::string &key);
        // A per-mod setting (declared in the manifest). The value must suit the declaration.
        Result setSetting(const std::string &key, const std::string &settingKey, const std::string &value);

        // ---- the session ----
        // Makes the live set the session set (what a restart does). Tests call this to stand in for a restart.
        void commitSession();
        // True when the live set of active mods (ids, versions, order) differs from the session set: some change is waiting for the
        // game to load it again, by a restart or, for data packs, by Load Now.
        bool restartNeeded() const;
        // The session's active mod ids in load order.
        std::vector<std::string> sessionActive() const;

        // ---- the session by kind (task h1: Load Now) ----
        // A data pack can be applied to the running game by loading the memory card again (a "hot load"); every other kind only by a
        // restart. So the session is kept in two parts that move separately: the data packs, and everything else.
        // True when the live set of active data packs (ids, versions, order) differs from what the running game has loaded.
        bool dataChangesPending() const;
        // True when the live set of active mods that are not data packs differs from the session: only a restart applies these.
        bool otherChangesPending() const;
        // The active data packs now, in load order, and their signature (what dataChangesPending compares). The host takes this
        // before it builds the profile card and hands it back to commitDataSession, so a change the player makes while the game is
        // loading is not mistaken for something that was loaded.
        struct DataLoad
        {
            std::vector<std::string> ids;
            std::string signature;
        };
        DataLoad liveDataPacks() const;
        // The running game has loaded `loaded`: the session's data packs become exactly those. The session's other mods (texture packs,
        // themes ...) are not touched, so restartNeeded() stays true for them.
        void commitDataSession(const DataLoad &loaded);

        // ---- reading mod files ----
        // The file `path` (relative, forward slashes, exact capitals) from the last active mod of the session that has it.
        // Only files under the content folders (assets/, data/, patch/, code/, theme/) are visible this way; mod.toml and the
        // manager's pictures are not.
        std::optional<std::vector<uint8_t>> open(const std::string &path) const;
        // The ids of the session mods that provide `path`, in load order (empty if none). The last one is what open() returns.
        std::vector<std::string> providers(const std::string &path) const;
        // Every content path that more than one session mod provides.
        std::vector<PathConflict> conflicts() const;
        // Every content path whose name starts with `prefix` (for example "assets/textures/"), once, with the mod that wins it. No
        // file is read. The kinds that read many files (texture packs) index them at start with this.
        std::vector<SessionFile> sessionFiles(const std::string &prefix) const;

        // ---- notes from the running game ----
        // A line the game attaches to a mod while it runs (a texture the pack could not use, with the reason). The Mods tab shows them
        // on the mod's page. Repeated lines are kept once, and at most kMaxRuntimeNotes per mod.
        static constexpr size_t kMaxRuntimeNotes = 24;
        void addRuntimeNote(const std::string &modId, const std::string &note);
        std::vector<std::string> runtimeNotes(const std::string &modId) const;
        // The same for lines that are not complaints (a data pack: "Changes 1,200 fields on the profile card.", "Kept your change to
        // wrestler 12's FIRST name."). The Mods tab shows them on the mod's page under "On your memory card". Repeats are kept once, at
        // most kMaxInfoNotes per mod.
        static constexpr size_t kMaxInfoNotes = 40;
        void addInfoNote(const std::string &modId, const std::string &note);
        std::vector<std::string> infoNotes(const std::string &modId) const;
        // Forgets both kinds of note for `modId` (a data pack whose profile was built again: the old lines may no longer be true).
        void clearNotes(const std::string &modId);
        // True when the installed mod `modId` (on or off) has the file `path`.
        bool hasFile(const std::string &modId, const std::string &path) const;
        // One file of one installed mod, on or off (the details page's pictures, a theme's images). nullopt if missing.
        std::optional<std::vector<uint8_t>> readFile(const std::string &modId, const std::string &path, uint64_t maxBytes = 16ull * 1024 * 1024,
                                                     std::string *error = nullptr) const;
        // The same through a name: "mod:<id>/<path>". The overlay's file interface calls this for the RCSS and images of theme
        // mods (RmlUi opens files by name).
        std::optional<std::vector<uint8_t>> readVirtual(const std::string &name) const;
        static std::string virtualName(const std::string &modId, const std::string &path) { return "mod:" + modId + "/" + path; }
        // The stylesheets of the session's active theme mods, as virtual names, in load order (the last is applied last).
        std::vector<std::string> sessionThemeStylesheets() const;

    private:
        struct Slot;
        struct Session;
        struct State;

        // All the *Locked functions expect m_mutex to be held.
        void scanLocked();
        void recomputeLocked();
        std::string requirementProblemLocked(const Slot &slot, const std::vector<std::string> &activeIds) const;
        bool orderOkLocked(const std::vector<size_t> &order) const;
        void normalizeOrderLocked();
        StateFile buildStateLocked() const;
        bool saveStateLocked();
        void commitSessionLocked();
        // The active mods of one side as "id@version;" in load order: the data packs (data = true) or every other kind.
        std::string signatureLocked(bool data) const;
        ModInfo infoLocked(const Slot &slot) const;
        Slot *slotLocked(const std::string &key);
        const Slot *slotLocked(const std::string &key) const;

        mutable std::mutex m_mutex;
        Config m_config;
        std::string m_initError;
        std::vector<std::string> m_notes;
        bool m_ready = false;
        bool m_listingFailed = false; // the last scan could not read the folder: do not prune the state file
        std::unique_ptr<State> m_state;
        std::vector<std::unique_ptr<Slot>> m_slots; // valid mods in load order, then the invalid ones by file name
        std::unique_ptr<Session> m_session;
        std::map<std::string, std::vector<std::string>> m_runtimeNotes; // by mod id
        std::map<std::string, std::vector<std::string>> m_infoNotes;    // by mod id
    };
}
