#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neobrowser {

inline constexpr unsigned kBrowserFileApiVersion = 5u;

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

// Creates a unique process-local path suitable for preparing a browser
// download. This does not prompt the user and does not start a download.
// The returned leaf name is sanitized for browser download use.
std::filesystem::path createDownloadPath(const std::string& downloadName);

enum class DownloadDisposition {
    Cancelled = 0,
    Saved = 1,
    Ready = 2,
};

struct DownloadResult {
    DownloadDisposition disposition{DownloadDisposition::Cancelled};
    std::string error;

    bool cancelled() const noexcept {
        return disposition == DownloadDisposition::Cancelled && error.empty();
    }
    bool saved() const noexcept {
        return disposition == DownloadDisposition::Saved && error.empty();
    }
    bool ready() const noexcept {
        return disposition == DownloadDisposition::Ready && error.empty();
    }
};

using DownloadCallback = std::function<void(DownloadResult)>;

// Prepares a persistent, real browser download action and returns immediately.
// The user completes the transfer with a normal DOM link click; no native save
// picker or asynchronous wait is entered from the wxWidgets event dispatch.
// callback runs later through wx's event queue once the link has been prepared.
void requestDownloadFile(const std::filesystem::path& virtualPath,
                         const std::string& downloadName,
                         DownloadCallback callback);

// Copies bytes out of WebAssembly memory and presents a prominent browser
// download action. This is the preferred path for generated or extracted data:
// it avoids Emscripten filesystem durability/atomic-replace operations entirely.
bool prepareDownloadBytes(const void* bytes,
                          std::size_t byteCount,
                          const std::string& downloadName);

// Legacy immediate facade retained for callers not yet migrated to the
// completion API. It prepares the same prominent, persistent browser download
// action without reporting completion.
bool downloadFile(const std::filesystem::path& virtualPath,
                  const std::string& downloadName = {});

// Synchronizes the browser-native wxWidgets controls with the application's
// theme. The pinned wxWidgets-WASM DOM port scopes its dark CSS under
// html.dark, so C++ colour changes alone are insufficient.
void setDarkMode(bool enabled);

} // namespace neobrowser
