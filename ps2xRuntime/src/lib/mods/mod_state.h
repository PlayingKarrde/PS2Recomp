// The mod manager's state file (mods.toml): which mods are on, in what order, and their per-mod settings. A TOML file that
// the game writes (through a temporary file and a rename) and that a player may edit while the game is closed.
//
//     # comment lines explaining the file
//     version = 1
//
//     [[mod]]                      # one table per mod, in load order: the last one wins
//     id = "example.overlay-crimson"
//     enabled = true
//
//     [mod.settings]               # optional; values keep the type the mod declared (true / 12 / "red")
//     compact = true
//
// Reading is forgiving (this is our own file): unknown fields are ignored, a mod table without a valid id is skipped, a
// second table for the same id is skipped. A file that is not TOML at all is an error the manager reports before it starts
// from an empty state.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace ps2x::mods
{
    struct StateValue
    {
        enum class Kind
        {
            String,
            Bool,
            Int
        };
        std::string text; // "true" / "false", a decimal integer, or the string itself
        Kind kind = Kind::String;
    };

    struct StateMod
    {
        std::string id;
        bool enabled = false;
        std::map<std::string, StateValue> settings;
    };

    struct StateFile
    {
        std::vector<StateMod> mods; // in load order
    };

    // Returns false and sets *error (one line) when `text` is not TOML or has the wrong shape; `out` is then empty.
    bool parseStateFile(const std::string &text, StateFile &out, std::string *error);
    std::string formatStateFile(const StateFile &state);
}
