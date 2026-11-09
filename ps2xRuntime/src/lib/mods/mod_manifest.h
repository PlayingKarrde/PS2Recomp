// The mod manifest (mod.toml) and the small pure helpers around it: versions, version constraints, mod ids, safe paths
// inside a package. The format is documented in docs/formats/fprmod.md (main repository); this file is the code that
// reads it.
//
// No SDL, no game code, no third-party types in this header (toml11 stays inside mod_manifest.cpp), so the unit tests
// and any tool can use it.
//
// Every failure is reported as ONE line of plain text meant for the player ("mod.toml: 'version' must be a string like
// 1.2.3."), not as an exception: the manager shows these lines next to the mod that caused them.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ps2x::mods
{
    // The game this build runs and the version of the mod layer (the `api` a manifest must name). A mod for another game
    // or another api is refused, not loaded: its addresses and files would mean something else.
    constexpr const char *kGameSerial = "SLUS_217.02";
    constexpr int kModApi = 1;
    constexpr const char *kManifestName = "mod.toml";

    // ---- versions -----------------------------------------------------------------------------------------------

    // Semantic versions: major.minor.patch with an optional -prerelease suffix ("1.2.3", "2.0.0-beta.1"). Build metadata
    // ("+abc") is not accepted.
    struct Version
    {
        int major = 0;
        int minor = 0;
        int patch = 0;
        std::string pre; // "" for a release
    };

    // Parses "1.2.3" (and "1.2.3-beta.1"). With allowShort, "1" and "1.2" are accepted too (missing parts are 0): that is
    // what a constraint may say. nullopt for anything else (leading zeros such as "01.2.3" included, as semver forbids).
    std::optional<Version> parseVersion(const std::string &text, bool allowShort = false);
    std::string versionToString(const Version &version);
    // <0, 0, >0. A prerelease is lower than the release of the same numbers; two prereleases compare as text.
    int compareVersions(const Version &a, const Version &b);

    // A version constraint: one or more comparisons joined by commas, all of which must hold. A comparison is an operator
    // (>=, >, <=, <, =) and a version; a bare version means "=". Examples: ">=1.2", ">=1.0, <2.0", "=1.4.1", "1.4.1".
    // Returns false and sets *error (one line) when the text is not a constraint.
    bool validConstraint(const std::string &constraint, std::string *error = nullptr);
    bool satisfies(const Version &version, const std::string &constraint);

    // ---- ids and paths ------------------------------------------------------------------------------------------

    // A mod id: 3 to 64 characters, lower-case letters, digits, dots and dashes; starts with a letter, ends with a letter or
    // digit, no two separators in a row. Examples: "seth.overlay-blue", "community.move-pack-1".
    bool validModId(const std::string &id, std::string *why = nullptr);

    // A path inside a package ("theme/accent.rcss"): forward slashes, relative, no ".." or "." parts, no drive letters or
    // other colons, no control characters, no empty parts, no Windows device names (CON, NUL...), no part ending in a dot
    // or a space, at most 240 bytes. A trailing slash is allowed (a folder entry). The point is that a package can never
    // name a file outside its own folder, on any system. Sets *why (one line) when it returns false.
    bool isSafeRelativePath(const std::string &path, std::string *why = nullptr);

    // ---- the manifest -------------------------------------------------------------------------------------------

    enum class Kind
    {
        Assets,
        Data,
        Patch,
        Code,
        Theme
    };
    const char *kindName(Kind kind);
    std::optional<Kind> kindFromName(const std::string &name);

    struct Dependency
    {
        std::string id;
        std::string constraint; // "" = any version
    };

    // A per-mod setting the manager shows on the mod's page and stores in mods.toml. No kind reads them yet.
    struct SettingDecl
    {
        enum class Type
        {
            Bool,
            Choice,
            Int
        };
        std::string key;   // [a-z][a-z0-9_-]{0,31}
        std::string label; // shown to the player
        std::string help;  // optional, shown in the help line
        Type type = Type::Bool;
        // Values are kept as text whatever the type: "true" / "false", a decimal integer, or one of the choices.
        std::string defaultValue;
        std::vector<std::string> choices; // Choice only
        int minValue = 0;                 // Int only
        int maxValue = 0;
    };
    const char *settingTypeName(SettingDecl::Type type);
    // True when `value` is a legal value for the setting (a bool, an integer in range, one of the choices).
    bool settingValueValid(const SettingDecl &decl, const std::string &value);

    struct Manifest
    {
        std::string id;
        std::string name;
        std::string version;
        Version parsedVersion;
        std::vector<std::string> authors;
        std::string description;
        std::string game;
        int api = 0;
        Kind kind = Kind::Assets;
        std::vector<Dependency> depends;
        std::vector<std::string> conflicts;
        std::vector<SettingDecl> settings;
        std::string thumbnail;                 // path in the package, "" if none
        std::vector<std::string> screenshots;  // paths in the package
        std::vector<std::string> stylesheets;  // theme mods: .rcss files loaded after the game's own theme, in this order
    };

    // What a manifest must say about the game and the layer. The defaults are this build's; tests use others.
    struct ManifestRules
    {
        std::string game = kGameSerial;
        int api = kModApi;
    };

    // What could be read of a manifest that failed, so the manager can still name the mod in its list.
    struct ManifestHints
    {
        std::string id;
        std::string name;
        std::string version;
    };

    struct ManifestResult
    {
        std::optional<Manifest> manifest; // set when valid
        std::string error;                // one line, set when not
        ManifestHints hints;
    };

    // Parses and validates the text of a mod.toml. Strict: unknown fields, wrong types, bad ids, bad versions, a different
    // game or api, a path that is not a safe package path, a theme without a stylesheet... each is an error. It does not
    // look inside the package: whether the named files exist is checked by readPackage (mod_package.h).
    ManifestResult parseManifest(const std::string &tomlText, const ManifestRules &rules = ManifestRules());

    // The limits parseManifest enforces, for the format document and the tests.
    constexpr size_t kMaxManifestBytes = 64 * 1024;
    constexpr size_t kMaxNameChars = 64;
    constexpr size_t kMaxDescriptionChars = 2000;
    constexpr size_t kMaxListItems = 16;
}
