#include "mod_state.h"

#include "mod_manifest.h"

#include <toml.hpp>

#include <set>

namespace ps2x::mods
{
    namespace
    {
        // A TOML basic string with the escapes TOML requires.
        std::string quote(const std::string &text)
        {
            std::string out = "\"";
            for (const unsigned char c : text)
            {
                switch (c)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20 || c == 0x7f)
                    {
                        static const char hex[] = "0123456789ABCDEF";
                        out += "\\u00";
                        out += hex[c >> 4];
                        out += hex[c & 15];
                    }
                    else
                    {
                        out += static_cast<char>(c);
                    }
                }
            }
            out += "\"";
            return out;
        }

        // A bare TOML key when the text is simple enough, else a quoted one.
        std::string key(const std::string &text)
        {
            bool simple = !text.empty();
            for (const char c : text)
                simple = simple && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-');
            return simple ? text : quote(text);
        }
    }

    bool parseStateFile(const std::string &text, StateFile &out, std::string *error)
    {
        out = StateFile();
        auto parsed = toml::try_parse_str(text);
        if (parsed.is_err())
        {
            std::string line;
            const auto &errors = parsed.unwrap_err();
            if (!errors.empty() && !errors.front().locations().empty())
                line = std::to_string(errors.front().locations().front().first.first_line_number());
            if (error)
                *error = "The mod state file is not valid TOML" + (line.empty() ? std::string() : " (line " + line + ")") + ".";
            return false;
        }
        const toml::value root = parsed.unwrap();
        if (!root.is_table())
        {
            if (error)
                *error = "The mod state file is not a table.";
            return false;
        }
        if (!root.contains("mod"))
            return true;
        const toml::value &mods = root.at("mod");
        if (!mods.is_array())
        {
            if (error)
                *error = "The mod state file's 'mod' must be a list of [[mod]] tables.";
            return false;
        }
        std::set<std::string> seen;
        for (const toml::value &item : mods.as_array())
        {
            if (!item.is_table() || !item.contains("id") || !item.at("id").is_string())
                continue;
            StateMod mod;
            mod.id = item.at("id").as_string();
            if (!validModId(mod.id) || !seen.insert(mod.id).second)
                continue;
            if (item.contains("enabled") && item.at("enabled").is_boolean())
                mod.enabled = item.at("enabled").as_boolean();
            if (item.contains("settings") && item.at("settings").is_table())
            {
                for (const auto &entry : item.at("settings").as_table())
                {
                    StateValue value;
                    if (entry.second.is_boolean())
                    {
                        value.kind = StateValue::Kind::Bool;
                        value.text = entry.second.as_boolean() ? "true" : "false";
                    }
                    else if (entry.second.is_integer())
                    {
                        value.kind = StateValue::Kind::Int;
                        value.text = std::to_string(entry.second.as_integer());
                    }
                    else if (entry.second.is_string())
                    {
                        value.kind = StateValue::Kind::String;
                        value.text = entry.second.as_string();
                    }
                    else
                    {
                        continue;
                    }
                    mod.settings[entry.first] = value;
                }
            }
            out.mods.push_back(std::move(mod));
        }
        return true;
    }

    std::string formatStateFile(const StateFile &state)
    {
        std::string out =
            "# Fire Pro Wrestling Returns (recomp): the mod manager's state.\n"
            "# The game writes this file when you turn a mod on or off, move it, change one of its settings, or install or remove\n"
            "# one. Mods are listed in load order: where two mods change the same thing, the last one wins.\n"
            "# You may edit it while the game is closed. A change to which mods are on takes effect the next time the game starts.\n"
            "version = 1\n";
        for (const StateMod &mod : state.mods)
        {
            out += "\n[[mod]]\n";
            out += "id = " + quote(mod.id) + "\n";
            out += std::string("enabled = ") + (mod.enabled ? "true" : "false") + "\n";
            if (!mod.settings.empty())
            {
                out += "\n[mod.settings]\n";
                for (const auto &entry : mod.settings)
                {
                    out += key(entry.first) + " = ";
                    switch (entry.second.kind)
                    {
                    case StateValue::Kind::Bool: out += entry.second.text == "true" ? "true" : "false"; break;
                    case StateValue::Kind::Int: out += entry.second.text; break;
                    case StateValue::Kind::String: out += quote(entry.second.text); break;
                    }
                    out += "\n";
                }
            }
        }
        return out;
    }
}
