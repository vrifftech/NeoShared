#pragma once

#include "NeoSettings.hpp"

#include <neoshared/game/Installation.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neogames {

using GameId = neoshared::game::GameId;
using GameDefinition = neoshared::game::GameDefinition;
using GameInstall = neoshared::game::GameInstall;
using DiscoveryOptions = neoshared::game::DiscoveryOptions;
using ResourceGameContext = neoshared::game::ResourceGameContext;
using TalkTableResolution = neoshared::game::TalkTableResolution;

using neoshared::game::confidenceText;
using neoshared::game::defaultInstallName;
using neoshared::game::existsPath;
using neoshared::game::findGame;
using neoshared::game::firstExisting;
using neoshared::game::gameIdFromString;
using neoshared::game::gameIdString;
using neoshared::game::installContainsPath;
using neoshared::game::isDirectoryPath;
using neoshared::game::knownGames;
using neoshared::game::makeInstall;
using neoshared::game::makeInstallId;
using neoshared::game::normalizeConfiguredPath;
using neoshared::game::refreshDerivedPaths;
using neoshared::game::sameInstallTarget;
using neoshared::game::sortInstalls;
using neoshared::game::upsertInstall;
using neoshared::game::validationScore;

inline std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
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

    std::optional<std::string> activeGameId() const {
        neosettings::SharedSettings settings;
        return settings.readString("GamePaths/ActiveGameId");
    }

    void setActiveGameId(const std::string& gameId) const {
        neosettings::SharedSettings settings;
        if (gameId.empty()) settings.deleteEntry("GamePaths/ActiveGameId");
        else settings.writeString("GamePaths/ActiveGameId", gameId);
    }

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

    // Read saved labels and paths without discovery. This is safe while a main
    // window and its menus are being constructed.
    std::vector<GameInstall> readSaved(const GameDefinition& game) const {
        neosettings::SharedSettings settings;
        const auto base = installListBase(game.id);
        const auto count = std::min<std::size_t>(parseCount(settings.readString(base + "Count", "0")), 256u);
        std::vector<GameInstall> installs;
        for (std::size_t i = 0; i < count; ++i) {
            const auto item = base + std::to_string(i) + "/";
            GameInstall install;
            install.id = game.id;
            install.installId = settings.readString(item + "Id", std::string{});
            install.installPath = normalizeConfiguredPath(
                settings.readPath(item + "InstallPath").value_or(std::filesystem::path{}));
            install.tlkPath = normalizeConfiguredPath(
                settings.readPath(item + "TLKPath").value_or(std::filesystem::path{}));
            install.overridePath = normalizeConfiguredPath(
                settings.readPath(item + "OverridePath").value_or(std::filesystem::path{}));
            install.dataRootPath = normalizeConfiguredPath(
                settings.readPath(item + "DataRootPath").value_or(std::filesystem::path{}));
            install.displayName = settings.readString(item + "Name", defaultInstallName(game, install.installPath));
            installs.push_back(std::move(install));
        }
        if (installs.empty()) {
            const auto legacy = settingBase(game.id);
            const auto root = settings.readPath(legacy + "InstallPath");
            if (root && !root->empty()) {
                GameInstall install;
                install.id = game.id;
                install.installPath = normalizeConfiguredPath(*root);
                install.displayName = settings.readString(
                    legacy + "DisplayName", defaultInstallName(game, *root));
                installs.push_back(std::move(install));
            }
        }
        return installs;
    }

    std::vector<GameInstall> readAll(const GameDefinition& game) const {
        neosettings::SharedSettings settings;
        const std::string base = installListBase(game.id);
        const std::size_t count = std::min<std::size_t>(
            parseCount(settings.readString(base + "Count", "0")), 256u);
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
            install.confidence = static_cast<int>(parseCount(
                settings.readString(item + "ConfidenceScore", "0")));
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

    void writeAll(const GameDefinition& game,
                  std::vector<GameInstall> installs,
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

        if (activeId.empty()) {
            activeId = settings.readString(settingBase(game.id) + "ActiveInstallId", std::string{});
        }
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
        settings.writeString(settingBase(game.id) + "Confidence",
                             confidenceText(active.confidence, active.userOverride));
    }

    bool renameInstall(const std::string& gameId,
                       const std::string& installId,
                       const std::string& newName) const {
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
        setActiveGameId(gameId);
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
        if (activeGameId().value_or(std::string{}) == gameId) setActiveGameId({});
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
            install.status = "user";
            install.userOverride = true;
            install.confidence = 8;
        }

        install.installId = makeInstallId(
            game, root.value_or(std::filesystem::path{}),
            savedTlk.value_or(std::filesystem::path{}));
        install.displayName = settings.readString(
            base + "DisplayName", defaultInstallName(game, install.installPath));
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

class GameInstallResolver final {
public:
    explicit GameInstallResolver(GamePathSettings settings = {}) : settings_(std::move(settings)) {}

    std::vector<GameInstall> resolveInstalls(
        const GameDefinition& game,
        const std::optional<std::filesystem::path>& hint = std::nullopt,
        bool persistDetected = true) const {
        auto installs = settings_.readAll(game);
        const auto detected = neoshared::game::discoverGameInstallations(game, hint);
        for (const auto& install : detected) upsertInstall(installs, game, install);
        const std::string active = settings_.activeInstallId(game.id).value_or(std::string{});
        sortInstalls(installs, active);
        if (persistDetected && !detected.empty()) settings_.writeAll(game, installs, active);
        return installs;
    }

    std::optional<GameInstall> resolve(
        const GameDefinition& game,
        const std::optional<std::filesystem::path>& hint = std::nullopt,
        bool persistDetected = true) const {
        auto installs = resolveInstalls(game, hint, persistDetected);
        return chooseInstall(game, installs, hint);
    }

    std::vector<GameInstall> resolveAll(
        const std::optional<std::filesystem::path>& hint = std::nullopt,
        bool persistDetected = true) const {
        std::vector<GameInstall> resolved;
        for (const auto& game : knownGames()) {
            auto installs = resolveInstalls(game, hint, persistDetected);
            if (const auto install = chooseInstall(game, installs, hint)) {
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

    std::vector<GameInstall> resolveAllInstalls(
        const std::optional<std::filesystem::path>& hint = std::nullopt,
        bool persistDetected = true) const {
        DiscoveryOptions options;
        if (hint && !hint->empty()) {
            std::filesystem::path current = normalizeConfiguredPath(*hint);
            if (!isDirectoryPath(current)) current = current.parent_path();
            while (!current.empty()) {
                options.additionalCandidates.push_back(current);
                const auto parent = current.parent_path();
                if (parent.empty() || parent == current) break;
                current = parent;
            }
        }
        const auto detected = neoshared::game::discoverGameInstallations(options);

        std::vector<GameInstall> out;
        for (const auto& game : knownGames()) {
            auto installs = settings_.readAll(game);
            for (const auto& install : detected) {
                if (install.id == game.id) upsertInstall(installs, game, install);
            }
            const std::string active = settings_.activeInstallId(game.id).value_or(std::string{});
            sortInstalls(installs, active);
            if (persistDetected && std::any_of(detected.begin(), detected.end(), [&](const GameInstall& install) {
                    return install.id == game.id;
                })) {
                settings_.writeAll(game, installs, active);
            }
            out.insert(out.end(), installs.begin(), installs.end());
        }
        return out;
    }

    std::optional<GameInstall> inferFromOpenedPath(const std::filesystem::path& path,
                                                   bool persistDetected = true) const {
        if (path.empty()) return std::nullopt;

        std::optional<GameInstall> best;
        std::size_t bestLength = 0;
        for (const auto& game : knownGames()) {
            for (auto install : settings_.readAll(game)) {
                refreshDerivedPaths(game, install);
                if (!installContainsPath(install, path)) continue;
                const std::size_t length = neoshared::pathToUtf8(
                    normalizeConfiguredPath(install.installPath.empty() ? install.tlkPath : install.installPath)).size();
                if (!best || length > bestLength) {
                    best = install;
                    bestLength = length;
                }
            }
        }
        if (!best) {
            std::filesystem::path current = normalizeConfiguredPath(path);
            if (!isDirectoryPath(current)) current = current.parent_path();
            while (!current.empty()) {
                if (const auto inspected = neoshared::game::inspectGameInstallation(
                        current, std::nullopt, "Opened resource")) {
                    best = *inspected;
                    break;
                }
                const auto parent = current.parent_path();
                if (parent.empty() || parent == current) break;
                current = parent;
            }
        }
        if (!best) return std::nullopt;

        if (persistDetected) {
            if (const auto* game = findGame(best->id)) {
                auto installs = settings_.readAll(*game);
                upsertInstall(installs, *game, *best);
                settings_.writeAll(*game, installs, best->installId);
                settings_.setActiveGameId(best->id);
            }
        }
        return best;
    }

    TalkTableResolution resolveContext(const std::filesystem::path& path,
                                       ResourceGameContext context = {},
                                       bool persistDetected = true) const {
        if (!context.preferredGame) {
            const auto active = settings_.activeGameId();
            if (active) context.preferredGame = gameIdFromString(*active);
        }

        auto installs = resolveAllInstalls(path.empty()
            ? std::optional<std::filesystem::path>{}
            : std::optional<std::filesystem::path>{path}, persistDetected);
        auto resolution = neoshared::game::resolveTalkTable(path, installs, context);
        if (resolution.installation && persistDetected) {
            const auto& install = *resolution.installation;
            if (const auto* game = findGame(install.id)) {
                auto gameInstalls = settings_.readAll(*game);
                upsertInstall(gameInstalls, *game, install);
                settings_.writeAll(*game, gameInstalls, install.installId);
                settings_.setActiveGameId(install.id);
            }
        }
        return resolution;
    }

    GameInstall rememberUserInstall(const std::string& gameId,
                                    const std::filesystem::path& root,
                                    const std::filesystem::path& explicitTlk = {},
                                    const std::string& displayName = {},
                                    const std::string& installId = {}) const {
        const auto* game = findGame(gameId);
        if (game == nullptr || root.empty()) return {};
        GameInstall install = makeInstall(
            *game, normalizeConfiguredPath(root), false, true,
            std::max(8, validationScore(*game, normalizeConfiguredPath(root))),
            displayName, installId);
        if (!explicitTlk.empty()) install.tlkPath = normalizeConfiguredPath(explicitTlk);
        refreshDerivedPaths(*game, install);

        auto installs = settings_.readAll(*game);
        upsertInstall(installs, *game, install);
        const auto actual = std::find_if(installs.begin(), installs.end(), [&](const GameInstall& saved) {
            return sameInstallTarget(saved, install);
        });
        const std::string activeId = actual == installs.end() ? install.installId : actual->installId;
        settings_.writeAll(*game, installs, activeId);
        settings_.setActiveGameId(gameId);
        return actual == installs.end() ? install : *actual;
    }

    GameInstall rememberUserTlk(const std::string& gameId,
                                const std::filesystem::path& tlkPath,
                                const std::string& installId = {},
                                const std::string& displayName = {}) const {
        const auto* game = findGame(gameId);
        if (game == nullptr || tlkPath.empty()) return {};
        const auto normalizedTlk = normalizeConfiguredPath(tlkPath);

        auto installs = settings_.readAll(*game);
        auto it = installs.end();
        if (!installId.empty()) {
            it = std::find_if(installs.begin(), installs.end(), [&](const GameInstall& install) {
                return install.installId == installId;
            });
        }
        if (it == installs.end()) {
            it = std::find_if(installs.begin(), installs.end(), [&](const GameInstall& install) {
                return installContainsPath(install, normalizedTlk);
            });
        }

        GameInstall install;
        if (it != installs.end()) {
            install = *it;
            installs.erase(it);
        } else {
            install.id = game->id;
            if (lowerAscii(neoshared::pathToUtf8(normalizedTlk.filename())) == "dialog.tlk") {
                install.installPath = normalizedTlk.parent_path();
            }
            install.installId = makeInstallId(*game, install.installPath, normalizedTlk);
            install.displayName = displayName.empty()
                ? defaultInstallName(*game, install.installPath) : displayName;
            install.confidence = 8;
        }

        if (!displayName.empty()) install.displayName = displayName;
        install.tlkPath = normalizedTlk;
        install.userOverride = true;
        install.detected = false;
        install.confidence = std::max(install.confidence, 8);
        refreshDerivedPaths(*game, install);
        upsertInstall(installs, *game, install);
        const auto actual = std::find_if(installs.begin(), installs.end(), [&](const GameInstall& saved) {
            return sameInstallTarget(saved, install);
        });
        const std::string activeId = actual == installs.end() ? install.installId : actual->installId;
        settings_.writeAll(*game, installs, activeId);
        settings_.setActiveGameId(gameId);
        return actual == installs.end() ? install : *actual;
    }

    std::optional<std::filesystem::path> bestTlkForPath(const std::filesystem::path& path) const {
        const auto resolution = resolveContext(path);
        if (!resolution.tlkPath.empty() && neoshared::game::isRegularFilePath(resolution.tlkPath)) {
            return resolution.tlkPath;
        }
        return std::nullopt;
    }

    GamePathSettings& settings() noexcept { return settings_; }
    const GamePathSettings& settings() const noexcept { return settings_; }

private:
    std::optional<GameInstall> chooseInstall(
        const GameDefinition& game,
        std::vector<GameInstall>& installs,
        const std::optional<std::filesystem::path>& hint) const {
        if (installs.empty()) return std::nullopt;
        if (hint && !hint->empty()) {
            auto best = installs.end();
            std::size_t bestLength = 0;
            for (auto it = installs.begin(); it != installs.end(); ++it) {
                if (!installContainsPath(*it, *hint)) continue;
                const std::size_t length = neoshared::pathToUtf8(
                    normalizeConfiguredPath(it->installPath.empty() ? it->tlkPath : it->installPath)).size();
                if (best == installs.end() || length > bestLength) {
                    best = it;
                    bestLength = length;
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
            return neoshared::game::isRegularFilePath(install.tlkPath);
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
