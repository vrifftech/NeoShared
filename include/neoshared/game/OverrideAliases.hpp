#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace neoshared::override_aliases {

enum class Game {
    Auto,
    Kotor,
    Kotor2,
    JadeEmpire,
};

struct OverrideRoot {
    std::string alias;
    std::filesystem::path path;
    std::filesystem::path iniPath;
    std::size_t precedence{};
    bool fromIni{};
};

namespace detail {

inline std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

inline std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1u);
}

inline bool isRegularFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

inline bool isDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

inline std::optional<std::filesystem::path> caseInsensitiveChild(
    const std::filesystem::path& parent, std::string_view wanted) {
    std::error_code ec;
    if (!std::filesystem::is_directory(parent, ec) || ec) return std::nullopt;
    const std::string foldedWanted = lowerAscii(std::string(wanted));
    for (std::filesystem::directory_iterator it(
             parent, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (lowerAscii(name) == foldedWanted) return it->path();
    }
    return std::nullopt;
}

inline std::optional<std::filesystem::path> findFile(
    const std::filesystem::path& parent, std::string_view wanted) {
    if (auto child = caseInsensitiveChild(parent, wanted); child && isRegularFile(*child)) return child;
    return std::nullopt;
}

inline std::optional<std::filesystem::path> findDirectory(
    const std::filesystem::path& parent, std::string_view wanted) {
    if (auto child = caseInsensitiveChild(parent, wanted); child && isDirectory(*child)) return child;
    return std::nullopt;
}

inline std::optional<std::filesystem::path> homeDirectory() {
#if defined(_WIN32)
    if (const char* value = std::getenv("USERPROFILE"); value && *value)
        return std::filesystem::path(value);
    const char* drive = std::getenv("HOMEDRIVE");
    const char* path = std::getenv("HOMEPATH");
    if (drive && *drive && path && *path) return std::filesystem::path(std::string(drive) + path);
#endif
    if (const char* value = std::getenv("HOME"); value && *value)
        return std::filesystem::path(value);
    return std::nullopt;
}

inline bool containsFolded(std::string haystack, std::string_view needle) {
    haystack = lowerAscii(std::move(haystack));
    return haystack.find(lowerAscii(std::string(needle))) != std::string::npos;
}

inline Game detectGame(const std::filesystem::path& installRoot) {
    if (findFile(installRoot, "JadeEmpire.ini") || findFile(installRoot, "JadeEmpire.exe"))
        return Game::JadeEmpire;
    if (findFile(installRoot, "swkotor2.ini") || findFile(installRoot, "swkotor2.exe") ||
        findFile(installRoot, "KOTOR2") || findDirectory(installRoot, "steamassets"))
        return Game::Kotor2;
    if (findFile(installRoot, "swkotor.ini") || findFile(installRoot, "swkotor.exe"))
        return Game::Kotor;

    const std::string folded = lowerAscii(installRoot.generic_string());
    if (folded.find("jade empire") != std::string::npos || folded.find("jadeempire") != std::string::npos)
        return Game::JadeEmpire;
    if (folded.find("old republic ii") != std::string::npos || folded.find("kotor2") != std::string::npos ||
        folded.find("kotor 2") != std::string::npos)
        return Game::Kotor2;
    if (folded.find("knights of the old republic") != std::string::npos ||
        folded.find("swkotor") != std::string::npos || folded.find("kotor") != std::string::npos)
        return Game::Kotor;
    return Game::Auto;
}

inline const char* iniName(Game game) {
    switch (game) {
    case Game::Kotor: return "swkotor.ini";
    case Game::Kotor2: return "swkotor2.ini";
    case Game::JadeEmpire: return "JadeEmpire.ini";
    case Game::Auto: break;
    }
    return "";
}

inline void addCandidate(std::vector<std::filesystem::path>& paths,
                         const std::filesystem::path& parent,
                         std::string_view filename) {
    if (parent.empty()) return;
    if (auto path = findFile(parent, filename)) paths.push_back(*path);
}

inline std::string pathKey(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) normalized = path.lexically_normal();
    std::string key = normalized.generic_string();
#if defined(_WIN32) || defined(__APPLE__)
    key = lowerAscii(std::move(key));
#endif
    return key;
}

inline std::vector<std::filesystem::path> iniCandidates(Game requested,
                                                        const std::filesystem::path& installRoot) {
    Game game = requested == Game::Auto ? detectGame(installRoot) : requested;
    const bool gameKnown = game != Game::Auto;
    std::vector<Game> games;
    if (game == Game::Auto) games = {Game::Kotor, Game::Kotor2, Game::JadeEmpire};
    else games = {game};

    std::vector<std::filesystem::path> result;
    const auto home = homeDirectory();
    for (Game current : games) {
        const std::string filename = iniName(current);
#if defined(__APPLE__)
        if (home && gameKnown && current == Game::Kotor) {
            const auto steam = *home / "Library/Application Support/Knights of the Old Republic";
            const auto appStore = *home / "Library/Containers/com.aspyr.kotor/Data/Library/Application Support/Knights of the Old Republic";
            const bool containerInstall = containsFolded(installRoot.generic_string(), "containers/com.aspyr.kotor");
            addCandidate(result, containerInstall ? appStore : steam, filename);
            addCandidate(result, containerInstall ? steam : appStore, filename);
        }
        if (home && gameKnown && current == Game::Kotor2) {
            const auto steam = *home / "Library/Application Support/Star Wars Knights of the Old Republic II";
            const auto appStore = *home / "Library/Containers/com.aspyr.kotor2.appstore/Data/Library/Application Support/Star Wars Knights of the Old Republic II";
            const bool containerInstall = containsFolded(installRoot.generic_string(), "containers/com.aspyr.kotor2");
            addCandidate(result, containerInstall ? appStore : steam, filename);
            addCandidate(result, containerInstall ? steam : appStore, filename);
        }
#elif !defined(_WIN32)
        if (home && gameKnown && current == Game::Kotor2)
            addCandidate(result, *home / ".local/share/aspyr-media/kotor2", filename);
#endif
        if (current == Game::Kotor2) {
            if (auto steamAssets = findDirectory(installRoot, "steamassets"))
                addCandidate(result, *steamAssets, filename);
        }
        addCandidate(result, installRoot, filename);
    }

    std::set<std::string> seen;
    std::vector<std::filesystem::path> unique;
    for (const auto& path : result) {
        if (seen.insert(pathKey(path)).second) unique.push_back(path);
    }
    return unique;
}

inline std::string expandEnvironment(std::string value) {
    if (!value.empty() && value.front() == '~') {
        if (auto home = homeDirectory())
            value = home->string() + value.substr(1u);
    }

    for (std::size_t pos = 0; (pos = value.find('%', pos)) != std::string::npos;) {
        const auto end = value.find('%', pos + 1u);
        if (end == std::string::npos) break;
        const std::string name = value.substr(pos + 1u, end - pos - 1u);
        if (const char* env = std::getenv(name.c_str())) {
            value.replace(pos, end - pos + 1u, env);
            pos += std::char_traits<char>::length(env);
        } else {
            pos = end + 1u;
        }
    }

    for (std::size_t pos = 0; (pos = value.find('$', pos)) != std::string::npos;) {
        std::size_t nameStart = pos + 1u;
        std::size_t nameEnd = nameStart;
        bool braced = nameStart < value.size() && value[nameStart] == '{';
        if (braced) {
            ++nameStart;
            nameEnd = value.find('}', nameStart);
            if (nameEnd == std::string::npos) break;
        } else {
            while (nameEnd < value.size()) {
                const unsigned char c = static_cast<unsigned char>(value[nameEnd]);
                if (!(std::isalnum(c) || c == '_')) break;
                ++nameEnd;
            }
            if (nameEnd == nameStart) { ++pos; continue; }
        }
        const std::string name = value.substr(nameStart, nameEnd - nameStart);
        if (const char* env = std::getenv(name.c_str())) {
            const std::size_t replaceEnd = braced ? nameEnd + 1u : nameEnd;
            value.replace(pos, replaceEnd - pos, env);
            pos += std::char_traits<char>::length(env);
        } else {
            pos = braced ? nameEnd + 1u : nameEnd;
        }
    }
    return value;
}

inline bool aliasOrdinal(std::string key, std::size_t& ordinal) {
    key = lowerAscii(trim(std::move(key)));
    if (key == "override") { ordinal = 0u; return true; }
    constexpr std::string_view suffix = "override";
    if (key.size() <= suffix.size() ||
        key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    const std::string prefix = key.substr(0u, key.size() - suffix.size());
    if (prefix.empty() || !std::all_of(prefix.begin(), prefix.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        return false;
    try {
        ordinal = static_cast<std::size_t>(std::stoull(prefix));
        return true;
    } catch (...) {
        return false;
    }
}

inline std::vector<std::pair<std::string, std::string>> parseAliasSection(
    const std::filesystem::path& iniPath) {
    std::ifstream input(iniPath, std::ios::binary);
    if (!input) return {};
    std::string line;
    bool inAlias = false;
    bool firstLine = true;
    std::map<std::size_t, std::pair<std::string, std::string>> values;
    while (std::getline(input, line)) {
        if (firstLine) {
            firstLine = false;
            if (line.size() >= 3u && static_cast<unsigned char>(line[0]) == 0xefu &&
                static_cast<unsigned char>(line[1]) == 0xbbu && static_cast<unsigned char>(line[2]) == 0xbfu)
                line.erase(0u, 3u);
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string stripped = trim(line);
        if (stripped.empty() || stripped.front() == ';' || stripped.front() == '#') continue;
        if (stripped.front() == '[' && stripped.back() == ']') {
            inAlias = lowerAscii(trim(stripped.substr(1u, stripped.size() - 2u))) == "alias";
            continue;
        }
        if (!inAlias) continue;
        const auto equals = stripped.find('=');
        if (equals == std::string::npos) continue;
        std::string key = trim(stripped.substr(0u, equals));
        std::string value = trim(stripped.substr(equals + 1u));
        if (value.size() >= 2u && ((value.front() == '"' && value.back() == '"') ||
                                   (value.front() == '\'' && value.back() == '\'')))
            value = value.substr(1u, value.size() - 2u);
        if (value.empty()) continue;
        std::size_t ordinal{};
        if (!aliasOrdinal(key, ordinal)) continue;
        values[ordinal] = {ordinal == 0u ? "OVERRIDE" : std::to_string(ordinal) + "OVERRIDE", value};
    }
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(values.size());
    for (auto& [ordinal, entry] : values) result.push_back(std::move(entry));
    return result;
}

inline std::filesystem::path resolveAliasPath(std::string value,
                                              const std::filesystem::path& iniPath) {
    value = expandEnvironment(trim(std::move(value)));
#if !defined(_WIN32)
    std::replace(value.begin(), value.end(), '\\', '/');
#endif
    std::filesystem::path result(value);
    if (result.is_relative()) result = iniPath.parent_path() / result;
    return result.lexically_normal();
}

} // namespace detail

inline Game detectGame(const std::filesystem::path& installRoot) {
    return detail::detectGame(installRoot);
}

inline std::vector<std::filesystem::path> findIniFiles(
    const std::filesystem::path& installRoot, Game game = Game::Auto) {
    return detail::iniCandidates(game, installRoot);
}

inline std::vector<OverrideRoot> discover(
    const std::filesystem::path& installRoot, Game game = Game::Auto) {
    std::vector<OverrideRoot> result;
    std::set<std::string> seenPaths;
    std::size_t precedence = 0u;
    for (const auto& iniPath : detail::iniCandidates(game, installRoot)) {
        const auto aliases = detail::parseAliasSection(iniPath);
        for (const auto& [alias, value] : aliases) {
            auto path = detail::resolveAliasPath(value, iniPath);
            if (!seenPaths.insert(detail::pathKey(path)).second) continue;
            result.push_back(OverrideRoot{alias, std::move(path), iniPath, precedence++, true});
        }
        if (aliases.empty()) {
            auto fallback = detail::findDirectory(iniPath.parent_path(), "Override");
            if (fallback && seenPaths.insert(detail::pathKey(*fallback)).second)
                result.push_back(OverrideRoot{"OVERRIDE", *fallback, iniPath, precedence++, false});
        }
    }

    if (auto fallback = detail::findDirectory(installRoot, "Override")) {
        if (seenPaths.insert(detail::pathKey(*fallback)).second)
            result.push_back(OverrideRoot{"OVERRIDE", *fallback, {}, precedence++, false});
    }
    return result;
}

} // namespace neoshared::override_aliases
