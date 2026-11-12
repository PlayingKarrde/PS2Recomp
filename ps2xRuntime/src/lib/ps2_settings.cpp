#include "ps2_settings.h"

#if !defined(PLATFORM_VITA)
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace ps2x_settings
{
    namespace
    {
        const std::vector<Definition> kDefinitions = {
            {"renderer", "PS2X_GS_BACKEND", "gpu"},
            {"resolution", "PS2X_GS_SCALE", "1"},
            {"aspect", "PS2X_GS_WIDE", "4:3"},
            {"fullscreen", "PS2X_FULLSCREEN", "off"},
            {"vsync", "PS2X_GPU_PRESENT_MODE", "off"},
            {"filter", "PS2X_SCALE", "sharp"},
            {"volume", "PS2X_VOLUME", "100"},
            {"texture_tools", "PS2X_AUTHORING", "off"},
            {"skip_intros", "PS2X_SKIP_INTROS", "off"},
            {"strike_depth", "PS2X_STRIKE_DEPTH", "original"},
        };

        // strike_depth (task s1): how deep a strike's hit box is, front to back. "original" leaves the game's own test in place.
        bool strikeDepthValue(const std::string &value)
        {
            return value == "original" || value == "wide" || value == "wider";
        }

        std::string trim(const std::string &text)
        {
            size_t begin = 0;
            size_t end = text.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
                ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
                --end;
            return text.substr(begin, end - begin);
        }

        std::string lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::optional<int> parseInt(const std::string &text)
        {
            if (text.empty())
                return std::nullopt;
            char *end = nullptr;
            const long value = std::strtol(text.c_str(), &end, 10);
            if (end == text.c_str() || *end != '\0')
                return std::nullopt;
            return static_cast<int>(value);
        }

        // The value a file or the overlay may store for a setting, normalised; nullopt for anything else.
        std::optional<std::string> normalize(const std::string &key, const std::string &raw)
        {
            const std::string value = lower(trim(raw));
            if (key == "renderer")
                return (value == "gpu" || value == "cpu") ? std::optional<std::string>(value) : std::nullopt;
            if (key == "resolution")
            {
                const auto n = parseInt(value);
                return (n && *n >= 1 && *n <= 4) ? std::optional<std::string>(std::to_string(*n)) : std::nullopt;
            }
            if (key == "aspect")
                return (value == "4:3" || value == "16:9" || value == "21:9") ? std::optional<std::string>(value) : std::nullopt;
            if (key == "fullscreen" || key == "vsync" || key == "texture_tools" || key == "skip_intros")
            {
                if (value == "on" || value == "1" || value == "true" || value == "yes")
                    return std::string("on");
                if (value == "off" || value == "0" || value == "false" || value == "no")
                    return std::string("off");
                return std::nullopt;
            }
            if (key == "filter")
            {
                if (value == "sharp")
                    return std::string("sharp");
                if (value == "linear" || value == "bilinear")
                    return std::string("linear");
                return std::nullopt;
            }
            if (key == "volume")
            {
                const auto n = parseInt(value);
                return (n && *n >= 0 && *n <= 100) ? std::optional<std::string>(std::to_string(*n)) : std::nullopt;
            }
            if (key == "strike_depth")
                return strikeDepthValue(value) ? std::optional<std::string>(value) : std::nullopt;
            return std::nullopt;
        }

        const char *getEnv(const char *name)
        {
            return std::getenv(name);
        }

        void setEnv(const char *name, const char *value)
        {
#if defined(_WIN32)
            _putenv_s(name, value);
#else
            setenv(name, value, 1);
#endif
        }

        struct Item
        {
            std::string key;
            std::string fileValue; // what the file says (or the default): what save() writes
            std::string current;   // fileValue as changed by set()
            std::string startup;   // the effective value when the program started
            bool envFixed = false;
            std::string envName;
            std::string envText;
        };

        struct Store
        {
            std::mutex mutex;
            bool initialized = false;
            bool dirty = false;
            std::string path;
            std::vector<Item> items;
        };
        Store &store()
        {
            static Store s;
            return s;
        }

        Item *itemFor(Store &s, const std::string &key)
        {
            for (Item &item : s.items)
                if (item.key == key)
                    return &item;
            return nullptr;
        }

        std::string effective(const Item &item)
        {
            if (item.envFixed)
            {
                if (auto fromEnv = valueFromEnvironment(item.key, item.envText))
                    return *fromEnv;
                const Definition *def = find(item.key);
                return def ? def->defaultValue : std::string();
            }
            return item.current;
        }

        void say(const char *format, const std::string &a = std::string(), const std::string &b = std::string())
        {
            std::printf(format, a.c_str(), b.c_str());
            std::printf("\n");
            std::fflush(stdout);
        }
    }

    const std::vector<Definition> &definitions()
    {
        return kDefinitions;
    }

    const Definition *find(const std::string &key)
    {
        for (const Definition &def : kDefinitions)
            if (key == def.key)
                return &def;
        return nullptr;
    }

    std::optional<std::string> valueFromEnvironment(const std::string &key, const std::string &envText)
    {
        const std::string text = lower(trim(envText));
        if (key == "renderer")
            return (text == "gpu" || text == "cpu") ? std::optional<std::string>(text) : std::nullopt;
        if (key == "resolution")
        {
            const auto n = parseInt(text);
            return (n && *n >= 1 && *n <= 4) ? std::optional<std::string>(std::to_string(*n)) : std::nullopt;
        }
        if (key == "aspect")
        {
            if (text.empty() || text == "off" || text == "0")
                return std::string("4:3");
            float w = 0.0f, h = 0.0f;
            if (std::sscanf(text.c_str(), "%f:%f", &w, &h) != 2 || !(w > 0.0f) || !(h > 0.0f))
                return std::nullopt;
            const float ratio = w / h;
            if (std::fabs(ratio - 4.0f / 3.0f) < 0.01f)
                return std::string("4:3");
            if (std::fabs(ratio - 16.0f / 9.0f) < 0.01f)
                return std::string("16:9");
            if (std::fabs(ratio - 21.0f / 9.0f) < 0.01f)
                return std::string("21:9");
            return text; // another shape (32:9): shown as it is
        }
        if (key == "fullscreen" || key == "texture_tools" || key == "skip_intros")
        {
            if (text == "1" || text == "on" || text == "true" || text == "yes")
                return std::string("on");
            if (text == "0" || text == "off" || text == "false" || text == "no" || text.empty())
                return std::string("off");
            return std::nullopt;
        }
        if (key == "vsync")
        {
            if (text == "vsync")
                return std::string("on");
            if (text == "immediate" || text == "mailbox")
                return std::string("off");
            return std::nullopt;
        }
        if (key == "filter")
        {
            if (text == "sharp" || text.empty())
                return std::string("sharp");
            if (text == "bilinear" || text == "linear")
                return std::string("linear");
            if (text == "nearest")
                return std::string("nearest");
            return std::nullopt;
        }
        if (key == "volume")
        {
            const auto n = parseInt(text);
            if (!n)
                return std::nullopt;
            return std::to_string(std::clamp(*n, 0, 100));
        }
        if (key == "strike_depth")
        {
            // "check" is for testing only: the game keeps its own test and the rewrite is compared with it (src/overrides).
            if (text.empty() || text == "0" || text == "off")
                return std::string("original");
            if (strikeDepthValue(text) || text == "check")
                return text;
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<std::string> environmentFromValue(const std::string &key, const std::string &value)
    {
        const auto v = normalize(key, value);
        const Definition *def = find(key);
        if (!v || !def || *v == def->defaultValue)
            return std::nullopt;
        if (key == "fullscreen" || key == "texture_tools" || key == "skip_intros")
            return std::string("1");
        if (key == "vsync")
            return std::string("vsync");
        if (key == "filter")
            return std::string("bilinear"); // linear (sharp is the default and was filtered out above)
        return *v; // renderer: cpu; resolution: 2..4; aspect: 16:9 or 21:9; volume: 0..99; strike_depth: wide or wider
    }

    std::vector<Entry> parseFile(const std::string &text)
    {
        std::vector<Entry> entries;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = lower(trim(line.substr(0, eq)));
            const std::string value = trim(line.substr(eq + 1));
            if (!find(key))
                continue;
            auto existing = std::find_if(entries.begin(), entries.end(), [&](const Entry &e) { return e.key == key; });
            if (existing != entries.end())
                existing->value = value;
            else
                entries.push_back({key, value});
        }
        return entries;
    }

    std::string formatFile(const std::vector<Entry> &entries)
    {
        std::string out = "# Fire Pro Wrestling Returns (recomp) settings. Written by the settings overlay; edit when the game is closed.\n"
                          "# renderer = gpu | cpu          resolution = 1..4         aspect = 4:3 | 16:9 | 21:9\n"
                          "# fullscreen = on | off         vsync = on | off          filter = sharp | linear\n"
                          "# volume = 0..100              texture_tools = on | off (the modder's tools, docs/modding-textures.md)\n"
                          "# skip_intros = on | off      start at the title screen without the logos and the opening movie (next start)\n"
                          "# strike_depth = original | wide | wider   a strike's reach toward or away from the screen (x1, x1.5, x2)\n"
                          "# An environment variable (PS2X_GS_BACKEND, PS2X_GS_SCALE, PS2X_GS_WIDE, PS2X_FULLSCREEN, PS2X_GPU_PRESENT_MODE,\n"
                          "# PS2X_SCALE, PS2X_VOLUME, PS2X_AUTHORING, PS2X_SKIP_INTROS, PS2X_STRIKE_DEPTH) set before the game starts wins over this file.\n";
        for (const Entry &entry : entries)
            out += entry.key + " = " + entry.value + "\n";
        return out;
    }

    std::string defaultPath()
    {
        if (const char *override_ = getEnv("PS2X_SETTINGS"))
            if (override_[0] != '\0')
                return override_;
#if !defined(PLATFORM_VITA)
        char *pref = SDL_GetPrefPath("FPWR-Recomp", "FPWR");
        if (pref)
        {
            std::string path = std::string(pref) + "settings.cfg";
            SDL_free(pref);
            return path;
        }
#endif
        return std::string();
    }

    void initialize(const std::string &path)
    {
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.initialized)
            return;
        s.initialized = true;
        s.path = path;
        s.dirty = false;
        s.items.clear();

        std::vector<Entry> fileEntries;
        bool fileRead = false;
        if (!path.empty())
        {
            std::ifstream in(path, std::ios::binary);
            if (in)
            {
                std::ostringstream buffer;
                buffer << in.rdbuf();
                fileEntries = parseFile(buffer.str());
                fileRead = true;
            }
        }

        std::string applied;
        std::string fixed;
        for (const Definition &def : kDefinitions)
        {
            Item item;
            item.key = def.key;
            item.fileValue = def.defaultValue;
            for (const Entry &entry : fileEntries)
            {
                if (entry.key != item.key)
                    continue;
                if (auto value = normalize(item.key, entry.value))
                    item.fileValue = *value;
                else
                    std::fprintf(stderr, "[settings] %s: '%s' is not a valid value for %s, using %s\n", path.c_str(), entry.value.c_str(),
                                 def.key, def.defaultValue);
            }
            item.current = item.fileValue;

            if (const char *envText = getEnv(def.envName))
            {
                item.envFixed = true;
                item.envName = def.envName;
                item.envText = envText;
                fixed += (fixed.empty() ? "" : ", ") + std::string(def.envName);
            }
            else if (auto envValue = environmentFromValue(item.key, item.fileValue))
            {
                setEnv(def.envName, envValue->c_str());
                applied += (applied.empty() ? "" : ", ") + std::string(def.envName) + "=" + *envValue;
            }
            item.startup = effective(item);
            s.items.push_back(std::move(item));
        }

        std::printf("[settings] file %s%s; from the file: %s; fixed by the environment: %s\n", path.empty() ? "(none)" : path.c_str(),
                    fileRead ? "" : " (not there yet)", applied.empty() ? "nothing set" : applied.c_str(),
                    fixed.empty() ? "nothing" : fixed.c_str());
        std::fflush(stdout);
    }

    bool initialized()
    {
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.initialized;
    }

    const std::string &path()
    {
        return store().path;
    }

    std::string get(const std::string &key)
    {
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (const Item *item = itemFor(s, key))
            return effective(*item);
        const Definition *def = find(key);
        return def ? def->defaultValue : std::string();
    }

    std::string startupValue(const std::string &key)
    {
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (const Item *item = itemFor(s, key))
            return item->startup;
        const Definition *def = find(key);
        return def ? def->defaultValue : std::string();
    }

    bool fixedByEnvironment(const std::string &key)
    {
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        const Item *item = itemFor(s, key);
        return item && item->envFixed;
    }

    std::string envName(const std::string &key)
    {
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        const Item *item = itemFor(s, key);
        if (item && item->envFixed)
            return item->envName;
        const Definition *def = find(key);
        return def ? def->envName : std::string();
    }

    bool set(const std::string &key, const std::string &value)
    {
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        Item *item = itemFor(s, key);
        if (!item || item->envFixed)
            return false;
        const auto normal = normalize(key, value);
        if (!normal)
            return false;
        if (item->current != *normal)
        {
            item->current = *normal;
            item->fileValue = *normal;
            s.dirty = true;
        }
        return true;
    }

    bool dirty()
    {
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.dirty;
    }

    bool save()
    {
        Store &s = store();
        std::string path;
        std::string text;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.dirty)
                return true;
            if (s.path.empty())
                return false;
            std::vector<Entry> entries;
            for (const Item &item : s.items)
                entries.push_back({item.key, item.fileValue});
            path = s.path;
            text = formatFile(entries);
        }
        std::error_code ec;
        const std::filesystem::path target(path);
        if (target.has_parent_path())
            std::filesystem::create_directories(target.parent_path(), ec);
        const std::filesystem::path temp = path + ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out << text;
            out.flush();
            if (!out)
            {
                std::fprintf(stderr, "[settings] could not write %s\n", temp.string().c_str());
                return false;
            }
        }
        std::filesystem::rename(temp, target, ec);
        if (ec)
        {
            std::fprintf(stderr, "[settings] could not replace %s: %s\n", path.c_str(), ec.message().c_str());
            std::filesystem::remove(temp, ec);
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.dirty = false;
        }
        say("[settings] saved %s", path);
        return true;
    }

    void resetForTest()
    {
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.initialized = false;
        s.dirty = false;
        s.path.clear();
        s.items.clear();
    }
}
