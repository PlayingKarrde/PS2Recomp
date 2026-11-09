// A mod package: one zip file (.fprmod) with mod.toml at its root. This reads packages without unpacking them to disk
// and decides whether a package is acceptable. Format: docs/formats/fprmod.md.
//
// Reading goes through miniz (MIT), which stays inside mod_package.cpp: no third-party type appears here.
//
// What readPackage checks, so the manager can trust a package it lists:
//   - it is a zip, not larger than Limits::maxFileBytes, with at most Limits::maxEntries entries;
//   - every entry name is a safe relative path (isSafeRelativePath: no "..", no absolute paths, no drive letters or
//     backslashes), no two names are the same (capital letters ignored: a Windows folder could not hold both);
//   - no entry is encrypted, a link, or packed with a method miniz cannot read, and none is larger than
//     Limits::maxEntryBytes when unpacked, nor all of them together above Limits::maxTotalBytes;
//   - mod.toml is at the top, parses (mod_manifest.h), and every file it names (thumbnail, screenshots, stylesheets) is in
//     the package, and an image is not larger than Limits::maxImageBytes.
// The sizes the zip declares are trusted only as upper bounds: an entry is unpacked into a buffer of exactly its declared
// size, and miniz fails the read if the data does not fit or its checksum is wrong.
//
// Functions here are safe to call from any thread (no shared state).
#pragma once

#include "mod_manifest.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ps2x::mods
{
    struct Limits
    {
        uint64_t maxFileBytes = 1024ull * 1024 * 1024;   // the .fprmod file itself
        uint32_t maxEntries = 10000;                     // files and folders in the zip
        uint64_t maxEntryBytes = 512ull * 1024 * 1024;   // one file, unpacked
        uint64_t maxTotalBytes = 2048ull * 1024 * 1024;  // all files, unpacked
        uint64_t maxImageBytes = 8ull * 1024 * 1024;     // thumbnail and each screenshot
    };

    struct PackageEntry
    {
        std::string name;
        uint64_t size = 0;       // unpacked
        uint64_t packedSize = 0; // in the zip
        bool isDirectory = false;
    };

    struct Package
    {
        Manifest manifest;
        std::vector<PackageEntry> entries; // in the zip's order
        uint64_t fileSize = 0;
        // The entry called `name` (exact match), or nullptr.
        const PackageEntry *find(const std::string &name) const;
    };

    struct PackageResult
    {
        std::optional<Package> package; // set when the package is acceptable
        std::string error;              // one line, set when not
        ManifestHints hints;            // what could be read of the manifest, to name the mod in a list
    };

    // Opens `file`, checks it as described above and returns the manifest and the file list.
    PackageResult readPackage(const std::filesystem::path &file, const ManifestRules &rules = ManifestRules(),
                              const Limits &limits = Limits());

    // Unpacks one entry into memory. nullopt (with *error, one line) when the file is not a zip, has no such entry, or the
    // entry is larger than maxBytes.
    std::optional<std::vector<uint8_t>> readPackageEntry(const std::filesystem::path &file, const std::string &name,
                                                         uint64_t maxBytes, std::string *error = nullptr);

    // Builds a zip (deflate) holding `files` (name, content) in order. For the unit tests and for tools that make packages;
    // the names are not checked, so a test can build a package with an unsafe name. Returns nullopt and sets *error on failure.
    std::optional<std::string> buildPackage(const std::vector<std::pair<std::string, std::string>> &files, std::string *error = nullptr);
    bool writeBytesToFile(const std::filesystem::path &file, const std::string &bytes);
}
