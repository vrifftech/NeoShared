#include "neoshared/erf/Utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <cstdint>
#include <cerrno>
#include <sstream>
#include <cstring>
#include <chrono>
#include <atomic>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace neoshared::erf {
namespace {


#if defined(_WIN32)
class ScopedWin32Handle {
public:
    explicit ScopedWin32Handle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~ScopedWin32Handle() {
        close();
    }

    ScopedWin32Handle(const ScopedWin32Handle&) = delete;
    ScopedWin32Handle& operator=(const ScopedWin32Handle&) = delete;
    ScopedWin32Handle(ScopedWin32Handle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }
    ScopedWin32Handle& operator=(ScopedWin32Handle&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    bool close() noexcept {
        if (handle_ == INVALID_HANDLE_VALUE) {
            return true;
        }
        HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return CloseHandle(handle) != 0;
    }

private:
    HANDLE handle_;
};

ScopedWin32Handle open_regular_file_for_read_win32(const std::filesystem::path& path,
                                                   std::uintmax_t* size_out = nullptr) {
    ScopedWin32Handle handle(CreateFileW(path.c_str(),
                                         GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr,
                                         OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                         nullptr));
    if (!handle.valid()) {
        return ScopedWin32Handle();
    }
    if (GetFileType(handle.get()) != FILE_TYPE_DISK) {
        return ScopedWin32Handle();
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size) || size.QuadPart < 0) {
        return ScopedWin32Handle();
    }
    if (size_out != nullptr) {
        *size_out = static_cast<std::uintmax_t>(size.QuadPart);
    }
    return handle;
}


FileIdentity capture_identity_from_win32_handle(HANDLE handle, std::uintmax_t size) {
    FileIdentity identity{};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        return identity;
    }
    identity.valid = true;
    identity.size = size;
    identity.volume_serial = info.dwVolumeSerialNumber;
    identity.file_index_high = info.nFileIndexHigh;
    identity.file_index_low = info.nFileIndexLow;
    return identity;
}

bool read_exact_from_win32_handle(HANDLE handle, char* buffer, DWORD size) {
    DWORD total = 0;
    while (total < size) {
        DWORD got = 0;
        if (!ReadFile(handle, buffer + total, size - total, &got, nullptr) || got == 0) {
            return false;
        }
        total += got;
    }
    return true;
}
#else
class ScopedPosixFd {
public:
    explicit ScopedPosixFd(int fd = -1) noexcept : fd_(fd) {}
    ~ScopedPosixFd() {
        close();
    }
    ScopedPosixFd(const ScopedPosixFd&) = delete;
    ScopedPosixFd& operator=(const ScopedPosixFd&) = delete;
    ScopedPosixFd(ScopedPosixFd&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }
    ScopedPosixFd& operator=(ScopedPosixFd&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    bool close() noexcept {
        if (fd_ < 0) {
            return true;
        }
        const int fd = fd_;
        fd_ = -1;
        return ::close(fd) == 0;
    }

private:
    int fd_ = -1;
};
#endif


std::uint64_t current_process_id_for_temp_suffix() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::uint64_t temp_process_nonce_for_suffix() {
    static const char nonce_anchor = 0;
    static const std::uint64_t nonce = [] {
        const auto steady = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto high = static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        const auto pid = current_process_id_for_temp_suffix();
        const auto anchor = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&nonce_anchor));
        return steady ^ (high << 7u) ^ (pid << 17u) ^ anchor;
    }();
    return nonce;
}

std::string copy_temp_suffix() {
    static std::atomic<std::uint64_t> counter{0};
    const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream out;
    out << std::hex << ticks << "-p" << current_process_id_for_temp_suffix()
        << "-n" << temp_process_nonce_for_suffix()
        << "-" << counter.fetch_add(1, std::memory_order_relaxed);
    return out.str();
}

std::filesystem::path make_unique_copy_temp_path(const std::filesystem::path& destination) {
    std::filesystem::path dir = destination.parent_path();
    if (dir.empty()) {
        std::error_code ec;
        dir = std::filesystem::current_path(ec);
        if (ec) {
            dir = std::filesystem::path(".");
        }
    }
    const std::string leaf = destination.filename().string().empty() ? std::string("copy") : destination.filename().string();
    for (int attempt = 0; attempt < 128; ++attempt) {
        std::filesystem::path candidate = dir / (leaf + ".neoerf-copying-" + copy_temp_suffix() + ".tmp");
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    throw std::runtime_error("Unable to choose a unique temporary copy path near: " + destination.string());
}

void remove_file_noexcept(const std::filesystem::path& filename) noexcept {
    try {
#if defined(_WIN32)
        DeleteFileW(filename.c_str());
#else
        std::error_code ignored;
        std::filesystem::remove(filename, ignored);
#endif
    } catch (...) {
    }
}

class ScopedTempCopyFile {
public:
    explicit ScopedTempCopyFile(std::filesystem::path path) : path_(std::move(path)) {}
    ~ScopedTempCopyFile() {
        if (active_) {
            remove_file_if_same_identity_noexcept(path_, identity_);
        }
    }
    ScopedTempCopyFile(const ScopedTempCopyFile&) = delete;
    ScopedTempCopyFile& operator=(const ScopedTempCopyFile&) = delete;
    void activate(FileIdentity identity) noexcept {
        identity_ = identity;
        active_ = identity_.valid;
    }
    void release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    FileIdentity identity_{};
    bool active_ = false;
};

#if !defined(_WIN32)
#if !defined(__EMSCRIPTEN__)
bool fsync_retry(int fd) {
    for (;;) {
        if (::fsync(fd) == 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}
#endif

int close_on_exec_flag() {
#ifdef O_CLOEXEC
    return O_CLOEXEC;
#else
    return 0;
#endif
}

#if !defined(__EMSCRIPTEN__)
void fsync_parent_directory_after_copy_replace(const std::filesystem::path& target) {
    std::filesystem::path dir = target.parent_path();
    if (dir.empty()) {
        std::error_code ec;
        dir = std::filesystem::current_path(ec);
        if (ec) {
            dir = std::filesystem::path(".");
        }
    }
    ScopedPosixFd fd(::open(path_to_string(dir).c_str(), O_RDONLY | O_DIRECTORY | close_on_exec_flag()));
    if (!fd.valid()) {
        throw std::runtime_error("Unable to open output directory for sync after copy replace: " + path_to_string(dir));
    }
    if (!fsync_retry(fd.get())) {
        throw std::runtime_error("Unable to sync output directory after copy replace: " + path_to_string(dir));
    }
    if (!fd.close()) {
        throw std::runtime_error("Unable to close output directory after copy replace: " + path_to_string(dir));
    }
}
#endif

int safe_open_read_flags() {
    return O_RDONLY | O_NONBLOCK | close_on_exec_flag();
}

ScopedPosixFd open_regular_file_for_read_nonblocking(const std::filesystem::path& path,
                                                     std::uintmax_t* size_out = nullptr) {
    ScopedPosixFd fd(::open(path_to_string(path).c_str(), safe_open_read_flags()));
    if (!fd.valid()) {
        return ScopedPosixFd();
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        return ScopedPosixFd();
    }
    if (size_out != nullptr) {
        *size_out = static_cast<std::uintmax_t>(st.st_size);
    }
    return fd;
}


FileIdentity capture_identity_from_posix_fd(int fd, std::uintmax_t size) {
    FileIdentity identity{};
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        return identity;
    }
    identity.valid = true;
    identity.size = size;
    identity.device = static_cast<std::uint64_t>(st.st_dev);
    identity.inode = static_cast<std::uint64_t>(st.st_ino);
    return identity;
}

bool read_exact_from_fd(int fd, char* buffer, std::size_t size) {
    std::size_t total = 0;
    while (total < size) {
        const ssize_t got = ::read(fd, buffer + total, size - total);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(got);
    }
    return true;
}
#endif




bool same_identity_values(const FileIdentity& a, const FileIdentity& b) {
    if (!a.valid || !b.valid) {
        return false;
    }
#if defined(_WIN32)
    return a.volume_serial == b.volume_serial &&
           a.file_index_high == b.file_index_high &&
           a.file_index_low == b.file_index_low;
#else
    return a.device == b.device && a.inode == b.inode;
#endif
}

void ensure_opened_source_matches_expected(const FileIdentity* expected,
                                           const FileIdentity& opened,
                                           const std::filesystem::path& source) {
    if (expected != nullptr && !same_identity_values(opened, *expected)) {
        throw std::runtime_error("Staged input file was replaced before copy could bind it: " + path_to_string(source));
    }
}

bool files_match_after_copy(const std::filesystem::path& source,
                            const std::filesystem::path& copied,
                            std::uintmax_t max_bytes) {
    // Data-safe copy verification. The copy path snapshots the source size
    // before copying to avoid unbounded reads, but a user-controlled staged
    // file can grow or change while the copy is in progress. Verify that the
    // completed temporary copy still matches the source before replacing the
    // requested destination or treating the staged copy as archive input.
#if defined(_WIN32)
    std::uintmax_t source_size = 0;
    std::uintmax_t copied_size = 0;
    auto source_handle = open_regular_file_for_read_win32(source, &source_size);
    auto copied_handle = open_regular_file_for_read_win32(copied, &copied_size);
    if (!source_handle.valid() || !copied_handle.valid()) {
        return false;
    }
    if (source_size != copied_size || source_size > max_bytes) {
        return false;
    }

    std::array<char, kDataSafeIoBufferSize> abuf{};
    std::array<char, kDataSafeIoBufferSize> bbuf{};
    std::uintmax_t remaining = source_size;
    while (remaining > 0) {
        const DWORD want = static_cast<DWORD>(std::min<std::uintmax_t>(abuf.size(), remaining));
        if (!read_exact_from_win32_handle(source_handle.get(), abuf.data(), want) ||
            !read_exact_from_win32_handle(copied_handle.get(), bbuf.data(), want)) {
            return false;
        }
        if (std::memcmp(abuf.data(), bbuf.data(), want) != 0) {
            return false;
        }
        remaining -= static_cast<std::uintmax_t>(want);
    }
    return true;
#else
    std::uintmax_t source_size = 0;
    std::uintmax_t copied_size = 0;
    auto source_fd = open_regular_file_for_read_nonblocking(source, &source_size);
    auto copied_fd = open_regular_file_for_read_nonblocking(copied, &copied_size);
    if (!source_fd.valid() || !copied_fd.valid()) {
        return false;
    }
    if (source_size != copied_size || source_size > max_bytes) {
        return false;
    }

    std::array<char, kDataSafeIoBufferSize> abuf{};
    std::array<char, kDataSafeIoBufferSize> bbuf{};
    std::uintmax_t remaining = source_size;
    while (remaining > 0) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uintmax_t>(abuf.size(), remaining));
        if (!read_exact_from_fd(source_fd.get(), abuf.data(), want) ||
            !read_exact_from_fd(copied_fd.get(), bbuf.data(), want)) {
            return false;
        }
        if (std::memcmp(abuf.data(), bbuf.data(), want) != 0) {
            return false;
        }
        remaining -= static_cast<std::uintmax_t>(want);
    }
    return true;
#endif
}

void replace_file_after_successful_copy(const std::filesystem::path& source,
                                        const FileIdentity& source_identity,
                                        const std::filesystem::path& destination,
                                        const ReplacementTargetState& destination_state,
                                        bool replace_existing) {
    if (!same_regular_file_identity(source, source_identity)) {
        throw std::runtime_error("Temporary copy output was replaced before final destination update: " + path_to_string(source));
    }
    ensure_replacement_target_unchanged(destination, destination_state, "Copy destination safety check failed");
#if defined(_WIN32)
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (replace_existing) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (!MoveFileExW(source.c_str(), destination.c_str(), flags)) {
        throw std::runtime_error(std::string(replace_existing ? "Unable to replace" : "Unable to create") +
                                 " copied destination safely: " + path_to_string(destination));
    }
    if (!same_regular_file_identity(destination, source_identity)) {
        throw std::runtime_error("Copied destination does not match the verified temporary copy: " + path_to_string(destination));
    }
#elif defined(__EMSCRIPTEN__)
    (void)replace_existing;
    // Browser save staging lives entirely in MEMFS. Rename the verified temp
    // node instead of relying on hard-link and directory-fsync semantics that
    // exist only to protect persistent desktop filesystems.
    std::error_code ec;
    std::filesystem::rename(source, destination, ec);
    if (ec) {
        throw std::runtime_error("Unable to finalize browser save staging file: " +
                                 path_to_string(destination) + ": " + ec.message());
    }
    if (!same_regular_file_identity(destination, source_identity)) {
        throw std::runtime_error("Browser save staging destination does not match the verified temporary copy: " +
                                 path_to_string(destination));
    }
#else
    if (replace_existing) {
        std::error_code ec;
        std::filesystem::rename(source, destination, ec);
        if (ec) {
            throw std::runtime_error("Unable to replace copied destination safely: " + destination.string() + ": " + ec.message());
        }
        if (!same_regular_file_identity(destination, source_identity)) {
            throw std::runtime_error("Copied destination does not match the verified temporary copy: " + path_to_string(destination));
        }
    } else {
        if (::link(path_to_string(source).c_str(), path_to_string(destination).c_str()) != 0) {
            throw std::runtime_error("Unable to create copied destination safely without overwriting: " +
                                     path_to_string(destination) + ": " + std::strerror(errno));
        }
        if (!same_regular_file_identity(destination, source_identity)) {
            const FileIdentity created_identity = capture_regular_file_identity(destination);
            remove_file_if_same_identity_noexcept(destination, created_identity);
            throw std::runtime_error("Copied destination does not match the verified temporary copy: " + path_to_string(destination));
        }
        remove_file_if_same_identity_noexcept(source, source_identity);
    }
    fsync_parent_directory_after_copy_replace(destination);
#endif
}

bool is_allowed_resref_char(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_' || ch == '-' || ch == '+';
}

std::string extension_from_leaf(const std::string& value) {
    // Win32 extension extraction scans for '.', PathDelim '\\',
    // and DriveDelim ':'. It does not treat POSIX '/' as a path delimiter.
    // A forward slash is treated as a path separator before resource-name parsing.
    const auto slash = value.find_last_of("\\:");
    const std::size_t leaf_start = (slash == std::string::npos) ? 0 : slash + 1;
    const auto pos = value.find_last_of('.');
    if (pos == std::string::npos || pos < leaf_start) {
        return std::string();
    }
    return value.substr(pos);
}

} // namespace

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string ascii_upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::string path_to_string(const std::filesystem::path& path) {
    return path.string();
}

bool is_number(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::filesystem::path include_trailing_separator(const std::filesystem::path& path) {
    if (path.empty()) {
        return path;
    }
    const auto native = path.native();
    if (!native.empty() && native.back() == std::filesystem::path::preferred_separator) {
        return path;
    }
    return path / std::filesystem::path();
}

std::string filename_string(const std::filesystem::path& path) {
    const std::string value = path_to_string(path);
    const auto slash = value.find_last_of("\\/:");
    return slash == std::string::npos ? value : value.substr(slash + 1);
}

std::string extension_string(const std::filesystem::path& path) {
    return extension_from_leaf(filename_string(path));
}

std::string resource_stem_from_text(const std::string& filename) {
    const std::string leaf = filename_string(std::filesystem::path(filename));
    const std::string ext = extension_from_leaf(leaf);
    if (ext.empty()) {
        return leaf;
    }
    return leaf.substr(0, leaf.size() - ext.size());
}


bool file_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

bool directory_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

std::filesystem::path normalized_absolute_path_for_compare(const std::filesystem::path& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical.lexically_normal();
    }
    ec.clear();
    auto absolute = std::filesystem::absolute(path, ec);
    if (!ec) {
        return absolute.lexically_normal();
    }
    return path.lexically_normal();
}

bool paths_refer_to_same_existing_file(const std::filesystem::path& a, const std::filesystem::path& b) {
    // Data-safe guard: prevent operations that would replace the loaded archive
    // or a staged input file with their own output.  Use filesystem identity
    // when both paths exist so relative/absolute aliases and hard links are
    // detected.  Failure to prove equivalence is treated as non-equivalent; the
    // later open/replace path still fails closed if the filesystem rejects it.
    std::error_code ec;
    if (!std::filesystem::exists(a, ec) || ec) {
        return false;
    }
    ec.clear();
    if (!std::filesystem::exists(b, ec) || ec) {
        return false;
    }
    ec.clear();
    return std::filesystem::equivalent(a, b, ec) && !ec;
}

bool paths_refer_to_same_existing_file_or_location(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (paths_refer_to_same_existing_file(a, b)) {
        return true;
    }

    const auto na = normalized_absolute_path_for_compare(a);
    const auto nb = normalized_absolute_path_for_compare(b);
#if defined(_WIN32)
    return ascii_lower(path_to_string(na)) == ascii_lower(path_to_string(nb));
#else
    return na == nb;
#endif
}

FileIdentity copy_file_limited_impl(const std::filesystem::path& source, const std::filesystem::path& destination, std::uintmax_t max_bytes, const FileIdentity* expected_source_identity, bool replace_existing) {
    if (!file_exists(source)) {
        throw std::runtime_error("Source file for copying is not a regular file: " + path_to_string(source));
    }
    if (paths_refer_to_same_existing_file_or_location(source, destination)) {
        throw std::runtime_error("Refusing to copy a file over itself: " + path_to_string(source));
    }
    if (!replace_existing && (file_exists(destination) || directory_exists(destination))) {
        throw std::runtime_error("Refusing to create copied destination because it already exists: " + path_to_string(destination));
    }
    const ReplacementTargetState destination_state = capture_replacement_target_state(destination);

    // Data-safe policy: never open the requested destination with truncating
    // create until the source has been fully copied, flushed, and closed.  This
    // helper is used for save staging and is also exposed through BackupFile /
    // CopyNewFile compatibility wrappers; a failed copy must leave any existing
    // destination untouched.
    const auto temp_destination = make_unique_copy_temp_path(destination);
    ScopedTempCopyFile temp_guard(temp_destination);
    FileIdentity temp_identity{};

#if defined(_WIN32)
    // CopyNewFile/BackupFile read from fmOpenRead or fmShareDenyNone.
    // Keep the file-sharing guard, but write to a CREATE_NEW temp file
    // and replace the destination only after the full copy succeeds.
    std::uintmax_t source_size = 0;
    auto in = open_regular_file_for_read_win32(source, &source_size);
    if (!in.valid()) {
        throw std::runtime_error("Unable to open regular source file for copying: " + path_to_string(source));
    }
    if (source_size > max_bytes) {
        throw std::runtime_error("Refusing to copy source file because it exceeds the permitted size: " + path_to_string(source));
    }
    ensure_opened_source_matches_expected(expected_source_identity,
                                          capture_identity_from_win32_handle(in.get(), source_size),
                                          source);

    ScopedWin32Handle out(CreateFileW(temp_destination.c_str(),
                                     GENERIC_READ | GENERIC_WRITE,
                                     0,
                                     nullptr,
                                     CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL,
                                     nullptr));
    if (!out.valid()) {
        throw std::runtime_error("Unable to create temporary copy destination: " + path_to_string(temp_destination));
    }
    temp_identity = capture_identity_from_win32_handle(out.get(), 0);
    if (!temp_identity.valid) {
        throw std::runtime_error("Unable to capture temporary copy ownership: " + path_to_string(temp_destination));
    }
    temp_guard.activate(temp_identity);

    std::array<char, kDataSafeIoBufferSize> buffer{};
    std::uint64_t remaining = static_cast<std::uint64_t>(source_size);
    while (remaining > 0) {
        const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), remaining));
        DWORD bytes_read = 0;
        if (!ReadFile(in.get(), buffer.data(), want, &bytes_read, nullptr) || bytes_read != want) {
            throw std::runtime_error("Unable to copy file data from " + path_to_string(source) + " to " + path_to_string(destination));
        }
        DWORD bytes_written = 0;
        if (!WriteFile(out.get(), buffer.data(), want, &bytes_written, nullptr) || bytes_written != want) {
            throw std::runtime_error("Unable to copy file data from " + path_to_string(source) + " to " + path_to_string(destination));
        }
        remaining -= want;
    }
    if (!FlushFileBuffers(out.get())) {
        throw std::runtime_error("Unable to flush copied file data to " + path_to_string(temp_destination));
    }
    if (!out.close()) {
        throw std::runtime_error("Unable to close copied file " + path_to_string(temp_destination));
    }
#else
    std::uintmax_t source_size = 0;
    auto in = open_regular_file_for_read_nonblocking(source, &source_size);
    if (!in.valid()) {
        throw std::runtime_error("Unable to open regular source file for copying: " + path_to_string(source));
    }
    if (source_size > max_bytes) {
        throw std::runtime_error("Refusing to copy source file because it exceeds the permitted size: " + path_to_string(source));
    }
    ensure_opened_source_matches_expected(expected_source_identity,
                                          capture_identity_from_posix_fd(in.get(), source_size),
                                          source);

    ScopedPosixFd out(::open(path_to_string(temp_destination).c_str(), O_WRONLY | O_CREAT | O_EXCL | close_on_exec_flag(), 0600));
    if (!out.valid()) {
        throw std::runtime_error("Unable to create temporary copy destination: " + path_to_string(temp_destination));
    }
    temp_identity = capture_identity_from_posix_fd(out.get(), 0);
    if (!temp_identity.valid) {
        throw std::runtime_error("Unable to capture temporary copy ownership: " + path_to_string(temp_destination));
    }
    temp_guard.activate(temp_identity);

    std::array<char, kDataSafeIoBufferSize> buffer{};
    std::uintmax_t remaining = source_size;
    while (remaining > 0) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uintmax_t>(buffer.size(), remaining));
        if (!read_exact_from_fd(in.get(), buffer.data(), want)) {
            throw std::runtime_error("Unable to copy file data from " + path_to_string(source) + " to " + path_to_string(destination));
        }
        std::size_t written_total = 0;
        while (written_total < want) {
            const ssize_t written = ::write(out.get(), buffer.data() + written_total, want - written_total);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                throw std::runtime_error("Unable to copy file data from " + path_to_string(source) + " to " + path_to_string(destination));
            }
            written_total += static_cast<std::size_t>(written);
        }
        remaining -= static_cast<std::uintmax_t>(want);
    }
#if !defined(__EMSCRIPTEN__)
    if (!fsync_retry(out.get())) {
        throw std::runtime_error("Unable to flush copied file data to " + path_to_string(temp_destination));
    }
#endif
    if (!out.close()) {
        throw std::runtime_error("Unable to close copied file " + path_to_string(temp_destination));
    }
#endif

    if (!files_match_after_copy(source, temp_destination, max_bytes)) {
        throw std::runtime_error("Source file changed while copying; refusing to replace destination: " + path_to_string(source));
    }
    if (expected_source_identity != nullptr && !same_regular_file_identity(source, *expected_source_identity)) {
        throw std::runtime_error("Staged input file was replaced while copying; refusing to use staged payload: " + path_to_string(source));
    }
    if (paths_refer_to_same_existing_file_or_location(source, destination)) {
        throw std::runtime_error("Refusing to copy a file over itself after copy verification: " + path_to_string(source));
    }
    if (!same_regular_file_identity(temp_destination, temp_identity)) {
        throw std::runtime_error("Temporary copy output was replaced before final destination update: " + path_to_string(temp_destination));
    }

    ensure_replacement_target_unchanged(destination, destination_state, "Copy destination safety check failed");
    replace_file_after_successful_copy(temp_destination, temp_identity, destination, destination_state, replace_existing);
    temp_guard.release();
    FileIdentity destination_identity = capture_regular_file_identity(destination);
    if (!destination_identity.valid) {
        throw std::runtime_error("Unable to bind copied destination ownership after replacement: " + path_to_string(destination));
    }
    return destination_identity;
}

FileIdentity copy_file_overwrite_limited(const std::filesystem::path& source, const std::filesystem::path& destination, std::uintmax_t max_bytes, const FileIdentity* expected_source_identity) {
    return copy_file_limited_impl(source, destination, max_bytes, expected_source_identity, true);
}

FileIdentity copy_file_create_new_limited(const std::filesystem::path& source, const std::filesystem::path& destination, std::uintmax_t max_bytes, const FileIdentity* expected_source_identity) {
    return copy_file_limited_impl(source, destination, max_bytes, expected_source_identity, false);
}

void copy_file_overwrite(const std::filesystem::path& source, const std::filesystem::path& destination) {
    copy_file_overwrite_limited(source, destination, std::numeric_limits<std::uintmax_t>::max());
}

std::uintmax_t regular_file_size_after_open(const std::filesystem::path& path) {
#if defined(_WIN32)
    std::uintmax_t size = 0;
    auto handle = open_regular_file_for_read_win32(path, &size);
    if (!handle.valid()) {
        throw std::runtime_error("Unable to open regular file for sizing: " + path_to_string(path));
    }
    return size;
#else
    std::uintmax_t size = 0;
    auto fd = open_regular_file_for_read_nonblocking(path, &size);
    if (!fd.valid()) {
        throw std::runtime_error("Unable to open regular file for sizing: " + path_to_string(path));
    }
    return size;
#endif
}

FileIdentity capture_regular_file_identity(const std::filesystem::path& path) {
    FileIdentity identity{};
#if defined(_WIN32)
    std::uintmax_t size = 0;
    auto handle = open_regular_file_for_read_win32(path, &size);
    if (!handle.valid()) {
        throw std::runtime_error("Unable to open regular file for identity capture: " + path_to_string(path));
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.get(), &info)) {
        throw std::runtime_error("Unable to read regular file identity: " + path_to_string(path));
    }
    identity.valid = true;
    identity.size = size;
    identity.modified_seconds = static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32u) |
        info.ftLastWriteTime.dwLowDateTime);
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic))) {
        throw std::runtime_error("Unable to read file change revision: " + path_to_string(path));
    }
    identity.changed_seconds = basic.ChangeTime.QuadPart;
    identity.volume_serial = static_cast<std::uint32_t>(info.dwVolumeSerialNumber);
    identity.file_index_high = static_cast<std::uint32_t>(info.nFileIndexHigh);
    identity.file_index_low = static_cast<std::uint32_t>(info.nFileIndexLow);
#else
    std::uintmax_t size = 0;
    auto fd = open_regular_file_for_read_nonblocking(path, &size);
    if (!fd.valid()) {
        throw std::runtime_error("Unable to open regular file for identity capture: " + path_to_string(path));
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        throw std::runtime_error("Unable to read regular file identity: " + path_to_string(path));
    }
    identity.valid = true;
    identity.size = static_cast<std::uintmax_t>(st.st_size);
    identity.device = static_cast<std::uint64_t>(st.st_dev);
    identity.inode = static_cast<std::uint64_t>(st.st_ino);
#if defined(__APPLE__)
    identity.modified_seconds = st.st_mtimespec.tv_sec;
    identity.modified_fraction = st.st_mtimespec.tv_nsec;
    identity.changed_seconds = st.st_ctimespec.tv_sec;
    identity.changed_fraction = st.st_ctimespec.tv_nsec;
#else
    identity.modified_seconds = st.st_mtim.tv_sec;
    identity.modified_fraction = st.st_mtim.tv_nsec;
    identity.changed_seconds = st.st_ctim.tv_sec;
    identity.changed_fraction = st.st_ctim.tv_nsec;
#endif
#endif
    return identity;
}

bool same_regular_file_identity(const std::filesystem::path& path, const FileIdentity& expected) {
    if (!expected.valid) {
        return false;
    }
    try {
        const FileIdentity current = capture_regular_file_identity(path);
        if (!current.valid) {
            return false;
        }
#if defined(_WIN32)
        return current.volume_serial == expected.volume_serial &&
               current.file_index_high == expected.file_index_high &&
               current.file_index_low == expected.file_index_low;
#else
        return current.device == expected.device && current.inode == expected.inode;
#endif
    } catch (...) {
        return false;
    }
}

bool same_regular_file_revision(const std::filesystem::path& path, const FileIdentity& expected) {
    if (!expected.valid) return false;
    try {
        const auto current = capture_regular_file_identity(path);
        const bool identityMatches =
#if defined(_WIN32)
            current.volume_serial == expected.volume_serial &&
            current.file_index_high == expected.file_index_high &&
            current.file_index_low == expected.file_index_low;
#else
            current.device == expected.device && current.inode == expected.inode;
#endif
        return identityMatches && current.size == expected.size &&
            current.modified_seconds == expected.modified_seconds &&
            current.modified_fraction == expected.modified_fraction &&
            current.changed_seconds == expected.changed_seconds &&
            current.changed_fraction == expected.changed_fraction;
    } catch (...) { return false; }
}

void ensure_same_regular_file_identity(const std::filesystem::path& path, const FileIdentity& expected, const std::string& context) {
    if (!same_regular_file_identity(path, expected)) {
        throw std::runtime_error(context + ": staged input file was replaced after it was added: " + path_to_string(path));
    }
}

PathIdentity capture_path_identity(const std::filesystem::path& path) {
    PathIdentity identity{};
#if defined(_WIN32)
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return identity;
    }
    ScopedWin32Handle handle(CreateFileW(path.c_str(),
                                         0,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr,
                                         OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                         nullptr));
    if (!handle.valid()) {
        return identity;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.get(), &info)) {
        return identity;
    }
    identity.valid = true;
    identity.is_directory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    identity.is_regular = !identity.is_directory && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    identity.volume_serial = static_cast<std::uint32_t>(info.dwVolumeSerialNumber);
    identity.file_index_high = static_cast<std::uint32_t>(info.nFileIndexHigh);
    identity.file_index_low = static_cast<std::uint32_t>(info.nFileIndexLow);
#else
    struct stat st {};
    if (::lstat(path_to_string(path).c_str(), &st) != 0) {
        return identity;
    }
    identity.valid = true;
    identity.is_directory = S_ISDIR(st.st_mode);
    identity.is_regular = S_ISREG(st.st_mode);
    identity.device = static_cast<std::uint64_t>(st.st_dev);
    identity.inode = static_cast<std::uint64_t>(st.st_ino);
#endif
    return identity;
}

bool same_path_identity(const std::filesystem::path& path, const PathIdentity& expected) {
    if (!expected.valid) {
        return false;
    }
    const PathIdentity current = capture_path_identity(path);
    if (!current.valid || current.is_directory != expected.is_directory || current.is_regular != expected.is_regular) {
        return false;
    }
#if defined(_WIN32)
    return current.volume_serial == expected.volume_serial &&
           current.file_index_high == expected.file_index_high &&
           current.file_index_low == expected.file_index_low;
#else
    return current.device == expected.device && current.inode == expected.inode;
#endif
}

ReplacementTargetState capture_replacement_target_state(const std::filesystem::path& path) {
    ReplacementTargetState state{};
    state.identity = capture_path_identity(path);
    if (state.identity.valid && !state.identity.is_regular) {
        throw std::runtime_error("Refusing to replace a non-regular filesystem object: " + path_to_string(path));
    }
    return state;
}

void ensure_replacement_target_unchanged(const std::filesystem::path& path,
                                         const ReplacementTargetState& expected,
                                         const std::string& context) {
    const PathIdentity current = capture_path_identity(path);
    if (!expected.identity.valid) {
        if (current.valid) {
            throw std::runtime_error(context + ": destination path appeared before replacement: " + path_to_string(path));
        }
        return;
    }
    if (!current.valid || !same_path_identity(path, expected.identity)) {
        throw std::runtime_error(context + ": destination path was replaced before final update: " + path_to_string(path));
    }
}

void remove_file_if_same_identity_noexcept(const std::filesystem::path& filename, const FileIdentity& expected) noexcept {
    try {
        if (!expected.valid) {
            return;
        }
        const PathIdentity current = capture_path_identity(filename);
        if (!current.valid || !current.is_regular) {
            return;
        }
#if defined(_WIN32)
        if (current.volume_serial != expected.volume_serial ||
            current.file_index_high != expected.file_index_high ||
            current.file_index_low != expected.file_index_low) {
            return;
        }
#else
        if (current.device != expected.device || current.inode != expected.inode) {
            return;
        }
#endif
        remove_file_noexcept(filename);
    } catch (...) {
    }
}

void remove_tree_if_same_identity_noexcept(const std::filesystem::path& folder, const PathIdentity& expected) noexcept {
    try {
        if (!expected.valid || !expected.is_directory || !same_path_identity(folder, expected)) {
            return;
        }
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
    } catch (...) {
    }
}

std::uint64_t write_regular_file_to_stream_limited(std::ostream& out,
                                                   const std::filesystem::path& source,
                                                   std::uintmax_t max_bytes,
                                                   const FileIdentity* expected_source_identity) {
    std::array<char, kDataSafeIoBufferSize> buffer{};
#if defined(_WIN32)
    std::uintmax_t source_size = 0;
    auto in = open_regular_file_for_read_win32(source, &source_size);
    if (!in.valid()) {
        throw std::runtime_error("Unable to open regular resource input file: " + path_to_string(source));
    }
    if (source_size > max_bytes) {
        throw std::runtime_error("Resource input payload exceeds the archive format limit: " + path_to_string(source));
    }
    ensure_opened_source_matches_expected(expected_source_identity,
                                          capture_identity_from_win32_handle(in.get(), source_size),
                                          source);
    std::uint64_t remaining = static_cast<std::uint64_t>(source_size);
    std::uint64_t copied = 0;
    while (remaining > 0) {
        const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), remaining));
        if (!read_exact_from_win32_handle(in.get(), buffer.data(), want)) {
            throw std::runtime_error("Unable to read resource input file: " + path_to_string(source));
        }
        out.write(buffer.data(), static_cast<std::streamsize>(want));
        if (!out) {
            throw std::runtime_error("Unable to write resource input file: " + path_to_string(source));
        }
        copied += static_cast<std::uint64_t>(want);
        remaining -= static_cast<std::uint64_t>(want);
    }
    return copied;
#else
    std::uintmax_t source_size = 0;
    auto in = open_regular_file_for_read_nonblocking(source, &source_size);
    if (!in.valid()) {
        throw std::runtime_error("Unable to open regular resource input file: " + path_to_string(source));
    }
    if (source_size > max_bytes) {
        throw std::runtime_error("Resource input payload exceeds the archive format limit: " + path_to_string(source));
    }
    ensure_opened_source_matches_expected(expected_source_identity,
                                          capture_identity_from_posix_fd(in.get(), source_size),
                                          source);
    std::uintmax_t remaining = source_size;
    std::uint64_t copied = 0;
    while (remaining > 0) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uintmax_t>(buffer.size(), remaining));
        if (!read_exact_from_fd(in.get(), buffer.data(), want)) {
            throw std::runtime_error("Unable to read resource input file: " + path_to_string(source));
        }
        out.write(buffer.data(), static_cast<std::streamsize>(want));
        if (!out) {
            throw std::runtime_error("Unable to write resource input file: " + path_to_string(source));
        }
        copied += static_cast<std::uint64_t>(want);
        remaining -= static_cast<std::uintmax_t>(want);
    }
    return copied;
#endif
}

std::vector<std::filesystem::path> files_in_folder(const std::filesystem::path& folder, bool names_only, bool sort_for_determinism, bool close_search_handle) {
    std::vector<std::filesystem::path> result;

    (void)close_search_handle;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (ec) {
            break;
        }
        if (file_exists(entry.path())) {
            result.push_back(names_only ? entry.path().filename() : entry.path());
        }
    }

    if (sort_for_determinism) {
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return ascii_lower(a.filename().string()) < ascii_lower(b.filename().string());
        });
    }

    return result;
}

std::string string_to_resref(const std::string& text, bool extended) {
    const std::size_t limit = std::min<std::size_t>(text.size(), extended ? 32 : 16);
    std::string out;
    out.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (is_allowed_resref_char(ch)) {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

} // namespace neoshared::erf
