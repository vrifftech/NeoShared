// SPDX-License-Identifier: GPL-3.0-or-later

#include "neotlk/TlkFile.hpp"

#include "GFFFile.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <new>
#include <sstream>
#include <unordered_set>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif


namespace neotlk {
namespace {

void readExact(std::istream& input, char* destination, std::size_t byteCount, const char* message) {
    input.read(destination, static_cast<std::streamsize>(byteCount));
    if (input.gcount() != static_cast<std::streamsize>(byteCount)) {
        throw NeoTLKError(message);
    }
}


void writeExact(std::ostream& output, const char* source, std::size_t byteCount, const char* message) {
    output.write(source, static_cast<std::streamsize>(byteCount));
    if (!output) {
        throw NeoTLKError(message);
    }
}

std::uint16_t readU16LE(std::istream& input) {
    unsigned char b[2]{};
    readExact(input, reinterpret_cast<char*>(b), sizeof(b), "Unexpected end of file while reading TLK data!");
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[0]) |
                                      static_cast<std::uint16_t>(b[1] << 8));
}

UInt32 readU32LE(std::istream& input) {
    unsigned char b[4]{};
    readExact(input, reinterpret_cast<char*>(b), sizeof(b), "Unexpected end of file while reading TLK data!");
    return static_cast<UInt32>(b[0]) |
           (static_cast<UInt32>(b[1]) << 8) |
           (static_cast<UInt32>(b[2]) << 16) |
           (static_cast<UInt32>(b[3]) << 24);
}

void writeU16LE(std::ostream& output, std::uint16_t value) {
    const unsigned char b[2] = {
        static_cast<unsigned char>(value & 0xffu),
        static_cast<unsigned char>((value >> 8) & 0xffu),
    };
    writeExact(output, reinterpret_cast<const char*>(b), sizeof(b), "Unable to write TLK data!");
}

void writeU32LE(std::ostream& output, UInt32 value) {
    const unsigned char b[4] = {
        static_cast<unsigned char>(value & 0xffu),
        static_cast<unsigned char>((value >> 8) & 0xffu),
        static_cast<unsigned char>((value >> 16) & 0xffu),
        static_cast<unsigned char>((value >> 24) & 0xffu),
    };
    writeExact(output, reinterpret_cast<const char*>(b), sizeof(b), "Unable to write TLK data!");
}

float readFloatLE(std::istream& input) {
    static_assert(sizeof(float) == sizeof(UInt32), "NeoTLK requires 32-bit IEEE float support.");
    const UInt32 bits = readU32LE(input);
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

void writeFloatLE(std::ostream& output, float value) {
    static_assert(sizeof(float) == sizeof(UInt32), "NeoTLK requires 32-bit IEEE float support.");
    UInt32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeU32LE(output, bits);
}

std::streamoff inputPosition(std::istream& stream) {
    const auto pos = stream.tellg();
    if (pos < 0) {
        throw NeoTLKError("Unable to determine input file position!");
    }
    return pos;
}

void seekInput(std::istream& input, std::streamoff position) {
    input.seekg(position, std::ios::beg);
    if (!input) {
        throw NeoTLKError("Unable to seek within TLK file!");
    }
}


std::array<char, 4> makeFourChar(const char (&value)[5]) {
    return {value[0], value[1], value[2], value[3]};
}

bool isFourChar(const std::array<char, 4>& value, const char (&expected)[5]) {
    return value[0] == expected[0] && value[1] == expected[1] &&
           value[2] == expected[2] && value[3] == expected[3];
}

UInt32 checkedSizeToUInt32(std::size_t size) {
    if (size > std::numeric_limits<UInt32>::max()) {
        throw NeoTLKError("TLK string text exceeds the supported 32-bit size range!");
    }
    return static_cast<UInt32>(size);
}

UInt32 checkedEntryCountToUInt32(std::size_t count) {
    if (count > std::numeric_limits<UInt32>::max()) {
        throw NeoTLKError("TLK entry count exceeds the supported 32-bit range!");
    }
    return static_cast<UInt32>(count);
}

void rejectEmbeddedNulPath(const std::string& filename, const char* message) {
    if (filename.find('\0') != std::string::npos) {
        throw NeoTLKError(message);
    }
}

std::filesystem::path userPathFromString(const std::string& filename) {
    return std::filesystem::path(filename);
}

std::string stringFromUserPath(const std::filesystem::path& path) {
    return path.string();
}

void rejectUnsafeLoadSource(const std::filesystem::path& source) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(source, ec);
    if (ec) {
        throw NeoTLKError("Unable to load specified TLK file since it could not be found!");
    }
    if (!std::filesystem::exists(status)) {
        throw NeoTLKError("Unable to load specified TLK file since it could not be found!");
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw NeoTLKError("Unable to load specified TLK file. The input path is not a regular file!");
    }
}

std::string stableAbsolutePathString(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::string();
    }
    std::error_code ec;
    const std::filesystem::path absolutePath = std::filesystem::absolute(path, ec);
    if (ec) {
        return stringFromUserPath(path.lexically_normal());
    }
    return stringFromUserPath(absolutePath.lexically_normal());
}

struct TemporarySaveFile {
    std::filesystem::path directory;
    std::filesystem::path file;
};

struct FileSignature {
    bool valid = false;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type writeTime{};
    std::uint64_t contentHash = 0;
};

std::uint64_t hashExistingRegularFileContent(const std::filesystem::path& path, const char* message) {
    // FNV-1a over the file contents. This is not a cryptographic proof, but it
    // closes the important safety hole left by size+mtime-only fingerprints:
    // same-size edits, coarse timestamp granularity, and restored timestamps.
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw NeoTLKError(message);
    }

    std::uint64_t hash = 14695981039346656037ull;
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = input.gcount();
        for (std::streamsize i = 0; i < got; ++i) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
            hash *= 1099511628211ull;
        }
    }
    if (!input.eof()) {
        throw NeoTLKError(message);
    }
    return hash;
}

FileSignature captureExistingRegularFileSignature(const std::filesystem::path& path, const char* message) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::exists(status) || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
        throw NeoTLKError(message);
    }

    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw NeoTLKError(message);
    }
    ec.clear();
    const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        throw NeoTLKError(message);
    }

    const std::uint64_t contentHash = hashExistingRegularFileContent(path, message);

    return FileSignature{true, size, writeTime, contentHash};
}

bool existingRegularFileSignatureMatches(const std::filesystem::path& path, const FileSignature& expected) {
    if (!expected.valid) {
        return false;
    }

    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::exists(status) || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
        return false;
    }

    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size != expected.size) {
        return false;
    }
    ec.clear();
    const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, ec);
    if (ec || writeTime != expected.writeTime) {
        return false;
    }

    try {
        return hashExistingRegularFileContent(path, "Unable to verify output TLK file safely!") == expected.contentHash;
    } catch (const NeoTLKError&) {
        return false;
    }
}


FileSignature captureOptionalExistingRegularFileSignature(const std::filesystem::path& path, const char* message) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        std::error_code existsEc;
        const bool exists = std::filesystem::exists(path, existsEc);
        if (!existsEc && !exists) {
            return FileSignature{};
        }
        throw NeoTLKError(message);
    }
    if (!std::filesystem::exists(status)) {
        return FileSignature{};
    }
    if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
        throw NeoTLKError(message);
    }
    return captureExistingRegularFileSignature(path, message);
}

bool outputTargetStillMatchesStart(const std::filesystem::path& path, const FileSignature& startSignature) {
    if (startSignature.valid) {
        return existingRegularFileSignatureMatches(path, startSignature);
    }

    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        std::error_code existsEc;
        const bool exists = std::filesystem::exists(path, existsEc);
        return !existsEc && !exists;
    }
    return !std::filesystem::exists(status);
}

std::filesystem::path resolveExistingSymlinkSaveTarget(const std::filesystem::path& requestedTarget) {
    if (requestedTarget.empty() || requestedTarget.filename().empty()) {
        throw NeoTLKError("Unable to create output TLK file. The output path does not name a file!");
    }

    std::filesystem::path current = requestedTarget;
    for (int depth = 0; depth < 40; ++depth) {
        std::error_code ec;
        const std::filesystem::file_status status = std::filesystem::symlink_status(current, ec);
        if (ec) {
            std::error_code existsEc;
            const bool exists = std::filesystem::exists(current, existsEc);
            if (!existsEc && !exists) {
                // A nonexistent final component is acceptable for Save As; the parent
                // directory is validated when the private temporary directory is created.
                return current.lexically_normal();
            }
            throw NeoTLKError("Unable to inspect output TLK path safely!");
        }

        if (std::filesystem::is_directory(status) && !std::filesystem::is_symlink(status)) {
            throw NeoTLKError("Unable to create output TLK file. The output path is a directory!");
        }

        if (!std::filesystem::is_symlink(status)) {
            return current.lexically_normal();
        }

        std::filesystem::path linkedTarget = std::filesystem::read_symlink(current, ec);
        if (ec || linkedTarget.empty()) {
            throw NeoTLKError("Unable to resolve symbolic-link output TLK path safely!");
        }
        if (linkedTarget.is_relative()) {
            linkedTarget = current.parent_path() / linkedTarget;
        }
        if (linkedTarget.empty() || linkedTarget.filename().empty()) {
            throw NeoTLKError("Unable to resolve symbolic-link output TLK path to a file!");
        }
        current = linkedTarget.lexically_normal();
    }

    throw NeoTLKError("Unable to resolve output TLK path safely because the symbolic-link chain is too deep or cyclic!");
}


std::filesystem::path resolveStableFilesystemTargetPath(const std::filesystem::path& requestedTarget,
                                                        const char* resolveMessage,
                                                        const char* missingParentMessage) {
    const std::filesystem::path finalTarget = resolveExistingSymlinkSaveTarget(requestedTarget);
    std::error_code ec;
    const std::filesystem::file_status finalStatus = std::filesystem::symlink_status(finalTarget, ec);
    if (ec) {
        std::error_code existsEc;
        const bool exists = std::filesystem::exists(finalTarget, existsEc);
        if (existsEc || exists) {
            throw NeoTLKError(resolveMessage);
        }
    } else if (std::filesystem::exists(finalStatus)) {
        const std::filesystem::path canonicalTarget = std::filesystem::canonical(finalTarget, ec);
        if (ec || canonicalTarget.empty() || canonicalTarget.filename().empty()) {
            throw NeoTLKError(resolveMessage);
        }
        return canonicalTarget;
    }

    const std::filesystem::path parent = finalTarget.has_parent_path() ? finalTarget.parent_path() : std::filesystem::path(".");
    const std::filesystem::path canonicalParent = std::filesystem::canonical(parent, ec);
    if (ec || canonicalParent.empty()) {
        throw NeoTLKError(missingParentMessage);
    }
    if (finalTarget.filename().empty()) {
        throw NeoTLKError(resolveMessage);
    }
    return (canonicalParent / finalTarget.filename()).lexically_normal();
}

void rejectUnsafeExistingSaveTarget(const std::filesystem::path& target) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(target, ec);
    if (ec) {
        std::error_code existsEc;
        const bool exists = std::filesystem::exists(target, existsEc);
        if (!existsEc && !exists) {
            return;
        }
        throw NeoTLKError("Unable to inspect existing output TLK file safely before overwrite!");
    }
    if (!std::filesystem::exists(status)) {
        return;
    }

    if (std::filesystem::is_directory(status)) {
        throw NeoTLKError("Unable to create output TLK file. The output path is a directory!");
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw NeoTLKError("Unable to create output TLK file. The existing output path is not a regular file!");
    }

    ec.clear();
    const auto linkCount = std::filesystem::hard_link_count(target, ec);
    if (!ec && linkCount > 1) {
        throw NeoTLKError("Refusing to overwrite output TLK file because it has multiple hard links. Save to a new file instead.");
    }

    const auto writeBits = std::filesystem::perms::owner_write |
                           std::filesystem::perms::group_write |
                           std::filesystem::perms::others_write;
    const auto permissions = status.permissions();
    if (permissions != std::filesystem::perms::unknown && (permissions & writeBits) == std::filesystem::perms::none) {
        throw NeoTLKError("Refusing to overwrite output TLK file because it is marked read-only.");
    }
}

bool looksLikeStructuredTlkFile(const std::filesystem::path& target, std::uintmax_t size) {
    if (size == 0) {
        return true;
    }
    if (size < 20u) {
        return false;
    }

    try {
        std::ifstream input(target, std::ios::binary);
        if (!input) {
            return false;
        }

        std::array<char, 4> type{};
        std::array<char, 4> version{};
        readExact(input, type.data(), type.size(), "Unable to inspect existing output TLK file safely before overwrite!");
        readExact(input, version.data(), version.size(), "Unable to inspect existing output TLK file safely before overwrite!");
        if (isFourChar(type, "GFF ") && isFourChar(version, "V4.0")) {
            std::array<char, 4> platform{};
            std::array<char, 4> resourceType{};
            std::array<char, 4> resourceVersion{};
            readExact(input, platform.data(), platform.size(), "Unable to inspect Dragon Age TLK header!");
            readExact(input, resourceType.data(), resourceType.size(), "Unable to inspect Dragon Age TLK header!");
            readExact(input, resourceVersion.data(), resourceVersion.size(), "Unable to inspect Dragon Age TLK header!");
            if (!isFourChar(resourceType, "TLK ") || !isFourChar(resourceVersion, "V0.2")) return false;
            try {
                neogff::GffFile file;
                file.LoadFile(target);
                return file.isGff4() && file.filetype() == "TLK " && file.version() == "V0.2";
            } catch (const std::exception&) {
                return false;
            }
        }
        if (!isFourChar(type, "TLK ")) return false;

        const UInt32 language = readU32LE(input);
        const UInt32 count = readU32LE(input);
        (void)language;
        if (count < 1) {
            return false;
        }

        const unsigned long long fileSize = static_cast<unsigned long long>(size);

        if (isFourChar(version, "V3.0")) {
            const UInt32 stringDataOffset = readU32LE(input);
            constexpr unsigned long long headerBytes = 20ull;
            constexpr unsigned long long entryBytes = 40ull;
            const unsigned long long minimumOffset = headerBytes + entryBytes * static_cast<unsigned long long>(count);
            if (minimumOffset > fileSize ||
                static_cast<unsigned long long>(stringDataOffset) < minimumOffset ||
                static_cast<unsigned long long>(stringDataOffset) > fileSize) {
                return false;
            }

            const unsigned long long stringBlockSize = fileSize - static_cast<unsigned long long>(stringDataOffset);
            for (UInt32 i = 0; i < count; ++i) {
                (void)readU32LE(input); // flags
                std::array<char, 16> sound{};
                readExact(input, sound.data(), sound.size(), "Unable to inspect existing output TLK file safely before overwrite!");
                (void)readU32LE(input); // volume variance
                (void)readU32LE(input); // pitch variance
                const UInt32 offset = readU32LE(input);
                const UInt32 length = readU32LE(input);
                (void)readFloatLE(input); // sound length

                if (length == 0) {
                    continue;
                }
                const unsigned long long start = static_cast<unsigned long long>(offset);
                const unsigned long long byteCount = static_cast<unsigned long long>(length);
                if (start > stringBlockSize || byteCount > stringBlockSize - start) {
                    return false;
                }
            }
            return true;
        }

        if (isFourChar(version, "V4.0")) {
            const UInt32 entryTableOffset = readU32LE(input);
            const UInt32 stringDataOffset = readU32LE(input);
            (void)readU32LE(input); // reserved
            (void)readU32LE(input); // reserved

            constexpr unsigned long long headerBytes = 32ull;
            constexpr unsigned long long entryBytes = 10ull;
            const unsigned long long entryTableEnd = static_cast<unsigned long long>(entryTableOffset) +
                entryBytes * static_cast<unsigned long long>(count);
            if (static_cast<unsigned long long>(entryTableOffset) < headerBytes ||
                entryTableEnd < static_cast<unsigned long long>(entryTableOffset) ||
                entryTableEnd > static_cast<unsigned long long>(stringDataOffset) ||
                static_cast<unsigned long long>(stringDataOffset) > fileSize) {
                return false;
            }

            input.seekg(static_cast<std::streamoff>(entryTableOffset), std::ios::beg);
            if (!input) {
                return false;
            }
            for (UInt32 i = 0; i < count; ++i) {
                (void)readU32LE(input); // sound id
                const UInt32 offset = readU32LE(input);
                const std::uint16_t length = readU16LE(input);
                if (length == 0) {
                    continue;
                }
                const unsigned long long start = static_cast<unsigned long long>(offset);
                const unsigned long long byteCount = static_cast<unsigned long long>(length);
                if (start < static_cast<unsigned long long>(stringDataOffset) ||
                    start > fileSize || byteCount > fileSize - start) {
                    return false;
                }
            }
            return true;
        }

        return false;
    } catch (const std::exception&) {
        return false;
    }
}

void rejectExistingNonTlkOutputTarget(const std::filesystem::path& target) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(target, ec);
    if (ec) {
        std::error_code existsEc;
        const bool exists = std::filesystem::exists(target, existsEc);
        if (!existsEc && !exists) {
            return;
        }
        throw NeoTLKError("Unable to inspect existing output TLK file safely before overwrite!");
    }
    if (!std::filesystem::exists(status)) {
        return;
    }
    if (!std::filesystem::is_regular_file(status)) {
        return;
    }

    const std::uintmax_t size = std::filesystem::file_size(target, ec);
    if (ec) {
        throw NeoTLKError("Unable to inspect existing output TLK file safely before overwrite!");
    }

    // A zero-byte placeholder is safe to claim as a new TLK output. Any
    // non-empty existing target must be structurally recognizable as a supported TLK
    // before replacement. Checking only the first eight signature bytes can
    // erase unrelated/corrupt data that happens to begin with "TLK V3.0".
    if (!looksLikeStructuredTlkFile(target, size)) {
        throw NeoTLKError("Refusing to overwrite output file because it is not a structurally valid supported TLK file. Delete it first or choose another path.");
    }
}

std::filesystem::perms existingFilePermissionsOrUnknown(const std::filesystem::path& target) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(target, ec);
    if (ec || !std::filesystem::exists(status) || !std::filesystem::is_regular_file(status)) {
        return std::filesystem::perms::unknown;
    }
    return status.permissions();
}

void applyExistingPermissionsToTemporary(const std::filesystem::path& temporary, std::filesystem::perms permissions) {
    if (permissions == std::filesystem::perms::unknown) {
        return;
    }
    std::error_code ec;
    std::filesystem::permissions(temporary, permissions, std::filesystem::perm_options::replace, ec);
    if (ec) {
        throw NeoTLKError("Unable to preserve output TLK file permissions on the completed temporary file!");
    }
}

TemporarySaveFile makeTemporarySaveFile(const std::filesystem::path& target) {
    const std::filesystem::path directory = target.has_parent_path() ? target.parent_path() : std::filesystem::path(".");
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec)) {
        throw NeoTLKError("Unable to create output TLK file. The output directory does not exist!");
    }

    const std::string baseName = stringFromUserPath(target.filename());
    if (baseName.empty()) {
        throw NeoTLKError("Unable to create output TLK file. The output path does not name a file!");
    }

    const std::string stem = baseName + ".tmp." +
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());

    for (int attempt = 0; attempt < 1000; ++attempt) {
        TemporarySaveFile candidate;
        candidate.directory = directory / (stem + "." + std::to_string(attempt) + ".dir");
        candidate.file = candidate.directory / baseName;
        ec.clear();
        if (std::filesystem::create_directory(candidate.directory, ec)) {
            ec.clear();
            std::filesystem::permissions(candidate.directory, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, ec);
            if (ec) {
                std::filesystem::remove(candidate.directory, ec);
                throw NeoTLKError("Unable to secure temporary TLK output directory!");
            }
            return candidate;
        }
    }
    throw NeoTLKError("Unable to create a unique temporary TLK output path!");
}

void removeTemporarySaveFileBestEffort(const TemporarySaveFile& temporary) noexcept {
    std::error_code ignored;
    if (temporary.directory.empty()) {
        return;
    }

    const auto directoryStatus = std::filesystem::symlink_status(temporary.directory, ignored);
    if (ignored || !std::filesystem::exists(directoryStatus) ||
        std::filesystem::is_symlink(directoryStatus) || !std::filesystem::is_directory(directoryStatus)) {
        return;
    }

    if (!temporary.file.empty()) {
        ignored.clear();
        const auto fileStatus = std::filesystem::symlink_status(temporary.file, ignored);
        if (!ignored && std::filesystem::exists(fileStatus) &&
            !std::filesystem::is_symlink(fileStatus) && std::filesystem::is_regular_file(fileStatus)) {
            std::filesystem::remove(temporary.file, ignored);
        }
    }

    ignored.clear();
    const auto finalDirectoryStatus = std::filesystem::symlink_status(temporary.directory, ignored);
    if (!ignored && std::filesystem::exists(finalDirectoryStatus) &&
        !std::filesystem::is_symlink(finalDirectoryStatus) && std::filesystem::is_directory(finalDirectoryStatus)) {
        // The directory was created specifically for this save attempt. Remove
        // just that directory entry; do not recurse, so cleanup cannot delete
        // unrelated contents even if another process has modified the directory.
        std::filesystem::remove(temporary.directory, ignored);
    }
}


void flushPathToStableStorage(const std::filesystem::path& path, const char* message) {
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) {
        throw NeoTLKError(message);
    }
    const int result = ::fsync(fd);
    const int closeResult = ::close(fd);
    if (result != 0 || closeResult != 0) {
        throw NeoTLKError(message);
    }
#else
    (void)path;
    (void)message;
#endif
}

void flushDirectoryBestEffort(const std::filesystem::path& directory) noexcept {
#ifndef _WIN32
    if (directory.empty()) {
        return;
    }
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return;
    }
    (void)::fsync(fd);
    (void)::close(fd);
#else
    (void)directory;
#endif
}

void replaceFileWithTemporary(const std::filesystem::path& temporary, const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        throw NeoTLKError("Unable to replace output TLK file with completed temporary file!");
    }
}

void normalizeEntryForSave(TalkString& entry) {
    entry.stringSize = checkedSizeToUInt32(entry.text.size());
}

void validateStringPayloadRanges(const std::vector<TalkString>& entries,
                                 UInt32 stringEntriesOffset,
                                 std::streamoff fileSize) {
    if (fileSize < 0) {
        throw NeoTLKError("Unable to determine TLK file size!");
    }

    constexpr unsigned long long headerBytes = 20ull;
    constexpr unsigned long long entryBytes = 40ull;
    const unsigned long long minimumStringDataOffset =
        headerBytes + entryBytes * static_cast<unsigned long long>(entries.size());
    const unsigned long long fileSizeUnsigned = static_cast<unsigned long long>(fileSize);

    if (static_cast<unsigned long long>(stringEntriesOffset) < minimumStringDataOffset ||
        static_cast<unsigned long long>(stringEntriesOffset) > fileSizeUnsigned) {
        throw NeoTLKError("Invalid TLK string data offset in file header!");
    }

    const unsigned long long stringDataBlockSize =
        fileSizeUnsigned - static_cast<unsigned long long>(stringEntriesOffset);

    for (const TalkString& entry : entries) {
        if (entry.stringSize == 0) {
            continue;
        }
        const unsigned long long offset = static_cast<unsigned long long>(entry.offsetToString);
        const unsigned long long size = static_cast<unsigned long long>(entry.stringSize);
        if (offset > stringDataBlockSize || size > stringDataBlockSize - offset) {
            throw NeoTLKError("Invalid TLK string payload range. Declared string data extends beyond the end of the file!");
        }
    }
}

void writeTlkV30Image(std::ostream& output,
                   const std::array<char, 4>& fileType,
                   const std::array<char, 4>& fileVersion,
                   UInt32 languageId,
                   std::vector<TalkString>& entries,
                   UInt32& stringEntriesOffset) {
    const UInt32 stringCount = checkedEntryCountToUInt32(entries.size());
    std::vector<std::string> encodedPayloads;
    encodedPayloads.reserve(entries.size());
    for (TalkString& entry : entries) {
        try {
            encodedPayloads.push_back(encodeTextBytes(entry.text, entry.textEncoding));
        } catch (const std::exception& ex) {
            throw NeoTLKError(std::string("Unable to encode TLK string #") + std::to_string(entry.strRef) + ": " + ex.what());
        }
        entry.stringSize = checkedSizeToUInt32(encodedPayloads.back().size());
    }
    constexpr unsigned long long headerBytes = 20ull;
    constexpr unsigned long long entryBytes = 40ull;

    if (entries.size() > (std::numeric_limits<unsigned long long>::max() - headerBytes) / entryBytes) {
        throw NeoTLKError("TLK entry count is too large to serialize safely!");
    }

    const unsigned long long minimumOffset = headerBytes + entryBytes * static_cast<unsigned long long>(entries.size());
    if (minimumOffset > static_cast<unsigned long long>(std::numeric_limits<UInt32>::max())) {
        throw NeoTLKError("TLK entry table exceeds the supported 32-bit offset range!");
    }

    stringEntriesOffset = static_cast<UInt32>(minimumOffset);

    unsigned long long relativeStringCursor = 0;
    for (std::size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex) {
        TalkString& entry = entries[entryIndex];
        entry.strRef = checkedEntryCountToUInt32(entryIndex);
        const std::string& payload = encodedPayloads[entryIndex];

        if (payload.empty()) {
            entry.offsetToString = 0;
            continue;
        }

        if (relativeStringCursor > static_cast<unsigned long long>(std::numeric_limits<UInt32>::max())) {
            throw NeoTLKError("TLK string payload offsets exceed the supported 32-bit range!");
        }
        if (static_cast<unsigned long long>(payload.size()) >
            static_cast<unsigned long long>(std::numeric_limits<UInt32>::max()) - relativeStringCursor) {
            throw NeoTLKError("TLK string payload block exceeds the supported 32-bit range!");
        }
        if (static_cast<unsigned long long>(stringEntriesOffset) + relativeStringCursor >
            static_cast<unsigned long long>(std::numeric_limits<UInt32>::max())) {
            throw NeoTLKError("TLK file grew beyond the supported 32-bit offset range!");
        }

        entry.offsetToString = static_cast<UInt32>(relativeStringCursor);
        relativeStringCursor += static_cast<unsigned long long>(payload.size());
    }

    if (static_cast<unsigned long long>(stringEntriesOffset) + relativeStringCursor >
        static_cast<unsigned long long>(std::numeric_limits<UInt32>::max())) {
        throw NeoTLKError("TLK file grew beyond the supported 32-bit offset range!");
    }

    writeExact(output, fileType.data(), fileType.size(), "Unable to write TLK file header!");
    writeExact(output, fileVersion.data(), fileVersion.size(), "Unable to write TLK file header!");
    writeU32LE(output, languageId);
    writeU32LE(output, stringCount);
    writeU32LE(output, stringEntriesOffset);

    for (TalkString& entry : entries) {
        writeU32LE(output, entry.flags);
        writeExact(output, entry.soundResref.bytes.data(), entry.soundResref.bytes.size(), "Unable to write TLK sound ResRef!");
        writeU32LE(output, entry.volumeVariance);
        writeU32LE(output, entry.pitchVariance);
        writeU32LE(output, entry.offsetToString);
        writeU32LE(output, entry.stringSize);
        writeFloatLE(output, entry.soundLength);
    }


    for (const std::string& payload : encodedPayloads) {
        if (!payload.empty()) {
            writeExact(output, payload.data(), payload.size(), "Unable to write TLK string text!");
        }
    }

}

void writeTlkV40Image(std::ostream& output,
                      const std::array<char, 4>& fileType,
                      const std::array<char, 4>& fileVersion,
                      UInt32 languageId,
                      std::vector<TalkString>& entries,
                      UInt32& stringEntriesOffset) {
    const UInt32 stringCount = checkedEntryCountToUInt32(entries.size());
    std::vector<std::string> encodedPayloads;
    encodedPayloads.reserve(entries.size());
    for (TalkString& entry : entries) {
        if (!isValidUtf8(entry.text)) {
            throw NeoTLKError("Unable to write Jade Empire TLK V4.0 because an entry is not valid UTF-8.");
        }
        entry.textEncoding = TextEncoding::Utf8;
        encodedPayloads.push_back(entry.text);
        entry.stringSize = checkedSizeToUInt32(entry.text.size());
    }
    constexpr unsigned long long headerBytes = 32ull;
    constexpr unsigned long long entryBytes = 10ull;

    if (entries.size() > (std::numeric_limits<unsigned long long>::max() - headerBytes) / entryBytes) {
        throw NeoTLKError("TLK V4.0 entry count is too large to serialize safely!");
    }

    const unsigned long long stringDataOffset64 = headerBytes + entryBytes * static_cast<unsigned long long>(entries.size());
    if (stringDataOffset64 > static_cast<unsigned long long>(std::numeric_limits<UInt32>::max())) {
        throw NeoTLKError("TLK V4.0 entry table exceeds the supported 32-bit offset range!");
    }

    stringEntriesOffset = static_cast<UInt32>(stringDataOffset64);

    unsigned long long absoluteStringCursor = stringDataOffset64;
    for (std::size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex) {
        TalkString& entry = entries[entryIndex];
        entry.strRef = checkedEntryCountToUInt32(entryIndex);
        const std::string& payload = encodedPayloads[entryIndex];

        if (payload.size() > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())) {
            throw NeoTLKError("TLK V4.0 string text exceeds the supported 16-bit per-entry size range!");
        }

        if (payload.empty()) {
            if (absoluteStringCursor > static_cast<unsigned long long>(std::numeric_limits<UInt32>::max())) {
                throw NeoTLKError("TLK V4.0 string payload offsets exceed the supported 32-bit range!");
            }
            entry.offsetToString = static_cast<UInt32>(absoluteStringCursor);
            continue;
        }

        if (absoluteStringCursor > static_cast<unsigned long long>(std::numeric_limits<UInt32>::max())) {
            throw NeoTLKError("TLK V4.0 string payload offsets exceed the supported 32-bit range!");
        }
        entry.offsetToString = static_cast<UInt32>(absoluteStringCursor);
        absoluteStringCursor += static_cast<unsigned long long>(payload.size());
    }

    if (absoluteStringCursor > static_cast<unsigned long long>(std::numeric_limits<UInt32>::max())) {
        throw NeoTLKError("TLK V4.0 file grew beyond the supported 32-bit offset range!");
    }

    writeExact(output, fileType.data(), fileType.size(), "Unable to write TLK file header!");
    writeExact(output, fileVersion.data(), fileVersion.size(), "Unable to write TLK file header!");
    writeU32LE(output, languageId);
    writeU32LE(output, stringCount);
    writeU32LE(output, 32u);
    writeU32LE(output, stringEntriesOffset);
    writeU32LE(output, 0u);
    writeU32LE(output, 0u);

    for (TalkString& entry : entries) {
        writeU32LE(output, entry.soundId);
        writeU32LE(output, entry.offsetToString);
        writeU16LE(output, static_cast<std::uint16_t>(entry.stringSize));
    }

    for (const std::string& payload : encodedPayloads) {
        if (!payload.empty()) {
            writeExact(output, payload.data(), payload.size(), "Unable to write TLK V4.0 string text!");
        }
    }
}

std::unique_ptr<neogff::GffStruct> cloneGffStruct(const neogff::GffStruct& source) {
    std::unique_ptr<neogff::GffField> clonedField = source.Clone();
    auto* clonedStruct = dynamic_cast<neogff::GffStruct*>(clonedField.get());
    if (clonedStruct == nullptr) {
        throw NeoTLKError("Unable to clone Dragon Age TLK record structure.");
    }
    clonedField.release();
    return std::unique_ptr<neogff::GffStruct>(clonedStruct);
}

neogff::GffField* findGff4Field(neogff::GffStruct& structure,
                                UInt32 labelId,
                                UInt32 expectedType,
                                bool allowTypeFallback = true) {
    for (auto& owned : structure.allFields()) {
        neogff::GffField* field = owned.get();
        if (field != nullptr && field->hasGff4LabelId && field->gff4LabelId == labelId &&
            field->fieldtype == expectedType) {
            return field;
        }
    }
    if (allowTypeFallback) {
        for (auto& owned : structure.allFields()) {
            neogff::GffField* field = owned.get();
            if (field != nullptr && field->fieldtype == expectedType) return field;
        }
    }
    return nullptr;
}

const neogff::GffField* findGff4Field(const neogff::GffStruct& structure,
                                      UInt32 labelId,
                                      UInt32 expectedType,
                                      bool allowTypeFallback = true) {
    for (const auto& owned : structure.allFields()) {
        const neogff::GffField* field = owned.get();
        if (field != nullptr && field->hasGff4LabelId && field->gff4LabelId == labelId &&
            field->fieldtype == expectedType) {
            return field;
        }
    }
    if (allowTypeFallback) {
        for (const auto& owned : structure.allFields()) {
            const neogff::GffField* field = owned.get();
            if (field != nullptr && field->fieldtype == expectedType) return field;
        }
    }
    return nullptr;
}

neogff::GffList* findDragonAgeStringList(neogff::GffFile& file, UInt32 labelId = 19001u) {
    neogff::GffStruct* root = file.root();
    if (root == nullptr) return nullptr;
    for (auto& owned : root->allFields()) {
        auto* list = dynamic_cast<neogff::GffList*>(owned.get());
        if (list != nullptr && list->hasGff4LabelId && list->gff4LabelId == labelId) return list;
    }
    for (auto& owned : root->allFields()) {
        if (auto* list = dynamic_cast<neogff::GffList*>(owned.get())) return list;
    }
    return nullptr;
}

} // namespace

ResRef ResRef::fromString(std::string_view value) {
    ResRef result;
    const std::size_t limit = std::min<std::size_t>(result.bytes.size(), value.size());
    for (std::size_t i = 0; i < limit; ++i) {
        result.bytes[i] = value[i];
    }
    return result;
}

std::string ResRef::soundString() const {
    std::size_t length = 0;
    while (length < bytes.size() && bytes[length] != '\0') {
        ++length;
    }
    return std::string(bytes.data(), length);
}

std::string ResRef::toString() const {
    return soundString();
}

std::string TalkString::soundString() const {
    const std::string resref = soundResref.soundString();
    if (!resref.empty()) {
        return resref;
    }
    if (soundId != 0xffffffffu) {
        return std::to_string(soundId);
    }
    return {};
}

void TalkString::cloneFrom(const TalkString& other) {
    flags = other.flags;
    soundResref = other.soundResref;
    volumeVariance = other.volumeVariance;
    pitchVariance = other.pitchVariance;
    offsetToString = other.offsetToString;
    stringSize = other.stringSize;
    soundLength = other.soundLength;
    soundId = other.soundId;
    strRef = other.strRef;
    text = other.text;
    textEncoding = other.textEncoding;
    custom = other.custom;
}

TalkTable::TalkTable() {
    reset();
}

TalkTable::TalkTable(const std::string& filename) {
    load(filename);
}

TalkTable::~TalkTable() = default;
TalkTable::TalkTable(TalkTable&&) noexcept = default;
TalkTable& TalkTable::operator=(TalkTable&&) noexcept = default;

std::string storageFormatName(TlkStorageFormat format) {
    switch (format) {
    case TlkStorageFormat::ClassicV30:
        return "Classic TLK V3.0";
    case TlkStorageFormat::JadeV40:
        return "Jade Empire TLK V4.0";
    case TlkStorageFormat::DragonAgeV02:
        return "Dragon Age GFF TLK V0.2";
    }
    return "Unknown TLK format";
}

std::string storageFormatToken(TlkStorageFormat format) {
    switch (format) {
    case TlkStorageFormat::ClassicV30: return "ClassicV30";
    case TlkStorageFormat::JadeV40: return "JadeV40";
    case TlkStorageFormat::DragonAgeV02: return "DragonAgeV02";
    }
    throw NeoTLKError("Unknown TLK storage format value.");
}

TlkStorageFormat parseStorageFormat(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (char raw : value) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        if (std::isalnum(ch) != 0) normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (normalized == "classicv30" || normalized == "classic" || normalized == "tlkv30" ||
        normalized == "classictlkv30") {
        return TlkStorageFormat::ClassicV30;
    }
    if (normalized == "jadev40" || normalized == "jade" || normalized == "jadeempire" ||
        normalized == "jadeempiretlkv40" || normalized == "tlkv40") {
        return TlkStorageFormat::JadeV40;
    }
    if (normalized == "dragonagev02" || normalized == "dragonage" || normalized == "datlk" ||
        normalized == "dragonagegfftlkv02" || normalized == "gfftlkv02") {
        return TlkStorageFormat::DragonAgeV02;
    }
    throw NeoTLKError("Unknown TLK storage format: " + std::string(value));
}

UInt32 TalkTable::count() const {
    return checkedEntryCountToUInt32(entries_.size());
}

UInt32 TalkTable::minStrRef() const noexcept {
    if (entries_.empty()) return 0u;
    UInt32 result = entries_.front().strRef;
    for (const TalkString& entry : entries_) result = std::min(result, entry.strRef);
    return result;
}

UInt32 TalkTable::maxStrRef() const noexcept {
    if (entries_.empty()) return 0u;
    UInt32 result = entries_.front().strRef;
    for (const TalkString& entry : entries_) result = std::max(result, entry.strRef);
    return result;
}

UInt32 TalkTable::nextAvailableStrRef() const {
    if (entries_.empty()) return 0u;
    const UInt32 maximum = maxStrRef();
    if (maximum == std::numeric_limits<UInt32>::max()) {
        throw NeoTLKError("Unable to add a TLK entry because no additional 32-bit StrRef is available!");
    }
    return maximum + 1u;
}

bool TalkTable::containsStrRef(UInt32 strRef) const noexcept {
    if (!hasSparseStrRefs() && strRef < entries_.size() && entries_[static_cast<std::size_t>(strRef)].strRef == strRef) {
        return true;
    }
    return std::any_of(entries_.begin(), entries_.end(), [strRef](const TalkString& entry) {
        return entry.strRef == strRef;
    });
}

std::string TalkTable::textEncodingSummary() const {
    std::size_t utf8 = 0;
    std::size_t cp1252 = 0;
    std::size_t cp1250 = 0;
    for (const TalkString& entry : entries_) {
        switch (entry.textEncoding) {
        case TextEncoding::Utf8: ++utf8; break;
        case TextEncoding::Windows1252: ++cp1252; break;
        case TextEncoding::Windows1250: ++cp1250; break;
        }
    }
    int used = (utf8 != 0u ? 1 : 0) + (cp1252 != 0u ? 1 : 0) + (cp1250 != 0u ? 1 : 0);
    if (used <= 1) return textEncodingName(preferredTextEncoding_);
    std::ostringstream out;
    out << "Mixed (";
    bool first = true;
    auto add = [&](const char* name, std::size_t n) {
        if (n == 0u) return;
        if (!first) out << ", ";
        first = false;
        out << name << ": " << n;
    };
    add("UTF-8", utf8);
    add("Windows-1252", cp1252);
    add("Windows-1250", cp1250);
    out << ")";
    return out.str();
}

void TalkTable::setLanguage(UInt32 languageId) {
    if (!supportsLanguageId()) {
        throw NeoTLKError("Dragon Age TLK V0.2 files do not store a classic TLK language ID.");
    }
    if (languageId_ != languageId) {
        languageId_ = languageId;
        if (languageId == 5u) preferredTextEncoding_ = TextEncoding::Windows1250;
        if (fileOpen_) modified_ = true;
    }
}

void TalkTable::setVersion30() {
    if (isDragonAgeV02()) {
        throw NeoTLKError("Dragon Age TLK V0.2 cannot be converted to classic TLK V3.0 by changing the version field.");
    }
    storageFormat_ = TlkStorageFormat::ClassicV30;
    if (!isFourChar(fileVersion_, "V3.0")) {
        fileVersion_ = makeFourChar("V3.0");
        if (fileOpen_) modified_ = true;
    }
}

void TalkTable::setVersion40() {
    if (isDragonAgeV02()) {
        throw NeoTLKError("Dragon Age TLK V0.2 cannot be converted to Jade Empire TLK V4.0 by changing the version field.");
    }
    storageFormat_ = TlkStorageFormat::JadeV40;
    preferredTextEncoding_ = TextEncoding::Utf8;
    for (TalkString& entry : entries_) entry.textEncoding = TextEncoding::Utf8;
    if (!isFourChar(fileVersion_, "V4.0")) {
        fileVersion_ = makeFourChar("V4.0");
        if (fileOpen_) modified_ = true;
    }
}

bool TalkTable::isVersion40() const noexcept {
    return storageFormat_ == TlkStorageFormat::JadeV40;
}

TalkString& TalkTable::entryAtStrRef(UInt32 strRef) {
    if (!hasSparseStrRefs() && strRef < entries_.size()) {
        TalkString& entry = entries_[static_cast<std::size_t>(strRef)];
        if (entry.strRef == strRef) {
            if (fileOpen_) modified_ = true;
            return entry;
        }
    }
    for (TalkString& candidate : entries_) {
        if (candidate.strRef == strRef) {
            if (fileOpen_) modified_ = true;
            return candidate;
        }
    }
    throw NeoTLKError("Invalid StrRef specified!");
}

const TalkString& TalkTable::entryAtStrRef(UInt32 strRef) const {
    if (!hasSparseStrRefs() && strRef < entries_.size()) {
        const TalkString& entry = entries_[static_cast<std::size_t>(strRef)];
        if (entry.strRef == strRef) return entry;
    }
    for (const TalkString& candidate : entries_) {
        if (candidate.strRef == strRef) return candidate;
    }
    throw NeoTLKError("Invalid StrRef specified!");
}

namespace {

void prepareAddedEntry(TalkString& entry, UInt32 strRef, TextEncoding preferredEncoding) {
    normalizeEntryForSave(entry);
    entry.custom = true;
    entry.strRef = strRef;
    if (entry.text.empty() || entry.textEncoding == TextEncoding::Utf8) {
        entry.textEncoding = preferredEncoding;
    }
}

} // namespace

void TalkTable::addEntry(TalkString& entry) {
    if (!fileOpen_) throw NeoTLKError("Unable to add new entry. No TLK file is open!");
    const UInt32 newStrRef = hasSparseStrRefs() ? nextAvailableStrRef() : count();
    prepareAddedEntry(entry, newStrRef, preferredTextEncoding_);
    entries_.push_back(entry);
    synchronizeCount();
    modified_ = true;
}

void TalkTable::addEntry(TalkString&& entry) {
    if (!fileOpen_) throw NeoTLKError("Unable to add new entry. No TLK file is open!");
    const UInt32 newStrRef = hasSparseStrRefs() ? nextAvailableStrRef() : count();
    prepareAddedEntry(entry, newStrRef, preferredTextEncoding_);
    entries_.push_back(std::move(entry));
    synchronizeCount();
    modified_ = true;
}

void TalkTable::addEntryAtStrRef(UInt32 strRef, TalkString entry) {
    if (!fileOpen_) throw NeoTLKError("Unable to add new entry. No TLK file is open!");
    if (containsStrRef(strRef)) throw NeoTLKError("Unable to add entry because that StrRef already exists!");
    if (!hasSparseStrRefs() && strRef != count()) {
        throw NeoTLKError("Classic TLK entries must be appended in sequential StrRef order.");
    }
    prepareAddedEntry(entry, strRef, preferredTextEncoding_);
    entries_.push_back(std::move(entry));
    synchronizeCount();
    modified_ = true;
}

TalkString& TalkTable::ensureEntryAtStrRef(UInt32 strRef) {
    if (containsStrRef(strRef)) return entryAtStrRef(strRef);
    TalkString entry;
    addEntryAtStrRef(strRef, std::move(entry));
    return entryAtStrRef(strRef);
}

void TalkTable::replaceEntry(TalkString& entry) {
    if (!fileOpen_) throw NeoTLKError("Unable to modify entry. No TLK file is currently open!");
    for (TalkString& candidate : entries_) {
        if (&candidate == &entry) {
            normalizeEntryForSave(candidate);
            candidate.custom = true;
            modified_ = true;
            return;
        }
    }

    TalkString replacement = entry;
    TalkString& destination = entryAtStrRef(replacement.strRef);
    replacement.strRef = destination.strRef;
    replacement.offsetToString = destination.offsetToString;
    replacement.textEncoding = destination.textEncoding;
    normalizeEntryForSave(replacement);
    replacement.custom = true;
    destination = std::move(replacement);

    entry.textEncoding = destination.textEncoding;
    normalizeEntryForSave(entry);
    entry.custom = true;
    modified_ = true;
}

void TalkTable::replaceEntry(UInt32 strRef, const TalkString& replacement) {
    TalkString detached = replacement;
    detached.strRef = strRef;
    replaceEntry(detached);
}

void TalkTable::replaceAllEntries(std::vector<TalkString> entries) {
    if (!fileOpen_) {
        throw NeoTLKError("Unable to replace TLK entries. No TLK file is open!");
    }
    if (entries.empty()) {
        throw NeoTLKError(
            "A TLK document must contain at least one entry. A metadata-only filtered CSV/TSV can be merged into an existing TLK, but cannot create or replace a complete TLK document.");
    }
    if (entries.size() > static_cast<std::size_t>(std::numeric_limits<UInt32>::max())) {
        throw NeoTLKError("Unable to replace TLK entries. The table exceeds the 32-bit entry count limit!");
    }

    if (hasSparseStrRefs()) {
        std::sort(entries.begin(), entries.end(), [](const TalkString& left, const TalkString& right) {
            return left.strRef < right.strRef;
        });
        for (std::size_t index = 1; index < entries.size(); ++index) {
            if (entries[index - 1u].strRef == entries[index].strRef) {
                throw NeoTLKError("Unable to replace TLK entries because duplicate StrRef " +
                                  std::to_string(entries[index].strRef) + " was supplied.");
            }
        }
    } else {
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const UInt32 expected = checkedEntryCountToUInt32(index);
            if (entries[index].strRef != expected) {
                throw NeoTLKError("Dense TLK entries must be supplied in sequential StrRef order starting at zero.");
            }
        }
    }

    for (TalkString& entry : entries) {
        normalizeEntryForSave(entry);
        entry.custom = true;
        if (storageFormat_ == TlkStorageFormat::ClassicV30) {
            entry.soundId = 0xffffffffu;
        } else if (storageFormat_ == TlkStorageFormat::JadeV40) {
            entry.textEncoding = TextEncoding::Utf8;
            entry.flags = entry.text.empty() ? 0u : TEXT_PRESENT;
            entry.soundResref = {};
            entry.volumeVariance = 0u;
            entry.pitchVariance = 0u;
            entry.soundLength = 0.0f;
        } else {
            entry.textEncoding = TextEncoding::Utf8;
            entry.flags = entry.text.empty() ? 0u : TEXT_PRESENT;
            entry.soundResref = {};
            entry.soundId = 0xffffffffu;
            entry.volumeVariance = 0u;
            entry.pitchVariance = 0u;
            entry.soundLength = 0.0f;
        }
    }

    entries_.swap(entries);
    synchronizeCount();
    modified_ = true;
}

void TalkTable::deleteEntry(UInt32 strRef) {
    if (!fileOpen_) throw NeoTLKError("Unable to delete entry. No TLK file is open!");
    const auto found = std::find_if(entries_.begin(), entries_.end(), [strRef](const TalkString& entry) {
        return entry.strRef == strRef;
    });
    if (found == entries_.end()) throw NeoTLKError("Invalid StrRef of entry to delete!");

    const std::size_t deleteIndex = static_cast<std::size_t>(std::distance(entries_.begin(), found));
    std::vector<TalkString> nextEntries;
    try {
        nextEntries.reserve(entries_.size() - 1u);
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            if (index == deleteIndex) continue;
            TalkString copy;
            copy.cloneFrom(entries_[index]);
            if (!hasSparseStrRefs() && index > deleteIndex) --copy.strRef;
            nextEntries.push_back(std::move(copy));
        }
    } catch (const std::exception&) {
        throw NeoTLKError("Unable to delete entry. Not enough memory to prepare the delete operation safely!");
    }
    entries_.swap(nextEntries);
    synchronizeCount();
    modified_ = true;
}

void TalkTable::newFile() {
    reset();
    storageFormat_ = TlkStorageFormat::ClassicV30;
    preferredTextEncoding_ = TextEncoding::Windows1252;
    fileOpen_ = true;
    fileType_ = makeFourChar("TLK ");
    fileVersion_ = makeFourChar("V3.0");
    filename_ = "untitled.tlk";
    hasSaveTarget_ = false;
    modified_ = true;
}

void TalkTable::load(const std::string& filename) {
    rejectEmbeddedNulPath(filename, "Unable to load specified TLK file. The path contains an embedded NUL byte!");
    const std::filesystem::path inputPath = userPathFromString(filename);
    const std::filesystem::path loadedSaveTargetPath = resolveStableFilesystemTargetPath(
        inputPath,
        "Unable to resolve input TLK path safely!",
        "Unable to load specified TLK file since it could not be found!");
    rejectUnsafeLoadSource(loadedSaveTargetPath);
    const FileSignature initialLoadSignature = captureExistingRegularFileSignature(
        loadedSaveTargetPath,
        "Unable to load TLK file safely because the source file changed or became unavailable while it was being read!");

    std::ifstream input(loadedSaveTargetPath, std::ios::binary);
    if (!input) {
        throw NeoTLKError("Unable to load specified TLK file since it could not be found!");
    }

    input.seekg(0, std::ios::end);
    const std::streamoff fileSize = inputPosition(input);
    seekInput(input, 0);

    if (fileSize >= 20) {
        std::array<char, 20> probe{};
        readExact(input, probe.data(), probe.size(), "Unable to read TLK file header!");
        const bool dragonAgeV02 =
            std::string(probe.data(), 8) == "GFF V4.0" &&
            std::string(probe.data() + 12, 4) == "TLK " &&
            std::string(probe.data() + 16, 4) == "V0.2";
        seekInput(input, 0);
        if (dragonAgeV02) {
            loadDragonAgeV02(loadedSaveTargetPath, filename,
                             initialLoadSignature.size,
                             initialLoadSignature.writeTime,
                             initialLoadSignature.contentHash);
            return;
        }
    }

    std::array<char, 4> loadedFileType{};
    std::array<char, 4> loadedFileVersion{};
    UInt32 loadedLanguageId = 0;
    UInt32 loadedStringCount = 0;
    UInt32 loadedStringEntriesOffset = 0;
    TextEncoding loadedPreferredEncoding = TextEncoding::Utf8;
    std::vector<TalkString> loadedEntries;

    readExact(input, loadedFileType.data(), loadedFileType.size(), "Unable to read TLK file header!");
    readExact(input, loadedFileVersion.data(), loadedFileVersion.size(), "Unable to read TLK file header!");

    if (!isFourChar(loadedFileType, "TLK ")) {
        throw NeoTLKError("Type mismatch. Specified file is not a valid TLK file!");
    }

    const bool isV30 = isFourChar(loadedFileVersion, "V3.0");
    const bool isV40 = isFourChar(loadedFileVersion, "V4.0");
    if (!isV30 && !isV40) {
        throw NeoTLKError("Version mismatch. File is not a valid v3.0 or v4.0 TLK file!");
    }

    loadedLanguageId = readU32LE(input);
    loadedStringCount = readU32LE(input);

    if (loadedStringCount < 1) {
        throw NeoTLKError("No entries found in the specified TLK file!");
    }

    try {
        loadedEntries.reserve(loadedStringCount);
    } catch (const std::exception&) {
        throw NeoTLKError("Unable to load TLK file. The declared string count is too large for available memory!");
    }

    if (isV30) {
        loadedStringEntriesOffset = readU32LE(input);

        constexpr unsigned long long headerBytes = 20ull;
        constexpr unsigned long long entryBytes = 40ull;
        const unsigned long long minimumStringDataOffset = headerBytes + entryBytes * static_cast<unsigned long long>(loadedStringCount);
        if (static_cast<unsigned long long>(loadedStringEntriesOffset) < minimumStringDataOffset ||
            static_cast<unsigned long long>(loadedStringEntriesOffset) > static_cast<unsigned long long>(fileSize)) {
            throw NeoTLKError("Invalid TLK string data offset in file header!");
        }

        for (UInt32 i = 0; i < loadedStringCount; ++i) {
            if (static_cast<UInt32>(inputPosition(input)) >= loadedStringEntriesOffset) {
                throw NeoTLKError("Error reading string data table. Overflow into entry data table!");
            }

            TalkString entry;
            entry.flags = readU32LE(input);
            readExact(input, entry.soundResref.bytes.data(), entry.soundResref.bytes.size(), "Unexpected end of file while reading TLK sound ResRef!");
            entry.volumeVariance = readU32LE(input);
            entry.pitchVariance = readU32LE(input);
            entry.offsetToString = readU32LE(input);
            entry.stringSize = readU32LE(input);
            entry.soundLength = readFloatLE(input);
            entry.soundId = 0xffffffffu;
            entry.strRef = i;

            loadedEntries.push_back(std::move(entry));
        }

        if (fileSize < static_cast<std::streamoff>(loadedStringEntriesOffset)) {
            throw NeoTLKError("Invalid TLK string data offset in file header!");
        }
        const auto stringDataBlockSize = static_cast<unsigned long long>(fileSize - static_cast<std::streamoff>(loadedStringEntriesOffset));
        if (stringDataBlockSize > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
            throw NeoTLKError("Unable to load TLK file. The string data block is too large for this platform!");
        }

        std::string stringDataBlock;
        try {
            stringDataBlock.resize(static_cast<std::size_t>(stringDataBlockSize));
        } catch (const std::bad_alloc&) {
            throw NeoTLKError("Unable to load TLK file. The string data block is too large for available memory!");
        }
        if (!stringDataBlock.empty()) {
            seekInput(input, static_cast<std::streamoff>(loadedStringEntriesOffset));
            readExact(input, stringDataBlock.data(), stringDataBlock.size(), "Unexpected end of file while reading TLK string data block!");
        }

        for (TalkString& entry : loadedEntries) {
            if (entry.stringSize == 0) {
                entry.text.clear();
                continue;
            }
            const unsigned long long offset = static_cast<unsigned long long>(entry.offsetToString);
            const unsigned long long size = static_cast<unsigned long long>(entry.stringSize);
            if (offset > stringDataBlockSize || size > stringDataBlockSize - offset) {
                throw NeoTLKError("Invalid TLK string payload range. Declared string data extends beyond the end of the file!");
            }
            try {
                entry.text.assign(stringDataBlock.data() + static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
            } catch (const std::bad_alloc&) {
                throw NeoTLKError("Unable to load TLK file. A string payload is too large for available memory!");
            }
        }

        validateStringPayloadRanges(loadedEntries, loadedStringEntriesOffset, fileSize);

        std::vector<std::string> rawPayloads;
        rawPayloads.reserve(loadedEntries.size());
        for (const TalkString& entry : loadedEntries) rawPayloads.push_back(entry.text);
        loadedPreferredEncoding = detectClassicPreferredEncoding(loadedLanguageId, false, rawPayloads);
        for (TalkString& entry : loadedEntries) {
            const std::string raw = std::move(entry.text);
            entry.textEncoding = detectClassicEntryEncoding(raw, loadedPreferredEncoding, loadedLanguageId);
            try {
                entry.text = decodeTextBytes(raw, entry.textEncoding);
            } catch (const std::exception& ex) {
                throw NeoTLKError(std::string("Unable to decode TLK string #") + std::to_string(entry.strRef) + ": " + ex.what());
            }
        }
    } else {
        const UInt32 entryTableOffset = readU32LE(input);
        const UInt32 stringDataOffset = readU32LE(input);
        const UInt32 reserved0 = readU32LE(input);
        const UInt32 reserved1 = readU32LE(input);
        (void)reserved0;
        (void)reserved1;

        constexpr unsigned long long headerBytes = 32ull;
        constexpr unsigned long long entryBytes = 10ull;
        const unsigned long long entryTableEnd = static_cast<unsigned long long>(entryTableOffset) +
            entryBytes * static_cast<unsigned long long>(loadedStringCount);
        const unsigned long long fileSizeUnsigned = static_cast<unsigned long long>(fileSize);

        if (static_cast<unsigned long long>(entryTableOffset) < headerBytes ||
            entryTableEnd < static_cast<unsigned long long>(entryTableOffset) ||
            entryTableEnd > static_cast<unsigned long long>(stringDataOffset) ||
            static_cast<unsigned long long>(stringDataOffset) > fileSizeUnsigned) {
            throw NeoTLKError("Invalid TLK V4.0 table or string-data offset in file header!");
        }

        seekInput(input, static_cast<std::streamoff>(entryTableOffset));
        for (UInt32 i = 0; i < loadedStringCount; ++i) {
            TalkString entry;
            entry.soundId = readU32LE(input);
            const UInt32 absoluteStringOffset = readU32LE(input);
            entry.stringSize = readU16LE(input);
            entry.offsetToString = absoluteStringOffset;
            entry.strRef = i;
            entry.flags = entry.stringSize == 0 ? 0u : TEXT_PRESENT;

            if (entry.stringSize != 0) {
                const unsigned long long offset = static_cast<unsigned long long>(absoluteStringOffset);
                const unsigned long long size = static_cast<unsigned long long>(entry.stringSize);
                if (offset < static_cast<unsigned long long>(stringDataOffset) ||
                    offset > fileSizeUnsigned ||
                    size > fileSizeUnsigned - offset) {
                    throw NeoTLKError("Invalid TLK V4.0 string payload range. Declared string data extends beyond the end of the file!");
                }
            }

            loadedEntries.push_back(std::move(entry));
        }

        for (TalkString& entry : loadedEntries) {
            if (entry.stringSize == 0) {
                entry.text.clear();
                continue;
            }
            seekInput(input, static_cast<std::streamoff>(entry.offsetToString));
            try {
                entry.text.resize(entry.stringSize);
            } catch (const std::bad_alloc&) {
                throw NeoTLKError("Unable to load TLK file. A string payload is too large for available memory!");
            }
            readExact(input, entry.text.data(), entry.text.size(), "Unexpected end of file while reading TLK V4.0 string text!");
            if (!isValidUtf8(entry.text)) {
                throw NeoTLKError("Invalid Jade Empire TLK V4.0 string payload: text is not valid UTF-8.");
            }
            entry.textEncoding = TextEncoding::Utf8;
        }

        loadedStringEntriesOffset = stringDataOffset;
        loadedPreferredEncoding = TextEncoding::Utf8;
    }

    const std::string loadedSaveTargetFilename = stableAbsolutePathString(loadedSaveTargetPath);
    if (!existingRegularFileSignatureMatches(loadedSaveTargetPath, initialLoadSignature)) {
        throw NeoTLKError("Unable to load TLK file safely because the source file changed while it was being read!");
    }
    const FileSignature loadedSaveTargetSignature = initialLoadSignature;

    storageFormat_ = isV40 ? TlkStorageFormat::JadeV40 : TlkStorageFormat::ClassicV30;
    preferredTextEncoding_ = loadedPreferredEncoding;
    dragonAgeBacking_.reset();
    dragonAgeEntryPrototype_.reset();
    dragonAgeReservedStructs_.clear();
    fileType_ = loadedFileType;
    fileVersion_ = loadedFileVersion;
    languageId_ = loadedLanguageId;
    stringCount_ = loadedStringCount;
    stringEntriesOffset_ = loadedStringEntriesOffset;
    entries_ = std::move(loadedEntries);
    filename_ = filename;
    saveTargetFilename_ = loadedSaveTargetFilename;
    hasSaveTarget_ = true;
    saveTargetSnapshotValid_ = loadedSaveTargetSignature.valid;
    saveTargetSize_ = loadedSaveTargetSignature.size;
    saveTargetWriteTime_ = loadedSaveTargetSignature.writeTime;
    saveTargetContentHash_ = loadedSaveTargetSignature.contentHash;
    fileOpen_ = true;
    modified_ = false;
}

void TalkTable::loadDragonAgeV02(const std::filesystem::path& sourcePath,
                                      const std::string& displayFilename,
                                      std::uintmax_t sourceSize,
                                      const std::filesystem::file_time_type& sourceWriteTime,
                                      std::uint64_t sourceContentHash) {
    auto backing = std::make_unique<neogff::GffFile>();
    try {
        backing->LoadFile(sourcePath);
    } catch (const std::exception& ex) {
        throw NeoTLKError(std::string("Unable to read Dragon Age TLK V0.2 file: ") + ex.what());
    }
    if (!backing->isGff4() || backing->filetype() != "TLK " || backing->version() != "V0.2") {
        throw NeoTLKError("Type/version mismatch. File is not a supported Dragon Age GFF TLK V0.2 file!");
    }

    neogff::GffList* list = findDragonAgeStringList(*backing);
    if (list == nullptr) {
        throw NeoTLKError("Dragon Age TLK V0.2 does not contain its string record list.");
    }

    UInt32 listLabelId = list->hasGff4LabelId ? list->gff4LabelId : 19001u;
    UInt32 idLabelId = 19002u;
    UInt32 textLabelId = 19003u;
    std::vector<TalkString> loadedEntries;
    std::vector<std::unique_ptr<neogff::GffStruct>> reservedStructs;
    std::unique_ptr<neogff::GffStruct> prototype;
    std::unordered_set<UInt32> seenStrRefs;

    loadedEntries.reserve(list->count());
    for (const auto& ownedStruct : list->allStructs()) {
        if (!ownedStruct) throw NeoTLKError("Dragon Age TLK V0.2 contains a null string record.");
        const neogff::GffStruct& structure = *ownedStruct;
        const auto* idFieldBase = findGff4Field(structure, idLabelId, neogff::FIELD_TYPE_DWORD);
        const auto* textFieldBase = findGff4Field(structure, textLabelId, neogff::FIELD_TYPE_CEXOSTRING);
        const auto* idField = dynamic_cast<const neogff::GffUInt32Field*>(idFieldBase);
        const auto* textField = dynamic_cast<const neogff::GffExoStringField*>(textFieldBase);
        if (idField == nullptr || textField == nullptr) {
            throw NeoTLKError("Dragon Age TLK V0.2 string record is missing its ID or text field.");
        }
        if (idFieldBase->hasGff4LabelId) idLabelId = idFieldBase->gff4LabelId;
        if (textFieldBase->hasGff4LabelId) textLabelId = textFieldBase->gff4LabelId;
        if (!prototype) prototype = cloneGffStruct(structure);

        if (idField->value == std::numeric_limits<UInt32>::max()) {
            reservedStructs.push_back(cloneGffStruct(structure));
            continue;
        }
        if (!seenStrRefs.insert(idField->value).second) {
            throw NeoTLKError("Dragon Age TLK V0.2 contains duplicate string IDs.");
        }

        TalkString entry;
        entry.flags = TEXT_PRESENT;
        entry.strRef = idField->value;
        entry.text = textField->GetString();
        if (!isValidUtf8(entry.text)) {
            throw NeoTLKError("Dragon Age TLK V0.2 contains text that could not be represented as UTF-8.");
        }
        entry.textEncoding = TextEncoding::Utf8;
        entry.stringSize = checkedSizeToUInt32(entry.text.size());
        loadedEntries.push_back(std::move(entry));
    }

    const FileSignature expected{true, sourceSize, sourceWriteTime, sourceContentHash};
    if (!existingRegularFileSignatureMatches(sourcePath, expected)) {
        throw NeoTLKError("Unable to load TLK file safely because the source file changed while it was being read!");
    }

    storageFormat_ = TlkStorageFormat::DragonAgeV02;
    preferredTextEncoding_ = TextEncoding::Utf8;
    fileType_ = makeFourChar("TLK ");
    fileVersion_ = makeFourChar("V0.2");
    languageId_ = 0u;
    stringEntriesOffset_ = 0u;
    entries_ = std::move(loadedEntries);
    synchronizeCount();
    filename_ = displayFilename;
    saveTargetFilename_ = stableAbsolutePathString(sourcePath);
    hasSaveTarget_ = true;
    saveTargetSnapshotValid_ = true;
    saveTargetSize_ = sourceSize;
    saveTargetWriteTime_ = sourceWriteTime;
    saveTargetContentHash_ = sourceContentHash;
    fileOpen_ = true;
    modified_ = false;
    dragonAgeBacking_ = std::move(backing);
    dragonAgeEntryPrototype_ = std::move(prototype);
    dragonAgeReservedStructs_ = std::move(reservedStructs);
    dragonAgeListLabelId_ = listLabelId;
    dragonAgeIdLabelId_ = idLabelId;
    dragonAgeTextLabelId_ = textLabelId;
}

void TalkTable::prepareDragonAgeV02ForSave() {
    if (!dragonAgeBacking_) throw NeoTLKError("Dragon Age TLK backing GFF is unavailable.");
    neogff::GffList* list = findDragonAgeStringList(*dragonAgeBacking_, dragonAgeListLabelId_);
    if (list == nullptr) throw NeoTLKError("Dragon Age TLK string record list is unavailable.");
    if (!entries_.empty() && !dragonAgeEntryPrototype_) {
        throw NeoTLKError("Dragon Age TLK has no record template for writing entries.");
    }

    std::vector<std::unique_ptr<neogff::GffStruct>> nextStructs;
    nextStructs.reserve(dragonAgeReservedStructs_.size() + entries_.size());
    for (const auto& reserved : dragonAgeReservedStructs_) {
        if (reserved) nextStructs.push_back(cloneGffStruct(*reserved));
    }
    for (const TalkString& entry : entries_) {
        std::unique_ptr<neogff::GffStruct> record = cloneGffStruct(*dragonAgeEntryPrototype_);
        auto* idField = dynamic_cast<neogff::GffUInt32Field*>(
            findGff4Field(*record, dragonAgeIdLabelId_, neogff::FIELD_TYPE_DWORD));
        auto* textField = dynamic_cast<neogff::GffExoStringField*>(
            findGff4Field(*record, dragonAgeTextLabelId_, neogff::FIELD_TYPE_CEXOSTRING));
        if (idField == nullptr || textField == nullptr) {
            throw NeoTLKError("Dragon Age TLK record template is missing its ID or text field.");
        }
        if (!isValidUtf8(entry.text)) {
            throw NeoTLKError("Edited Dragon Age TLK text is not valid UTF-8.");
        }
        idField->value = entry.strRef;
        textField->SetString(entry.text);
        nextStructs.push_back(std::move(record));
    }
    list->allStructs() = std::move(nextStructs);
    dragonAgeBacking_->dirty(true);
}

void TalkTable::save(const std::string& filename) {
    if (!fileOpen_) {
        throw NeoTLKError("There is no open file to save!");
    }

    const bool explicitOutputFilename = !filename.empty();
    std::string outputFilename = filename;
    if (outputFilename.empty()) {
        if (!hasSaveTarget_) {
            throw NeoTLKError("Unable to create output TLK file. No save target has been selected!");
        }
        outputFilename = saveTargetFilename_.empty() ? filename_ : saveTargetFilename_;
    }
    if (outputFilename.empty()) {
        throw NeoTLKError("Unable to create output TLK file!");
    }
    rejectEmbeddedNulPath(outputFilename, "Unable to create output TLK file. The path contains an embedded NUL byte!");

    if (!isFourChar(fileType_, "TLK ")) {
        throw NeoTLKError("There is no valid TLK file currently open to save!");
    }

    if (entries_.empty() && !isDragonAgeV02()) {
        throw NeoTLKError("There is no current data in the TLK file to write!");
    }

    const std::filesystem::path requestedOutputPath = userPathFromString(outputFilename);
    const std::filesystem::path outputPath = resolveStableFilesystemTargetPath(
        requestedOutputPath,
        "Unable to resolve output TLK path safely!",
        "Unable to create output TLK file. The output directory does not exist!");
    const std::string stableOutputPath = stableAbsolutePathString(outputPath);
    const bool savingKnownTarget = hasSaveTarget_ && !saveTargetFilename_.empty() && stableOutputPath == saveTargetFilename_;
    const FileSignature rememberedSaveTarget{saveTargetSnapshotValid_, saveTargetSize_, saveTargetWriteTime_, saveTargetContentHash_};
    rejectUnsafeExistingSaveTarget(outputPath);
    rejectExistingNonTlkOutputTarget(outputPath);
    const FileSignature outputTargetAtSaveStart = captureOptionalExistingRegularFileSignature(
        outputPath,
        "Unable to inspect output TLK file safely before overwrite!");

    if (savingKnownTarget && !existingRegularFileSignatureMatches(outputPath, rememberedSaveTarget)) {
        throw NeoTLKError("Refusing to overwrite the TLK file because it has changed on disk since it was loaded or last saved. Reload the file or use Save As to write a separate copy.");
    }

    if (!explicitOutputFilename && !modified_) {
        std::error_code ec;
        const std::filesystem::file_status status = std::filesystem::symlink_status(outputPath, ec);
        if (!ec && std::filesystem::exists(status) && std::filesystem::is_regular_file(status)) {
            return;
        }
    }


    std::vector<TalkString> workingEntries = entries_;
    UInt32 workingStringEntriesOffset = stringEntriesOffset_;
    for (TalkString& entry : workingEntries) {
        normalizeEntryForSave(entry);
    }

    const std::filesystem::perms previousPermissions = existingFilePermissionsOrUnknown(outputPath);
    TemporarySaveFile temporary = makeTemporarySaveFile(outputPath);

    try {
        if (isDragonAgeV02()) {
            prepareDragonAgeV02ForSave();
            try {
                dragonAgeBacking_->SaveFile(temporary.file);
            } catch (const std::exception& ex) {
                throw NeoTLKError(std::string("Unable to write Dragon Age TLK V0.2 file: ") + ex.what());
            }
        } else {
            std::ofstream output(temporary.file, std::ios::binary | std::ios::out | std::ios::trunc);
            if (!output) throw NeoTLKError("Unable to create output TLK file!");
            if (isVersion40()) {
                writeTlkV40Image(output, fileType_, fileVersion_, languageId_, workingEntries, workingStringEntriesOffset);
            } else {
                writeTlkV30Image(output, fileType_, fileVersion_, languageId_, workingEntries, workingStringEntriesOffset);
            }
            output.flush();
            if (!output) throw NeoTLKError("Unable to flush output TLK file!");
        }
        applyExistingPermissionsToTemporary(temporary.file, previousPermissions);
        flushPathToStableStorage(temporary.file, "Unable to flush output TLK file to stable storage!");
        flushDirectoryBestEffort(temporary.directory);
        rejectUnsafeExistingSaveTarget(outputPath);
        rejectExistingNonTlkOutputTarget(outputPath);
        if (savingKnownTarget && !existingRegularFileSignatureMatches(outputPath, rememberedSaveTarget)) {
            throw NeoTLKError("Refusing to overwrite the TLK file because it changed while the save operation was being prepared. Reload the file or use Save As to write a separate copy.");
        }
        if (!outputTargetStillMatchesStart(outputPath, outputTargetAtSaveStart)) {
            throw NeoTLKError("Refusing to overwrite the output TLK file because it changed or appeared while the save operation was being prepared. Choose another output path or retry after reviewing the file.");
        }
        replaceFileWithTemporary(temporary.file, outputPath);
        temporary.file.clear();
        removeTemporarySaveFileBestEffort(temporary);
        flushDirectoryBestEffort(outputPath.has_parent_path() ? outputPath.parent_path() : std::filesystem::path("."));
    } catch (...) {
        removeTemporarySaveFileBestEffort(temporary);
        throw;
    }

    entries_ = std::move(workingEntries);
    stringEntriesOffset_ = workingStringEntriesOffset;
    synchronizeCount();
    filename_ = outputFilename;
    saveTargetFilename_ = stableOutputPath;
    const FileSignature savedSignature = captureExistingRegularFileSignature(outputPath, "Unable to verify the saved TLK file after replacement!");
    saveTargetSnapshotValid_ = savedSignature.valid;
    saveTargetSize_ = savedSignature.size;
    saveTargetWriteTime_ = savedSignature.writeTime;
    saveTargetContentHash_ = savedSignature.contentHash;
    hasSaveTarget_ = true;
    modified_ = false;
}

void TalkTable::reset() {
    storageFormat_ = TlkStorageFormat::ClassicV30;
    preferredTextEncoding_ = TextEncoding::Windows1252;
    fileType_.fill('\0');
    fileVersion_.fill('\0');
    languageId_ = 0;
    stringCount_ = 0;
    stringEntriesOffset_ = 0;
    entries_.clear();
    filename_.clear();
    saveTargetFilename_.clear();
    dragonAgeBacking_.reset();
    dragonAgeEntryPrototype_.reset();
    dragonAgeReservedStructs_.clear();
    dragonAgeListLabelId_ = 0u;
    dragonAgeIdLabelId_ = 0u;
    dragonAgeTextLabelId_ = 0u;
    saveTargetSnapshotValid_ = false;
    saveTargetSize_ = 0;
    saveTargetWriteTime_ = {};
    saveTargetContentHash_ = 0;
    fileOpen_ = false;
    modified_ = false;
    hasSaveTarget_ = false;
}

UInt32 TalkTable::appendFrom(const TalkTable& other) {
    if (!fileOpen_) {
        throw NeoTLKError("Unable to add new entry. No TLK file is open!");
    }
    if (!other.fileOpen_) {
        throw NeoTLKError("Unable to append entries. The source TLK file is not open!");
    }

    const std::vector<TalkString> sourceEntries = (&other == this) ? entries_ : other.entries();
    if (sourceEntries.empty()) {
        return 0;
    }
    if (sourceEntries.size() > static_cast<std::size_t>(std::numeric_limits<UInt32>::max()) - entries_.size()) {
        throw NeoTLKError("Unable to append entries. The resulting TLK would exceed the 32-bit entry count limit!");
    }

    const UInt32 startCount = hasSparseStrRefs() ? nextAvailableStrRef() : count();
    std::vector<TalkString> nextEntries;
    try {
        nextEntries = entries_;
        nextEntries.reserve(entries_.size() + sourceEntries.size());
        for (std::size_t i = 0; i < sourceEntries.size(); ++i) {
            TalkString newEntry;
            newEntry.cloneFrom(sourceEntries[i]);
            prepareAddedEntry(newEntry, startCount + checkedEntryCountToUInt32(i), preferredTextEncoding_);
            nextEntries.push_back(std::move(newEntry));
        }
    } catch (const std::exception&) {
        throw NeoTLKError("Unable to append entries. Not enough memory to prepare the append operation safely!");
    }

    entries_.swap(nextEntries);
    synchronizeCount();
    modified_ = true;
    return checkedEntryCountToUInt32(sourceEntries.size());
}

UInt32 TalkTable::padToStrRef(UInt32 targetStrRef) {
    if (!fileOpen_) {
        throw NeoTLKError("Unable to pad entries. No TLK file is open!");
    }
    if (hasSparseStrRefs()) {
        throw NeoTLKError("Pad to StrRef is not applicable to sparse Dragon Age TLK V0.2 files. Add the desired string ID directly instead.");
    }
    if (targetStrRef <= count()) {
        throw NeoTLKError("Invalid StrRef specified! You must specify a number at least 2 bigger than the current last StrRef!");
    }

    const UInt32 startCount = count();
    const UInt32 addedCount = targetStrRef - startCount;
    std::vector<TalkString> nextEntries;
    try {
        nextEntries = entries_;
        nextEntries.reserve(static_cast<std::size_t>(targetStrRef));
        for (UInt32 i = startCount; i < targetStrRef; ++i) {
            TalkString padEntry;
            prepareAddedEntry(padEntry, i, preferredTextEncoding_);
            nextEntries.push_back(std::move(padEntry));
        }
    } catch (const std::exception&) {
        throw NeoTLKError("Unable to pad entries. Not enough memory to prepare the pad operation safely!");
    }

    entries_.swap(nextEntries);
    synchronizeCount();
    modified_ = true;
    return addedCount;
}

void TalkTable::synchronizeCount() {
    stringCount_ = checkedEntryCountToUInt32(entries_.size());
}

std::string fourCharToString(const std::array<char, 4>& value) {
    return std::string(value.data(), value.size());
}

} // namespace neotlk
