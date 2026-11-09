#include "mod_manifest.h"

#include <toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

namespace ps2x::mods
{
    namespace
    {
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

        std::string upperAscii(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        }

        std::string lowerAscii(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        // Number of characters (UTF-8 code points) in the text.
        size_t charCount(const std::string &text)
        {
            size_t n = 0;
            for (const unsigned char c : text)
                if ((c & 0xC0u) != 0x80u)
                    ++n;
            return n;
        }

        bool hasControlChars(const std::string &text, bool allowNewlines)
        {
            for (const unsigned char c : text)
            {
                if (c == '\n' && allowNewlines)
                    continue;
                if (c < 0x20 || c == 0x7f)
                    return true;
            }
            return false;
        }

        bool allDigits(const std::string &text)
        {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        }

        // "0" or a number without leading zeros, at most 9 digits.
        std::optional<int> parseVersionNumber(const std::string &text)
        {
            if (!allDigits(text) || text.size() > 9 || (text.size() > 1 && text[0] == '0'))
                return std::nullopt;
            return std::atoi(text.c_str());
        }

        std::vector<std::string> splitOn(const std::string &text, char separator)
        {
            std::vector<std::string> parts;
            size_t start = 0;
            while (true)
            {
                const size_t end = text.find(separator, start);
                parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            return parts;
        }

        // Compares two prerelease strings the way semver does: identifier by identifier, numbers numerically and lower
        // than words, a shorter list lower when all its identifiers are equal.
        int comparePre(const std::string &a, const std::string &b)
        {
            const std::vector<std::string> ia = splitOn(a, '.');
            const std::vector<std::string> ib = splitOn(b, '.');
            for (size_t i = 0; i < std::min(ia.size(), ib.size()); ++i)
            {
                const bool na = allDigits(ia[i]);
                const bool nb = allDigits(ib[i]);
                int result = 0;
                if (na && nb)
                {
                    const unsigned long long x = std::strtoull(ia[i].c_str(), nullptr, 10);
                    const unsigned long long y = std::strtoull(ib[i].c_str(), nullptr, 10);
                    result = x < y ? -1 : (x > y ? 1 : 0);
                }
                else if (na != nb)
                {
                    result = na ? -1 : 1;
                }
                else
                {
                    result = ia[i].compare(ib[i]);
                    result = result < 0 ? -1 : (result > 0 ? 1 : 0);
                }
                if (result != 0)
                    return result;
            }
            if (ia.size() == ib.size())
                return 0;
            return ia.size() < ib.size() ? -1 : 1;
        }

        // One comparison of a constraint, already split.
        struct Comparison
        {
            std::string op; // ">=", ">", "<=", "<", "="
            Version version;
            int parts = 3; // how many numbers the text gave ("1.2" = 2): "=" then matches a prefix
        };

        bool parseComparison(const std::string &raw, Comparison &out, std::string *error)
        {
            const std::string text = trim(raw);
            if (text.empty())
            {
                if (error)
                    *error = "an empty comparison";
                return false;
            }
            size_t opLength = 0;
            std::string op = "=";
            for (const char *candidate : {">=", "<=", ">", "<", "="})
            {
                const std::string c(candidate);
                if (text.compare(0, c.size(), c) == 0)
                {
                    op = c;
                    opLength = c.size();
                    break;
                }
            }
            const std::string versionText = trim(text.substr(opLength));
            const auto version = parseVersion(versionText, true);
            if (!version)
            {
                if (error)
                    *error = "'" + versionText + "' is not a version";
                return false;
            }
            out.op = op;
            out.version = *version;
            out.parts = 1;
            const size_t core = versionText.find('-');
            const std::string numbers = versionText.substr(0, core);
            out.parts = static_cast<int>(std::count(numbers.begin(), numbers.end(), '.')) + 1;
            return true;
        }
    }

    // ---- versions -----------------------------------------------------------------------------------------------

    std::optional<Version> parseVersion(const std::string &text, bool allowShort)
    {
        if (text.empty())
            return std::nullopt;
        Version version;
        const size_t dash = text.find('-');
        const std::string core = text.substr(0, dash);
        if (dash != std::string::npos)
        {
            version.pre = text.substr(dash + 1);
            if (version.pre.empty() || version.pre.front() == '.' || version.pre.back() == '.' ||
                version.pre.find("..") != std::string::npos)
                return std::nullopt;
            for (const unsigned char c : version.pre)
                if (!(std::isalnum(c) || c == '.' || c == '-'))
                    return std::nullopt;
        }
        const std::vector<std::string> numbers = splitOn(core, '.');
        if (numbers.size() > 3 || numbers.empty() || (!allowShort && numbers.size() != 3))
            return std::nullopt;
        int values[3] = {0, 0, 0};
        for (size_t i = 0; i < numbers.size(); ++i)
        {
            const auto n = parseVersionNumber(numbers[i]);
            if (!n)
                return std::nullopt;
            values[i] = *n;
        }
        version.major = values[0];
        version.minor = values[1];
        version.patch = values[2];
        return version;
    }

    std::string versionToString(const Version &version)
    {
        std::string text = std::to_string(version.major) + "." + std::to_string(version.minor) + "." + std::to_string(version.patch);
        if (!version.pre.empty())
            text += "-" + version.pre;
        return text;
    }

    int compareVersions(const Version &a, const Version &b)
    {
        if (a.major != b.major)
            return a.major < b.major ? -1 : 1;
        if (a.minor != b.minor)
            return a.minor < b.minor ? -1 : 1;
        if (a.patch != b.patch)
            return a.patch < b.patch ? -1 : 1;
        if (a.pre.empty() != b.pre.empty())
            return a.pre.empty() ? 1 : -1; // a release is higher than its prerelease
        if (a.pre.empty())
            return 0;
        return comparePre(a.pre, b.pre);
    }

    bool validConstraint(const std::string &constraint, std::string *error)
    {
        if (trim(constraint).empty())
        {
            if (error)
                *error = "the version constraint is empty";
            return false;
        }
        for (const std::string &part : splitOn(constraint, ','))
        {
            Comparison comparison;
            if (!parseComparison(part, comparison, error))
                return false;
        }
        return true;
    }

    bool satisfies(const Version &version, const std::string &constraint)
    {
        for (const std::string &part : splitOn(constraint, ','))
        {
            Comparison c;
            if (!parseComparison(part, c, nullptr))
                return false;
            const int order = compareVersions(version, c.version);
            bool ok = false;
            if (c.op == ">=")
                ok = order >= 0;
            else if (c.op == ">")
                ok = order > 0;
            else if (c.op == "<=")
                ok = order <= 0;
            else if (c.op == "<")
                ok = order < 0;
            else
            {
                // "=1.2" accepts 1.2.x, "=1" accepts 1.x.y, "=1.2.3" only that version.
                if (c.parts >= 3)
                    ok = order == 0;
                else if (c.parts == 2)
                    ok = version.major == c.version.major && version.minor == c.version.minor;
                else
                    ok = version.major == c.version.major;
            }
            if (!ok)
                return false;
        }
        return true;
    }

    // ---- ids and paths ------------------------------------------------------------------------------------------

    bool validModId(const std::string &id, std::string *why)
    {
        auto fail = [&](const char *message) {
            if (why)
                *why = message;
            return false;
        };
        if (id.size() < 3 || id.size() > 64)
            return fail("an id has 3 to 64 characters");
        if (!(id.front() >= 'a' && id.front() <= 'z'))
            return fail("an id starts with a lower-case letter");
        const char last = id.back();
        if (!((last >= 'a' && last <= 'z') || (last >= '0' && last <= '9')))
            return fail("an id ends with a lower-case letter or a digit");
        bool previousSeparator = false;
        for (const char c : id)
        {
            const bool separator = c == '.' || c == '-';
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || separator;
            if (!ok)
                return fail("an id uses only lower-case letters, digits, dots and dashes");
            if (separator && previousSeparator)
                return fail("an id has no two dots or dashes in a row");
            previousSeparator = separator;
        }
        return true;
    }

    bool isSafeRelativePath(const std::string &path, std::string *why)
    {
        auto fail = [&](const char *message) {
            if (why)
                *why = message;
            return false;
        };
        if (path.empty())
            return fail("the path is empty");
        if (path.size() > 240)
            return fail("the path is longer than 240 bytes");
        if (path.front() == '/')
            return fail("the path is absolute");
        if (path.find('\\') != std::string::npos)
            return fail("the path has a backslash");
        if (path.find(':') != std::string::npos)
            return fail("the path has a colon (a drive letter or a stream name)");
        for (const unsigned char c : path)
        {
            if (c < 0x20 || c == 0x7f)
                return fail("the path has a control character");
            if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
                return fail("the path has a character that Windows does not allow");
        }
        const std::string body = path.back() == '/' ? path.substr(0, path.size() - 1) : path;
        if (body.empty())
            return fail("the path is empty");
        static const std::set<std::string> reserved = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
                                                       "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
        for (const std::string &part : splitOn(body, '/'))
        {
            if (part.empty())
                return fail("the path has an empty part (two slashes in a row)");
            if (part == "." || part == "..")
                return fail("the path has a '.' or '..' part");
            if (part.back() == '.' || part.back() == ' ')
                return fail("a part of the path ends with a dot or a space");
            if (reserved.count(upperAscii(part.substr(0, part.find('.')))))
                return fail("a part of the path is a Windows device name");
        }
        return true;
    }

    // ---- kinds and settings -------------------------------------------------------------------------------------

    const char *kindName(Kind kind)
    {
        switch (kind)
        {
        case Kind::Assets: return "assets";
        case Kind::Data: return "data";
        case Kind::Patch: return "patch";
        case Kind::Code: return "code";
        case Kind::Theme: return "theme";
        }
        return "assets";
    }

    std::optional<Kind> kindFromName(const std::string &name)
    {
        for (const Kind kind : {Kind::Assets, Kind::Data, Kind::Patch, Kind::Code, Kind::Theme})
            if (name == kindName(kind))
                return kind;
        return std::nullopt;
    }

    const char *settingTypeName(SettingDecl::Type type)
    {
        switch (type)
        {
        case SettingDecl::Type::Bool: return "bool";
        case SettingDecl::Type::Choice: return "choice";
        case SettingDecl::Type::Int: return "int";
        }
        return "bool";
    }

    bool settingValueValid(const SettingDecl &decl, const std::string &value)
    {
        switch (decl.type)
        {
        case SettingDecl::Type::Bool:
            return value == "true" || value == "false";
        case SettingDecl::Type::Choice:
            return std::find(decl.choices.begin(), decl.choices.end(), value) != decl.choices.end();
        case SettingDecl::Type::Int:
        {
            if (value.empty() || value.size() > 11)
                return false;
            char *end = nullptr;
            const long long n = std::strtoll(value.c_str(), &end, 10);
            if (!end || *end != '\0')
                return false;
            return n >= decl.minValue && n <= decl.maxValue;
        }
        }
        return false;
    }

    // ---- parseManifest ------------------------------------------------------------------------------------------

    namespace
    {
        struct Failure
        {
            std::string message;
        };

        // Throws a Failure with one line; parseManifest catches it. Keeps the field checks below readable.
        [[noreturn]] void fail(const std::string &message)
        {
            throw Failure{message};
        }

        const toml::value *field(const toml::value &table, const char *key)
        {
            return table.contains(key) ? &table.at(key) : nullptr;
        }

        std::string requiredString(const toml::value &table, const char *key, const char *what)
        {
            const toml::value *v = field(table, key);
            if (!v)
                fail(std::string("mod.toml: '") + key + "' is missing.");
            if (!v->is_string())
                fail(std::string("mod.toml: '") + key + "' must be a string" + what + ".");
            return v->as_string();
        }

        void checkText(const std::string &text, const char *key, size_t maxChars, bool allowNewlines, bool allowEmpty)
        {
            if (!allowEmpty && trim(text).empty())
                fail(std::string("mod.toml: '") + key + "' is empty.");
            if (charCount(text) > maxChars)
                fail(std::string("mod.toml: '") + key + "' is longer than " + std::to_string(maxChars) + " characters.");
            if (hasControlChars(text, allowNewlines))
                fail(std::string("mod.toml: '") + key + "' has a control character.");
        }

        std::vector<std::string> stringArray(const toml::value &table, const char *key, size_t minItems, size_t maxItems,
                                             size_t maxChars)
        {
            std::vector<std::string> out;
            const toml::value *v = field(table, key);
            if (!v)
            {
                if (minItems > 0)
                    fail(std::string("mod.toml: '") + key + "' is missing.");
                return out;
            }
            if (!v->is_array())
                fail(std::string("mod.toml: '") + key + "' must be a list of strings.");
            if (v->as_array().size() > maxItems)
                fail(std::string("mod.toml: '") + key + "' has more than " + std::to_string(maxItems) + " items.");
            for (const toml::value &item : v->as_array())
            {
                if (!item.is_string())
                    fail(std::string("mod.toml: '") + key + "' must be a list of strings.");
                checkText(item.as_string(), key, maxChars, false, false);
                out.push_back(item.as_string());
            }
            if (out.size() < minItems)
                fail(std::string("mod.toml: '") + key + "' needs at least " + std::to_string(minItems) + " entry.");
            return out;
        }

        bool endsWithIgnoreCase(const std::string &text, const char *suffix)
        {
            const std::string s = lowerAscii(text);
            const std::string x(suffix);
            return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
        }

        std::string packagePath(const std::string &path, const char *key, const char *extension)
        {
            std::string why;
            if (!isSafeRelativePath(path, &why))
                fail(std::string("mod.toml: '") + key + "' has the path '" + path + "': " + why + ".");
            if (path.back() == '/' || !endsWithIgnoreCase(path, extension))
                fail(std::string("mod.toml: '") + key + "' has the path '" + path + "', which must end in " + extension + ".");
            return path;
        }

        void rejectUnknownKeys(const toml::value &table, const std::set<std::string> &allowed, const std::string &where)
        {
            std::vector<std::string> unknown;
            for (const auto &entry : table.as_table())
                if (!allowed.count(entry.first))
                    unknown.push_back(entry.first);
            if (!unknown.empty())
            {
                std::sort(unknown.begin(), unknown.end());
                fail(where + ": unknown field '" + unknown.front() + "'.");
            }
        }

        SettingDecl parseSetting(const toml::value &table, size_t index)
        {
            const std::string where = "mod.toml: settings entry " + std::to_string(index + 1);
            if (!table.is_table())
                fail(where + " must be a table.");
            rejectUnknownKeys(table, {"key", "label", "help", "type", "default", "choices", "min", "max"}, where);
            SettingDecl decl;
            const toml::value *key = field(table, "key");
            if (!key || !key->is_string())
                fail(where + " needs a 'key' string.");
            decl.key = key->as_string();
            const std::string which = "mod.toml: setting '" + decl.key + "'";
            {
                bool ok = !decl.key.empty() && decl.key.size() <= 32 && decl.key.front() >= 'a' && decl.key.front() <= 'z';
                for (const char c : decl.key)
                    ok = ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-');
                if (!ok)
                    fail(where + ": the key '" + decl.key + "' is not valid (a lower-case letter, then lower-case letters, digits, _ or -; at most 32).");
            }
            const toml::value *label = field(table, "label");
            if (!label || !label->is_string())
                fail(which + " needs a 'label' string.");
            decl.label = label->as_string();
            checkText(decl.label, "label", 48, false, false);
            if (const toml::value *help = field(table, "help"))
            {
                if (!help->is_string())
                    fail(which + ": 'help' must be a string.");
                decl.help = help->as_string();
                checkText(decl.help, "help", 120, false, true);
            }
            const toml::value *type = field(table, "type");
            if (!type || !type->is_string())
                fail(which + " needs a 'type' of bool, choice or int.");
            const toml::value *def = field(table, "default");
            if (!def)
                fail(which + " needs a 'default'.");
            const std::string typeName = type->as_string();
            if (typeName == "bool")
            {
                decl.type = SettingDecl::Type::Bool;
                if (!def->is_boolean())
                    fail(which + ": the default of a bool must be true or false.");
                decl.defaultValue = def->as_boolean() ? "true" : "false";
                if (table.contains("choices") || table.contains("min") || table.contains("max"))
                    fail(which + ": a bool has no choices, min or max.");
            }
            else if (typeName == "choice")
            {
                decl.type = SettingDecl::Type::Choice;
                if (table.contains("min") || table.contains("max"))
                    fail(which + ": a choice has no min or max.");
                const toml::value *choices = field(table, "choices");
                if (!choices || !choices->is_array() || choices->as_array().size() < 2 || choices->as_array().size() > 16)
                    fail(which + ": 'choices' must be a list of 2 to 16 strings.");
                for (const toml::value &item : choices->as_array())
                {
                    if (!item.is_string())
                        fail(which + ": 'choices' must be a list of strings.");
                    checkText(item.as_string(), "choices", 32, false, false);
                    if (std::find(decl.choices.begin(), decl.choices.end(), item.as_string()) != decl.choices.end())
                        fail(which + ": the choice '" + item.as_string() + "' is listed twice.");
                    decl.choices.push_back(item.as_string());
                }
                if (!def->is_string() || std::find(decl.choices.begin(), decl.choices.end(), def->as_string()) == decl.choices.end())
                    fail(which + ": the default must be one of the choices.");
                decl.defaultValue = def->as_string();
            }
            else if (typeName == "int")
            {
                decl.type = SettingDecl::Type::Int;
                if (table.contains("choices"))
                    fail(which + ": an int has no choices.");
                const toml::value *lo = field(table, "min");
                const toml::value *hi = field(table, "max");
                if (!lo || !hi || !lo->is_integer() || !hi->is_integer())
                    fail(which + ": an int needs whole-number 'min' and 'max'.");
                const long long a = lo->as_integer();
                const long long b = hi->as_integer();
                if (a < -1000000 || b > 1000000 || a >= b)
                    fail(which + ": 'min' and 'max' must satisfy -1000000 <= min < max <= 1000000.");
                decl.minValue = static_cast<int>(a);
                decl.maxValue = static_cast<int>(b);
                if (!def->is_integer() || def->as_integer() < a || def->as_integer() > b)
                    fail(which + ": the default must be a whole number between min and max.");
                decl.defaultValue = std::to_string(def->as_integer());
            }
            else
            {
                fail(which + ": the type '" + typeName + "' is not bool, choice or int.");
            }
            return decl;
        }

        Manifest parseValidated(const toml::value &root, const ManifestRules &rules)
        {
            Manifest m;

            // The layer and the game first: a mod written for another layer may have fields this one does not know, and a
            // list of "unknown field" errors would hide the real reason.
            const toml::value *api = field(root, "api");
            if (!api)
                fail("mod.toml: 'api' is missing.");
            if (!api->is_integer())
                fail("mod.toml: 'api' must be a whole number.");
            if (api->as_integer() != rules.api)
                fail("This mod is for mod layer version " + std::to_string(api->as_integer()) + "; this game supports version " +
                     std::to_string(rules.api) + ".");
            m.api = rules.api;
            m.game = requiredString(root, "game", " like \"SLUS_217.02\"");
            if (m.game != rules.game)
                fail("This mod is for the game '" + m.game + "', not '" + rules.game + "'.");

            m.id = requiredString(root, "id", "");
            {
                std::string why;
                if (!validModId(m.id, &why))
                    fail("mod.toml: the id '" + m.id + "' is not valid: " + why + ".");
            }
            m.name = requiredString(root, "name", "");
            checkText(m.name, "name", kMaxNameChars, false, false);
            m.version = requiredString(root, "version", " like 1.2.3");
            {
                const auto version = parseVersion(m.version);
                if (!version)
                    fail("mod.toml: the version '" + m.version + "' is not a version like 1.2.3 (or 1.2.3-beta.1).");
                m.parsedVersion = *version;
            }
            m.authors = stringArray(root, "authors", 1, kMaxListItems, 64);
            if (const toml::value *description = field(root, "description"))
            {
                if (!description->is_string())
                    fail("mod.toml: 'description' must be a string.");
                m.description = description->as_string();
                checkText(m.description, "description", kMaxDescriptionChars, true, true);
            }
            {
                const std::string kind = requiredString(root, "kind", "");
                const auto parsed = kindFromName(kind);
                if (!parsed)
                    fail("mod.toml: the kind '" + kind + "' is not one of assets, data, patch, code, theme.");
                m.kind = *parsed;
            }

            // depends = [{ id = "...", version = ">=1.0" }]
            if (const toml::value *depends = field(root, "depends"))
            {
                if (!depends->is_array())
                    fail("mod.toml: 'depends' must be a list like [{ id = \"x.y\", version = \">=1.0\" }].");
                if (depends->as_array().size() > kMaxListItems)
                    fail("mod.toml: 'depends' has more than " + std::to_string(kMaxListItems) + " entries.");
                for (const toml::value &item : depends->as_array())
                {
                    if (!item.is_table())
                        fail("mod.toml: each entry of 'depends' must look like { id = \"x.y\", version = \">=1.0\" }.");
                    rejectUnknownKeys(item, {"id", "version"}, "mod.toml: depends");
                    const toml::value *id = field(item, "id");
                    if (!id || !id->is_string())
                        fail("mod.toml: each entry of 'depends' needs an 'id' string.");
                    Dependency dep;
                    dep.id = id->as_string();
                    std::string why;
                    if (!validModId(dep.id, &why))
                        fail("mod.toml: depends on the id '" + dep.id + "', which is not valid: " + why + ".");
                    if (dep.id == m.id)
                        fail("mod.toml: a mod cannot depend on itself.");
                    for (const Dependency &other : m.depends)
                        if (other.id == dep.id)
                            fail("mod.toml: depends lists '" + dep.id + "' twice.");
                    if (const toml::value *version = field(item, "version"))
                    {
                        if (!version->is_string())
                            fail("mod.toml: the version of a dependency must be a string like \">=1.0\".");
                        dep.constraint = version->as_string();
                        std::string error;
                        if (!validConstraint(dep.constraint, &error))
                            fail("mod.toml: the version '" + dep.constraint + "' of dependency '" + dep.id + "' is not a constraint (" +
                                 error + "); use something like \">=1.0\".");
                    }
                    m.depends.push_back(std::move(dep));
                }
            }

            m.conflicts = stringArray(root, "conflicts", 0, kMaxListItems, 64);
            for (size_t i = 0; i < m.conflicts.size(); ++i)
            {
                std::string why;
                if (!validModId(m.conflicts[i], &why))
                    fail("mod.toml: conflicts names the id '" + m.conflicts[i] + "', which is not valid: " + why + ".");
                if (m.conflicts[i] == m.id)
                    fail("mod.toml: a mod cannot conflict with itself.");
                if (std::find(m.conflicts.begin(), m.conflicts.begin() + static_cast<long>(i), m.conflicts[i]) != m.conflicts.begin() + static_cast<long>(i))
                    fail("mod.toml: conflicts lists '" + m.conflicts[i] + "' twice.");
                for (const Dependency &dep : m.depends)
                    if (dep.id == m.conflicts[i])
                        fail("mod.toml: '" + dep.id + "' is both a dependency and a conflict.");
            }

            // [[settings]]
            if (const toml::value *settings = field(root, "settings"))
            {
                if (!settings->is_array())
                    fail("mod.toml: 'settings' must be a list of [[settings]] tables.");
                if (settings->as_array().size() > kMaxListItems)
                    fail("mod.toml: more than " + std::to_string(kMaxListItems) + " settings.");
                size_t index = 0;
                for (const toml::value &item : settings->as_array())
                {
                    SettingDecl decl = parseSetting(item, index++);
                    for (const SettingDecl &other : m.settings)
                        if (other.key == decl.key)
                            fail("mod.toml: the setting '" + decl.key + "' is declared twice.");
                    m.settings.push_back(std::move(decl));
                }
            }

            if (const toml::value *thumbnail = field(root, "thumbnail"))
            {
                if (!thumbnail->is_string())
                    fail("mod.toml: 'thumbnail' must be a path like \"thumbnail.png\".");
                m.thumbnail = packagePath(thumbnail->as_string(), "thumbnail", ".png");
            }
            for (const std::string &path : stringArray(root, "screenshots", 0, 8, 240))
                m.screenshots.push_back(packagePath(path, "screenshots", ".png"));

            for (const std::string &path : stringArray(root, "stylesheets", 0, 8, 240))
                m.stylesheets.push_back(packagePath(path, "stylesheets", ".rcss"));
            if (m.kind == Kind::Theme && m.stylesheets.empty())
                fail("mod.toml: a theme mod needs 'stylesheets', a list of .rcss paths.");
            if (m.kind != Kind::Theme && !m.stylesheets.empty())
                fail("mod.toml: 'stylesheets' is only for theme mods.");

            rejectUnknownKeys(root,
                              {"id", "name", "version", "authors", "description", "game", "api", "kind", "depends", "conflicts", "settings",
                               "thumbnail", "screenshots", "stylesheets"},
                              "mod.toml");
            return m;
        }

        std::string oneLine(std::string text)
        {
            for (char &c : text)
                if (c == '\n' || c == '\r' || c == '\t')
                    c = ' ';
            return text;
        }
    }

    ManifestResult parseManifest(const std::string &tomlText, const ManifestRules &rules)
    {
        ManifestResult result;
        if (tomlText.size() > kMaxManifestBytes)
        {
            result.error = "mod.toml is larger than " + std::to_string(kMaxManifestBytes / 1024) + " KiB.";
            return result;
        }
        auto parsed = toml::try_parse_str(tomlText);
        if (parsed.is_err())
        {
            std::string line;
            std::string title;
            const auto &errors = parsed.unwrap_err();
            if (!errors.empty())
            {
                title = errors.front().title();
                if (!errors.front().locations().empty())
                    line = std::to_string(errors.front().locations().front().first.first_line_number());
            }
            const size_t colon = title.find(": ");
            if (colon != std::string::npos && title.compare(0, 6, "toml::") == 0)
                title = title.substr(colon + 2);
            result.error = "mod.toml is not valid TOML" + (line.empty() ? std::string() : " (line " + line + ")") +
                           (title.empty() ? std::string() : ": " + oneLine(title)) + ".";
            return result;
        }
        const toml::value root = parsed.unwrap();
        if (!root.is_table())
        {
            result.error = "mod.toml is not a table of fields.";
            return result;
        }
        {
            const std::pair<const char *, std::string *> hinted[] = {
                {"id", &result.hints.id}, {"name", &result.hints.name}, {"version", &result.hints.version}};
            for (const auto &hint : hinted)
                if (const toml::value *v = field(root, hint.first); v && v->is_string() && v->as_string().size() < 200)
                    *hint.second = v->as_string();
        }
        try
        {
            result.manifest = parseValidated(root, rules);
        }
        catch (const Failure &failure)
        {
            result.error = oneLine(failure.message);
        }
        catch (const std::exception &e)
        {
            result.error = "mod.toml: " + oneLine(e.what());
        }
        return result;
    }
}
