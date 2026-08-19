#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neobrowser {

inline constexpr unsigned kBrowserFileApiVersion = 2u;

struct OpenFilesResult {
    std::vector<std::filesystem::path> paths;
    std::string error;

    bool cancelled() const noexcept { return paths.empty() && error.empty(); }
};

using OpenFilesCallback = std::function<void(OpenFilesResult)>;

// Starts a real browser file picker and returns immediately. Once the browser
// has imported the selected host files into the process-local Emscripten
// filesystem, callback is invoked from a later JavaScript task. This callback
// form is required for wxWidgets-WASM: ordinary DOM menu/control events enter
// WebAssembly through a synchronous ccall and must be allowed to return before
// any asynchronous browser API is awaited.
void requestOpenFiles(const std::string& title,
                      const std::string& accept,
                      bool multiple,
                      OpenFilesCallback callback);

// Legacy synchronous facade retained temporarily for source compatibility.
// Do not call this from a wxWidgets-WASM event handler; use requestOpenFiles().
std::vector<std::filesystem::path> chooseOpenFiles(const std::string& title,
                                                   const std::string& accept,
                                                   bool multiple);

// Requests a download filename and returns a unique process-local path where
// the application can write the result before calling downloadFile().
std::optional<std::filesystem::path> chooseSaveFile(const std::string& title,
                                                    const std::string& defaultFile);

// Downloads an existing process-local file through the browser. When
// downloadName is empty, the virtual file's leaf name is used.
bool downloadFile(const std::filesystem::path& virtualPath,
                  const std::string& downloadName = {});

// Synchronizes the browser-native wxWidgets controls with the application's
// theme. The pinned wxWidgets-WASM DOM port scopes its dark CSS under
// html.dark, so C++ colour changes alone are insufficient.
void setDarkMode(bool enabled);

} // namespace neobrowser
