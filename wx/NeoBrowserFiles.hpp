#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neobrowser {

inline constexpr unsigned kBrowserFileApiVersion = 7u;

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
                                                    const std::string& defaultFile,
                                                    const std::string& defaultExtension = {});

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

// Browser package-directory support used by patcher Write-to-INI workflows.
// The directory handle remains in JavaScript; C++ receives an opaque session
// ID and works against a process-local mirror before committing the verified
// files back to the user-selected installer folder.
bool packageDirectoryAccessSupported();

struct PackageDirectoryResult {
    std::uint32_t sessionId = 0;
    std::string displayName;
    std::vector<std::string> iniPaths;
    std::string error;

    bool cancelled() const noexcept {
        return sessionId == 0 && error.empty();
    }
};

using PackageDirectoryCallback = std::function<void(PackageDirectoryResult)>;

// Starts a browser directory picker in read/write mode and returns immediately.
// Existing INIs are returned as package-relative paths so multiple files named
// changes.ini remain distinguishable.
void requestPackageDirectory(PackageDirectoryCallback callback);

// Normalizes and validates a portable path relative to the selected installer
// directory. Backslashes are accepted and normalized to '/'. Absolute paths,
// empty components, '.'/'..', control characters, and (when requireIni is
// true) non-INI leaves are rejected.
std::string normalizePackageRelativePath(std::string path,
                                         bool requireIni = false);

struct PackageWorkspaceResult {
    std::uint32_t sessionId = 0;
    std::filesystem::path workspaceRoot;
    std::filesystem::path iniPath;
    std::string relativeIniPath;
    bool iniExisted = false;
    std::string error;
};

using PackageWorkspaceCallback = std::function<void(PackageWorkspaceResult)>;

// Mirrors the selected INI and the named package-relative files into a fresh
// Emscripten workspace. The callback runs later through wx's event queue.
void requestPackageWorkspace(std::uint32_t sessionId,
                             const std::string& relativeIniPath,
                             const std::vector<std::string>& relativeFiles,
                             PackageWorkspaceCallback callback);

struct PackageCommitResult {
    std::size_t filesWritten = 0;
    std::size_t filesReused = 0;
    bool iniChanged = false;
    std::string error;
};

using PackageCommitCallback = std::function<void(PackageCommitResult)>;

// Commits verified workspace outputs back to the selected installer folder.
// Existing host files are rechecked against the imported baseline first.
// Payloads are committed before the INI, and same-content files are retained.
void requestCommitPackageWorkspace(std::uint32_t sessionId,
                                   const std::filesystem::path& workspaceRoot,
                                   const std::string& relativeIniPath,
                                   const std::vector<std::string>& relativeFiles,
                                   PackageCommitCallback callback);

// Synchronizes the browser-native wxWidgets controls with the application's
// theme. The pinned wxWidgets-WASM DOM port scopes its dark CSS under
// html.dark, so C++ colour changes alone are insufficient.
void setDarkMode(bool enabled);

} // namespace neobrowser
