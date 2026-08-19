#include "NeoBrowserFiles.hpp"

#include <cstdlib>
#include <system_error>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#if defined(__EMSCRIPTEN__)

EM_JS_DEPS(neo_browser_file_deps, "$UTF8ToString,$stringToNewUTF8");

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

#endif

namespace {

#if defined(__EMSCRIPTEN__)

std::string takeAllocatedString(char* value) {
    if (value == nullptr) return {};
    std::string result(value);
    std::free(value);
    return result;
}

#endif

} // namespace

namespace neobrowser {

std::vector<std::filesystem::path> chooseOpenFiles(const std::string& title,
                                                   const std::string& accept,
                                                   bool multiple) {
#if defined(__EMSCRIPTEN__)
    const std::string payload = takeAllocatedString(
        neo_browser_choose_open_files_js(title.c_str(), accept.c_str(), multiple ? 1 : 0));
    if (payload.empty()) return {};

    std::vector<std::filesystem::path> paths;
    std::size_t begin = 0;
    while (begin <= payload.size()) {
        const std::size_t end = payload.find('\n', begin);
        const std::string item = payload.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!item.empty()) paths.emplace_back(item);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return paths;
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

} // namespace neobrowser
