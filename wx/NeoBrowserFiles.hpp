#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neobrowser {

inline constexpr unsigned kBrowserFileApiVersion = 1u;

// Opens a real browser file picker and imports the selected files into the
// process-local Emscripten filesystem. The returned paths can be consumed by
// existing std::filesystem-based parsers without exposing browser File objects
// throughout the application model.
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

} // namespace neobrowser
