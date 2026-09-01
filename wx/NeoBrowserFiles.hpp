#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neobrowser {

inline constexpr unsigned kBrowserFileApiVersion = 10u;

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

// Releases one or more legacy MEMFS imports returned by requestOpenFiles() or
// chooseOpenFiles(). A path identifies its complete picker session, so all
// files imported by that selection are removed together. Applications should
// call this as soon as those virtual files are no longer needed.
void releaseImportedFiles(const std::vector<std::filesystem::path>& paths) noexcept;

// Move-only ownership wrapper for legacy MEMFS imports. Existing callers may
// continue to use OpenFilesResult, but new code should retain this lease for as
// long as it needs the imported paths. Destruction releases the complete picker
// session, including every file selected in that operation.
class BrowserImportLease {
public:
    BrowserImportLease() = default;
    explicit BrowserImportLease(std::vector<std::filesystem::path> paths);
    ~BrowserImportLease() noexcept;

    BrowserImportLease(const BrowserImportLease&) = delete;
    BrowserImportLease& operator=(const BrowserImportLease&) = delete;
    BrowserImportLease(BrowserImportLease&& other) noexcept;
    BrowserImportLease& operator=(BrowserImportLease&& other) noexcept;

    const std::vector<std::filesystem::path>& paths() const noexcept { return paths_; }
    bool empty() const noexcept { return paths_.empty(); }

    // Releases the current import, then optionally assumes ownership of a new
    // picker result. detach() transfers the raw paths to legacy code and
    // disables automatic release.
    void reset(std::vector<std::filesystem::path> paths = {});
    std::vector<std::filesystem::path> detach() noexcept;

private:
    std::vector<std::filesystem::path> paths_;
};

struct OwnedOpenFilesResult {
    BrowserImportLease import;
    std::string error;

    bool cancelled() const noexcept { return import.empty() && error.empty(); }
};

using OwnedOpenFilesCallback = std::function<void(OwnedOpenFilesResult)>;

// Ownership-safe variant of requestOpenFiles(). The returned import is released
// automatically unless the callback moves it into longer-lived document state.
void requestOpenFilesOwned(const std::string& title,
                           const std::string& accept,
                           bool multiple,
                           OwnedOpenFilesCallback callback);

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

// Retained browser-file sessions provide random access to host File objects
// without copying complete files into MEMFS. This is intended for large archive
// browsers such as NeoBIF. Paths are portable, relative paths supplied by the
// browser; file contents remain owned by the browser for the session lifetime.
struct RetainedFileInfo {
    std::uint32_t fileId = 0;
    std::string relativePath;
    std::uint64_t size = 0;
};

struct RetainedFileSetResult {
    std::uint32_t sessionId = 0;
    std::string displayName;
    std::vector<RetainedFileInfo> files;
    std::string error;

    bool cancelled() const noexcept {
        return sessionId == 0 && files.empty() && error.empty();
    }
};

using RetainedFileSetCallback = std::function<void(RetainedFileSetResult)>;

// Selects ordinary files and retains their browser File objects. No selected
// file is copied into Emscripten's virtual filesystem.
void requestRetainedFiles(const std::string& title,
                          const std::string& accept,
                          bool multiple,
                          RetainedFileSetCallback callback);

// Selects a directory recursively. showDirectoryPicker is used where available;
// webkitdirectory is used as a read-only fallback. Only files matching accept
// are retained when an accept list is supplied.
void requestRetainedDirectory(const std::string& title,
                              const std::string& accept,
                              RetainedFileSetCallback callback);

// Invalidates a retained session and any in-flight reads/exports using it.
void releaseRetainedFileSet(std::uint32_t sessionId);

// Drops browser File objects that are no longer part of the active archive
// model while preserving the listed file IDs in the session.
void retainOnlyRetainedFiles(std::uint32_t sessionId,
                             const std::vector<std::uint32_t>& fileIds);

// Reads one bounded range from a retained browser file. This uses Asyncify and
// must be called from the completion callback of requestRetainedFiles() or
// requestRetainedDirectory(), whose JavaScript entry is invoked with
// ccall(..., {async:true}). It is intended for small headers and index tables,
// not whole large archives.
bool readRetainedFileRange(std::uint32_t sessionId,
                           std::uint32_t fileId,
                           std::uint64_t offset,
                           std::size_t length,
                           std::vector<std::uint8_t>& bytes,
                           std::string& error);

enum class RetainedExportMode {
    DirectDownload = 0,
    ZipDownload = 1,
    Directory = 2,
};

struct RetainedExportEntry {
    std::uint32_t sessionId = 0;
    std::uint32_t fileId = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::string outputPath;
};

struct RetainedExportResult {
    DownloadDisposition disposition{DownloadDisposition::Cancelled};
    std::size_t filesWritten = 0;
    std::uint64_t bytesWritten = 0;
    bool usedDirectory = false;
    std::string error;

    bool cancelled() const noexcept {
        return disposition == DownloadDisposition::Cancelled && error.empty();
    }
};

using RetainedExportCallback = std::function<void(RetainedExportResult)>;

// Exports byte ranges directly from retained browser files. DirectDownload
// creates one Blob slice, ZipDownload writes a store-only ZIP incrementally,
// and Directory streams each range into a selected writable directory. Large
// source files are never materialized in MEMFS.
void requestExportRetainedFiles(RetainedExportMode mode,
                                const std::string& defaultName,
                                const std::vector<RetainedExportEntry>& entries,
                                RetainedExportCallback callback);

// True when the browser can grant a writable directory handle. Read-only
// directory scanning remains available through webkitdirectory when false.
bool retainedDirectoryWriteSupported();



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
