#pragma once

#include <wx/config.h>
#include <wx/menu.h>
#include <wx/string.h>
#include <wx/toplevel.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace neosettings {

constexpr int kMaxRecentFiles = 10;
constexpr const char* kSharedConfigName = "NeoTools";

inline wxString toWx(const std::string& text) {
    return wxString::FromUTF8(text.c_str());
}

inline std::string toStd(const wxString& text) {
    const wxScopedCharBuffer buffer = text.ToUTF8();
    return buffer ? std::string(buffer.data()) : std::string();
}

// std::filesystem::path::u8string() returns std::string in C++17 and
// std::u8string in C++20. Keep all shared path boundaries explicitly UTF-8 so
// consumers compile correctly regardless of the language mode selected by a
// newer compiler or a parent project.
inline std::string pathToUtf8(const std::filesystem::path& path) {
#if defined(__cpp_lib_char8_t)
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
#else
    return path.u8string();
#endif
}

inline std::filesystem::path pathFromUtf8(const std::string& text) {
    if (text.empty()) return {};
    return std::filesystem::u8path(text.begin(), text.end());
}

inline wxString pathToWx(const std::filesystem::path& path) {
#if defined(_WIN32)
    return wxString(path.native().c_str());
#else
    return toWx(pathToUtf8(path));
#endif
}

inline std::filesystem::path pathFromWx(const wxString& text) {
#if defined(_WIN32)
    return std::filesystem::path(text.ToStdWstring());
#else
    return pathFromUtf8(toStd(text));
#endif
}

inline std::string trimSlashes(std::string key) {
    while (!key.empty() && (key.front() == '/' || key.front() == '\\')) key.erase(key.begin());
    while (!key.empty() && (key.back() == '/' || key.back() == '\\')) key.pop_back();
    return key;
}

inline std::filesystem::path normalizedPath(const std::filesystem::path& input) {
    if (input.empty()) return {};
    std::error_code ec;
    std::filesystem::path path = input;
    if (std::filesystem::exists(path, ec)) {
        const auto canonical = std::filesystem::weakly_canonical(path, ec);
        if (!ec && !canonical.empty()) return canonical;
    }
    ec.clear();
    const auto absolute = std::filesystem::absolute(path, ec);
    if (!ec && !absolute.empty()) path = absolute;
    return path.lexically_normal();
}

inline bool samePathForMru(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    const std::string a = pathToUtf8(normalizedPath(lhs));
    const std::string b = pathToUtf8(normalizedPath(rhs));
#if defined(_WIN32)
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
#else
    return a == b;
#endif
}

class ConfigStore {
public:
    explicit ConfigStore(std::string configName) : configName_(std::move(configName)) {}

    const std::string& configName() const noexcept { return configName_; }

    bool hasEntry(const std::string& key) const {
        wxConfig config(toWx(configName_));
        return config.HasEntry(toWx(trimSlashes(key)));
    }

    bool readBool(const std::string& key, bool fallback = false,
                  const std::vector<std::string>& legacyKeys = {}) const {
        wxConfig config(toWx(configName_));
        bool value = fallback;
        const std::string canonical = trimSlashes(key);
        if (config.Read(toWx(canonical), &value)) {
            return value;
        }
        for (const auto& legacy : legacyKeys) {
            if (config.Read(toWx(trimSlashes(legacy)), &value)) {
                config.Write(toWx(canonical), value);
                config.Flush();
                return value;
            }
        }
        return fallback;
    }

    void writeBool(const std::string& key, bool value) const {
        wxConfig config(toWx(configName_));
        config.Write(toWx(trimSlashes(key)), value);
        config.Flush();
    }

    std::optional<std::string> readString(const std::string& key,
                                          const std::vector<std::string>& legacyKeys = {}) const {
        wxConfig config(toWx(configName_));
        wxString value;
        const std::string canonical = trimSlashes(key);
        if (config.Read(toWx(canonical), &value) && !value.empty()) {
            return toStd(value);
        }
        for (const auto& legacy : legacyKeys) {
            if (config.Read(toWx(trimSlashes(legacy)), &value) && !value.empty()) {
                config.Write(toWx(canonical), value);
                config.Flush();
                return toStd(value);
            }
        }
        return std::nullopt;
    }

    std::string readString(const std::string& key, const std::string& fallback,
                           const std::vector<std::string>& legacyKeys = {}) const {
        const auto value = readString(key, legacyKeys);
        return value ? *value : fallback;
    }

    void writeString(const std::string& key, const std::string& value) const {
        wxConfig config(toWx(configName_));
        config.Write(toWx(trimSlashes(key)), toWx(value));
        config.Flush();
    }

    double readDouble(const std::string& key, double fallback = 0.0,
                      const std::vector<std::string>& legacyKeys = {}) const {
        auto parse = [](const std::string& text) -> std::optional<double> {
            try {
                std::size_t consumed = 0;
                const double value = std::stod(text, &consumed);
                if (consumed == 0 || !std::isfinite(value)) return std::nullopt;
                return value;
            } catch (...) {
                return std::nullopt;
            }
        };
        if (const auto value = readString(key, legacyKeys)) {
            if (const auto parsed = parse(*value)) return *parsed;
        }
        return fallback;
    }

    void writeDouble(const std::string& key, double value) const {
        std::ostringstream out;
        out << value;
        writeString(key, out.str());
    }

    std::optional<std::filesystem::path> readPath(const std::string& key,
                                                  const std::vector<std::string>& legacyKeys = {}) const {
        const auto value = readString(key, legacyKeys);
        if (!value || value->empty()) return std::nullopt;
        return pathFromUtf8(*value);
    }

    void writePath(const std::string& key, const std::filesystem::path& path) const {
        writeString(key, path.empty() ? std::string{} : pathToUtf8(path));
    }

    void deleteEntry(const std::string& key) const {
        wxConfig config(toWx(configName_));
        config.DeleteEntry(toWx(trimSlashes(key)), false);
        config.Flush();
    }

    void deleteGroup(const std::string& key) const {
        wxConfig config(toWx(configName_));
        config.DeleteGroup(toWx(trimSlashes(key)));
        config.Flush();
    }

protected:
    std::string configName_;
};

class AppSettings final : public ConfigStore {
public:
    explicit AppSettings(std::string appName) : ConfigStore(std::move(appName)) {}

    bool darkMode() const {
        return readBool("UI/DarkMode", false, {"DarkMode", "dark_mode"});
    }

    void setDarkMode(bool enabled) const {
        writeBool("UI/DarkMode", enabled);
    }

    double fontScale() const {
        double scale = readDouble("UI/FontScale", 1.0, {"FontScale"});
        if (!std::isfinite(scale)) scale = 1.0;
        return std::clamp(scale, 0.75, 2.0);
    }

    void setFontScale(double scale) const {
        if (!std::isfinite(scale)) scale = 1.0;
        writeDouble("UI/FontScale", std::clamp(scale, 0.75, 2.0));
    }

    std::optional<std::filesystem::path> lastTlkPath() const {
        return readPath("Paths/LastTLKPath", {"LastTLKPath"});
    }

    void setLastTlkPath(const std::filesystem::path& path) const {
        writePath("Paths/LastTLKPath", path);
    }

    void clearLastTlkPath() const {
        deleteEntry("Paths/LastTLKPath");
        deleteEntry("LastTLKPath");
    }

    std::string preferredView(const std::string& fallback = {}) const {
        return readString("View/PreferredMode", fallback, {"PreferredView"});
    }

    void setPreferredView(const std::string& value) const {
        writeString("View/PreferredMode", value);
    }

    // Named path lists are for app-specific MRUs that are not "recent files",
    // such as model roots, import folders, or output directories. Keeping the
    // storage format here avoids each GUI inventing its own delimiter and path
    // comparison rules.
    std::vector<std::filesystem::path> recentPaths(
        const std::string& name,
        std::size_t maxItems = kMaxRecentFiles,
        const std::vector<std::string>& legacyDelimitedKeys = {}) const {
        std::vector<std::filesystem::path> paths;
        auto addUnique = [&](const std::filesystem::path& path) {
            if (path.empty()) return;
            const auto normalized = normalizedPath(path);
            if (std::find_if(paths.begin(), paths.end(), [&](const auto& existing) {
                    return samePathForMru(existing, normalized);
                }) == paths.end()) {
                paths.push_back(normalized);
            }
        };

        const std::string group = "MRU/Paths/" + trimSlashes(name);
        wxConfig config(toWx(configName_));
        wxString value;
        for (std::size_t i = 0; i < maxItems; ++i) {
            if (config.Read(toWx(group + "/" + std::to_string(i)), &value) && !value.empty()) {
                addUnique(pathFromWx(value));
            }
        }

        bool foundLegacy = false;
        for (const auto& legacyKey : legacyDelimitedKeys) {
            if (!config.Read(toWx(trimSlashes(legacyKey)), &value) || value.empty()) continue;
            foundLegacy = true;
            std::string delimited = toStd(value);
            std::size_t begin = 0;
            while (begin <= delimited.size()) {
                const std::size_t end = delimited.find('|', begin);
                const std::string item = delimited.substr(
                    begin, end == std::string::npos ? std::string::npos : end - begin);
                if (!item.empty()) addUnique(pathFromUtf8(item));
                if (end == std::string::npos) break;
                begin = end + 1;
            }
        }

        if (paths.size() > maxItems) paths.resize(maxItems);
        if (foundLegacy) setRecentPaths(name, paths, maxItems);
        return paths;
    }

    void setRecentPaths(const std::string& name,
                        const std::vector<std::filesystem::path>& paths,
                        std::size_t maxItems = kMaxRecentFiles) const {
        const std::string group = "MRU/Paths/" + trimSlashes(name);
        wxConfig config(toWx(configName_));
        config.DeleteGroup(toWx(group));

        std::vector<std::filesystem::path> writtenPaths;
        for (const auto& path : paths) {
            if (path.empty()) continue;
            const auto normalized = normalizedPath(path);
            if (std::find_if(writtenPaths.begin(), writtenPaths.end(), [&](const auto& existing) {
                    return samePathForMru(existing, normalized);
                }) != writtenPaths.end()) {
                continue;
            }
            config.Write(toWx(group + "/" + std::to_string(writtenPaths.size())),
                         pathToWx(normalized));
            writtenPaths.push_back(normalized);
            if (writtenPaths.size() >= maxItems) break;
        }
        config.Write(toWx(group + "/Count"), static_cast<long>(writtenPaths.size()));
        config.Flush();
    }

    void addRecentPath(const std::string& name,
                       const std::filesystem::path& path,
                       std::size_t maxItems = kMaxRecentFiles) const {
        if (path.empty()) return;
        auto paths = recentPaths(name, maxItems);
        const auto normalized = normalizedPath(path);
        paths.erase(std::remove_if(paths.begin(), paths.end(), [&](const auto& existing) {
            return samePathForMru(existing, normalized);
        }), paths.end());
        paths.insert(paths.begin(), normalized);
        if (paths.size() > maxItems) paths.resize(maxItems);
        setRecentPaths(name, paths, maxItems);
    }

    void removeRecentPath(const std::string& name,
                          const std::filesystem::path& path,
                          std::size_t maxItems = kMaxRecentFiles) const {
        auto paths = recentPaths(name, maxItems);
        paths.erase(std::remove_if(paths.begin(), paths.end(), [&](const auto& existing) {
            return samePathForMru(existing, path);
        }), paths.end());
        setRecentPaths(name, paths, maxItems);
    }

    void clearRecentPaths(const std::string& name) const {
        deleteGroup("MRU/Paths/" + trimSlashes(name));
    }

    std::vector<std::filesystem::path> recentFiles(std::size_t maxItems = kMaxRecentFiles) const {
        std::vector<std::filesystem::path> files;
        auto addUnique = [&](const std::filesystem::path& path) {
            if (path.empty()) return;
            const auto normalized = normalizedPath(path);
            if (std::find_if(files.begin(), files.end(), [&](const auto& existing) { return samePathForMru(existing, normalized); }) == files.end()) {
                files.push_back(normalized);
            }
        };

        wxConfig config(toWx(configName_));
        wxString value;
        bool foundLegacy = false;
        for (std::size_t i = 0; i < maxItems; ++i) {
            const std::string key = "MRU/Files/" + std::to_string(i);
            if (config.Read(toWx(key), &value) && !value.empty()) addUnique(pathFromWx(value));
        }

        // Legacy locations used by older experimental builds and K-GFF-style INI code.
        for (std::size_t i = 0; i < maxItems; ++i) {
            const std::string index = std::to_string(i);
            for (const auto& key : {std::string("RecentFile") + index,
                                    std::string("RecentFiles/") + index,
                                    std::string("MRU/") + index}) {
                if (config.Read(toWx(key), &value) && !value.empty()) {
                    foundLegacy = true;
                    addUnique(pathFromWx(value));
                }
            }
        }

        if (files.size() > maxItems) files.resize(maxItems);
        if (foundLegacy) setRecentFiles(files, maxItems);
        return files;
    }

    void setRecentFiles(const std::vector<std::filesystem::path>& files,
                        std::size_t maxItems = kMaxRecentFiles) const {
        wxConfig config(toWx(configName_));
        config.DeleteGroup("MRU/Files");
        std::size_t written = 0;
        for (const auto& file : files) {
            if (file.empty()) continue;
            config.Write(toWx("MRU/Files/" + std::to_string(written)), pathToWx(normalizedPath(file)));
            ++written;
            if (written >= maxItems) break;
        }
        config.Write("MRU/FileCount", static_cast<long>(written));
        config.Flush();
    }

    void addRecentFile(const std::filesystem::path& path,
                       std::size_t maxItems = kMaxRecentFiles) const {
        if (path.empty()) return;
        std::vector<std::filesystem::path> files = recentFiles(maxItems);
        const auto normalized = normalizedPath(path);
        files.erase(std::remove_if(files.begin(), files.end(), [&](const auto& existing) {
            return samePathForMru(existing, normalized);
        }), files.end());
        files.insert(files.begin(), normalized);
        if (files.size() > maxItems) files.resize(maxItems);
        setRecentFiles(files, maxItems);
    }

    void removeRecentFile(const std::filesystem::path& path,
                          std::size_t maxItems = kMaxRecentFiles) const {
        std::vector<std::filesystem::path> files = recentFiles(maxItems);
        files.erase(std::remove_if(files.begin(), files.end(), [&](const auto& existing) {
            return samePathForMru(existing, path);
        }), files.end());
        setRecentFiles(files, maxItems);
    }

    void clearRecentFiles() const {
        wxConfig config(toWx(configName_));
        config.DeleteGroup("MRU");
        for (std::size_t i = 0; i < kMaxRecentFiles; ++i) {
            const std::string index = std::to_string(i);
            config.DeleteEntry(toWx(std::string("RecentFile") + index), false);
            config.DeleteEntry(toWx(std::string("RecentFiles/") + index), false);
        }
        config.Flush();
    }

    void saveWindowPlacement(const wxTopLevelWindow& window, const std::string& name = "Main") const {
        if (window.IsIconized()) return;
        wxConfig config(toWx(configName_));
        const std::string base = "Windows/" + trimSlashes(name) + "/";
        config.Write(toWx(base + "X"), static_cast<long>(window.GetPosition().x));
        config.Write(toWx(base + "Y"), static_cast<long>(window.GetPosition().y));
        config.Write(toWx(base + "W"), static_cast<long>(window.GetSize().GetWidth()));
        config.Write(toWx(base + "H"), static_cast<long>(window.GetSize().GetHeight()));
        config.Write(toWx(base + "Maximized"), window.IsMaximized());
        config.Flush();
    }

    bool restoreWindowPlacement(wxTopLevelWindow& window, const std::string& name = "Main") const {
        wxConfig config(toWx(configName_));
        const std::string base = "Windows/" + trimSlashes(name) + "/";
        long x = 0, y = 0, w = 0, h = 0;
        if (!config.Read(toWx(base + "W"), &w) || !config.Read(toWx(base + "H"), &h) || w < 320 || h < 240) {
            return false;
        }
        config.Read(toWx(base + "X"), &x, static_cast<long>(window.GetPosition().x));
        config.Read(toWx(base + "Y"), &y, static_cast<long>(window.GetPosition().y));
        window.SetSize(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h));
        bool maximized = false;
        if (config.Read(toWx(base + "Maximized"), &maximized) && maximized) {
            window.Maximize(true);
        }
        return true;
    }
};

class SharedSettings final : public ConfigStore {
public:
    SharedSettings() : ConfigStore(kSharedConfigName) {}
};

inline std::string escapeMenuLabel(std::string text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (char ch : text) {
        escaped.push_back(ch);
        if (ch == '&') escaped.push_back('&');
    }
    return escaped;
}

inline std::string ellipsizeMiddle(const std::string& text, std::size_t maxChars) {
    if (text.size() <= maxChars) return text;
    if (maxChars <= 5) return text.substr(0, maxChars);
    const std::size_t head = (maxChars - 3) / 2;
    const std::size_t tail = maxChars - 3 - head;
    return text.substr(0, head) + "..." + text.substr(text.size() - tail);
}

inline void populateRecentFilesMenu(wxMenu& menu, const AppSettings& settings,
                                    int firstRecentId, int clearRecentId,
                                    std::size_t maxItems = kMaxRecentFiles) {
    while (menu.GetMenuItemCount() > 0) {
        wxMenuItem* item = menu.FindItemByPosition(0);
        if (item == nullptr) break;
        menu.Delete(item);
    }

    const auto files = settings.recentFiles(maxItems);
    if (files.empty()) {
        wxMenuItem* none = menu.Append(wxID_ANY, "(No recent files)");
        none->Enable(false);
    } else {
        for (std::size_t i = 0; i < files.size(); ++i) {
            const std::string label = "&" + std::to_string(i + 1) + " " +
                                      ellipsizeMiddle(pathToUtf8(files[i]), 72);
            wxMenuItem* item = menu.Append(firstRecentId + static_cast<int>(i), toWx(escapeMenuLabel(label)));
            item->SetHelp(pathToWx(files[i]));
        }
    }
    menu.AppendSeparator();
    wxMenuItem* clear = menu.Append(clearRecentId, "Clear Recent Files");
    clear->Enable(!files.empty());
}

} // namespace neosettings
