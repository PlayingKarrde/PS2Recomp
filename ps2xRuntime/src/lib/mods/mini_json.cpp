#include "mini_json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2x::mods::json
{
    Value Value::makeNull()
    {
        return Value();
    }

    void Value::push(Value value)
    {
        items.push_back(std::move(value));
    }

    Value Value::makeBool(bool b)
    {
        Value v;
        v.type = Type::Bool;
        v.boolean = b;
        return v;
    }

    Value Value::makeInt(int64_t i)
    {
        Value v;
        v.type = Type::Int;
        v.integer = i;
        return v;
    }

    Value Value::makeString(std::string s)
    {
        Value v;
        v.type = Type::String;
        v.text = std::move(s);
        return v;
    }

    Value Value::makeArray()
    {
        Value v;
        v.type = Type::Array;
        return v;
    }

    Value Value::makeObject()
    {
        Value v;
        v.type = Type::Object;
        return v;
    }

    const Value *Value::find(const std::string &key) const
    {
        if (type != Type::Object)
            return nullptr;
        for (const auto &member : members)
            if (member.first == key)
                return &member.second;
        return nullptr;
    }

    Value &Value::set(const std::string &key, Value value)
    {
        type = Type::Object;
        for (auto &member : members)
        {
            if (member.first == key)
            {
                member.second = std::move(value);
                return member.second;
            }
        }
        members.push_back(Member{key, std::move(value)});
        return members.back().second;
    }

    const char *Value::describe() const
    {
        switch (type)
        {
        case Type::Null: return "null";
        case Type::Bool: return "true or false";
        case Type::Int: return "a whole number";
        case Type::Double: return "a number with a fraction";
        case Type::String: return "text";
        case Type::Array: return "a list";
        case Type::Object: return "an object";
        }
        return "something";
    }

    namespace
    {
        struct Parser
        {
            const std::string &s;
            size_t pos = 0;
            std::string error;

            explicit Parser(const std::string &text) : s(text) {}

            bool fail(const std::string &message, size_t at)
            {
                if (!error.empty())
                    return false;
                size_t line = 1, column = 1;
                for (size_t i = 0; i < at && i < s.size(); ++i)
                {
                    if (s[i] == '\n')
                    {
                        ++line;
                        column = 1;
                    }
                    else
                    {
                        ++column;
                    }
                }
                error = "line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + message;
                return false;
            }
            bool fail(const std::string &message) { return fail(message, pos); }

            void skipSpace()
            {
                while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
                    ++pos;
            }

            static void appendUtf8(std::string &out, uint32_t cp)
            {
                if (cp < 0x80)
                {
                    out += static_cast<char>(cp);
                }
                else if (cp < 0x800)
                {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                else if (cp < 0x10000)
                {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                else
                {
                    out += static_cast<char>(0xF0 | (cp >> 18));
                    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
            }

            bool hex4(uint32_t &out)
            {
                if (pos + 4 > s.size())
                    return fail("a \\u escape needs four hexadecimal digits");
                uint32_t v = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const char c = s[pos + static_cast<size_t>(i)];
                    uint32_t d;
                    if (c >= '0' && c <= '9')
                        d = static_cast<uint32_t>(c - '0');
                    else if (c >= 'a' && c <= 'f')
                        d = static_cast<uint32_t>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F')
                        d = static_cast<uint32_t>(c - 'A' + 10);
                    else
                        return fail("a \\u escape needs four hexadecimal digits", pos + static_cast<size_t>(i));
                    v = v * 16 + d;
                }
                pos += 4;
                out = v;
                return true;
            }

            bool parseString(std::string &out)
            {
                // s[pos] == '"'
                ++pos;
                out.clear();
                while (true)
                {
                    if (pos >= s.size())
                        return fail("the text starts here but never ends", pos);
                    const unsigned char c = static_cast<unsigned char>(s[pos]);
                    if (c == '"')
                    {
                        ++pos;
                        return true;
                    }
                    if (c < 0x20)
                        return fail("a line break or control character inside text (write it as an escape such as \\n)");
                    if (c != '\\')
                    {
                        out += static_cast<char>(c);
                        ++pos;
                        continue;
                    }
                    ++pos;
                    if (pos >= s.size())
                        return fail("a backslash at the end of the file");
                    const char e = s[pos++];
                    switch (e)
                    {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                    {
                        uint32_t cp = 0;
                        if (!hex4(cp))
                            return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF)
                        {
                            if (pos + 6 <= s.size() && s[pos] == '\\' && s[pos + 1] == 'u')
                            {
                                pos += 2;
                                uint32_t low = 0;
                                if (!hex4(low))
                                    return false;
                                if (low < 0xDC00 || low > 0xDFFF)
                                    return fail("a high surrogate \\u escape must be followed by a low surrogate", pos - 6);
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            }
                            else
                            {
                                return fail("a high surrogate \\u escape without its low surrogate", pos - 6);
                            }
                        }
                        else if (cp >= 0xDC00 && cp <= 0xDFFF)
                        {
                            return fail("a low surrogate \\u escape without its high surrogate", pos - 6);
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default:
                        return fail(std::string("'\\") + e + "' is not a JSON escape", pos - 2);
                    }
                }
            }

            bool parseNumber(Value &out)
            {
                const size_t start = pos;
                bool real = false;
                if (s[pos] == '-')
                    ++pos;
                if (pos >= s.size())
                    return fail("a number is not complete", start);
                if (s[pos] == '0')
                {
                    ++pos;
                    if (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
                        return fail("a number cannot start with 0 (write 7, not 07)", start);
                }
                else if (s[pos] >= '1' && s[pos] <= '9')
                {
                    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
                        ++pos;
                }
                else
                {
                    return fail("this is not a value", start);
                }
                if (pos < s.size() && s[pos] == '.')
                {
                    real = true;
                    ++pos;
                    if (pos >= s.size() || s[pos] < '0' || s[pos] > '9')
                        return fail("a number needs digits after the point", pos);
                    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
                        ++pos;
                }
                if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E'))
                {
                    real = true;
                    ++pos;
                    if (pos < s.size() && (s[pos] == '+' || s[pos] == '-'))
                        ++pos;
                    if (pos >= s.size() || s[pos] < '0' || s[pos] > '9')
                        return fail("a number needs digits in its exponent", pos);
                    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
                        ++pos;
                }
                const std::string token = s.substr(start, pos - start);
                if (!real)
                {
                    errno = 0;
                    char *end = nullptr;
                    const long long v = std::strtoll(token.c_str(), &end, 10);
                    if (errno == 0 && end && *end == '\0')
                    {
                        out = Value::makeInt(v);
                        return true;
                    }
                }
                errno = 0;
                const double d = std::strtod(token.c_str(), nullptr);
                if (!std::isfinite(d))
                    return fail("this number is too large", start);
                out.type = Value::Type::Double;
                out.number = d;
                return true;
            }

            bool parseValue(Value &out, int depth)
            {
                if (depth > kMaxDepth)
                    return fail("the nesting is deeper than " + std::to_string(kMaxDepth) + " levels");
                skipSpace();
                if (pos >= s.size())
                    return fail("the file ends where a value was expected");
                const char c = s[pos];
                if (c == '{')
                {
                    out = Value::makeObject();
                    ++pos;
                    skipSpace();
                    if (pos < s.size() && s[pos] == '}')
                    {
                        ++pos;
                        return true;
                    }
                    while (true)
                    {
                        skipSpace();
                        if (pos >= s.size() || s[pos] != '"')
                            return fail("a member name in quotes was expected");
                        const size_t keyAt = pos;
                        std::string key;
                        if (!parseString(key))
                            return false;
                        for (const auto &member : out.members)
                            if (member.first == key)
                                return fail("the name \"" + key + "\" is used twice in one object", keyAt);
                        skipSpace();
                        if (pos >= s.size() || s[pos] != ':')
                            return fail("a ':' was expected after the name");
                        ++pos;
                        Value child;
                        if (!parseValue(child, depth + 1))
                            return false;
                        out.members.push_back(Value::Member{std::move(key), std::move(child)});
                        skipSpace();
                        if (pos < s.size() && s[pos] == ',')
                        {
                            ++pos;
                            continue;
                        }
                        if (pos < s.size() && s[pos] == '}')
                        {
                            ++pos;
                            return true;
                        }
                        return fail("a ',' or '}' was expected");
                    }
                }
                if (c == '[')
                {
                    out = Value::makeArray();
                    ++pos;
                    skipSpace();
                    if (pos < s.size() && s[pos] == ']')
                    {
                        ++pos;
                        return true;
                    }
                    while (true)
                    {
                        Value child;
                        if (!parseValue(child, depth + 1))
                            return false;
                        out.items.push_back(std::move(child));
                        skipSpace();
                        if (pos < s.size() && s[pos] == ',')
                        {
                            ++pos;
                            continue;
                        }
                        if (pos < s.size() && s[pos] == ']')
                        {
                            ++pos;
                            return true;
                        }
                        return fail("a ',' or ']' was expected");
                    }
                }
                if (c == '"')
                {
                    std::string text;
                    if (!parseString(text))
                        return false;
                    out = Value::makeString(std::move(text));
                    return true;
                }
                if (c == '-' || (c >= '0' && c <= '9'))
                    return parseNumber(out);
                auto literal = [&](const char *word, Value value) {
                    const size_t n = std::strlen(word);
                    if (s.compare(pos, n, word) == 0)
                    {
                        pos += n;
                        out = std::move(value);
                        return true;
                    }
                    return false;
                };
                if (literal("true", Value::makeBool(true)) || literal("false", Value::makeBool(false)) || literal("null", Value::makeNull()))
                    return true;
                return fail("this is not a value");
            }
        };

        void appendIndent(std::string &out, int level)
        {
            out.append(static_cast<size_t>(level) * 1, ' ');
        }

        void dumpValue(const Value &v, bool pretty, int level, std::string &out)
        {
            switch (v.type)
            {
            case Value::Type::Null: out += "null"; break;
            case Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
            case Value::Type::Int: out += std::to_string(v.integer); break;
            case Value::Type::Double:
            {
                char buffer[40];
                std::snprintf(buffer, sizeof(buffer), "%.17g", v.number);
                out += buffer;
                break;
            }
            case Value::Type::String: out += quote(v.text); break;
            case Value::Type::Array:
            {
                if (v.items.empty())
                {
                    out += "[]";
                    break;
                }
                bool flat = true;
                for (const Value &item : v.items)
                    if (item.isArray() || item.isObject())
                        flat = false;
                out += '[';
                for (size_t i = 0; i < v.items.size(); ++i)
                {
                    if (i)
                        out += ',';
                    if (pretty && !flat)
                    {
                        out += '\n';
                        appendIndent(out, level + 1);
                    }
                    dumpValue(v.items[i], pretty, level + 1, out);
                }
                if (pretty && !flat)
                {
                    out += '\n';
                    appendIndent(out, level);
                }
                out += ']';
                break;
            }
            case Value::Type::Object:
            {
                if (v.members.empty())
                {
                    out += "{}";
                    break;
                }
                out += '{';
                for (size_t i = 0; i < v.members.size(); ++i)
                {
                    if (i)
                        out += ',';
                    if (pretty)
                    {
                        out += '\n';
                        appendIndent(out, level + 1);
                    }
                    out += quote(v.members[i].first);
                    out += pretty ? ": " : ":";
                    dumpValue(v.members[i].second, pretty, level + 1, out);
                }
                if (pretty)
                {
                    out += '\n';
                    appendIndent(out, level);
                }
                out += '}';
                break;
            }
            }
        }
    }

    bool parse(const std::string &text, Value &out, std::string *error)
    {
        out = Value();
        Parser parser(text);
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
        {
            // A byte order mark: Windows editors add it. JSON does not allow it; saying so beats "this is not a value".
            if (error)
                *error = "line 1, column 1: the file starts with a UTF-8 byte order mark; save it as plain UTF-8 without one";
            return false;
        }
        Value value;
        if (!parser.parseValue(value, 0))
        {
            if (error)
                *error = parser.error;
            return false;
        }
        parser.skipSpace();
        if (parser.pos != text.size())
        {
            parser.fail("there is more text after the end of the document");
            if (error)
                *error = parser.error;
            return false;
        }
        out = std::move(value);
        return true;
    }

    std::string quote(const std::string &text)
    {
        std::string out = "\"";
        for (const char ch : text)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20)
                {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                }
                else
                {
                    out += static_cast<char>(c);
                }
            }
        }
        out += '"';
        return out;
    }

    std::string dump(const Value &value, bool pretty)
    {
        std::string out;
        dumpValue(value, pretty, 0, out);
        if (pretty)
            out += '\n';
        return out;
    }
}
