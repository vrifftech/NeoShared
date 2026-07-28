#include "Common.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace neogff {

namespace {

#ifdef _WIN32
bool isWindowsReparsePoint(const std::filesystem::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool isWindowsReparsePointHandle(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(handle, &info) == FALSE) {
        return true;
    }
    return (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool sameWindowsFileSnapshot(const BY_HANDLE_FILE_INFORMATION& before,
                             const BY_HANDLE_FILE_INFORMATION& after) {
    return before.dwVolumeSerialNumber == after.dwVolumeSerialNumber &&
           before.nFileIndexHigh == after.nFileIndexHigh &&
           before.nFileIndexLow == after.nFileIndexLow &&
           before.nFileSizeHigh == after.nFileSizeHigh &&
           before.nFileSizeLow == after.nFileSizeLow &&
           before.ftLastWriteTime.dwLowDateTime == after.ftLastWriteTime.dwLowDateTime &&
           before.ftLastWriteTime.dwHighDateTime == after.ftLastWriteTime.dwHighDateTime &&
           before.ftCreationTime.dwLowDateTime == after.ftCreationTime.dwLowDateTime &&
           before.ftCreationTime.dwHighDateTime == after.ftCreationTime.dwHighDateTime;
}
#else
bool isWindowsReparsePoint(const std::filesystem::path&) {
    return false;
}
#endif

#ifndef _WIN32
bool sameRegularFileSnapshot(const struct stat& before, const struct stat& after) {
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino || before.st_size != after.st_size) {
        return false;
    }
#if defined(__APPLE__) || defined(__MACH__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    return before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
           before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
           before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
           before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#elif defined(_AIX)
    return before.st_mtime == after.st_mtime && before.st_ctime == after.st_ctime;
#else
    return before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
}
#endif

std::invalid_argument invalidInteger(const std::string& text) {
    return std::invalid_argument("'" + text + "' is not a valid integer value");
}

std::invalid_argument invalidFloat(const std::string& text) {
    return std::invalid_argument("'" + text + "' is not a valid floating point value");
}

template <typename T>
T parseIntegralDecimal(const std::string& text) {
    if (text.empty()) {
        throw invalidInteger(text);
    }

    T value{};
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value, 10);
    if (ec != std::errc{} || ptr != last) {
        throw invalidInteger(text);
    }
    return value;
}

template <typename T>
T parseFloatingDecimal(const std::string& text) {
    if (text.empty()) {
        throw invalidFloat(text);
    }

    T value{};
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value, std::chars_format::general);
    if (ec != std::errc{} || ptr != last || !std::isfinite(static_cast<double>(value))) {
        throw invalidFloat(text);
    }
    return value;
}

template <typename T>
std::string formatFloating(T value) {
    if (!std::isfinite(static_cast<double>(value))) {
        return std::to_string(static_cast<double>(value));
    }

    std::array<char, 64> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general);
    if (ec == std::errc{}) {
        return std::string(buffer.data(), ptr);
    }

    return std::to_string(static_cast<double>(value));
}

std::string trimAscii(std::string value) {
    const auto notSpace = [](unsigned char ch) { return std::isspace(ch) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

template <std::size_t ComponentCount>
std::array<float, ComponentCount> parseGffVectorText(const std::string& text,
                                                      const char* fieldName) {
    std::array<float, ComponentCount> result{};
    std::size_t start = 0;

    for (std::size_t index = 0; index < ComponentCount; ++index) {
        const std::size_t separator = text.find('|', start);
        const bool finalComponent = index + 1u == ComponentCount;

        if ((!finalComponent && separator == std::string::npos) ||
            (finalComponent && separator != std::string::npos)) {
            throw std::invalid_argument(
                std::string(fieldName) + " requires exactly " +
                std::to_string(ComponentCount) + " pipe-separated numbers.");
        }

        const std::size_t end = separator == std::string::npos ? text.size() : separator;
        const std::string component = trimAscii(text.substr(start, end - start));
        if (component.empty()) {
            throw std::invalid_argument(std::string(fieldName) +
                                        " cannot contain an empty component.");
        }

        result[index] = ParseFloatDecimal(component);
        start = end + 1u;
    }

    return result;
}

template <std::size_t ComponentCount>
std::string formatGffVectorText(const std::array<float, ComponentCount>& value) {
    std::string text;
    for (std::size_t index = 0; index < ComponentCount; ++index) {
        if (index != 0u) text.push_back('|');
        text += FormatNumber(value[index]);
    }
    return text;
}

} // namespace

bool IsUnsignedDecimal(const std::string& text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

bool IsSignedDecimal(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    const std::size_t start = (text.front() == '-' || text.front() == '+') ? 1u : 0u;
    return start < text.size() &&
           std::all_of(text.begin() + static_cast<std::ptrdiff_t>(start), text.end(), [](unsigned char c) {
               return c >= '0' && c <= '9';
           });
}

bool IsDecimalNumber(const std::string& text) {
    try {
        (void)parseFloatingDecimal<double>(text);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::uint32_t ParseUInt32Decimal(const std::string& text) {
    return parseIntegralDecimal<std::uint32_t>(text);
}

std::uint64_t ParseUInt64Decimal(const std::string& text) {
    return parseIntegralDecimal<std::uint64_t>(text);
}

std::int32_t ParseInt32Decimal(const std::string& text) {
    return parseIntegralDecimal<std::int32_t>(text);
}

std::int64_t ParseInt64Decimal(const std::string& text) {
    return parseIntegralDecimal<std::int64_t>(text);
}

double ParseDoubleDecimal(const std::string& text) {
    return parseFloatingDecimal<double>(text);
}

float ParseFloatDecimal(const std::string& text) {
    return parseFloatingDecimal<float>(text);
}

GffVector3 ParseGffVector3Text(const std::string& text) {
    return parseGffVectorText<3>(text, "GFF Vector3");
}

GffVector4 ParseGffVector4Text(const std::string& text) {
    return parseGffVectorText<4>(text, "GFF Vector4");
}

std::string FormatGffVector3Text(const GffVector3& value) {
    return formatGffVectorText(value);
}

std::string FormatGffVector4Text(const GffVector4& value) {
    return formatGffVectorText(value);
}

std::filesystem::path ResolveOutputTarget(const std::filesystem::path& target) {
    if (target.empty()) {
        return target;
    }

    std::error_code ec;
    const auto st = std::filesystem::symlink_status(target, ec);
    if (!ec && std::filesystem::is_symlink(st)) {
        std::error_code canonicalEc;
        const auto resolved = std::filesystem::canonical(target, canonicalEc);
        if (canonicalEc) {
            throw std::filesystem::filesystem_error("Unable to resolve symlink output target",
                                                    target,
                                                    canonicalEc);
        }
        return resolved;
    }

    // Also bind the parent directory to its real location. Otherwise a save to
    // a path under a directory symlink could allocate a temporary file in one
    // directory and then replace a different destination if that parent symlink
    // is changed before promotion. Existing callers use the resolved path for
    // both temp allocation and final replacement, so normal writes through a
    // directory symlink still update the linked directory's real target.
    const std::filesystem::path parent = target.parent_path().empty()
        ? std::filesystem::path(".")
        : target.parent_path();
    std::error_code parentEc;
    const auto canonicalParent = std::filesystem::weakly_canonical(parent, parentEc);
    if (!parentEc) {
        return (canonicalParent / target.filename()).lexically_normal();
    }
    std::error_code absEc;
    const auto absolute = std::filesystem::absolute(target, absEc);
    return absEc ? target : absolute.lexically_normal();
}

std::filesystem::path normalizeManagedTempPath(const std::filesystem::path& path);
void RegisterManagedTempPayload(const std::filesystem::path& file, const std::filesystem::path& target);
bool IsRegisteredManagedTempPayload(const std::filesystem::path& file);
bool ManagedTempPayloadMatchesTarget(const std::filesystem::path& file, const std::filesystem::path& target);
void UnregisterManagedTempPayload(const std::filesystem::path& file);


void WriteManagedTempMarkerFile(const std::filesystem::path& marker) {
    static constexpr char markerText[] = "NeoTools GFF temporary output directory\n";
#ifdef _WIN32
    HANDLE handle = CreateFileW(marker.c_str(),
                                GENERIC_WRITE,
                                0,
                                nullptr,
                                CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        throw std::filesystem::filesystem_error("Unable to create managed temporary directory marker",
                                                marker,
                                                std::error_code(static_cast<int>(error), std::system_category()));
    }
    DWORD written = 0;
    const DWORD toWrite = static_cast<DWORD>(sizeof(markerText) - 1);
    if (WriteFile(handle, markerText, toWrite, &written, nullptr) == FALSE || written != toWrite) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        throw std::filesystem::filesystem_error("Unable to write managed temporary directory marker",
                                                marker,
                                                std::error_code(static_cast<int>(error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error), std::system_category()));
    }
    if (FlushFileBuffers(handle) == FALSE) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        throw std::filesystem::filesystem_error("Unable to finalize managed temporary directory marker",
                                                marker,
                                                std::error_code(static_cast<int>(error), std::system_category()));
    }
    CloseHandle(handle);
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(marker.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        throw std::filesystem::filesystem_error("Unable to create managed temporary directory marker",
                                                marker,
                                                std::error_code(errno, std::generic_category()));
    }
    auto closeFd = [&]() noexcept {
        if (fd >= 0) {
            (void)close(fd);
            fd = -1;
        }
    };
    try {
        std::size_t done = 0;
        constexpr std::size_t len = sizeof(markerText) - 1;
        while (done < len) {
            const ssize_t n = write(fd, markerText + done, len - done);
            if (n > 0) {
                done += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            const int saved = errno == 0 ? EIO : errno;
            throw std::filesystem::filesystem_error("Unable to write managed temporary directory marker",
                                                    marker,
                                                    std::error_code(saved, std::generic_category()));
        }
        struct stat st{};
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
            const int saved = errno == 0 ? EINVAL : errno;
            throw std::filesystem::filesystem_error("Refusing unsafe managed temporary directory marker",
                                                    marker,
                                                    std::error_code(saved, std::generic_category()));
        }
        if (fsync(fd) != 0) {
            throw std::filesystem::filesystem_error("Unable to finalize managed temporary directory marker",
                                                    marker,
                                                    std::error_code(errno, std::generic_category()));
        }
        if (close(fd) != 0) {
            const int saved = errno;
            fd = -1;
            throw std::filesystem::filesystem_error("Unable to close managed temporary directory marker",
                                                    marker,
                                                    std::error_code(saved, std::generic_category()));
        }
        fd = -1;
    } catch (...) {
        closeFd();
        throw;
    }
#endif
}

std::filesystem::path MakeSiblingTempPath(const std::filesystem::path& target, const std::string& tag) {
    if (target.empty()) {
        throw std::runtime_error("Unable to derive a temporary path for an empty target filename.");
    }

    // Bind the managed payload to the resolved final destination at allocation
    // time. A temp payload created for one output file must not later be
    // accidentally promoted over a different sibling file.
    const std::filesystem::path resolvedTarget = ResolveOutputTarget(target);
    const auto parent = resolvedTarget.parent_path();
    const std::string filename = resolvedTarget.filename().empty() ? std::string("output") : resolvedTarget.filename().string();
    const auto now = static_cast<unsigned long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::random_device rd;

    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        std::ostringstream name;
        name << filename << "." << tag << "." << std::hex
             << (now ^ (static_cast<unsigned long long>(rd()) << 32) ^ attempt) << ".tmpdir";
        const std::filesystem::path tempDir = parent.empty()
            ? std::filesystem::path(name.str())
            : parent / name.str();

        bool created = false;
#ifdef _WIN32
        const std::string tempDirText = tempDir.string();
        if (CreateDirectoryA(tempDirText.c_str(), nullptr) != FALSE) {
            created = true;
        } else {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
                throw std::filesystem::filesystem_error("Unable to create temporary output directory",
                                                        tempDir,
                                                        std::error_code(static_cast<int>(error), std::system_category()));
            }
        }
#else
        if (mkdir(tempDir.c_str(), S_IRWXU) == 0) {
            created = true;
        } else if (errno != EEXIST) {
            throw std::filesystem::filesystem_error("Unable to create temporary output directory",
                                                    tempDir,
                                                    std::error_code(errno, std::generic_category()));
        }
#endif
        if (!created) {
            continue;
        }

#ifndef _WIN32
        std::error_code permEc;
        std::filesystem::permissions(tempDir,
                                     std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace,
                                     permEc);
        if (permEc) {
            std::error_code removeEc;
            std::filesystem::remove(tempDir, removeEc);
            throw std::filesystem::filesystem_error("Unable to restrict managed temporary directory permissions",
                                                    tempDir,
                                                    permEc);
        }
        std::error_code verifyPermEc;
        const auto restrictedStatus = std::filesystem::status(tempDir, verifyPermEc);
        const auto exposedPerms = restrictedStatus.permissions() &
            (std::filesystem::perms::group_all | std::filesystem::perms::others_all);
        if (verifyPermEc || exposedPerms != std::filesystem::perms::none) {
            std::error_code removeEc;
            std::filesystem::remove(tempDir, removeEc);
            throw std::filesystem::filesystem_error("Unable to verify managed temporary directory privacy",
                                                    tempDir,
                                                    verifyPermEc ? verifyPermEc : std::make_error_code(std::errc::permission_denied));
        }
#endif

        // Leave an internal marker so cleanup routines can distinguish a
        // NeoTools GFF-managed temp directory from an arbitrary user directory
        // whose name happens to end in .tmpdir. Without that marker, a public
        // cleanup helper could remove unrelated empty directories after a
        // caller passed a matching path shape.
        const std::filesystem::path marker = tempDir / ".neogff-managed-temp";
        try {
            WriteManagedTempMarkerFile(marker);
        } catch (...) {
            std::error_code removeMarkerEc;
            std::filesystem::remove(marker, removeMarkerEc);
            std::error_code removeDirEc;
            std::filesystem::remove(tempDir, removeDirEc);
            throw;
        }
        const std::filesystem::path payload = normalizeManagedTempPath(tempDir / "payload.tmp");
        RegisterManagedTempPayload(payload, resolvedTarget);
        return payload;
    }
    throw std::runtime_error("Unable to allocate a unique temporary path near " + target.string());
}

bool hasManagedTempDirectoryName(const std::filesystem::path& directory) {
    const std::string name = directory.filename().string();
    constexpr const char* suffix = ".tmpdir";
    const std::size_t suffixLen = std::strlen(suffix);
    return name.size() > suffixLen && name.compare(name.size() - suffixLen, suffixLen, suffix) == 0;
}

std::filesystem::path managedTempMarkerPath(const std::filesystem::path& directory) {
    return directory / ".neogff-managed-temp";
}

std::mutex& managedTempRegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::filesystem::path, std::filesystem::path>& managedTempRegistry() {
    static std::map<std::filesystem::path, std::filesystem::path> registry;
    return registry;
}

std::filesystem::path normalizeManagedTempPath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    return absolute.lexically_normal();
}

void RegisterManagedTempPayload(const std::filesystem::path& file, const std::filesystem::path& target) {
    std::lock_guard<std::mutex> lock(managedTempRegistryMutex());
    managedTempRegistry()[normalizeManagedTempPath(file)] = normalizeManagedTempPath(ResolveOutputTarget(target));
}

bool IsRegisteredManagedTempPayload(const std::filesystem::path& file) {
    std::lock_guard<std::mutex> lock(managedTempRegistryMutex());
    return managedTempRegistry().find(normalizeManagedTempPath(file)) != managedTempRegistry().end();
}

bool ManagedTempPayloadMatchesTarget(const std::filesystem::path& file, const std::filesystem::path& target) {
    std::lock_guard<std::mutex> lock(managedTempRegistryMutex());
    const auto it = managedTempRegistry().find(normalizeManagedTempPath(file));
    if (it == managedTempRegistry().end()) {
        return false;
    }
    return it->second == normalizeManagedTempPath(target);
}

void UnregisterManagedTempPayload(const std::filesystem::path& file) {
    std::lock_guard<std::mutex> lock(managedTempRegistryMutex());
    managedTempRegistry().erase(normalizeManagedTempPath(file));
}

bool isManagedTempPayloadPath(const std::filesystem::path& file) {
    const std::filesystem::path parent = file.parent_path();
    if (parent.empty() || file.filename() != "payload.tmp" || !hasManagedTempDirectoryName(parent)) {
        return false;
    }

    // Treat a managed temp directory as trusted only when the directory itself is
    // a real directory, not a symlink to a caller-controlled location. The
    // payload may already have been renamed away when cleanup calls this helper,
    // so the payload path is validated separately immediately before promotion.
    std::error_code parentEc;
    const auto parentStatus = std::filesystem::symlink_status(parent, parentEc);
    if (parentEc || !std::filesystem::is_directory(parentStatus) || std::filesystem::is_symlink(parentStatus) || isWindowsReparsePoint(parent)) {
        return false;
    }
#ifndef _WIN32
    struct stat parentStat{};
    if (stat(parent.c_str(), &parentStat) != 0 || parentStat.st_uid != geteuid()) {
        return false;
    }
    if ((parentStat.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return false;
    }
#endif

    std::error_code ec;
    const auto markerStatus = std::filesystem::symlink_status(managedTempMarkerPath(parent), ec);
    return !ec && std::filesystem::is_regular_file(markerStatus) && !std::filesystem::is_symlink(markerStatus);
}


void RemoveManagedTempParentNoThrow(const std::filesystem::path& file) {
    const bool registered = IsRegisteredManagedTempPayload(file);
    const std::filesystem::path parent = file.parent_path();
    if (!registered || !isManagedTempPayloadPath(file)) {
        if (registered) {
            UnregisterManagedTempPayload(file);
        }
        return;
    }
    std::error_code ec;
    std::filesystem::remove(managedTempMarkerPath(parent), ec);
    ec.clear();
    std::filesystem::remove(parent, ec);
    UnregisterManagedTempPayload(file);
}


void RejectHardLinkedRegularFile(const std::filesystem::path& file, const char* message) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(file.c_str(),
                                FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        throw std::filesystem::filesystem_error("Unable to inspect file link count",
                                                file,
                                                std::error_code(static_cast<int>(error), std::system_category()));
    }
    if (isWindowsReparsePointHandle(handle)) {
        CloseHandle(handle);
        throw std::filesystem::filesystem_error("Refusing to inspect link count for a reparse-point path",
                                                file,
                                                std::make_error_code(std::errc::invalid_argument));
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(handle, &info) == FALSE) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        throw std::filesystem::filesystem_error("Unable to inspect file link count",
                                                file,
                                                std::error_code(static_cast<int>(error), std::system_category()));
    }
    CloseHandle(handle);
    if (info.nNumberOfLinks > 1) {
        throw std::filesystem::filesystem_error(message,
                                                file,
                                                std::make_error_code(std::errc::too_many_links));
    }
#else
    struct stat st{};
    if (lstat(file.c_str(), &st) != 0) {
        throw std::filesystem::filesystem_error("Unable to inspect file link count",
                                                file,
                                                std::error_code(errno, std::generic_category()));
    }
    if (!S_ISREG(st.st_mode)) {
        throw std::filesystem::filesystem_error("Refusing to inspect link count for a non-file path",
                                                file,
                                                std::make_error_code(std::errc::invalid_argument));
    }
    if (st.st_nlink > 1) {
        throw std::filesystem::filesystem_error(message,
                                                file,
                                                std::make_error_code(std::errc::too_many_links));
    }
#endif
}

void FlushFileToDisk(const std::filesystem::path& file) {
    if (file.empty()) {
        throw std::runtime_error("Unable to flush an empty file path.");
    }
    std::error_code statusEc;
    const auto status = std::filesystem::symlink_status(file, statusEc);
    if (statusEc || !std::filesystem::is_regular_file(status)) {
        throw std::filesystem::filesystem_error("Refusing to flush a non-file temporary output",
                                                file,
                                                statusEc ? statusEc : std::make_error_code(std::errc::invalid_argument));
    }
#ifdef _WIN32
    HANDLE handle = CreateFileW(file.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        throw std::filesystem::filesystem_error("Unable to open temporary output for flush",
                                                file,
                                                std::error_code(static_cast<int>(error), std::system_category()));
    }
    if (GetFileType(handle) != FILE_TYPE_DISK || isWindowsReparsePointHandle(handle)) {
        CloseHandle(handle);
        throw std::filesystem::filesystem_error("Refusing to flush a non-file temporary output",
                                                file,
                                                std::make_error_code(std::errc::invalid_argument));
    }
    if (FlushFileBuffers(handle) == FALSE) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        throw std::filesystem::filesystem_error("Unable to flush temporary output to disk",
                                                file,
                                                std::error_code(static_cast<int>(error), std::system_category()));
    }
    CloseHandle(handle);
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
    const int fd = open(file.c_str(), flags);
    if (fd < 0) {
        throw std::filesystem::filesystem_error("Unable to open temporary output for fsync",
                                                file,
                                                std::error_code(errno, std::generic_category()));
    }
    struct stat openedStatus {};
    if (fstat(fd, &openedStatus) != 0 || !S_ISREG(openedStatus.st_mode)) {
        const int saved = errno == 0 ? EINVAL : errno;
        close(fd);
        throw std::filesystem::filesystem_error("Refusing to flush a non-file temporary output",
                                                file,
                                                std::error_code(saved, std::generic_category()));
    }
    if (fsync(fd) != 0) {
        const int saved = errno;
        close(fd);
        throw std::filesystem::filesystem_error("Unable to fsync temporary output",
                                                file,
                                                std::error_code(saved, std::generic_category()));
    }
    if (close(fd) != 0) {
        throw std::filesystem::filesystem_error("Unable to close temporary output after fsync",
                                                file,
                                                std::error_code(errno, std::generic_category()));
    }
#endif
}


SafeOutputFile::SafeOutputFile(const std::filesystem::path& path)
    : path_(normalizeManagedTempPath(path)), closed_(false) {
    if (!IsRegisteredManagedTempPayload(path_) || !isManagedTempPayloadPath(path_)) {
        throw std::filesystem::filesystem_error("Refusing to create unmanaged temporary output payload",
                                                path_,
                                                std::make_error_code(std::errc::invalid_argument));
    }
#ifdef _WIN32
    HANDLE handle = CreateFileW(path_.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                0,
                                nullptr,
                                CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        closed_ = true;
        throw std::filesystem::filesystem_error("Unable to create temporary output payload",
                                                path_,
                                                std::error_code(static_cast<int>(error), std::system_category()));
    }
    handle_ = handle;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(handle) != FILE_TYPE_DISK ||
        isWindowsReparsePointHandle(handle) ||
        GetFileInformationByHandle(handle, &info) == FALSE ||
        info.nNumberOfLinks != 1) {
        closeNoThrow();
        throw std::filesystem::filesystem_error("Refusing unsafe temporary output payload",
                                                path_,
                                                std::make_error_code(std::errc::invalid_argument));
    }
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd_ = open(path_.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd_ < 0) {
        const int saved = errno;
        closed_ = true;
        throw std::filesystem::filesystem_error("Unable to create temporary output payload",
                                                path_,
                                                std::error_code(saved, std::generic_category()));
    }
    struct stat st{};
    if (fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        const int saved = errno == 0 ? EINVAL : errno;
        closeNoThrow();
        throw std::filesystem::filesystem_error("Refusing unsafe temporary output payload",
                                                path_,
                                                std::error_code(saved, std::generic_category()));
    }
#endif
}

SafeOutputFile::~SafeOutputFile() {
    closeNoThrow();
}

SafeOutputFile::SafeOutputFile(SafeOutputFile&& other) noexcept
    : path_(std::move(other.path_)),
#ifdef _WIN32
      handle_(other.handle_),
#else
      fd_(other.fd_),
#endif
      position_(other.position_),
      closed_(other.closed_) {
#ifdef _WIN32
    other.handle_ = nullptr;
#else
    other.fd_ = -1;
#endif
    other.position_ = 0;
    other.closed_ = true;
}

SafeOutputFile& SafeOutputFile::operator=(SafeOutputFile&& other) noexcept {
    if (this != &other) {
        closeNoThrow();
        path_ = std::move(other.path_);
#ifdef _WIN32
        handle_ = other.handle_;
        other.handle_ = nullptr;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
        position_ = other.position_;
        closed_ = other.closed_;
        other.position_ = 0;
        other.closed_ = true;
    }
    return *this;
}

void SafeOutputFile::writeBytes(const void* data, std::size_t size) {
    if (size == 0) {
        return;
    }
    if (closed_) {
        throw std::filesystem::filesystem_error("Unable to write closed temporary output payload",
                                                path_,
                                                std::make_error_code(std::errc::bad_file_descriptor));
    }
    const char* ptr = static_cast<const char*>(data);
#ifdef _WIN32
    while (size > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (WriteFile(static_cast<HANDLE>(handle_), ptr, chunk, &written, nullptr) == FALSE || written == 0) {
            const DWORD error = GetLastError();
            throw std::filesystem::filesystem_error("Unable to write temporary output payload",
                                                    path_,
                                                    std::error_code(static_cast<int>(error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error), std::system_category()));
        }
        ptr += written;
        size -= written;
        position_ += written;
    }
#else
    while (size > 0) {
        const std::size_t request = std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t n = write(fd_, ptr, request);
        if (n > 0) {
            ptr += n;
            size -= static_cast<std::size_t>(n);
            position_ += static_cast<std::uint64_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        const int saved = errno == 0 ? EIO : errno;
        throw std::filesystem::filesystem_error("Unable to write temporary output payload",
                                                path_,
                                                std::error_code(saved, std::generic_category()));
    }
#endif
}

void SafeOutputFile::seek(std::uint64_t offset) {
    if (closed_) {
        throw std::filesystem::filesystem_error("Unable to seek closed temporary output payload",
                                                path_,
                                                std::make_error_code(std::errc::bad_file_descriptor));
    }
#ifdef _WIN32
    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(offset);
    if (SetFilePointerEx(static_cast<HANDLE>(handle_), target, nullptr, FILE_BEGIN) == FALSE) {
        const DWORD error = GetLastError();
        throw std::filesystem::filesystem_error("Unable to seek temporary output payload",
                                                path_,
                                                std::error_code(static_cast<int>(error), std::system_category()));
    }
#else
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        lseek(fd_, static_cast<off_t>(offset), SEEK_SET) == static_cast<off_t>(-1)) {
        throw std::filesystem::filesystem_error("Unable to seek temporary output payload",
                                                path_,
                                                std::error_code(errno, std::generic_category()));
    }
#endif
    position_ = offset;
}

std::uint64_t SafeOutputFile::position() const {
    return position_;
}

void SafeOutputFile::flush() {
    if (closed_) {
        throw std::filesystem::filesystem_error("Unable to flush closed temporary output payload",
                                                path_,
                                                std::make_error_code(std::errc::bad_file_descriptor));
    }
#ifdef _WIN32
    if (FlushFileBuffers(static_cast<HANDLE>(handle_)) == FALSE) {
        const DWORD error = GetLastError();
        throw std::filesystem::filesystem_error("Unable to flush temporary output payload",
                                                path_,
                                                std::error_code(static_cast<int>(error), std::system_category()));
    }
#else
    if (fsync(fd_) != 0) {
        throw std::filesystem::filesystem_error("Unable to flush temporary output payload",
                                                path_,
                                                std::error_code(errno, std::generic_category()));
    }
#endif
}

void SafeOutputFile::close() {
    if (closed_) {
        return;
    }
    flush();
#ifdef _WIN32
    if (CloseHandle(static_cast<HANDLE>(handle_)) == FALSE) {
        const DWORD error = GetLastError();
        handle_ = nullptr;
        closed_ = true;
        throw std::filesystem::filesystem_error("Unable to close temporary output payload",
                                                path_,
                                                std::error_code(static_cast<int>(error), std::system_category()));
    }
    handle_ = nullptr;
#else
    if (::close(fd_) != 0) {
        const int saved = errno;
        fd_ = -1;
        closed_ = true;
        throw std::filesystem::filesystem_error("Unable to close temporary output payload",
                                                path_,
                                                std::error_code(saved, std::generic_category()));
    }
    fd_ = -1;
#endif
    closed_ = true;
}

void SafeOutputFile::closeNoThrow() noexcept {
    if (closed_) {
        return;
    }
#ifdef _WIN32
    if (handle_ != nullptr) {
        FlushFileBuffers(static_cast<HANDLE>(handle_));
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
#else
    if (fd_ >= 0) {
        (void)fsync(fd_);
        (void)::close(fd_);
        fd_ = -1;
    }
#endif
    closed_ = true;
}

namespace {

void FlushDirectoryNoThrow(const std::filesystem::path& directory) {
#ifndef _WIN32
    const std::filesystem::path dir = directory.empty() ? std::filesystem::path(".") : directory;
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = open(dir.c_str(), flags);
    if (fd >= 0) {
        (void)fsync(fd);
        (void)close(fd);
    }
#else
    (void)directory;
#endif
}

} // namespace

void ReplaceFileWithTemp(const std::filesystem::path& tempFile, const std::filesystem::path& targetFile) {
    if (tempFile.empty() || targetFile.empty()) {
        throw std::runtime_error("Unable to replace file because the source or target path is empty.");
    }

    // Callers that want user-facing symlink semantics must resolve the output
    // target before allocating the sibling temp path.  Do not resolve here: if a
    // previously regular destination is swapped into a symlink between temp
    // allocation and promotion, following it at this boundary could mutate an
    // unrelated linked file instead of refusing the changed path.
    const std::filesystem::path finalTarget = targetFile;

    // Only promote payloads allocated by MakeSiblingTempPath(). This prevents the
    // public helper from accidentally moving arbitrary caller-supplied files over
    // user data and keeps replacement inside the private sibling temp-directory
    // model used by GFF/TLK/INI/backup writers.
    if (!IsRegisteredManagedTempPayload(tempFile) || !isManagedTempPayloadPath(tempFile)) {
        throw std::filesystem::filesystem_error("Refusing to replace target with an unmanaged temporary output",
                                                tempFile,
                                                finalTarget,
                                                std::make_error_code(std::errc::invalid_argument));
    }
    if (!ManagedTempPayloadMatchesTarget(tempFile, finalTarget)) {
        throw std::filesystem::filesystem_error("Refusing to replace a different target than the temporary output was allocated for",
                                                tempFile,
                                                finalTarget,
                                                std::make_error_code(std::errc::invalid_argument));
    }
    const std::filesystem::path tempRoot = normalizeManagedTempPath(tempFile.parent_path().parent_path());
    const std::filesystem::path targetParent = finalTarget.parent_path().empty()
        ? std::filesystem::path(".")
        : finalTarget.parent_path();
    const std::filesystem::path targetRoot = normalizeManagedTempPath(targetParent);
    if (tempRoot != targetRoot) {
        throw std::filesystem::filesystem_error("Refusing to replace target with temporary output from a different directory",
                                                tempFile,
                                                finalTarget,
                                                std::make_error_code(std::errc::invalid_argument));
    }

    std::error_code tempInitialEc;
    const auto tempInitialStatus = std::filesystem::symlink_status(tempFile, tempInitialEc);
    if (tempInitialEc) {
        throw std::filesystem::filesystem_error("Unable to inspect temporary output file",
                                                tempFile,
                                                tempInitialEc);
    }
    if (!std::filesystem::is_regular_file(tempInitialStatus) || std::filesystem::is_symlink(tempInitialStatus)) {
        throw std::filesystem::filesystem_error("Refusing to replace target with a non-file temporary output",
                                                tempFile,
                                                finalTarget,
                                                std::make_error_code(std::errc::invalid_argument));
    }
    RejectHardLinkedRegularFile(tempFile, "Refusing to replace target with a hard-linked temporary output");
    FlushFileToDisk(tempFile);

    std::error_code equivalentEc;
    if (std::filesystem::equivalent(tempFile, finalTarget, equivalentEc) && !equivalentEc) {
        throw std::filesystem::filesystem_error("Refusing to replace a file with itself",
                                                tempFile,
                                                finalTarget,
                                                std::make_error_code(std::errc::invalid_argument));
    }

#ifdef _WIN32
    const DWORD oldAttrs = GetFileAttributesW(finalTarget.c_str());
    const bool hadAttrs = oldAttrs != INVALID_FILE_ATTRIBUTES;
    bool clearedReadOnly = false;
    if (hadAttrs) {
        std::error_code statusEc;
        const auto oldStatus = std::filesystem::status(finalTarget, statusEc);
        if (statusEc || !std::filesystem::is_regular_file(oldStatus) ||
            (oldAttrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            throw std::filesystem::filesystem_error("Refusing to replace a non-file target",
                                                    tempFile,
                                                    finalTarget,
                                                    statusEc ? statusEc : std::make_error_code(std::errc::invalid_argument));
        }
        RejectHardLinkedRegularFile(finalTarget, "Refusing to replace a hard-linked target file");
        if ((oldAttrs & FILE_ATTRIBUTE_READONLY) != 0) {
            const DWORD writableAttrs = oldAttrs & ~FILE_ATTRIBUTE_READONLY;
            if (SetFileAttributesW(finalTarget.c_str(), writableAttrs) == FALSE) {
                const DWORD error = GetLastError();
                throw std::filesystem::filesystem_error("Unable to prepare read-only target for replacement",
                                                        finalTarget,
                                                        std::error_code(static_cast<int>(error), std::system_category()));
            }
            clearedReadOnly = true;
        }
        if (SetFileAttributesW(tempFile.c_str(), oldAttrs & ~FILE_ATTRIBUTE_READONLY) == FALSE) {
            const DWORD error = GetLastError();
            if (clearedReadOnly) {
                SetFileAttributesW(finalTarget.c_str(), oldAttrs);
            }
            throw std::filesystem::filesystem_error("Unable to prepare temporary output attributes",
                                                    tempFile,
                                                    std::error_code(static_cast<int>(error), std::system_category()));
        }
    }
    if (MoveFileExW(tempFile.c_str(), finalTarget.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD error = GetLastError();
        if (clearedReadOnly) {
            SetFileAttributesW(finalTarget.c_str(), oldAttrs);
        }
        throw std::filesystem::filesystem_error("Unable to replace target file with temporary output",
                                                tempFile,
                                                finalTarget,
                                                std::error_code(static_cast<int>(error), std::system_category()));
    }
    if (hadAttrs) {
        SetFileAttributesW(finalTarget.c_str(), oldAttrs);
    }
    RemoveManagedTempParentNoThrow(tempFile);
#else
    std::error_code ec;
    const auto targetLinkStatus = std::filesystem::symlink_status(finalTarget, ec);
    bool existed = false;
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) {
            ec.clear();
        } else {
            throw std::filesystem::filesystem_error("Unable to inspect replacement target",
                                                    finalTarget,
                                                    ec);
        }
    } else {
        existed = std::filesystem::exists(targetLinkStatus);
    }

    auto oldPerms = std::filesystem::perms::unknown;
    if (existed) {
        if (std::filesystem::is_symlink(targetLinkStatus)) {
            throw std::filesystem::filesystem_error("Refusing to replace a symlink target",
                                                    tempFile,
                                                    finalTarget,
                                                    std::make_error_code(std::errc::invalid_argument));
        }
        if (!std::filesystem::is_regular_file(targetLinkStatus)) {
            throw std::filesystem::filesystem_error("Refusing to replace a non-file target",
                                                    tempFile,
                                                    finalTarget,
                                                    std::make_error_code(std::errc::invalid_argument));
        }
        RejectHardLinkedRegularFile(finalTarget, "Refusing to replace a hard-linked target file");
        oldPerms = targetLinkStatus.permissions();
        std::error_code tempPermEc;
        std::filesystem::permissions(tempFile,
                                     oldPerms,
                                     std::filesystem::perm_options::replace,
                                     tempPermEc);
        if (tempPermEc) {
            throw std::filesystem::filesystem_error("Unable to prepare temporary output permissions",
                                                    tempFile,
                                                    tempPermEc);
        }
    }

    std::filesystem::rename(tempFile, finalTarget, ec);
    if (ec) {
        throw std::filesystem::filesystem_error("Unable to replace target file with temporary output",
                                                tempFile,
                                                finalTarget,
                                                ec);
    }
    if (existed) {
        std::error_code permEc;
        std::filesystem::permissions(finalTarget,
                                     oldPerms,
                                     std::filesystem::perm_options::replace,
                                     permEc);
    }
    FlushDirectoryNoThrow(finalTarget.parent_path());
    RemoveManagedTempParentNoThrow(tempFile);
#endif
}


void RemoveFileNoThrow(const std::filesystem::path& file) {
    if (file.empty() || !IsRegisteredManagedTempPayload(file) || !isManagedTempPayloadPath(file)) {
        return;
    }
    std::error_code ec;
    const auto st = std::filesystem::symlink_status(file, ec);
    if (!ec && (std::filesystem::is_regular_file(st) || std::filesystem::is_symlink(st))) {
        std::filesystem::remove(file, ec);
    }
    RemoveManagedTempParentNoThrow(file);
}

std::string ReadRegularFileBytes(const std::filesystem::path& file) {
    if (file.empty()) {
        throw std::filesystem::filesystem_error("Unable to open regular input file",
                                                file,
                                                std::make_error_code(std::errc::no_such_file_or_directory));
    }
    const std::filesystem::path inputFile = ResolveOutputTarget(file);

#ifdef _WIN32
    HANDLE handle = CreateFileW(inputFile.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        throw std::filesystem::filesystem_error("Unable to open regular input file",
                                                inputFile,
                                                std::error_code(static_cast<int>(error), std::system_category()));
    }

    auto closeHandle = [&]() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    };

    try {
        if (GetFileType(handle) != FILE_TYPE_DISK || isWindowsReparsePointHandle(handle)) {
            throw std::filesystem::filesystem_error("Refusing to read a non-regular input file",
                                                    inputFile,
                                                    std::make_error_code(std::errc::invalid_argument));
        }

        BY_HANDLE_FILE_INFORMATION startInfo{};
        if (GetFileInformationByHandle(handle, &startInfo) == FALSE) {
            const DWORD error = GetLastError();
            throw std::filesystem::filesystem_error("Unable to inspect regular input file metadata",
                                                    inputFile,
                                                    std::error_code(static_cast<int>(error), std::system_category()));
        }
        ULARGE_INTEGER size{};
        size.LowPart = startInfo.nFileSizeLow;
        size.HighPart = startInfo.nFileSizeHigh;
        if (size.QuadPart > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
            throw std::filesystem::filesystem_error("Regular input file is too large to read safely",
                                                    inputFile,
                                                    std::make_error_code(std::errc::file_too_large));
        }

        std::string data;
        data.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t total = 0;
        while (total < data.size()) {
            const DWORD toRead = static_cast<DWORD>(std::min<std::size_t>(data.size() - total, 1u << 20));
            DWORD readNow = 0;
            if (ReadFile(handle, data.data() + total, toRead, &readNow, nullptr) == FALSE) {
                const DWORD error = GetLastError();
                throw std::filesystem::filesystem_error("Unable to read regular input file",
                                                        inputFile,
                                                        std::error_code(static_cast<int>(error), std::system_category()));
            }
            if (readNow == 0) {
                throw std::filesystem::filesystem_error("Regular input file ended before the expected byte count was read",
                                                        inputFile,
                                                        std::make_error_code(std::errc::text_file_busy));
            }
            total += readNow;
        }
        BY_HANDLE_FILE_INFORMATION endInfo{};
        if (GetFileInformationByHandle(handle, &endInfo) == FALSE || !sameWindowsFileSnapshot(startInfo, endInfo)) {
            const DWORD error = GetLastError();
            throw std::filesystem::filesystem_error("Regular input file changed while it was being read",
                                                    inputFile,
                                                    std::error_code(static_cast<int>(error == ERROR_SUCCESS ? ERROR_BUSY : error), std::system_category()));
        }
        closeHandle();
        return data;
    } catch (...) {
        closeHandle();
        throw;
    }
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(inputFile.c_str(), flags);
    if (fd < 0) {
        throw std::filesystem::filesystem_error("Unable to open regular input file",
                                                inputFile,
                                                std::error_code(errno, std::generic_category()));
    }

    auto closeFd = [&]() {
        if (fd >= 0) {
            (void)close(fd);
        }
    };

    try {
        struct stat st {};
        if (fstat(fd, &st) != 0) {
            const int saved = errno;
            throw std::filesystem::filesystem_error("Unable to inspect regular input file",
                                                    inputFile,
                                                    std::error_code(saved, std::generic_category()));
        }
        if (!S_ISREG(st.st_mode)) {
            throw std::filesystem::filesystem_error("Refusing to read a non-regular input file",
                                                    inputFile,
                                                    std::make_error_code(std::errc::invalid_argument));
        }

        std::string data;
        if (st.st_size > 0) {
            if (static_cast<unsigned long long>(st.st_size) >
                static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
                throw std::filesystem::filesystem_error("Regular input file is too large to read safely",
                                                        inputFile,
                                                        std::make_error_code(std::errc::file_too_large));
            }
            data.reserve(static_cast<std::size_t>(st.st_size));
        }

        std::array<char, 1 << 16> buffer{};
        unsigned long long remaining = st.st_size > 0 ? static_cast<unsigned long long>(st.st_size) : 0ull;
        while (remaining > 0) {
            const std::size_t request = static_cast<std::size_t>(std::min<unsigned long long>(remaining, buffer.size()));
            const ssize_t n = read(fd, buffer.data(), request);
            if (n > 0) {
                data.append(buffer.data(), static_cast<std::size_t>(n));
                remaining -= static_cast<unsigned long long>(n);
                continue;
            }
            if (n == 0) {
                throw std::filesystem::filesystem_error("Regular input file ended before the expected byte count was read",
                                                        inputFile,
                                                        std::make_error_code(std::errc::text_file_busy));
            }
            if (errno == EINTR) {
                continue;
            }
#ifdef EAGAIN
            if (errno == EAGAIN) {
                continue;
            }
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || EWOULDBLOCK != EAGAIN)
            if (errno == EWOULDBLOCK) {
                continue;
            }
#endif
            const int saved = errno;
            throw std::filesystem::filesystem_error("Unable to read regular input file",
                                                    inputFile,
                                                    std::error_code(saved, std::generic_category()));
        }
        struct stat endSt {};
        if (fstat(fd, &endSt) != 0) {
            const int saved = errno;
            throw std::filesystem::filesystem_error("Unable to re-inspect regular input file",
                                                    inputFile,
                                                    std::error_code(saved, std::generic_category()));
        }
        if (!sameRegularFileSnapshot(st, endSt)) {
            throw std::filesystem::filesystem_error("Regular input file changed while it was being read",
                                                    inputFile,
                                                    std::make_error_code(std::errc::text_file_busy));
        }
        closeFd();
        return data;
    } catch (...) {
        closeFd();
        throw;
    }
#endif
}


void CopyRegularFileToOutput(const std::filesystem::path& file, SafeOutputFile& out) {
    if (file.empty()) {
        throw std::filesystem::filesystem_error("Unable to open backup source file",
                                                file,
                                                std::make_error_code(std::errc::no_such_file_or_directory));
    }
    const std::filesystem::path inputFile = ResolveOutputTarget(file);
#ifdef _WIN32
    HANDLE handle = CreateFileW(inputFile.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        throw std::filesystem::filesystem_error("Unable to open backup source file",
                                                inputFile,
                                                std::error_code(static_cast<int>(error), std::system_category()));
    }
    auto closeHandle = [&]() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    };
    try {
        if (GetFileType(handle) != FILE_TYPE_DISK || isWindowsReparsePointHandle(handle)) {
            throw std::filesystem::filesystem_error("Refusing to back up a non-regular source file",
                                                    inputFile,
                                                    std::make_error_code(std::errc::invalid_argument));
        }
        BY_HANDLE_FILE_INFORMATION startInfo{};
        if (GetFileInformationByHandle(handle, &startInfo) == FALSE) {
            const DWORD error = GetLastError();
            throw std::filesystem::filesystem_error("Unable to inspect backup source file metadata",
                                                    inputFile,
                                                    std::error_code(static_cast<int>(error), std::system_category()));
        }
        ULARGE_INTEGER sourceSize{};
        sourceSize.LowPart = startInfo.nFileSizeLow;
        sourceSize.HighPart = startInfo.nFileSizeHigh;
        unsigned long long remaining = sourceSize.QuadPart;
        std::array<char, 1 << 16> buffer{};
        while (remaining > 0) {
            const DWORD toRead = static_cast<DWORD>(std::min<unsigned long long>(remaining, buffer.size()));
            DWORD readNow = 0;
            if (ReadFile(handle, buffer.data(), toRead, &readNow, nullptr) == FALSE) {
                const DWORD error = GetLastError();
                throw std::filesystem::filesystem_error("Unable to copy backup file data",
                                                        inputFile,
                                                        std::error_code(static_cast<int>(error), std::system_category()));
            }
            if (readNow == 0) {
                throw std::filesystem::filesystem_error("Backup source file ended before the expected byte count was copied",
                                                        inputFile,
                                                        std::make_error_code(std::errc::text_file_busy));
            }
            out.writeBytes(buffer.data(), static_cast<std::size_t>(readNow));
            remaining -= static_cast<unsigned long long>(readNow);
        }
        BY_HANDLE_FILE_INFORMATION endInfo{};
        if (GetFileInformationByHandle(handle, &endInfo) == FALSE || !sameWindowsFileSnapshot(startInfo, endInfo)) {
            const DWORD error = GetLastError();
            throw std::filesystem::filesystem_error("Backup source file changed while it was being copied",
                                                    inputFile,
                                                    std::error_code(static_cast<int>(error == ERROR_SUCCESS ? ERROR_BUSY : error), std::system_category()));
        }
        closeHandle();
    } catch (...) {
        closeHandle();
        throw;
    }
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(inputFile.c_str(), flags);
    if (fd < 0) {
        throw std::filesystem::filesystem_error("Unable to open backup source file",
                                                inputFile,
                                                std::error_code(errno, std::generic_category()));
    }
    auto closeFd = [&]() {
        if (fd >= 0) {
            (void)close(fd);
        }
    };
    try {
        struct stat st {};
        if (fstat(fd, &st) != 0) {
            const int saved = errno;
            throw std::filesystem::filesystem_error("Unable to inspect backup source file",
                                                    inputFile,
                                                    std::error_code(saved, std::generic_category()));
        }
        if (!S_ISREG(st.st_mode)) {
            throw std::filesystem::filesystem_error("Refusing to back up a non-regular source file",
                                                    inputFile,
                                                    std::make_error_code(std::errc::invalid_argument));
        }
        std::array<char, 1 << 16> buffer{};
        unsigned long long remaining = st.st_size > 0 ? static_cast<unsigned long long>(st.st_size) : 0ull;
        while (remaining > 0) {
            const std::size_t request = static_cast<std::size_t>(std::min<unsigned long long>(remaining, buffer.size()));
            const ssize_t n = read(fd, buffer.data(), request);
            if (n > 0) {
                out.writeBytes(buffer.data(), static_cast<std::size_t>(n));
                remaining -= static_cast<unsigned long long>(n);
                continue;
            }
            if (n == 0) {
                throw std::filesystem::filesystem_error("Backup source file ended before the expected byte count was copied",
                                                        inputFile,
                                                        std::make_error_code(std::errc::text_file_busy));
            }
            if (errno == EINTR) {
                continue;
            }
#ifdef EAGAIN
            if (errno == EAGAIN) {
                continue;
            }
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || EWOULDBLOCK != EAGAIN)
            if (errno == EWOULDBLOCK) {
                continue;
            }
#endif
            const int saved = errno;
            throw std::filesystem::filesystem_error("Unable to copy backup file data",
                                                    inputFile,
                                                    std::error_code(saved, std::generic_category()));
        }
        struct stat endSt {};
        if (fstat(fd, &endSt) != 0) {
            const int saved = errno;
            throw std::filesystem::filesystem_error("Unable to re-inspect backup source file",
                                                    inputFile,
                                                    std::error_code(saved, std::generic_category()));
        }
        if (!sameRegularFileSnapshot(st, endSt)) {
            throw std::filesystem::filesystem_error("Backup source file changed while it was being copied",
                                                    inputFile,
                                                    std::make_error_code(std::errc::text_file_busy));
        }
        closeFd();
    } catch (...) {
        closeFd();
        throw;
    }
#endif
}


std::string FormatNumber(float value) {
    return formatFloating(value);
}

std::string FormatNumber(double value) {
    return formatFloating(value);
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}


} // namespace neogff
