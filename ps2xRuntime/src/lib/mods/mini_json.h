// A small strict JSON reader and writer for the data-pack code (data/renames.json and the profile's ledger.json).
//
// Strict means: standard JSON only (no comments, no trailing commas, no NaN), a duplicate key in one object is an error (a typo
// in a hand-edited pack would otherwise silently drop one of the two), nesting at most 64 deep. A failure is ONE line of plain
// text that names the line and column, in the style of the manifest's errors. Strings are UTF-8 bytes; \uXXXX escapes (with
// surrogate pairs) are turned into UTF-8; a lone surrogate is an error. Integers that fit in 64 bits are kept exactly (Int);
// anything with a fraction or exponent is a Double.
//
// No SDL, no game code. Not thread-safe per value; parse() and dump() are pure functions.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ps2x::mods::json
{
    struct Value
    {
        enum class Type
        {
            Null,
            Bool,
            Int,
            Double,
            String,
            Array,
            Object
        };

        Type type = Type::Null;
        bool boolean = false;
        int64_t integer = 0;
        double number = 0.0;
        std::string text;
        std::vector<Value> items;                          // Array
        struct Member;                                      // a name and its value; defined after Value is complete
        std::vector<Member> members;                        // Object, in file order

        static Value makeNull();
        static Value makeBool(bool b);
        static Value makeInt(int64_t i);
        static Value makeString(std::string s);
        static Value makeArray();
        static Value makeObject();

        bool isNull() const { return type == Type::Null; }
        bool isBool() const { return type == Type::Bool; }
        bool isInt() const { return type == Type::Int; }
        bool isNumber() const { return type == Type::Int || type == Type::Double; }
        bool isString() const { return type == Type::String; }
        bool isArray() const { return type == Type::Array; }
        bool isObject() const { return type == Type::Object; }

        // The member called `key` of an object, or nullptr.
        const Value *find(const std::string &key) const;
        // Appends / sets (an existing key is replaced in place).
        Value &set(const std::string &key, Value value);
        void push(Value value);

        // A short word for error messages: "a string", "a whole number", "a list"...
        const char *describe() const;
    };

    struct Value::Member
    {
        std::string first;
        Value second;
    };

    // Parses `text`. Returns false and sets *error (one line) on failure; `out` is then left empty.
    bool parse(const std::string &text, Value &out, std::string *error);

    // Compact text, or with `pretty` one member / item per line (members of an object that are themselves scalars stay on one line).
    std::string dump(const Value &value, bool pretty = false);
    // `text` as a JSON string literal, quotes included. Bytes >= 0x80 are copied (UTF-8 in, UTF-8 out); controls are escaped.
    std::string quote(const std::string &text);

    constexpr int kMaxDepth = 64;
}
