#include "NeoBrowserFiles.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <wx/app.h>
#endif

#if defined(__EMSCRIPTEN__)

EM_JS_DEPS(neo_browser_file_deps, "$UTF8ToString,$stringToNewUTF8");

EM_JS(int, neo_browser_request_open_files_js,
      (unsigned int requestId, const char* title, const char* accept, int multiple), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.requestOpenFiles) {
            console.error('[NeoTools] Browser file picker bridge is unavailable.');
            return 0;
        }
        return Module.neoToolsBrowserFiles.requestOpenFiles(requestId, {
            title: UTF8ToString(title),
            accept: UTF8ToString(accept),
            multiple: multiple !== 0
        }) ? 1 : 0;
    } catch (error) {
        console.error('[NeoTools] Browser file picker request failed:', error);
        return 0;
    }
});


EM_JS(void, neo_browser_release_imported_files_js, (const char* payload), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.releaseImportedFiles) return;
        var value = payload ? UTF8ToString(payload) : String();
        var paths = value ? value.split(String.fromCharCode(10)) : [];
        Module.neoToolsBrowserFiles.releaseImportedFiles(paths);
    } catch (error) {
        console.error('[NeoTools] Unable to release imported browser files:', error);
    }
});

EM_JS(int, neo_browser_request_retained_files_js,
      (unsigned int requestId, const char* title, const char* accept, int multiple), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.requestRetainedFiles) {
            console.error('[NeoTools] Retained browser-file bridge is unavailable.');
            return 0;
        }
        return Module.neoToolsBrowserFiles.requestRetainedFiles(requestId, {
            title: UTF8ToString(title),
            accept: UTF8ToString(accept),
            multiple: multiple !== 0
        }) ? 1 : 0;
    } catch (error) {
        console.error('[NeoTools] Retained browser-file request failed:', error);
        return 0;
    }
});

EM_JS(int, neo_browser_request_retained_directory_js,
      (unsigned int requestId, const char* title, const char* accept), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.requestRetainedDirectory) {
            console.error('[NeoTools] Retained browser-directory bridge is unavailable.');
            return 0;
        }
        return Module.neoToolsBrowserFiles.requestRetainedDirectory(requestId, {
            title: UTF8ToString(title),
            accept: UTF8ToString(accept)
        }) ? 1 : 0;
    } catch (error) {
        console.error('[NeoTools] Retained browser-directory request failed:', error);
        return 0;
    }
});

EM_JS(void, neo_browser_release_retained_file_set_js, (unsigned int sessionId), {
    try {
        if (Module.neoToolsBrowserFiles && Module.neoToolsBrowserFiles.releaseRetainedFileSet) {
            Module.neoToolsBrowserFiles.releaseRetainedFileSet(sessionId);
        }
    } catch (error) {
        console.error('[NeoTools] Unable to release retained browser files:', error);
    }
});

EM_JS(void, neo_browser_retain_only_retained_files_js,
      (unsigned int sessionId, const char* payload), {
    try {
        if (Module.neoToolsBrowserFiles && Module.neoToolsBrowserFiles.retainOnlyRetainedFiles) {
            Module.neoToolsBrowserFiles.retainOnlyRetainedFiles(
                sessionId, UTF8ToString(payload));
        }
    } catch (error) {
        console.error('[NeoTools] Unable to prune retained browser files:', error);
    }
});

EM_ASYNC_JS(char*, neo_browser_read_retained_file_range_js,
            (unsigned int sessionId, unsigned int fileId, double offset,
             size_t length, void* destination), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.readRetainedFileRange) {
            throw new Error('Retained browser-file range bridge is unavailable.');
        }
        await Module.neoToolsBrowserFiles.readRetainedFileRange(
            sessionId, fileId, offset, Number(length), Number(destination));
        return 0;
    } catch (error) {
        var message = error && error.message ? error.message : String(error || 'Unknown browser range-read error.');
        return stringToNewUTF8(message);
    }
});

EM_JS(int, neo_browser_request_retained_export_js,
      (unsigned int requestId, int mode, const char* defaultName, const char* payload), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.requestExportRetainedFiles) {
            console.error('[NeoTools] Retained browser-file export bridge is unavailable.');
            return 0;
        }
        return Module.neoToolsBrowserFiles.requestExportRetainedFiles(
            requestId, mode, UTF8ToString(defaultName), UTF8ToString(payload)) ? 1 : 0;
    } catch (error) {
        console.error('[NeoTools] Retained browser-file export request failed:', error);
        return 0;
    }
});

EM_JS(int, neo_browser_retained_directory_write_supported_js, (), {
    try {
        return Module.neoToolsBrowserFiles &&
               Module.neoToolsBrowserFiles.retainedDirectoryWriteSupported &&
               Module.neoToolsBrowserFiles.retainedDirectoryWriteSupported() ? 1 : 0;
    } catch (error) {
        console.error('[NeoTools] Unable to query retained directory-write support:', error);
        return 0;
    }
});

// Kept only for compatibility with applications that have not yet migrated to
// the callback API. Calling it from a normal wx DOM event can strand the port's
// synchronous event-dispatch chain; new code must use requestOpenFiles().
EM_ASYNC_JS(char*, neo_browser_choose_open_files_js,
            (const char* title, const char* accept, int multiple), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.chooseOpenFiles) {
            console.error('[NeoTools] Browser file picker bridge is unavailable.');
            return 0;
        }
        const paths = await Module.neoToolsBrowserFiles.chooseOpenFiles({
            title: UTF8ToString(title),
            accept: UTF8ToString(accept),
            multiple: multiple !== 0
        });
        if (!paths || paths.length === 0) return 0;
        return stringToNewUTF8(paths.join(String.fromCharCode(10)));
    } catch (error) {
        console.error('[NeoTools] Browser file picker failed:', error);
        return 0;
    }
});

EM_JS(char*, neo_browser_choose_save_file_js,
      (const char* title, const char* defaultFile, const char* defaultExtension), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.chooseSaveFile) {
            console.error('[NeoTools] Browser save bridge is unavailable.');
            return 0;
        }
        const path = Module.neoToolsBrowserFiles.chooseSaveFile({
            title: UTF8ToString(title),
            defaultFile: UTF8ToString(defaultFile),
            defaultExtension: UTF8ToString(defaultExtension)
        });
        if (!path) return 0;
        return stringToNewUTF8(path);
    } catch (error) {
        console.error('[NeoTools] Browser save filename selection failed:', error);
        return 0;
    }
});

EM_JS(int, neo_browser_request_download_file_js,
      (unsigned int requestId, const char* virtualPath, const char* downloadName), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.requestDownloadFile) {
            console.error('[NeoTools] Browser download transaction bridge is unavailable.');
            return 0;
        }
        return Module.neoToolsBrowserFiles.requestDownloadFile(
            requestId, UTF8ToString(virtualPath), UTF8ToString(downloadName)) ? 1 : 0;
    } catch (error) {
        console.error('[NeoTools] Browser download transaction failed to start:', error);
        return 0;
    }
});

EM_JS(int, neo_browser_download_file_js,
      (const char* virtualPath, const char* downloadName), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.downloadFile) {
            console.error('[NeoTools] Browser download bridge is unavailable.');
            return 0;
        }
        return Module.neoToolsBrowserFiles.downloadFile(
            UTF8ToString(virtualPath), UTF8ToString(downloadName)) ? 1 : 0;
    } catch (error) {
        console.error('[NeoTools] Browser download failed:', error);
        return 0;
    }
});

EM_JS(int, neo_browser_prepare_download_bytes_js,
      (const void* bytes, size_t byteCount, const char* downloadName), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.prepareDownloadBytes) {
            console.error('[NeoTools] Browser byte-download bridge is unavailable.');
            return 0;
        }
        var begin = Number(bytes);
        var count = Number(byteCount);
        var copy = new Uint8Array(count);
        if (count > 0) copy.set(HEAPU8.subarray(begin, begin + count));
        return Module.neoToolsBrowserFiles.prepareDownloadBytes(
            copy, UTF8ToString(downloadName)) ? 1 : 0;
    } catch (error) {
        console.error('[NeoTools] Unable to prepare browser byte download:', error);
        return 0;
    }
});

EM_JS(int, neo_browser_package_directory_supported_js, (), {
    try {
        return Module.neoToolsBrowserFiles &&
               Module.neoToolsBrowserFiles.packageDirectorySupported &&
               Module.neoToolsBrowserFiles.packageDirectorySupported() ? 1 : 0;
    } catch (error) {
        console.error('[NeoTools] Unable to query browser package-directory support:', error);
        return 0;
    }
});

EM_JS(int, neo_browser_request_package_directory_js, (unsigned int requestId), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.requestPackageDirectory) {
            console.error('[NeoTools] Browser package-directory bridge is unavailable.');
            return 0;
        }
        return Module.neoToolsBrowserFiles.requestPackageDirectory(requestId) ? 1 : 0;
    } catch (error) {
        console.error('[NeoTools] Browser package-directory request failed:', error);
        return 0;
    }
});

EM_JS(int, neo_browser_request_package_workspace_js,
      (unsigned int requestId, unsigned int sessionId, const char* relativeIniPath,
       const char* relativeFiles), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.requestPackageWorkspace) {
            console.error('[NeoTools] Browser package-workspace bridge is unavailable.');
            return 0;
        }
        return Module.neoToolsBrowserFiles.requestPackageWorkspace(
            requestId,
            sessionId,
            UTF8ToString(relativeIniPath),
            UTF8ToString(relativeFiles)) ? 1 : 0;
    } catch (error) {
        console.error('[NeoTools] Browser package-workspace request failed:', error);
        return 0;
    }
});

EM_JS(int, neo_browser_request_package_commit_js,
      (unsigned int requestId, unsigned int sessionId, const char* workspaceRoot,
       const char* relativeIniPath, const char* relativeFiles), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.requestCommitPackageWorkspace) {
            console.error('[NeoTools] Browser package-commit bridge is unavailable.');
            return 0;
        }
        return Module.neoToolsBrowserFiles.requestCommitPackageWorkspace(
            requestId,
            sessionId,
            UTF8ToString(workspaceRoot),
            UTF8ToString(relativeIniPath),
            UTF8ToString(relativeFiles)) ? 1 : 0;
    } catch (error) {
        console.error('[NeoTools] Browser package-commit request failed:', error);
        return 0;
    }
});

EM_JS(void, neo_browser_set_dark_mode_js, (int enabled), {
    try {
        const dark = enabled !== 0;
        if (typeof document === 'undefined') return;
        document.documentElement.classList.toggle('dark', dark);
        document.documentElement.style.colorScheme = dark ? 'dark' : 'light';
        if (document.body) document.body.classList.toggle('dark', dark);
    } catch (error) {
        console.error('[NeoTools] Unable to synchronize browser theme:', error);
    }
});

#endif

namespace {

std::string safeDownloadName(std::string name) {
    for (char& ch : name) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (ch == '/' || ch == '\\' || value < 0x20u || value == 0x7Fu) ch = '_';
    }
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())) != 0) {
        name.erase(name.begin());
    }
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())) != 0) {
        name.pop_back();
    }
    if (name.empty() || name == "." || name == "..") return "download.bin";
    return name;
}

std::uint64_t nextDownloadPathId() {
    static std::uint64_t next = 1;
    return next++;
}

#if defined(__EMSCRIPTEN__)

using OpenCallbackMap = std::unordered_map<std::uint32_t, neobrowser::OpenFilesCallback>;
using RetainedFileSetCallbackMap =
    std::unordered_map<std::uint32_t, neobrowser::RetainedFileSetCallback>;
using DownloadCallbackMap = std::unordered_map<std::uint32_t, neobrowser::DownloadCallback>;
using RetainedExportCallbackMap =
    std::unordered_map<std::uint32_t, neobrowser::RetainedExportCallback>;
using PackageDirectoryCallbackMap =
    std::unordered_map<std::uint32_t, neobrowser::PackageDirectoryCallback>;
using PackageWorkspaceCallbackMap =
    std::unordered_map<std::uint32_t, neobrowser::PackageWorkspaceCallback>;
using PackageCommitCallbackMap =
    std::unordered_map<std::uint32_t, neobrowser::PackageCommitCallback>;

OpenCallbackMap& openFileCallbacks() {
    static OpenCallbackMap callbacks;
    return callbacks;
}

RetainedFileSetCallbackMap& retainedFileSetCallbacks() {
    static RetainedFileSetCallbackMap callbacks;
    return callbacks;
}

DownloadCallbackMap& downloadCallbacks() {
    static DownloadCallbackMap callbacks;
    return callbacks;
}

RetainedExportCallbackMap& retainedExportCallbacks() {
    static RetainedExportCallbackMap callbacks;
    return callbacks;
}

PackageDirectoryCallbackMap& packageDirectoryCallbacks() {
    static PackageDirectoryCallbackMap callbacks;
    return callbacks;
}

PackageWorkspaceCallbackMap& packageWorkspaceCallbacks() {
    static PackageWorkspaceCallbackMap callbacks;
    return callbacks;
}

PackageCommitCallbackMap& packageCommitCallbacks() {
    static PackageCommitCallbackMap callbacks;
    return callbacks;
}

template <typename Map>
std::uint32_t nextRequestId(Map& callbacks) {
    static std::uint32_t next = 1;
    for (;;) {
        const std::uint32_t candidate = next++;
        if (next == 0) next = 1;
        if (candidate != 0 && callbacks.find(candidate) == callbacks.end()) return candidate;
    }
}

std::vector<std::filesystem::path> parsePathPayload(const char* payload) {
    std::vector<std::filesystem::path> paths;
    if (payload == nullptr || *payload == '\0') return paths;

    const std::string value(payload);
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t end = value.find('\n', begin);
        const std::string item = value.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!item.empty()) paths.emplace_back(item);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return paths;
}

std::vector<std::string> parseStringPayload(const char* payload) {
    std::vector<std::string> values;
    if (payload == nullptr || *payload == '\0') return values;

    const std::string source(payload);
    std::size_t begin = 0;
    while (begin <= source.size()) {
        const std::size_t end = source.find('\n', begin);
        const std::string item = source.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!item.empty()) values.push_back(item);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return values;
}


int hexDigitValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string percentDecode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            result.push_back(value[index]);
            continue;
        }
        if (index + 2u >= value.size()) {
            throw std::runtime_error("Invalid percent-encoded browser path.");
        }
        const int high = hexDigitValue(value[index + 1u]);
        const int low = hexDigitValue(value[index + 2u]);
        if (high < 0 || low < 0) {
            throw std::runtime_error("Invalid percent-encoded browser path.");
        }
        result.push_back(static_cast<char>((high << 4) | low));
        index += 2u;
    }
    return result;
}

std::string percentEncode(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                          ch == '.' || ch == '~' || ch == '/';
        if (safe) {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back('%');
            result.push_back(kHex[(ch >> 4u) & 0x0Fu]);
            result.push_back(kHex[ch & 0x0Fu]);
        }
    }
    return result;
}

std::uint64_t parseUnsigned64(std::string_view text, const char* fieldName) {
    if (text.empty()) throw std::runtime_error(std::string("Missing ") + fieldName + '.');
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error(std::string("Invalid ") + fieldName + '.');
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
            throw std::runtime_error(std::string(fieldName) + " is too large.");
        }
        value = value * 10u + digit;
    }
    return value;
}

std::vector<neobrowser::RetainedFileInfo> parseRetainedFilePayload(const char* payload) {
    std::vector<neobrowser::RetainedFileInfo> files;
    if (payload == nullptr || *payload == '\0') return files;
    const std::string source(payload);
    std::size_t begin = 0;
    while (begin <= source.size()) {
        const std::size_t end = source.find('\n', begin);
        const std::string_view line(source.data() + begin,
            (end == std::string::npos ? source.size() : end) - begin);
        if (!line.empty()) {
            const std::size_t first = line.find('\t');
            const std::size_t second = first == std::string_view::npos
                ? std::string_view::npos : line.find('\t', first + 1u);
            if (first == std::string_view::npos || second == std::string_view::npos) {
                throw std::runtime_error("Invalid retained browser-file metadata.");
            }
            const std::uint64_t fileId = parseUnsigned64(line.substr(0, first), "browser file ID");
            const std::uint64_t size = parseUnsigned64(
                line.substr(first + 1u, second - first - 1u), "browser file size");
            if (fileId == 0 || fileId > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("Browser file ID is out of range.");
            }
            neobrowser::RetainedFileInfo file;
            file.fileId = static_cast<std::uint32_t>(fileId);
            file.size = size;
            file.relativePath = percentDecode(line.substr(second + 1u));
            if (file.relativePath.empty()) {
                throw std::runtime_error("Browser file path must not be empty.");
            }
            files.push_back(std::move(file));
        }
        if (end == std::string::npos) break;
        begin = end + 1u;
    }
    return files;
}

std::string serializeRetainedExportPayload(
    const std::vector<neobrowser::RetainedExportEntry>& entries) {
    std::ostringstream output;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        if (index != 0u) output << '\n';
        output << entry.sessionId << '\t'
               << entry.fileId << '\t'
               << entry.offset << '\t'
               << entry.size << '\t'
               << percentEncode(entry.outputPath);
    }
    return output.str();
}


std::string serializeStringPayload(const std::vector<std::string>& values) {
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0u) output << '\n';
        output << values[index];
    }
    return output.str();
}

std::string takeAllocatedString(char* value) {
    if (value == nullptr) return {};
    std::string result(value);
    std::free(value);
    return result;
}

void runOpenFilesCallback(neobrowser::OpenFilesCallback callback,
                          neobrowser::OpenFilesResult result) noexcept {
    // A callback that throws has not established a reliable owner for the
    // imported MEMFS session. Keep a release copy until delivery succeeds.
    std::vector<std::filesystem::path> releaseOnFailure;
    try {
        releaseOnFailure = result.paths;
        callback(std::move(result));
    } catch (const std::exception& exception) {
        neobrowser::releaseImportedFiles(
            releaseOnFailure.empty() ? result.paths : releaseOnFailure);
        std::fprintf(stderr, "[NeoTools] Browser file completion failed: %s\n", exception.what());
    } catch (...) {
        neobrowser::releaseImportedFiles(
            releaseOnFailure.empty() ? result.paths : releaseOnFailure);
        std::fprintf(stderr, "[NeoTools] Browser file completion failed with an unknown exception.\n");
    }
}

void runRetainedFileSetCallback(neobrowser::RetainedFileSetCallback callback,
                                neobrowser::RetainedFileSetResult result) noexcept {
    try {
        callback(std::move(result));
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[NeoTools] Retained browser-file completion failed: %s\n",
                     exception.what());
    } catch (...) {
        std::fprintf(stderr,
                     "[NeoTools] Retained browser-file completion failed with an unknown exception.\n");
    }
}

void runDownloadCallback(neobrowser::DownloadCallback callback,
                         neobrowser::DownloadResult result) noexcept {
    try {
        callback(std::move(result));
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[NeoTools] Browser download completion failed: %s\n", exception.what());
    } catch (...) {
        std::fprintf(stderr, "[NeoTools] Browser download completion failed with an unknown exception.\n");
    }
}

void runRetainedExportCallback(neobrowser::RetainedExportCallback callback,
                               neobrowser::RetainedExportResult result) noexcept {
    try {
        callback(std::move(result));
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[NeoTools] Retained browser-file export completion failed: %s\n",
                     exception.what());
    } catch (...) {
        std::fprintf(stderr,
                     "[NeoTools] Retained browser-file export completion failed with an unknown exception.\n");
    }
}

void runPackageDirectoryCallback(neobrowser::PackageDirectoryCallback callback,
                                 neobrowser::PackageDirectoryResult result) noexcept {
    try {
        callback(std::move(result));
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[NeoTools] Browser package-directory completion failed: %s\n",
                     exception.what());
    } catch (...) {
        std::fprintf(stderr,
                     "[NeoTools] Browser package-directory completion failed with an unknown exception.\n");
    }
}

void runPackageWorkspaceCallback(neobrowser::PackageWorkspaceCallback callback,
                                 neobrowser::PackageWorkspaceResult result) noexcept {
    try {
        callback(std::move(result));
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[NeoTools] Browser package-workspace completion failed: %s\n",
                     exception.what());
    } catch (...) {
        std::fprintf(stderr,
                     "[NeoTools] Browser package-workspace completion failed with an unknown exception.\n");
    }
}

void runPackageCommitCallback(neobrowser::PackageCommitCallback callback,
                              neobrowser::PackageCommitResult result) noexcept {
    try {
        callback(std::move(result));
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[NeoTools] Browser package-commit completion failed: %s\n",
                     exception.what());
    } catch (...) {
        std::fprintf(stderr,
                     "[NeoTools] Browser package-commit completion failed with an unknown exception.\n");
    }
}

struct PendingOpenFilesDelivery {
    neobrowser::OpenFilesCallback callback;
    neobrowser::OpenFilesResult result;
    bool delivered = false;

    ~PendingOpenFilesDelivery() {
        // wx may destroy queued CallAfter functors while the application is
        // shutting down. Release the import if the callback never ran.
        if (!delivered) neobrowser::releaseImportedFiles(result.paths);
    }
};

void scheduleOpenFilesCallback(neobrowser::OpenFilesCallback callback,
                               neobrowser::OpenFilesResult result) noexcept {
    if (wxTheApp != nullptr) {
        auto delivery = std::make_shared<PendingOpenFilesDelivery>();
        delivery->callback = std::move(callback);
        delivery->result = std::move(result);
        wxTheApp->CallAfter([delivery]() mutable {
            delivery->delivered = true;
            runOpenFilesCallback(
                std::move(delivery->callback), std::move(delivery->result));
        });
        return;
    }
    runOpenFilesCallback(std::move(callback), std::move(result));
}

void scheduleDownloadCallback(neobrowser::DownloadCallback callback,
                              neobrowser::DownloadResult result) noexcept {
    if (wxTheApp != nullptr) {
        wxTheApp->CallAfter(
            [callback = std::move(callback), result = std::move(result)]() mutable {
                runDownloadCallback(std::move(callback), std::move(result));
            });
        return;
    }
    runDownloadCallback(std::move(callback), std::move(result));
}

void scheduleRetainedExportCallback(neobrowser::RetainedExportCallback callback,
                                    neobrowser::RetainedExportResult result) noexcept {
    if (wxTheApp != nullptr) {
        wxTheApp->CallAfter(
            [callback = std::move(callback), result = std::move(result)]() mutable {
                runRetainedExportCallback(std::move(callback), std::move(result));
            });
        return;
    }
    runRetainedExportCallback(std::move(callback), std::move(result));
}

void schedulePackageDirectoryCallback(neobrowser::PackageDirectoryCallback callback,
                                      neobrowser::PackageDirectoryResult result) noexcept {
    if (wxTheApp != nullptr) {
        wxTheApp->CallAfter(
            [callback = std::move(callback), result = std::move(result)]() mutable {
                runPackageDirectoryCallback(std::move(callback), std::move(result));
            });
        return;
    }
    runPackageDirectoryCallback(std::move(callback), std::move(result));
}

void schedulePackageWorkspaceCallback(neobrowser::PackageWorkspaceCallback callback,
                                      neobrowser::PackageWorkspaceResult result) noexcept {
    if (wxTheApp != nullptr) {
        wxTheApp->CallAfter(
            [callback = std::move(callback), result = std::move(result)]() mutable {
                runPackageWorkspaceCallback(std::move(callback), std::move(result));
            });
        return;
    }
    runPackageWorkspaceCallback(std::move(callback), std::move(result));
}

void schedulePackageCommitCallback(neobrowser::PackageCommitCallback callback,
                                   neobrowser::PackageCommitResult result) noexcept {
    if (wxTheApp != nullptr) {
        wxTheApp->CallAfter(
            [callback = std::move(callback), result = std::move(result)]() mutable {
                runPackageCommitCallback(std::move(callback), std::move(result));
            });
        return;
    }
    runPackageCommitCallback(std::move(callback), std::move(result));
}

void invokeRetainedFileSetCallback(std::uint32_t requestId,
                                   std::uint32_t sessionId,
                                   std::string displayName,
                                   std::vector<neobrowser::RetainedFileInfo> files,
                                   std::string error) noexcept {
    auto& callbacks = retainedFileSetCallbacks();
    const auto found = callbacks.find(requestId);
    if (found == callbacks.end()) return;
    neobrowser::RetainedFileSetCallback callback = std::move(found->second);
    callbacks.erase(found);
    // Deliberately invoke inline. JavaScript calls this export through
    // ccall(..., {async:true}), allowing the callback to perform bounded
    // Asyncify-backed range reads while parsing an archive index.
    runRetainedFileSetCallback(
        std::move(callback),
        neobrowser::RetainedFileSetResult{
            sessionId, std::move(displayName), std::move(files), std::move(error)});
}

void invokeOpenFilesCallback(std::uint32_t requestId,
                             std::vector<std::filesystem::path> paths,
                             std::string error) noexcept {
    auto& callbacks = openFileCallbacks();
    const auto found = callbacks.find(requestId);
    if (found == callbacks.end()) {
        neobrowser::releaseImportedFiles(paths);
        return;
    }

    neobrowser::OpenFilesCallback callback = std::move(found->second);
    callbacks.erase(found);
    scheduleOpenFilesCallback(
        std::move(callback),
        neobrowser::OpenFilesResult{std::move(paths), std::move(error)});
}

neobrowser::DownloadDisposition downloadDispositionFromInt(int value) noexcept {
    if (value == static_cast<int>(neobrowser::DownloadDisposition::Saved)) {
        return neobrowser::DownloadDisposition::Saved;
    }
    if (value == static_cast<int>(neobrowser::DownloadDisposition::Ready)) {
        return neobrowser::DownloadDisposition::Ready;
    }
    return neobrowser::DownloadDisposition::Cancelled;
}

void invokeDownloadCallback(std::uint32_t requestId,
                            int disposition,
                            std::string error) noexcept {
    auto& callbacks = downloadCallbacks();
    const auto found = callbacks.find(requestId);
    if (found == callbacks.end()) return;

    neobrowser::DownloadCallback callback = std::move(found->second);
    callbacks.erase(found);
    scheduleDownloadCallback(
        std::move(callback),
        neobrowser::DownloadResult{downloadDispositionFromInt(disposition), std::move(error)});
}

void invokeRetainedExportCallback(std::uint32_t requestId,
                                 int disposition,
                                 std::size_t filesWritten,
                                 std::uint64_t bytesWritten,
                                 bool usedDirectory,
                                 std::string error) noexcept {
    auto& callbacks = retainedExportCallbacks();
    const auto found = callbacks.find(requestId);
    if (found == callbacks.end()) return;
    neobrowser::RetainedExportCallback callback = std::move(found->second);
    callbacks.erase(found);
    scheduleRetainedExportCallback(
        std::move(callback),
        neobrowser::RetainedExportResult{
            downloadDispositionFromInt(disposition), filesWritten, bytesWritten,
            usedDirectory, std::move(error)});
}

void invokePackageDirectoryCallback(std::uint32_t requestId,
                                    std::uint32_t sessionId,
                                    std::string displayName,
                                    std::vector<std::string> iniPaths,
                                    std::string error) noexcept {
    auto& callbacks = packageDirectoryCallbacks();
    const auto found = callbacks.find(requestId);
    if (found == callbacks.end()) return;

    neobrowser::PackageDirectoryCallback callback = std::move(found->second);
    callbacks.erase(found);
    schedulePackageDirectoryCallback(
        std::move(callback),
        neobrowser::PackageDirectoryResult{
            sessionId, std::move(displayName), std::move(iniPaths), std::move(error)});
}

void invokePackageWorkspaceCallback(std::uint32_t requestId,
                                    std::uint32_t sessionId,
                                    std::string workspaceRoot,
                                    std::string iniPath,
                                    std::string relativeIniPath,
                                    bool iniExisted,
                                    std::string error) noexcept {
    auto& callbacks = packageWorkspaceCallbacks();
    const auto found = callbacks.find(requestId);
    if (found == callbacks.end()) return;

    neobrowser::PackageWorkspaceCallback callback = std::move(found->second);
    callbacks.erase(found);
    schedulePackageWorkspaceCallback(
        std::move(callback),
        neobrowser::PackageWorkspaceResult{
            sessionId,
            std::filesystem::path(std::move(workspaceRoot)),
            std::filesystem::path(std::move(iniPath)),
            std::move(relativeIniPath),
            iniExisted,
            std::move(error)});
}

void invokePackageCommitCallback(std::uint32_t requestId,
                                 std::size_t filesWritten,
                                 std::size_t filesReused,
                                 bool iniChanged,
                                 std::string error) noexcept {
    auto& callbacks = packageCommitCallbacks();
    const auto found = callbacks.find(requestId);
    if (found == callbacks.end()) return;

    neobrowser::PackageCommitCallback callback = std::move(found->second);
    callbacks.erase(found);
    schedulePackageCommitCallback(
        std::move(callback),
        neobrowser::PackageCommitResult{
            filesWritten, filesReused, iniChanged, std::move(error)});
}

#endif

} // namespace

#if defined(__EMSCRIPTEN__)

extern "C" EMSCRIPTEN_KEEPALIVE void neo_browser_retained_file_set_completed(
    unsigned int requestId,
    unsigned int sessionId,
    const char* displayName,
    const char* payload,
    const char* error) {
    try {
        invokeRetainedFileSetCallback(
            requestId,
            sessionId,
            displayName == nullptr ? std::string{} : std::string(displayName),
            parseRetainedFilePayload(payload),
            error == nullptr ? std::string{} : std::string(error));
    } catch (const std::exception& exception) {
        invokeRetainedFileSetCallback(
            requestId, sessionId, {}, {}, exception.what());
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void neo_browser_retained_export_completed(
    unsigned int requestId,
    int disposition,
    unsigned int filesWritten,
    const char* bytesWritten,
    int usedDirectory,
    const char* error) {
    std::uint64_t parsedBytes = 0;
    std::string parsedError = error == nullptr ? std::string{} : std::string(error);
    try {
        parsedBytes = parseUnsigned64(
            bytesWritten == nullptr ? std::string_view{"0"} : std::string_view{bytesWritten},
            "browser export byte count");
    } catch (const std::exception& exception) {
        if (parsedError.empty()) parsedError = exception.what();
    }
    invokeRetainedExportCallback(
        requestId,
        disposition,
        static_cast<std::size_t>(filesWritten),
        parsedBytes,
        usedDirectory != 0,
        std::move(parsedError));
}

extern "C" EMSCRIPTEN_KEEPALIVE void neo_browser_open_files_completed(
    unsigned int requestId,
    const char* payload,
    const char* error) {
    invokeOpenFilesCallback(
        requestId,
        parsePathPayload(payload),
        error == nullptr ? std::string{} : std::string(error));
}

extern "C" EMSCRIPTEN_KEEPALIVE void neo_browser_download_completed(
    unsigned int requestId,
    int disposition,
    const char* error) {
    invokeDownloadCallback(
        requestId,
        disposition,
        error == nullptr ? std::string{} : std::string(error));
}

extern "C" EMSCRIPTEN_KEEPALIVE void neo_browser_package_directory_completed(
    unsigned int requestId,
    unsigned int sessionId,
    const char* displayName,
    const char* iniPaths,
    const char* error) {
    invokePackageDirectoryCallback(
        requestId,
        sessionId,
        displayName == nullptr ? std::string{} : std::string(displayName),
        parseStringPayload(iniPaths),
        error == nullptr ? std::string{} : std::string(error));
}

extern "C" EMSCRIPTEN_KEEPALIVE void neo_browser_package_workspace_completed(
    unsigned int requestId,
    unsigned int sessionId,
    const char* workspaceRoot,
    const char* iniPath,
    const char* relativeIniPath,
    int iniExisted,
    const char* error) {
    invokePackageWorkspaceCallback(
        requestId,
        sessionId,
        workspaceRoot == nullptr ? std::string{} : std::string(workspaceRoot),
        iniPath == nullptr ? std::string{} : std::string(iniPath),
        relativeIniPath == nullptr ? std::string{} : std::string(relativeIniPath),
        iniExisted != 0,
        error == nullptr ? std::string{} : std::string(error));
}

extern "C" EMSCRIPTEN_KEEPALIVE void neo_browser_package_commit_completed(
    unsigned int requestId,
    unsigned int filesWritten,
    unsigned int filesReused,
    int iniChanged,
    const char* error) {
    invokePackageCommitCallback(
        requestId,
        static_cast<std::size_t>(filesWritten),
        static_cast<std::size_t>(filesReused),
        iniChanged != 0,
        error == nullptr ? std::string{} : std::string(error));
}

#endif

namespace neobrowser {

BrowserImportLease::BrowserImportLease(std::vector<std::filesystem::path> paths)
    : paths_(std::move(paths)) {}

BrowserImportLease::~BrowserImportLease() noexcept { reset(); }

BrowserImportLease::BrowserImportLease(BrowserImportLease&& other) noexcept
    : paths_(std::move(other.paths_)) {
    other.paths_.clear();
}

BrowserImportLease& BrowserImportLease::operator=(BrowserImportLease&& other) noexcept {
    if (this != &other) {
        reset();
        paths_ = std::move(other.paths_);
        other.paths_.clear();
    }
    return *this;
}

void BrowserImportLease::reset(std::vector<std::filesystem::path> paths) {
    if (!paths_.empty()) releaseImportedFiles(paths_);
    paths_ = std::move(paths);
}

std::vector<std::filesystem::path> BrowserImportLease::detach() noexcept {
    std::vector<std::filesystem::path> result = std::move(paths_);
    paths_.clear();
    return result;
}

void requestRetainedFiles(const std::string& title,
                          const std::string& accept,
                          bool multiple,
                          RetainedFileSetCallback callback) {
    if (!callback) return;
#if defined(__EMSCRIPTEN__)
    auto& callbacks = retainedFileSetCallbacks();
    const std::uint32_t requestId = nextRequestId(callbacks);
    callbacks.emplace(requestId, std::move(callback));
    if (neo_browser_request_retained_files_js(
            requestId, title.c_str(), accept.c_str(), multiple ? 1 : 0) == 0) {
        invokeRetainedFileSetCallback(
            requestId, 0, {}, {}, "The retained browser-file picker is unavailable.");
    }
#else
    (void)title;
    (void)accept;
    (void)multiple;
    callback(RetainedFileSetResult{
        0, {}, {}, "Retained browser-file selection is unavailable in this build."});
#endif
}

void requestRetainedDirectory(const std::string& title,
                              const std::string& accept,
                              RetainedFileSetCallback callback) {
    if (!callback) return;
#if defined(__EMSCRIPTEN__)
    auto& callbacks = retainedFileSetCallbacks();
    const std::uint32_t requestId = nextRequestId(callbacks);
    callbacks.emplace(requestId, std::move(callback));
    if (neo_browser_request_retained_directory_js(
            requestId, title.c_str(), accept.c_str()) == 0) {
        invokeRetainedFileSetCallback(
            requestId, 0, {}, {}, "The retained browser-directory picker is unavailable.");
    }
#else
    (void)title;
    (void)accept;
    callback(RetainedFileSetResult{
        0, {}, {}, "Retained browser-directory selection is unavailable in this build."});
#endif
}

void releaseRetainedFileSet(std::uint32_t sessionId) {
#if defined(__EMSCRIPTEN__)
    if (sessionId != 0) neo_browser_release_retained_file_set_js(sessionId);
#else
    (void)sessionId;
#endif
}

void retainOnlyRetainedFiles(std::uint32_t sessionId,
                             const std::vector<std::uint32_t>& fileIds) {
#if defined(__EMSCRIPTEN__)
    if (sessionId == 0) return;
    std::ostringstream payload;
    for (std::size_t index = 0; index < fileIds.size(); ++index) {
        if (index != 0u) payload << ',';
        payload << fileIds[index];
    }
    const std::string encoded = payload.str();
    neo_browser_retain_only_retained_files_js(sessionId, encoded.c_str());
#else
    (void)sessionId;
    (void)fileIds;
#endif
}

bool readRetainedFileRange(std::uint32_t sessionId,
                           std::uint32_t fileId,
                           std::uint64_t offset,
                           std::size_t length,
                           std::vector<std::uint8_t>& bytes,
                           std::string& error) {
    bytes.clear();
    error.clear();
#if defined(__EMSCRIPTEN__)
    constexpr std::size_t kMaximumIndexRange = 64u * 1024u * 1024u;
    if (sessionId == 0 || fileId == 0) {
        error = "Retained browser-file identity is invalid.";
        return false;
    }
    if (length > kMaximumIndexRange) {
        error = "Requested browser index range exceeds the 64 MiB safety limit.";
        return false;
    }
    if (offset > 9007199254740991ULL ||
        static_cast<std::uint64_t>(length) > 9007199254740991ULL - offset) {
        error = "Requested browser-file range exceeds JavaScript's exact integer range.";
        return false;
    }
    try {
        bytes.resize(length);
    } catch (const std::exception& exception) {
        error = std::string("Unable to allocate browser index range: ") + exception.what();
        return false;
    }
    char* jsError = neo_browser_read_retained_file_range_js(
        sessionId, fileId, static_cast<double>(offset), length,
        bytes.empty() ? nullptr : bytes.data());
    error = takeAllocatedString(jsError);
    if (!error.empty()) {
        bytes.clear();
        return false;
    }
    return true;
#else
    (void)sessionId;
    (void)fileId;
    (void)offset;
    (void)length;
    error = "Retained browser-file range access is unavailable in this build.";
    return false;
#endif
}

bool retainedDirectoryWriteSupported() {
#if defined(__EMSCRIPTEN__)
    return neo_browser_retained_directory_write_supported_js() != 0;
#else
    return false;
#endif
}

void requestOpenFiles(const std::string& title,
                      const std::string& accept,
                      bool multiple,
                      OpenFilesCallback callback) {
    if (!callback) return;
#if defined(__EMSCRIPTEN__)
    auto& callbacks = openFileCallbacks();
    const std::uint32_t requestId = nextRequestId(callbacks);
    callbacks.emplace(requestId, std::move(callback));
    if (neo_browser_request_open_files_js(
            requestId, title.c_str(), accept.c_str(), multiple ? 1 : 0) == 0) {
        invokeOpenFilesCallback(
            requestId,
            {},
            "The browser file picker bridge is unavailable.");
    }
#else
    (void)title;
    (void)accept;
    (void)multiple;
    callback(OpenFilesResult{{}, "Browser file selection is unavailable in this build."});
#endif
}

void requestOpenFilesOwned(const std::string& title,
                           const std::string& accept,
                           bool multiple,
                           OwnedOpenFilesCallback callback) {
    if (!callback) return;
    requestOpenFiles(
        title, accept, multiple,
        [callback = std::move(callback)](OpenFilesResult result) mutable {
            OwnedOpenFilesResult owned{
                BrowserImportLease(std::move(result.paths)), std::move(result.error)};
            callback(std::move(owned));
        });
}

void releaseImportedFiles(const std::vector<std::filesystem::path>& paths) noexcept {
#if defined(__EMSCRIPTEN__)
    if (paths.empty()) return;
    try {
        std::ostringstream payload;
        for (std::size_t index = 0; index < paths.size(); ++index) {
            if (index != 0u) payload << '\n';
            payload << paths[index].generic_string();
        }
        const std::string encoded = payload.str();
        neo_browser_release_imported_files_js(encoded.c_str());
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[NeoTools] Unable to release browser imports: %s\n", exception.what());
    } catch (...) {
        std::fprintf(stderr, "[NeoTools] Unable to release browser imports.\n");
    }
#else
    (void)paths;
#endif
}

std::vector<std::filesystem::path> chooseOpenFiles(const std::string& title,
                                                   const std::string& accept,
                                                   bool multiple) {
#if defined(__EMSCRIPTEN__)
    const std::string payload = takeAllocatedString(
        neo_browser_choose_open_files_js(title.c_str(), accept.c_str(), multiple ? 1 : 0));
    return parsePathPayload(payload.c_str());
#else
    (void)title;
    (void)accept;
    (void)multiple;
    return {};
#endif
}

std::optional<std::filesystem::path> chooseSaveFile(const std::string& title,
                                                    const std::string& defaultFile,
                                                    const std::string& defaultExtension) {
#if defined(__EMSCRIPTEN__)
    const std::string path = takeAllocatedString(
        neo_browser_choose_save_file_js(
            title.c_str(), defaultFile.c_str(), defaultExtension.c_str()));
    if (path.empty()) return std::nullopt;
    return std::filesystem::path(path);
#else
    (void)title;
    (void)defaultFile;
    (void)defaultExtension;
    return std::nullopt;
#endif
}

std::filesystem::path createDownloadPath(const std::string& downloadName) {
#if defined(__EMSCRIPTEN__)
    const std::filesystem::path directory =
        std::filesystem::path("/tmp/neotools-download") /
        std::to_string(nextDownloadPathId());
    std::filesystem::create_directories(directory);
    return directory / safeDownloadName(downloadName);
#else
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "neotools-download" /
        std::to_string(nextDownloadPathId());
    std::filesystem::create_directories(directory);
    return directory / safeDownloadName(downloadName);
#endif
}

void requestDownloadFile(const std::filesystem::path& virtualPath,
                         const std::string& downloadName,
                         DownloadCallback callback) {
    if (!callback) return;
#if defined(__EMSCRIPTEN__)
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(virtualPath, filesystemError) || filesystemError) {
        scheduleDownloadCallback(
            std::move(callback),
            DownloadResult{
                DownloadDisposition::Cancelled,
                "The prepared browser download file does not exist."});
        return;
    }

    const std::string path = virtualPath.generic_string();
    const std::string name = downloadName.empty()
        ? virtualPath.filename().generic_string()
        : downloadName;
    auto& callbacks = downloadCallbacks();
    const std::uint32_t requestId = nextRequestId(callbacks);
    callbacks.emplace(requestId, std::move(callback));
    if (neo_browser_request_download_file_js(requestId, path.c_str(), name.c_str()) == 0) {
        invokeDownloadCallback(
            requestId,
            static_cast<int>(DownloadDisposition::Cancelled),
            "The browser download bridge is unavailable.");
    }
#else
    (void)virtualPath;
    (void)downloadName;
    callback(DownloadResult{
        DownloadDisposition::Cancelled,
        "Browser downloads are unavailable in this build."});
#endif
}

bool prepareDownloadBytes(const void* bytes,
                          std::size_t byteCount,
                          const std::string& downloadName) {
#if defined(__EMSCRIPTEN__)
    if (byteCount > 0 && bytes == nullptr) return false;
    const std::string safeName = safeDownloadName(downloadName);
    return neo_browser_prepare_download_bytes_js(
        bytes, byteCount, safeName.c_str()) != 0;
#else
    (void)bytes;
    (void)byteCount;
    (void)downloadName;
    return false;
#endif
}

bool downloadFile(const std::filesystem::path& virtualPath,
                  const std::string& downloadName) {
#if defined(__EMSCRIPTEN__)
    std::error_code error;
    if (!std::filesystem::is_regular_file(virtualPath, error) || error) return false;
    const std::string path = virtualPath.generic_string();
    const std::string name = downloadName.empty()
        ? virtualPath.filename().generic_string()
        : downloadName;
    return neo_browser_download_file_js(path.c_str(), name.c_str()) != 0;
#else
    (void)virtualPath;
    (void)downloadName;
    return false;
#endif
}

void requestExportRetainedFiles(RetainedExportMode mode,
                                const std::string& defaultName,
                                const std::vector<RetainedExportEntry>& entries,
                                RetainedExportCallback callback) {
    if (!callback) return;
#if defined(__EMSCRIPTEN__)
    if (entries.empty()) {
        callback(RetainedExportResult{
            DownloadDisposition::Cancelled, 0, 0, false,
            "No retained browser-file ranges were selected for export."});
        return;
    }
    constexpr std::uint64_t kMaximumExactJavaScriptInteger = 9007199254740991ULL;
    for (const auto& entry : entries) {
        if (entry.sessionId == 0 || entry.fileId == 0 || entry.outputPath.empty() ||
            entry.outputPath.find('\n') != std::string::npos ||
            entry.outputPath.find('\r') != std::string::npos ||
            entry.outputPath.find('\t') != std::string::npos) {
            callback(RetainedExportResult{
                DownloadDisposition::Cancelled, 0, 0, false,
                "A retained browser-file export entry is invalid."});
            return;
        }
        if (entry.offset > std::numeric_limits<std::uint64_t>::max() - entry.size) {
            callback(RetainedExportResult{
                DownloadDisposition::Cancelled, 0, 0, false,
                "A retained browser-file export range overflows."});
            return;
        }
        if (entry.offset > kMaximumExactJavaScriptInteger ||
            entry.size > kMaximumExactJavaScriptInteger - entry.offset) {
            callback(RetainedExportResult{
                DownloadDisposition::Cancelled, 0, 0, false,
                "A retained browser-file export range exceeds JavaScript's exact integer range."});
            return;
        }
    }
    const std::string payload = serializeRetainedExportPayload(entries);
    auto& callbacks = retainedExportCallbacks();
    const std::uint32_t requestId = nextRequestId(callbacks);
    callbacks.emplace(requestId, std::move(callback));
    if (neo_browser_request_retained_export_js(
            requestId, static_cast<int>(mode),
            safeDownloadName(defaultName).c_str(), payload.c_str()) == 0) {
        invokeRetainedExportCallback(
            requestId, 0, 0, 0, false,
            "The retained browser-file export bridge is unavailable.");
    }
#else
    (void)mode;
    (void)defaultName;
    (void)entries;
    callback(RetainedExportResult{
        DownloadDisposition::Cancelled, 0, 0, false,
        "Retained browser-file export is unavailable in this build."});
#endif
}

bool packageDirectoryAccessSupported() {
#if defined(__EMSCRIPTEN__)
    return neo_browser_package_directory_supported_js() != 0;
#else
    return false;
#endif
}

std::string normalizePackageRelativePath(std::string path, bool requireIni) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && std::isspace(static_cast<unsigned char>(path.front())) != 0) {
        path.erase(path.begin());
    }
    while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back())) != 0) {
        path.pop_back();
    }
    if (path.empty()) {
        throw std::runtime_error("Package-relative path must not be empty.");
    }
    if (path.front() == '/' || (path.size() >= 2u && path[1] == ':')) {
        throw std::runtime_error("Package paths must be relative to the selected installer folder.");
    }

    std::vector<std::string> components;
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::string component = path.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (component.empty() || component == "." || component == "..") {
            throw std::runtime_error(
                "Package paths must not contain empty, current, or parent components.");
        }
        if (component.find(':') != std::string::npos) {
            throw std::runtime_error("Package paths must not contain a colon.");
        }
        for (unsigned char ch : component) {
            if (ch < 0x20u || ch == 0x7Fu) {
                throw std::runtime_error("Package paths must not contain control characters.");
            }
        }
        components.push_back(component);
        if (end == std::string::npos) break;
        begin = end + 1u;
    }

    std::ostringstream normalized;
    for (std::size_t index = 0; index < components.size(); ++index) {
        if (index != 0u) normalized << '/';
        normalized << components[index];
    }
    std::string result = normalized.str();
    if (requireIni) {
        const std::string leaf = components.back();
        const std::size_t dot = leaf.find_last_of('.');
        std::string extension = dot == std::string::npos ? std::string{} : leaf.substr(dot);
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (extension.empty()) {
            result += ".ini";
        } else if (extension != ".ini") {
            throw std::runtime_error("The selected installer configuration must use the .ini extension.");
        }
    }
    return result;
}

void requestPackageDirectory(PackageDirectoryCallback callback) {
    if (!callback) return;
#if defined(__EMSCRIPTEN__)
    auto& callbacks = packageDirectoryCallbacks();
    const std::uint32_t requestId = nextRequestId(callbacks);
    callbacks.emplace(requestId, std::move(callback));
    if (neo_browser_request_package_directory_js(requestId) == 0) {
        invokePackageDirectoryCallback(
            requestId, 0, {}, {},
            "Writable installer-folder selection is unavailable in this browser.");
    }
#else
    callback(PackageDirectoryResult{
        0, {}, {}, "Browser package-directory access is unavailable in this build."});
#endif
}

void requestPackageWorkspace(std::uint32_t sessionId,
                             const std::string& relativeIniPath,
                             const std::vector<std::string>& relativeFiles,
                             PackageWorkspaceCallback callback) {
    if (!callback) return;
#if defined(__EMSCRIPTEN__)
    try {
        const std::string normalizedIni = normalizePackageRelativePath(relativeIniPath, true);
        std::vector<std::string> normalizedFiles;
        normalizedFiles.reserve(relativeFiles.size() + 1u);
        normalizedFiles.push_back(normalizedIni);
        for (const auto& file : relativeFiles) {
            const std::string normalized = normalizePackageRelativePath(file, false);
            if (std::find(normalizedFiles.begin(), normalizedFiles.end(), normalized) ==
                normalizedFiles.end()) {
                normalizedFiles.push_back(normalized);
            }
        }
        const std::string payload = serializeStringPayload(normalizedFiles);
        auto& callbacks = packageWorkspaceCallbacks();
        const std::uint32_t requestId = nextRequestId(callbacks);
        callbacks.emplace(requestId, std::move(callback));
        if (neo_browser_request_package_workspace_js(
                requestId, sessionId, normalizedIni.c_str(), payload.c_str()) == 0) {
            invokePackageWorkspaceCallback(
                requestId, sessionId, {}, {}, normalizedIni, false,
                "The browser package-workspace bridge is unavailable.");
        }
    } catch (const std::exception& exception) {
        callback(PackageWorkspaceResult{
            sessionId, {}, {}, {}, false, exception.what()});
    }
#else
    (void)sessionId;
    (void)relativeIniPath;
    (void)relativeFiles;
    callback(PackageWorkspaceResult{
        0, {}, {}, {}, false,
        "Browser package workspaces are unavailable in this build."});
#endif
}

void requestCommitPackageWorkspace(std::uint32_t sessionId,
                                   const std::filesystem::path& workspaceRoot,
                                   const std::string& relativeIniPath,
                                   const std::vector<std::string>& relativeFiles,
                                   PackageCommitCallback callback) {
    if (!callback) return;
#if defined(__EMSCRIPTEN__)
    try {
        const std::string normalizedIni = normalizePackageRelativePath(relativeIniPath, true);
        std::vector<std::string> normalizedFiles;
        normalizedFiles.reserve(relativeFiles.size() + 1u);
        for (const auto& file : relativeFiles) {
            const std::string normalized = normalizePackageRelativePath(file, false);
            if (std::find(normalizedFiles.begin(), normalizedFiles.end(), normalized) ==
                normalizedFiles.end()) {
                normalizedFiles.push_back(normalized);
            }
        }
        if (std::find(normalizedFiles.begin(), normalizedFiles.end(), normalizedIni) ==
            normalizedFiles.end()) {
            normalizedFiles.push_back(normalizedIni);
        }
        const std::string payload = serializeStringPayload(normalizedFiles);
        const std::string root = workspaceRoot.generic_string();
        auto& callbacks = packageCommitCallbacks();
        const std::uint32_t requestId = nextRequestId(callbacks);
        callbacks.emplace(requestId, std::move(callback));
        if (neo_browser_request_package_commit_js(
                requestId, sessionId, root.c_str(), normalizedIni.c_str(), payload.c_str()) == 0) {
            invokePackageCommitCallback(
                requestId, 0u, 0u, false,
                "The browser package-commit bridge is unavailable.");
        }
    } catch (const std::exception& exception) {
        callback(PackageCommitResult{0u, 0u, false, exception.what()});
    }
#else
    (void)sessionId;
    (void)workspaceRoot;
    (void)relativeIniPath;
    (void)relativeFiles;
    callback(PackageCommitResult{
        0u, 0u, false, "Browser package commits are unavailable in this build."});
#endif
}

void setDarkMode(bool enabled) {
#if defined(__EMSCRIPTEN__)
    neo_browser_set_dark_mode_js(enabled ? 1 : 0);
#else
    (void)enabled;
#endif
}

} // namespace neobrowser
