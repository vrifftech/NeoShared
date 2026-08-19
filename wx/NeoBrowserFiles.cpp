#include "NeoBrowserFiles.hpp"

#include <cstdint>
#include <exception>
#include <cstdio>
#include <cstdlib>
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

#if defined(__EMSCRIPTEN__)

using CallbackMap = std::unordered_map<std::uint32_t, neobrowser::OpenFilesCallback>;

CallbackMap& openFileCallbacks() {
    static CallbackMap callbacks;
    return callbacks;
}

std::uint32_t nextOpenFileRequestId() {
    static std::uint32_t next = 1;
    auto& callbacks = openFileCallbacks();
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

void invokeOpenFilesCallback(std::uint32_t requestId,
                             std::vector<std::filesystem::path> paths,
                             std::string error) noexcept {
    auto& callbacks = openFileCallbacks();
    const auto found = callbacks.find(requestId);
    if (found == callbacks.end()) return;

    neobrowser::OpenFilesCallback callback = std::move(found->second);
    callbacks.erase(found);
    neobrowser::OpenFilesResult result{std::move(paths), std::move(error)};

    // Return from the JavaScript completion ccall before running application
    // code. The callback may parse a large resource, update wx controls, or
    // open a modal error dialog; all of those belong in wx's normal pending-
    // event pump, not inside the bridge ccall itself.
    if (wxTheApp != nullptr) {
        wxTheApp->CallAfter(
            [callback = std::move(callback), result = std::move(result)]() mutable {
                runOpenFilesCallback(std::move(callback), std::move(result));
            });
        return;
    }
    runOpenFilesCallback(std::move(callback), std::move(result));
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

#endif

namespace neobrowser {

void requestOpenFiles(const std::string& title,
                      const std::string& accept,
                      bool multiple,
                      OpenFilesCallback callback) {
    if (!callback) return;
#if defined(__EMSCRIPTEN__)
    const std::uint32_t requestId = nextOpenFileRequestId();
    openFileCallbacks().emplace(requestId, std::move(callback));
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
