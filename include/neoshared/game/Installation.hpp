#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neoshared::game {

enum class GameId {
    Unknown = 0,
    Kotor1,
    Kotor2,
    JadeEmpire,
    NeverwinterNights,
    NeverwinterNights2,
    Witcher1,
    DragonAgeOrigins,
    DragonAge2,
};

struct GameDefinition {
    GameId game = GameId::Unknown;
    std::string id;
    std::string displayName;
    std::vector<std::string> strongMarkers;
    std::vector<std::string> weakMarkers;
    std::vector<std::string> tlkRelativePaths;
    std::vector<std::string> overrideRelativePaths;
    std::vector<std::string> dataRelativePaths;
    std::vector<std::string> commonDirectoryNames;
    std::vector<std::string> registryValues;
    std::string steamAppId;
    std::string gogAppId;
};

struct GameInstall {
    std::string id;
    std::string installId;
    std::string displayName;
    std::filesystem::path installPath;
    std::filesystem::path tlkPath;
    std::filesystem::path overridePath;
    std::filesystem::path dataRootPath;
    std::filesystem::path foundAt;
    std::vector<std::string> sources;
    std::vector<std::string> evidence;
    bool detected = false;
    bool userOverride = false;
    int confidence = 0;
    std::string status;
};

struct DiscoveryOptions {
    bool launcherMetadata = true;
    bool platformRegistry = true;
    bool knownLocations = true;
    bool scanWinePrefixes = true;
    bool lutris = true;
    bool useDefaultSteamRoots = true;
    bool useDefaultApplicationRoots = true;
    bool useDefaultWinePrefixes = true;
    std::vector<std::filesystem::path> steamRoots;
    std::vector<std::filesystem::path> applicationRoots;
    std::vector<std::filesystem::path> winePrefixes;
    std::vector<std::filesystem::path> additionalCandidates;
};

struct ResourceGameContext {
    std::optional<GameId> definitiveGame;
    std::vector<GameId> compatibleGames;
    std::optional<GameId> preferredGame;
};

struct TalkTableResolution {
    std::optional<GameId> game;
    std::optional<GameInstall> installation;
    std::filesystem::path tlkPath;
    bool ambiguous = false;
    std::string diagnostic;
};

const std::vector<GameDefinition>& knownGames();
const GameDefinition* findGame(GameId game) noexcept;
const GameDefinition* findGame(std::string_view id) noexcept;
std::optional<GameId> gameIdFromString(std::string_view id) noexcept;
std::string gameIdString(GameId game);

bool existsPath(const std::filesystem::path& path);
bool isDirectoryPath(const std::filesystem::path& path);
bool isRegularFilePath(const std::filesystem::path& path);
std::filesystem::path normalizeConfiguredPath(const std::filesystem::path& path);
std::filesystem::path findExistingPathCaseInsensitive(const std::filesystem::path& path);
std::filesystem::path firstExisting(const std::filesystem::path& root,
                                    const std::vector<std::string>& relatives);
bool pathStartsWith(const std::filesystem::path& child, const std::filesystem::path& root);
bool installContainsPath(const GameInstall& install, const std::filesystem::path& path);

int validationScore(const GameDefinition& game, const std::filesystem::path& root);
std::string confidenceText(int confidence, bool userOverride);
std::string defaultInstallName(const GameDefinition& game,
                               const std::filesystem::path& root = {});
std::string makeInstallId(const GameDefinition& game,
                          const std::filesystem::path& root,
                          const std::filesystem::path& tlkPath = {});
void refreshDerivedPaths(const GameDefinition& game, GameInstall& install);
GameInstall makeInstall(const GameDefinition& game,
                        const std::filesystem::path& root,
                        bool detected,
                        bool userOverride,
                        int confidence,
                        std::string displayName = {},
                        std::string installId = {});
bool sameInstallTarget(const GameInstall& lhs, const GameInstall& rhs);
void mergeInstall(GameInstall& existing, const GameInstall& incoming);
void upsertInstall(std::vector<GameInstall>& installs,
                   const GameDefinition& game,
                   GameInstall incoming);
void sortInstalls(std::vector<GameInstall>& installs,
                  const std::string& activeInstallId = {});

std::vector<GameInstall> discoverGameInstallations(
    const DiscoveryOptions& options = {});
std::vector<GameInstall> discoverGameInstallations(
    const GameDefinition& game,
    const std::optional<std::filesystem::path>& hint = std::nullopt,
    const DiscoveryOptions& options = {});
std::optional<GameInstall> inspectGameInstallation(
    const std::filesystem::path& candidate,
    std::optional<GameId> gameHint = std::nullopt,
    std::string source = "Configured installation");

TalkTableResolution resolveTalkTable(
    const std::filesystem::path& resourcePath,
    const std::vector<GameInstall>& installations,
    const ResourceGameContext& context = {});

} // namespace neoshared::game
