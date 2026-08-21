#include "NeoBrowserFiles.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
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
        return stringToNewUTF8(paths.join('\n'));
    } catch (error) {
        console.error('[NeoTools] Browser file picker failed:', error);
        return 0;
    }
});

EM_JS(char*, neo_browser_choose_save_file_js,
      (const char* title, const char* defaultFile), {
    try {
        if (!Module.neoToolsBrowserFiles || !Module.neoToolsBrowserFiles.chooseSaveFile) {
            console.error('[NeoTools] Browser save bridge is unavailable.');
            return 0;
        }
        const path = Module.neoToolsBrowserFiles.chooseSaveFile({
            title: UTF8ToString(title),
            defaultFile: UTF8ToString(defaultFile)
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
using DownloadCallbackMap = std::unordered_map<std::uint32_t, neobrowser::DownloadCallback>;

OpenCallbackMap& openFileCallbacks() {
    static OpenCallbackMap callbacks;
    return callbacks;
}

DownloadCallbackMap& downloadCallbacks() {
    static DownloadCallbackMap callbacks;
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

std::string takeAllocatedString(char* value) {
    if (value == nullptr) return {};
    std::string result(value);
    std::free(value);
    return result;
}

void runOpenFilesCallback(neobrowser::OpenFilesCallback callback,
                          neobrowser::OpenFilesResult result) noexcept {
    try {
        callback(std::move(result));
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[NeoTools] Browser file completion failed: %s\n", exception.what());
    } catch (...) {
        std::fprintf(stderr, "[NeoTools] Browser file completion failed with an unknown exception.\n");
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

void scheduleOpenFilesCallback(neobrowser::OpenFilesCallback callback,
                               neobrowser::OpenFilesResult result) noexcept {
    if (wxTheApp != nullptr) {
        wxTheApp->CallAfter(
            [callback = std::move(callback), result = std::move(result)]() mutable {
                runOpenFilesCallback(std::move(callback), std::move(result));
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

void invokeOpenFilesCallback(std::uint32_t requestId,
                             std::vector<std::filesystem::path> paths,
                             std::string error) noexcept {
    auto& callbacks = openFileCallbacks();
    const auto found = callbacks.find(requestId);
    if (found == callbacks.end()) return;

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

#endif

} // namespace

#if defined(__EMSCRIPTEN__)

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

#endif

namespace neobrowser {

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
                                                    const std::string& defaultFile) {
#if defined(__EMSCRIPTEN__)
    const std::string path = takeAllocatedString(
        neo_browser_choose_save_file_js(title.c_str(), defaultFile.c_str()));
    if (path.empty()) return std::nullopt;
    return std::filesystem::path(path);
#else
    (void)title;
    (void)defaultFile;
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

void setDarkMode(bool enabled) {
#if defined(__EMSCRIPTEN__)
    neo_browser_set_dark_mode_js(enabled ? 1 : 0);
#else
    (void)enabled;
#endif
}

} // namespace neobrowser
