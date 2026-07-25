#pragma once

#include "NeoSettings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neogames {

struct GameDefinition {
    std::string id;
    std::string displayName;
    std::vector<std::string> strongMarkers;
    std::vector<std::string> weakMarkers;
    std::vector<std::string> tlkRelativePaths;
    std::vector<std::string> overrideRelativePaths;
    std::vector<std::string> dataRelativePaths;
    std::vector<std::string> commonDirectoryNames;
    std::vector<std::string> registryValues;
};

struct GameInstall {
    // Stable game identifier, e.g. "kotor2". Kept as id for compatibility with
    // the first shared-resolver implementation.
    std::string id;

    // Stable per-install identifier. A game may have any number of install records.
    std::string installId;

    // User-facing install label, e.g. "K2 Test" or "KOTOR2 Steam". Users may
    // rename this freely without changing installId.
    std::string displayName;

    std::filesystem::path installPath;
    std::filesystem::path tlkPath;
    std::filesystem::path overridePath;
    std::filesystem::path dataRootPath;
    bool detected = false;
    bool userOverride = false;
    int confidence = 0;
    std::string status;
};

inline const std::vector<GameDefinition>& knownGames() {
    static const std::vector<GameDefinition> games = {
        {"kotor", "Star Wars: Knights of the Old Republic",
         {"dialog.tlk", "chitin.key"}, {"Override", "override", "swkotor.exe"},
         {"dialog.tlk"}, {"Override", "override"}, {"data", "Data"},
         {"swkotor", "Knights of the Old Republic", "Star Wars - Knights of the Old Republic", "Star Wars Knights of the Old Republic"},
         {"Software\\BioWare\\SW\\KOTOR/Path", "Software\\WOW6432Node\\BioWare\\SW\\KOTOR/Path"}},
        {"kotor2", "Star Wars: Knights of the Old Republic II",
         {"dialog.tlk", "chitin.key"}, {"Override", "override", "swkotor2.exe"},
         {"dialog.tlk"}, {"Override", "override"}, {"data", "Data"},
         {"Knights of the Old Republic II", "Knights of the Old Republic 2", "swkotor2", "Star Wars Knights of the Old Republic II"},
         {"Software\\Obsidian\\Star Wars KOTOR2/Path", "Software\\WOW6432Node\\Obsidian\\Star Wars KOTOR2/Path"}},
        {"jade", "Jade Empire",
         {"dialog.tlk"}, {"JadeEmpire.exe", "Jade Empire.exe", "data", "Data"},
         {"dialog.tlk"}, {"override", "Override"}, {"data", "Data"},
         {"Jade Empire", "Jade Empire Special Edition"},
         {"Software\\BioWare\\Jade Empire/Path", "Software\\WOW6432Node\\BioWare\\Jade Empire/Path"}},
        {"nwn", "Neverwinter Nights",
         {"dialog.tlk"}, {"nwn.ini", "nwn.exe", "override", "Override"},
         {"dialog.tlk"}, {"override", "Override"}, {"data", "Data", "modules", "Modules"},
         {"Neverwinter Nights", "NeverwinterNights", "NWN"},
         {"Software\\BioWare\\NWN\\Neverwinter/Location", "Software\\WOW6432Node\\BioWare\\NWN\\Neverwinter/Location"}},
        {"nwn2", "Neverwinter Nights 2",
         {"dialog.tlk"}, {"nwn2.exe", "nwn2.ini", "Override", "override"},
         {"dialog.tlk"}, {"Override", "override"}, {"Data", "data", "modules", "Modules"},
         {"Neverwinter Nights 2", "NeverwinterNights2", "NWN2"},
         {"Software\\Obsidian\\NWN 2\\Neverwinter/Location", "Software\\WOW6432Node\\Obsidian\\NWN 2\\Neverwinter/Location"}},
        {"witcher1", "The Witcher",
         {"Data"}, {"System/witcher.exe", "System/witcher", "Data/dialogues"},
         {"Data/dialogues/dialog.tlk", "Data/dialog.tlk", "dialog.tlk"}, {"Override", "override"}, {"Data", "data"},
         {"The Witcher", "The Witcher Enhanced Edition"},
         {"Software\\CD Projekt Red\\The Witcher/InstallFolder", "Software\\WOW6432Node\\CD Projekt Red\\The Witcher/InstallFolder"}},
        {"dao", "Dragon Age: Origins",
         {"packages/core/data"}, {"bin_ship/daorigins.exe", "modules/Single Player", "modules/single player"},
         {"modules/Single Player/data/talktables/dialog.tlk", "modules/single player/data/talktables/dialog.tlk", "packages/core/data/talktables/dialog.tlk", "dialog.tlk"},
         {"packages/core/override", "packages/core/Override", "override", "Override"}, {"packages/core/data", "packages/core/Data"},
         {"Dragon Age Origins", "Dragon Age Ultimate Edition", "Dragon Age"},
         {"Software\\BioWare\\Dragon Age/Path", "Software\\WOW6432Node\\BioWare\\Dragon Age/Path"}},
        {"da2", "Dragon Age II",
         {"packages/core/data"}, {"bin_ship/DragonAge2.exe", "modules/single player", "modules/Single Player"},
         {"modules/single player/data/talktables/core_en-us.tlk", "modules/Single Player/data/talktables/core_en-us.tlk", "packages/core/data/talktables/core_en-us.tlk", "dialog.tlk"},
         {"packages/core/override", "packages/core/Override", "override", "Override"}, {"packages/core/data", "packages/core/Data"},
         {"Dragon Age II", "Dragon Age 2"},
         {"Software\\BioWare\\Dragon Age 2/Path", "Software\\WOW6432Node\\BioWare\\Dragon Age 2/Path"}}
    };
    return games;
}

inline const GameDefinition* findGame(const std::string& id) {
    const auto& games = knownGames();
    const auto it = std::find_if(games.begin(), games.end(), [&](const GameDefinition& game) { return game.id == id; });
    return it == games.end() ? nullptr : &*it;
}

inline bool existsPath(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec);
}

inline bool isDirectoryPath(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_directory(path, ec);
}

inline std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

inline bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    return lowerAscii(haystack).find(lowerAscii(needle)) != std::string::npos;
}

inline std::filesystem::path firstExisting(const std::filesystem::path& root,
                                           const std::vector<std::string>& relatives) {
    for (const auto& rel : relatives) {
        const std::filesystem::path candidate = root / std::filesystem::path(rel);
        if (existsPath(candidate)) return neosettings::normalizedPath(candidate);
    }
    return {};
}

inline int validationScore(const GameDefinition& game, const std::filesystem::path& root) {
    if (!isDirectoryPath(root)) return 0;
    int score = 0;
    for (const auto& marker : game.strongMarkers) {
        if (existsPath(root / std::filesystem::path(marker))) score += 3;
    }
    for (const auto& marker : game.weakMarkers) {
        if (existsPath(root / std::filesystem::path(marker))) score += 1;
    }
    for (const auto& rel : game.tlkRelativePaths) {
        if (existsPath(root / std::filesystem::path(rel))) {
            score += 2;
            break;
        }
    }
    return score;
}

inline std::string confidenceText(int confidence, bool userOverride) {
    if (userOverride) return "user";
    if (confidence >= 7) return "high";
    if (confidence >= 4) return "medium";
    if (confidence >= 2) return "low";
    return "none";
}

inline std::uint64_t fnv1a64(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (char ch : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= 1099511628211ull;
    }
    return hash;
}

inline std::string toHex(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << value;
    return out.str();
}

inline std::string storefrontLabel(const std::filesystem::path& root) {
    const std::string path = root.u8string();
    if (containsCaseInsensitive(path, "steamapps")) return "Steam";
    if (containsCaseInsensitive(path, "gog galaxy") || containsCaseInsensitive(path, "gog games") ||
        containsCaseInsensitive(path, "gog.com")) return "GOG";
    if (containsCaseInsensitive(path, "origin games") || containsCaseInsensitive(path, "ea games")) return "EA";
    return {};
}

inline std::string defaultInstallName(const GameDefinition& game, const std::filesystem::path& root = {}) {
    const std::string source = storefrontLabel(root);
    if (!source.empty()) return game.displayName + " (" + source + ")";
    const std::string leaf = root.empty() ? std::string{} : root.filename().u8string();
    if (!leaf.empty() && leaf != "." && leaf != "..") return game.displayName + " - " + leaf;
    return game.displayName;
}

inline std::string makeInstallId(const GameDefinition& game,
                                 const std::filesystem::path& root,
                                 const std::filesystem::path& tlkPath = {}) {
    const std::string seed = game.id + "|" + neosettings::normalizedPath(root).u8string() + "|" +
                             neosettings::normalizedPath(tlkPath).u8string();
    return std::string("i_") + toHex(fnv1a64(seed));
}

inline std::string installStatus(const GameDefinition& game, const GameInstall& install) {
    const bool hasInstallRoot = !install.installPath.empty() && validationScore(game, install.installPath) > 0;
    const bool hasTlk = existsPath(install.tlkPath);
    if (!hasInstallRoot && !hasTlk && (!install.installPath.empty() || !install.tlkPath.empty())) return "missing";
    return confidenceText(install.confidence, install.userOverride);
}

inline void refreshDerivedPaths(const GameDefinition& game, GameInstall& install) {
    install.id = game.id;
    install.installPath = neosettings::normalizedPath(install.installPath);
    install.tlkPath = neosettings::normalizedPath(install.tlkPath);
    install.overridePath = neosettings::normalizedPath(install.overridePath);
    install.dataRootPath = neosettings::normalizedPath(install.dataRootPath);

    const int score = validationScore(game, install.installPath);
    if (score > install.confidence) install.confidence = score;
    if (install.userOverride) install.confidence = std::max(install.confidence, 8);

    if (!install.installPath.empty()) {
        if (install.tlkPath.empty() || !existsPath(install.tlkPath)) {
            install.tlkPath = firstExisting(install.installPath, game.tlkRelativePaths);
        }
        if (install.overridePath.empty() || !existsPath(install.overridePath)) {
            install.overridePath = firstExisting(install.installPath, game.overrideRelativePaths);
        }
        if (install.dataRootPath.empty() || !existsPath(install.dataRootPath)) {
            install.dataRootPath = firstExisting(install.installPath, game.dataRelativePaths);
        }
    }

    if (install.installId.empty()) install.installId = makeInstallId(game, install.installPath, install.tlkPath);
    if (install.displayName.empty()) install.displayName = defaultInstallName(game, install.installPath);
    install.status = installStatus(game, install);
}

inline GameInstall makeInstall(const GameDefinition& game, const std::filesystem::path& root,
                               bool detected, bool userOverride, int confidence,
                               std::string displayName = {}, std::string installId = {}) {
    GameInstall install;
    install.id = game.id;
    install.installId = std::move(installId);
    install.displayName = std::move(displayName);
    install.installPath = neosettings::normalizedPath(root);
    install.tlkPath = firstExisting(install.installPath, game.tlkRelativePaths);
    install.overridePath = firstExisting(install.installPath, game.overrideRelativePaths);
    install.dataRootPath = firstExisting(install.installPath, game.dataRelativePaths);
    install.detected = detected;
    install.userOverride = userOverride;
    install.confidence = confidence;
    refreshDerivedPaths(game, install);
    return install;
}

inline bool sameInstallTarget(const GameInstall& lhs, const GameInstall& rhs) {
    if (!lhs.installId.empty() && lhs.installId == rhs.installId) return true;
    if (!lhs.installPath.empty() && !rhs.installPath.empty() &&
        neosettings::samePathForMru(lhs.installPath, rhs.installPath)) return true;
    if (!lhs.tlkPath.empty() && !rhs.tlkPath.empty() &&
        neosettings::samePathForMru(lhs.tlkPath, rhs.tlkPath)) return true;
    return false;
}

inline void mergeInstall(GameInstall& existing, const GameInstall& incoming) {
    if (existing.installId.empty()) existing.installId = incoming.installId;
    if (incoming.userOverride || existing.displayName.empty()) {
        if (!incoming.displayName.empty()) existing.displayName = incoming.displayName;
    }
    if (!incoming.installPath.empty()) existing.installPath = incoming.installPath;
    if (!incoming.tlkPath.empty() && (existing.tlkPath.empty() || incoming.userOverride)) existing.tlkPath = incoming.tlkPath;
    if (!incoming.overridePath.empty() && existing.overridePath.empty()) existing.overridePath = incoming.overridePath;
    if (!incoming.dataRootPath.empty() && existing.dataRootPath.empty()) existing.dataRootPath = incoming.dataRootPath;
    existing.detected = existing.detected || incoming.detected;
    existing.userOverride = existing.userOverride || incoming.userOverride;
    existing.confidence = std::max(existing.confidence, incoming.confidence);
    if (existing.status.empty()) existing.status = incoming.status;
}

inline void upsertInstall(std::vector<GameInstall>& installs, const GameDefinition& game, GameInstall incoming) {
    refreshDerivedPaths(game, incoming);
    auto it = std::find_if(installs.begin(), installs.end(), [&](const GameInstall& existing) {
        return sameInstallTarget(existing, incoming);
    });
    if (it == installs.end()) {
        installs.push_back(std::move(incoming));
        return;
    }
    mergeInstall(*it, incoming);
    refreshDerivedPaths(game, *it);
}

inline std::string pathPrefixText(const std::filesystem::path& path) {
    std::string text = neosettings::normalizedPath(path).u8string();
    std::replace(text.begin(), text.end(), '\\', '/');
#if defined(_WIN32)
    text = lowerAscii(text);
#endif
    return text;
}

inline bool pathStartsWith(const std::filesystem::path& child, const std::filesystem::path& root) {
    if (child.empty() || root.empty()) return false;
    std::string childText = pathPrefixText(child);
    std::string rootText = pathPrefixText(root);
    if (childText == rootText) return true;
    if (rootText.empty()) return false;
    if (rootText.back() != '/') rootText.push_back('/');
    if (childText.size() < rootText.size()) return false;
    return childText.compare(0, rootText.size(), rootText) == 0;
}

inline bool installContainsPath(const GameInstall& install, const std::filesystem::path& path) {
    if (!install.installPath.empty() && pathStartsWith(path, install.installPath)) return true;
    if (!install.tlkPath.empty() && neosettings::samePathForMru(path, install.tlkPath)) return true;
    if (!install.overridePath.empty() && pathStartsWith(path, install.overridePath)) return true;
    if (!install.dataRootPath.empty() && pathStartsWith(path, install.dataRootPath)) return true;
    return false;
}

inline std::size_t pathTextLength(const std::filesystem::path& path) {
    return neosettings::normalizedPath(path).u8string().size();
}

inline void sortInstalls(std::vector<GameInstall>& installs, const std::string& activeInstallId = {}) {
    std::stable_sort(installs.begin(), installs.end(), [&](const GameInstall& lhs, const GameInstall& rhs) {
        const bool lhsActive = !activeInstallId.empty() && lhs.installId == activeInstallId;
        const bool rhsActive = !activeInstallId.empty() && rhs.installId == activeInstallId;
        if (lhsActive != rhsActive) return lhsActive;
        if (lhs.userOverride != rhs.userOverride) return lhs.userOverride;
        if (lhs.confidence != rhs.confidence) return lhs.confidence > rhs.confidence;
        return lowerAscii(lhs.displayName) < lowerAscii(rhs.displayName);
    });
}

inline std::string settingBase(const std::string& gameId) {
    return std::string("GamePaths/") + gameId + "/";
}

inline std::string installListBase(const std::string& gameId) {
    return settingBase(gameId) + "Installs/";
}

class GamePathSettings final {
public:
    GamePathSettings() = default;

    std::optional<std::string> activeInstallId(const std::string& gameId) const {
        neosettings::SharedSettings settings;
        return settings.readString(settingBase(gameId) + "ActiveInstallId");
    }

    std::optional<GameInstall> read(const GameDefinition& game) const {
        auto installs = readAll(game);
        if (installs.empty()) return std::nullopt;
        const auto active = activeInstallId(game.id);
        if (active && !active->empty()) {
            const auto it = std::find_if(installs.begin(), installs.end(), [&](const GameInstall& install) {
                return install.installId == *active;
            });
            if (it != installs.end()) return *it;
        }
        return installs.front();
    }

    std::vector<GameInstall> readAll(const GameDefinition& game) const {
        neosettings::SharedSettings settings;
        const std::string base = installListBase(game.id);
        const std::size_t count = parseCount(settings.readString(base + "Count", "0"));
        std::vector<GameInstall> installs;
        installs.reserve(count);

        for (std::size_t i = 0; i < count; ++i) {
            const std::string item = base + std::to_string(i) + "/";
            GameInstall install;
            install.id = game.id;
            install.installId = settings.readString(item + "Id", std::string{});
            install.installPath = settings.readPath(item + "InstallPath").value_or(std::filesystem::path{});
            install.tlkPath = settings.readPath(item + "TLKPath").value_or(std::filesystem::path{});
            install.overridePath = settings.readPath(item + "OverridePath").value_or(std::filesystem::path{});
            install.dataRootPath = settings.readPath(item + "DataRootPath").value_or(std::filesystem::path{});
            install.displayName = settings.readString(item + "Name", defaultInstallName(game, install.installPath));
            install.detected = settings.readBool(item + "Detected", false);
            install.userOverride = settings.readBool(item + "UserOverride", false);
            install.confidence = static_cast<int>(parseCount(settings.readString(item + "ConfidenceScore", "0")));
            refreshDerivedPaths(game, install);
            upsertInstall(installs, game, std::move(install));
        }

        if (installs.empty()) {
            if (auto legacy = readLegacySingle(game)) {
                installs.push_back(*legacy);
                writeAll(game, installs, legacy->installId);
            }
        }

        sortInstalls(installs, activeInstallId(game.id).value_or(std::string{}));
        return installs;
    }

    void write(const GameInstall& install) const {
        const auto* game = findGame(install.id);
        if (game == nullptr) return;
        auto installs = readAll(*game);
        upsertInstall(installs, *game, install);
        std::string active = activeInstallId(install.id).value_or(std::string{});
        if (active.empty()) active = install.installId;
        writeAll(*game, installs, active);
    }

    void writeAll(const GameDefinition& game, std::vector<GameInstall> installs,
                  std::string activeId = {}) const {
        neosettings::SharedSettings settings;
        const std::string root = installListBase(game.id);
        settings.deleteGroup(root);

        std::vector<GameInstall> normalized;
        normalized.reserve(installs.size());
        for (auto& install : installs) {
            refreshDerivedPaths(game, install);
            upsertInstall(normalized, game, install);
        }

        if (activeId.empty()) activeId = settings.readString(settingBase(game.id) + "ActiveInstallId", std::string{});
        if (std::find_if(normalized.begin(), normalized.end(), [&](const GameInstall& install) {
                return install.installId == activeId;
            }) == normalized.end()) {
            activeId = normalized.empty() ? std::string{} : normalized.front().installId;
        }

        settings.writeString(root + "Count", std::to_string(normalized.size()));
        for (std::size_t i = 0; i < normalized.size(); ++i) {
            const GameInstall& install = normalized[i];
            const std::string item = root + std::to_string(i) + "/";
            settings.writeString(item + "Id", install.installId);
            settings.writeString(item + "Name", install.displayName);
            settings.writePath(item + "InstallPath", install.installPath);
            settings.writePath(item + "TLKPath", install.tlkPath);
            settings.writePath(item + "OverridePath", install.overridePath);
            settings.writePath(item + "DataRootPath", install.dataRootPath);
            settings.writeBool(item + "Detected", install.detected);
            settings.writeBool(item + "UserOverride", install.userOverride);
            settings.writeString(item + "ConfidenceScore", std::to_string(install.confidence));
            settings.writeString(item + "Confidence", confidenceText(install.confidence, install.userOverride));
        }

        if (normalized.empty()) {
            clearLegacySingleKeys(game.id);
            settings.deleteEntry(settingBase(game.id) + "ActiveInstallId");
            return;
        }

        settings.writeString(settingBase(game.id) + "ActiveInstallId", activeId);

        // Compatibility: keep the older single-install keys pointing at the active
        // install. The multi-install list is authoritative.
        const auto activeIt = std::find_if(normalized.begin(), normalized.end(), [&](const GameInstall& install) {
            return install.installId == activeId;
        });
        const GameInstall& active = activeIt == normalized.end() ? normalized.front() : *activeIt;
        settings.writeString(settingBase(game.id) + "DisplayName", active.displayName);
        settings.writePath(settingBase(game.id) + "InstallPath", active.installPath);
        settings.writePath(settingBase(game.id) + "TLKPath", active.tlkPath);
        settings.writePath(settingBase(game.id) + "OverridePath", active.overridePath);
        settings.writePath(settingBase(game.id) + "DataRootPath", active.dataRootPath);
        settings.writeBool(settingBase(game.id) + "Detected", active.detected);
        settings.writeBool(settingBase(game.id) + "UserOverride", active.userOverride);
        settings.writeString(settingBase(game.id) + "Confidence", confidenceText(active.confidence, active.userOverride));
    }

    bool renameInstall(const std::string& gameId, const std::string& installId, const std::string& newName) const {
        const auto* game = findGame(gameId);
        if (game == nullptr || installId.empty() || newName.empty()) return false;
        auto installs = readAll(*game);
        bool changed = false;
        for (auto& install : installs) {
            if (install.installId == installId) {
                install.displayName = newName;
                install.userOverride = true;
                changed = true;
                break;
            }
        }
        if (changed) writeAll(*game, installs, activeInstallId(gameId).value_or(std::string{}));
        return changed;
    }

    bool setActiveInstall(const std::string& gameId, const std::string& installId) const {
        const auto* game = findGame(gameId);
        if (game == nullptr || installId.empty()) return false;
        auto installs = readAll(*game);
        const auto it = std::find_if(installs.begin(), installs.end(), [&](const GameInstall& install) {
            return install.installId == installId;
        });
        if (it == installs.end()) return false;
        writeAll(*game, installs, installId);
        return true;
    }

    bool clearInstall(const std::string& gameId, const std::string& installId) const {
        const auto* game = findGame(gameId);
        if (game == nullptr || installId.empty()) return false;
        auto installs = readAll(*game);
        const auto before = installs.size();
        installs.erase(std::remove_if(installs.begin(), installs.end(), [&](const GameInstall& install) {
            return install.installId == installId;
        }), installs.end());
        if (installs.size() == before) return false;
        writeAll(*game, installs);
        return true;
    }

    void clear(const std::string& gameId) const {
        neosettings::SharedSettings settings;
        settings.deleteGroup(settingBase(gameId));
    }

private:
    static std::size_t parseCount(const std::string& text) {
        try {
            return static_cast<std::size_t>(std::stoull(text));
        } catch (...) {
            return 0;
        }
    }

    static void clearLegacySingleKeys(const std::string& gameId) {
        neosettings::SharedSettings settings;
        const std::string base = settingBase(gameId);
        for (const auto& key : {"DisplayName", "InstallPath", "TLKPath", "OverridePath",
                                "DataRootPath", "Detected", "UserOverride", "Confidence"}) {
            settings.deleteEntry(base + key);
        }
    }

    std::optional<GameInstall> readLegacySingle(const GameDefinition& game) const {
        neosettings::SharedSettings settings;
        const std::string base = settingBase(game.id);
        const auto root = settings.readPath(base + "InstallPath");
        const auto savedTlk = settings.readPath(base + "TLKPath");
        if ((!root || root->empty()) && (!savedTlk || savedTlk->empty())) return std::nullopt;

        GameInstall install;
        if (root && !root->empty()) {
            install = makeInstall(game, *root, false, false, validationScore(game, *root));
        } else {
            install.id = game.id;
            install.installPath.clear();
            install.status = "user";
            install.userOverride = true;
            install.confidence = 8;
        }

        install.installId = makeInstallId(game, root.value_or(std::filesystem::path{}), savedTlk.value_or(std::filesystem::path{}));
        install.displayName = settings.readString(base + "DisplayName", defaultInstallName(game, install.installPath));
        install.tlkPath = savedTlk.value_or(install.tlkPath);
        install.overridePath = settings.readPath(base + "OverridePath").value_or(install.overridePath);
        install.dataRootPath = settings.readPath(base + "DataRootPath").value_or(install.dataRootPath);
        install.detected = settings.readBool(base + "Detected", false);
        install.userOverride = settings.readBool(base + "UserOverride", install.userOverride);
        if (install.userOverride) install.confidence = std::max(install.confidence, 8);
        refreshDerivedPaths(game, install);
        return install;
    }
};

#if defined(_WIN32)
inline std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), needed);
    while (!wide.empty() && wide.back() == L'\0') wide.pop_back();
    return wide;
}

inline std::string wideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), needed, nullptr, nullptr);
    while (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

inline std::optional<std::filesystem::path> readRegistryPath(const std::string& descriptor) {
    const auto slash = descriptor.find('/');
    if (slash == std::string::npos) return std::nullopt;
    const std::wstring keyPath = utf8ToWide(descriptor.substr(0, slash));
    const std::wstring valueName = utf8ToWide(descriptor.substr(slash + 1));
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, keyPath.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS &&
            RegOpenKeyExW(root, keyPath.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
            continue;
        }
        DWORD type = 0;
        DWORD bytes = 0;
        const LONG query = RegQueryValueExW(key, valueName.c_str(), nullptr, &type, nullptr, &bytes);
        if (query != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0) {
            RegCloseKey(key);
            continue;
        }
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(key, valueName.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(value.data()), &bytes) != ERROR_SUCCESS) {
            RegCloseKey(key);
            continue;
        }
        RegCloseKey(key);
        while (!value.empty() && value.back() == L'\0') value.pop_back();
        if (type == REG_EXPAND_SZ) {
            DWORD expandedLen = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
            if (expandedLen > 0) {
                std::wstring expanded(expandedLen, L'\0');
                ExpandEnvironmentStringsW(value.c_str(), expanded.data(), expandedLen);
                while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
                value = std::move(expanded);
            }
        }
        if (!value.empty()) return std::filesystem::path(wideToUtf8(value));
    }
    return std::nullopt;
}
#else
inline std::optional<std::filesystem::path> readRegistryPath(const std::string&) { return std::nullopt; }
#endif

inline std::optional<std::string> envValue(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t valueSize = 0;
    if (_dupenv_s(&value, &valueSize, name) != 0 || value == nullptr || valueSize == 0 || *value == '\0') {
        if (value != nullptr) {
            std::free(value);
        }
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::string(value);
#endif
}

inline void addIfDirectory(std::vector<std::filesystem::path>& roots, const std::filesystem::path& path) {
    if (isDirectoryPath(path) && std::find_if(roots.begin(), roots.end(), [&](const auto& existing) {
            return neosettings::samePathForMru(existing, path);
        }) == roots.end()) {
        roots.push_back(neosettings::normalizedPath(path));
    }
}

inline std::vector<std::filesystem::path> parseSteamLibraryFolders(const std::filesystem::path& vdf) {
    std::vector<std::filesystem::path> libraries;
    std::ifstream in(vdf);
    if (!in) return libraries;
    std::string line;
    while (std::getline(in, line)) {
        const auto pathPos = line.find("\"path\"");
        if (pathPos == std::string::npos && line.find("\"0\"") == std::string::npos && line.find("\"1\"") == std::string::npos) continue;
        std::vector<std::string> quoted;
        std::size_t pos = 0;
        while (true) {
            const auto begin = line.find('"', pos);
            if (begin == std::string::npos) break;
            const auto end = line.find('"', begin + 1);
            if (end == std::string::npos) break;
            quoted.push_back(line.substr(begin + 1, end - begin - 1));
            pos = end + 1;
        }
        if (quoted.size() >= 2) {
            std::string path = quoted.back();
            std::string unescaped;
            unescaped.reserve(path.size());
            for (std::size_t i = 0; i < path.size(); ++i) {
                if (path[i] == '\\' && i + 1 < path.size()) {
                    const char next = path[++i];
                    if (next == '\\') unescaped.push_back('\\');
                    else { unescaped.push_back('\\'); unescaped.push_back(next); }
                } else {
                    unescaped.push_back(path[i]);
                }
            }
            addIfDirectory(libraries, std::filesystem::path(unescaped));
        }
    }
    return libraries;
}

inline std::vector<std::filesystem::path> launcherRoots() {
    std::vector<std::filesystem::path> roots;
#if defined(_WIN32)
    for (const char* env : {"ProgramFiles(x86)", "ProgramFiles", "ProgramW6432"}) {
        if (const auto value = envValue(env)) {
            addIfDirectory(roots, std::filesystem::path(*value) / "Steam" / "steamapps" / "common");
            addIfDirectory(roots, std::filesystem::path(*value) / "GOG Galaxy" / "Games");
            addIfDirectory(roots, std::filesystem::path(*value) / "GOG.com");
            addIfDirectory(roots, std::filesystem::path(*value) / "EA Games");
            addIfDirectory(roots, std::filesystem::path(*value) / "Origin Games");
        }
    }
    if (const auto gog = envValue("GOG_GALAXY_GAMES")) addIfDirectory(roots, std::filesystem::path(*gog));
#else
    if (const auto home = envValue("HOME")) {
        addIfDirectory(roots, std::filesystem::path(*home) / ".steam" / "steam" / "steamapps" / "common");
        addIfDirectory(roots, std::filesystem::path(*home) / ".local" / "share" / "Steam" / "steamapps" / "common");
        addIfDirectory(roots, std::filesystem::path(*home) / "GOG Games");
    }
#if defined(__APPLE__)
    addIfDirectory(roots, "/Applications");
#endif
#endif

    std::vector<std::filesystem::path> steamLibraries;
#if defined(_WIN32)
    for (const char* env : {"ProgramFiles(x86)", "ProgramFiles", "ProgramW6432"}) {
        if (const auto value = envValue(env)) {
            const auto parsed = parseSteamLibraryFolders(std::filesystem::path(*value) / "Steam" / "steamapps" / "libraryfolders.vdf");
            steamLibraries.insert(steamLibraries.end(), parsed.begin(), parsed.end());
        }
    }
#else
    if (const auto home = envValue("HOME")) {
        for (const auto& vdf : {std::filesystem::path(*home) / ".steam" / "steam" / "steamapps" / "libraryfolders.vdf",
                               std::filesystem::path(*home) / ".local" / "share" / "Steam" / "steamapps" / "libraryfolders.vdf"}) {
            const auto parsed = parseSteamLibraryFolders(vdf);
            steamLibraries.insert(steamLibraries.end(), parsed.begin(), parsed.end());
        }
    }
#endif
    for (const auto& library : steamLibraries) {
        addIfDirectory(roots, library / "steamapps" / "common");
    }
    return roots;
}

inline std::vector<std::filesystem::path> candidateRootsForGame(const GameDefinition& game) {
    std::vector<std::filesystem::path> candidates;

    for (const auto& descriptor : game.registryValues) {
        if (const auto path = readRegistryPath(descriptor)) addIfDirectory(candidates, *path);
    }

    const auto roots = launcherRoots();
    for (const auto& root : roots) {
        for (const auto& name : game.commonDirectoryNames) {
            addIfDirectory(candidates, root / name);
        }
    }

#if defined(_WIN32)
    for (const char* env : {"ProgramFiles(x86)", "ProgramFiles", "ProgramW6432"}) {
        if (const auto value = envValue(env)) {
            for (const auto& name : game.commonDirectoryNames) {
                addIfDirectory(candidates, std::filesystem::path(*value) / name);
                addIfDirectory(candidates, std::filesystem::path(*value) / "LucasArts" / name);
                addIfDirectory(candidates, std::filesystem::path(*value) / "BioWare" / name);
                addIfDirectory(candidates, std::filesystem::path(*value) / "EA Games" / name);
                addIfDirectory(candidates, std::filesystem::path(*value) / "GOG.com" / name);
            }
        }
    }
#else
    if (const auto home = envValue("HOME")) {
        for (const auto& name : game.commonDirectoryNames) {
            addIfDirectory(candidates, std::filesystem::path(*home) / "Games" / name);
            addIfDirectory(candidates, std::filesystem::path(*home) / "GOG Games" / name);
        }
    }
#endif
    return candidates;
}

inline std::vector<std::filesystem::path> ancestorDirectories(const std::filesystem::path& hint) {
    std::vector<std::filesystem::path> dirs;
    if (hint.empty()) return dirs;
    std::filesystem::path current = hint;
    std::error_code ec;
    if (!std::filesystem::is_directory(current, ec)) current = current.parent_path();
    while (!current.empty()) {
        addIfDirectory(dirs, current);
        const auto parent = current.parent_path();
        if (parent == current || parent.empty()) break;
        current = parent;
    }
    return dirs;
}

inline std::vector<GameInstall> detectInstallsForGame(const GameDefinition& game,
                                                       const std::optional<std::filesystem::path>& hint = std::nullopt,
                                                       bool commonLocations = true) {
    std::vector<std::filesystem::path> candidates;
    if (hint && !hint->empty()) {
        const auto ancestors = ancestorDirectories(*hint);
        candidates.insert(candidates.end(), ancestors.begin(), ancestors.end());
    }
    if (commonLocations) {
        const auto common = candidateRootsForGame(game);
        candidates.insert(candidates.end(), common.begin(), common.end());
    }

    std::vector<GameInstall> installs;
    for (const auto& candidate : candidates) {
        const int score = validationScore(game, candidate);
        if (score <= 0) continue;
        upsertInstall(installs, game, makeInstall(game, candidate, true, false, score));
    }
    sortInstalls(installs);
    return installs;
}

class GameInstallResolver final {
public:
    explicit GameInstallResolver(GamePathSettings settings = {}) : settings_(std::move(settings)) {}

    std::vector<GameInstall> resolveInstalls(const GameDefinition& game,
                                             const std::optional<std::filesystem::path>& hint = std::nullopt,
                                             bool persistDetected = true) const {
        auto installs = settings_.readAll(game);
        for (auto& install : installs) refreshDerivedPaths(game, install);

        const auto detected = detectInstallsForGame(game, hint, true);
        for (const auto& install : detected) upsertInstall(installs, game, install);

        const std::string active = settings_.activeInstallId(game.id).value_or(std::string{});
        sortInstalls(installs, active);
        if (persistDetected && !detected.empty()) settings_.writeAll(game, installs, active);
        return installs;
    }

    std::optional<GameInstall> resolve(const GameDefinition& game,
                                       const std::optional<std::filesystem::path>& hint = std::nullopt,
                                       bool persistDetected = true) const {
        auto installs = resolveInstalls(game, hint, persistDetected);
        if (installs.empty()) return std::nullopt;
        return chooseInstall(game, installs, hint);
    }

    std::vector<GameInstall> resolveAll(const std::optional<std::filesystem::path>& hint = std::nullopt,
                                        bool persistDetected = true) const {
        std::vector<GameInstall> resolved;
        for (const auto& game : knownGames()) {
            if (auto install = resolve(game, hint, persistDetected)) {
                resolved.push_back(*install);
            } else {
                GameInstall missing;
                missing.id = game.id;
                missing.displayName = game.displayName;
                missing.status = "not found";
                resolved.push_back(std::move(missing));
            }
        }
        return resolved;
    }

    std::vector<GameInstall> resolveAllInstalls(const std::optional<std::filesystem::path>& hint = std::nullopt,
                                                bool persistDetected = true) const {
        std::vector<GameInstall> out;
        for (const auto& game : knownGames()) {
            auto installs = resolveInstalls(game, hint, persistDetected);
            out.insert(out.end(), installs.begin(), installs.end());
        }
        return out;
    }

    std::optional<GameInstall> inferFromOpenedPath(const std::filesystem::path& path,
                                                   bool persistDetected = true) const {
        if (path.empty()) return std::nullopt;

        std::optional<GameInstall> savedMatch;
        std::size_t savedMatchLen = 0;
        for (const auto& game : knownGames()) {
            for (auto install : settings_.readAll(game)) {
                refreshDerivedPaths(game, install);
                if (!installContainsPath(install, path)) continue;
                const std::size_t len = pathTextLength(install.installPath.empty() ? install.tlkPath : install.installPath);
                if (!savedMatch || len > savedMatchLen) {
                    savedMatch = install;
                    savedMatchLen = len;
                }
            }
        }
        if (savedMatch) return savedMatch;

        GameInstall best;
        int bestScore = 0;
        std::size_t bestLen = 0;
        const auto ancestors = ancestorDirectories(path);
        for (const auto& game : knownGames()) {
            for (const auto& ancestor : ancestors) {
                const int score = validationScore(game, ancestor);
                const std::size_t len = pathTextLength(ancestor);
                if (score > bestScore || (score == bestScore && score > 0 && len > bestLen)) {
                    bestScore = score;
                    bestLen = len;
                    best = makeInstall(game, ancestor, true, false, score);
                }
            }
        }
        if (bestScore > 0) {
            if (persistDetected) settings_.write(best);
            return best;
        }
        return std::nullopt;
    }

    GameInstall rememberUserInstall(const std::string& gameId, const std::filesystem::path& root,
                                    const std::filesystem::path& explicitTlk = {},
                                    const std::string& displayName = {},
                                    const std::string& installId = {}) const {
        const auto* game = findGame(gameId);
        if (game == nullptr || root.empty()) return {};
        GameInstall install = makeInstall(*game, root, false, true, std::max(8, validationScore(*game, root)),
                                          displayName, installId);
        if (!explicitTlk.empty()) install.tlkPath = neosettings::normalizedPath(explicitTlk);
        if (install.displayName.empty()) install.displayName = defaultInstallName(*game, install.installPath);
        refreshDerivedPaths(*game, install);

        auto installs = settings_.readAll(*game);
        upsertInstall(installs, *game, install);
        auto actual = std::find_if(installs.begin(), installs.end(), [&](const GameInstall& saved) {
            return sameInstallTarget(saved, install);
        });
        const std::string activeId = actual == installs.end() ? install.installId : actual->installId;
        settings_.writeAll(*game, installs, activeId);
        return actual == installs.end() ? install : *actual;
    }

    GameInstall rememberUserTlk(const std::string& gameId, const std::filesystem::path& tlkPath,
                                const std::string& installId = {},
                                const std::string& displayName = {}) const {
        const auto* game = findGame(gameId);
        if (game == nullptr || tlkPath.empty()) return {};

        auto installs = settings_.readAll(*game);
        auto it = installs.end();
        if (!installId.empty()) {
            it = std::find_if(installs.begin(), installs.end(), [&](const GameInstall& install) {
                return install.installId == installId;
            });
        }
        if (it == installs.end()) {
            it = std::find_if(installs.begin(), installs.end(), [&](const GameInstall& install) {
                return installContainsPath(install, tlkPath);
            });
        }

        GameInstall install;
        if (it != installs.end()) {
            install = *it;
            installs.erase(it);
        } else {
            install.id = game->id;
            const std::string tlkLeaf = lowerAscii(tlkPath.filename().u8string());
            install.installPath = tlkLeaf == "dialog.tlk" ? tlkPath.parent_path() : std::filesystem::path{};
            install.installId = makeInstallId(*game, install.installPath, tlkPath);
            install.displayName = displayName.empty() ? defaultInstallName(*game, install.installPath) : displayName;
            install.confidence = 8;
        }

        if (!displayName.empty()) install.displayName = displayName;
        install.tlkPath = neosettings::normalizedPath(tlkPath);
        install.userOverride = true;
        install.detected = false;
        install.confidence = std::max(install.confidence, 8);
        refreshDerivedPaths(*game, install);
        upsertInstall(installs, *game, install);
        auto actual = std::find_if(installs.begin(), installs.end(), [&](const GameInstall& saved) {
            return sameInstallTarget(saved, install);
        });
        const std::string activeId = actual == installs.end() ? install.installId : actual->installId;
        settings_.writeAll(*game, installs, activeId);
        return actual == installs.end() ? install : *actual;
    }

    std::optional<std::filesystem::path> bestTlkForPath(const std::filesystem::path& path) const {
        if (const auto inferred = inferFromOpenedPath(path, true)) {
            if (existsPath(inferred->tlkPath)) return inferred->tlkPath;
            return std::nullopt;
        }
        for (const auto& game : knownGames()) {
            auto installs = resolveInstalls(game, std::nullopt, false);
            if (installs.empty()) continue;
            if (auto chosen = chooseInstall(game, installs, std::nullopt)) {
                if (existsPath(chosen->tlkPath)) return chosen->tlkPath;
            }
        }
        return std::nullopt;
    }

    GamePathSettings& settings() noexcept { return settings_; }
    const GamePathSettings& settings() const noexcept { return settings_; }

private:
    std::optional<GameInstall> chooseInstall(const GameDefinition& game,
                                             std::vector<GameInstall>& installs,
                                             const std::optional<std::filesystem::path>& hint) const {
        if (installs.empty()) return std::nullopt;
        if (hint && !hint->empty()) {
            auto best = installs.end();
            std::size_t bestLen = 0;
            for (auto it = installs.begin(); it != installs.end(); ++it) {
                if (!installContainsPath(*it, *hint)) continue;
                const std::size_t len = pathTextLength(it->installPath.empty() ? it->tlkPath : it->installPath);
                if (best == installs.end() || len > bestLen) {
                    best = it;
                    bestLen = len;
                }
            }
            if (best != installs.end()) return *best;
        }

        const auto active = settings_.activeInstallId(game.id);
        if (active && !active->empty()) {
            const auto it = std::find_if(installs.begin(), installs.end(), [&](const GameInstall& install) {
                return install.installId == *active;
            });
            if (it != installs.end()) return *it;
        }

        const auto withTlk = std::find_if(installs.begin(), installs.end(), [](const GameInstall& install) {
            return existsPath(install.tlkPath);
        });
        if (withTlk != installs.end()) return *withTlk;

        sortInstalls(installs);
        return installs.front();
    }

    GamePathSettings settings_;
};

inline GameInstallResolver& resolver() {
    static GameInstallResolver instance;
    return instance;
}

} // namespace neogames
