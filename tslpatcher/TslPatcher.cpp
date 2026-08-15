#include "TslPatcher.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace neotsl {
namespace {

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}


std::string encodeIniText(const std::string& value) {
    std::string encoded;
    encoded.reserve(value.size());
    for (const char ch : value) {
        if (ch == '\r') encoded += "<#CR#>";
        else if (ch == '\n') encoded += "<#LF#>";
        else encoded.push_back(ch);
    }
    return encoded;
}
std::string trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool iequals(const std::string& a, const std::string& b) {
    return lowerAscii(a) == lowerAscii(b);
}

std::string cellOrEmpty(const std::vector<std::string>& row, std::size_t col) {
    return col < row.size() ? row[col] : std::string();
}

std::size_t optionalColumn(const neotabular::Table& table, const std::string& name) {
    const std::string want = lowerAscii(name);
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        if (lowerAscii(table.columns[i]) == want) return i;
    }
    return table.columns.size();
}

std::size_t requiredColumn(const neotabular::Table& table, const std::string& name) {
    const std::size_t idx = optionalColumn(table, name);
    if (idx == table.columns.size()) throw std::runtime_error("Required table column is missing: " + name);
    return idx;
}

std::string baseNameNoExt(const std::string& patchFilename) {
    std::filesystem::path p(patchFilename);
    std::string stem = lowerAscii(p.stem().string());
    if (stem.empty()) stem = "file";
    return sanitizeSectionName(stem);
}

std::string uniqueSectionName(const PatchProject& project, const std::string& base) {
    std::string clean = sanitizeSectionName(base);
    if (clean.empty()) clean = "section";
    for (std::size_t i = 0;; ++i) {
        std::string candidate = clean + "_" + std::to_string(i);
        if (!project.findSection(candidate)) return candidate;
    }
}

std::string nextKey(const IniSection& section, const std::string& prefix) {
    std::size_t next = 0;
    const std::string want = lowerAscii(prefix);
    for (const auto& kv : section.entries) {
        const std::string key = lowerAscii(kv.key);
        if (key.rfind(want, 0) != 0) continue;
        const std::string suffix = key.substr(want.size());
        if (suffix.empty()) continue;
        bool ok = true;
        std::size_t value = 0;
        for (char ch : suffix) {
            if (!std::isdigit(static_cast<unsigned char>(ch))) { ok = false; break; }
            value = value * 10 + static_cast<std::size_t>(ch - '0');
        }
        if (ok && value >= next) next = value + 1;
    }
    return prefix + std::to_string(next);
}

void addAssetIfRequested(PatchProject& project, bool copyBaselineAsset, const std::filesystem::path& baselineAsset, const std::string& patchFilename) {
    if (copyBaselineAsset && !baselineAsset.empty()) {
        project.assets.push_back({baselineAsset, patchFilename, {}});
    }
}

void addNumberedEntry(PatchProject& project, const std::string& sectionName, const std::string& prefix, const std::string& value) {
    auto& section = project.section(sectionName);
    section.entries.push_back({nextKey(section, prefix), value});
}

void addPlainEntry(PatchProject& project, const std::string& sectionName, const std::string& key, const std::string& value) {
    project.section(sectionName).entries.push_back({key, value});
}

std::string gffPatchType(std::string type) {
    type = trim(type);
    const std::string key = lowerAscii(type);
    if (key == "cexostring") return "ExoString";
    if (key == "cresref") return "ResRef";
    if (key == "cexolocstring") return "ExoLocString";
    if (key == "cexolocstring strref") return "ExoLocString";
    if (key == "cexolocstring text") return "ExoLocString";
    return type;
}

bool gffTypeIsEditableText(const std::string& type) {
    const std::string key = lowerAscii(type);
    return key != "struct" && key != "list" && key != "void" && key != "cexolocstring";
}

bool originalTslCompatibleAddFieldType(const std::string& type) {
    const std::string key = lowerAscii(gffPatchType(type));
    return key == "byte" || key == "char" || key == "word" || key == "short" ||
           key == "dword" || key == "int" || key == "int64" || key == "float" ||
           key == "double" || key == "exostring" || key == "resref" ||
           key == "exolocstring" || key == "orientation" || key == "position" ||
           key == "struct" || key == "list";
}

bool originalTslCompatibleDirectFieldType(const std::string& type) {
    const std::string key = lowerAscii(gffPatchType(type));
    return key == "byte" || key == "char" || key == "word" || key == "short" ||
           key == "dword" || key == "int" || key == "int64" || key == "float" ||
           key == "double" || key == "exostring" || key == "resref" ||
           key == "orientation" || key == "position";
}

bool truthy(std::string text) {
    text = lowerAscii(trim(std::move(text)));
    return text == "1" || text == "yes" || text == "true" || text == "editable";
}

std::string parentPathOf(const std::string& path) {
    const std::size_t pos = path.find_last_of('\\');
    if (pos == std::string::npos) return {};
    return path.substr(0, pos);
}

std::string leafOf(const std::string& path) {
    const std::size_t pos = path.find_last_of('\\');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

bool pathIsAtOrBelow(const std::string& path, const std::string& root) {
    return path == root ||
           (path.size() > root.size() &&
            path.compare(0, root.size(), root) == 0 &&
            path[root.size()] == '\\');
}

struct GffRow {
    std::string path;
    std::string label;
    std::string type;
    std::string editable;
    std::string value;
    std::size_t order = 0;
};

std::map<std::string, GffRow> gffRowsByPath(const neotabular::Table& table) {
    const std::size_t pathCol = requiredColumn(table, "Path");
    const std::size_t labelCol = optionalColumn(table, "Label");
    const std::size_t typeCol = optionalColumn(table, "Type");
    const std::size_t editableCol = optionalColumn(table, "Editable");
    const std::size_t valueCol = requiredColumn(table, "Value");
    std::map<std::string, GffRow> out;
    for (std::size_t i = 0; i < table.rows.size(); ++i) {
        const auto& row = table.rows[i];
        const std::string path = cellOrEmpty(row, pathCol);
        if (path.empty()) continue;
        out[path] = GffRow{path, cellOrEmpty(row, labelCol), cellOrEmpty(row, typeCol), cellOrEmpty(row, editableCol), cellOrEmpty(row, valueCol), i};
    }
    return out;
}

bool isLocStringChildPath(const std::string& path) {
    return path.size() > 8 && path.back() == ')' && path.find('(') != std::string::npos;
}

std::string locStringParentPath(const std::string& path) {
    const std::size_t marker = path.rfind('(');
    return marker == std::string::npos ? std::string{} : path.substr(0, marker);
}

bool containsLineBreak(const std::string& value) {
    return value.find('\r') != std::string::npos || value.find('\n') != std::string::npos;
}

bool pathHasDuplicateOccurrence(const std::string& path) {
    return path.find("[#") != std::string::npos;
}


std::optional<std::uint32_t> unsignedDecimal(const std::string& text) {
    if (text.empty()) return std::nullopt;
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return std::nullopt;
        value = value * 10u + static_cast<unsigned>(ch - '0');
        if (value > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

std::optional<std::uint32_t> structTypeIdFromSummary(const std::string& value) {
    const std::string prefix = "typeid=";
    const std::size_t begin = lowerAscii(value).find(prefix);
    if (begin == std::string::npos) return std::nullopt;
    const std::size_t numberBegin = begin + prefix.size();
    std::size_t numberEnd = numberBegin;
    while (numberEnd < value.size() && std::isdigit(static_cast<unsigned char>(value[numberEnd]))) ++numberEnd;
    return unsignedDecimal(value.substr(numberBegin, numberEnd - numberBegin));
}

std::string structuralParentPath(const std::string& path) {
    return isLocStringChildPath(path) ? locStringParentPath(path) : parentPathOf(path);
}

bool pathIsLocStringChildOf(const std::string& path, const std::string& parent) {
    return path.size() > parent.size() + 2u && path.compare(0, parent.size(), parent) == 0 &&
           path[parent.size()] == '(' && path.back() == ')';
}

bool pathBelongsToSubtree(const std::string& path, const std::string& root) {
    return pathIsAtOrBelow(path, root) || pathIsLocStringChildOf(path, root);
}

std::string patchLabelForRow(const GffRow& row, const std::string& parentType) {
    if (lowerAscii(gffPatchType(row.type)) == "struct" && lowerAscii(gffPatchType(parentType)) == "list") {
        return {};
    }
    return leafOf(row.path);
}

std::vector<std::string> dataColumns(const neotabular::Table& table) {
    if (table.columns.empty()) return {};
    return std::vector<std::string>(table.columns.begin() + 1, table.columns.end());
}

bool tableRowsEqualAt(const neotabular::Table& a, const neotabular::Table& b, std::size_t row, std::size_t col) {
    return cellOrEmpty(a.rows[row], col) == cellOrEmpty(b.rows[row], col);
}


struct ParsedIniEntry {
    std::vector<std::string> leadingTrivia;
    std::string key;
    std::string value;
    std::string originalLine;
    bool modified = false;
};

struct ParsedIniSection {
    std::vector<std::string> leadingTrivia;
    std::string name;
    std::string originalHeader;
    std::vector<ParsedIniEntry> entries;
};

struct ParsedIniDocument {
    std::string bom;
    std::string newline = "\r\n";
    bool finalNewline = true;
    std::vector<std::string> preamble;
    std::vector<ParsedIniSection> sections;
    std::vector<std::string> epilogue;

    static ParsedIniDocument load(const std::filesystem::path& path) {
        ParsedIniDocument doc;
        std::ifstream input(path, std::ios::binary);
        if (!input) return doc;
        std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (bytes.size() >= 3u && static_cast<unsigned char>(bytes[0]) == 0xefu &&
            static_cast<unsigned char>(bytes[1]) == 0xbbu && static_cast<unsigned char>(bytes[2]) == 0xbfu) {
            doc.bom.assign(bytes.data(), 3u);
            bytes.erase(0u, 3u);
        }
        if (bytes.find("\r\n") != std::string::npos) doc.newline = "\r\n";
        else if (bytes.find('\n') != std::string::npos) doc.newline = "\n";
        doc.finalNewline = bytes.empty() || bytes.back() == '\n' || bytes.back() == '\r';

        std::vector<std::string> lines;
        std::size_t begin = 0;
        while (begin < bytes.size()) {
            const std::size_t lf = bytes.find('\n', begin);
            std::size_t end = lf == std::string::npos ? bytes.size() : lf;
            if (end > begin && bytes[end - 1u] == '\r') --end;
            lines.push_back(bytes.substr(begin, end - begin));
            if (lf == std::string::npos) break;
            begin = lf + 1u;
        }
        if (bytes.empty()) lines.clear();

        ParsedIniSection* current = nullptr;
        std::vector<std::string> pending;
        for (const auto& line : lines) {
            const std::string stripped = trim(line);
            const bool trivia = stripped.empty() || stripped.front() == ';' || stripped.front() == '#';
            if (trivia) {
                if (current == nullptr) doc.preamble.push_back(line);
                else pending.push_back(line);
                continue;
            }
            if (stripped.size() >= 2u && stripped.front() == '[') {
                const std::size_t close = stripped.find(']');
                const std::string trailing = close == std::string::npos
                    ? std::string{}
                    : trim(stripped.substr(close + 1u));
                if (close != std::string::npos &&
                    (trailing.empty() || trailing.front() == ';' || trailing.front() == '#')) {
                    doc.sections.push_back({
                        std::move(pending),
                        stripped.substr(1u, close - 1u),
                        line,
                        {}});
                    pending.clear();
                    current = &doc.sections.back();
                    continue;
                }
            }
            const std::size_t equals = line.find('=');
            if (current != nullptr && equals != std::string::npos) {
                current->entries.push_back({
                    std::move(pending),
                    trim(line.substr(0u, equals)),
                    trim(line.substr(equals + 1u)),
                    line,
                    false});
                pending.clear();
            } else if (current != nullptr) {
                pending.push_back(line);
            } else {
                doc.preamble.push_back(line);
            }
        }
        doc.epilogue = std::move(pending);
        return doc;
    }

    std::vector<ParsedIniSection*> findSections(const std::string& name) {
        std::vector<ParsedIniSection*> result;
        for (auto& section : sections) if (iequals(section.name, name)) result.push_back(&section);
        return result;
    }

    std::vector<const ParsedIniSection*> findSections(const std::string& name) const {
        std::vector<const ParsedIniSection*> result;
        for (const auto& section : sections) if (iequals(section.name, name)) result.push_back(&section);
        return result;
    }

    ParsedIniSection* findUniqueSection(const std::string& name) {
        auto matches = findSections(name);
        if (matches.size() > 1u) {
            throw std::runtime_error("Cannot merge generated instructions because the selected INI contains duplicate [" +
                                     name + "] sections.");
        }
        return matches.empty() ? nullptr : matches.front();
    }

    const ParsedIniSection* findUniqueSection(const std::string& name) const {
        auto matches = findSections(name);
        if (matches.size() > 1u) {
            throw std::runtime_error("Cannot inspect the selected INI because it contains duplicate [" + name + "] sections.");
        }
        return matches.empty() ? nullptr : matches.front();
    }

    ParsedIniSection& ensureSection(const std::string& name, IniMergeReport& report) {
        if (auto* section = findUniqueSection(name)) return *section;
        std::vector<std::string> trivia;
        if (!sections.empty()) trivia.emplace_back();
        sections.push_back({std::move(trivia), name, {}, {}});
        ++report.sectionsAdded;
        return sections.back();
    }

    static ParsedIniEntry* findEntry(ParsedIniSection& section, const std::string& key) {
        for (auto& entry : section.entries) if (iequals(entry.key, key)) return &entry;
        return nullptr;
    }

    static const ParsedIniEntry* findEntry(const ParsedIniSection& section, const std::string& key) {
        for (const auto& entry : section.entries) if (iequals(entry.key, key)) return &entry;
        return nullptr;
    }

    static std::vector<ParsedIniEntry*> findEntries(ParsedIniSection& section,
                                                    const std::string& key) {
        std::vector<ParsedIniEntry*> result;
        for (auto& entry : section.entries) {
            if (iequals(entry.key, key)) result.push_back(&entry);
        }
        return result;
    }

    static std::vector<const ParsedIniEntry*> findEntries(const ParsedIniSection& section,
                                                          const std::string& key) {
        std::vector<const ParsedIniEntry*> result;
        for (const auto& entry : section.entries) {
            if (iequals(entry.key, key)) result.push_back(&entry);
        }
        return result;
    }

    void saveAtomically(const std::filesystem::path& path) const {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const std::filesystem::path temporary = path.string() + ".neo-tmp-" + std::to_string(stamp);
        const std::filesystem::path backup = path.string() + ".neo-bak-" + std::to_string(stamp);
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("Unable to create temporary INI output: " + temporary.string());
            output.write(bom.data(), static_cast<std::streamsize>(bom.size()));
            bool firstLine = true;
            auto line = [&](const std::string& value) {
                if (!firstLine) output.write(newline.data(), static_cast<std::streamsize>(newline.size()));
                output.write(value.data(), static_cast<std::streamsize>(value.size()));
                firstLine = false;
            };
            for (const auto& value : preamble) line(value);
            for (const auto& section : sections) {
                for (const auto& value : section.leadingTrivia) line(value);
                line(section.originalHeader.empty() ? "[" + section.name + "]" : section.originalHeader);
                for (const auto& entry : section.entries) {
                    for (const auto& value : entry.leadingTrivia) line(value);
                    line(!entry.modified && !entry.originalLine.empty()
                             ? entry.originalLine
                             : entry.key + "=" + entry.value);
                }
            }
            for (const auto& value : epilogue) line(value);
            if (finalNewline && !firstLine) output.write(newline.data(), static_cast<std::streamsize>(newline.size()));
            if (!output) throw std::runtime_error("Unable to write temporary INI output: " + temporary.string());
        }

        std::error_code ec;
        const bool existed = std::filesystem::exists(path, ec) && !ec;
        if (existed) {
            std::filesystem::rename(path, backup, ec);
            if (ec) {
                std::filesystem::remove(temporary);
                throw std::runtime_error("Unable to prepare existing INI for atomic replacement: " + ec.message());
            }
        }
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            if (existed) {
                std::error_code ignored;
                std::filesystem::rename(backup, path, ignored);
            }
            std::filesystem::remove(temporary);
            throw std::runtime_error("Unable to replace INI file atomically: " + ec.message());
        }
        if (existed) std::filesystem::remove(backup, ec);
    }
};

bool startsWithI(const std::string& text, const std::string& prefix) {
    if (text.size() < prefix.size()) return false;
    return lowerAscii(text.substr(0u, prefix.size())) == lowerAscii(prefix);
}

bool isTokenBoundary(char ch) {
    const unsigned char value = static_cast<unsigned char>(ch);
    return !std::isalnum(value) && ch != '_';
}

std::set<std::size_t> tokenIndicesInText(const std::string& text, const std::string& prefix) {
    std::set<std::size_t> result;
    const std::string lowerText = lowerAscii(text);
    const std::string lowerPrefix = lowerAscii(prefix);
    for (std::size_t pos = 0; pos + lowerPrefix.size() < lowerText.size(); ++pos) {
        if (lowerText.compare(pos, lowerPrefix.size(), lowerPrefix) != 0) continue;
        if (pos > 0u && !isTokenBoundary(lowerText[pos - 1u])) continue;
        const std::size_t digitsBegin = pos + lowerPrefix.size();
        std::size_t digitsEnd = digitsBegin;
        while (digitsEnd < lowerText.size() && std::isdigit(static_cast<unsigned char>(lowerText[digitsEnd]))) ++digitsEnd;
        if (digitsEnd == digitsBegin) continue;
        if (digitsEnd < lowerText.size() && !isTokenBoundary(lowerText[digitsEnd])) continue;
        result.insert(static_cast<std::size_t>(std::stoull(lowerText.substr(digitsBegin, digitsEnd - digitsBegin))));
        pos = digitsEnd - 1u;
    }
    return result;
}

std::string remapTokensInText(const std::string& text,
                              const std::string& prefix,
                              const std::map<std::size_t, std::size_t>& mapping) {
    if (mapping.empty()) return text;
    const std::string lowerText = lowerAscii(text);
    const std::string lowerPrefix = lowerAscii(prefix);
    std::string output;
    output.reserve(text.size());
    std::size_t cursor = 0u;
    while (cursor < text.size()) {
        std::size_t found = lowerText.find(lowerPrefix, cursor);
        if (found == std::string::npos) {
            output.append(text, cursor, std::string::npos);
            break;
        }
        const bool leftBoundary = found == 0u || isTokenBoundary(lowerText[found - 1u]);
        std::size_t digitsEnd = found + lowerPrefix.size();
        while (digitsEnd < lowerText.size() && std::isdigit(static_cast<unsigned char>(lowerText[digitsEnd]))) ++digitsEnd;
        const bool hasDigits = digitsEnd > found + lowerPrefix.size();
        const bool rightBoundary = digitsEnd == lowerText.size() || isTokenBoundary(lowerText[digitsEnd]);
        if (!leftBoundary || !hasDigits || !rightBoundary) {
            output.append(text, cursor, found - cursor + 1u);
            cursor = found + 1u;
            continue;
        }
        const std::size_t oldIndex = static_cast<std::size_t>(std::stoull(
            lowerText.substr(found + lowerPrefix.size(), digitsEnd - found - lowerPrefix.size())));
        const auto replacement = mapping.find(oldIndex);
        output.append(text, cursor, found - cursor);
        if (replacement == mapping.end()) output.append(text, found, digitsEnd - found);
        else output += prefix + std::to_string(replacement->second);
        cursor = digitsEnd;
    }
    return output;
}

std::set<std::size_t> collectDocumentTokens(const ParsedIniDocument& doc, const std::string& prefix) {
    std::set<std::size_t> result;
    for (const auto& section : doc.sections) {
        for (const auto& entry : section.entries) {
            const auto keyTokens = tokenIndicesInText(entry.key, prefix);
            const auto valueTokens = tokenIndicesInText(entry.value, prefix);
            result.insert(keyTokens.begin(), keyTokens.end());
            result.insert(valueTokens.begin(), valueTokens.end());
        }
    }
    return result;
}

std::set<std::size_t> collectProjectTokens(const PatchProject& project, const std::string& prefix) {
    std::set<std::size_t> result;
    for (const auto& section : project.sections) {
        for (const auto& entry : section.entries) {
            const auto keyTokens = tokenIndicesInText(entry.key, prefix);
            const auto valueTokens = tokenIndicesInText(entry.value, prefix);
            result.insert(keyTokens.begin(), keyTokens.end());
            result.insert(valueTokens.begin(), valueTokens.end());
        }
    }
    return result;
}

std::map<std::size_t, std::size_t> allocateTokenMap(const std::set<std::size_t>& existing,
                                                    const std::set<std::size_t>& incoming,
                                                    std::size_t minimum,
                                                    const std::set<std::size_t>& preserved = {}) {
    std::set<std::size_t> reserved = existing;
    std::map<std::size_t, std::size_t> mapping;
    std::size_t next = minimum;
    for (const std::size_t oldIndex : incoming) {
        if (preserved.count(oldIndex) != 0u) {
            mapping[oldIndex] = oldIndex;
            continue;
        }
        if (oldIndex >= minimum && reserved.count(oldIndex) == 0u) {
            mapping[oldIndex] = oldIndex;
            reserved.insert(oldIndex);
            continue;
        }
        while (reserved.count(next) != 0u) ++next;
        mapping[oldIndex] = next;
        reserved.insert(next);
        ++next;
    }
    return mapping;
}

void applyProjectTokenMap(PatchProject& project,
                          const std::string& prefix,
                          const std::map<std::size_t, std::size_t>& mapping) {
    for (auto& section : project.sections) {
        for (auto& entry : section.entries) {
            entry.key = remapTokensInText(entry.key, prefix, mapping);
            entry.value = remapTokensInText(entry.value, prefix, mapping);
        }
    }
}

void remapProjectTokens(PatchProject& project,
                        const ParsedIniDocument& existing,
                        const std::string& prefix,
                        std::size_t minimum,
                        const std::set<std::size_t>& preserved = {}) {
    const auto mapping = allocateTokenMap(collectDocumentTokens(existing, prefix),
                                          collectProjectTokens(project, prefix),
                                          minimum,
                                          preserved);
    applyProjectTokenMap(project, prefix, mapping);
}

struct TokenReference {
    std::string prefix;
    std::size_t index = 0u;
};

struct TokenizedIniText {
    std::string shape;
    std::vector<TokenReference> references;
};

bool parseTokenAt(const std::string& text,
                  std::size_t position,
                  const std::string& prefix,
                  std::size_t& end,
                  std::size_t& index) {
    if (position + prefix.size() >= text.size()) return false;
    if (position > 0u && !isTokenBoundary(text[position - 1u])) return false;
    for (std::size_t offset = 0u; offset < prefix.size(); ++offset) {
        const unsigned char lhs = static_cast<unsigned char>(text[position + offset]);
        const unsigned char rhs = static_cast<unsigned char>(prefix[offset]);
        if (std::tolower(lhs) != std::tolower(rhs)) return false;
    }
    const std::size_t digitsBegin = position + prefix.size();
    end = digitsBegin;
    while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
    if (end == digitsBegin) return false;
    if (end < text.size() && !isTokenBoundary(text[end])) return false;
    index = static_cast<std::size_t>(std::stoull(text.substr(digitsBegin, end - digitsBegin)));
    return true;
}

TokenizedIniText tokenizeIniText(const std::string& text, bool foldCase) {
    static const std::array<std::string, 2u> prefixes = {"2DAMEMORY", "StrRef"};
    TokenizedIniText result;
    result.shape.reserve(text.size());
    for (std::size_t cursor = 0u; cursor < text.size();) {
        bool found = false;
        for (const auto& prefix : prefixes) {
            std::size_t end = cursor;
            std::size_t index = 0u;
            if (!parseTokenAt(text, cursor, prefix, end, index)) continue;
            result.shape += prefix + "#";
            result.references.push_back({prefix, index});
            cursor = end;
            found = true;
            break;
        }
        if (found) continue;
        const unsigned char value = static_cast<unsigned char>(text[cursor]);
        result.shape.push_back(foldCase ? static_cast<char>(std::tolower(value)) : text[cursor]);
        ++cursor;
    }
    return result;
}

struct TokenizedIniEntry {
    std::string shape;
    std::vector<TokenReference> references;
};

TokenizedIniEntry tokenizeIniEntry(const std::string& key, const std::string& encodedValue) {
    const auto tokenizedKey = tokenizeIniText(key, true);
    const auto tokenizedValue = tokenizeIniText(encodedValue, false);
    TokenizedIniEntry result;
    result.shape = tokenizedKey.shape + '\x1f' + tokenizedValue.shape;
    result.references = tokenizedKey.references;
    result.references.insert(result.references.end(),
                             tokenizedValue.references.begin(),
                             tokenizedValue.references.end());
    return result;
}

struct TokenReuseMaps {
    std::map<std::size_t, std::size_t> twoDa;
    std::map<std::size_t, std::size_t> reverseTwoDa;
    std::map<std::size_t, std::size_t> strRef;
    std::map<std::size_t, std::size_t> reverseStrRef;
};

bool recordTokenReuse(TokenReuseMaps& maps,
                      const TokenReference& incoming,
                      const TokenReference& existing) {
    if (!iequals(incoming.prefix, existing.prefix)) return false;
    auto* forward = iequals(incoming.prefix, "2DAMEMORY") ? &maps.twoDa : &maps.strRef;
    auto* reverse = iequals(incoming.prefix, "2DAMEMORY") ? &maps.reverseTwoDa : &maps.reverseStrRef;
    const auto current = forward->find(incoming.index);
    if (current != forward->end() && current->second != existing.index) return false;
    const auto owner = reverse->find(existing.index);
    if (owner != reverse->end() && owner->second != incoming.index) return false;
    (*forward)[incoming.index] = existing.index;
    (*reverse)[existing.index] = incoming.index;
    return true;
}

bool tokenEquivalentSections(const ParsedIniSection& existing,
                             const IniSection& incoming,
                             const TokenReuseMaps& initial,
                             TokenReuseMaps& result) {
    if (existing.entries.size() != incoming.entries.size()) return false;

    std::vector<TokenizedIniEntry> existingEntries;
    std::vector<TokenizedIniEntry> incomingEntries;
    existingEntries.reserve(existing.entries.size());
    incomingEntries.reserve(incoming.entries.size());
    std::map<std::string, std::vector<std::size_t>> existingGroups;
    std::map<std::string, std::vector<std::size_t>> incomingGroups;

    for (std::size_t index = 0u; index < existing.entries.size(); ++index) {
        existingEntries.push_back(tokenizeIniEntry(existing.entries[index].key, existing.entries[index].value));
        existingGroups[existingEntries.back().shape].push_back(index);
    }
    for (std::size_t index = 0u; index < incoming.entries.size(); ++index) {
        incomingEntries.push_back(tokenizeIniEntry(incoming.entries[index].key,
                                                   encodeIniText(incoming.entries[index].value)));
        incomingGroups[incomingEntries.back().shape].push_back(index);
    }
    if (existingGroups.size() != incomingGroups.size()) return false;

    TokenReuseMaps candidate = initial;
    for (const auto& [shape, incomingIndexes] : incomingGroups) {
        const auto match = existingGroups.find(shape);
        if (match == existingGroups.end() || match->second.size() != incomingIndexes.size()) return false;
        for (std::size_t position = 0u; position < incomingIndexes.size(); ++position) {
            const auto& incomingReferences = incomingEntries[incomingIndexes[position]].references;
            const auto& existingReferences = existingEntries[match->second[position]].references;
            if (incomingReferences.size() != existingReferences.size()) return false;
            for (std::size_t ref = 0u; ref < incomingReferences.size(); ++ref) {
                if (!recordTokenReuse(candidate, incomingReferences[ref], existingReferences[ref])) return false;
            }
        }
    }
    result = std::move(candidate);
    return true;
}

struct ReusedTokenTargets {
    std::set<std::size_t> twoDa;
    std::set<std::size_t> strRef;
};

std::optional<std::size_t> exactTokenKeyIndex(const std::string& key,
                                              const std::string& prefix) {
    if (!startsWithI(key, prefix) || key.size() == prefix.size()) return std::nullopt;
    const std::string suffix = key.substr(prefix.size());
    if (!std::all_of(suffix.begin(), suffix.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0;
        })) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::stoull(suffix));
}

std::set<std::size_t> reuseExistingStrRefTokens(PatchProject& project,
                                                const ParsedIniDocument& existing) {
    const auto* existingTlk = existing.findUniqueSection("TLKList");
    auto* incomingTlk = const_cast<IniSection*>(project.findSection("TLKList"));
    if (!existingTlk || !incomingTlk) return {};

    std::unordered_map<std::string, std::size_t> existingByValue;
    for (const auto& entry : existingTlk->entries) {
        const auto index = exactTokenKeyIndex(entry.key, "StrRef");
        if (index) existingByValue.emplace(entry.value, *index);
    }

    std::map<std::size_t, std::size_t> mapping;
    for (const auto& entry : incomingTlk->entries) {
        const auto sourceIndex = exactTokenKeyIndex(entry.key, "StrRef");
        if (!sourceIndex) continue;
        const auto match = existingByValue.find(encodeIniText(entry.value));
        if (match != existingByValue.end()) mapping[*sourceIndex] = match->second;
    }
    if (mapping.empty()) return {};

    applyProjectTokenMap(project, "StrRef", mapping);
    std::set<std::size_t> preserved;
    for (const auto& [source, target] : mapping) {
        (void)source;
        preserved.insert(target);
    }
    return preserved;
}

bool sectionNameExists(const ParsedIniDocument& doc, const std::string& name) {
    return !doc.findSections(name).empty();
}

bool projectSectionExists(const PatchProject& project, const std::string& name) {
    return project.findSection(name) != nullptr;
}

std::string uniqueMergedSectionName(const ParsedIniDocument& doc,
                                    const PatchProject& project,
                                    const std::string& base) {
    for (std::size_t index = 1u;; ++index) {
        const std::string candidate = sanitizeSectionName(base) + "_neo" + std::to_string(index);
        if (!sectionNameExists(doc, candidate) && !projectSectionExists(project, candidate)) return candidate;
    }
}

std::string lowerKey(const std::string& value) { return lowerAscii(value); }

bool isNumberedInstructionKey(const std::string& key,
                              const std::string& prefix) {
    if (!startsWithI(key, prefix) || key.size() == prefix.size()) return false;
    return std::all_of(key.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                       key.end(),
                       [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

bool entryValueReferencesHelperSection(const std::string& key) {
    static const std::array<std::string, 5u> prefixes = {
        "AddField", "AddRow", "ChangeRow", "CopyRow", "AddColumn"};
    return std::any_of(prefixes.begin(), prefixes.end(), [&](const std::string& prefix) {
        return isNumberedInstructionKey(key, prefix);
    });
}

void applySectionRenames(PatchProject& project,
                         const std::unordered_map<std::string, std::string>& renames) {
    if (renames.empty()) return;
    for (auto& section : project.sections) {
        const auto sectionRename = renames.find(lowerKey(section.name));
        if (sectionRename != renames.end()) section.name = sectionRename->second;
        for (auto& entry : section.entries) {
            if (iequals(section.name, "InstallList")) {
                const auto keyRename = renames.find(lowerKey(entry.key));
                if (keyRename != renames.end()) entry.key = keyRename->second;
            }
            if (entryValueReferencesHelperSection(entry.key)) {
                const auto valueRename = renames.find(lowerKey(entry.value));
                if (valueRename != renames.end()) entry.value = valueRename->second;
            }
        }
    }
}

std::unordered_set<std::string> mergeableSectionNames(const PatchProject& project) {
    static const std::vector<std::string> roots = {
        "Settings", "TLKList", "InstallList", "2DAList", "GFFList", "CompileList", "SSFList"};
    std::unordered_set<std::string> result;
    for (const auto& root : roots) result.insert(lowerKey(root));
    for (const auto& asset : project.assets) {
        if (!asset.targetName.empty()) result.insert(lowerKey(asset.targetName));
    }
    for (const auto& listName : {"2DAList", "GFFList", "SSFList", "CompileList", "TLKList"}) {
        if (const auto* section = project.findSection(listName)) {
            for (const auto& entry : section->entries) {
                if (project.findSection(entry.value)) result.insert(lowerKey(entry.value));
            }
        }
    }
    if (const auto* installs = project.findSection("InstallList")) {
        for (const auto& entry : installs->entries) {
            if (project.findSection(entry.key)) result.insert(lowerKey(entry.key));
        }
    }
    for (const auto& section : project.sections) {
        if (std::any_of(section.entries.begin(), section.entries.end(), [](const KeyValue& entry) {
                return !entry.key.empty() && entry.key.front() == '!';
            })) {
            result.insert(lowerKey(section.name));
        }
    }
    return result;
}

const ParsedIniEntry* findEntryWithValue(const ParsedIniSection& section, const std::string& value) {
    for (const auto& entry : section.entries) if (iequals(entry.value, value)) return &entry;
    return nullptr;
}

bool sectionsEquivalent(const ParsedIniSection& existing, const IniSection& incoming) {
    if (existing.entries.size() != incoming.entries.size()) return false;
    std::vector<bool> matched(existing.entries.size(), false);
    for (const auto& candidate : incoming.entries) {
        const std::string encodedValue = encodeIniText(candidate.value);
        bool found = false;
        for (std::size_t index = 0u; index < existing.entries.size(); ++index) {
            if (matched[index]) continue;
            const auto& current = existing.entries[index];
            if (!iequals(current.key, candidate.key) || current.value != encodedValue) continue;
            matched[index] = true;
            found = true;
            break;
        }
        if (!found) return false;
    }
    return true;
}

bool matchingExistingSection(const ParsedIniDocument& existing, const IniSection& incoming) {
    const auto matches = existing.findSections(incoming.name);
    return matches.size() == 1u && sectionsEquivalent(*matches.front(), incoming);
}

bool isGeneratedHelperVariant(const std::string& candidate, const std::string& base) {
    if (iequals(candidate, base)) return true;
    const std::string prefix = lowerKey(sanitizeSectionName(base) + "_neo");
    const std::string lowered = lowerKey(candidate);
    if (lowered.size() <= prefix.size() || lowered.compare(0u, prefix.size(), prefix) != 0) return false;
    return std::all_of(lowered.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                       lowered.end(),
                       [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

ReusedTokenTargets reuseExistingHelperSections(PatchProject& project,
                                                const ParsedIniDocument& existing) {
    TokenReuseMaps mappings;
    std::unordered_map<std::string, std::string> renames;
    std::unordered_set<std::string> usedExistingSections;
    const auto mergeable = mergeableSectionNames(project);

    for (const auto& section : project.sections) {
        if (mergeable.count(lowerKey(section.name)) != 0u) continue;

        std::vector<const ParsedIniSection*> candidates;
        for (const auto& existingSection : existing.sections) {
            if (isGeneratedHelperVariant(existingSection.name, section.name)) {
                candidates.push_back(&existingSection);
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(), [&](const auto* lhs, const auto* rhs) {
            return iequals(lhs->name, section.name) && !iequals(rhs->name, section.name);
        });

        for (const auto* candidateSection : candidates) {
            if (usedExistingSections.count(lowerKey(candidateSection->name)) != 0u) continue;
            TokenReuseMaps candidateMappings;
            if (!tokenEquivalentSections(*candidateSection, section, mappings, candidateMappings)) continue;
            mappings = std::move(candidateMappings);
            usedExistingSections.insert(lowerKey(candidateSection->name));
            if (!iequals(candidateSection->name, section.name)) {
                renames[lowerKey(section.name)] = candidateSection->name;
            }
            break;
        }
    }

    applyProjectTokenMap(project, "2DAMEMORY", mappings.twoDa);
    applyProjectTokenMap(project, "StrRef", mappings.strRef);
    applySectionRenames(project, renames);

    ReusedTokenTargets result;
    for (const auto& [source, target] : mappings.twoDa) {
        (void)source;
        result.twoDa.insert(target);
    }
    for (const auto& [source, target] : mappings.strRef) {
        (void)source;
        result.strRef.insert(target);
    }
    return result;
}

void normalizeIncomingSectionNames(PatchProject& project, const ParsedIniDocument& existing) {
    std::unordered_map<std::string, std::string> renames;

    if (const auto* incomingInstalls = project.findSection("InstallList")) {
        const auto* existingInstalls = existing.findUniqueSection("InstallList");
        for (const auto& entry : incomingInstalls->entries) {
            if (!project.findSection(entry.key)) continue;
            if (existingInstalls) {
                if (const auto* sameDestination = findEntryWithValue(*existingInstalls, entry.value)) {
                    renames[lowerKey(entry.key)] = sameDestination->key;
                    continue;
                }
            }
            if (sectionNameExists(existing, entry.key) ||
                (existingInstalls && ParsedIniDocument::findEntry(*existingInstalls, entry.key))) {
                renames[lowerKey(entry.key)] = uniqueMergedSectionName(existing, project, entry.key);
            }
        }
    }
    applySectionRenames(project, renames);

    const auto mergeable = mergeableSectionNames(project);
    renames.clear();
    for (const auto& section : project.sections) {
        if (mergeable.count(lowerKey(section.name)) != 0u) continue;
        if (sectionNameExists(existing, section.name) && !matchingExistingSection(existing, section)) {
            renames[lowerKey(section.name)] = uniqueMergedSectionName(existing, project, section.name);
        }
    }
    applySectionRenames(project, renames);
}

std::optional<std::string> numberedInstructionPrefix(const std::string& key) {
    static const std::vector<std::string> prefixes = {
        "AddField", "AddRow", "ChangeRow", "CopyRow", "AddColumn",
        "File", "Replace", "Table", "ReplaceFile", "AppendFile", "Compile"};
    for (const auto& prefix : prefixes) {
        if (!startsWithI(key, prefix) || key.size() == prefix.size()) continue;
        if (std::all_of(key.begin() + static_cast<std::ptrdiff_t>(prefix.size()), key.end(), [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            })) {
            return prefix;
        }
    }
    return std::nullopt;
}

std::string nextAvailableNumberedKey(const ParsedIniSection& section, const std::string& prefix) {
    std::size_t next = 0u;
    for (const auto& entry : section.entries) {
        if (!startsWithI(entry.key, prefix) || entry.key.size() == prefix.size()) continue;
        const std::string suffix = entry.key.substr(prefix.size());
        if (!std::all_of(suffix.begin(), suffix.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) continue;
        const std::size_t value = static_cast<std::size_t>(std::stoull(suffix));
        next = std::max(next, value + 1u);
    }
    return prefix + std::to_string(next);
}

bool rootListDeduplicatesValues(const std::string& sectionName) {
    return iequals(sectionName, "2DAList") || iequals(sectionName, "GFFList") ||
           iequals(sectionName, "SSFList") || iequals(sectionName, "CompileList") ||
           iequals(sectionName, "InstallList");
}

bool numberedEntryWithValueAlreadyExists(const ParsedIniSection& section,
                                           const std::string& key,
                                           const std::string& value) {
    const auto prefix = numberedInstructionPrefix(key);
    if (!prefix) return false;
    for (const auto& entry : section.entries) {
        if (startsWithI(entry.key, *prefix) && iequals(entry.value, value)) return true;
    }
    return false;
}

bool tlkFileEntryAlreadyExists(const ParsedIniSection& section,
                               const std::string& key,
                               const std::string& value) {
    if (!iequals(section.name, "TLKList")) return false;
    std::optional<std::string> prefix;
    if (startsWithI(key, "AppendFile")) prefix = "AppendFile";
    else if (startsWithI(key, "ReplaceFile")) prefix = "ReplaceFile";
    if (!prefix) return false;
    for (const auto& entry : section.entries) {
        if (startsWithI(entry.key, *prefix) && iequals(entry.value, value)) return true;
    }
    return false;
}

void appendEntry(ParsedIniSection& section,
                 std::string key,
                 std::string value,
                 IniMergeReport& report) {
    section.entries.push_back({{}, std::move(key), encodeIniText(value), {}, true});
    ++report.entriesAdded;
}

void mergeSectionEntries(ParsedIniSection& target,
                         const IniSection& incoming,
                         IniMergeReport& report) {
    const bool settings = iequals(target.name, "Settings");
    const bool dedupeValue = rootListDeduplicatesValues(target.name);
    for (const auto& raw : incoming.entries) {
        std::string key = raw.key;
        const std::string encodedValue = encodeIniText(raw.value);

        if (settings) {
            auto existingSettings = ParsedIniDocument::findEntries(target, key);
            if (existingSettings.size() > 1u) {
                throw std::runtime_error(
                    "Cannot merge generated instructions into [" + target.name + "] because " + key +
                    " occurs more than once. Remove the duplicate instructions or select another installer INI.");
            }
            if (existingSettings.empty()) {
                appendEntry(target, key, iequals(key, "InstallerMode") ? "1" : raw.value, report);
            } else if (iequals(key, "InstallerMode") && existingSettings.front()->value != "1") {
                existingSettings.front()->value = "1";
                existingSettings.front()->modified = true;
                ++report.entriesUpdated;
                report.notes.push_back("Enabled [Settings] InstallerMode for patcher-package export.");
            }
            continue;
        }
        if (dedupeValue && findEntryWithValue(target, encodedValue)) continue;
        if (tlkFileEntryAlreadyExists(target, key, encodedValue)) continue;
        if (numberedEntryWithValueAlreadyExists(target, key, encodedValue)) continue;

        auto matchingKeys = ParsedIniDocument::findEntries(target, key);
        if (matchingKeys.size() > 1u) {
            throw std::runtime_error(
                "Cannot merge generated instructions into [" + target.name + "] because " + key +
                " occurs more than once. Remove the duplicate instructions or select another installer INI.");
        }
        if (matchingKeys.empty()) {
            appendEntry(target, key, raw.value, report);
            continue;
        }
        ParsedIniEntry* existing = matchingKeys.front();
        if (existing->value == encodedValue) continue;

        if (const auto prefix = numberedInstructionPrefix(key)) {
            key = nextAvailableNumberedKey(target, *prefix);
            appendEntry(target, key, raw.value, report);
            continue;
        }
        if (!key.empty() && key.front() == '!') {
            throw std::runtime_error(
                "Cannot merge generated instructions into [" + target.name + "] because " + key +
                " already has a different value ('" + existing->value + "' versus '" + encodedValue + "').");
        }

        existing->value = encodedValue;
        existing->modified = true;
        ++report.entriesUpdated;
        report.notes.push_back("Updated [" + target.name + "] " + key + ".");
    }
}

IniMergeReport mergeProjectIntoDocument(const PatchProject& input,
                                         ParsedIniDocument& document,
                                         const std::filesystem::path& path,
                                         bool includeSettings) {
    IniMergeReport report;
    report.iniPath = path;
    report.mergedExisting = true;
    PatchProject project = input;

    // Reuse an existing StrRef token when it already points at the same
    // append/replace payload index.
    auto reusedStrRefs = reuseExistingStrRefTokens(project, document);

    // A prior export may already contain this generated helper with tokens that
    // were renumbered to avoid collisions. Reuse those semantic token numbers
    // before allocating new ones, so exporting the same project again remains
    // idempotent.
    const auto reusedHelpers = reuseExistingHelperSections(project, document);
    reusedStrRefs.insert(reusedHelpers.strRef.begin(), reusedHelpers.strRef.end());

    // Original TSLPatcher reserves 2DAMEMORY0. StrRef0 is valid.
    remapProjectTokens(project, document, "2DAMEMORY", 1u, reusedHelpers.twoDa);
    remapProjectTokens(project, document, "StrRef", 0u, reusedStrRefs);

    // Helper equivalence must be evaluated after token normalization. Otherwise
    // an already merged helper can be mistaken for a new section solely because
    // its tokens were previously renumbered.
    normalizeIncomingSectionNames(project, document);

    if (includeSettings) {
        auto& settings = document.ensureSection("Settings", report);
        auto installerModes = ParsedIniDocument::findEntries(settings, "InstallerMode");
        if (installerModes.size() > 1u) {
            throw std::runtime_error(
                "Cannot merge generated instructions into [Settings] because InstallerMode occurs more than once. "
                "Remove the duplicate instructions or select another installer INI.");
        }
        if (!installerModes.empty()) {
            auto* installerMode = installerModes.front();
            if (installerMode->value != "1") {
                installerMode->value = "1";
                installerMode->modified = true;
                ++report.entriesUpdated;
                report.notes.push_back("Enabled [Settings] InstallerMode for patcher-package export.");
            }
        } else {
            appendEntry(settings, "InstallerMode", "1", report);
        }
    }

    static const std::vector<std::string> preferred = {
        "Settings", "TLKList", "InstallList", "2DAList", "GFFList", "CompileList", "SSFList"};
    std::unordered_set<std::string> done;
    auto mergeOne = [&](const IniSection& section) {
        const std::string name = lowerKey(section.name);
        if ((!includeSettings && name == "settings") || section.entries.empty()) return;
        if (!done.insert(name).second) return;
        auto& target = document.ensureSection(section.name, report);
        mergeSectionEntries(target, section, report);
    };
    for (const auto& name : preferred) {
        if (const auto* section = project.findSection(name)) mergeOne(*section);
    }
    for (const auto& section : project.sections) mergeOne(section);
    return report;
}

bool streamContentsEqual(std::istream& left, std::istream& right) {
    constexpr std::size_t size = 64u * 1024u;
    std::array<char, size> a{};
    std::array<char, size> b{};
    while (left && right) {
        left.read(a.data(), static_cast<std::streamsize>(a.size()));
        right.read(b.data(), static_cast<std::streamsize>(b.size()));
        if (left.gcount() != right.gcount()) return false;
        if (!std::equal(a.data(), a.data() + left.gcount(), b.data())) return false;
    }
    return true;
}

bool fileMatchesAsset(const std::filesystem::path& target, const StagedAsset& asset) {
    std::ifstream existing(target, std::ios::binary);
    if (!existing) return false;
    if (!asset.source.empty()) {
        std::ifstream source(asset.source, std::ios::binary);
        return source && streamContentsEqual(existing, source);
    }
    std::string existingBytes((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
    return existingBytes.size() == asset.data.size() &&
           std::equal(existingBytes.begin(), existingBytes.end(), asset.data.begin());
}

void writeGeneratedAssetAtomically(const StagedAsset& asset, const std::filesystem::path& target) {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path temporary = target.string() + ".neo-tmp-" + std::to_string(stamp);
    std::error_code ec;
    if (!asset.source.empty()) {
        std::filesystem::copy_file(asset.source, temporary, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) throw std::runtime_error("Unable to stage asset " + asset.source.string() + ": " + ec.message());
    } else {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Unable to create generated package asset: " + temporary.string());
        if (!asset.data.empty()) {
            output.write(reinterpret_cast<const char*>(asset.data.data()), static_cast<std::streamsize>(asset.data.size()));
        }
        if (!output) throw std::runtime_error("Unable to write generated package asset: " + temporary.string());
    }
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("Unable to install generated package asset " + target.string() + ": " + ec.message());
    }
}

std::filesystem::path validateOutputIniPath(const std::filesystem::path& input) {
    if (input.empty()) throw std::runtime_error("The installer INI path is empty.");
    std::filesystem::path result = input;
    if (result.extension().empty()) result += ".ini";
    if (!iequals(result.extension().string(), ".ini")) {
        throw std::runtime_error("The selected installer configuration must use the .ini extension: " + result.string());
    }
    return result;
}

} // namespace

IniSection& PatchProject::section(const std::string& name) {
    for (auto& section : sections) {
        if (iequals(section.name, name)) return section;
    }
    sections.push_back({name, {}});
    return sections.back();
}

const IniSection* PatchProject::findSection(const std::string& name) const {
    for (const auto& section : sections) {
        if (iequals(section.name, name)) return &section;
    }
    return nullptr;
}

void PatchProject::add(const std::string& sectionName, std::string key, std::string value) {
    section(sectionName).entries.push_back({std::move(key), std::move(value)});
}

bool PatchProject::emptyInstructions() const {
    for (const auto& section : sections) {
        if (!section.entries.empty()) return false;
    }
    return assets.empty();
}

std::string sanitizeSectionName(std::string value) {
    for (char& ch : value) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (!std::isalnum(u) && ch != '_' && ch != '-' && ch != '.') ch = '_';
    }
    while (!value.empty() && value.front() == '_') value.erase(value.begin());
    while (!value.empty() && value.back() == '_') value.pop_back();
    if (value.empty()) return "section";
    return value;
}

std::string basenameForPatch(const std::filesystem::path& path) {
    std::string name = path.filename().string();
    if (name.empty()) throw std::runtime_error("Unable to infer a patch filename from an empty path.");
    return name;
}

std::pair<std::size_t, std::size_t> serializedIniCounts(const PatchProject& project,
                                                        bool includeSettings) {
    std::size_t sections = 0u;
    std::size_t entries = 0u;
    std::set<std::string> emitted;

    const auto countSection = [&](const IniSection& section) {
        const std::string name = lowerAscii(section.name);
        if (emitted.count(name) != 0u || (!includeSettings && name == "settings")) return;
        if (name != "settings" && section.entries.empty()) return;
        emitted.insert(name);
        ++sections;
        if (name == "settings" && includeSettings) ++entries;
        for (const auto& entry : section.entries) {
            if (name == "settings" && iequals(entry.key, "InstallerMode")) continue;
            ++entries;
        }
    };

    static const std::vector<std::string> preferred = {
        "Settings", "TLKList", "InstallList", "2DAList", "GFFList", "CompileList", "SSFList"};
    for (const auto& name : preferred) {
        if (const auto* section = project.findSection(name)) countSection(*section);
        else if (includeSettings && name == "Settings") countSection(IniSection{"Settings", {}});
    }
    for (const auto& section : project.sections) countSection(section);
    return {sections, entries};
}

std::string writeIniText(const PatchProject& project, bool includeSettings) {
    std::ostringstream out;
    out << "; Neo tool generated TSLPatcher/HoloPatcher instructions\r\n\r\n";
    const std::vector<std::string> preferred = {"Settings", "TLKList", "InstallList", "2DAList", "GFFList", "CompileList", "SSFList"};
    std::set<std::string> emitted;
    auto emitSection = [&](const IniSection& section) {
        const std::string lname = lowerAscii(section.name);
        if (emitted.count(lname) != 0u || (!includeSettings && lname == "settings")) return;
        if (lname != "settings" && section.entries.empty()) return;
        emitted.insert(lname);
        out << '[' << section.name << "]\r\n";
        if (includeSettings && lname == "settings") out << "InstallerMode=1\r\n";
        for (const auto& kv : section.entries) {
            if (lname == "settings" && iequals(kv.key, "InstallerMode")) continue;
            out << kv.key << '=' << encodeIniText(kv.value) << "\r\n";
        }
        out << "\r\n";
    };
    for (const auto& name : preferred) {
        if (const auto* section = project.findSection(name)) emitSection(*section);
        else if (includeSettings && name == "Settings") emitSection(IniSection{"Settings", {}});
    }
    for (const auto& section : project.sections) emitSection(section);
    return out.str();
}

std::string writeIniFragmentText(const PatchProject& project) {
    std::ostringstream out;
    bool firstSection = true;
    const std::vector<std::string> preferred = {
        "TLKList", "InstallList", "2DAList", "GFFList", "CompileList", "SSFList"};
    std::set<std::string> emitted;

    const auto emitSection = [&](const IniSection& section) {
        const std::string name = lowerAscii(section.name);
        if (name == "settings" || section.entries.empty() || emitted.count(name) != 0u) return;
        emitted.insert(name);
        if (!firstSection) out << "\r\n";
        firstSection = false;
        out << '[' << section.name << "]\r\n";
        for (const auto& entry : section.entries) {
            out << entry.key << '=' << encodeIniText(entry.value) << "\r\n";
        }
    };

    for (const auto& name : preferred) {
        if (const auto* section = project.findSection(name)) emitSection(*section);
    }
    for (const auto& section : project.sections) emitSection(section);
    return out.str();
}

IniMergeReport preflightIniMerge(const PatchProject& project,
                                 const std::filesystem::path& requestedPath,
                                 bool includeSettings) {
    const std::filesystem::path path = validateOutputIniPath(requestedPath);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        IniMergeReport report;
        report.iniPath = path;
        report.mergedExisting = false;
        const auto [sections, entries] = serializedIniCounts(project, includeSettings);
        report.sectionsAdded = sections;
        report.entriesAdded = entries;
        return report;
    }
    auto document = ParsedIniDocument::load(path);
    return mergeProjectIntoDocument(project, document, path, includeSettings);
}

IniMergeReport writeIniFile(const PatchProject& project,
                            const std::filesystem::path& requestedPath,
                            bool includeSettings) {
    const std::filesystem::path path = validateOutputIniPath(requestedPath);
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) throw std::runtime_error("Unable to create installer INI folder: " + path.parent_path().string() + ": " + ec.message());
    }

    const bool exists = std::filesystem::exists(path, ec) && !ec;
    if (!exists) {
        IniMergeReport report;
        report.iniPath = path;
        report.mergedExisting = false;
        const std::string text = writeIniText(project, includeSettings);
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const std::filesystem::path temporary = path.string() + ".neo-tmp-" + std::to_string(stamp);
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("Unable to create installer INI: " + temporary.string());
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!output) throw std::runtime_error("Unable to write installer INI: " + temporary.string());
        }
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            std::filesystem::remove(temporary);
            throw std::runtime_error("Unable to install generated INI: " + path.string() + ": " + ec.message());
        }
        const auto [sections, entries] = serializedIniCounts(project, includeSettings);
        report.sectionsAdded = sections;
        report.entriesAdded = entries;
        return report;
    }

    auto document = ParsedIniDocument::load(path);
    IniMergeReport report = mergeProjectIntoDocument(project, document, path, includeSettings);
    if (report.sectionsAdded != 0u || report.entriesAdded != 0u || report.entriesUpdated != 0u) {
        document.saveAtomically(path);
    }
    return report;
}

IniMergeReport writePackageToIni(const PatchProject& project,
                                 const std::filesystem::path& requestedIni,
                                 bool includeSettings) {
    const std::filesystem::path outputIni = validateOutputIniPath(requestedIni);
    const std::filesystem::path outputDir = outputIni.parent_path().empty()
        ? std::filesystem::current_path()
        : outputIni.parent_path();

    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) throw std::runtime_error("Unable to create TSLPatcher package folder: " + outputDir.string() + ": " + ec.message());

    std::unordered_set<std::string> incomingAssetNames;
    for (const auto& asset : project.assets) {
        if (asset.targetName.empty()) continue;
        const std::string key = lowerAscii(std::filesystem::path(asset.targetName).lexically_normal().generic_string());
        if (!incomingAssetNames.insert(key).second) {
            throw std::runtime_error("Generated package contains more than one payload named " + asset.targetName + ".");
        }
    }

    (void)preflightIniMerge(project, outputIni, includeSettings);
    std::vector<std::string> assetNotes;
    for (const auto& asset : project.assets) {
        if (asset.targetName.empty()) continue;
        const std::filesystem::path relative(asset.targetName);
        if (relative.is_absolute() || relative.empty() ||
            std::any_of(relative.begin(), relative.end(), [](const std::filesystem::path& component) {
                return component == "..";
            })) {
            throw std::runtime_error("Package asset target must be a safe relative path: " + asset.targetName);
        }
        if (iequals(relative.lexically_normal().generic_string(), outputIni.filename().generic_string())) {
            throw std::runtime_error("Package asset collides with the selected installer INI filename: " + asset.targetName);
        }
        const std::filesystem::path target = outputDir / relative;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) throw std::runtime_error("Unable to create asset folder: " + target.parent_path().string() + ": " + ec.message());
        if (std::filesystem::exists(target, ec) && !ec) {
            if (!fileMatchesAsset(target, asset)) {
                throw std::runtime_error(
                    "The package already contains a different file named " + relative.generic_string() +
                    ". Neo tools will not overwrite an existing payload while merging an installer INI. "
                    "Reconcile or rename that payload, or choose another tslpatchdata folder.");
            }
            assetNotes.push_back("Retained identical package asset " + relative.generic_string() + ".");
        }
    }

    for (const auto& asset : project.assets) {
        if (asset.targetName.empty()) continue;
        const std::filesystem::path target = outputDir / std::filesystem::path(asset.targetName);
        if (std::filesystem::exists(target, ec) && !ec) continue;
        writeGeneratedAssetAtomically(asset, target);
    }

    const std::filesystem::path infoPath = outputDir / "info.rtf";
    if (!std::filesystem::exists(infoPath)) {
        static constexpr const char kDefaultInfoRtf[] =
            "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Arial;}}"
            "\\viewkind4\\uc1\\pard\\f0\\fs20 Neo tool patch package.\\par}\r\n";
        StagedAsset infoAsset;
        infoAsset.targetName = "info.rtf";
        infoAsset.data.assign(kDefaultInfoRtf, kDefaultInfoRtf + sizeof(kDefaultInfoRtf) - 1u);
        writeGeneratedAssetAtomically(infoAsset, infoPath);
    }

    IniMergeReport report = writeIniFile(project, outputIni, includeSettings);
    report.notes.insert(report.notes.end(), assetNotes.begin(), assetNotes.end());
    return report;
}

IniMergeReport writePackage(const PatchProject& project,
                            const std::filesystem::path& outputDir,
                            bool includeSettings) {
    return writePackageToIni(project, outputDir / "changes.ini", includeSettings);
}

IniMergeReport writeFragment(const PatchProject& project,
                             const std::filesystem::path& requestedIni) {
    const std::filesystem::path outputIni = validateOutputIniPath(requestedIni);
    std::error_code ec;
    if (std::filesystem::exists(outputIni, ec)) {
        throw std::runtime_error(
            "Fragment mode only creates a new INI file and will not overwrite or merge into an existing file: " +
            outputIni.string());
    }
    if (ec) {
        throw std::runtime_error(
            "Unable to check the selected fragment output path: " + ec.message());
    }

    const std::filesystem::path parent = outputIni.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            throw std::runtime_error(
                "Unable to create the fragment output directory: " + ec.message());
        }
    }

    const std::string text = writeIniFragmentText(project);
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path temporary =
        outputIni.string() + ".neo-fragment-tmp-" + std::to_string(stamp);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error(
                "Unable to create the temporary fragment file: " + temporary.string());
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output) {
            std::filesystem::remove(temporary);
            throw std::runtime_error(
                "Unable to write the temporary fragment file: " + temporary.string());
        }
    }

    // copy_file without overwrite_existing provides a portable create-new
    // operation. Fragment mode must never replace or merge with an existing INI.
    std::filesystem::copy_file(
        temporary,
        outputIni,
        std::filesystem::copy_options::none,
        ec);
    std::filesystem::remove(temporary);
    if (ec) {
        throw std::runtime_error(
            "Unable to create the new fragment INI without overwriting an existing file: " +
            outputIni.string() + " (" + ec.message() + ")");
    }

    IniMergeReport report;
    report.iniPath = outputIni;
    report.mergedExisting = false;
    for (const auto& section : project.sections) {
        if (iequals(section.name, "Settings") || section.entries.empty()) continue;
        ++report.sectionsAdded;
        report.entriesAdded += section.entries.size();
    }
    return report;
}

std::vector<KeyValue> readIniSectionEntries(const std::filesystem::path& path,
                                            const std::string& sectionName) {
    if (!std::filesystem::exists(path)) return {};
    const auto document = ParsedIniDocument::load(path);
    const auto* section = document.findUniqueSection(sectionName);
    if (!section) return {};
    std::vector<KeyValue> result;
    result.reserve(section->entries.size());
    for (const auto& entry : section->entries) result.push_back({entry.key, entry.value});
    return result;
}

std::optional<std::string> readIniValue(const std::filesystem::path& path,
                                        const std::string& sectionName,
                                        const std::string& key) {
    if (!std::filesystem::exists(path)) return std::nullopt;
    const auto document = ParsedIniDocument::load(path);
    const auto* section = document.findUniqueSection(sectionName);
    if (!section) return std::nullopt;
    const auto* entry = ParsedIniDocument::findEntry(*section, key);
    if (!entry) return std::nullopt;
    return entry->value;
}

void throwIfUnsupported(const PatchProject& project) {
    if (project.unsupported.empty()) return;
    std::ostringstream out;
    out << "TSLPatcher/HoloPatcher instruction generation found unsupported changes:";
    for (const auto& item : project.unsupported) out << "\n  - " << item;
    throw std::runtime_error(out.str());
}

void printReport(const PatchProject& project) {
    if (project.unsupported.empty() && project.warnings.empty()) return;
    if (!project.unsupported.empty()) {
        std::cerr << "Unsupported changes:\n";
        for (const auto& item : project.unsupported) std::cerr << "  - " << item << '\n';
    }
    if (!project.warnings.empty()) {
        std::cerr << "Warnings:\n";
        for (const auto& item : project.warnings) std::cerr << "  - " << item << '\n';
    }
}

PatchProject diffGffFlatTable(const neotabular::Table& original,
                              const neotabular::Table& modified,
                              const std::string& patchFilename,
                              bool copyBaselineAsset,
                              const std::filesystem::path& baselineAsset) {
    PatchProject project;
    project.add("GFFList", "File0", patchFilename);
    project.section(patchFilename);
    addAssetIfRequested(project, copyBaselineAsset, baselineAsset, patchFilename);

    const auto orig = gffRowsByPath(original);
    const auto mod = gffRowsByPath(modified);
    const std::string stem = baseNameNoExt(patchFilename);

    // GFF paths containing duplicate-label occurrence suffixes are an internal
    // NeoGFF representation. Neither original TSLPatcher nor HoloPatcher can
    // address the second or later field with the same label.
    for (const auto& [path, before] : orig) {
        const auto after = mod.find(path);
        if (after == mod.end()) continue;
        if (pathHasDuplicateOccurrence(path) &&
            (before.type != after->second.type || before.value != after->second.value)) {
            project.unsupported.push_back(
                "Duplicate-label GFF fields cannot be addressed unambiguously by TSLPatcher/HoloPatcher: " + path);
        }
    }

    std::vector<std::string> removedRoots;
    for (const auto& [path, row] : orig) {
        if (mod.find(path) == mod.end()) removedRoots.push_back(path);
    }
    std::sort(removedRoots.begin(), removedRoots.end(), [](const std::string& lhs, const std::string& rhs) {
        if (lhs.size() != rhs.size()) return lhs.size() < rhs.size();
        return lhs < rhs;
    });
    removedRoots.erase(
        std::remove_if(removedRoots.begin(), removedRoots.end(), [&](const std::string& candidate) {
            return std::any_of(removedRoots.begin(), removedRoots.end(), [&](const std::string& root) {
                return root != candidate && root.size() < candidate.size() && pathBelongsToSubtree(candidate, root);
            });
        }),
        removedRoots.end());
    for (const auto& path : removedRoots) {
        project.unsupported.push_back(
            "GFF field or list-item deletion requires complete-file installation: " + path);
    }

    std::vector<std::string> newRoots;
    for (const auto& [path, row] : mod) {
        if (orig.find(path) != orig.end()) continue;
        const std::string parent = structuralParentPath(path);
        if (parent.empty() || orig.find(parent) != orig.end()) newRoots.push_back(path);
    }
    std::sort(newRoots.begin(), newRoots.end(), [&](const std::string& lhs, const std::string& rhs) {
        return mod.at(lhs).order < mod.at(rhs).order;
    });

    auto validateNewSubtree = [&](const std::string& root, std::vector<std::string>& reasons) {
        for (const auto& [path, row] : mod) {
            if (orig.find(path) != orig.end() || !pathBelongsToSubtree(path, root)) continue;
            if (pathHasDuplicateOccurrence(path)) {
                reasons.push_back("duplicate-label field " + path);
                continue;
            }
            if (isLocStringChildPath(path)) continue;
            if (!originalTslCompatibleAddFieldType(row.type)) {
                reasons.push_back("unsupported field type " + row.type + " at " + path);
                continue;
            }
            if (lowerAscii(gffPatchType(row.type)) == "struct" && !structTypeIdFromSummary(row.value)) {
                reasons.push_back("missing or invalid Struct TypeId at " + path);
            }
        }
    };

    std::set<std::string> rejectedRoots;
    for (const auto& root : newRoots) {
        std::vector<std::string> reasons;
        validateNewSubtree(root, reasons);
        if (reasons.empty()) continue;
        rejectedRoots.insert(root);
        std::ostringstream message;
        message << "Cannot represent new GFF subtree " << root << " for both original TSLPatcher and HoloPatcher:";
        for (const auto& reason : reasons) message << " " << reason << ";";
        project.unsupported.push_back(message.str());
    }

    std::set<std::string> emitted;
    std::function<void(const std::string&, const std::string&, bool)> emitNewField;
    emitNewField = [&](const std::string& path, const std::string& ownerSection, bool nested) {
        if (!emitted.insert(path).second) return;
        const auto rowIt = mod.find(path);
        if (rowIt == mod.end()) return;
        const GffRow& row = rowIt->second;
        const std::string type = gffPatchType(row.type);
        const std::string typeKey = lowerAscii(type);
        const std::string parentPath = structuralParentPath(path);
        const auto parentIt = mod.find(parentPath);
        const std::string parentType = parentIt == mod.end() ? std::string{} : parentIt->second.type;

        const std::string sectionName = uniqueSectionName(project, "gff_" + stem + "_" + leafOf(path));
        addNumberedEntry(project, ownerSection, "AddField", sectionName);
        project.add(sectionName, "FieldType", type);
        if (!nested) project.add(sectionName, "Path", parentPath);
        project.add(sectionName, "Label", patchLabelForRow(row, parentType));

        if (typeKey == "struct") {
            const std::uint32_t typeId = *structTypeIdFromSummary(row.value);
            const auto listIndex = unsignedDecimal(leafOf(path));
            if (lowerAscii(gffPatchType(parentType)) == "list" && listIndex && *listIndex == typeId) {
                project.add(sectionName, "TypeId", "ListIndex");
            } else {
                project.add(sectionName, "TypeId", std::to_string(typeId));
            }
        } else if (typeKey == "exolocstring") {
            const auto strref = mod.find(path + "(strref)");
            project.add(sectionName, "StrRef", strref == mod.end() ? "-1" : strref->second.value);
            std::vector<const GffRow*> localized;
            for (const auto& [candidatePath, candidate] : mod) {
                if (pathIsLocStringChildOf(candidatePath, path) &&
                    candidatePath.rfind(path + "(lang", 0) == 0) {
                    localized.push_back(&candidate);
                }
            }
            std::sort(localized.begin(), localized.end(), [](const GffRow* lhs, const GffRow* rhs) {
                return lhs->order < rhs->order;
            });
            for (const GffRow* child : localized) {
                const std::size_t begin = child->path.rfind("(lang") + 1u;
                project.add(sectionName, child->path.substr(begin, child->path.size() - begin - 1u), child->value);
                emitted.insert(child->path);
            }
            emitted.insert(path + "(strref)");
        } else if (typeKey != "list") {
            project.add(sectionName, "Value", row.value);
        }

        if (typeKey == "struct" || typeKey == "list") {
            std::vector<const GffRow*> children;
            for (const auto& [candidatePath, candidate] : mod) {
                if (orig.find(candidatePath) != orig.end() || isLocStringChildPath(candidatePath)) continue;
                if (structuralParentPath(candidatePath) == path) children.push_back(&candidate);
            }
            std::sort(children.begin(), children.end(), [](const GffRow* lhs, const GffRow* rhs) {
                return lhs->order < rhs->order;
            });
            for (const GffRow* child : children) emitNewField(child->path, sectionName, true);
        }
    };

    for (const auto& root : newRoots) {
        if (rejectedRoots.count(root) || isLocStringChildPath(root)) continue;
        emitNewField(root, patchFilename, false);
    }

    for (const auto& [path, row] : mod) {
        const auto found = orig.find(path);
        if (found == orig.end()) {
            if (emitted.count(path)) continue;
            if (isLocStringChildPath(path)) {
                const std::string parent = locStringParentPath(path);
                if (orig.find(parent) != orig.end()) {
                    if (lowerAscii(row.type) == "cexolocstring text" && containsLineBreak(row.value)) {
                        project.unsupported.push_back(
                            "HoloPatcher 1.7 cannot decode <#LF#>/<#CR#> tokens for an existing localized-string substring: " + path +
                            ". Install the complete resource or use a single-line value.");
                    } else {
                        addPlainEntry(project, patchFilename, path, row.value);
                    }
                }
            }
            continue;
        }

        if (!iequals(found->second.type, row.type)) {
            project.unsupported.push_back(
                "GFF field type change requires complete-file installation at " + path + ": " +
                found->second.type + " -> " + row.type);
            continue;
        }
        if (found->second.value == row.value) continue;
        if (pathHasDuplicateOccurrence(path)) continue;
        if (!(truthy(row.editable) || gffTypeIsEditableText(row.type) || isLocStringChildPath(path))) continue;

        if (isLocStringChildPath(path)) {
            if (lowerAscii(row.type) == "cexolocstring text" && containsLineBreak(row.value)) {
                project.unsupported.push_back(
                    "HoloPatcher 1.7 cannot decode <#LF#>/<#CR#> tokens for an existing localized-string substring: " + path +
                    ". Install the complete resource or use a single-line value.");
            } else {
                addPlainEntry(project, patchFilename, path, row.value);
            }
            continue;
        }
        if (lowerAscii(gffPatchType(row.type)) == "exolocstring") continue;
        if (!originalTslCompatibleDirectFieldType(row.type)) {
            project.unsupported.push_back(
                "TSLPatcher/HoloPatcher cannot directly modify GFF field type " + row.type +
                " at " + path + ". Use a complete-file installation.");
            continue;
        }
        addPlainEntry(project, patchFilename, path, row.value);
    }

    return project;
}

PatchProject diffTwoDA(const neotabular::Table& original,
                       const neotabular::Table& modified,
                       const std::string& patchFilename,
                       bool copyBaselineAsset,
                       const std::filesystem::path& baselineAsset) {
    if (original.columns.empty() || modified.columns.empty()) throw std::runtime_error("2DA diff requires non-empty tables.");
    PatchProject project;
    project.add("2DAList", "Table0", patchFilename);
    project.section(patchFilename);
    addAssetIfRequested(project, copyBaselineAsset, baselineAsset, patchFilename);

    const auto origCols = dataColumns(original);
    const auto modCols = dataColumns(modified);
    const std::string stem = baseNameNoExt(patchFilename);

    const std::size_t commonCols = std::min(origCols.size(), modCols.size());
    for (std::size_t c = 0; c < commonCols; ++c) {
        if (!iequals(origCols[c], modCols[c])) {
            project.unsupported.push_back("2DA column rename/reorder at column " + std::to_string(c) + ": " + origCols[c] + " -> " + modCols[c]);
        }
    }
    if (origCols.size() > modCols.size()) {
        for (std::size_t c = modCols.size(); c < origCols.size(); ++c) {
            project.unsupported.push_back("2DA deleted column is not representable: " + origCols[c]);
        }
    }
    if (original.rows.size() > modified.rows.size()) {
        for (std::size_t r = modified.rows.size(); r < original.rows.size(); ++r) {
            project.unsupported.push_back("2DA deleted row is not representable at original row " + std::to_string(r));
        }
    }

    for (std::size_t c = origCols.size(); c < modCols.size(); ++c) {
        const std::string sectionName = uniqueSectionName(project, stem + "_col_" + modCols[c]);
        addNumberedEntry(project, patchFilename, "AddColumn", sectionName);
        project.add(sectionName, "ColumnLabel", modCols[c]);
        project.add(sectionName, "DefaultValue", "****");
        // AddColumn executes before AddRow in the generated file section.
        // Only address rows that already exist in the destination table here;
        // values for newly appended rows are emitted by their AddRow sections.
        for (std::size_t r = 0; r < original.rows.size(); ++r) {
            const std::string value = cellOrEmpty(modified.rows[r], c + 1);
            if (!value.empty() && value != "****") project.add(sectionName, "I" + std::to_string(r), value);
        }
    }

    const std::size_t rowCommon = std::min(original.rows.size(), modified.rows.size());
    for (std::size_t r = 0; r < rowCommon; ++r) {
        bool any = false;
        std::string sectionName;
        for (std::size_t c = 0; c < commonCols; ++c) {
            if (tableRowsEqualAt(original, modified, r, c + 1)) continue;
            if (!any) {
                any = true;
                sectionName = uniqueSectionName(project, stem + "_mod_row_" + std::to_string(r));
                addNumberedEntry(project, patchFilename, "ChangeRow", sectionName);
                project.add(sectionName, "RowIndex", std::to_string(r));
            }
            project.add(sectionName, modCols[c], cellOrEmpty(modified.rows[r], c + 1));
        }
        if (cellOrEmpty(original.rows[r], 0) != cellOrEmpty(modified.rows[r], 0)) {
            project.unsupported.push_back("2DA row label change at row " + std::to_string(r) + " is not a stock TSLPatcher row operation.");
        }
    }

    for (std::size_t r = original.rows.size(); r < modified.rows.size(); ++r) {
        const std::string sectionName = uniqueSectionName(project, stem + "_add_row_" + std::to_string(r));
        addNumberedEntry(project, patchFilename, "AddRow", sectionName);
        const std::string rowLabel = cellOrEmpty(modified.rows[r], 0);
        if (!rowLabel.empty()) project.add(sectionName, "RowLabel", rowLabel);
        for (std::size_t c = 0; c < modCols.size(); ++c) {
            const std::string value = cellOrEmpty(modified.rows[r], c + 1);
            if (!value.empty() && value != "****") project.add(sectionName, modCols[c], value);
        }
    }
    return project;
}


PatchProject diffSsfTable(const neotabular::Table& original,
                          const neotabular::Table& modified,
                          const std::string& patchFilename,
                          bool copyBaselineAsset,
                          const std::filesystem::path& baselineAsset) {
    PatchProject project;
    project.add("SSFList", "File0", patchFilename);
    project.section(patchFilename);
    addAssetIfRequested(project, copyBaselineAsset, baselineAsset, patchFilename);

    // These 28 named slots are the intersection supported by both the original
    // TSLPatcher and HoloPatcher 1.7. The original patcher also accepts
    // Unknown(29)-Unknown(40), but HoloPatcher 1.7 has no mapping for them.
    static constexpr const char* kSoundNames[] = {
        "Battlecry 1", "Battlecry 2", "Battlecry 3", "Battlecry 4", "Battlecry 5", "Battlecry 6",
        "Selected 1", "Selected 2", "Selected 3", "Attack 1", "Attack 2", "Attack 3",
        "Pain 1", "Pain 2", "Low health", "Death", "Critical hit", "Target immune",
        "Place mine", "Disarm mine", "Stealth on", "Search", "Pick lock start", "Pick lock fail",
        "Pick lock done", "Leave party", "Rejoin party", "Poisoned"
    };

    const std::size_t indexCol = requiredColumn(modified, "Index");
    const std::size_t strRefCol = requiredColumn(modified, "StrRef");
    const std::size_t origStrRefCol = requiredColumn(original, "StrRef");
    const std::size_t soundFileCol = optionalColumn(modified, "SoundFile");
    const std::size_t origSoundFileCol = optionalColumn(original, "SoundFile");
    const std::size_t common = std::min(original.rows.size(), modified.rows.size());
    for (std::size_t r = 0; r < common; ++r) {
        if (soundFileCol < modified.columns.size() && origSoundFileCol < original.columns.size() &&
            cellOrEmpty(original.rows[r], origSoundFileCol) != cellOrEmpty(modified.rows[r], soundFileCol)) {
            project.unsupported.push_back(
                "SSFList cannot modify a direct Sound ResRef: row " + std::to_string(r + 1u));
        }

        const std::string oldValue = cellOrEmpty(original.rows[r], origStrRefCol);
        const std::string newValue = cellOrEmpty(modified.rows[r], strRefCol);
        if (oldValue == newValue) continue;

        std::size_t oneBasedIndex = 0;
        const std::string indexText = cellOrEmpty(modified.rows[r], indexCol);
        bool validIndex = !indexText.empty();
        for (char ch : indexText) {
            if (ch < '0' || ch > '9') { validIndex = false; break; }
            oneBasedIndex = oneBasedIndex * 10u + static_cast<std::size_t>(ch - '0');
        }
        if (!validIndex || oneBasedIndex < 1u || oneBasedIndex > std::size(kSoundNames)) {
            project.unsupported.push_back(
                "A package compatible with both original TSLPatcher and HoloPatcher 1.7 can modify only "
                "the 28 named KotOR SSF slots; row " + std::to_string(r + 1u) +
                " cannot be emitted by SSFList.");
            continue;
        }
        addPlainEntry(project, patchFilename, kSoundNames[oneBasedIndex - 1u], newValue.empty() ? "-1" : newValue);
    }
    if (modified.rows.size() > original.rows.size()) {
        for (std::size_t r = original.rows.size(); r < modified.rows.size(); ++r) {
            project.unsupported.push_back("SSF extra/trailing slot cannot be guaranteed stock-compatible: row " + std::to_string(r));
        }
    }
    if (original.rows.size() > modified.rows.size()) {
        for (std::size_t r = modified.rows.size(); r < original.rows.size(); ++r) {
            project.unsupported.push_back("SSF deleted slot is not representable: row " + std::to_string(r));
        }
    }
    return project;
}


} // namespace neotsl
