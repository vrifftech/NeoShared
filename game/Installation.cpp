#include <neoshared/game/Installation.hpp>

#include <neoshared/PathUtf8.hpp>
#include "SimpleJson.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

#if defined(NEOSHARED_HAVE_SQLITE3)
#include <sqlite3.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace neoshared::game {
namespace {

constexpr std::size_t kMaximumMetadataBytes = 1024u * 1024u;
constexpr std::size_t kMaximumLibraries = 128u;
constexpr std::size_t kMaximumDataRootDirectories = 64u;
constexpr std::size_t kMaximumApplications = 512u;
constexpr std::size_t kMaximumPrefixes = 64u;
constexpr std::size_t kMaximumRegistryBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaximumAllRegistryBytes = 64u * 1024u * 1024u;

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::string trimAscii(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}

bool asciiEqual(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) return false;
    }
    return true;
}

bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    return lowerAscii(haystack).find(lowerAscii(needle)) != std::string::npos;
}

std::optional<std::string> environmentValue(const char* name) {
#if defined(_WIN32)
    const int wideCount = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, nullptr, 0);
    if (wideCount <= 0) return std::nullopt;
    std::wstring wideName(static_cast<std::size_t>(wideCount), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wideName.data(), wideCount);
    const DWORD required = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (required == 0) return std::nullopt;
    std::wstring wideValue(static_cast<std::size_t>(required), L'\0');
    const DWORD copied = GetEnvironmentVariableW(wideName.c_str(), wideValue.data(), required);
    if (copied == 0 || copied >= required) return std::nullopt;
    wideValue.resize(static_cast<std::size_t>(copied));
    return pathToUtf8(std::filesystem::path(wideValue));
#else
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::string(value);
#endif
}

std::filesystem::path homeDirectory() {
#if defined(_WIN32)
    if (const auto userProfile = environmentValue("USERPROFILE")) return pathFromUtf8(*userProfile);
    const auto drive = environmentValue("HOMEDRIVE");
    const auto path = environmentValue("HOMEPATH");
    if (drive && path) return pathFromUtf8(*drive + *path);
#else
    if (const auto home = environmentValue("HOME")) return pathFromUtf8(*home);
#endif
    return {};
}

std::string sanitizeConfiguredPathText(std::string text) {
    text = trimAscii(std::move(text));
    if (text.size() >= 2u && ((text.front() == '"' && text.back() == '"') ||
                              (text.front() == '\'' && text.back() == '\''))) {
        text = text.substr(1u, text.size() - 2u);
    }
#if !defined(_WIN32)
    // Old settings sometimes contain shell-escaped spaces. Filesystem APIs do
    // not need shell quoting; retain all other backslashes literally.
    std::string unescaped;
    unescaped.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1u < text.size() && text[i + 1u] == ' ') {
            unescaped.push_back(' ');
            ++i;
        } else {
            unescaped.push_back(text[i]);
        }
    }
    text = std::move(unescaped);
#endif
    return text;
}

std::string expandEnvironmentVariables(std::string text) {
#if defined(_WIN32)
    std::wstring input = pathFromUtf8(text).native();
    const DWORD required = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (required > 0) {
        std::wstring expanded(static_cast<std::size_t>(required), L'\0');
        const DWORD written = ExpandEnvironmentStringsW(input.c_str(), expanded.data(), required);
        if (written > 0 && written <= required) {
            while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
            return pathToUtf8(std::filesystem::path(expanded));
        }
    }
    return text;
#else
    std::string output;
    output.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '$') {
            output.push_back(text[i++]);
            continue;
        }
        std::size_t begin = i + 1u;
        std::size_t end = begin;
        bool braced = false;
        if (begin < text.size() && text[begin] == '{') {
            braced = true;
            begin += 1u;
            end = text.find('}', begin);
            if (end == std::string::npos) {
                output.push_back(text[i++]);
                continue;
            }
        } else {
            while (end < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) ++end;
            if (end == begin) {
                output.push_back(text[i++]);
                continue;
            }
        }
        const std::string name = text.substr(begin, end - begin);
        if (const auto value = environmentValue(name.c_str())) output += *value;
        else output += text.substr(i, (braced ? end + 1u : end) - i);
        i = braced ? end + 1u : end;
    }
    return output;
#endif
}

std::filesystem::path expandPathText(const std::filesystem::path& supplied) {
    std::string text = sanitizeConfiguredPathText(pathToUtf8(supplied));
    text = expandEnvironmentVariables(std::move(text));
#if !defined(_WIN32)
    if (text == "~" || (text.size() > 1u && text.front() == '~' && text[1] == '/')) {
        const auto home = homeDirectory();
        if (!home.empty()) {
            return text == "~" ? home : home / pathFromUtf8(text.substr(2u));
        }
    }
#endif
    return pathFromUtf8(text);
}

std::filesystem::path normalizeLexically(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::error_code ec;
    const auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path, ec);
    const auto& candidate = ec ? path : absolute;
    ec.clear();
    const auto canonical = std::filesystem::weakly_canonical(candidate, ec);
    return (ec ? candidate.lexically_normal() : canonical);
}

std::vector<std::filesystem::path> directoryEntries(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::directory_iterator end;
    while (!ec && it != end) {
        out.push_back(it->path());
        it.increment(ec);
    }
    std::sort(out.begin(), out.end(), [](const auto& lhs, const auto& rhs) {
        const std::string left = lowerAscii(pathToUtf8(lhs.filename()));
        const std::string right = lowerAscii(pathToUtf8(rhs.filename()));
        return left == right ? pathToUtf8(lhs.filename()) < pathToUtf8(rhs.filename()) : left < right;
    });
    return out;
}

std::filesystem::path caseAwareChild(const std::filesystem::path& directory, std::string_view name) {
    if (directory.empty()) return {};
    const auto exact = directory / pathFromUtf8(name);
    std::error_code ec;
    if (std::filesystem::exists(exact, ec) && !ec) return exact;
    std::filesystem::path match;
    std::size_t matches = 0;
    for (const auto& entry : directoryEntries(directory)) {
        if (asciiEqual(pathToUtf8(entry.filename()), name)) {
            match = entry;
            ++matches;
        }
    }
    return matches == 1u ? match : std::filesystem::path{};
}

std::filesystem::path caseAwareRelative(const std::filesystem::path& root,
                                        const std::filesystem::path& relative) {
    if (root.empty()) return {};
    std::filesystem::path current = root;
    for (const auto& component : relative) {
        const std::string name = pathToUtf8(component);
        if (name.empty() || name == ".") continue;
        if (name == "..") return {};
        current = caseAwareChild(current, name);
        if (current.empty()) return {};
    }
    return current;
}

std::filesystem::path firstRegularFile(
    const std::filesystem::path& root,
    const std::vector<std::string>& relatives) {
    if (!isDirectoryPath(root)) return {};
    for (const auto& relative : relatives) {
        const auto path = caseAwareRelative(root, pathFromUtf8(relative));
        if (isRegularFilePath(path)) return path;
    }
    return {};
}

bool rootHasRequiredFile(const GameDefinition& game,
                         const std::filesystem::path& root) {
    return !firstRegularFile(root, game.requiredRootFileAlternatives).empty();
}

bool rootHasIdentityFile(const GameDefinition& game,
                         const std::filesystem::path& root) {
    return !firstRegularFile(root, game.identityFileAlternatives).empty();
}

std::string pathIdentity(const std::filesystem::path& path) {
#if !defined(_WIN32)
    struct stat info {};
    if (::stat(path.c_str(), &info) == 0 && info.st_ino != 0) {
        return "inode:" + std::to_string(static_cast<unsigned long long>(info.st_dev)) + ":" +
               std::to_string(static_cast<unsigned long long>(info.st_ino));
    }
#endif
    std::string value = genericPathToUtf8(normalizeLexically(path));
#if defined(_WIN32)
    value = lowerAscii(std::move(value));
#endif
    return "path:" + value;
}

void addUniquePath(std::vector<std::filesystem::path>& paths,
                   const std::filesystem::path& candidate,
                   bool requireDirectory = true) {
    auto normalized = normalizeConfiguredPath(candidate);
    if (normalized.empty()) return;
    if (requireDirectory && !isDirectoryPath(normalized)) return;
    const std::string identity = pathIdentity(normalized);
    if (std::none_of(paths.begin(), paths.end(), [&](const auto& existing) {
            return pathIdentity(existing) == identity;
        })) paths.push_back(std::move(normalized));
}

std::string storefrontLabel(const std::filesystem::path& root,
                            const std::vector<std::string>& sources = {}) {
    for (const auto& source : sources) {
        if (containsCaseInsensitive(source, "steam")) return "Steam";
        if (containsCaseInsensitive(source, "gog") || containsCaseInsensitive(source, "heroic")) return "GOG";
        if (containsCaseInsensitive(source, "lutris")) return "Lutris";
    }
    const std::string path = pathToUtf8(root);
    if (containsCaseInsensitive(path, "steamapps")) return "Steam";
    if (containsCaseInsensitive(path, "gog galaxy") || containsCaseInsensitive(path, "gog games") ||
        containsCaseInsensitive(path, "gog.com")) return "GOG";
    if (containsCaseInsensitive(path, "heroic")) return "Heroic";
    if (containsCaseInsensitive(path, "lutris")) return "Lutris";
    if (containsCaseInsensitive(path, "amazon games")) return "Amazon";
    if (containsCaseInsensitive(path, "origin games") || containsCaseInsensitive(path, "ea games")) return "EA";
    return {};
}

std::uint64_t fnv1a64(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const char ch : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hexValue(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << value;
    return out.str();
}

struct VdfValue {
    std::string scalar;
    std::map<std::string, VdfValue> object;
    bool isObject = false;

    const VdfValue* find(std::string_view key) const {
        const auto it = object.find(lowerAscii(std::string(key)));
        return it == object.end() ? nullptr : &it->second;
    }
};

class VdfParser {
public:
    explicit VdfParser(std::string text) : text_(std::move(text)) {}

    VdfValue parse() {
        tokenize();
        index_ = 0;
        VdfValue root;
        root.isObject = true;
        root.object = parseBlock(false, 0u);
        if (index_ != tokens_.size()) throw std::runtime_error("Unexpected VDF token");
        return root;
    }

private:
    void tokenize() {
        std::size_t i = 0;
        if (text_.size() >= 3u && static_cast<unsigned char>(text_[0]) == 0xEFu &&
            static_cast<unsigned char>(text_[1]) == 0xBBu && static_cast<unsigned char>(text_[2]) == 0xBFu) i = 3u;
        while (i < text_.size()) {
            const unsigned char ch = static_cast<unsigned char>(text_[i]);
            if (std::isspace(ch)) { ++i; continue; }
            if (text_[i] == '/' && i + 1u < text_.size() && text_[i + 1u] == '/') {
                i += 2u;
                while (i < text_.size() && text_[i] != '\n' && text_[i] != '\r') ++i;
                continue;
            }
            if (text_[i] == '{' || text_[i] == '}') {
                tokens_.push_back(std::string(1u, text_[i++]));
                continue;
            }
            if (text_[i] == '"') {
                ++i;
                std::string token;
                bool closed = false;
                while (i < text_.size()) {
                    const char current = text_[i++];
                    if (current == '"') { closed = true; break; }
                    if (current == '\\' && i < text_.size() &&
                        (text_[i] == '\\' || text_[i] == '"')) token.push_back(text_[i++]);
                    else token.push_back(current);
                }
                if (!closed) throw std::runtime_error("Unterminated VDF string");
                tokens_.push_back(std::move(token));
                continue;
            }
            const std::size_t begin = i;
            while (i < text_.size() && !std::isspace(static_cast<unsigned char>(text_[i])) &&
                   text_[i] != '{' && text_[i] != '}') ++i;
            tokens_.push_back(text_.substr(begin, i - begin));
        }
    }

    std::map<std::string, VdfValue> parseBlock(bool expectClose, std::size_t depth) {
        if (depth > 32u) throw std::runtime_error("VDF nesting limit exceeded");
        std::map<std::string, VdfValue> result;
        while (index_ < tokens_.size()) {
            if (tokens_[index_] == "}") {
                if (!expectClose) throw std::runtime_error("Unexpected VDF closing brace");
                ++index_;
                return result;
            }
            const std::string key = lowerAscii(tokens_[index_++]);
            if (key == "{" || index_ >= tokens_.size()) throw std::runtime_error("Malformed VDF key/value");
            VdfValue value;
            if (tokens_[index_] == "{") {
                ++index_;
                value.isObject = true;
                value.object = parseBlock(true, depth + 1u);
            } else if (tokens_[index_] == "}") {
                throw std::runtime_error("Missing VDF value");
            } else {
                value.scalar = tokens_[index_++];
            }
            if (!result.emplace(key, std::move(value)).second) throw std::runtime_error("Duplicate VDF key");
        }
        if (expectClose) throw std::runtime_error("Unclosed VDF block");
        return result;
    }

    std::string text_;
    std::vector<std::string> tokens_;
    std::size_t index_ = 0;
};

std::optional<std::string> readBoundedText(const std::filesystem::path& file,
                                           std::size_t maximum = kMaximumMetadataBytes) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string bytes(maximum + 1u, '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto read = stream.gcount();
    if (read < 0 || static_cast<std::size_t>(read) > maximum) return std::nullopt;
    bytes.resize(static_cast<std::size_t>(read));
    return bytes;
}

VdfValue readVdf(const std::filesystem::path& file) {
    try {
        const auto text = readBoundedText(file);
        return text ? VdfParser(*text).parse() : VdfValue{};
    } catch (...) {
        return {};
    }
}

struct Candidate {
    std::filesystem::path path;
    std::optional<GameId> game;
    std::string source;
};

class Scan {
public:
    std::filesystem::path directory(const std::filesystem::path& value) {
        const std::string key = pathToUtf8(value);
        const auto cached = directories_.find(key);
        if (cached != directories_.end()) return cached->second;
        auto path = normalizeConfiguredPath(value);
        if (!isDirectoryPath(path)) path.clear();
        directories_[key] = path;
        return path;
    }

    std::filesystem::path child(const std::filesystem::path& directory, std::string_view name) {
        return caseAwareChild(directory, name);
    }

    std::filesystem::path file(const std::filesystem::path& directory, std::string_view name) {
        const auto path = child(directory, name);
        return isRegularFilePath(path) ? path : std::filesystem::path{};
    }

    std::vector<std::filesystem::path> children(const std::filesystem::path& directory) {
        const std::string key = pathIdentity(directory);
        const auto cached = entries_.find(key);
        if (cached != entries_.end()) return cached->second;
        auto values = directoryEntries(directory);
        entries_.emplace(key, values);
        return values;
    }

    std::vector<std::filesystem::path> dataRoots(const std::filesystem::path& candidate) {
        static const std::set<std::string> containers = {
            "steamassets", "contents", "assets", "gamedata", "resources", "transgaming", "c_drive",
            "program files", "program files (x86)", "lucasarts", "swkotor", "swkotor2", "kotor2", "game"
        };
        std::deque<std::pair<std::filesystem::path, std::size_t>> pending;
        pending.emplace_back(candidate, 0u);
        std::set<std::string> seen;
        std::vector<std::filesystem::path> roots;
        while (!pending.empty() && seen.size() < kMaximumDataRootDirectories) {
            auto [directory, depth] = pending.front();
            pending.pop_front();
            const std::string identity = pathIdentity(directory);
            if (!seen.insert(identity).second) continue;
            if (!file(directory, "chitin.key").empty()) {
                addUniquePath(roots, directory);
                continue;
            }
            if (depth >= 9u) continue;
            for (const auto& entry : children(directory)) {
                const std::string name = lowerAscii(pathToUtf8(entry.filename()));
                if (containers.count(name) == 0u &&
                    !containsCaseInsensitive(name, ".app") ) continue;
                std::error_code ec;
                if (!std::filesystem::is_symlink(entry, ec) && !ec && std::filesystem::is_directory(entry, ec) && !ec) {
                    pending.emplace_back(entry, depth + 1u);
                }
            }
        }
        return roots;
    }

    std::pair<std::set<GameId>, std::vector<std::string>> identify(
        const std::filesystem::path& root) {
        std::set<GameId> games;
        std::vector<std::string> evidence;
        for (const auto& game : knownGames()) {
            if (!rootHasRequiredFile(game, root) ||
                !rootHasIdentityFile(game, root)) {
                continue;
            }
            games.insert(game.game);
            const auto marker = firstRegularFile(root, game.identityFileAlternatives);
            evidence.push_back(game.id + ": " +
                genericPathToUtf8(marker.lexically_relative(root)));
        }
        return {games, evidence};
    }

    std::size_t registryBytesLeft = kMaximumAllRegistryBytes;

private:
    std::unordered_map<std::string, std::filesystem::path> directories_;
    std::unordered_map<std::string, std::vector<std::filesystem::path>> entries_;
};

std::filesystem::path xdgPath(const char* variable, const char* fallback) {
    if (const auto value = environmentValue(variable)) {
        auto path = normalizeConfiguredPath(pathFromUtf8(*value));
        if (path.is_absolute()) return path;
    }
    return normalizeConfiguredPath(pathFromUtf8(fallback));
}

void addCandidate(std::vector<Candidate>& candidates, Scan& scan,
                  const std::filesystem::path& path, std::optional<GameId> game,
                  std::string source) {
    const auto directory = scan.directory(path);
    if (directory.empty()) return;
    candidates.push_back({directory, game, std::move(source)});
}

std::vector<std::filesystem::path> steamRoots(const DiscoveryOptions& options) {
    std::vector<std::filesystem::path> roots;
    for (const auto& root : options.steamRoots) addUniquePath(roots, root, false);
    if (!options.useDefaultSteamRoots) return roots;
    for (const char* variable : {"STEAM_DIR", "STEAM_PATH"}) {
        if (const auto value = environmentValue(variable)) addUniquePath(roots, pathFromUtf8(*value), false);
    }
#if defined(_WIN32)
    for (const auto& pair : std::array<std::pair<const char*, const char*>, 2>{{
             {"ProgramFiles(x86)", R"(C:\Program Files (x86))"}, {"ProgramFiles", R"(C:\Program Files)"}}}) {
        const auto value = environmentValue(pair.first).value_or(pair.second);
        addUniquePath(roots, pathFromUtf8(value) / "Steam", false);
    }
#elif defined(__APPLE__)
    const auto home = homeDirectory();
    addUniquePath(roots, home / "Library/Application Support/Steam", false);
    addUniquePath(roots, home / "Library/Applications/Steam", false);
#else
    if (const auto xdg = environmentValue("XDG_DATA_HOME")) {
        const auto root = pathFromUtf8(*xdg);
        if (root.is_absolute()) addUniquePath(roots, root / "Steam", false);
    }
    const auto home = homeDirectory();
    for (const auto& relative : {
             ".local/share/Steam", ".steam/steam", ".steam/root", ".steam/debian-installation",
             ".var/app/com.valvesoftware.Steam/data/Steam",
             ".var/app/com.valvesoftware.Steam/.local/share/Steam",
             "snap/steam/common/.local/share/Steam"}) {
        addUniquePath(roots, home / relative, false);
    }
#endif
    return roots;
}

void addSteamCandidates(std::vector<Candidate>& candidates, Scan& scan, const DiscoveryOptions& options) {
    std::vector<std::filesystem::path> libraries;
    std::set<std::string> seen;
    auto addLibrary = [&](const std::filesystem::path& value) {
        if (libraries.size() >= kMaximumLibraries) return;
        const auto directory = scan.directory(value);
        if (directory.empty()) return;
        if (seen.insert(pathIdentity(directory)).second) libraries.push_back(directory);
    };
    for (const auto& root : steamRoots(options)) addLibrary(root);

    for (std::size_t i = 0; i < libraries.size() && i < kMaximumLibraries; ++i) {
        const auto library = libraries[i];
        for (const char* folder : {"steamapps", "config"}) {
            const auto directory = scan.child(library, folder);
            const auto metadata = directory.empty() ? std::filesystem::path{} : scan.file(directory, "libraryfolders.vdf");
            if (metadata.empty()) continue;
            const VdfValue root = readVdf(metadata);
            const VdfValue* folders = root.find("libraryfolders");
            if (folders == nullptr || !folders->isObject) continue;
            for (const auto& [key, value] : folders->object) {
                if (!std::all_of(key.begin(), key.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) continue;
                const std::string path = value.isObject && value.find("path") ? value.find("path")->scalar : value.scalar;
                if (!trimAscii(path).empty()) addLibrary(pathFromUtf8(path));
            }
        }
    }

    const std::array<std::pair<const char*, GameId>, 2> apps{{
        {"32370", GameId::Kotor1}, {"208580", GameId::Kotor2}
    }};
    const std::array<const char*, 3> fallbackNames{{"swkotor", "Knights of the Old Republic II", "kotor2"}};
    for (const auto& library : libraries) {
        const auto steamapps = scan.child(library, "steamapps");
        const auto common = steamapps.empty() ? std::filesystem::path{} : scan.child(steamapps, "common");
        if (common.empty()) continue;
        for (const auto& [appid, game] : apps) {
            const auto manifest = scan.file(steamapps, std::string("appmanifest_") + appid + ".acf");
            if (manifest.empty()) continue;
            const VdfValue root = readVdf(manifest);
            const VdfValue* state = root.find("appstate");
            if (state == nullptr || !state->isObject) continue;
            const auto* id = state->find("appid");
            const auto* install = state->find("installdir");
            if (id == nullptr || install == nullptr || id->scalar != appid) continue;
            const std::string name = trimAscii(install->scalar);
            if (name.empty() || name == "." || name == ".." ||
                name.find_first_of("/\\:") != std::string::npos) continue;
            addCandidate(candidates, scan, common / pathFromUtf8(name), game,
                         std::string("Steam app ") + appid + ": " + pathToUtf8(manifest));
        }
        for (const char* name : fallbackNames) {
            addCandidate(candidates, scan, common / name, std::nullopt,
                         "Steam fallback: " + pathToUtf8(library));
        }
    }
}

std::optional<neojson::Value> readJson(const std::filesystem::path& file) {
    try {
        const auto text = readBoundedText(file);
        if (!text) return std::nullopt;
        return neojson::parse(*text);
    } catch (...) {
        return std::nullopt;
    }
}

void addHeroicCandidates(std::vector<Candidate>& candidates, Scan& scan) {
    std::vector<std::filesystem::path> configs;
#if defined(_WIN32)
    const auto appData = environmentValue("APPDATA");
    configs.push_back((appData ? pathFromUtf8(*appData) : homeDirectory() / "AppData/Roaming") / "heroic");
#elif defined(__APPLE__)
    configs.push_back(homeDirectory() / "Library/Application Support/heroic");
#else
    configs.push_back(xdgPath("XDG_CONFIG_HOME", "~/.config") / "heroic");
    configs.push_back(homeDirectory() / ".var/app/com.heroicgameslauncher.hgl/config/heroic");
#endif
    for (const auto& config : configs) {
        const auto root = scan.directory(config / "gog_store");
        const auto metadata = root.empty() ? std::filesystem::path{} : scan.file(root, "installed.json");
        if (metadata.empty()) continue;
        const auto json = readJson(metadata);
        if (!json || !json->isObject()) continue;
        const auto* installed = json->find("installed");
        if (installed == nullptr || !installed->isArray()) continue;
        for (const auto& item : installed->asArray()) {
            if (!item.isObject()) continue;
            const auto* app = item.find("appName");
            const auto* path = item.find("install_path");
            if (app == nullptr || path == nullptr || !path->isString()) continue;
            const std::string appId = app->asStringOrNumber();
            const auto game = appId == "1207666283" ? std::optional<GameId>(GameId::Kotor1) :
                              appId == "1421404581" ? std::optional<GameId>(GameId::Kotor2) : std::nullopt;
            if (!game) continue;
            addCandidate(candidates, scan, pathFromUtf8(path->asString()), *game,
                         "Heroic GOG " + appId + ": " + pathToUtf8(metadata));
        }
    }
}

#if defined(NEOSHARED_HAVE_SQLITE3)
void addLutrisDatabaseCandidates(std::vector<Candidate>& candidates, Scan& scan,
                                 const std::filesystem::path& database) {
    if (!isRegularFilePath(database)) return;
    sqlite3* handle = nullptr;
    const std::string uri = "file:" + pathToUtf8(normalizeConfiguredPath(database)) + "?mode=ro";
    if (sqlite3_open_v2(uri.c_str(), &handle, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
        if (handle != nullptr) sqlite3_close(handle);
        return;
    }
    sqlite3_busy_timeout(handle, 100);
    std::set<std::string> columns;
    sqlite3_stmt* info = nullptr;
    if (sqlite3_prepare_v2(handle, "PRAGMA table_info(games)", -1, &info, nullptr) == SQLITE_OK) {
        while (sqlite3_step(info) == SQLITE_ROW) {
            const auto* text = sqlite3_column_text(info, 1);
            if (text != nullptr) columns.insert(reinterpret_cast<const char*>(text));
        }
    }
    if (info != nullptr) sqlite3_finalize(info);
    if (columns.count("installed") == 0u || columns.count("directory") == 0u) {
        sqlite3_close(handle);
        return;
    }
    const std::string query =
        "SELECT directory, "
        + std::string(columns.count("service") ? "service" : "NULL") + ", "
        + std::string(columns.count("service_id") ? "CAST(service_id AS TEXT)" : "NULL") + ", "
        + std::string(columns.count("name") ? "name" : "NULL") + ", "
        + std::string(columns.count("slug") ? "slug" : "NULL") +
        " FROM games WHERE installed=1 LIMIT 512";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(handle, query.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_close(handle);
        return;
    }
    std::size_t accepted = 0;
    while (accepted < 128u && sqlite3_step(statement) == SQLITE_ROW) {
        const auto textAt = [&](int column) -> std::string {
            const auto* text = sqlite3_column_text(statement, column);
            return text == nullptr ? std::string{} : std::string(reinterpret_cast<const char*>(text));
        };
        const std::string directory = textAt(0);
        const std::string service = lowerAscii(textAt(1));
        const std::string serviceId = textAt(2);
        const std::string name = lowerAscii(textAt(3));
        const std::string slug = lowerAscii(textAt(4));
        std::optional<GameId> game;
        if (service == "gog" && serviceId == "1207666283") game = GameId::Kotor1;
        else if (service == "gog" && serviceId == "1421404581") game = GameId::Kotor2;
        const bool nameMatch = name.find("kotor") != std::string::npos || slug.find("kotor") != std::string::npos ||
                               name.find("knights of the old republic") != std::string::npos ||
                               slug.find("knights-of-the-old-republic") != std::string::npos;
        if (!game && !nameMatch) continue;
        addCandidate(candidates, scan, pathFromUtf8(directory), game, "Lutris: " + pathToUtf8(database));
        ++accepted;
    }
    sqlite3_finalize(statement);
    sqlite3_close(handle);
}
#endif

void addLutrisCandidates(std::vector<Candidate>& candidates, Scan& scan) {
#if defined(__linux__) && defined(NEOSHARED_HAVE_SQLITE3)
    std::vector<std::filesystem::path> databases;
    const auto configHome = xdgPath("XDG_CONFIG_HOME", "~/.config");
    const auto dataHome = xdgPath("XDG_DATA_HOME", "~/.local/share");
    databases.push_back(dataHome / "lutris/pga.db");
    databases.push_back(homeDirectory() / ".var/app/net.lutris.Lutris/data/lutris/pga.db");
    addUniquePath(databases, configHome / "lutris/pga.db", false);
    for (const auto& database : databases) addLutrisDatabaseCandidates(candidates, scan, database);
#else
    (void)candidates;
    (void)scan;
#endif
}

void addCollectionCandidates(std::vector<Candidate>& candidates, Scan& scan,
                             const std::vector<std::filesystem::path>& collections,
                             const std::string& source) {
    std::set<std::string> seen;
    std::size_t offered = 0;
    for (const auto& collection : collections) {
        const auto directory = scan.directory(collection);
        if (directory.empty() || !seen.insert(pathIdentity(directory)).second) continue;
        for (const auto& child : scan.children(directory)) {
            if (offered++ >= kMaximumApplications) return;
            if (isDirectoryPath(child)) candidates.push_back({child, std::nullopt, source + ": " + pathToUtf8(directory)});
        }
    }
}

std::vector<std::filesystem::path> nonSteamCollections() {
    std::vector<std::filesystem::path> locations;
#if defined(_WIN32)
    std::vector<std::filesystem::path> programs;
    for (const auto& pair : std::array<std::pair<const char*, const char*>, 3>{{
             {"ProgramFiles", R"(C:\Program Files)"}, {"ProgramFiles(x86)", R"(C:\Program Files (x86))"},
             {"ProgramW6432", R"(C:\Program Files)"}}}) {
        programs.push_back(pathFromUtf8(environmentValue(pair.first).value_or(pair.second)));
    }
    const auto drive = pathFromUtf8(environmentValue("SystemDrive").value_or("C:") + "\\");
    locations.push_back(drive / "GOG Games");
    locations.push_back(drive / "Games");
    locations.push_back(drive / "Amazon Games/Library");
    for (const auto& folder : programs) {
        locations.push_back(folder / "LucasArts");
        locations.push_back(folder / "BioWare");
        locations.push_back(folder / "GOG Games");
        locations.push_back(folder / "GOG Galaxy/Games");
    }
#else
    const auto home = homeDirectory();
    for (const char* name : {"Games", "GOG Games", "Games/Heroic", "Games/GOG Games"}) locations.push_back(home / name);
#endif
    return locations;
}

void addNonSteamCandidates(std::vector<Candidate>& candidates, Scan& scan) {
    addCollectionCandidates(candidates, scan, nonSteamCollections(), "Non-Steam install location");
}

std::vector<std::filesystem::path> knownPathsFor(GameId game) {
    const auto home = homeDirectory();
    std::vector<std::filesystem::path> values;
#if defined(_WIN32)
    if (game == GameId::Kotor1) {
        for (const char* value : {
                 R"(C:\Program Files\Steam\steamapps\common\swkotor)",
                 R"(C:\Program Files (x86)\Steam\steamapps\common\swkotor)",
                 R"(C:\Program Files\LucasArts\SWKotOR)", R"(C:\Program Files (x86)\LucasArts\SWKotOR)",
                 R"(C:\GOG Games\Star Wars - KotOR)",
                 R"(C:\Amazon Games\Library\Star Wars - Knights of the Old)"}) values.emplace_back(value);
    } else if (game == GameId::Kotor2) {
        for (const char* value : {
                 R"(C:\Program Files\Steam\steamapps\common\Knights of the Old Republic II)",
                 R"(C:\Program Files (x86)\Steam\steamapps\common\Knights of the Old Republic II)",
                 R"(C:\Program Files\LucasArts\SWKotOR2)", R"(C:\Program Files (x86)\LucasArts\SWKotOR2)",
                 R"(C:\GOG Games\Star Wars - KotOR2)"}) values.emplace_back(value);
    }
#elif defined(__APPLE__)
    if (game == GameId::Kotor1) {
        values.push_back(home / "Library/Application Support/Steam/steamapps/common/swkotor/Knights of the Old Republic.app/Contents/Assets");
        values.push_back(home / "Library/Applications/Steam/steamapps/common/swkotor/Knights of the Old Republic.app/Contents/Assets");
    } else if (game == GameId::Kotor2) {
        values.push_back(home / "Library/Application Support/Steam/steamapps/common/Knights of the Old Republic II/Knights of the Old Republic II.app/Contents/Assets");
        values.push_back(home / "Library/Applications/Steam/steamapps/common/Knights of the Old Republic II/Star Wars™: Knights of the Old Republic II.app/Contents/GameData");
        values.push_back(home / "Library/Application Support/Steam/steamapps/common/Knights of the Old Republic II/KOTOR2.app/Contents/GameData");
        values.emplace_back("/Applications/Knights of the Old Republic 2.app/Contents/Resources/transgaming/c_drive/Program Files/SWKotOR2");
    }
#else
    if (game == GameId::Kotor1) {
        for (const char* relative : {
                 ".local/share/Steam/steamapps/common/swkotor", ".steam/steam/steamapps/common/swkotor",
                 ".local/share/steam/common/swkotor", ".steam/debian-installation/steamapps/common/swkotor",
                 ".steam/root/steamapps/common/swkotor",
                 ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/swkotor",
                 ".var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/swkotor"}) values.push_back(home / relative);
    } else if (game == GameId::Kotor2) {
        for (const char* relative : {
                 ".local/share/Steam/steamapps/common/Knights of the Old Republic II",
                 ".local/share/Steam/steamapps/common/kotor2", ".local/share/aspyr-media/kotor2",
                 ".local/share/aspyr-media/Knights of the Old Republic II",
                 ".steam/debian-installation/steamapps/common/Knights of the Old Republic II",
                 ".steam/debian-installation/steamapps/common/kotor2",
                 ".steam/root/steamapps/common/Knights of the Old Republic II",
                 ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Knights of the Old Republic II/steamassets",
                 ".var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Knights of the Old Republic II"}) values.push_back(home / relative);
    }
#endif
    return values;
}

#if defined(_WIN32)
std::optional<std::filesystem::path> readRegistryString(HKEY root, const std::wstring& subkey,
                                                        const std::wstring& name, REGSAM view) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) return std::nullopt;
    DWORD type = 0;
    DWORD bytes = 0;
    LONG result = RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &bytes);
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    result = RegQueryValueExW(key, name.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(value.data()), &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) return std::nullopt;
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    if (type == REG_EXPAND_SZ && !value.empty()) {
        const DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (needed > 0) {
            std::wstring expanded(static_cast<std::size_t>(needed), L'\0');
            const DWORD written = ExpandEnvironmentStringsW(value.c_str(), expanded.data(), needed);
            if (written > 0 && written <= needed) {
                while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
                value = std::move(expanded);
            }
        }
    }
    return value.empty() ? std::nullopt : std::optional<std::filesystem::path>(value);
}

void addWindowsRegistryCandidates(std::vector<Candidate>& candidates, Scan& scan) {
    struct RegistryItem { GameId game; const wchar_t* key; const wchar_t* value; };
    const std::array<RegistryItem, 6> items{{
        {GameId::Kotor1, L"Software\\BioWare\\SW\\KOTOR", L"Path"},
        {GameId::Kotor1, L"Software\\LucasArts\\KotOR", L"Path"},
        {GameId::Kotor2, L"Software\\Obsidian\\Star Wars KOTOR2", L"Path"},
        {GameId::Kotor2, L"Software\\LucasArts\\KotOR2", L"Path"},
        {GameId::Kotor1, L"Software\\GOG.com\\Games\\1207666283", L"path"},
        {GameId::Kotor2, L"Software\\GOG.com\\Games\\1421404581", L"path"},
    }};
    for (const auto& item : items) {
        for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            for (REGSAM view : {REGSAM(0), KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
                const auto path = readRegistryString(hive, item.key, item.value, view);
                if (path) addCandidate(candidates, scan, *path, item.game, "Windows registry");
            }
        }
    }
}
#else
void addWindowsRegistryCandidates(std::vector<Candidate>&, Scan&) {}
#endif

void addMacApplicationCandidates(std::vector<Candidate>& candidates, Scan& scan, const DiscoveryOptions& options) {
#if defined(__APPLE__)
    std::deque<std::pair<std::filesystem::path, std::size_t>> pending;
    std::vector<std::filesystem::path> roots = options.applicationRoots;
    if (options.useDefaultApplicationRoots) {
        roots.push_back(std::filesystem::path("/Applications"));
        roots.push_back(homeDirectory() / "Applications");
    }
    for (const auto& root : roots) {
        const auto directory = scan.directory(root);
        if (!directory.empty()) pending.emplace_back(directory, 0u);
    }
    std::set<std::string> seen;
    std::size_t offered = 0;
    while (!pending.empty() && seen.size() < 64u && offered < kMaximumApplications) {
        const auto [directory, depth] = pending.front();
        pending.pop_front();
        if (!seen.insert(pathIdentity(directory)).second) continue;
        for (const auto& child : scan.children(directory)) {
            if (offered++ >= kMaximumApplications) return;
            std::error_code ec;
            if (std::filesystem::is_symlink(child, ec) || ec || !std::filesystem::is_directory(child, ec) || ec) continue;
            const std::string name = lowerAscii(pathToUtf8(child.filename()));
            if (name.size() >= 4u && name.substr(name.size() - 4u) == ".app") {
                candidates.push_back({child, std::nullopt, "Applications: " + pathToUtf8(directory)});
            } else if (depth == 0u) {
                pending.emplace_back(child, depth + 1u);
            }
        }
    }
#else
    (void)candidates;
    (void)scan;
    (void)options;
#endif
}

std::vector<std::filesystem::path> winePrefixSeeds(const DiscoveryOptions& options) {
    std::vector<std::filesystem::path> values = options.winePrefixes;
    if (!options.useDefaultWinePrefixes) return values;
    if (const auto prefix = environmentValue("WINEPREFIX")) values.push_back(pathFromUtf8(*prefix));
    const auto home = homeDirectory();
    values.push_back(home / ".wine");
#if defined(__linux__)
    values.push_back(xdgPath("XDG_DATA_HOME", "~/.local/share") / "bottles/bottles");
    values.push_back(home / ".var/app/com.usebottles.bottles/data/bottles/bottles");
    values.push_back(home / ".PlayOnLinux/wineprefix");
    values.push_back(home / ".cxoffice");
#elif defined(__APPLE__)
    values.push_back(home / "Library/Application Support/CrossOver/Bottles");
    values.push_back(home / "Library/PlayOnMac/wineprefix");
#endif
    values.push_back(home / "Games/Heroic/Prefixes");
    values.push_back(home / "Games/Heroic/Prefixes/default");
    return values;
}


std::string wineUnescape(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    const auto appendUtf8 = [&](std::uint32_t codePoint) {
        if (codePoint <= 0x7Fu) output.push_back(static_cast<char>(codePoint));
        else if (codePoint <= 0x7FFu) {
            output.push_back(static_cast<char>(0xC0u | (codePoint >> 6u)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        } else if (codePoint <= 0xFFFFu) {
            output.push_back(static_cast<char>(0xE0u | (codePoint >> 12u)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        } else if (codePoint <= 0x10FFFFu) {
            output.push_back(static_cast<char>(0xF0u | (codePoint >> 18u)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        }
    };
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1u >= value.size()) {
            output.push_back(value[i]);
            continue;
        }
        const char escaped = value[++i];
        if (escaped == 'x') {
            std::size_t end = i + 1u;
            while (end < value.size() && end <= i + 4u &&
                   std::isxdigit(static_cast<unsigned char>(value[end])) != 0) ++end;
            if (end == i + 1u) { output.push_back('x'); continue; }
            std::uint32_t codePoint = 0;
            for (std::size_t pos = i + 1u; pos < end; ++pos) {
                const char digit = value[pos];
                codePoint = codePoint * 16u + static_cast<std::uint32_t>(
                    digit >= '0' && digit <= '9' ? digit - '0' :
                    digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 : digit - 'A' + 10);
            }
            appendUtf8(codePoint);
            i = end - 1u;
            continue;
        }
        if (escaped >= '0' && escaped <= '7') {
            std::uint32_t codePoint = static_cast<std::uint32_t>(escaped - '0');
            std::size_t digits = 1u;
            while (digits < 3u && i + 1u < value.size() && value[i + 1u] >= '0' && value[i + 1u] <= '7') {
                codePoint = codePoint * 8u + static_cast<std::uint32_t>(value[++i] - '0');
                ++digits;
            }
            appendUtf8(codePoint);
            continue;
        }
        switch (escaped) {
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        default: output.push_back(escaped); break;
        }
    }
    return output;
}

std::string removeWow6432Node(std::string key) {
    const std::string marker = "\\wow6432node";
    std::size_t offset = 0;
    while ((offset = key.find(marker, offset)) != std::string::npos) key.erase(offset, marker.size());
    return key;
}

std::optional<std::filesystem::path> wineMappedPath(Scan& scan,
                                                     const std::filesystem::path& prefix,
                                                     std::string value) {
    static const std::array<std::pair<const char*, const char*>, 3> variables{{
        {"programfiles", R"(C:\Program Files)"},
        {"programfiles(x86)", R"(C:\Program Files (x86))"},
        {"systemdrive", "C:"},
    }};
    value = trimAscii(std::move(value));
    if (value.size() >= 2u && value.front() == '"' && value.back() == '"')
        value = value.substr(1u, value.size() - 2u);
    for (const auto& [name, replacement] : variables) {
        const std::string token = "%" + std::string(name) + "%";
        std::string lowered = lowerAscii(value);
        std::size_t pos = 0;
        while ((pos = lowered.find(token, pos)) != std::string::npos) {
            value.replace(pos, token.size(), replacement);
            lowered.replace(pos, token.size(), lowerAscii(replacement));
            pos += std::char_traits<char>::length(replacement);
        }
    }
    if (value.empty() || value.find('%') != std::string::npos ||
        value.find('\0') != std::string::npos || value.find('\r') != std::string::npos ||
        value.find('\n') != std::string::npos) return std::nullopt;
    if (value.size() < 3u || std::isalpha(static_cast<unsigned char>(value[0])) == 0 ||
        value[1] != ':' || (value[2] != '\\' && value[2] != '/')) return std::nullopt;
    const char driveLetter = static_cast<char>(std::tolower(static_cast<unsigned char>(value[0])));
    std::filesystem::path drive = scan.directory(prefix / "dosdevices" / (std::string(1u, driveLetter) + ":"));
    if (drive.empty() && driveLetter == 'c') drive = scan.directory(prefix / "drive_c");
    if (drive.empty()) return std::nullopt;
    std::filesystem::path mapped = drive;
    std::string component;
    const auto append = [&]() -> bool {
        if (component.empty() || component == ".") { component.clear(); return true; }
        if (component == "..") return false;
        mapped /= pathFromUtf8(component);
        component.clear();
        return true;
    };
    for (std::size_t i = 3u; i <= value.size(); ++i) {
        if (i == value.size() || value[i] == '\\' || value[i] == '/') {
            if (!append()) return std::nullopt;
        } else component.push_back(value[i]);
    }
    const auto directory = scan.directory(mapped);
    return directory.empty() ? std::nullopt : std::optional<std::filesystem::path>(directory);
}

void addWineRegistryCandidates(std::vector<Candidate>& candidates, Scan& scan,
                               const std::filesystem::path& prefix) {
    std::map<std::string, GameId> productKeys;
    for (const auto& game : knownGames()) {
        for (const auto& encoded : game.registryValues) {
            const auto separator = encoded.rfind('/');
            std::string key = separator == std::string::npos ? encoded : encoded.substr(0u, separator);
            const auto firstSlash = key.find('\\');
            if (firstSlash != std::string::npos) key.erase(0u, firstSlash + 1u);
            productKeys[removeWow6432Node(lowerAscii(key))] = game.game;
        }
        if (!game.gogAppId.empty()) {
            productKeys["software\\microsoft\\windows\\currentversion\\uninstall\\" +
                        lowerAscii(game.gogAppId) + "_is1"] = game.game;
        }
    }
    productKeys["software\\microsoft\\windows\\currentversion\\uninstall\\amazongames/star wars - knights of the old"] = GameId::Kotor1;
    const std::string uninstall = "software\\microsoft\\windows\\currentversion\\uninstall\\";
    const std::regex header(R"(^\[((?:\\.|[^\]\\])*)\])");
    const std::regex setting(R"REG(^"((?:\\.|[^"\\])*)"=(?:str\(2\):)?"((?:\\.|[^"\\])*)"\s*$)REG");

    for (const char* filename : {"system.reg", "user.reg"}) {
        const auto metadata = scan.file(prefix, filename);
        if (metadata.empty() || scan.registryBytesLeft == 0u) continue;
        std::ifstream input(metadata, std::ios::binary);
        if (!input) continue;
        std::string key;
        std::map<std::string, std::string> fields;
        const auto emit = [&]() {
            std::optional<GameId> game;
            if (const auto it = productKeys.find(key); it != productKeys.end()) game = it->second;
            const std::string display = lowerAscii(fields.count("displayname") ? fields.at("displayname") : std::string{});
            if (!game && !(key.rfind(uninstall, 0u) == 0u &&
                           display.find("knights of the old republic") != std::string::npos)) return;
            for (const char* field : {"path", "internalpath", "installlocation"}) {
                const auto it = fields.find(field);
                if (it == fields.end()) continue;
                if (const auto directory = wineMappedPath(scan, prefix, it->second)) {
                    addCandidate(candidates, scan, *directory, game,
                                 "Wine registry: " + pathToUtf8(metadata) + " [" + key + "]");
                }
            }
        };
        std::size_t consumed = 0u;
        std::string line;
        while (consumed < kMaximumRegistryBytes && scan.registryBytesLeft > 0u) {
            const std::size_t limit = std::min({std::size_t(65536u), kMaximumRegistryBytes - consumed,
                                                scan.registryBytesLeft});
            if (limit == 0u) break;
            line.clear();
            char ch = '\0';
            bool complete = false;
            while (line.size() < limit && input.get(ch)) {
                ++consumed;
                --scan.registryBytesLeft;
                if (ch == '\n') { complete = true; break; }
                line.push_back(ch);
            }
            if (line.size() >= 65536u && !complete) break;
            if (!input && line.empty()) { emit(); break; }
            line = trimAscii(std::move(line));
            std::smatch match;
            if (std::regex_search(line, match, header)) {
                emit();
                key = removeWow6432Node(lowerAscii(wineUnescape(match[1].str())));
                fields.clear();
            } else if (productKeys.count(key) != 0u || key.rfind(uninstall, 0u) == 0u) {
                if (std::regex_match(line, match, setting)) {
                    const std::string field = lowerAscii(wineUnescape(match[1].str()));
                    if (field == "path" || field == "internalpath" || field == "installlocation" ||
                        field == "displayname") fields[field] = wineUnescape(match[2].str());
                }
            }
            if (!input) { emit(); break; }
        }
    }
}

void addWineCollectionCandidates(std::vector<Candidate>& candidates, Scan& scan,
                                 const std::filesystem::path& drive,
                                 const std::string& source) {
    std::vector<std::filesystem::path> collections = {
        drive / "GOG Games", drive / "Games", drive / "Amazon Games/Library",
        drive / "Program Files/LucasArts", drive / "Program Files (x86)/LucasArts",
        drive / "Program Files/BioWare", drive / "Program Files (x86)/BioWare",
        drive / "Program Files/GOG Games", drive / "Program Files (x86)/GOG Games",
        drive / "Program Files/GOG Galaxy/Games", drive / "Program Files (x86)/GOG Galaxy/Games",
    };
    addCollectionCandidates(candidates, scan, collections, source);
    for (const auto& parent : {drive, drive / "Program Files", drive / "Program Files (x86)"}) {
        for (const char* name : {"SWKotOR", "SWKotOR2", "KotOR", "KotOR2"}) {
            addCandidate(candidates, scan, parent / name, std::nullopt, source);
        }
    }
}

void addWineCandidates(std::vector<Candidate>& candidates, Scan& scan, const DiscoveryOptions& options) {
#if defined(__linux__) || defined(__APPLE__)
    std::vector<std::filesystem::path> prefixes;
    const auto considerPrefix = [&](const std::filesystem::path& value) {
        if (prefixes.size() >= kMaximumPrefixes) return;
        const auto directory = scan.directory(value);
        if (directory.empty()) return;
        const bool hasDrive = !scan.directory(directory / "drive_c").empty();
        const bool hasRegistry = !scan.file(directory, "system.reg").empty() ||
                                 !scan.file(directory, "user.reg").empty();
        if ((hasDrive || hasRegistry) &&
            std::none_of(prefixes.begin(), prefixes.end(), [&](const auto& existing) {
                return pathIdentity(existing) == pathIdentity(directory);
            })) prefixes.push_back(directory);
    };
    for (const auto& seed : winePrefixSeeds(options)) {
        const auto directory = scan.directory(seed);
        if (directory.empty()) continue;
        for (const char* suffix : {"", "pfx", "prefix", "Contents/Resources",
                                   "Contents/Resources/wineprefix", "Contents/SharedSupport/prefix"}) {
            considerPrefix(suffix[0] == '\0' ? directory : directory / suffix);
        }
        for (const auto& child : scan.children(directory)) {
            if (prefixes.size() >= kMaximumPrefixes) break;
            for (const char* suffix : {"", "pfx", "prefix", "Contents/Resources",
                                       "Contents/Resources/wineprefix", "Contents/SharedSupport/prefix"}) {
                considerPrefix(suffix[0] == '\0' ? child : child / suffix);
            }
        }
    }
    for (const auto& prefix : prefixes) {
        addWineRegistryCandidates(candidates, scan, prefix);
        const auto drive = scan.directory(prefix / "drive_c");
        if (!drive.empty()) addWineCollectionCandidates(candidates, scan, drive, "Wine location: " + pathToUtf8(prefix));
    }
#else
    (void)candidates;
    (void)scan;
    (void)options;
#endif
}

void addKnownCandidates(std::vector<Candidate>& candidates, Scan& scan) {
    for (const auto game : {GameId::Kotor1, GameId::Kotor2}) {
        for (const auto& path : knownPathsFor(game)) addCandidate(candidates, scan, path, game, "Known location");
    }
}

void addEnvironmentCandidates(std::vector<Candidate>& candidates, Scan& scan) {
    struct EnvironmentCandidate {
        GameId game;
        std::array<const char*, 3> variables;
    };
    static constexpr std::array<EnvironmentCandidate, 2> entries{{
        {GameId::Kotor1, {"K1_PATH", "KOTOR_PATH", "KOTOR1_PATH"}},
        {GameId::Kotor2, {"K2_PATH", "TSL_PATH", "KOTOR2_PATH"}},
    }};
    for (const auto& entry : entries) {
        for (const char* variable : entry.variables) {
            const auto value = environmentValue(variable);
            if (!value || value->empty()) continue;
            addCandidate(candidates, scan, pathFromUtf8(*value), entry.game,
                         std::string("Environment: ") + variable);
        }
    }
}

void addGenericCandidates(std::vector<Candidate>& candidates, Scan& scan, const DiscoveryOptions& options) {
    std::vector<std::filesystem::path> launcherCollections;
    for (const auto& steam : steamRoots(options)) launcherCollections.push_back(steam / "steamapps/common");
    const auto other = nonSteamCollections();
    launcherCollections.insert(launcherCollections.end(), other.begin(), other.end());
    for (const auto& game : knownGames()) {
        if (game.game == GameId::Kotor1 || game.game == GameId::Kotor2) continue;
        for (const auto& collection : launcherCollections) {
            for (const auto& name : game.commonDirectoryNames) {
                addCandidate(candidates, scan, collection / pathFromUtf8(name), game.game,
                             "Known launcher location");
            }
        }
    }
}

std::vector<GameInstall> materializeCandidates(const std::vector<Candidate>& candidates, Scan& scan) {
    struct Aggregate {
        std::filesystem::path foundAt;
        std::filesystem::path root;
        std::set<GameId> games;
        std::set<std::string> sources;
        std::set<std::string> evidence;
    };

    std::map<std::string, std::vector<std::filesystem::path>> rootsCache;
    std::map<std::string, Aggregate> found;
    for (const auto& candidate : candidates) {
        const std::string candidateId = pathIdentity(candidate.path);
        auto rootsIt = rootsCache.find(candidateId);
        if (rootsIt == rootsCache.end()) {
            auto roots = scan.dataRoots(candidate.path);
            if (roots.empty()) {
                bool directInstall = false;
                if (candidate.game) {
                    if (const auto* definition = findGame(*candidate.game)) {
                        directInstall = rootHasRequiredFile(*definition, candidate.path);
                    }
                } else {
                    directInstall = !scan.identify(candidate.path).first.empty();
                }
                if (directInstall) roots.push_back(candidate.path);
            }
            rootsIt = rootsCache.emplace(candidateId, std::move(roots)).first;
        }

        for (const auto& root : rootsIt->second) {
            auto [games, evidence] = scan.identify(root);
            if (candidate.game) {
                const auto* definition = findGame(*candidate.game);
                if (definition == nullptr || !rootHasRequiredFile(*definition, root)) continue;
                // A game-specific executable/configuration file is stronger than
                // launcher metadata when the two disagree. A canonical root with
                // no local identity evidence may use the launcher/registry hint.
                if (!games.empty() && games.count(*candidate.game) == 0u) continue;
                games.clear();
                games.insert(*candidate.game);
                evidence.push_back(gameIdString(*candidate.game) + ": " + candidate.source);
            }
            if (games.empty()) continue;

            const std::string rootId = pathIdentity(root);
            auto [it, inserted] = found.try_emplace(rootId);
            if (inserted) {
                it->second.foundAt = candidate.path;
                it->second.root = root;
            }
            it->second.games.insert(games.begin(), games.end());
            it->second.sources.insert(candidate.source);
            it->second.evidence.insert(evidence.begin(), evidence.end());
        }
    }

    std::vector<GameInstall> results;
    for (auto& [identity, aggregate] : found) {
        (void)identity;
        if (aggregate.games.size() != 1u) continue;
        const GameId gameId = *aggregate.games.begin();
        const GameDefinition* game = findGame(gameId);
        if (game == nullptr || !rootHasRequiredFile(*game, aggregate.root)) continue;
        GameInstall install = makeInstall(*game, aggregate.root, true, false,
                                          validationScore(*game, aggregate.root));
        install.foundAt = aggregate.foundAt;
        install.sources.assign(aggregate.sources.begin(), aggregate.sources.end());
        install.evidence.assign(aggregate.evidence.begin(), aggregate.evidence.end());
        const std::string store = storefrontLabel(install.installPath, install.sources);
        if (!store.empty()) install.displayName = game->displayName + " (" + store + ")";
        refreshDerivedPaths(*game, install);
        upsertInstall(results, *game, std::move(install));
    }
    std::stable_sort(results.begin(), results.end(), [](const GameInstall& lhs, const GameInstall& rhs) {
        if (lhs.id != rhs.id) return lhs.id < rhs.id;
        return pathToUtf8(lhs.installPath) < pathToUtf8(rhs.installPath);
    });
    return results;
}

std::vector<GameId> normalizedCompatibleGames(const ResourceGameContext& context) {
    std::vector<GameId> out;
    for (const GameId game : context.compatibleGames) {
        if (game == GameId::Unknown) continue;
        if (std::find(out.begin(), out.end(), game) == out.end()) out.push_back(game);
    }
    return out;
}

bool gameAllowed(GameId game, const std::vector<GameId>& compatible) {
    return compatible.empty() || std::find(compatible.begin(), compatible.end(), game) != compatible.end();
}

std::size_t pathLength(const std::filesystem::path& path) {
    return genericPathToUtf8(normalizeConfiguredPath(path)).size();
}

} // namespace

const std::vector<GameDefinition>& knownGames() {
    static const std::vector<GameDefinition> games = {
        {GameId::Kotor1, "kotor", "Star Wars: Knights of the Old Republic",
         {"chitin.key", "dialog.tlk"},
         {"swkotor.exe", "swkotor", "swkotor.ini", "goggame-1207666283.info"},
         {"dialog.tlk"}, {"Override", "override"}, {"data", "Data"},
         {"swkotor", "Knights of the Old Republic", "STAR WARS Knights of the Old Republic",
          "Star Wars - KotOR", "Star Wars - Knights of the Old Republic", "Star Wars Knights of the Old Republic"},
         {"Software\\BioWare\\SW\\KOTOR/Path", "Software\\LucasArts\\KotOR/Path"}, "32370", "1207666283",
         {"chitin.key"},
         {"swkotor.exe", "swkotor", "swkotor.ini", "goggame-1207666283.info"}},
        {GameId::Kotor2, "kotor2", "Star Wars: Knights of the Old Republic II",
         {"chitin.key", "dialog.tlk"},
         {"swkotor2.exe", "swkotor2", "swkotor2.ini", "goggame-1421404581.info"},
         {"dialog.tlk"}, {"Override", "override"}, {"data", "Data"},
         {"Knights of the Old Republic II", "Knights of the Old Republic 2", "swkotor2", "kotor2",
          "STAR WARS Knights of the Old Republic II", "Star Wars - KotOR2", "Star Wars Knights of the Old Republic II"},
         {"Software\\Obsidian\\Star Wars KOTOR2/Path", "Software\\LucasArts\\KotOR2/Path"}, "208580", "1421404581",
         {"chitin.key"},
         {"swkotor2.exe", "swkotor2", "swkotor2.ini", "goggame-1421404581.info"}},
        {GameId::JadeEmpire, "jade", "Jade Empire",
         {"chitin.key", "dialog.tlk"},
         {"JadeEmpire.exe", "Jade Empire.exe", "JadeEmpireLauncher.exe"},
         {"dialog.tlk"}, {"override", "Override"}, {"data", "Data"},
         {"Jade Empire", "Jade Empire Special Edition"},
         {"Software\\BioWare\\Jade Empire/Path"}, {}, {},
         {"chitin.key"},
         {"JadeEmpire.exe", "Jade Empire.exe", "JadeEmpireLauncher.exe"}},
        {GameId::NeverwinterNights, "nwn", "Neverwinter Nights",
         {"chitin.key", "dialog.tlk"},
         {"nwn.exe", "nwmain.exe", "nwn", "nwn.ini"},
         {"dialog.tlk"}, {"override", "Override"}, {"data", "Data", "modules", "Modules"},
         {"Neverwinter Nights", "NeverwinterNights", "NWN"},
         {"Software\\BioWare\\NWN\\Neverwinter/Location"}, {}, {},
         {"chitin.key"},
         {"nwn.exe", "nwmain.exe", "nwn", "nwn.ini"}},
        {GameId::NeverwinterNights2, "nwn2", "Neverwinter Nights 2",
         {"chitin.key", "dialog.tlk"},
         {"nwn2main.exe", "nwn2.exe", "nwn2main", "nwn2.ini"},
         {"dialog.tlk"}, {"Override", "override"}, {"Data", "data", "modules", "Modules"},
         {"Neverwinter Nights 2", "NeverwinterNights2", "NWN2"},
         {"Software\\Obsidian\\NWN 2\\Neverwinter/Location"}, {}, {},
         {"chitin.key"},
         {"nwn2main.exe", "nwn2.exe", "nwn2main", "nwn2.ini"}},
        {GameId::Witcher1, "witcher1", "The Witcher",
         {"System/witcher.exe", "System/witcher"},
         {"Data/dialogues/dialog.tlk"},
         {"Data/dialogues/dialog.tlk", "Data/dialog.tlk", "dialog.tlk"},
         {"Override", "override"}, {"Data", "data"},
         {"The Witcher", "The Witcher Enhanced Edition"},
         {"Software\\CD Projekt Red\\The Witcher/InstallFolder"}, {}, {},
         {"System/witcher.exe", "System/witcher"},
         {"System/witcher.exe", "System/witcher"}},
        {GameId::DragonAgeOrigins, "dao", "Dragon Age: Origins",
         {"bin_ship/daorigins.exe", "bin_ship/daorigins"},
         {"packages/core/data/talktables/dialog.tlk"},
         {"modules/Single Player/data/talktables/dialog.tlk", "modules/single player/data/talktables/dialog.tlk",
          "packages/core/data/talktables/dialog.tlk", "dialog.tlk"},
         {"packages/core/override", "packages/core/Override", "override", "Override"},
         {"packages/core/data", "packages/core/Data"},
         {"Dragon Age Origins", "Dragon Age Ultimate Edition", "Dragon Age"},
         {"Software\\BioWare\\Dragon Age/Path"}, {}, {},
         {"bin_ship/daorigins.exe", "bin_ship/daorigins"},
         {"bin_ship/daorigins.exe", "bin_ship/daorigins"}},
        {GameId::DragonAge2, "da2", "Dragon Age II",
         {"bin_ship/DragonAge2.exe", "bin_ship/DragonAge2"},
         {"packages/core/data/talktables/core_en-us.tlk"},
         {"modules/single player/data/talktables/core_en-us.tlk", "modules/Single Player/data/talktables/core_en-us.tlk",
          "packages/core/data/talktables/core_en-us.tlk", "dialog.tlk"},
         {"packages/core/override", "packages/core/Override", "override", "Override"},
         {"packages/core/data", "packages/core/Data"},
         {"Dragon Age II", "Dragon Age 2"},
         {"Software\\BioWare\\Dragon Age 2/Path"}, {}, {},
         {"bin_ship/DragonAge2.exe", "bin_ship/DragonAge2"},
         {"bin_ship/DragonAge2.exe", "bin_ship/DragonAge2"}},
    };
    return games;
}

const GameDefinition* findGame(GameId game) noexcept {
    const auto& games = knownGames();
    const auto it = std::find_if(games.begin(), games.end(), [&](const GameDefinition& value) { return value.game == game; });
    return it == games.end() ? nullptr : &*it;
}

const GameDefinition* findGame(std::string_view id) noexcept {
    const auto& games = knownGames();
    const auto it = std::find_if(games.begin(), games.end(), [&](const GameDefinition& value) { return value.id == id; });
    return it == games.end() ? nullptr : &*it;
}

std::optional<GameId> gameIdFromString(std::string_view id) noexcept {
    const auto* game = findGame(id);
    return game == nullptr ? std::nullopt : std::optional<GameId>(game->game);
}

std::string gameIdString(GameId game) {
    const auto* definition = findGame(game);
    return definition == nullptr ? std::string{} : definition->id;
}

bool existsPath(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec) && !ec;
}

bool isDirectoryPath(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_directory(path, ec) && !ec;
}

bool isRegularFilePath(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec) && !ec;
}

std::filesystem::path findExistingPathCaseInsensitive(const std::filesystem::path& supplied) {
    if (supplied.empty()) return {};
    auto path = expandPathText(supplied);
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec) return normalizeLexically(path);
    if (!path.is_absolute()) {
        path = std::filesystem::absolute(path, ec);
        if (ec) return {};
    }
    std::filesystem::path current = path.root_path();
    const auto relative = path.relative_path();
    for (const auto& component : relative) {
        current = caseAwareChild(current, pathToUtf8(component));
        if (current.empty()) return {};
    }
    return normalizeLexically(current);
}

std::filesystem::path normalizeConfiguredPath(const std::filesystem::path& supplied) {
    if (supplied.empty()) return {};
    const auto expanded = expandPathText(supplied);
    const auto existing = findExistingPathCaseInsensitive(expanded);
    return existing.empty() ? normalizeLexically(expanded) : existing;
}

std::filesystem::path firstExisting(const std::filesystem::path& root,
                                    const std::vector<std::string>& relatives) {
    const auto normalizedRoot = normalizeConfiguredPath(root);
    for (const auto& relative : relatives) {
        const auto candidate = caseAwareRelative(normalizedRoot, pathFromUtf8(relative));
        if (existsPath(candidate)) return normalizeLexically(candidate);
    }
    return {};
}

bool pathStartsWith(const std::filesystem::path& child, const std::filesystem::path& root) {
    if (child.empty() || root.empty()) return false;
    const auto normalizedChild = normalizeConfiguredPath(child);
    const auto normalizedRoot = normalizeConfiguredPath(root);
    auto childIt = normalizedChild.begin();
    for (auto rootIt = normalizedRoot.begin(); rootIt != normalizedRoot.end(); ++rootIt, ++childIt) {
        if (childIt == normalizedChild.end()) return false;
#if defined(_WIN32)
        if (!asciiEqual(pathToUtf8(*rootIt), pathToUtf8(*childIt))) return false;
#else
        if (*rootIt != *childIt) return false;
#endif
    }
    return true;
}

bool installContainsPath(const GameInstall& install, const std::filesystem::path& path) {
    const GameDefinition* game = findGame(install.id);
    const bool validRoot = game != nullptr &&
        isValidGameInstallation(*game, install.installPath);
    if (validRoot && pathStartsWith(path, install.installPath)) return true;
    if ((validRoot || install.explicitTlk) && !install.tlkPath.empty() &&
        normalizeConfiguredPath(path) == normalizeConfiguredPath(install.tlkPath)) return true;
    if (validRoot && !install.overridePath.empty() &&
        pathStartsWith(path, install.overridePath)) return true;
    if (validRoot && !install.dataRootPath.empty() &&
        pathStartsWith(path, install.dataRootPath)) return true;
    return false;
}

bool hasRequiredInstallationFile(const GameDefinition& game,
                                 const std::filesystem::path& root) {
    return rootHasRequiredFile(game, normalizeConfiguredPath(root));
}

bool hasGameIdentityFile(const GameDefinition& game,
                         const std::filesystem::path& root) {
    return rootHasIdentityFile(game, normalizeConfiguredPath(root));
}

bool isValidGameInstallation(const GameDefinition& game,
                             const std::filesystem::path& root) {
    const auto normalized = normalizeConfiguredPath(root);
    return isDirectoryPath(normalized) && rootHasRequiredFile(game, normalized);
}

bool isUsableGameInstall(const GameDefinition& game,
                         const GameInstall& install) {
    return isValidGameInstallation(game, install.installPath) ||
           (install.explicitTlk && isRegularFilePath(install.tlkPath));
}

std::string installationRequirementText(const GameDefinition& game) {
    if (game.requiredRootFileAlternatives.empty()) return "a canonical game file";
    std::ostringstream text;
    for (std::size_t i = 0; i < game.requiredRootFileAlternatives.size(); ++i) {
        if (i != 0u) text << (i + 1u == game.requiredRootFileAlternatives.size()
                              ? " or " : ", ");
        text << game.requiredRootFileAlternatives[i];
    }
    return text.str();
}

int validationScore(const GameDefinition& game, const std::filesystem::path& root) {
    const auto normalized = normalizeConfiguredPath(root);
    if (!isValidGameInstallation(game, normalized)) return 0;
    int score = 5;
    if (rootHasIdentityFile(game, normalized)) score += 2;
    if (!firstRegularFile(normalized, game.tlkRelativePaths).empty()) score += 2;
    return score;
}

std::string confidenceText(int confidence, bool userOverride) {
    if (userOverride) return "user";
    if (confidence >= 7) return "high";
    if (confidence >= 4) return "medium";
    if (confidence >= 2) return "low";
    return "none";
}

std::string defaultInstallName(const GameDefinition& game, const std::filesystem::path& root) {
    const std::string source = storefrontLabel(root);
    if (!source.empty()) return game.displayName + " (" + source + ")";
    const std::string leaf = root.empty() ? std::string{} : pathToUtf8(root.filename());
    if (!leaf.empty() && leaf != "." && leaf != "..") return game.displayName + " - " + leaf;
    return game.displayName;
}

std::string makeInstallId(const GameDefinition& game, const std::filesystem::path& root,
                          const std::filesystem::path& tlkPath) {
    const std::string seed = game.id + "|" + genericPathToUtf8(normalizeConfiguredPath(root)) + "|" +
                             genericPathToUtf8(normalizeConfiguredPath(tlkPath));
    return "i_" + hexValue(fnv1a64(seed));
}

void refreshDerivedPaths(const GameDefinition& game, GameInstall& install) {
    install.id = game.id;
    install.installPath = normalizeConfiguredPath(install.installPath);
    install.tlkPath = normalizeConfiguredPath(install.tlkPath);
    install.overridePath = normalizeConfiguredPath(install.overridePath);
    install.dataRootPath = normalizeConfiguredPath(install.dataRootPath);
    install.foundAt = normalizeConfiguredPath(install.foundAt);

    const bool rootValid = isValidGameInstallation(game, install.installPath);
    if (rootValid) {
        if (!install.explicitTlk) {
            install.tlkPath = firstRegularFile(install.installPath, game.tlkRelativePaths);
        }
        if (!isDirectoryPath(install.overridePath)) {
            install.overridePath = firstExisting(install.installPath, game.overrideRelativePaths);
        }
        if (!isDirectoryPath(install.dataRootPath)) {
            install.dataRootPath = firstExisting(install.installPath, game.dataRelativePaths);
        }
    } else {
        if (!install.explicitTlk) install.tlkPath.clear();
        install.overridePath.clear();
        install.dataRootPath.clear();
    }

    install.confidence = std::max(install.confidence,
                                  validationScore(game, install.installPath));
    if (install.userOverride) install.confidence = std::max(install.confidence, 8);
    if (install.installId.empty()) {
        install.installId = makeInstallId(game, install.installPath, install.tlkPath);
    }
    if (install.displayName.empty()) {
        install.displayName = defaultInstallName(game, install.installPath);
    }

    const bool tlkValid = isRegularFilePath(install.tlkPath);
    if (rootValid) {
        install.status = !tlkValid
            ? "TLK missing" : confidenceText(install.confidence, install.userOverride);
    } else if (install.explicitTlk && tlkValid) {
        install.status = "TLK only";
    } else if (!install.installPath.empty() || !install.tlkPath.empty()) {
        install.status = "invalid install";
    } else {
        install.status = "not found";
    }
}

GameInstall makeInstall(const GameDefinition& game, const std::filesystem::path& root,
                        bool detected, bool userOverride, int confidence,
                        std::string displayName, std::string installId) {
    GameInstall install;
    install.id = game.id;
    install.installId = std::move(installId);
    install.displayName = std::move(displayName);
    install.installPath = normalizeConfiguredPath(root);
    install.detected = detected;
    install.userOverride = userOverride;
    install.confidence = confidence;
    refreshDerivedPaths(game, install);
    return install;
}

bool sameInstallTarget(const GameInstall& lhs, const GameInstall& rhs) {
    if (!lhs.installId.empty() && lhs.installId == rhs.installId) return true;
    if (!lhs.installPath.empty() && !rhs.installPath.empty() &&
        pathIdentity(normalizeConfiguredPath(lhs.installPath)) == pathIdentity(normalizeConfiguredPath(rhs.installPath))) return true;
    if (!lhs.tlkPath.empty() && !rhs.tlkPath.empty() &&
        pathIdentity(normalizeConfiguredPath(lhs.tlkPath)) == pathIdentity(normalizeConfiguredPath(rhs.tlkPath))) return true;
    return false;
}

void mergeInstall(GameInstall& existing, const GameInstall& incoming) {
    if (existing.installId.empty()) existing.installId = incoming.installId;
    if (incoming.userOverride || existing.displayName.empty()) {
        if (!incoming.displayName.empty()) existing.displayName = incoming.displayName;
    }
    if (!incoming.installPath.empty()) existing.installPath = incoming.installPath;
    if (incoming.explicitTlk) {
        existing.tlkPath = incoming.tlkPath;
        existing.explicitTlk = true;
    } else if (!existing.explicitTlk && !incoming.tlkPath.empty()) {
        existing.tlkPath = incoming.tlkPath;
    }
    if (!incoming.overridePath.empty() && existing.overridePath.empty()) existing.overridePath = incoming.overridePath;
    if (!incoming.dataRootPath.empty() && existing.dataRootPath.empty()) existing.dataRootPath = incoming.dataRootPath;
    if (!incoming.foundAt.empty() && existing.foundAt.empty()) existing.foundAt = incoming.foundAt;
    existing.detected = existing.detected || incoming.detected;
    existing.userOverride = existing.userOverride || incoming.userOverride;
    existing.confidence = std::max(existing.confidence, incoming.confidence);
    existing.sources.insert(existing.sources.end(), incoming.sources.begin(), incoming.sources.end());
    existing.evidence.insert(existing.evidence.end(), incoming.evidence.begin(), incoming.evidence.end());
    std::sort(existing.sources.begin(), existing.sources.end());
    existing.sources.erase(std::unique(existing.sources.begin(), existing.sources.end()), existing.sources.end());
    std::sort(existing.evidence.begin(), existing.evidence.end());
    existing.evidence.erase(std::unique(existing.evidence.begin(), existing.evidence.end()), existing.evidence.end());
}

void upsertInstall(std::vector<GameInstall>& installs, const GameDefinition& game, GameInstall incoming) {
    refreshDerivedPaths(game, incoming);
    auto it = std::find_if(installs.begin(), installs.end(), [&](const GameInstall& existing) {
        return sameInstallTarget(existing, incoming);
    });
    if (it == installs.end()) installs.push_back(std::move(incoming));
    else {
        mergeInstall(*it, incoming);
        refreshDerivedPaths(game, *it);
    }
}

void sortInstalls(std::vector<GameInstall>& installs, const std::string& activeInstallId) {
    std::stable_sort(installs.begin(), installs.end(), [&](const GameInstall& lhs, const GameInstall& rhs) {
        const bool lhsActive = !activeInstallId.empty() && lhs.installId == activeInstallId;
        const bool rhsActive = !activeInstallId.empty() && rhs.installId == activeInstallId;
        if (lhsActive != rhsActive) return lhsActive;
        if (lhs.userOverride != rhs.userOverride) return lhs.userOverride;
        if (lhs.confidence != rhs.confidence) return lhs.confidence > rhs.confidence;
        return lowerAscii(lhs.displayName) < lowerAscii(rhs.displayName);
    });
}

std::vector<GameInstall> discoverGameInstallations(const DiscoveryOptions& options) {
    Scan scan;
    std::vector<Candidate> candidates;
    if (options.launcherMetadata) {
        addSteamCandidates(candidates, scan, options);
        addHeroicCandidates(candidates, scan);
        if (options.lutris) addLutrisCandidates(candidates, scan);
    }
    if (options.platformRegistry) {
        addWindowsRegistryCandidates(candidates, scan);
        addMacApplicationCandidates(candidates, scan, options);
    }
    if (options.knownLocations) {
        addEnvironmentCandidates(candidates, scan);
        addNonSteamCandidates(candidates, scan);
        addKnownCandidates(candidates, scan);
        addGenericCandidates(candidates, scan, options);
    }
    if (options.scanWinePrefixes) addWineCandidates(candidates, scan, options);
    for (const auto& path : options.additionalCandidates) addCandidate(candidates, scan, path, std::nullopt, "Configured candidate");
    return materializeCandidates(candidates, scan);
}

std::vector<GameInstall> discoverGameInstallations(const GameDefinition& game,
                                                    const std::optional<std::filesystem::path>& hint,
                                                    const DiscoveryOptions& options) {
    DiscoveryOptions broad = options;
    const auto configuredCandidates = broad.additionalCandidates;
    broad.additionalCandidates.clear();

    std::vector<GameInstall> filtered;
    for (auto install : discoverGameInstallations(broad)) {
        if (install.id == game.id) upsertInstall(filtered, game, std::move(install));
    }

    const auto inspectCandidate = [&](const std::filesystem::path& candidate,
                                      const std::string& source) {
        if (const auto install = inspectGameInstallation(candidate, game.game, source)) {
            upsertInstall(filtered, game, *install);
            return true;
        }
        return false;
    };
    for (const auto& candidate : configuredCandidates) {
        inspectCandidate(candidate, "Configured candidate");
    }

    if (hint && !hint->empty()) {
        auto current = normalizeConfiguredPath(*hint);
        if (!isDirectoryPath(current)) current = current.parent_path();
        while (!current.empty()) {
            if (inspectCandidate(current, "Opened resource")) break;
            const auto parent = current.parent_path();
            if (parent.empty() || parent == current) break;
            current = parent;
        }
    }
    sortInstalls(filtered);
    return filtered;
}

std::optional<GameInstall> inspectGameInstallation(const std::filesystem::path& candidate,
                                                    std::optional<GameId> gameHint,
                                                    std::string source) {
    Scan scan;
    auto normalized = normalizeConfiguredPath(candidate);
    if (!isDirectoryPath(normalized)) normalized = normalized.parent_path();
    const auto directory = scan.directory(normalized);
    if (directory.empty()) return std::nullopt;
    std::vector<Candidate> candidates{{directory, gameHint, std::move(source)}};
    auto installs = materializeCandidates(candidates, scan);
    if (gameHint) {
        const std::string id = gameIdString(*gameHint);
        const auto it = std::find_if(installs.begin(), installs.end(),
            [&](const GameInstall& install) { return install.id == id; });
        if (it != installs.end()) return *it;
        return std::nullopt;
    }
    return installs.size() == 1u
        ? std::optional<GameInstall>(installs.front()) : std::nullopt;
}

TalkTableResolution resolveTalkTable(const std::filesystem::path& resourcePath,
                                     const std::vector<GameInstall>& installations,
                                     const ResourceGameContext& context) {
    TalkTableResolution resolution;
    const auto compatible = normalizedCompatibleGames(context);
    std::vector<const GameInstall*> eligible;
    std::vector<const GameInstall*> withTalkTable;
    for (const auto& install : installations) {
        const auto game = gameIdFromString(install.id);
        if (!game) continue;
        const GameDefinition* definition = findGame(*game);
        if (definition == nullptr || !isUsableGameInstall(*definition, install)) continue;
        if (context.definitiveGame && *game != *context.definitiveGame) continue;
        if (!context.definitiveGame && !gameAllowed(*game, compatible)) continue;
        eligible.push_back(&install);
        if (isRegularFilePath(install.tlkPath)) withTalkTable.push_back(&install);
    }

    const GameInstall* containing = nullptr;
    std::size_t containingLength = 0;
    for (const auto* install : eligible) {
        if (!installContainsPath(*install, resourcePath)) continue;
        const std::size_t length = pathLength(install->installPath.empty() ? install->tlkPath : install->installPath);
        if (containing == nullptr || length > containingLength) {
            containing = install;
            containingLength = length;
        }
    }
    if (containing != nullptr) {
        resolution.installation = *containing;
        resolution.game = gameIdFromString(containing->id);
        if (isRegularFilePath(containing->tlkPath)) resolution.tlkPath = containing->tlkPath;
        else resolution.diagnostic = "The containing game installation was detected, but it has no readable talk table.";
        return resolution;
    }

    const GameInstall* chosen = nullptr;
    if (context.preferredGame) {
        const std::string preferred = gameIdString(*context.preferredGame);
        const auto it = std::find_if(withTalkTable.begin(), withTalkTable.end(), [&](const GameInstall* install) {
            return install->id == preferred;
        });
        if (it != withTalkTable.end()) chosen = *it;
    }
    if (chosen == nullptr && context.definitiveGame) {
        const std::string required = gameIdString(*context.definitiveGame);
        const auto it = std::find_if(withTalkTable.begin(), withTalkTable.end(), [&](const GameInstall* install) {
            return install->id == required;
        });
        if (it != withTalkTable.end()) chosen = *it;
    }
    if (chosen == nullptr) {
        std::set<std::string> games;
        for (const auto* install : withTalkTable) games.insert(install->id);
        if (games.size() == 1u && !withTalkTable.empty()) chosen = withTalkTable.front();
        else if (withTalkTable.size() == 1u) chosen = withTalkTable.front();
        else if (games.size() > 1u) {
            resolution.ambiguous = true;
            resolution.diagnostic = "More than one compatible game installation has a talk table; select the active game installation.";
        }
    }

    if (chosen != nullptr) {
        resolution.installation = *chosen;
        resolution.tlkPath = chosen->tlkPath;
        resolution.game = gameIdFromString(chosen->id);
        return resolution;
    }
    if (context.definitiveGame) resolution.game = context.definitiveGame;
    else if (context.preferredGame && gameAllowed(*context.preferredGame, compatible)) resolution.game = context.preferredGame;
    if (resolution.diagnostic.empty()) {
        resolution.diagnostic = withTalkTable.empty()
            ? "No compatible configured game installation has a readable talk table."
            : "No unambiguous talk table could be selected for this resource.";
    }
    return resolution;
}

} // namespace neoshared::game
