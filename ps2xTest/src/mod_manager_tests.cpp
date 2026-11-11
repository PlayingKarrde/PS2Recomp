#include "MiniTest.h"

#include "fpr_save.h"
#include "hot_load.h"
#include "mod_manager.h"
#include "mod_manifest.h"
#include "mod_package.h"
#include "mod_state.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
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

        tc.Run("readPackageEntryAt reads stored, deflated and empty entries without the central directory, and checks them", [](TestCase &t)
        {
            std::string big;
            for (int i = 0; i < 3000; ++i)
                big += "texture " + std::to_string(i % 17) + " ";
            Files files = {{"mod.toml", tomlFor("rd.mod")}, {"assets/big.txt", big}, {"assets/small.bin", std::string("\x01\x02\x03", 3)}, {"assets/empty.bin", ""}};
            for (int i = 0; i < 200; ++i)
                files.emplace_back("assets/f" + std::to_string(i) + ".txt", "file " + std::to_string(i * 7919));
            fs::path dir = tempDir("readat");
            const fs::path file = dir / "rd.fprmod";
            auto zip = buildPackage(files);
            t.IsTrue(zip.has_value() && writeBytesToFile(file, *zip), "package written");
            PackageResult read = readPackage(file);
            t.IsTrue(read.package.has_value(), "package reads: " + read.error);
            if (!read.package)
                return;
            auto find = [&](const std::string &name) -> const PackageEntry * { return read.package->find(name); };
            for (const auto &f : files)
            {
                if (f.first == "mod.toml")
                    continue;
                const PackageEntry *entry = find(f.first);
                t.IsTrue(entry != nullptr, "entry listed: " + f.first);
                if (!entry)
                    continue;
                std::string error;
                auto bytes = readPackageEntryAt(file, *entry, 1 << 20, &error);
                t.IsTrue(bytes.has_value() && std::string(bytes->begin(), bytes->end()) == f.second, "the same bytes as put in: " + f.first + " " + error);
            }
            const PackageEntry *entry = find("assets/big.txt");
            t.IsTrue(entry && entry->method == 8 && entry->packedSize < entry->size, "the big file is deflated");
            std::string error;
            t.IsFalse(readPackageEntryAt(file, *entry, 100, &error).has_value(), "over the byte limit");
            PackageEntry wrong = *entry;
            wrong.crc32 ^= 1;
            t.IsFalse(readPackageEntryAt(file, wrong, 1 << 20, &error).has_value(), "a wrong checksum is refused");
            t.IsTrue(error.find("checksum") != std::string::npos, "says so: " + error);
            wrong = *entry;
            wrong.localOffset += 5;
            t.IsFalse(readPackageEntryAt(file, wrong, 1 << 20, &error).has_value(), "a wrong place is refused");
            t.IsFalse(readPackageEntryAt(dir / "none.fprmod", *entry, 1 << 20, &error).has_value(), "a missing file");
            std::error_code ec;
            fs::remove_all(dir, ec);
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

            // The listing the texture packs index their files with: every content path once, with the mod that wins it.
            const auto assets = env.manager.sessionFiles("assets/");
            t.Equals(assets.size(), static_cast<size_t>(3), "three asset paths");
            bool listingOk = assets.size() == 3;
            if (listingOk)
            {
                listingOk = assets[0].path == "assets/only-a.txt" && assets[0].modId == "aaa.mod" && assets[0].order == 0 &&
                            assets[1].path == "assets/only-b.txt" && assets[1].modId == "bbb.mod" && assets[1].order == 1 &&
                            assets[2].path == "assets/shared.txt" && assets[2].modId == "bbb.mod" && assets[2].order == 1;
            }
            t.IsTrue(listingOk, "sorted by path, the winner of each, its load order position");
            t.Equals(env.manager.sessionFiles("assets/sh").size(), static_cast<size_t>(1), "a longer prefix");
            t.Equals(env.manager.sessionFiles("data/").size(), static_cast<size_t>(1), "another content folder");
            t.IsTrue(env.manager.sessionFiles("textures/").empty() && env.manager.sessionFiles("misc/").empty(), "nothing there, and misc/ is not a content folder");

            // Notes the running game attaches to a mod (a refused texture), kept once and capped.
            env.manager.addRuntimeNote("aaa.mod", "0123.png is too big");
            env.manager.addRuntimeNote("aaa.mod", "0123.png is too big");
            t.Equals(env.manager.runtimeNotes("aaa.mod").size(), static_cast<size_t>(1), "a repeated note is kept once");
            for (int i = 0; i < 40; ++i)
                env.manager.addRuntimeNote("aaa.mod", "note " + std::to_string(i));
            t.Equals(env.manager.runtimeNotes("aaa.mod").size(), Manager::kMaxRuntimeNotes, "capped");
            t.IsTrue(env.manager.runtimeNotes("bbb.mod").empty(), "per mod");

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

    // ---- Load Now (task h1): the session by kind, the guest check, the request's state machine, the plan ------------------------

    MiniTest::Case("ModSessionByKind", [](TestCase &tc)
    {
        // A data pack and a texture pack: the first can be hot loaded, the second needs a restart.
        auto twoMods = [](Env &env) {
            writeMod(env.mods, "data-a.fprmod", tomlFor("data.a", "", "data"), {{"data/renames.json", "{}"}});
            writeMod(env.mods, "data-b.fprmod", tomlFor("data.b", "", "data"), {{"data/renames.json", "{}"}});
            writeMod(env.mods, "tex.fprmod", tomlFor("tex.one"), {{"assets/textures/x.png", fakePng()}});
        };

        tc.Run("a data pack turned on is a data change; a texture pack is a change only a restart applies", [](TestCase &t)
        {
            Env env("sess_flags");
            writeMod(env.mods, "data-a.fprmod", tomlFor("data.a", "", "data"), {{"data/renames.json", "{}"}});
            writeMod(env.mods, "tex.fprmod", tomlFor("tex.one"), {{"assets/textures/x.png", fakePng()}});
            t.IsTrue(env.start(), "starts");
            t.IsFalse(env.manager.dataChangesPending() || env.manager.otherChangesPending() || env.manager.restartNeeded(), "nothing pending");
            t.IsTrue(env.manager.setEnabled("data.a", true).ok, "data pack on");
            t.IsTrue(env.manager.dataChangesPending(), "a data change waits");
            t.IsFalse(env.manager.otherChangesPending(), "no other change");
            t.IsTrue(env.manager.restartNeeded(), "a restart would apply it too");
            t.IsTrue(env.manager.setEnabled("tex.one", true).ok, "texture pack on");
            t.IsTrue(env.manager.otherChangesPending(), "a change only a restart applies waits");
        });

        tc.Run("committing the data session clears the data change and leaves the restart-only one", [=](TestCase &t)
        {
            Env env("sess_commit");
            twoMods(env);
            t.IsTrue(env.start(), "starts");
            env.manager.setEnabled("data.a", true);
            env.manager.setEnabled("tex.one", true);
            const Manager::DataLoad load = env.manager.liveDataPacks();
            t.Equals(load.ids, (std::vector<std::string>{"data.a"}), "the live data packs");
            t.IsFalse(load.signature.empty(), "with a signature");
            env.manager.commitDataSession(load);
            t.IsFalse(env.manager.dataChangesPending(), "the data change is applied");
            t.IsTrue(env.manager.otherChangesPending(), "the texture pack still needs a restart");
            t.IsTrue(env.manager.restartNeeded(), "so a restart is still needed");
            const auto session = env.manager.sessionActive();
            t.Equals(session, (std::vector<std::string>{"data.a"}), "the session has the data pack and not the texture pack");
            t.IsTrue(env.manager.find("data.a")->inSession, "the data pack is in the session");
            t.IsFalse(env.manager.find("tex.one")->inSession, "the texture pack is not");
            env.manager.commitSession();
            t.IsFalse(env.manager.restartNeeded(), "a full commit (a restart) clears both");
        });

        tc.Run("a data pack turned off is dropped from the session by the data commit; the session's texture pack keeps serving its files", [=](TestCase &t)
        {
            Env env("sess_keep");
            twoMods(env);
            t.IsTrue(env.start(), "starts");
            env.manager.setEnabled("data.a", true);
            env.manager.setEnabled("tex.one", true);
            env.manager.commitSession(); // the game started with both
            t.Equals(env.manager.sessionActive(), (std::vector<std::string>{"data.a", "tex.one"}), "the session in load order");
            env.manager.setEnabled("data.a", false);
            env.manager.setEnabled("tex.one", false);
            t.IsTrue(env.manager.dataChangesPending() && env.manager.otherChangesPending(), "both moved");
            env.manager.commitDataSession(env.manager.liveDataPacks());
            t.IsFalse(env.manager.dataChangesPending(), "the data side matches");
            t.IsTrue(env.manager.otherChangesPending(), "the texture pack is still running in the game");
            t.Equals(env.manager.sessionActive(), (std::vector<std::string>{"tex.one"}), "only the texture pack is left in the session");
            t.IsTrue(env.manager.open("assets/textures/x.png").has_value(), "its file is still served to the running game");
            t.IsFalse(env.manager.find("data.a")->inSession, "the data pack is out");
            // uninstalled while the game still runs with it
            t.IsTrue(env.manager.uninstall("tex.one").ok, "uninstalls (it is off)");
            env.manager.setEnabled("data.b", true);
            env.manager.commitDataSession(env.manager.liveDataPacks());
            t.Equals(env.manager.providers("assets/textures/x.png"), (std::vector<std::string>{"tex.one"}),
                     "a data commit does not drop a session mod whose package was uninstalled (the file itself is gone with the package)");
            t.Equals(env.manager.sessionActive(), (std::vector<std::string>{"data.b", "tex.one"}), "the data pack first (load order), the orphan after");
        });

        tc.Run("what was loaded is what the commit says: a change made while the game was loading stays pending", [=](TestCase &t)
        {
            Env env("sess_race");
            twoMods(env);
            t.IsTrue(env.start(), "starts");
            env.manager.setEnabled("data.a", true);
            const Manager::DataLoad taken = env.manager.liveDataPacks(); // the snapshot the load was built from
            env.manager.setEnabled("data.b", true);                      // the player turns another on while the game loads
            env.manager.commitDataSession(taken);
            t.IsTrue(env.manager.dataChangesPending(), "the pack turned on meanwhile is still waiting");
            t.Equals(env.manager.sessionActive(), (std::vector<std::string>{"data.a"}), "the session is exactly what was loaded");
            env.manager.commitDataSession(env.manager.liveDataPacks());
            t.IsFalse(env.manager.dataChangesPending(), "a second load catches up");
        });

        tc.Run("the order of data packs is part of the signature; moving one is a data change", [=](TestCase &t)
        {
            Env env("sess_order");
            twoMods(env);
            t.IsTrue(env.start(), "starts");
            env.manager.setEnabled("data.a", true);
            env.manager.setEnabled("data.b", true);
            env.manager.commitSession();
            t.IsFalse(env.manager.dataChangesPending(), "in step");
            t.IsTrue(env.manager.move("data.b", -1).ok, "moves data.b earlier");
            t.IsTrue(env.manager.dataChangesPending(), "a different order is a data change");
            t.IsFalse(env.manager.otherChangesPending(), "and nothing else");
        });

        tc.Run("clearNotes forgets a pack's notes only", [](TestCase &t)
        {
            Env env("sess_notes");
            writeMod(env.mods, "data-a.fprmod", tomlFor("data.a", "", "data"), {{"data/renames.json", "{}"}});
            writeMod(env.mods, "tex.fprmod", tomlFor("tex.one"), {{"assets/textures/x.png", fakePng()}});
            t.IsTrue(env.start(), "starts");
            env.manager.addInfoNote("data.a", "Changes 3 fields on the profile card.");
            env.manager.addRuntimeNote("data.a", "data/renames.json: broken");
            env.manager.addRuntimeNote("tex.one", "a picture was refused");
            env.manager.clearNotes("data.a");
            t.IsTrue(env.manager.infoNotes("data.a").empty() && env.manager.runtimeNotes("data.a").empty(), "the pack's notes are gone");
            t.Equals(env.manager.runtimeNotes("tex.one").size(), static_cast<size_t>(1), "another mod's note stays");
        });
    });

    MiniTest::Case("HotLoadVerify", [](TestCase &tc)
    {
        namespace hl = ps2x::mods::hotload;
        namespace fpr = ps2x::mods::fpr;
        // A save image with a recognisable roster area and two used edit slots; the "game's memory" starts as a copy of those two areas.
        auto makeSave = []() {
            std::vector<uint8_t> save(fpr::kSaveSize, 0);
            for (size_t i = 0; i < hl::kRosterSize; ++i)
                save[hl::kRosterOffset + i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
            for (size_t k : {size_t(0), size_t(5)})
            {
                uint8_t *slot = save.data() + hl::kSlotsOffset + k * hl::kSlotSize;
                slot[0] = 1;
                for (size_t i = 2; i < hl::kSlotSize; ++i)
                    slot[i] = static_cast<uint8_t>((i + k) & 0xFF);
            }
            return save;
        };
        struct Guest
        {
            std::vector<uint8_t> roster, slots;
            hl::GuestView view()
            {
                hl::GuestView v;
                v.roster = roster.data();
                v.rosterSize = roster.size();
                v.slots = slots.data();
                v.slotsSize = slots.size();
                return v;
            }
        };
        auto guestOf = [](const std::vector<uint8_t> &save) {
            Guest g;
            g.roster.assign(save.begin() + hl::kRosterOffset, save.begin() + hl::kRosterOffset + hl::kRosterSize);
            g.slots.assign(save.begin() + hl::kSlotsOffset, save.begin() + hl::kSlotsOffset + hl::kSlotCount * hl::kSlotSize);
            return g;
        };

        tc.Run("memory that equals the save passes", [=](TestCase &t)
        {
            const auto save = makeSave();
            Guest g = guestOf(save);
            hl::GuestView v = g.view();
            const hl::Verdict verdict = hl::verifyGuest(save, v);
            t.IsTrue(verdict.ok, verdict.detail);
            t.Equals(verdict.scrubbedSlots, static_cast<size_t>(0), "nothing to clean");
        });

        tc.Run("a different roster byte fails and is described", [=](TestCase &t)
        {
            const auto save = makeSave();
            Guest g = guestOf(save);
            g.roster[0x4A00] ^= 0x40;
            hl::GuestView v = g.view();
            const hl::Verdict verdict = hl::verifyGuest(save, v);
            t.IsFalse(verdict.ok, "differs");
            t.IsTrue(contains(verdict.detail, "roster data differs in 1 bytes"), verdict.detail);
        });

        tc.Run("a used edit slot must be identical, and a slot used on one side only fails", [=](TestCase &t)
        {
            const auto save = makeSave();
            {
                Guest g = guestOf(save);
                g.slots[5 * hl::kSlotSize + 0x100] ^= 1;
                hl::GuestView v = g.view();
                const hl::Verdict verdict = hl::verifyGuest(save, v);
                t.IsFalse(verdict.ok, "a changed byte in slot 5");
                t.IsTrue(contains(verdict.detail, "1 edit slots differ (the first is slot 5)"), verdict.detail);
            }
            {
                Guest g = guestOf(save);
                g.slots[7 * hl::kSlotSize] = 1; // the game thinks slot 7 is used; the card says not
                hl::GuestView v = g.view();
                t.IsFalse(hl::verifyGuest(save, v).ok, "used in memory only");
            }
            {
                Guest g = guestOf(save);
                std::fill(g.slots.begin() + static_cast<std::ptrdiff_t>(5 * hl::kSlotSize), g.slots.begin() + static_cast<std::ptrdiff_t>(6 * hl::kSlotSize), 0);
                hl::GuestView v = g.view();
                t.IsFalse(hl::verifyGuest(save, v).ok, "used on the card only");
            }
        });

        tc.Run("leftovers in an unused slot (the loader only clears its flag) are zeroed, as a restart has them", [=](TestCase &t)
        {
            const auto save = makeSave();
            Guest g = guestOf(save);
            // slots 9 and 11: an earlier wrestler's bytes with the flag cleared
            for (size_t k : {size_t(9), size_t(11)})
            {
                g.slots[k * hl::kSlotSize + 0] = 0;
                g.slots[k * hl::kSlotSize + 1] = 0;
                for (size_t i = 2; i < hl::kSlotSize; ++i)
                    g.slots[k * hl::kSlotSize + i] = 0xAB;
            }
            hl::GuestView v = g.view();
            const hl::Verdict verdict = hl::verifyGuest(save, v);
            t.IsTrue(verdict.ok, verdict.detail);
            t.Equals(verdict.scrubbedSlots, static_cast<size_t>(2), "two slots cleaned");
            t.IsTrue(std::all_of(g.slots.begin() + static_cast<std::ptrdiff_t>(9 * hl::kSlotSize), g.slots.begin() + static_cast<std::ptrdiff_t>(10 * hl::kSlotSize),
                                 [](uint8_t b) { return b == 0; }), "slot 9 is all zero now");
            t.IsTrue(std::equal(g.slots.begin(), g.slots.end(), save.begin() + static_cast<std::ptrdiff_t>(hl::kSlotsOffset)), "the whole area equals the card's");
        });

        tc.Run("missing or wrongly sized memory and a wrongly sized save fail without touching anything", [=](TestCase &t)
        {
            const auto save = makeSave();
            Guest g = guestOf(save);
            hl::GuestView none;
            t.IsFalse(hl::verifyGuest(save, none).ok, "no pointers");
            hl::GuestView v = g.view();
            v.rosterSize -= 1;
            t.IsFalse(hl::verifyGuest(save, v).ok, "short roster");
            v = g.view();
            std::vector<uint8_t> small(100, 0);
            const hl::Verdict verdict = hl::verifyGuest(small, v);
            t.IsFalse(verdict.ok, "a save of the wrong size");
            t.IsTrue(contains(verdict.detail, "wrong size"), verdict.detail);
        });
    });

    MiniTest::Case("HotLoadController", [](TestCase &tc)
    {
        namespace hl = ps2x::mods::hotload;
        namespace fpr = ps2x::mods::fpr;
        using Ctl = hl::Controller;
        constexpr uint64_t ms = 1'000'000ull;

        struct Fixture
        {
            Ctl ctl;
            std::vector<std::string> calls;
            std::vector<uint8_t> planSave;
            bool busy = false, planOk = true;
            std::string planWhy = "the profile could not be built";
            Fixture()
            {
                planSave.assign(fpr::kSaveSize, 0);
                for (size_t i = 0; i < hl::kRosterSize; ++i)
                    planSave[hl::kRosterOffset + i] = static_cast<uint8_t>(i * 13);
            }
            Ctl::Hooks hooks()
            {
                Ctl::Hooks h;
                h.cardBusy = [this]() { calls.push_back("busy?"); return busy ? std::string("the game is writing to the memory card.") : std::string(); };
                h.plan = [this](hl::Plan &plan, std::string &why) {
                    calls.push_back("plan");
                    if (!planOk)
                    {
                        why = planWhy;
                        return false;
                    }
                    plan.save = planSave;
                    plan.useProfile = true;
                    plan.line = "Memory card: profile card with 1 data pack on (your own card is not changed).";
                    return true;
                };
                h.switchCard = [this](const hl::Plan &) { calls.push_back("switch"); };
                h.commit = [this](const hl::Plan &) { calls.push_back("commit"); };
                return h;
            }
            // guest memory equal to planSave
            std::vector<uint8_t> roster, slots;
            hl::GuestView guest(bool wrong = false)
            {
                roster.assign(planSave.begin() + hl::kRosterOffset, planSave.begin() + hl::kRosterOffset + hl::kRosterSize);
                slots.assign(hl::kSlotCount * hl::kSlotSize, 0);
                if (wrong)
                    roster[10] ^= 0xFF;
                hl::GuestView v;
                v.roster = roster.data();
                v.rosterSize = roster.size();
                v.slots = slots.data();
                v.slotsSize = slots.size();
                return v;
            }
            // the screen was a safe point 10 ms before the overlay opened
            void atSafePoint(uint64_t now = 1'000'000'000ull, bool unsaved = false)
            {
                ctl.noteSafePoint(hl::Where::Menu, unsaved, now - 10 * ms);
                ctl.guestStopped(now);
            }
        };

        tc.Run("nothing to load: no button, and a request is refused", [=](TestCase &t)
        {
            Fixture f;
            f.atSafePoint();
            const Ctl::Offer o = f.ctl.offer(false);
            t.IsFalse(o.shown, "not shown");
            t.IsTrue(o.note.empty(), "no line");
            const auto r = f.ctl.request(false, 1000 * ms);
            t.IsFalse(r.ok, "refused");
            t.IsTrue(contains(r.message, "nothing to load"), r.message);
            t.IsFalse(f.ctl.requested(), "no request is waiting");
        });

        tc.Run("the button follows the game: waiting, not at a safe point, stale, fresh, after the menu closed", [=](TestCase &t)
        {
            Fixture f;
            Ctl::Offer o = f.ctl.offer(true);
            t.IsTrue(o.shown && !o.enabled, "shown but dimmed before the game has stopped");
            t.Equals(o.help, std::string("Waiting for the game to pause."), "and it says why");
            f.ctl.guestStopped(1000 * ms); // stopped with no safe point ever stamped
            o = f.ctl.offer(true);
            t.IsTrue(o.shown && !o.enabled, "dimmed with no safe point");
            t.Equals(o.help, std::string("Go to the title screen or the main menu."), "and it says where to go");
            f.ctl.noteSafePoint(hl::Where::Title, false, 1000 * ms - hl::kFreshNs - 1);
            f.ctl.guestStopped(1000 * ms);
            t.IsFalse(f.ctl.offer(true).enabled, "a stamp older than the limit does not count");
            f.ctl.noteSafePoint(hl::Where::Title, false, 1000 * ms - hl::kFreshNs);
            f.ctl.guestStopped(1000 * ms);
            o = f.ctl.offer(true);
            t.IsTrue(o.shown && o.enabled, "a stamp exactly at the limit counts");
            t.IsFalse(o.unsaved, "no unsaved changes");
            t.IsTrue(contains(o.question, "since you last saved"), o.question);
            f.ctl.guestResumed();
            t.IsFalse(f.ctl.offer(true).enabled, "when the menu has closed the decision is void");
        });

        tc.Run("unsaved changes in the game make the question stronger", [=](TestCase &t)
        {
            Fixture f;
            f.atSafePoint(1000 * ms, true);
            const Ctl::Offer o = f.ctl.offer(true);
            t.IsTrue(o.enabled && o.unsaved, "enabled, and the flag is carried");
            t.IsTrue(contains(o.question, "have not saved yet"), o.question);
        });

        tc.Run("a request needs a pending change, a stopped game and a safe point; one request at a time", [=](TestCase &t)
        {
            Fixture f;
            t.IsFalse(f.ctl.request(true, 1000 * ms).ok, "game not stopped yet");
            f.ctl.guestStopped(1000 * ms);
            auto r = f.ctl.request(true, 1000 * ms);
            t.IsFalse(r.ok, "not at a safe point");
            t.IsTrue(contains(r.message, "title screen or the main menu"), r.message);
            f.atSafePoint();
            r = f.ctl.request(true, 1000 * ms);
            t.IsTrue(r.ok, r.message);
            t.IsTrue(f.ctl.requested() && f.ctl.state() == Ctl::State::Requested, "the request is waiting");
            const Ctl::Offer o = f.ctl.offer(true);
            t.IsFalse(o.shown, "the button is hidden while it waits");
            t.IsTrue(contains(o.note, "Waiting for the game"), o.note);
            t.IsFalse(f.ctl.request(true, 1001 * ms).ok, "a second request is refused");
        });

        tc.Run("begin does nothing when no request is waiting", [=](TestCase &t)
        {
            Fixture f;
            f.atSafePoint();
            t.IsFalse(f.ctl.begin(hl::Where::Menu, 1000 * ms, f.hooks()), "no request");
            t.IsTrue(f.calls.empty(), "no step ran");
        });

        tc.Run("the steps run in order, and the card is switched only when the loader will start", [=](TestCase &t)
        {
            Fixture f;
            f.atSafePoint();
            t.IsTrue(f.ctl.request(true, 1000 * ms).ok, "requested");
            t.IsTrue(f.ctl.begin(hl::Where::Menu, 1020 * ms, f.hooks()), "go");
            t.Equals(f.calls, (std::vector<std::string>{"busy?", "plan", "switch"}), "card checked, plan built and checked, then the switch");
            t.IsTrue(f.ctl.state() == Ctl::State::Loading, "loading");
            t.IsFalse(f.ctl.requested(), "the request is taken");
            hl::GuestView g = f.guest();
            f.ctl.end(g, f.hooks());
            t.Equals(f.calls.back(), std::string("commit"), "committed after the check");
            t.IsTrue(f.ctl.state() == Ctl::State::Loaded, "loaded");
            t.Equals(f.ctl.message(), std::string("Loaded."), "the tab says Loaded");
            std::string shown;
            t.IsFalse(f.ctl.takeResult(shown), "a success is not announced by reopening the menu");
            const Ctl::Offer o = f.ctl.offer(false);
            t.Equals(o.note, std::string("Loaded."), "the line after the load");
            t.IsTrue(f.ctl.whereLoaded() == hl::Where::Menu, "and where it ran");
        });

        tc.Run("a card file open for writing refuses before anything is built or switched", [=](TestCase &t)
        {
            Fixture f;
            f.busy = true;
            f.atSafePoint();
            f.ctl.request(true, 1000 * ms);
            t.IsFalse(f.ctl.begin(hl::Where::Title, 1010 * ms, f.hooks()), "refused");
            t.Equals(f.calls, (std::vector<std::string>{"busy?"}), "only the card was looked at");
            t.IsTrue(f.ctl.state() == Ctl::State::Refused, "refused");
            std::string shown;
            t.IsTrue(f.ctl.takeResult(shown) && contains(shown, "writing to the memory card"), "the player is told: " + shown);
            t.IsFalse(f.ctl.takeResult(shown), "once");
        });

        tc.Run("a plan that does not pass is refused with its reason; the card is not switched", [=](TestCase &t)
        {
            Fixture f;
            f.planOk = false;
            f.planWhy = "The save on the profile card does not pass the game's own checks, so the game would not load it.";
            f.atSafePoint();
            f.ctl.request(true, 1000 * ms);
            t.IsFalse(f.ctl.begin(hl::Where::Menu, 1010 * ms, f.hooks()), "refused");
            t.Equals(f.calls, (std::vector<std::string>{"busy?", "plan"}), "no switch");
            std::string shown;
            t.IsTrue(f.ctl.takeResult(shown), "told");
            t.IsTrue(contains(shown, "Load Now did not start") && contains(shown, "does not pass the game's own checks"), shown);
            t.IsTrue(oneLine(shown), "one line");
            // the button comes back for another try while the change is pending
            const Ctl::Offer o = f.ctl.offer(true);
            t.IsTrue(o.shown && contains(o.note, "did not start"), "the tab keeps the reason: " + o.note);
        });

        tc.Run("a memory that does not match the card fails and the session is not moved", [=](TestCase &t)
        {
            Fixture f;
            f.atSafePoint();
            f.ctl.request(true, 1000 * ms);
            t.IsTrue(f.ctl.begin(hl::Where::Menu, 1010 * ms, f.hooks()), "go");
            hl::GuestView g = f.guest(true);
            f.ctl.end(g, f.hooks());
            t.IsTrue(f.ctl.state() == Ctl::State::Failed, "failed");
            t.IsTrue(std::find(f.calls.begin(), f.calls.end(), "commit") == f.calls.end(), "no commit");
            std::string shown;
            t.IsTrue(f.ctl.takeResult(shown) && contains(shown, "Restart the game"), shown);
        });

        tc.Run("a request that waits too long is refused, by the game thread or the host's timer", [=](TestCase &t)
        {
            {
                Fixture f;
                f.atSafePoint();
                f.ctl.request(true, 1000 * ms);
                t.IsFalse(f.ctl.expire(1000 * ms + hl::kRequestTimeoutNs), "not yet at the limit");
                t.IsTrue(f.ctl.requested(), "still waiting");
                t.IsTrue(f.ctl.expire(1000 * ms + hl::kRequestTimeoutNs + 1), "past it");
                t.IsTrue(f.ctl.state() == Ctl::State::Refused && !f.ctl.requested(), "refused");
                std::string shown;
                t.IsTrue(f.ctl.takeResult(shown) && contains(shown, "not at the title screen or the main menu"), shown);
            }
            {
                Fixture f;
                f.atSafePoint();
                f.ctl.request(true, 1000 * ms);
                t.IsFalse(f.ctl.begin(hl::Where::Menu, 1000 * ms + hl::kRequestTimeoutNs + 5, f.hooks()), "a late safe point does not start it");
                t.IsTrue(f.calls.empty(), "and nothing is built");
                t.IsTrue(f.ctl.state() == Ctl::State::Refused, "refused");
            }
        });

        tc.Run("after a refusal the player can ask again", [=](TestCase &t)
        {
            Fixture f;
            f.busy = true;
            f.atSafePoint();
            f.ctl.request(true, 1000 * ms);
            f.ctl.begin(hl::Where::Menu, 1010 * ms, f.hooks());
            f.busy = false;
            f.atSafePoint(2000 * ms);
            t.IsTrue(f.ctl.request(true, 2000 * ms).ok, "asks again");
            f.calls.clear();
            t.IsTrue(f.ctl.begin(hl::Where::Menu, 2010 * ms, f.hooks()), "goes this time");
        });

        tc.Run("the safe-point stamp costs stores only", [=](TestCase &t)
        {
            Fixture f;
            for (int i = 0; i < 100000; ++i)
                f.ctl.noteSafePoint(hl::Where::Menu, false, static_cast<uint64_t>(i));
            t.IsFalse(f.ctl.requested(), "nothing is requested");
            t.IsTrue(f.calls.empty(), "no step ran");
        });
    });

    MiniTest::Case("HotLoadPlan", [](TestCase &tc)
    {
        namespace hl = ps2x::mods::hotload;
        namespace fpr = ps2x::mods::fpr;
        namespace pf = ps2x::mods::profile;

        // The same standard CRC table the other data pack tests use: the algorithm is the game's, the table is made up.
        static const fpr::CrcTable table = []() {
            fpr::CrcTable t{};
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i << 24;
                for (int k = 0; k < 8; ++k)
                    c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
                t[i] = c;
            }
            return t;
        }();
        auto putText = [](std::vector<uint8_t> &img, size_t at, const std::string &ascii, size_t width) {
            std::vector<uint32_t> points(ascii.begin(), ascii.end());
            std::vector<uint8_t> bytes;
            fpr::encodeGameText(points, bytes);
            bytes.resize(width, 0);
            std::copy(bytes.begin(), bytes.end(), img.begin() + static_cast<std::ptrdiff_t>(at));
        };
        // A small valid save: every built-in wrestler has a first name "FirstN".
        auto makeSave = [=](const std::string &tag) {
            std::vector<uint8_t> img(fpr::kSaveSize, 0);
            for (int i = 0; i < 4; ++i)
            {
                img[fpr::kSettingsMarkerOff + static_cast<size_t>(i)] = static_cast<uint8_t>(fpr::kSettingsMarker >> (8 * i));
                img[fpr::kRosterMarkerOff + static_cast<size_t>(i)] = static_cast<uint8_t>(fpr::kRosterMarker >> (8 * i));
            }
            for (int k = 0; k < fpr::kWrestlers; ++k)
            {
                const size_t base = fpr::kOffNames + static_cast<size_t>(k) * fpr::kNameStride;
                putText(img, base + 1, "First" + std::to_string(k), fpr::kWidthFirst);
                putText(img, base + 15, "Last" + std::to_string(k), fpr::kWidthLast);
                putText(img, base + 33, "Nick" + std::to_string(k), fpr::kWidthNickname);
            }
            img[0x1234] = static_cast<uint8_t>(tag.size()); // something the pack does not own, to tell cards apart
            fpr::fixIntegrity(img, table);
            return img;
        };
        auto write = [](const fs::path &path, const std::vector<uint8_t> &bytes) {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        };
        auto readBack = [](const fs::path &path) {
            std::ifstream in(path, std::ios::binary);
            std::ostringstream out;
            out << in.rdbuf();
            const std::string s = out.str();
            return std::vector<uint8_t>(s.begin(), s.end());
        };
        const std::string renameJson = "{\"format\": \"fpr-rename\", \"version\": 1, \"wrestlers\": {\"2\": {\"first\": \"Zed\"}}}";

        struct World
        {
            fs::path root, base0, base1, profile;
            Env env;
            explicit World(const char *name) : env(name)
            {
                root = env.root;
                base0 = root / "mc0";
                base1 = root / "mc1";
                profile = root / "profile";
            }
            fs::path baseSave() const { return base0 / fpr::kSaveName / fpr::kSaveName; }
            fs::path profileSave() const { return profile / "mc0" / fpr::kSaveName / fpr::kSaveName; }
        };
        auto input = [=](World &w) {
            hl::PlanInput in;
            in.manager = &w.env.manager;
            in.profileDir = w.profile;
            in.baseCard0 = w.base0;
            in.baseCard1 = w.base1;
            Manager *m = &w.env.manager;
            in.readPack = [m](const std::string &id) {
                pf::PackInput p;
                p.id = id;
                if (auto info = m->find(id))
                {
                    p.name = info->name;
                    p.version = info->version;
                }
                p.hasFile = m->hasFile(id, "data/renames.json");
                if (p.hasFile)
                    if (auto bytes = m->readFile(id, "data/renames.json"))
                        p.renamesJson.assign(bytes->begin(), bytes->end());
                return p;
            };
            in.loadCrcTable = [](fpr::CrcTable &out, std::string *) {
                out = table;
                return true;
            };
            return in;
        };
        auto firstNameOf = [](const std::vector<uint8_t> &save, int slot) {
            const size_t at = fpr::kOffNames + static_cast<size_t>(slot) * fpr::kNameStride + 1;
            bool isHex = false;
            return fpr::fieldTextOrHex(save.data() + at, fpr::kWidthFirst, &isHex);
        };

        tc.Run("no data pack and no profile: the plan is the player's own card, untouched", [=](TestCase &t)
        {
            World w("plan_base");
            t.IsTrue(w.env.start(), "starts");
            write(w.baseSave(), makeSave("base"));
            const auto before = readBack(w.baseSave());
            const hl::PlanResult r = hl::makePlan(input(w));
            t.IsTrue(r.ok, r.refusal);
            t.IsFalse(r.plan.useProfile, "the own card");
            t.IsTrue(r.plan.root0 == w.base0, "its root");
            t.IsTrue(r.plan.save == before, "the save it will load");
            t.IsFalse(fs::exists(w.profile), "no profile folder was made");
            t.IsTrue(readBack(w.baseSave()) == before, "the card is unchanged");
        });

        tc.Run("a data pack that is on: the profile is built from the pack and is the plan", [=](TestCase &t)
        {
            World w("plan_pack");
            writeMod(w.env.mods, "ren.fprmod", tomlFor("ren.one", "", "data"), {{"data/renames.json", renameJson}});
            t.IsTrue(w.env.start(), "starts");
            write(w.baseSave(), makeSave("base"));
            t.IsTrue(w.env.manager.setEnabled("ren.one", true).ok, "on");
            const hl::PlanResult r = hl::makePlan(input(w));
            t.IsTrue(r.ok, r.refusal);
            t.IsTrue(r.plan.useProfile && r.plan.root0 == w.profile / "mc0", "the profile card");
            t.Equals(firstNameOf(r.plan.save, 2), std::string("Zed"), "the pack's name is in the save the game will load");
            t.Equals(firstNameOf(r.plan.save, 3), std::string("First3"), "others are as they were");
            t.IsTrue(r.plan.save == readBack(w.profileSave()), "and it is the profile's file");
            t.IsTrue(fpr::checkIntegrity(r.plan.save, table).allOk(), "that passes the game's checks");
            t.Equals(firstNameOf(readBack(w.baseSave()), 2), std::string("First2"), "the player's own card is not changed");
            t.Equals(r.plan.data.ids, (std::vector<std::string>{"ren.one"}), "the plan says which packs it was built from");
            t.Equals(r.plan.packs.size(), static_cast<size_t>(1), "a report per pack");
            t.Equals(r.plan.packs[0].written, static_cast<size_t>(1), "one field written");
        });

        tc.Run("turning the pack off: the next plan reverts the field and stays on the profile", [=](TestCase &t)
        {
            World w("plan_off");
            writeMod(w.env.mods, "ren.fprmod", tomlFor("ren.one", "", "data"), {{"data/renames.json", renameJson}});
            t.IsTrue(w.env.start(), "starts");
            write(w.baseSave(), makeSave("base"));
            w.env.manager.setEnabled("ren.one", true);
            t.IsTrue(hl::makePlan(input(w)).ok, "built with the pack");
            w.env.manager.setEnabled("ren.one", false);
            const hl::PlanResult r = hl::makePlan(input(w));
            t.IsTrue(r.ok, r.refusal);
            t.IsTrue(r.plan.useProfile, "the game stays on the profile card");
            t.Equals(firstNameOf(r.plan.save, 2), std::string("First2"), "the name is back");
            t.IsTrue(r.plan.data.ids.empty(), "no packs");
        });

        tc.Run("Back to my card: the plan is the own card; with no save there it is refused", [=](TestCase &t)
        {
            World w("plan_back");
            writeMod(w.env.mods, "ren.fprmod", tomlFor("ren.one", "", "data"), {{"data/renames.json", renameJson}});
            t.IsTrue(w.env.start(), "starts");
            write(w.baseSave(), makeSave("base"));
            w.env.manager.setEnabled("ren.one", true);
            t.IsTrue(hl::makePlan(input(w)).ok, "profile built");
            w.env.manager.setEnabled("ren.one", false);
            std::string said;
            t.IsTrue(pf::recordOwnCard(w.profile, &said), said);
            const std::vector<uint8_t> profileBefore = readBack(w.profileSave());
            hl::PlanResult r = hl::makePlan(input(w));
            t.IsTrue(r.ok, r.refusal);
            t.IsFalse(r.plan.useProfile, "the own card");
            t.IsTrue(r.plan.root0 == w.base0, "its root");
            t.IsTrue(readBack(w.profileSave()) == profileBefore, "the profile was not touched");
            fs::remove(w.baseSave());
            r = hl::makePlan(input(w));
            t.IsFalse(r.ok, "no save on the own card");
            t.IsTrue(contains(r.refusal, "Your own memory card has no save"), r.refusal);
            write(w.baseSave(), std::vector<uint8_t>(1000, 7));
            r = hl::makePlan(input(w));
            t.IsFalse(r.ok, "a save of the wrong size");
            std::vector<uint8_t> bad = makeSave("base");
            bad[0x3000] ^= 1;
            write(w.baseSave(), bad);
            r = hl::makePlan(input(w));
            t.IsFalse(r.ok, "a save that fails the game's checks");
            t.IsTrue(contains(r.refusal, "does not pass the game's own checks"), r.refusal);
        });

        tc.Run("a pack on with no save anywhere: refused, nothing for the game to load", [=](TestCase &t)
        {
            World w("plan_nosave");
            writeMod(w.env.mods, "ren.fprmod", tomlFor("ren.one", "", "data"), {{"data/renames.json", renameJson}});
            t.IsTrue(w.env.start(), "starts");
            w.env.manager.setEnabled("ren.one", true);
            const hl::PlanResult r = hl::makePlan(input(w));
            t.IsFalse(r.ok, "refused");
            t.IsTrue(contains(r.refusal, "no save to load yet"), r.refusal);
            t.IsTrue(oneLine(r.refusal), "one line");
        });

        tc.Run("a damaged profile save is refused before the game sees it, and left as it is", [=](TestCase &t)
        {
            World w("plan_corrupt");
            writeMod(w.env.mods, "ren.fprmod", tomlFor("ren.one", "", "data"), {{"data/renames.json", renameJson}});
            t.IsTrue(w.env.start(), "starts");
            write(w.baseSave(), makeSave("base"));
            w.env.manager.setEnabled("ren.one", true);
            t.IsTrue(hl::makePlan(input(w)).ok, "built");
            std::vector<uint8_t> damaged = readBack(w.profileSave());
            damaged[0x5000] ^= 0x10; // a data byte: the block checksum fails
            write(w.profileSave(), damaged);
            const hl::PlanResult r = hl::makePlan(input(w));
            t.IsFalse(r.ok, "refused");
            t.IsTrue(contains(r.refusal, "does not pass the game's own checks"), r.refusal);
            t.IsTrue(readBack(w.profileSave()) == damaged, "the damaged file was not rewritten");
            write(w.profileSave(), std::vector<uint8_t>(500, 1));
            const hl::PlanResult small = hl::makePlan(input(w));
            t.IsFalse(small.ok, "a file of the wrong size");
            t.IsTrue(contains(small.refusal, "not a Fire Pro Wrestling Returns save"), small.refusal);
        });

        tc.Run("the checksum table missing: refused", [=](TestCase &t)
        {
            World w("plan_notable");
            t.IsTrue(w.env.start(), "starts");
            write(w.baseSave(), makeSave("base"));
            hl::PlanInput in = input(w);
            in.loadCrcTable = [](fpr::CrcTable &, std::string *why) {
                if (why)
                    *why = "no executable";
                return false;
            };
            const hl::PlanResult r = hl::makePlan(in);
            t.IsFalse(r.ok, "refused");
            t.IsTrue(contains(r.refusal, "checksum table could not be read (no executable)"), r.refusal);
        });

        tc.Run("a pack file that cannot be used is reported on the pack, and the load still goes ahead with the rest", [=](TestCase &t)
        {
            World w("plan_badpack");
            writeMod(w.env.mods, "bad.fprmod", tomlFor("bad.one", "", "data"), {{"data/renames.json", "{\"format\": \"other\"}"}});
            writeMod(w.env.mods, "ren.fprmod", tomlFor("ren.one", "", "data"), {{"data/renames.json", renameJson}});
            t.IsTrue(w.env.start(), "starts");
            write(w.baseSave(), makeSave("base"));
            w.env.manager.setEnabled("bad.one", true);
            w.env.manager.setEnabled("ren.one", true);
            const hl::PlanResult r = hl::makePlan(input(w));
            t.IsTrue(r.ok, r.refusal);
            t.Equals(r.plan.packs.size(), static_cast<size_t>(2), "two reports");
            size_t withError = 0;
            for (const auto &p : r.plan.packs)
                withError += p.error.empty() ? 0 : 1;
            t.Equals(withError, static_cast<size_t>(1), "one pack is not used, with its reason");
            t.Equals(firstNameOf(r.plan.save, 2), std::string("Zed"), "the other applies");
        });

        tc.Run("a full pass: plan, switch, load, check, commit moves the manager's session", [=](TestCase &t)
        {
            World w("plan_full");
            writeMod(w.env.mods, "ren.fprmod", tomlFor("ren.one", "", "data"), {{"data/renames.json", renameJson}});
            t.IsTrue(w.env.start(), "starts");
            write(w.baseSave(), makeSave("base"));
            w.env.manager.setEnabled("ren.one", true);
            t.IsTrue(w.env.manager.dataChangesPending(), "pending before");
            hl::Controller ctl;
            ctl.noteSafePoint(hl::Where::Title, false, 90);
            ctl.guestStopped(100);
            t.IsTrue(ctl.request(true, 100).ok, "requested");
            fs::path root;
            hl::Controller::Hooks hooks;
            hooks.cardBusy = []() { return std::string(); };
            hooks.plan = [&](hl::Plan &plan, std::string &why) {
                hl::PlanResult r = hl::makePlan(input(w));
                if (!r.ok)
                {
                    why = r.refusal;
                    return false;
                }
                plan = std::move(r.plan);
                return true;
            };
            hooks.switchCard = [&](const hl::Plan &plan) { root = plan.root0; };
            hooks.commit = [&](const hl::Plan &plan) { w.env.manager.commitDataSession(plan.data); };
            t.IsTrue(ctl.begin(hl::Where::Title, 110, hooks), "go");
            t.IsTrue(root == w.profile / "mc0", "the card root is the profile's");
            // the "game" loads the profile's save: its memory is those areas of the file
            const auto save = readBack(w.profileSave());
            std::vector<uint8_t> roster(save.begin() + hl::kRosterOffset, save.begin() + hl::kRosterOffset + hl::kRosterSize);
            std::vector<uint8_t> slots(save.begin() + hl::kSlotsOffset, save.begin() + hl::kSlotsOffset + hl::kSlotCount * hl::kSlotSize);
            slots[3 * hl::kSlotSize + 17] = 0x5A; // a leftover in an unused slot
            hl::GuestView guest{roster.data(), roster.size(), slots.data(), slots.size()};
            ctl.end(guest, hooks);
            t.IsTrue(ctl.state() == hl::Controller::State::Loaded, ctl.message());
            t.Equals(ctl.scrubbedSlots(), static_cast<size_t>(1), "the leftover was cleaned");
            t.IsFalse(w.env.manager.dataChangesPending(), "the data change is applied");
            t.IsFalse(w.env.manager.restartNeeded(), "and no restart is needed for it");
        });
    });

    MiniTest::Case("IoPathsSwitch", [](TestCase &tc)
    {
        // Load Now switches the memory card root while the game runs; the F1 debug panel reads the paths on another thread (task h1).
        tc.Run("a reference from getIoPaths() stays what it was, and a reader on another thread never sees a torn or freed path", [](TestCase &t)
        {
            const PS2Runtime::IoPaths original = PS2Runtime::getIoPaths();
            const PS2Runtime::IoPaths &held = PS2Runtime::getIoPaths();
            const std::string heldBefore = held.mcRoot.string();

            std::atomic<bool> stop{false};
            std::atomic<size_t> reads{0}, bad{0};
            const std::string originalRoot = original.mcRoot.string();
            std::thread reader([&]() {
                while (!stop.load())
                {
                    const PS2Runtime::IoPaths &now = PS2Runtime::getIoPaths();
                    const std::string root = now.mcRoot.string();
                    const std::string folder = now.elfDirectory.string();
                    const bool known = root == originalRoot || root.find("h1_root_a") != std::string::npos || root.find("h1_root_b") != std::string::npos;
                    if (!known || folder != original.elfDirectory.string())
                        bad.fetch_add(1);
                    reads.fetch_add(1);
                }
            });
            for (int i = 0; i < 20000; ++i)
            {
                PS2Runtime::IoPaths next = original;
                next.mcRoot = fs::temp_directory_path() / (i % 2 ? "h1_root_a" : "h1_root_b") / "mc0";
                PS2Runtime::setIoPaths(next);
            }
            stop.store(true);
            reader.join();
            t.Equals(bad.load(), static_cast<size_t>(0), "the reader never saw a path that was not one of the values set");
            t.IsTrue(reads.load() > 0, "the reader ran");
            t.Equals(held.mcRoot.string(), heldBefore, "the reference taken before the switches still holds the old value");
            t.IsTrue(PS2Runtime::getIoPaths().mcRoot.string().find("h1_root_") != std::string::npos, "the newest value is what getIoPaths() returns now");
            PS2Runtime::setIoPaths(original);
            t.Equals(PS2Runtime::getIoPaths().mcRoot.string(), originalRoot, "and it is put back");
        });
    });
}
