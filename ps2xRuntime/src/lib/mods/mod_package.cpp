#include "mod_package.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>

namespace ps2x::mods
{
    namespace fs = std::filesystem;

    namespace
    {
        FILE *openForRead(const fs::path &path)
        {
#if defined(_WIN32)
            FILE *file = nullptr;
            if (_wfopen_s(&file, path.c_str(), L"rb") != 0)
                return nullptr;
            return file;
#else
            return std::fopen(path.string().c_str(), "rb");
#endif
        }

        // A zip archive opened for reading. miniz does not close the FILE it was given, so this does.
        class ZipReader
        {
        public:
            ZipReader() { std::memset(&m_zip, 0, sizeof(m_zip)); }
            ~ZipReader() { close(); }
            ZipReader(const ZipReader &) = delete;
            ZipReader &operator=(const ZipReader &) = delete;

            bool open(const fs::path &path, std::string &error)
            {
                m_file = openForRead(path);
                if (!m_file)
                {
                    error = "The file cannot be opened.";
                    return false;
                }
                if (!mz_zip_reader_init_cfile(&m_zip, m_file, 0, 0))
                {
                    error = std::string("This is not a valid zip file (") + mz_zip_get_error_string(mz_zip_get_last_error(&m_zip)) + ").";
                    std::fclose(m_file);
                    m_file = nullptr;
                    return false;
                }
                m_open = true;
                return true;
            }

            void close()
            {
                if (m_open)
                    mz_zip_reader_end(&m_zip);
                m_open = false;
                if (m_file)
                    std::fclose(m_file);
                m_file = nullptr;
            }

            mz_zip_archive *get() { return &m_zip; }

            std::string errorText()
            {
                return mz_zip_get_error_string(mz_zip_get_last_error(&m_zip));
            }

            std::string nameOf(mz_uint index)
            {
                const mz_uint needed = mz_zip_reader_get_filename(&m_zip, index, nullptr, 0);
                if (needed == 0)
                    return std::string();
                std::string name(needed, '\0');
                mz_zip_reader_get_filename(&m_zip, index, name.data(), needed);
                name.resize(needed - 1);
                return name;
            }

        private:
            mz_zip_archive m_zip;
            FILE *m_file = nullptr;
            bool m_open = false;
        };

        // A name for a message: printable, short.
        std::string shown(const std::string &name)
        {
            std::string out;
            for (const unsigned char c : name)
            {
                out += (c < 0x20 || c == 0x7f) ? '?' : static_cast<char>(c);
                if (out.size() >= 80)
                {
                    out += "...";
                    break;
                }
            }
            return out;
        }

        std::string lowered(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::string mib(uint64_t bytes)
        {
            return std::to_string((bytes + (1024 * 1024 - 1)) / (1024 * 1024)) + " MiB";
        }

        bool extractToString(ZipReader &zip, mz_uint index, uint64_t size, std::string &out, std::string &error, const std::string &name)
        {
            out.assign(static_cast<size_t>(size), '\0');
            if (size > 0 && !mz_zip_reader_extract_to_mem(zip.get(), index, out.data(), out.size(), 0))
            {
                error = "The zip entry '" + shown(name) + "' cannot be unpacked (" + zip.errorText() + ").";
                return false;
            }
            return true;
        }
    }

    const PackageEntry *Package::find(const std::string &name) const
    {
        for (const PackageEntry &entry : entries)
            if (entry.name == name)
                return &entry;
        return nullptr;
    }

    PackageResult readPackage(const fs::path &file, const ManifestRules &rules, const Limits &limits)
    {
        PackageResult result;
        std::error_code ec;
        const uint64_t fileSize = fs::file_size(file, ec);
        if (ec)
        {
            result.error = "The file cannot be read.";
            return result;
        }
        if (fileSize > limits.maxFileBytes)
        {
            result.error = "The file is " + mib(fileSize) + ", more than the limit of " + mib(limits.maxFileBytes) + ".";
            return result;
        }

        ZipReader zip;
        if (!zip.open(file, result.error))
            return result;

        const mz_uint count = mz_zip_reader_get_num_files(zip.get());
        if (count > limits.maxEntries)
        {
            result.error = "The zip holds " + std::to_string(count) + " entries, more than the limit of " + std::to_string(limits.maxEntries) + ".";
            return result;
        }

        Package package;
        package.fileSize = fileSize;
        std::set<std::string> seen;
        uint64_t total = 0;
        int manifestIndex = -1;
        bool manifestInFolder = false;
        for (mz_uint i = 0; i < count; ++i)
        {
            const std::string name = zip.nameOf(i);
            mz_zip_archive_file_stat stat;
            if (!mz_zip_reader_file_stat(zip.get(), i, &stat))
            {
                result.error = "The zip entry " + std::to_string(i + 1) + " cannot be read (" + zip.errorText() + ").";
                return result;
            }
            std::string why;
            if (!isSafeRelativePath(name, &why))
            {
                result.error = "The zip names the unsafe path '" + shown(name) + "': " + why + ".";
                return result;
            }
            if (!seen.insert(lowered(name)).second)
            {
                result.error = "The zip has the name '" + shown(name) + "' twice (capital letters do not make names different).";
                return result;
            }
            PackageEntry entry;
            entry.name = name;
            entry.size = stat.m_uncomp_size;
            entry.packedSize = stat.m_comp_size;
            entry.isDirectory = stat.m_is_directory != 0;
            entry.localOffset = stat.m_local_header_ofs;
            entry.crc32 = stat.m_crc32;
            entry.method = stat.m_method;
            if (!entry.isDirectory)
            {
                if (stat.m_is_encrypted)
                {
                    result.error = "The zip entry '" + shown(name) + "' is encrypted.";
                    return result;
                }
                if (!stat.m_is_supported)
                {
                    result.error = "The zip entry '" + shown(name) + "' uses a compression method this game cannot read.";
                    return result;
                }
                const bool madeOnUnix = (stat.m_version_made_by >> 8) == 3;
                if (madeOnUnix && (((stat.m_external_attr >> 16) & 0xF000u) == 0xA000u))
                {
                    result.error = "The zip entry '" + shown(name) + "' is a link, which mods cannot contain.";
                    return result;
                }
                if (stat.m_uncomp_size > limits.maxEntryBytes)
                {
                    result.error = "The zip entry '" + shown(name) + "' is " + mib(stat.m_uncomp_size) + " unpacked, more than the limit of " +
                                   mib(limits.maxEntryBytes) + ".";
                    return result;
                }
                total += stat.m_uncomp_size;
                if (total > limits.maxTotalBytes)
                {
                    result.error = "The files in the zip add up to more than the limit of " + mib(limits.maxTotalBytes) + " unpacked.";
                    return result;
                }
            }
            if (name == kManifestName)
                manifestIndex = static_cast<int>(i);
            else if (name.size() > std::strlen(kManifestName) && name.compare(name.size() - std::strlen(kManifestName), std::string::npos, kManifestName) == 0 &&
                     name[name.size() - std::strlen(kManifestName) - 1] == '/')
                manifestInFolder = true;
            package.entries.push_back(std::move(entry));
        }

        if (manifestIndex < 0)
        {
            result.error = manifestInFolder ? "mod.toml is inside a folder; it must be at the top of the zip."
                                            : "mod.toml is missing from the package.";
            return result;
        }
        const PackageEntry &manifestEntry = package.entries[static_cast<size_t>(manifestIndex)];
        if (manifestEntry.size > kMaxManifestBytes)
        {
            result.error = "mod.toml is larger than " + std::to_string(kMaxManifestBytes / 1024) + " KiB.";
            return result;
        }
        std::string manifestText;
        if (!extractToString(zip, static_cast<mz_uint>(manifestIndex), manifestEntry.size, manifestText, result.error, kManifestName))
            return result;
        ManifestResult manifest = parseManifest(manifestText, rules);
        result.hints = manifest.hints;
        if (!manifest.manifest)
        {
            result.error = manifest.error;
            return result;
        }

        // The files the manifest names must exist.
        auto need = [&](const std::string &path, const char *what, bool isImage) -> bool {
            const PackageEntry *entry = package.find(path);
            if (!entry || entry->isDirectory)
            {
                result.error = std::string("mod.toml names the ") + what + " '" + shown(path) + "', which is not in the package.";
                return false;
            }
            if (isImage && entry->size > limits.maxImageBytes)
            {
                result.error = std::string("The ") + what + " '" + shown(path) + "' is " + mib(entry->size) + ", more than the limit of " +
                               mib(limits.maxImageBytes) + " for pictures.";
                return false;
            }
            return true;
        };
        if (!manifest.manifest->thumbnail.empty() && !need(manifest.manifest->thumbnail, "thumbnail", true))
            return result;
        for (const std::string &path : manifest.manifest->screenshots)
            if (!need(path, "screenshot", true))
                return result;
        for (const std::string &path : manifest.manifest->stylesheets)
            if (!need(path, "stylesheet", false))
                return result;

        package.manifest = std::move(*manifest.manifest);
        result.package = std::move(package);
        return result;
    }

    std::optional<std::vector<uint8_t>> readPackageEntryAt(const fs::path &file, const PackageEntry &entry, uint64_t maxBytes, std::string *error)
    {
        auto fail = [&](const std::string &message) -> std::optional<std::vector<uint8_t>> {
            if (error)
                *error = message;
            return std::nullopt;
        };
        if (entry.isDirectory)
            return fail("'" + shown(entry.name) + "' is a folder.");
        if (entry.size > maxBytes)
            return fail("The package entry '" + shown(entry.name) + "' is larger than " + mib(maxBytes) + ".");
        if (entry.method != 0 && entry.method != 8)
            return fail("The package entry '" + shown(entry.name) + "' cannot be read.");
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return fail("The file cannot be opened.");
        in.seekg(static_cast<std::streamoff>(entry.localOffset));
        uint8_t header[30];
        in.read(reinterpret_cast<char *>(header), sizeof(header));
        if (!in || header[0] != 'P' || header[1] != 'K' || header[2] != 3 || header[3] != 4)
            return fail("The package entry '" + shown(entry.name) + "' cannot be found where the zip says.");
        const uint64_t dataOffset = entry.localOffset + 30u + (header[26] | (header[27] << 8)) + (header[28] | (header[29] << 8));
        in.seekg(static_cast<std::streamoff>(dataOffset));
        std::vector<uint8_t> packed(static_cast<size_t>(entry.packedSize));
        if (!packed.empty())
        {
            in.read(reinterpret_cast<char *>(packed.data()), static_cast<std::streamsize>(packed.size()));
            if (!in)
                return fail("The package entry '" + shown(entry.name) + "' is cut short.");
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(entry.size));
        if (entry.method == 0)
        {
            if (packed.size() != bytes.size())
                return fail("The package entry '" + shown(entry.name) + "' is damaged.");
            bytes = std::move(packed);
        }
        else if (!bytes.empty())
        {
            const size_t got = tinfl_decompress_mem_to_mem(bytes.data(), bytes.size(), packed.data(), packed.size(), 0);
            if (got != bytes.size())
                return fail("The package entry '" + shown(entry.name) + "' cannot be unpacked.");
        }
        if (!bytes.empty() && static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, bytes.data(), bytes.size())) != entry.crc32)
            return fail("The package entry '" + shown(entry.name) + "' is damaged (its checksum fails).");
        return bytes;
    }

    std::optional<std::vector<uint8_t>> readPackageEntry(const fs::path &file, const std::string &name, uint64_t maxBytes,
                                                         std::string *error)
    {
        auto fail = [&](const std::string &message) -> std::optional<std::vector<uint8_t>> {
            if (error)
                *error = message;
            return std::nullopt;
        };
        ZipReader zip;
        std::string openError;
        if (!zip.open(file, openError))
            return fail(openError);
        const int index = mz_zip_reader_locate_file(zip.get(), name.c_str(), nullptr, 0);
        if (index < 0)
            return fail("The package has no file '" + shown(name) + "'.");
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(zip.get(), static_cast<mz_uint>(index), &stat))
            return fail("The package entry '" + shown(name) + "' cannot be read.");
        if (stat.m_is_directory)
            return fail("'" + shown(name) + "' is a folder.");
        if (stat.m_is_encrypted || !stat.m_is_supported)
            return fail("The package entry '" + shown(name) + "' cannot be read.");
        if (stat.m_uncomp_size > maxBytes)
            return fail("The package entry '" + shown(name) + "' is larger than " + mib(maxBytes) + ".");
        std::vector<uint8_t> bytes(static_cast<size_t>(stat.m_uncomp_size));
        if (!bytes.empty() && !mz_zip_reader_extract_to_mem(zip.get(), static_cast<mz_uint>(index), bytes.data(), bytes.size(), 0))
            return fail("The package entry '" + shown(name) + "' cannot be unpacked (" + zip.errorText() + ").");
        return bytes;
    }

    std::optional<std::string> buildPackage(const std::vector<std::pair<std::string, std::string>> &files, std::string *error)
    {
        auto fail = [&](const std::string &message) -> std::optional<std::string> {
            if (error)
                *error = message;
            return std::nullopt;
        };
        mz_zip_archive zip;
        std::memset(&zip, 0, sizeof(zip));
        if (!mz_zip_writer_init_heap(&zip, 0, 64 * 1024))
            return fail("Cannot start a zip in memory.");
        for (const auto &file : files)
        {
            if (!mz_zip_writer_add_mem(&zip, file.first.c_str(), file.second.data(), file.second.size(), MZ_DEFAULT_LEVEL))
            {
                const std::string message = std::string("Cannot add '") + file.first + "' to the zip (" +
                                            mz_zip_get_error_string(mz_zip_get_last_error(&zip)) + ").";
                mz_zip_writer_end(&zip);
                return fail(message);
            }
        }
        void *buffer = nullptr;
        size_t size = 0;
        if (!mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size))
        {
            mz_zip_writer_end(&zip);
            return fail("Cannot finish the zip.");
        }
        std::string bytes(static_cast<const char *>(buffer), size);
        mz_free(buffer);
        mz_zip_writer_end(&zip);
        return bytes;
    }

    bool writeBytesToFile(const fs::path &file, const std::string &bytes)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        return static_cast<bool>(out);
    }
}
