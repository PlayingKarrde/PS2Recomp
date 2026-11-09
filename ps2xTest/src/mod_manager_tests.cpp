#include "MiniTest.h"

#include "mod_manager.h"
#include "mod_manifest.h"
#include "mod_package.h"
#include "mod_state.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Tests for the mod layer (src/lib/mods): the manifest parser and its validation, version constraints, safe package paths,
// the zip reader and its limits, the state file, and the manager (scan, enable and disable, dependencies and conflicts, load
// order, settings, install, uninstall, the session and the virtual file lookup). Packages are built in memory with
// buildPackage and written to a temporary folder; nothing here touches a real mods folder.

namespace
{
    namespace fs = std::filesystem;
    using namespace ps2x::mods;

    using Files = std::vector<std::pair<std::string, std::string>>;

    fs::path tempDir(const char *name)
    {
        fs::path dir = fs::temp_directory_path() / (std::string("ps2x_mods_test_") + name);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        return dir;
    }

    std::string readAll(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    // A valid mod.toml for `id`. `extra` goes after the fixed fields; table headers ([[settings]]) must come last in it.
    std::string tomlFor(const std::string &id, const std::string &extra = std::string(), const std::string &kind = "assets",
                        const std::string &version = "1.0.0")
    {
        return "id = \"" + id + "\"\nname = \"Mod " + id + "\"\nversion = \"" + version + "\"\nauthors = [\"Tester\"]\n"
               "description = \"A test mod.\"\ngame = \"SLUS_217.02\"\napi = 1\nkind = \"" + kind + "\"\n" + extra;
    }

    // The same with one line replaced or added: for the table of bad manifests.
    std::string tomlWith(const std::string &key, const std::string &newLine)
    {
        std::istringstream in(tomlFor("test.mod"));
        std::string out, line;
        bool replaced = false;
        while (std::getline(in, line))
        {
            if (line.compare(0, key.size() + 3, key + " = ") == 0)
            {
                if (!newLine.empty())
                    out += newLine + "\n";
                replaced = true;
            }
            else
            {
                out += line + "\n";
            }
        }
        if (!replaced && !newLine.empty())
            out += newLine + "\n";
        return out;
    }

    // Writes a package to dir/fileName and returns its path.
    fs::path writeMod(const fs::path &dir, const std::string &fileName, const std::string &toml, Files extra = {})
    {
        Files files;
        files.emplace_back("mod.toml", toml);
        for (auto &f : extra)
            files.push_back(std::move(f));
        std::string error;
        auto bytes = buildPackage(files, &error);
        const fs::path path = dir / fileName;
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (bytes)
            writeBytesToFile(path, *bytes);
        return path;
    }

    // A stand-in PNG (the reader checks names and sizes, not that a file is a picture).
    std::string fakePng(size_t extra = 0)
    {
        return std::string("\x89PNG\r\n\x1a\n", 8) + std::string(extra, 'x');
    }

    bool contains(const std::string &haystack, const std::string &needle)
    {
        return haystack.find(needle) != std::string::npos;
    }

    bool oneLine(const std::string &text)
    {
        return text.find('\n') == std::string::npos && text.find('\r') == std::string::npos;
    }

    struct Env
    {
        fs::path root;
        fs::path mods;
        fs::path state;
        Manager manager;

        explicit Env(const char *name)
        {
            root = tempDir(name);
            mods = root / "mods";
            state = root / "mods.toml";
        }

        bool start(Limits limits = Limits())
        {
            Manager::Config config;
            config.modsDir = mods;
            config.statePath = state;
            config.limits = limits;
            return manager.init(config);
        }

        std::vector<std::string> keys() const
        {
            std::vector<std::string> out;
            for (const ModInfo &m : manager.list())
                out.push_back(m.key);
            return out;
        }
    };

    // Replaces the first `from` in `bytes` with `to` (same length): a zip with a name the writer would not make.
    void patchName(std::string &bytes, const std::string &from, const std::string &to)
    {
        size_t at = 0;
        while ((at = bytes.find(from, at)) != std::string::npos)
        {
            bytes.replace(at, from.size(), to);
            at += to.size();
        }
    }

    uint16_t le16(const std::string &s, size_t at) { return static_cast<uint16_t>(static_cast<uint8_t>(s[at]) | (static_cast<uint8_t>(s[at + 1]) << 8)); }

    // Offset of the central directory header of the first entry.
    size_t firstCentralHeader(const std::string &bytes)
    {
        const std::string signature("PK\x01\x02", 4);
        return bytes.find(signature);
    }
}

void register_mod_manager_tests()
{
    MiniTest::Case("ModVersions", [](TestCase &tc)
    {
        tc.Run("versions parse strictly and compare as semver", [](TestCase &t)
        {
            t.IsTrue(parseVersion("1.2.3").has_value(), "1.2.3");
            t.IsTrue(parseVersion("0.0.0").has_value(), "0.0.0");
            t.IsTrue(parseVersion("2.0.0-beta.1").has_value(), "prerelease");
            for (const char *bad : {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.x", "v1.2.3", "1.2.3+build", "1.2.3-", "1.2.3-a..b", " 1.2.3", "1.2.3 ", "-1.2.3", "1.2.3-\xC3\xA4"})
                t.IsFalse(parseVersion(bad).has_value(), std::string("rejected: '") + bad + "'");
            t.IsTrue(parseVersion("1.2", true).has_value(), "a constraint may say 1.2");
            t.Equals(versionToString(*parseVersion("1.2.3-rc.1")), std::string("1.2.3-rc.1"), "text round trip");
            auto v = [](const char *s) { return *parseVersion(s); };
            t.IsTrue(compareVersions(v("1.2.3"), v("1.2.4")) < 0, "patch");
            t.IsTrue(compareVersions(v("1.10.0"), v("1.9.0")) > 0, "numbers, not text");
            t.IsTrue(compareVersions(v("2.0.0"), v("1.99.99")) > 0, "major");
            t.IsTrue(compareVersions(v("1.0.0"), v("1.0.0")) == 0, "equal");
            t.IsTrue(compareVersions(v("1.0.0-beta"), v("1.0.0")) < 0, "a prerelease is below its release");
            t.IsTrue(compareVersions(v("1.0.0-alpha.1"), v("1.0.0-alpha.2")) < 0, "prerelease numbers");
            t.IsTrue(compareVersions(v("1.0.0-alpha.2"), v("1.0.0-alpha.10")) < 0, "numeric identifiers compare as numbers");
            t.IsTrue(compareVersions(v("1.0.0-1"), v("1.0.0-alpha")) < 0, "numbers are below words");
            t.IsTrue(compareVersions(v("1.0.0-alpha"), v("1.0.0-alpha.1")) < 0, "a shorter list is lower");
        });

        tc.Run("constraints", [](TestCase &t)
        {
            auto ok = [&](const char *version, const char *constraint) { return satisfies(*parseVersion(version), constraint); };
            t.IsTrue(ok("1.2.0", ">=1.2"), ">= with a short version");
            t.IsTrue(ok("1.2.5", ">=1.2"), ">= above");
            t.IsFalse(ok("1.1.9", ">=1.2"), ">= below");
            t.IsTrue(ok("1.5.0", ">=1.0, <2.0"), "a range");
            t.IsFalse(ok("2.0.0", ">=1.0, <2.0"), "the top of a range is excluded");
            t.IsTrue(ok("1.2.3", "=1.2.3"), "exact");
            t.IsFalse(ok("1.2.4", "=1.2.3"), "exact differs");
            t.IsTrue(ok("1.2.9", "=1.2"), "=1.2 accepts 1.2.x");
            t.IsFalse(ok("1.3.0", "=1.2"), "=1.2 refuses 1.3");
            t.IsTrue(ok("1.9.9", "=1"), "=1 accepts 1.x");
            t.IsTrue(ok("1.2.3", "1.2.3"), "a bare version means =");
            t.IsTrue(ok("1.2.4", ">1.2.3"), ">");
            t.IsFalse(ok("1.2.3", ">1.2.3"), "> excludes equal");
            t.IsTrue(ok("1.2.3", "<=1.2.3"), "<=");
            t.IsFalse(ok("1.0.0-beta", ">=1.0"), "a prerelease does not satisfy >=1.0");
            std::string error;
            for (const char *bad : {"", ">=", ">=abc", ">=1.2,", "~1.2", ">=1.2.3.4", ">= 1 2"})
                t.IsFalse(validConstraint(bad, &error), std::string("rejected: '") + bad + "'");
            t.IsTrue(validConstraint(">=1.0, <2.0", &error), "valid range");
        });
    });

    MiniTest::Case("ModPaths", [](TestCase &tc)
    {
        tc.Run("ids", [](TestCase &t)
        {
            for (const char *good : {"abc", "seth.overlay-blue", "community.move-pack-1", "a1b", "x.y.z"})
                t.IsTrue(validModId(good), std::string("good id ") + good);
            std::string why;
            for (const char *bad : {"", "ab", "Abc", "1abc", "-abc", "abc-", "abc.", "a..b", "a--b", "a b", "a_b", "abc/def", "\xC3\xA4.bc"})
                t.IsFalse(validModId(bad, &why), std::string("bad id '") + bad + "'");
            t.IsFalse(validModId(std::string(65, 'a')), "65 characters is too long");
            t.IsTrue(validModId(std::string(64, 'a')), "64 characters is fine");
        });

        tc.Run("package paths: safe and unsafe", [](TestCase &t)
        {
            for (const char *good : {"a.txt", "theme/accent.rcss", "assets/dir/file.png", "folder/", "a-b_c/d e.txt", "\xC3\xBCn\xC3\xAF/ok.txt"})
                t.IsTrue(isSafeRelativePath(good), std::string("safe: ") + good);
            std::string why;
            const std::vector<std::pair<std::string, std::string>> bad = {
                {"", "empty"}, {"/etc/passwd", "absolute"}, {"../x", ".."}, {"a/../b", ".."}, {"a/..", ".."}, {"a/./b", "."}, {"./a", "."},
                {"C:/x", "colon"}, {"C:\\x", "backslash"}, {"a\\b", "backslash"}, {"a//b", "empty part"}, {"con", "device"}, {"dir/NUL.txt", "device"},
                {"com1", "device"}, {"a.", "dot"}, {"a /b", "space"}, {std::string("x\x01y"), "control"}, {"a:b", "colon"}, {"a?b", "Windows"},
                {"a*b", "Windows"}, {"a|b", "Windows"}, {"a<b", "Windows"}, {std::string(241, 'a'), "240"}, {"/", "absolute"}};
            for (const auto &entry : bad)
            {
                why.clear();
                t.IsFalse(isSafeRelativePath(entry.first, &why), "unsafe: '" + entry.first + "'");
                t.IsTrue(contains(why, entry.second) || why.empty(), "reason for '" + entry.first + "' mentions " + entry.second + ": " + why);
                t.IsFalse(why.empty(), "a reason is given for '" + entry.first + "'");
            }
            t.IsTrue(isSafeRelativePath(std::string(240, 'a')), "240 bytes is fine");
        });
    });

    MiniTest::Case("ModManifest", [](TestCase &tc)
    {
        tc.Run("a minimal manifest parses", [](TestCase &t)
        {
            const ManifestResult r = parseManifest(tomlFor("seth.overlay-blue"));
            t.IsTrue(r.manifest.has_value(), "valid: " + r.error);
            if (!r.manifest)
                return;
            t.Equals(r.manifest->id, std::string("seth.overlay-blue"), "id");
            t.Equals(r.manifest->name, std::string("Mod seth.overlay-blue"), "name");
            t.Equals(r.manifest->version, std::string("1.0.0"), "version");
            t.Equals(r.manifest->authors.size(), static_cast<size_t>(1), "authors");
            t.Equals(r.manifest->description, std::string("A test mod."), "description");
            t.Equals(r.manifest->game, std::string("SLUS_217.02"), "game");
            t.Equals(r.manifest->api, 1, "api");
            t.IsTrue(r.manifest->kind == Kind::Assets, "kind");
            t.IsTrue(r.manifest->depends.empty() && r.manifest->conflicts.empty() && r.manifest->settings.empty(), "no extras");
        });

        tc.Run("a full manifest parses with every optional part", [](TestCase &t)
        {
            const std::string toml = tomlFor("full.mod",
                "depends = [{ id = \"base.mod\", version = \">=1.2\" }, { id = \"other.mod\" }]\n"
                "conflicts = [\"bad.mod\"]\n"
                "thumbnail = \"thumb.png\"\n"
                "screenshots = [\"shots/one.png\", \"shots/two.PNG\"]\n"
                "stylesheets = [\"theme/a.rcss\", \"theme/b.rcss\"]\n"
                "\n[[settings]]\nkey = \"compact\"\nlabel = \"Compact\"\ntype = \"bool\"\ndefault = true\nhelp = \"Smaller rows\"\n"
                "\n[[settings]]\nkey = \"colour\"\nlabel = \"Colour\"\ntype = \"choice\"\nchoices = [\"red\", \"blue\"]\ndefault = \"blue\"\n"
                "\n[[settings]]\nkey = \"size\"\nlabel = \"Size\"\ntype = \"int\"\nmin = 1\nmax = 9\ndefault = 4\n",
                "theme");
            const ManifestResult r = parseManifest(toml);
            t.IsTrue(r.manifest.has_value(), "valid: " + r.error);
            if (!r.manifest)
                return;
            const Manifest &m = *r.manifest;
            t.IsTrue(m.kind == Kind::Theme, "theme");
            t.Equals(m.depends.size(), static_cast<size_t>(2), "two dependencies");
            t.Equals(m.depends[0].constraint, std::string(">=1.2"), "constraint kept");
            t.Equals(m.depends[1].constraint, std::string(), "no constraint");
            t.Equals(m.conflicts.size(), static_cast<size_t>(1), "one conflict");
            t.Equals(m.thumbnail, std::string("thumb.png"), "thumbnail");
            t.Equals(m.screenshots.size(), static_cast<size_t>(2), "screenshots");
            t.Equals(m.stylesheets.size(), static_cast<size_t>(2), "stylesheets");
            t.Equals(m.settings.size(), static_cast<size_t>(3), "settings");
            t.Equals(m.settings[0].defaultValue, std::string("true"), "bool default");
            t.Equals(m.settings[0].help, std::string("Smaller rows"), "help");
            t.IsTrue(m.settings[1].type == SettingDecl::Type::Choice && m.settings[1].choices.size() == 2, "choice");
            t.Equals(m.settings[2].defaultValue, std::string("4"), "int default");
            t.IsTrue(m.settings[2].minValue == 1 && m.settings[2].maxValue == 9, "int range");
            t.IsTrue(settingValueValid(m.settings[0], "false") && !settingValueValid(m.settings[0], "yes"), "bool values");
            t.IsTrue(settingValueValid(m.settings[1], "red") && !settingValueValid(m.settings[1], "green"), "choice values");
            t.IsTrue(settingValueValid(m.settings[2], "9") && !settingValueValid(m.settings[2], "10") && !settingValueValid(m.settings[2], "x"), "int values");
        });

        tc.Run("every kind of bad manifest gives one clear line", [](TestCase &t)
        {
            struct Bad
            {
                std::string label;
                std::string toml;
                std::string expect;
            };
            const std::string longName(65, 'n');
            const std::string longDescription(2001, 'd');
            const std::string settingsHead = "\n[[settings]]\n";
            const std::vector<Bad> bad = {
                {"id missing", tomlWith("id", ""), "'id' is missing"},
                {"id upper case", tomlWith("id", "id = \"Seth.Mod\""), "not valid"},
                {"id too short", tomlWith("id", "id = \"ab\""), "3 to 64"},
                {"id starts with digit", tomlWith("id", "id = \"1abc\""), "starts with"},
                {"id has spaces", tomlWith("id", "id = \"my mod\""), "lower-case letters"},
                {"id double dot", tomlWith("id", "id = \"a..b\""), "two dots"},
                {"id not a string", tomlWith("id", "id = 5"), "'id' must be a string"},
                {"name missing", tomlWith("name", ""), "'name' is missing"},
                {"name empty", tomlWith("name", "name = \"  \""), "'name' is empty"},
                {"name too long", tomlWith("name", "name = \"" + longName + "\""), "longer than 64"},
                {"version missing", tomlWith("version", ""), "'version' is missing"},
                {"version short", tomlWith("version", "version = \"1.0\""), "not a version"},
                {"version leading zero", tomlWith("version", "version = \"01.0.0\""), "not a version"},
                {"version number", tomlWith("version", "version = 1"), "'version' must be a string"},
                {"authors missing", tomlWith("authors", ""), "'authors' is missing"},
                {"authors empty", tomlWith("authors", "authors = []"), "at least 1"},
                {"authors wrong type", tomlWith("authors", "authors = [1]"), "list of strings"},
                {"authors a string", tomlWith("authors", "authors = \"me\""), "list of strings"},
                {"description wrong type", tomlWith("description", "description = 5"), "'description' must be a string"},
                {"description too long", tomlWith("description", "description = \"" + longDescription + "\""), "longer than 2000"},
                {"game missing", tomlWith("game", ""), "'game' is missing"},
                {"wrong game", tomlWith("game", "game = \"SLUS_000.00\""), "not 'SLUS_217.02'"},
                {"api missing", tomlWith("api", ""), "'api' is missing"},
                {"api newer", tomlWith("api", "api = 2"), "mod layer version 2"},
                {"api text", tomlWith("api", "api = \"1\""), "'api' must be a whole number"},
                {"kind missing", tomlWith("kind", ""), "'kind' is missing"},
                {"kind unknown", tomlWith("kind", "kind = \"skin\""), "not one of assets, data, patch, code, theme"},
                {"unknown field", tomlWith("colour", "colour = \"red\""), "unknown field 'colour'"},
                {"theme without stylesheets", tomlWith("kind", "kind = \"theme\""), "needs 'stylesheets'"},
                {"stylesheets on a data mod", tomlWith("stylesheets", "stylesheets = [\"a.rcss\"]"), "only for theme"},
                {"stylesheet wrong extension", tomlWith("kind", "kind = \"theme\"") + "stylesheets = [\"a.css\"]\n", "must end in .rcss"},
                {"stylesheet path up", tomlWith("kind", "kind = \"theme\"") + "stylesheets = [\"../a.rcss\"]\n", "'..'"},
                {"stylesheet absolute", tomlWith("kind", "kind = \"theme\"") + "stylesheets = [\"/a.rcss\"]\n", "absolute"},
                {"thumbnail not png", tomlWith("thumbnail", "thumbnail = \"t.jpg\""), "must end in .png"},
                {"thumbnail number", tomlWith("thumbnail", "thumbnail = 5"), "'thumbnail' must be a path"},
                {"too many screenshots", tomlWith("screenshots", "screenshots = [\"1.png\",\"2.png\",\"3.png\",\"4.png\",\"5.png\",\"6.png\",\"7.png\",\"8.png\",\"9.png\"]"), "more than 8"},
                {"depends not a list", tomlWith("depends", "depends = \"x\""), "'depends' must be a list"},
                {"depends entry not a table", tomlWith("depends", "depends = [\"x.y\"]"), "each entry of 'depends'"},
                {"depends without id", tomlWith("depends", "depends = [{ version = \">=1.0\" }]"), "needs an 'id'"},
                {"depends bad id", tomlWith("depends", "depends = [{ id = \"Bad Id\" }]"), "not valid"},
                {"depends self", tomlWith("depends", "depends = [{ id = \"test.mod\" }]"), "depend on itself"},
                {"depends twice", tomlWith("depends", "depends = [{ id = \"a.b\" }, { id = \"a.b\" }]"), "twice"},
                {"depends bad constraint", tomlWith("depends", "depends = [{ id = \"a.b\", version = \"~1\" }]"), "not a constraint"},
                {"depends unknown key", tomlWith("depends", "depends = [{ id = \"a.b\", min = 1 }]"), "unknown field 'min'"},
                {"conflicts bad id", tomlWith("conflicts", "conflicts = [\"No Way\"]"), "not valid"},
                {"conflicts self", tomlWith("conflicts", "conflicts = [\"test.mod\"]"), "conflict with itself"},
                {"conflicts twice", tomlWith("conflicts", "conflicts = [\"a.b\", \"a.b\"]"), "twice"},
                {"depends and conflicts", tomlWith("conflicts", "conflicts = [\"a.b\"]") + "depends = [{ id = \"a.b\" }]\n", "both a dependency and a conflict"},
                {"setting without key", tomlFor("test.mod") + settingsHead + "label = \"L\"\ntype = \"bool\"\ndefault = true\n", "needs a 'key'"},
                {"setting bad key", tomlFor("test.mod") + settingsHead + "key = \"Bad Key\"\nlabel = \"L\"\ntype = \"bool\"\ndefault = true\n", "not valid"},
                {"setting without label", tomlFor("test.mod") + settingsHead + "key = \"k\"\ntype = \"bool\"\ndefault = true\n", "needs a 'label'"},
                {"setting without type", tomlFor("test.mod") + settingsHead + "key = \"k\"\nlabel = \"L\"\ndefault = true\n", "needs a 'type'"},
                {"setting unknown type", tomlFor("test.mod") + settingsHead + "key = \"k\"\nlabel = \"L\"\ntype = \"float\"\ndefault = 1\n", "not bool, choice or int"},
                {"setting without default", tomlFor("test.mod") + settingsHead + "key = \"k\"\nlabel = \"L\"\ntype = \"bool\"\n", "needs a 'default'"},
                {"bool default wrong", tomlFor("test.mod") + settingsHead + "key = \"k\"\nlabel = \"L\"\ntype = \"bool\"\ndefault = 1\n", "true or false"},
                {"choice default not listed", tomlFor("test.mod") + settingsHead + "key = \"k\"\nlabel = \"L\"\ntype = \"choice\"\nchoices = [\"a\",\"b\"]\ndefault = \"c\"\n", "one of the choices"},
                {"choice with one entry", tomlFor("test.mod") + settingsHead + "key = \"k\"\nlabel = \"L\"\ntype = \"choice\"\nchoices = [\"a\"]\ndefault = \"a\"\n", "2 to 16"},
                {"choice repeated", tomlFor("test.mod") + settingsHead + "key = \"k\"\nlabel = \"L\"\ntype = \"choice\"\nchoices = [\"a\",\"a\"]\ndefault = \"a\"\n", "listed twice"},
                {"int without range", tomlFor("test.mod") + settingsHead + "key = \"k\"\nlabel = \"L\"\ntype = \"int\"\ndefault = 1\n", "'min' and 'max'"},
                {"int default out of range", tomlFor("test.mod") + settingsHead + "key = \"k\"\nlabel = \"L\"\ntype = \"int\"\nmin = 1\nmax = 5\ndefault = 6\n", "between min and max"},
                {"int range backwards", tomlFor("test.mod") + settingsHead + "key = \"k\"\nlabel = \"L\"\ntype = \"int\"\nmin = 5\nmax = 1\ndefault = 3\n", "min < max"},
                {"setting repeated", tomlFor("test.mod") + settingsHead + "key = \"k\"\nlabel = \"L\"\ntype = \"bool\"\ndefault = true\n" + settingsHead + "key = \"k\"\nlabel = \"M\"\ntype = \"bool\"\ndefault = true\n", "declared twice"},
                {"setting unknown field", tomlFor("test.mod") + settingsHead + "key = \"k\"\nlabel = \"L\"\ntype = \"bool\"\ndefault = true\ncolour = 1\n", "unknown field 'colour'"},
                {"bool with choices", tomlFor("test.mod") + settingsHead + "key = \"k\"\nlabel = \"L\"\ntype = \"bool\"\ndefault = true\nchoices = [\"a\",\"b\"]\n", "no choices"},
                {"not TOML", "id = = 3\n", "not valid TOML"},
                {"not TOML, with a line", "id = \"a.b\"\nname = \n", "(line "},
                {"too large", std::string(kMaxManifestBytes + 1, '#'), "larger than 64 KiB"},
                {"control character in a name", tomlWith("name", "name = \"a\\u0007b\""), "control character"},
            };
            for (const Bad &b : bad)
            {
                const ManifestResult r = parseManifest(b.toml);
                t.IsFalse(r.manifest.has_value(), b.label + ": must be refused");
                t.IsTrue(contains(r.error, b.expect), b.label + ": error '" + r.error + "' should mention '" + b.expect + "'");
                t.IsTrue(oneLine(r.error) && !r.error.empty(), b.label + ": one line");
            }
        });

        tc.Run("hints survive a refused manifest, and the rules can name another game", [](TestCase &t)
        {
            const ManifestResult wrong = parseManifest(tomlWith("game", "game = \"SLUS_000.00\""));
            t.IsFalse(wrong.manifest.has_value(), "wrong game");
            t.Equals(wrong.hints.id, std::string("test.mod"), "id hint");
            t.Equals(wrong.hints.name, std::string("Mod test.mod"), "name hint");
            t.Equals(wrong.hints.version, std::string("1.0.0"), "version hint");
            ManifestRules rules;
            rules.game = "SLUS_000.00";
            t.IsTrue(parseManifest(tomlWith("game", "game = \"SLUS_000.00\""), rules).manifest.has_value(), "accepted under other rules");
            t.IsFalse(parseManifest(tomlFor("test.mod"), rules).manifest.has_value(), "refused under other rules");
        });
    });

    MiniTest::Case("ModPackage", [](TestCase &tc)
    {
        tc.Run("a good package reads, with its file list", [](TestCase &t)
        {
            const fs::path dir = tempDir("pkg_good");
            const fs::path file = writeMod(dir, "a.fprmod", tomlFor("good.mod", "thumbnail = \"t.png\"\n"), {{"t.png", fakePng()}, {"assets/x.txt", "hello"}, {"assets/", ""}});
            const PackageResult r = readPackage(file);
            t.IsTrue(r.package.has_value(), "valid: " + r.error);
            if (!r.package)
                return;
            t.Equals(r.package->manifest.id, std::string("good.mod"), "id");
            t.IsTrue(r.package->find("assets/x.txt") != nullptr, "file listed");
            t.Equals(r.package->find("assets/x.txt")->size, static_cast<uint64_t>(5), "size");
            t.IsTrue(r.package->find("assets/") != nullptr && r.package->find("assets/")->isDirectory, "folder entry");
            t.IsTrue(r.package->find("nope") == nullptr, "missing name");
            std::string error;
            auto bytes = readPackageEntry(file, "assets/x.txt", 100, &error);
            t.IsTrue(bytes.has_value() && std::string(bytes->begin(), bytes->end()) == "hello", "entry content: " + error);
            t.IsFalse(readPackageEntry(file, "assets/x.txt", 4, &error).has_value(), "over the byte limit");
            t.IsTrue(contains(error, "larger than"), "limit message: " + error);
            t.IsFalse(readPackageEntry(file, "missing.txt", 100, &error).has_value(), "missing entry");
            t.IsFalse(readPackageEntry(dir / "none.fprmod", "x", 100, &error).has_value(), "missing file");
        });

        tc.Run("not a zip, empty, and truncated files are refused", [](TestCase &t)
        {
            const fs::path dir = tempDir("pkg_notzip");
            writeBytesToFile(dir / "text.fprmod", "this is not a zip file at all, just text\n");
            writeBytesToFile(dir / "empty.fprmod", "");
            const std::string good = *buildPackage({{"mod.toml", tomlFor("good.mod")}});
            writeBytesToFile(dir / "cut.fprmod", good.substr(0, good.size() / 2));
            for (const char *name : {"text.fprmod", "empty.fprmod", "cut.fprmod"})
            {
                const PackageResult r = readPackage(dir / name);
                t.IsFalse(r.package.has_value(), std::string(name) + " refused");
                t.IsTrue(contains(r.error, "not a valid zip"), std::string(name) + ": " + r.error);
                t.IsTrue(oneLine(r.error), std::string(name) + " one line");
            }
            t.IsFalse(readPackage(dir / "absent.fprmod").package.has_value(), "absent file");
        });

        tc.Run("mod.toml: missing, in a folder, or bad", [](TestCase &t)
        {
            const fs::path dir = tempDir("pkg_manifest");
            writeBytesToFile(dir / "none.fprmod", *buildPackage({{"readme.txt", "x"}}));
            writeBytesToFile(dir / "folder.fprmod", *buildPackage({{"MyMod/mod.toml", tomlFor("good.mod")}}));
            writeBytesToFile(dir / "bad.fprmod", *buildPackage({{"mod.toml", "id = 5\n"}}));
            const PackageResult none = readPackage(dir / "none.fprmod");
            t.IsTrue(contains(none.error, "mod.toml is missing"), none.error);
            const PackageResult folder = readPackage(dir / "folder.fprmod");
            t.IsTrue(contains(folder.error, "inside a folder"), folder.error);
            const PackageResult bad = readPackage(dir / "bad.fprmod");
            t.IsTrue(contains(bad.error, "'id' must be a string") || contains(bad.error, "'api'"), bad.error);
        });

        tc.Run("unsafe zip paths are refused", [](TestCase &t)
        {
            const fs::path dir = tempDir("pkg_unsafe");
            const std::vector<std::pair<std::string, std::string>> names = {
                {"../evil.txt", "'..'"}, {"a/../../evil.txt", "'..'"}, {"C:/evil.txt", "colon"}, {"a\\b.txt", "backslash"}, {"a/./b.txt", "'.'"},
                {"a//b.txt", "empty part"}, {"nul.txt", "device name"}, {"dir/evil.", "dot or a space"}};
            for (const auto &entry : names)
            {
                const fs::path file = writeMod(dir, "x.fprmod", tomlFor("good.mod"), {{entry.first, "data"}});
                const PackageResult r = readPackage(file);
                t.IsFalse(r.package.has_value(), "refused: " + entry.first);
                t.IsTrue(contains(r.error, "unsafe path") && contains(r.error, entry.second), entry.first + ": " + r.error);
            }
            // An absolute name: the zip writer will not make one, so make it with a name of the same length and rename it in the bytes.
            std::string bytes = *buildPackage({{"mod.toml", tomlFor("good.mod")}, {"Xabs.txt", "data"}});
            patchName(bytes, "Xabs.txt", "/abs.txt");
            writeBytesToFile(dir / "abs.fprmod", bytes);
            const PackageResult absolute = readPackage(dir / "abs.fprmod");
            t.IsFalse(absolute.package.has_value(), "absolute path refused");
            t.IsTrue(contains(absolute.error, "absolute"), absolute.error);
        });

        tc.Run("names that differ only in capitals are refused", [](TestCase &t)
        {
            const fs::path dir = tempDir("pkg_case");
            const fs::path file = writeMod(dir, "x.fprmod", tomlFor("good.mod"), {{"assets/A.txt", "1"}, {"assets/a.txt", "2"}});
            const PackageResult r = readPackage(file);
            t.IsFalse(r.package.has_value(), "refused");
            t.IsTrue(contains(r.error, "twice"), r.error);
        });

        tc.Run("files the manifest names must be in the package", [](TestCase &t)
        {
            const fs::path dir = tempDir("pkg_missing");
            t.IsTrue(contains(readPackage(writeMod(dir, "a.fprmod", tomlFor("good.mod", "thumbnail = \"t.png\"\n"))).error, "thumbnail 't.png', which is not in the package"), "thumbnail");
            t.IsTrue(contains(readPackage(writeMod(dir, "b.fprmod", tomlFor("good.mod", "screenshots = [\"s.png\"]\n"))).error, "screenshot 's.png'"), "screenshot");
            t.IsTrue(contains(readPackage(writeMod(dir, "c.fprmod", tomlFor("good.mod", "stylesheets = [\"theme/a.rcss\"]\n", "theme"))).error, "stylesheet 'theme/a.rcss'"), "stylesheet");
            // A folder of that name is not the file.
            t.IsFalse(readPackage(writeMod(dir, "d.fprmod", tomlFor("good.mod", "thumbnail = \"t.png\"\n"), {{"t.png/", ""}})).package.has_value(), "a folder is not a file");
        });

        tc.Run("size limits", [](TestCase &t)
        {
            const fs::path dir = tempDir("pkg_limits");
            const fs::path file = writeMod(dir, "a.fprmod", tomlFor("good.mod", "thumbnail = \"t.png\"\n"), {{"t.png", fakePng(5000)}, {"assets/a.bin", std::string(3000, 'a')}, {"assets/b.bin", std::string(3000, 'b')}});
            t.IsTrue(readPackage(file).package.has_value(), "fine under the default limits");
            Limits limits;
            limits.maxEntries = 3;
            t.IsTrue(contains(readPackage(file, ManifestRules(), limits).error, "entries"), "entry count: " + readPackage(file, ManifestRules(), limits).error);
            limits = Limits();
            limits.maxEntryBytes = 2000;
            t.IsTrue(contains(readPackage(file, ManifestRules(), limits).error, "unpacked, more than the limit"), "one entry: " + readPackage(file, ManifestRules(), limits).error);
            limits = Limits();
            limits.maxTotalBytes = 8000;
            t.IsTrue(contains(readPackage(file, ManifestRules(), limits).error, "add up to more than"), "total: " + readPackage(file, ManifestRules(), limits).error);
            limits = Limits();
            limits.maxFileBytes = 100;
            t.IsTrue(contains(readPackage(file, ManifestRules(), limits).error, "more than the limit"), "file: " + readPackage(file, ManifestRules(), limits).error);
            limits = Limits();
            limits.maxImageBytes = 1000;
            t.IsTrue(contains(readPackage(file, ManifestRules(), limits).error, "limit of"), "picture: " + readPackage(file, ManifestRules(), limits).error);
        });

        tc.Run("encrypted entries, links and damaged data are refused", [](TestCase &t)
        {
            const fs::path dir = tempDir("pkg_odd");
            const std::string good = *buildPackage({{"mod.toml", tomlFor("good.mod")}, {"assets/x.txt", "hello hello hello hello"}});

            // The central directory header of the second entry: flag bit 0 = encrypted.
            std::string encrypted = good;
            size_t header = firstCentralHeader(encrypted);
            header = encrypted.find(std::string("PK\x01\x02", 4), header + 4);
            t.IsTrue(header != std::string::npos, "second central header found");
            encrypted[header + 8] = static_cast<char>(encrypted[header + 8] | 1);
            writeBytesToFile(dir / "enc.fprmod", encrypted);
            const PackageResult enc = readPackage(dir / "enc.fprmod");
            t.IsFalse(enc.package.has_value(), "encrypted refused");
            t.IsTrue(contains(enc.error, "encrypted"), enc.error);

            // Made on Unix (host 3) with the link file type in the attributes (mode 0xA1FF in the high half of the external attributes).
            std::string link = good;
            header = firstCentralHeader(link);
            header = link.find(std::string("PK\x01\x02", 4), header + 4);
            link[header + 5] = 3;
            link[header + 38] = 0;
            link[header + 39] = 0;
            link[header + 40] = static_cast<char>(0xFF);
            link[header + 41] = static_cast<char>(0xA1);
            writeBytesToFile(dir / "link.fprmod", link);
            const PackageResult linked = readPackage(dir / "link.fprmod");
            t.IsFalse(linked.package.has_value(), "link refused");
            t.IsTrue(contains(linked.error, "link"), linked.error);

            // Damaged data: the checksum fails when the entry is read.
            std::string damaged = good;
            const size_t nameLength = le16(damaged, 26);
            const size_t extraLength = le16(damaged, 28);
            const size_t firstData = 30 + nameLength + extraLength; // the data of the first entry, mod.toml
            damaged[firstData + 3] = static_cast<char>(damaged[firstData + 3] ^ 0x55);
            writeBytesToFile(dir / "bad.fprmod", damaged);
            const PackageResult broken = readPackage(dir / "bad.fprmod");
            t.IsFalse(broken.package.has_value(), "damaged mod.toml refused");
            t.IsTrue(contains(broken.error, "cannot be unpacked") || contains(broken.error, "not valid"), broken.error);
        });
    });

    MiniTest::Case("ModState", [](TestCase &tc)
    {
        tc.Run("a state file round trips, strings included", [](TestCase &t)
        {
            StateFile in;
            StateMod a;
            a.id = "first.mod";
            a.enabled = true;
            a.settings["compact"] = {"true", StateValue::Kind::Bool};
            a.settings["size"] = {"-12", StateValue::Kind::Int};
            a.settings["name"] = {"quote \" back \\ slash\nline\ttab \xC3\xA9", StateValue::Kind::String};
            a.settings["odd key"] = {"x", StateValue::Kind::String};
            StateMod b;
            b.id = "second.mod";
            in.mods = {a, b};
            const std::string text = formatStateFile(in);
            StateFile out;
            std::string error;
            t.IsTrue(parseStateFile(text, out, &error), "parses: " + error);
            t.Equals(out.mods.size(), static_cast<size_t>(2), "two mods");
            t.Equals(out.mods[0].id, std::string("first.mod"), "order kept");
            t.IsTrue(out.mods[0].enabled && !out.mods[1].enabled, "enabled flags");
            t.Equals(out.mods[0].settings.size(), static_cast<size_t>(4), "settings");
            t.Equals(out.mods[0].settings["compact"].text, std::string("true"), "bool");
            t.IsTrue(out.mods[0].settings["compact"].kind == StateValue::Kind::Bool, "bool kind");
            t.Equals(out.mods[0].settings["size"].text, std::string("-12"), "int");
            t.IsTrue(out.mods[0].settings["size"].kind == StateValue::Kind::Int, "int kind");
            t.Equals(out.mods[0].settings["name"].text, a.settings["name"].text, "string with escapes");
            t.Equals(out.mods[0].settings["odd key"].text, std::string("x"), "a key that needs quotes");
            t.Equals(formatStateFile(out), text, "formatting is stable");
        });

        tc.Run("reading is forgiving about odd entries and strict about broken TOML", [](TestCase &t)
        {
            StateFile out;
            std::string error;
            t.IsTrue(parseStateFile("version = 1\nfuture = 2\n[[mod]]\nid = \"a.mod\"\nenabled = true\nfuture = 1\n[[mod]]\nenabled = true\n"
                                    "[[mod]]\nid = \"Bad Id\"\n[[mod]]\nid = \"a.mod\"\nenabled = false\n[[mod]]\nid = \"b.mod\"\nenabled = \"yes\"\n",
                                    out, &error), "tolerant: " + error);
            t.Equals(out.mods.size(), static_cast<size_t>(2), "bad and repeated entries skipped");
            t.IsTrue(out.mods[0].id == "a.mod" && out.mods[0].enabled, "first a.mod wins");
            t.IsTrue(out.mods[1].id == "b.mod" && !out.mods[1].enabled, "a non-boolean enabled is off");
            t.IsTrue(parseStateFile("", out, &error) && out.mods.empty(), "an empty file is an empty state");
            t.IsFalse(parseStateFile("[[mod\nid = ", out, &error), "broken TOML");
            t.IsTrue(contains(error, "not valid TOML") && oneLine(error), "message: " + error);
            t.IsFalse(parseStateFile("mod = 5\n", out, &error), "mod must be a list");
        });
    });

    MiniTest::Case("ModManager", [](TestCase &tc)
    {
        tc.Run("an empty folder: created, nothing listed, no restart needed", [](TestCase &t)
        {
            Env env("empty");
            t.IsFalse(fs::exists(env.mods), "folder not there yet");
            t.IsTrue(env.start(), "starts");
            t.IsTrue(fs::is_directory(env.mods), "folder created");
            t.IsTrue(env.manager.ready(), "ready");
            t.IsTrue(env.manager.list().empty(), "no mods");
            t.IsFalse(env.manager.restartNeeded(), "nothing to restart for");
            t.IsFalse(fs::exists(env.state), "no state file is written until something changes");
            t.IsFalse(env.manager.open("assets/x").has_value(), "no files");
            t.IsTrue(env.manager.sessionThemeStylesheets().empty(), "no themes");
        });

        tc.Run("the folder is scanned: good mods first, bad ones with their reasons", [](TestCase &t)
        {
            Env env("scan");
            writeMod(env.mods, "b-second.fprmod", tomlFor("second.mod"));
            writeMod(env.mods, "a-first.fprmod", tomlFor("first.mod"));
            writeMod(env.mods, "dup.fprmod", tomlFor("first.mod"));                                    // same id as a-first
            writeMod(env.mods, "othergame.fprmod", tomlWith("game", "game = \"SLUS_000.00\""));
            writeBytesToFile(env.mods / "notzip.fprmod", "text");
            writeBytesToFile(env.mods / "ignored.txt", "not a mod");
            fs::create_directories(env.mods / "subfolder");
            writeMod(env.mods / "subfolder", "inside.fprmod", tomlFor("inside.mod"));                   // not scanned
            writeMod(env.mods, "CAPS.FPRMOD", tomlFor("caps.mod"));                                      // the extension ignores capitals
            t.IsTrue(env.start(), "starts");
            const auto list = env.manager.list();
            std::vector<std::string> keys = env.keys();
            t.Equals(list.size(), static_cast<size_t>(6), "six packages");
            if (list.size() != 6)
                return;
            t.IsTrue(list[0].valid && list[1].valid && list[2].valid, "three good mods first");
            t.Equals(list[0].id, std::string("caps.mod"), "by file name, bytes: CAPS.FPRMOD sorts before the lower case names");
            t.Equals(list[1].id, std::string("first.mod"), "a-first");
            t.Equals(list[2].id, std::string("second.mod"), "b-second");
            t.IsTrue(!list[3].valid && !list[4].valid && !list[5].valid, "three bad ones last");
            for (const ModInfo &m : list)
            {
                if (m.valid)
                    t.IsTrue(m.error.empty() && !m.enabled && !m.active, "good and off: " + m.key);
                else
                    t.IsTrue(!m.error.empty() && contains(m.key, "file:"), "bad one has an error: " + m.file);
            }
            std::string all;
            for (const ModInfo &m : list)
                all += m.file + "=" + m.error + "|";
            t.IsTrue(contains(all, "dup.fprmod=Another file already provides the mod 'first.mod'"), "duplicate id: " + all);
            t.IsTrue(contains(all, "othergame.fprmod=This mod is for the game 'SLUS_000.00'"), "other game: " + all);
            t.IsTrue(contains(all, "notzip.fprmod=This is not a valid zip file"), "not a zip: " + all);
            for (const ModInfo &m : list)
                if (m.file == "othergame.fprmod")
                    t.Equals(m.name, std::string("Mod test.mod"), "a refused manifest still names the mod");
            t.IsFalse(env.manager.setEnabled("file:notzip.fprmod", true).ok, "a bad mod cannot be turned on");
            t.IsFalse(env.manager.setEnabled("nothing.here", true).ok, "an unknown mod cannot be turned on");
        });

        tc.Run("turning mods on and off is kept in the state file", [](TestCase &t)
        {
            Env env("enable");
            writeMod(env.mods, "a.fprmod", tomlFor("aaa.mod"));
            writeMod(env.mods, "b.fprmod", tomlFor("bbb.mod"));
            t.IsTrue(env.start(), "starts");
            Result r = env.manager.setEnabled("aaa.mod", true);
            t.IsTrue(r.ok, "turns on: " + r.message);
            t.IsTrue(env.manager.find("aaa.mod")->enabled && env.manager.find("aaa.mod")->active, "on and active");
            t.IsTrue(fs::exists(env.state), "state file written");
            t.IsTrue(env.manager.restartNeeded(), "a restart is needed");
            r = env.manager.setEnabled("aaa.mod", true);
            t.IsTrue(r.ok && contains(r.message, "already on"), "turning on twice is fine");

            Manager again; // a restart
            Manager::Config config;
            config.modsDir = env.mods;
            config.statePath = env.state;
            t.IsTrue(again.init(config), "second start");
            t.IsTrue(again.find("aaa.mod")->enabled && again.find("aaa.mod")->active, "still on after a restart");
            t.IsFalse(again.find("bbb.mod")->enabled, "the other is still off");
            t.IsFalse(again.restartNeeded(), "the new session matches");
            t.Equals(again.sessionActive().size(), static_cast<size_t>(1), "session has the mod");

            r = again.setEnabled("aaa.mod", false);
            t.IsTrue(r.ok, "turns off: " + r.message);
            t.IsTrue(again.restartNeeded(), "off needs a restart too");
            again.setEnabled("aaa.mod", true);
            t.IsFalse(again.restartNeeded(), "back to the session's set: no restart needed");
        });

        tc.Run("dependencies", [](TestCase &t)
        {
            Env env("deps");
            writeMod(env.mods, "base.fprmod", tomlFor("base.mod", "", "assets", "1.2.0"));
            writeMod(env.mods, "needs.fprmod", tomlFor("needs.mod", "depends = [{ id = \"base.mod\", version = \">=1.0\" }]\n"));
            writeMod(env.mods, "newer.fprmod", tomlFor("newer.mod", "depends = [{ id = \"base.mod\", version = \">=2.0\" }]\n"));
            writeMod(env.mods, "lost.fprmod", tomlFor("lost.mod", "depends = [{ id = \"absent.mod\" }]\n"));
            t.IsTrue(env.start(), "starts");
            Result r = env.manager.setEnabled("needs.mod", true);
            t.IsFalse(r.ok, "cannot turn on without its dependency");
            t.IsTrue(contains(r.message, "Needs Mod base.mod to be turned on"), r.message);
            t.IsTrue(contains(env.manager.find("needs.mod")->problem, "to be turned on"), "the list says why: " + env.manager.find("needs.mod")->problem);
            r = env.manager.setEnabled("newer.mod", true);
            t.IsTrue(!r.ok && contains(r.message, ">=2.0") && contains(r.message, "1.2.0"), "version too old: " + r.message);
            r = env.manager.setEnabled("lost.mod", true);
            t.IsTrue(!r.ok && contains(r.message, "'absent.mod', which is not installed"), "missing: " + r.message);

            t.IsTrue(env.manager.setEnabled("base.mod", true).ok, "base on");
            r = env.manager.setEnabled("needs.mod", true);
            t.IsTrue(r.ok, "now it can be turned on: " + r.message);
            t.IsTrue(env.manager.find("needs.mod")->position > env.manager.find("base.mod")->position, "it loads after its dependency");
            r = env.manager.setEnabled("base.mod", false);
            t.IsTrue(!r.ok && contains(r.message, "needs it"), "cannot turn off a needed mod: " + r.message);
            t.IsTrue(env.manager.find("base.mod")->enabled, "still on");
            t.IsFalse(env.manager.uninstall("base.mod").ok, "cannot uninstall a needed mod either");
            t.IsTrue(env.manager.setEnabled("needs.mod", false).ok, "off first");
            t.IsTrue(env.manager.setEnabled("base.mod", false).ok, "then the dependency can go");
        });

        tc.Run("conflicts", [](TestCase &t)
        {
            Env env("conflicts");
            writeMod(env.mods, "a.fprmod", tomlFor("aaa.mod", "conflicts = [\"bbb.mod\"]\n"));
            writeMod(env.mods, "b.fprmod", tomlFor("bbb.mod"));
            writeMod(env.mods, "c.fprmod", tomlFor("ccc.mod", "conflicts = [\"aaa.mod\"]\n"));
            t.IsTrue(env.start(), "starts");
            t.IsTrue(env.manager.setEnabled("aaa.mod", true).ok, "a on");
            Result r = env.manager.setEnabled("bbb.mod", true);
            t.IsTrue(!r.ok && contains(r.message, "Conflicts with Mod aaa.mod"), "b conflicts (declared by a): " + r.message);
            r = env.manager.setEnabled("ccc.mod", true);
            t.IsTrue(!r.ok && contains(r.message, "Conflicts with Mod aaa.mod"), "c conflicts (declared by c): " + r.message);
            t.IsTrue(contains(env.manager.find("bbb.mod")->problem, "Conflicts with"), "the list shows it");
            t.IsTrue(env.manager.setEnabled("aaa.mod", false).ok, "a off");
            t.IsTrue(env.manager.setEnabled("bbb.mod", true).ok, "now b can go on");

            // A state file written by hand with two conflicting mods on: the earlier one in the order wins, the other is not active.
            Env hand("conflicts_hand");
            writeMod(hand.mods, "a.fprmod", tomlFor("aaa.mod", "conflicts = [\"bbb.mod\"]\n"));
            writeMod(hand.mods, "b.fprmod", tomlFor("bbb.mod"));
            writeBytesToFile(hand.state, "[[mod]]\nid = \"bbb.mod\"\nenabled = true\n[[mod]]\nid = \"aaa.mod\"\nenabled = true\n");
            t.IsTrue(hand.start(), "starts with a hand-made state");
            t.IsTrue(hand.manager.find("bbb.mod")->active, "the earlier mod is active");
            t.IsFalse(hand.manager.find("aaa.mod")->active, "the later one is not");
            t.IsTrue(hand.manager.find("aaa.mod")->enabled && contains(hand.manager.find("aaa.mod")->problem, "Conflicts with"), "it is on but says why it does nothing");
        });

        tc.Run("load order: moving, limits, and dependencies", [](TestCase &t)
        {
            Env env("order");
            writeMod(env.mods, "1.fprmod", tomlFor("aaa.mod"));
            writeMod(env.mods, "2.fprmod", tomlFor("bbb.mod", "depends = [{ id = \"ccc.mod\" }]\n"));
            writeMod(env.mods, "3.fprmod", tomlFor("ccc.mod"));
            t.IsTrue(env.start(), "starts");
            t.Equals(env.keys(), (std::vector<std::string>{"aaa.mod", "bbb.mod", "ccc.mod"}), "file order at first");
            Result r = env.manager.move("aaa.mod", -1);
            t.IsTrue(!r.ok && contains(r.message, "already first"), "first cannot go up: " + r.message);
            r = env.manager.move("ccc.mod", 1);
            t.IsTrue(!r.ok && contains(r.message, "already last"), "last cannot go down: " + r.message);
            t.IsTrue(env.manager.move("aaa.mod", 1).ok, "a down");
            t.Equals(env.keys(), (std::vector<std::string>{"bbb.mod", "aaa.mod", "ccc.mod"}), "a and b swapped");
            t.IsTrue(env.manager.move("ccc.mod", -1).ok, "c up");
            t.Equals(env.keys(), (std::vector<std::string>{"bbb.mod", "ccc.mod", "aaa.mod"}), "c above a");

            // b needs c: turning b on moves it below c.
            t.IsTrue(env.manager.setEnabled("ccc.mod", true).ok, "c on");
            t.IsTrue(env.manager.setEnabled("bbb.mod", true).ok, "b on");
            t.Equals(env.keys(), (std::vector<std::string>{"ccc.mod", "bbb.mod", "aaa.mod"}), "b moved below its dependency c");
            r = env.manager.move("bbb.mod", -1);
            t.IsTrue(!r.ok && contains(r.message, "needs"), "b cannot go above c: " + r.message);
            r = env.manager.move("ccc.mod", 1);
            t.IsTrue(!r.ok && contains(r.message, "needs"), "c cannot go below b: " + r.message);
            t.IsTrue(env.manager.move("bbb.mod", 1).ok, "b can go below a");
            t.Equals(env.keys(), (std::vector<std::string>{"ccc.mod", "aaa.mod", "bbb.mod"}), "order after");
            t.Equals(env.manager.find("bbb.mod")->position, 3, "position is 1-based");

            Manager again;
            Manager::Config config;
            config.modsDir = env.mods;
            config.statePath = env.state;
            again.init(config);
            std::vector<std::string> keys;
            for (const ModInfo &m : again.list())
                keys.push_back(m.key);
            t.Equals(keys, (std::vector<std::string>{"ccc.mod", "aaa.mod", "bbb.mod"}), "the order is kept across a restart");
            t.IsFalse(env.manager.move("file:none", 1).ok, "an unknown mod");
        });

        tc.Run("settings: defaults, validation, persistence", [](TestCase &t)
        {
            Env env("settings");
            writeMod(env.mods, "a.fprmod", tomlFor("aaa.mod", "",
                "assets") + "\n[[settings]]\nkey = \"compact\"\nlabel = \"Compact\"\ntype = \"bool\"\ndefault = false\n"
                "\n[[settings]]\nkey = \"colour\"\nlabel = \"Colour\"\ntype = \"choice\"\nchoices = [\"red\", \"blue\"]\ndefault = \"blue\"\n"
                "\n[[settings]]\nkey = \"size\"\nlabel = \"Size\"\ntype = \"int\"\nmin = 1\nmax = 9\ndefault = 4\n");
            t.IsTrue(env.start(), "starts");
            auto info = env.manager.find("aaa.mod");
            t.IsTrue(info.has_value() && info->valid, "valid");
            if (!info)
                return;
            t.Equals(info->settingValues["compact"], std::string("false"), "bool default");
            t.Equals(info->settingValues["colour"], std::string("blue"), "choice default");
            t.Equals(info->settingValues["size"], std::string("4"), "int default");
            t.IsFalse(env.manager.setSetting("aaa.mod", "compact", "yes").ok, "bad bool");
            t.IsFalse(env.manager.setSetting("aaa.mod", "colour", "green").ok, "bad choice");
            t.IsFalse(env.manager.setSetting("aaa.mod", "size", "10").ok, "int too big");
            t.IsFalse(env.manager.setSetting("aaa.mod", "size", "abc").ok, "int not a number");
            t.IsFalse(env.manager.setSetting("aaa.mod", "nothing", "1").ok, "unknown setting");
            t.IsFalse(env.manager.setSetting("nope.mod", "size", "1").ok, "unknown mod");
            t.IsTrue(env.manager.setSetting("aaa.mod", "compact", "true").ok, "set bool");
            t.IsTrue(env.manager.setSetting("aaa.mod", "colour", "red").ok, "set choice");
            t.IsTrue(env.manager.setSetting("aaa.mod", "size", "7").ok, "set int");
            const std::string text = readAll(env.state);
            t.IsTrue(contains(text, "compact = true") && contains(text, "size = 7") && contains(text, "colour = \"red\""), "typed values in the file: " + text);

            Manager again;
            Manager::Config config;
            config.modsDir = env.mods;
            config.statePath = env.state;
            again.init(config);
            auto kept = again.find("aaa.mod");
            t.IsTrue(kept->settingValues["compact"] == "true" && kept->settingValues["colour"] == "red" && kept->settingValues["size"] == "7", "values kept");

            // A state file with a value the declaration does not allow, and one for a setting that is not declared.
            writeBytesToFile(env.state, "[[mod]]\nid = \"aaa.mod\"\nenabled = false\n[mod.settings]\nsize = 99\ncolour = \"blue\"\nundeclared = 1\n");
            Manager third;
            third.init(config);
            auto fixed = third.find("aaa.mod");
            t.Equals(fixed->settingValues["size"], std::string("4"), "an invalid stored value falls back to the default");
            t.Equals(fixed->settingValues["colour"], std::string("blue"), "a valid one is kept");
            t.IsTrue(fixed->settingValues.count("undeclared") == 0, "undeclared settings are dropped");
        });

        tc.Run("state file: new mods are appended, orphans dropped, a broken file is set aside", [](TestCase &t)
        {
            Env env("state");
            writeMod(env.mods, "a.fprmod", tomlFor("aaa.mod"));
            writeMod(env.mods, "b.fprmod", tomlFor("bbb.mod"));
            writeBytesToFile(env.state, "[[mod]]\nid = \"gone.mod\"\nenabled = true\n[[mod]]\nid = \"bbb.mod\"\nenabled = true\n");
            t.IsTrue(env.start(), "starts");
            t.Equals(env.keys(), (std::vector<std::string>{"bbb.mod", "aaa.mod"}), "listed mods first, new mod after");
            t.IsTrue(env.manager.find("bbb.mod")->enabled, "enabled from the file");
            env.manager.setSetting("aaa.mod", "x", "1"); // refused, writes nothing
            t.IsTrue(env.manager.setEnabled("aaa.mod", true).ok, "a change writes the file");
            const std::string text = readAll(env.state);
            t.IsFalse(contains(text, "gone.mod"), "a mod that is not installed is forgotten");
            t.IsTrue(text.find("bbb.mod") < text.find("aaa.mod"), "order written");

            Env broken("state_broken");
            writeMod(broken.mods, "a.fprmod", tomlFor("aaa.mod"));
            writeBytesToFile(broken.state, "[[mod\nthis is not toml");
            t.IsTrue(broken.start(), "starts anyway");
            t.IsFalse(broken.manager.find("aaa.mod")->enabled, "nothing is on");
            t.IsTrue(fs::exists(broken.root / "mods.toml.bad"), "the old file is kept as mods.toml.bad");
            t.IsFalse(broken.manager.notes().empty(), "a note says so");
            t.IsTrue(contains(broken.manager.notes()[0], "mods.toml.bad"), broken.manager.notes()[0]);
        });

        tc.Run("install", [](TestCase &t)
        {
            Env env("install");
            const fs::path incoming = tempDir("install_incoming");
            t.IsTrue(env.start(), "starts");
            const fs::path good = writeMod(incoming, "nice.fprmod", tomlFor("nice.mod"));
            Result r = env.manager.install(good);
            t.IsTrue(r.ok && r.key == "nice.mod", "installed: " + r.message);
            t.IsTrue(contains(r.message, "Installed Mod nice.mod 1.0.0"), r.message);
            t.IsTrue(fs::exists(env.mods / "nice.fprmod"), "the file is in the mods folder");
            t.IsTrue(fs::exists(good), "the original stays where it was");
            t.IsFalse(fs::exists(env.mods / "nice.fprmod.part"), "no leftover partial copy");
            auto info = env.manager.find("nice.mod");
            t.IsTrue(info.has_value() && info->valid && !info->enabled, "listed and off");
            t.IsFalse(env.manager.restartNeeded(), "a mod that is off needs no restart");

            r = env.manager.install(good);
            t.IsTrue(!r.ok && contains(r.message, "already installed"), "the same mod twice: " + r.message);
            t.IsFalse(env.manager.install(env.mods / "nice.fprmod").ok, "a file that is already in the folder");

            // Another file with the same id.
            const fs::path again = writeMod(incoming, "other-name.fprmod", tomlFor("nice.mod", "", "assets", "2.0.0"));
            r = env.manager.install(again);
            t.IsTrue(!r.ok && contains(r.message, "version 1.0.0") && contains(r.message, "Uninstall it first"), "same id: " + r.message);
            t.IsFalse(fs::exists(env.mods / "other-name.fprmod"), "nothing copied");

            // Not a mod file, a missing file, a bad package.
            writeBytesToFile(incoming / "readme.txt", "hello");
            r = env.manager.install(incoming / "readme.txt");
            t.IsTrue(!r.ok && contains(r.message, "not a mod file"), "wrong extension: " + r.message);
            r = env.manager.install(incoming / "missing.fprmod");
            t.IsTrue(!r.ok && contains(r.message, "cannot be found"), "missing: " + r.message);
            writeBytesToFile(incoming / "junk.fprmod", "junk");
            r = env.manager.install(incoming / "junk.fprmod");
            t.IsTrue(!r.ok && contains(r.message, "Not installed: This is not a valid zip file"), "bad package: " + r.message);
            const fs::path wrongGame = writeMod(incoming, "wrong.fprmod", tomlWith("game", "game = \"SLUS_000.00\""));
            r = env.manager.install(wrongGame);
            t.IsTrue(!r.ok && contains(r.message, "not 'SLUS_217.02'"), "wrong game: " + r.message);
            t.Equals(env.manager.list().size(), static_cast<size_t>(1), "only the good mod is listed");

            // A file with the name of an existing file but another mod: it takes the mod's id as its name.
            const fs::path samename = writeMod(incoming, "nice.fprmod", tomlFor("different.mod"));
            r = env.manager.install(samename);
            t.IsTrue(r.ok, "installed under another name: " + r.message);
            t.IsTrue(fs::exists(env.mods / "different.mod.fprmod"), "named after its id");
            t.Equals(env.manager.list().size(), static_cast<size_t>(2), "two mods");

            Manager reload;
            Manager::Config config;
            config.modsDir = env.mods;
            config.statePath = env.state;
            reload.init(config);
            t.Equals(reload.list().size(), static_cast<size_t>(2), "both are still there after a restart");
        });

        tc.Run("an installed mod goes last in the order, even when no state file has been written yet", [](TestCase &t)
        {
            Env env("install_last");
            const fs::path incoming = tempDir("install_last_incoming");
            writeMod(env.mods, "z-late.fprmod", tomlFor("zzz.mod"));
            t.IsTrue(env.start(), "starts");
            t.IsFalse(fs::exists(env.state), "no state file yet");
            // Sorts before the installed one by file name, but it is the newer: it goes after.
            const Result r = env.manager.install(writeMod(incoming, "a-early.fprmod", tomlFor("aaa.mod")));
            t.IsTrue(r.ok, "installed: " + r.message);
            t.Equals(env.keys(), (std::vector<std::string>{"zzz.mod", "aaa.mod"}), "the new mod is last");
            t.Equals(env.manager.find("aaa.mod")->position, 2, "position 2");
            t.IsTrue(fs::exists(env.state), "the order is written");

            Manager again;
            Manager::Config config;
            config.modsDir = env.mods;
            config.statePath = env.state;
            again.init(config);
            std::vector<std::string> keys;
            for (const ModInfo &m : again.list())
                keys.push_back(m.key);
            t.Equals(keys, (std::vector<std::string>{"zzz.mod", "aaa.mod"}), "and it stays last after a restart");

            // The same for a file that appears in the folder by hand and is found by a rescan.
            writeMod(env.mods, "0-first.fprmod", tomlFor("bbb.mod"));
            env.manager.rescan();
            t.Equals(env.keys(), (std::vector<std::string>{"zzz.mod", "aaa.mod", "bbb.mod"}), "a mod found by a rescan goes last too");
        });

        tc.Run("uninstall moves the file to .trash and forgets the state", [](TestCase &t)
        {
            Env env("uninstall");
            writeMod(env.mods, "a.fprmod", tomlFor("aaa.mod"));
            writeMod(env.mods, "junk.fprmod", tomlFor("bad.mod", "kind = \"skin\"\n"));
            t.IsTrue(env.start(), "starts");
            t.IsTrue(env.manager.setEnabled("aaa.mod", true).ok, "on");
            Result r = env.manager.uninstall("aaa.mod");
            t.IsTrue(r.ok, "uninstalled: " + r.message);
            t.IsTrue(contains(r.message, ".trash"), r.message);
            t.IsFalse(fs::exists(env.mods / "a.fprmod"), "gone from the mods folder");
            t.IsTrue(fs::exists(env.mods / ".trash" / "a.fprmod"), "moved to .trash");
            t.IsFalse(env.manager.find("aaa.mod").has_value(), "no longer listed");
            t.IsFalse(contains(readAll(env.state), "aaa.mod"), "its state is gone");

            // The same name again: the trash keeps both.
            writeMod(env.mods, "a.fprmod", tomlFor("aaa.mod"));
            env.manager.rescan();
            t.IsTrue(env.manager.find("aaa.mod").has_value(), "back after a rescan");
            t.IsTrue(env.manager.uninstall("aaa.mod").ok, "uninstalled again");
            size_t inTrash = 0;
            for (const auto &entry : fs::directory_iterator(env.mods / ".trash"))
            {
                (void)entry;
                ++inTrash;
            }
            t.Equals(inTrash, static_cast<size_t>(2), "two files in .trash, none overwritten");

            // A bad package can be removed too, by its file key.
            const auto list = env.manager.list();
            t.Equals(list.size(), static_cast<size_t>(1), "the bad one is listed");
            r = env.manager.uninstall(list[0].key);
            t.IsTrue(r.ok, "bad package removed: " + r.message);
            t.IsTrue(env.manager.list().empty(), "nothing left");
            t.IsFalse(env.manager.uninstall("aaa.mod").ok, "unknown mod");
            env.manager.rescan();
            t.IsTrue(env.manager.list().empty(), ".trash is not scanned");
        });

        tc.Run("the session: what the game loaded versus what the player chose", [](TestCase &t)
        {
            Env env("session");
            writeMod(env.mods, "a.fprmod", tomlFor("aaa.mod"), {{"assets/one.txt", "from a"}});
            writeMod(env.mods, "b.fprmod", tomlFor("bbb.mod"), {{"assets/one.txt", "from b"}});
            t.IsTrue(env.start(), "starts");
            t.IsTrue(env.manager.setEnabled("aaa.mod", true).ok, "a on");
            t.IsFalse(env.manager.open("assets/one.txt").has_value(), "the running game has not loaded it");
            t.IsTrue(env.manager.providers("assets/one.txt").empty(), "no providers yet");
            t.IsTrue(env.manager.restartNeeded(), "restart needed");
            t.IsFalse(env.manager.find("aaa.mod")->inSession, "not in the session");
            env.manager.commitSession(); // what a restart does
            t.IsFalse(env.manager.restartNeeded(), "after the restart nothing is pending");
            t.IsTrue(env.manager.find("aaa.mod")->inSession, "in the session now");
            auto file = env.manager.open("assets/one.txt");
            t.IsTrue(file.has_value() && std::string(file->begin(), file->end()) == "from a", "a's file");
            t.IsTrue(env.manager.setEnabled("bbb.mod", true).ok, "b on");
            file = env.manager.open("assets/one.txt");
            t.IsTrue(file && std::string(file->begin(), file->end()) == "from a", "still a's: b is not loaded until a restart");
            env.manager.commitSession();
            file = env.manager.open("assets/one.txt");
            t.IsTrue(file && std::string(file->begin(), file->end()) == "from b", "b is later in the load order, so it wins");
            env.manager.move("bbb.mod", -1);
            t.IsTrue(env.manager.restartNeeded(), "a different order needs a restart");
            env.manager.commitSession();
            file = env.manager.open("assets/one.txt");
            t.IsTrue(file && std::string(file->begin(), file->end()) == "from a", "now a is later, so a wins");
            t.IsTrue(env.manager.setEnabled("aaa.mod", false).ok, "a off");
            env.manager.commitSession();
            file = env.manager.open("assets/one.txt");
            t.IsTrue(file && std::string(file->begin(), file->end()) == "from b", "a mod that is off provides nothing");
        });

        tc.Run("the virtual file lookup", [](TestCase &t)
        {
            Env env("vfs");
            writeMod(env.mods, "a.fprmod", tomlFor("aaa.mod", "thumbnail = \"thumb.png\"\n"),
                     {{"thumb.png", fakePng()}, {"assets/shared.txt", "a-shared"}, {"assets/only-a.txt", "only a"}, {"data/table.bin", "tbl"}, {"misc/notes.txt", "notes"}, {"assets/", ""}});
            writeMod(env.mods, "b.fprmod", tomlFor("bbb.mod"), {{"assets/shared.txt", "b-shared"}, {"assets/only-b.txt", "only b"}, {"code/x.txt", "code"}});
            writeMod(env.mods, "c.fprmod", tomlFor("ccc.mod"), {{"assets/shared.txt", "c-shared"}});
            t.IsTrue(env.start(), "starts");
            for (const char *id : {"aaa.mod", "bbb.mod"})
                t.IsTrue(env.manager.setEnabled(id, true).ok, std::string("on: ") + id);
            env.manager.commitSession();

            auto text = [&](const std::string &path) -> std::string {
                auto bytes = env.manager.open(path);
                return bytes ? std::string(bytes->begin(), bytes->end()) : std::string("<none>");
            };
            t.Equals(text("assets/shared.txt"), std::string("b-shared"), "the later mod wins");
            t.Equals(text("assets/only-a.txt"), std::string("only a"), "a's own file");
            t.Equals(text("assets/only-b.txt"), std::string("only b"), "b's own file");
            t.Equals(text("data/table.bin"), std::string("tbl"), "data folder");
            t.Equals(text("code/x.txt"), std::string("code"), "code folder");
            t.Equals(text("mod.toml"), std::string("<none>"), "mod.toml is not visible");
            t.Equals(text("thumb.png"), std::string("<none>"), "the manager's pictures are not visible");
            t.Equals(text("misc/notes.txt"), std::string("<none>"), "folders that are not content folders are not visible");
            t.Equals(text("assets/"), std::string("<none>"), "a folder entry is not a file");
            t.Equals(text("Assets/shared.txt"), std::string("<none>"), "names are exact");
            t.Equals(text("../x"), std::string("<none>"), "no way out");

            t.Equals(env.manager.providers("assets/shared.txt"), (std::vector<std::string>{"aaa.mod", "bbb.mod"}), "providers in load order");
            t.Equals(env.manager.providers("assets/only-a.txt"), (std::vector<std::string>{"aaa.mod"}), "one provider");
            t.IsTrue(env.manager.providers("assets/none").empty(), "none");
            const auto conflicts = env.manager.conflicts();
            t.Equals(conflicts.size(), static_cast<size_t>(1), "one shared path");
            if (!conflicts.empty())
                t.IsTrue(conflicts[0].path == "assets/shared.txt" && conflicts[0].providers.size() == 2, "it is assets/shared.txt, with both mods");

            // A mod that is off still shows its pictures to the manager.
            std::string error;
            auto thumb = env.manager.readFile("aaa.mod", "thumb.png", 1000, &error);
            t.IsTrue(thumb.has_value() && thumb->size() == 8, "thumbnail read: " + error);
            t.IsTrue(env.manager.readFile("ccc.mod", "assets/shared.txt").has_value(), "a mod that is off can be read by id");
            t.IsFalse(env.manager.readFile("aaa.mod", "../mod.toml", 1000, &error).has_value(), "an unsafe path is refused");
            t.IsFalse(env.manager.readFile("aaa.mod", "nothing.txt", 1000, &error).has_value(), "a missing file");
            t.IsFalse(env.manager.readFile("zzz.mod", "x", 1000, &error).has_value(), "an unknown mod");
            t.IsFalse(env.manager.readFile("aaa.mod", "thumb.png", 4, &error).has_value(), "over the byte limit");
            auto virt = env.manager.readVirtual("mod:aaa.mod/assets/only-a.txt");
            t.IsTrue(virt && std::string(virt->begin(), virt->end()) == "only a", "readVirtual");
            t.IsFalse(env.manager.readVirtual("file:aaa.mod/x").has_value(), "only the mod: names");
            t.IsFalse(env.manager.readVirtual("mod:aaa.mod").has_value(), "a name needs a path");

            // The winner's file goes away: the lookup falls back to the mod below it.
            t.IsTrue(env.manager.uninstall("bbb.mod").ok, "b uninstalled while the game runs");
            t.Equals(text("assets/shared.txt"), std::string("a-shared"), "falls back to a");
            t.Equals(text("assets/only-b.txt"), std::string("<none>"), "b's own files are gone");
        });

        tc.Run("theme stylesheets come from the session's active theme mods in load order", [](TestCase &t)
        {
            Env env("themes");
            writeMod(env.mods, "1.fprmod", tomlFor("theme.one", "stylesheets = [\"theme/a.rcss\", \"theme/b.rcss\"]\n", "theme"), {{"theme/a.rcss", "a{}"}, {"theme/b.rcss", "b{}"}});
            writeMod(env.mods, "2.fprmod", tomlFor("theme.two", "stylesheets = [\"theme/c.rcss\"]\n", "theme"), {{"theme/c.rcss", "c{}"}});
            writeMod(env.mods, "3.fprmod", tomlFor("data.mod"));
            t.IsTrue(env.start(), "starts");
            t.IsTrue(env.manager.sessionThemeStylesheets().empty(), "none at first");
            env.manager.setEnabled("theme.two", true);
            env.manager.setEnabled("theme.one", true);
            env.manager.setEnabled("data.mod", true);
            t.IsTrue(env.manager.sessionThemeStylesheets().empty(), "the session has not changed");
            env.manager.commitSession();
            const auto sheets = env.manager.sessionThemeStylesheets();
            t.Equals(sheets, (std::vector<std::string>{"mod:theme.one/theme/a.rcss", "mod:theme.one/theme/b.rcss", "mod:theme.two/theme/c.rcss"}),
                     "all theme stylesheets in load order (theme.one is listed first on disk)");
            auto css = env.manager.readVirtual(sheets[2]);
            t.IsTrue(css && std::string(css->begin(), css->end()) == "c{}", "the virtual name opens the file");
        });

        tc.Run("a package over the limits is listed as unusable", [](TestCase &t)
        {
            Env env("limits");
            writeMod(env.mods, "big.fprmod", tomlFor("big.mod"), {{"assets/huge.bin", std::string(5000, 'x')}});
            Limits limits;
            limits.maxEntryBytes = 1000;
            t.IsTrue(env.start(limits), "starts");
            auto info = env.manager.list();
            t.IsTrue(info.size() == 1 && !info[0].valid && contains(info[0].error, "more than the limit"), "refused for its size");
        });

        tc.Run("a folder that cannot be made is reported", [](TestCase &t)
        {
            Env env("nofolder");
            writeBytesToFile(env.root / "mods", "a file where the folder should be");
            t.IsFalse(env.start(), "init fails");
            t.IsTrue(contains(env.manager.initError(), "cannot be created or read"), env.manager.initError());
            t.IsFalse(env.manager.ready(), "not ready");
            t.IsFalse(env.manager.install(env.root / "x.fprmod").ok, "install says the manager is not running");
        });
    });
}
