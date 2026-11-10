// Short forms of long text for the Mods tab's one-line labels (task s3): a folder path that would run into the buttons beside it is
// shortened in the middle ("C:\Users\...\FPWR\mods"), a long message at its end. Plain UTF-8 in and out; the length is counted in
// characters (code points), which is as close as a proportional font allows. No dependencies, so the unit tests include it directly.
#pragma once

#include <cstddef>
#include <string>

namespace ps2x::mods
{
    // How many characters one of the Mods tab's folder lines may have before it is shortened (the column beside the buttons holds about 68 of
    // the overlay's 14 dp letters; this leaves a margin for wide ones).
    constexpr size_t kModsLabelChars = 60;

    // The number of characters (code points) in UTF-8 text.
    inline size_t textLength(const std::string &utf8)
    {
        size_t n = 0;
        for (const char c : utf8)
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
                ++n;
        return n;
    }

    // The byte offset in `utf8` after its first `count` characters.
    inline size_t byteOffsetAfter(const std::string &utf8, size_t count)
    {
        size_t seen = 0;
        for (size_t i = 0; i < utf8.size(); ++i)
        {
            if ((static_cast<unsigned char>(utf8[i]) & 0xC0) != 0x80)
            {
                if (seen == count)
                    return i;
                ++seen;
            }
        }
        return utf8.size();
    }

    // `text` cut to at most `maxChars` characters, ending in "..." when it was cut.
    inline std::string shortenEnd(const std::string &text, size_t maxChars)
    {
        if (textLength(text) <= maxChars)
            return text;
        if (maxChars <= 3)
            return text.substr(0, byteOffsetAfter(text, maxChars));
        return text.substr(0, byteOffsetAfter(text, maxChars - 3)) + "...";
    }

    // A folder path cut to at most `maxChars` characters by dropping the middle: the start (the drive and the first folder, or the first
    // folder of a Unix path) and as many whole folders from the end as fit, joined by "...". A path that fits is returned unchanged. When
    // not even the last folder fits, its end is kept.
    inline std::string shortenPath(const std::string &path, size_t maxChars)
    {
        if (textLength(path) <= maxChars)
            return path;
        auto isSep = [](char c) { return c == '/' || c == '\\'; };
        // The head: through the first separator that follows a name ("C:\Users\", "/home/", "\\server\share\").
        size_t headEnd = 0;
        size_t names = 0;
        for (size_t i = 0; i < path.size(); ++i)
        {
            if (isSep(path[i]))
            {
                if (i > 0 && !isSep(path[i - 1]))
                    ++names;
                if (names == 2 || (names == 1 && path.find_first_of("/\\", i + 1) == std::string::npos))
                {
                    headEnd = i + 1;
                    break;
                }
            }
        }
        // 'C:\Users\' counts the drive as the first name; a Unix path's leading separator is not a name.
        const std::string head = path.substr(0, headEnd);
        const std::string dots = "...";
        const size_t room = maxChars > textLength(head) + dots.size() ? maxChars - textLength(head) - dots.size() : 0;
        // The tail: the longest run of whole trailing folders (each starting at a separator) that fits in `room`.
        size_t tailStart = std::string::npos;
        for (size_t i = path.size(); i-- > headEnd;)
        {
            if (!isSep(path[i]))
                continue;
            if (textLength(path.substr(i)) > room)
                break;
            tailStart = i;
        }
        if (tailStart != std::string::npos && tailStart >= headEnd)
            return head + dots + path.substr(tailStart);
        // Not even the last folder fits: keep the end of the path.
        const size_t keep = maxChars > dots.size() ? maxChars - dots.size() : 0;
        const size_t total = textLength(path);
        return dots + path.substr(byteOffsetAfter(path, total - keep));
    }
}
