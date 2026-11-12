#include "MiniTest.h"
#include "ps2_settings.h"
#include "runtime/ps2_pause.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

// Tests for the settings file and its meeting with the environment (ps2_settings.h) and for the guest pause
// (runtime/ps2_pause.h). The overlay itself is checked by running the program (tools and the report for task u2).

namespace
{
    namespace fs = std::filesystem;

    void setEnvVar(const char *name, const char *value)
    {
#if defined(_WIN32)
        _putenv_s(name, value ? value : "");
#else
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
#endif
    }

    // Unsets every variable a setting uses, so a test starts from the same state wherever it runs.
    void clearSettingVariables()
    {
        for (const ps2x_settings::Definition &def : ps2x_settings::definitions())
        {
#if defined(_WIN32)
            _putenv_s(def.envName, ""); // an empty value removes the variable on Windows
#else
            unsetenv(def.envName);
#endif
        }
    }

    std::string readAll(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    fs::path tempDirectory(const char *name)
    {
        fs::path dir = fs::temp_directory_path() / (std::string("ps2x_settings_test_") + name);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        return dir;
    }
}

void register_ui_settings_tests()
{
    MiniTest::Case("UiSettings", [](TestCase &tc)
    {
        tc.Run("a file is parsed, unknown keys and comments are skipped, a later line wins", [](TestCase &t)
        {
            const auto entries = ps2x_settings::parseFile("# comment\n\nrenderer = cpu\n  Resolution=3  \nbogus = 1\nno equals sign\nrenderer = gpu\n");
            t.Equals(entries.size(), static_cast<size_t>(2), "two known settings");
            t.Equals(entries[0].key, std::string("renderer"), "first key");
            t.Equals(entries[0].value, std::string("gpu"), "the later line won");
            t.Equals(entries[1].key, std::string("resolution"), "keys are lower-cased");
            t.Equals(entries[1].value, std::string("3"), "values are trimmed");
        });

        tc.Run("formatFile output parses back to the same entries", [](TestCase &t)
        {
            const std::vector<ps2x_settings::Entry> in = {{"renderer", "cpu"}, {"aspect", "16:9"}, {"volume", "35"}};
            const auto out = ps2x_settings::parseFile(ps2x_settings::formatFile(in));
            t.Equals(out.size(), in.size(), "same count");
            for (size_t i = 0; i < std::min(in.size(), out.size()); ++i)
            {
                t.Equals(out[i].key, in[i].key, "key");
                t.Equals(out[i].value, in[i].value, "value");
            }
        });

        tc.Run("a value becomes an environment variable text and back; defaults set nothing", [](TestCase &t)
        {
            using namespace ps2x_settings;
            t.IsFalse(environmentFromValue("renderer", "gpu").has_value(), "renderer default sets nothing");
            t.Equals(environmentFromValue("renderer", "cpu").value_or(""), std::string("cpu"), "renderer cpu");
            t.IsFalse(environmentFromValue("resolution", "1").has_value(), "resolution default");
            t.Equals(environmentFromValue("resolution", "3").value_or(""), std::string("3"), "resolution 3");
            t.IsFalse(environmentFromValue("aspect", "4:3").has_value(), "aspect default");
            t.Equals(environmentFromValue("aspect", "21:9").value_or(""), std::string("21:9"), "aspect 21:9");
            t.Equals(environmentFromValue("fullscreen", "on").value_or(""), std::string("1"), "fullscreen on");
            t.Equals(environmentFromValue("vsync", "on").value_or(""), std::string("vsync"), "vsync on");
            t.Equals(environmentFromValue("filter", "linear").value_or(""), std::string("bilinear"), "filter linear");
            t.IsFalse(environmentFromValue("volume", "100").has_value(), "volume default");
            t.Equals(environmentFromValue("volume", "40").value_or(""), std::string("40"), "volume 40");
            t.IsFalse(environmentFromValue("volume", "loud").has_value(), "nonsense is refused");

            t.Equals(valueFromEnvironment("aspect", "16:9").value_or(""), std::string("16:9"), "16:9 from the environment");
            t.Equals(valueFromEnvironment("aspect", "1920:1080").value_or(""), std::string("16:9"), "1920:1080 is 16:9");
            t.Equals(valueFromEnvironment("aspect", "off").value_or(""), std::string("4:3"), "off is 4:3");
            t.Equals(valueFromEnvironment("aspect", "32:9").value_or(""), std::string("32:9"), "another shape is kept");
            t.Equals(valueFromEnvironment("vsync", "mailbox").value_or(""), std::string("off"), "mailbox is not vsync");
            t.Equals(valueFromEnvironment("vsync", "vsync").value_or(""), std::string("on"), "vsync");
            t.Equals(valueFromEnvironment("filter", "nearest").value_or(""), std::string("nearest"), "nearest");
            t.Equals(valueFromEnvironment("filter", "bilinear").value_or(""), std::string("linear"), "bilinear is linear");
            t.Equals(valueFromEnvironment("volume", "250").value_or(""), std::string("100"), "volume is clamped");

            // the modder's tools (task t2): off is the default and sets nothing, on is PS2X_AUTHORING=1
            t.IsFalse(environmentFromValue("texture_tools", "off").has_value(), "texture tools default");
            t.Equals(environmentFromValue("texture_tools", "on").value_or(""), std::string("1"), "texture tools on");
            t.IsFalse(environmentFromValue("texture_tools", "maybe").has_value(), "texture tools nonsense is refused");
            t.Equals(valueFromEnvironment("texture_tools", "1").value_or(""), std::string("on"), "PS2X_AUTHORING=1 is on");
            t.Equals(valueFromEnvironment("texture_tools", "0").value_or(""), std::string("off"), "PS2X_AUTHORING=0 is off");
            const ps2x_settings::Definition *tools = ps2x_settings::find("texture_tools");
            t.IsTrue(tools && std::string(tools->envName) == "PS2X_AUTHORING", "the setting is fixed by PS2X_AUTHORING");

            // skip the logos and the opening movie (task i1): off is the default and sets nothing, on is PS2X_SKIP_INTROS=1
            t.IsFalse(environmentFromValue("skip_intros", "off").has_value(), "skip intros default");
            t.Equals(environmentFromValue("skip_intros", "on").value_or(""), std::string("1"), "skip intros on");
            t.IsFalse(environmentFromValue("skip_intros", "maybe").has_value(), "skip intros nonsense is refused");
            t.Equals(valueFromEnvironment("skip_intros", "1").value_or(""), std::string("on"), "PS2X_SKIP_INTROS=1 is on");
            t.Equals(valueFromEnvironment("skip_intros", "0").value_or(""), std::string("off"), "PS2X_SKIP_INTROS=0 is off");
            t.IsFalse(valueFromEnvironment("skip_intros", "soon").has_value(), "PS2X_SKIP_INTROS nonsense means nothing");
            const ps2x_settings::Definition *intros = ps2x_settings::find("skip_intros");
            t.IsTrue(intros && std::string(intros->envName) == "PS2X_SKIP_INTROS" && std::string(intros->defaultValue) == "off",
                     "the setting is fixed by PS2X_SKIP_INTROS and is off by default");

            // strike depth (task s1): original is the default and sets nothing; check exists only as an environment value
            t.IsFalse(environmentFromValue("strike_depth", "original").has_value(), "strike depth default");
            t.Equals(environmentFromValue("strike_depth", "Wide").value_or(""), std::string("wide"), "strike depth wide");
            t.Equals(environmentFromValue("strike_depth", "wider").value_or(""), std::string("wider"), "strike depth wider");
            t.IsFalse(environmentFromValue("strike_depth", "check").has_value(), "the file cannot ask for check");
            t.Equals(valueFromEnvironment("strike_depth", "check").value_or(""), std::string("check"), "PS2X_STRIKE_DEPTH=check");
            t.Equals(valueFromEnvironment("strike_depth", "0").value_or(""), std::string("original"), "PS2X_STRIKE_DEPTH=0 is original");
            t.IsFalse(valueFromEnvironment("strike_depth", "huge").has_value(), "PS2X_STRIKE_DEPTH nonsense means nothing");
            const ps2x_settings::Definition *depth = ps2x_settings::find("strike_depth");
            t.IsTrue(depth && std::string(depth->envName) == "PS2X_STRIKE_DEPTH" && std::string(depth->defaultValue) == "original",
                     "the setting is fixed by PS2X_STRIKE_DEPTH and is original by default");
        });

        tc.Run("the file's values reach the environment, an environment variable wins, and a save round-trips", [](TestCase &t)
        {
            clearSettingVariables();
            ps2x_settings::resetForTest();
            const fs::path dir = tempDirectory("roundtrip");
            const fs::path file = dir / "settings.cfg";
            {
                std::ofstream out(file);
                out << "renderer = gpu\nresolution = 3\naspect = 16:9\nfullscreen = on\nvsync = on\nfilter = linear\nvolume = 40\n";
            }
            setEnvVar("PS2X_GS_SCALE", "2"); // fixed from outside: wins over resolution = 3
            ps2x_settings::initialize(file.string());

            t.Equals(ps2x_settings::get("resolution"), std::string("2"), "the environment variable wins");
            t.IsTrue(ps2x_settings::fixedByEnvironment("resolution"), "it is reported as fixed");
            t.Equals(ps2x_settings::envName("resolution"), std::string("PS2X_GS_SCALE"), "with its name");
            t.Equals(std::string(std::getenv("PS2X_GS_SCALE")), std::string("2"), "and was left alone");
            t.Equals(ps2x_settings::get("aspect"), std::string("16:9"), "aspect from the file");
            t.Equals(std::string(std::getenv("PS2X_GS_WIDE") ? std::getenv("PS2X_GS_WIDE") : ""), std::string("16:9"), "aspect reached the environment");
            t.Equals(std::string(std::getenv("PS2X_VOLUME") ? std::getenv("PS2X_VOLUME") : ""), std::string("40"), "volume reached the environment");
            t.IsFalse(ps2x_settings::fixedByEnvironment("aspect"), "aspect is not fixed");
            t.IsFalse(ps2x_settings::set("resolution", "4"), "a fixed setting cannot be changed");
            t.IsTrue(ps2x_settings::set("aspect", "21:9"), "a free one can");
            t.IsFalse(ps2x_settings::set("aspect", "5:4"), "an invalid value is refused");
            t.Equals(ps2x_settings::get("aspect"), std::string("21:9"), "now 21:9");
            t.Equals(ps2x_settings::startupValue("aspect"), std::string("16:9"), "the startup value did not change");
            t.IsTrue(ps2x_settings::dirty(), "dirty after a change");
            t.IsTrue(ps2x_settings::save(), "saved");
            t.IsFalse(ps2x_settings::dirty(), "clean after saving");

            const auto saved = ps2x_settings::parseFile(readAll(file));
            auto valueIn = [&](const char *key) {
                for (const auto &e : saved)
                    if (e.key == key)
                        return e.value;
                return std::string("(missing)");
            };
            t.Equals(valueIn("aspect"), std::string("21:9"), "the file has the new aspect");
            t.Equals(valueIn("resolution"), std::string("3"), "the file keeps its own resolution, not the variable's");
            t.Equals(valueIn("filter"), std::string("linear"), "other settings kept");
            t.IsFalse(fs::exists(dir / "settings.cfg.tmp"), "no temporary file is left");

            // A second run reads the new file (the variables are cleared first, as a new process would have them).
            clearSettingVariables();
            ps2x_settings::resetForTest();
            ps2x_settings::initialize(file.string());
            t.Equals(ps2x_settings::get("aspect"), std::string("21:9"), "the saved aspect survives a restart");
            t.Equals(ps2x_settings::get("resolution"), std::string("3"), "resolution is the file's again");
            t.IsFalse(ps2x_settings::fixedByEnvironment("resolution"), "and is no longer fixed");

            clearSettingVariables();
            ps2x_settings::resetForTest();
            std::error_code ec;
            fs::remove_all(dir, ec);
        });

        tc.Run("defaults change nothing in the environment, and bad lines are ignored", [](TestCase &t)
        {
            clearSettingVariables();
            ps2x_settings::resetForTest();
            const fs::path dir = tempDirectory("defaults");
            const fs::path file = dir / "settings.cfg";
            {
                std::ofstream out(file);
                out << "renderer = gpu\nresolution = 9\nvolume = loud\nfilter = sharp\n";
            }
            ps2x_settings::initialize(file.string());
            for (const char *name : {"PS2X_GS_BACKEND", "PS2X_GS_SCALE", "PS2X_GS_WIDE", "PS2X_FULLSCREEN", "PS2X_GPU_PRESENT_MODE", "PS2X_SCALE", "PS2X_VOLUME"})
            {
                const char *value = std::getenv(name);
                t.IsTrue(value == nullptr || value[0] == '\0', std::string(name) + " stays unset");
            }
            t.Equals(ps2x_settings::get("resolution"), std::string("1"), "an invalid value falls back to the default");
            t.Equals(ps2x_settings::get("volume"), std::string("100"), "so does a non-number");
            t.IsFalse(ps2x_settings::dirty(), "nothing to save");

            // A missing file is fine, and saving creates the folder.
            ps2x_settings::resetForTest();
            const fs::path nested = dir / "a" / "b" / "settings.cfg";
            ps2x_settings::initialize(nested.string());
            t.IsTrue(ps2x_settings::set("fullscreen", "on"), "set");
            t.IsTrue(ps2x_settings::save(), "saved into a new folder");
            t.IsTrue(fs::exists(nested), "the file exists");

            clearSettingVariables();
            ps2x_settings::resetForTest();
            std::error_code ec;
            fs::remove_all(dir, ec);
        });

        tc.Run("the guest pause blocks until resumed and counts the time it blocked", [](TestCase &t)
        {
            using namespace std::chrono_literals;
            t.IsFalse(ps2x_host::guestPaused(), "not paused at first");
            t.Equals(ps2x_host::waitWhileGuestPaused([] { return false; }), static_cast<uint64_t>(0), "no wait while not paused");
            const uint64_t totalBefore = ps2x_host::guestPausedTotalNs();
            ps2x_host::setGuestPaused(true);
            std::atomic<bool> returned{false};
            uint64_t waitedNs = 0;
            std::thread guest([&] {
                waitedNs = ps2x_host::waitWhileGuestPaused([] { return false; });
                returned.store(true);
            });
            std::this_thread::sleep_for(120ms);
            t.IsFalse(returned.load(), "still blocked after 120 ms");
            ps2x_host::setGuestPaused(false);
            guest.join();
            t.IsTrue(returned.load(), "released");
            t.IsTrue(waitedNs >= 100'000'000ull, "it reports about the time it blocked");
            t.IsTrue(ps2x_host::guestPausedTotalNs() - totalBefore >= 100'000'000ull, "and adds it to the total");
        });

        tc.Run("a stop request releases a paused guest", [](TestCase &t)
        {
            using namespace std::chrono_literals;
            ps2x_host::setGuestPaused(true);
            std::atomic<bool> stop{false};
            std::atomic<bool> returned{false};
            std::thread guest([&] {
                ps2x_host::waitWhileGuestPaused([&] { return stop.load(); });
                returned.store(true);
            });
            std::this_thread::sleep_for(60ms);
            t.IsFalse(returned.load(), "blocked");
            stop.store(true);
            guest.join();
            t.IsTrue(returned.load(), "released by the stop flag within a poll interval");
            ps2x_host::setGuestPaused(false);
        });
    });
}
