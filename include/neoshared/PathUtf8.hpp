#pragma once

#include <algorithm>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace neoshared {

// Application-facing path text is UTF-8. Keep std::filesystem::path native
// internally and convert only at text/UI/serialization boundaries.
inline std::string pathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::wstring& wide = path.native();
    if (wide.empty()) return {};
    if (wide.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("Windows path is too long to encode as UTF-8");
    }
    const int inputLength = static_cast<int>(wide.size());
    const int outputLength = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), inputLength, nullptr, 0, nullptr, nullptr);
    if (outputLength <= 0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to encode Windows path as UTF-8");
    }
    std::string output(static_cast<std::size_t>(outputLength), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), inputLength, output.data(), outputLength, nullptr, nullptr);
    if (converted != outputLength) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to encode Windows path as UTF-8");
    }
    return output;
#else
    // POSIX paths are byte sequences. NeoTools treats those bytes as UTF-8 at
    // application boundaries and otherwise leaves them untouched.
    return path.native();
#endif
}

inline std::string genericPathToUtf8(const std::filesystem::path& path) {
    std::string value = pathToUtf8(path);
#if defined(_WIN32)
    std::replace(value.begin(), value.end(), '\\', '/');
#endif
    return value;
}

inline std::filesystem::path pathFromUtf8(std::string_view utf8) {
    if (utf8.empty()) return {};
#if defined(_WIN32)
    if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("UTF-8 path is too long for the Windows conversion API");
    }
    const int inputLength = static_cast<int>(utf8.size());
    const int outputLength = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), inputLength, nullptr, 0);
    if (outputLength <= 0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Invalid UTF-8 path at Windows boundary");
    }
    std::wstring wide(static_cast<std::size_t>(outputLength), L'\0');
    const int converted = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), inputLength, wide.data(), outputLength);
    if (converted != outputLength) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to decode UTF-8 path at Windows boundary");
    }
    return std::filesystem::path(std::move(wide));
#else
    return std::filesystem::path(std::string(utf8));
#endif
}

} // namespace neoshared
