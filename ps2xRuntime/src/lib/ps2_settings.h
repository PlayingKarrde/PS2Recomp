#pragma once

// The game's settings file, and how it meets the environment variables.
//
// What it is for: the settings overlay (src/lib/ui) changes a few things about how the game is presented, and they
// should still be there the next time the game starts. They are kept in a small text file that we own, one
// `key = value` per line (`#` starts a comment):
//
//     # FPWR settings
//     renderer = gpu
//     resolution = 2
//     aspect = 16:9
//
// Where it is: SDL's preference path for the app, `<pref path>/settings.cfg`:
//     Windows  %APPDATA%\FPWR-Recomp\FPWR\settings.cfg
//     Linux    ~/.local/share/FPWR-Recomp/FPWR/settings.cfg   ($XDG_DATA_HOME is honoured)
//     macOS    ~/Library/Application Support/FPWR-Recomp/FPWR/settings.cfg
// PS2X_SETTINGS=<file> uses another file instead (the tests do, so a player's own settings never leak into a test).
//
// How it meets the environment: every setting has an environment variable that the game already reads
// (PS2X_GS_SCALE, PS2X_GS_WIDE...) or gets one (PS2X_FULLSCREEN, PS2X_VOLUME). At start-up, initialize() puts the
// file's values into the process environment for every setting whose variable is not already set, and only when the
// value differs from the default, so the rest of the runtime keeps reading its environment variables as before and an
// unset file changes nothing. A variable that was set before the program started wins over the file, and the
// overlay shows that setting as fixed by it (fixedByEnvironment()).
//
// Not thread-safe against itself in the sense that it expects the main thread to call it; reads are guarded by a
// mutex so the audio or game thread may call get().
//
// No game code and no SDL headers here, so it can be unit-tested. (The default path uses SDL_GetPrefPath in the .cpp.)

#include <optional>
#include <string>
#include <vector>

namespace ps2x_settings
{
    struct Definition
    {
        const char *key;          // the name in the file and in the overlay
        const char *envName;      // the environment variable that fixes it, which the runtime reads
        const char *defaultValue; // the value that changes nothing
    };

    // renderer (gpu|cpu), resolution (1..4), aspect (4:3|16:9|21:9), fullscreen (on|off), vsync (on|off),
    // filter (sharp|linear), volume (0..100), texture_tools (on|off), skip_intros (on|off: start at the title screen, task i1),
    // strike_depth (original|wide|wider: a strike's hit box x1, x1.5, x2 front to back, task s1; read by the game while it runs)
    const std::vector<Definition> &definitions();
    const Definition *find(const std::string &key);

    // ---- translating between a setting's value and its environment variable (also used by the tests) ----

    // The setting value an environment variable's text stands for. nullopt when the text means nothing sensible.
    std::optional<std::string> valueFromEnvironment(const std::string &key, const std::string &envText);
    // The text to put in the environment for a value, or nullopt when the value is the default (nothing to set).
    std::optional<std::string> environmentFromValue(const std::string &key, const std::string &value);

    // ---- the file format (also used by the tests) ----

    struct Entry
    {
        std::string key;
        std::string value;
    };
    // Reads `key = value` lines. Blank lines, `#` comments, lines without `=` and unknown keys are skipped; a later
    // line for the same key wins. Surrounding spaces are trimmed.
    std::vector<Entry> parseFile(const std::string &text);
    std::string formatFile(const std::vector<Entry> &entries);

    // ---- the run-time store ----

    // The file the settings come from: PS2X_SETTINGS, else SDL's preference path. Empty when neither exists.
    std::string defaultPath();

    // Reads `path` (a missing file is fine), notes which settings the environment fixes, and puts the file's values
    // into the environment as described above. Call once, before anything reads the variables. A second call is
    // ignored. Logs one line with the path.
    void initialize(const std::string &path);
    bool initialized();
    const std::string &path();

    // The value now: the environment's, else the file's (as changed since), else the default.
    std::string get(const std::string &key);
    // The value the program started with (what the running game uses).
    std::string startupValue(const std::string &key);
    // True when an environment variable fixed the setting at start-up; envName() is its name.
    bool fixedByEnvironment(const std::string &key);
    std::string envName(const std::string &key);

    // Changes a setting in the store (the file is written by save()). Ignored for a setting the environment fixes.
    // Returns false for an unknown key or a setting that is fixed.
    bool set(const std::string &key, const std::string &value);
    bool dirty();
    // Writes the file if anything changed since it was read or last saved. Writes a temporary file and renames it,
    // so a crash never leaves half a file. Returns false and logs when it cannot.
    bool save();

    // For tests: forget everything so initialize() can run again.
    void resetForTest();
}
