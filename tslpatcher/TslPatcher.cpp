#include "TslPatcher.hpp"

#include <algorithm>
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

bool isLocStringChildType(const std::string& type) {
    const std::string key = lowerAscii(type);
    return key == "cexolocstring strref" || key == "cexolocstring text";
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

std::string writeIniText(const PatchProject& project, bool includeSettings) {
    std::ostringstream out;
    out << "; Neo tool generated TSLPatcher/HoloPatcher instructions\r\n\r\n";
    if (includeSettings && !project.findSection("Settings")) {
        out << "[Settings]\r\nFileExists=1\r\nInstallerMode=1\r\n\r\n";
    }
    const std::vector<std::string> preferred = {"Settings", "TLKList", "InstallList", "2DAList", "GFFList", "CompileList", "SSFList"};
    std::set<std::string> emitted;
    auto emitSection = [&](const IniSection& section) {
        const std::string lname = lowerAscii(section.name);
        if (emitted.count(lname)) return;
        emitted.insert(lname);
        out << '[' << section.name << "]\r\n";
        if (includeSettings && lname == "settings") {
            const bool hasFileExists = std::any_of(section.entries.begin(), section.entries.end(), [](const KeyValue& value) {
                return lowerAscii(value.key) == "fileexists";
            });
            const bool hasInstallerMode = std::any_of(section.entries.begin(), section.entries.end(), [](const KeyValue& value) {
                return lowerAscii(value.key) == "installermode";
            });
            if (!hasFileExists) out << "FileExists=1\r\n";
            if (!hasInstallerMode) out << "InstallerMode=1\r\n";
        }
        for (const auto& kv : section.entries) {
            out << kv.key << '=' << encodeIniText(kv.value) << "\r\n";
        }
        out << "\r\n";
    };
    for (const auto& name : preferred) {
        if (const auto* section = project.findSection(name)) emitSection(*section);
        else if (includeSettings && name != "Settings") out << '[' << name << "]\r\n\r\n";
    }
    for (const auto& section : project.sections) emitSection(section);
    if (!project.unsupported.empty()) {
        out << "; Unsupported changes detected by generator:\r\n";
        for (const auto& item : project.unsupported) out << "; - " << item << "\r\n";
    }
    if (!project.warnings.empty()) {
        out << "; Warnings:\r\n";
        for (const auto& item : project.warnings) out << "; - " << item << "\r\n";
    }
    return out.str();
}

void writeIniFile(const PatchProject& project, const std::filesystem::path& path, bool includeSettings) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Unable to open changes.ini output: " + path.string());
    const std::string text = writeIniText(project, includeSettings);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) throw std::runtime_error("Unable to write changes.ini output: " + path.string());
}

void writePackage(const PatchProject& project, const std::filesystem::path& outputDir, bool includeSettings) {
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) throw std::runtime_error("Unable to create TSLPatcher package folder: " + outputDir.string() + ": " + ec.message());

    for (const auto& asset : project.assets) {
        if (asset.targetName.empty()) continue;
        const std::filesystem::path target = outputDir / asset.targetName;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) throw std::runtime_error("Unable to create asset folder: " + target.parent_path().string() + ": " + ec.message());

        if (!asset.source.empty()) {
            std::filesystem::copy_file(asset.source, target, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                throw std::runtime_error(
                    "Unable to stage asset " + asset.source.string() + " as " + target.string() + ": " + ec.message());
            }
        } else {
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("Unable to create generated package asset: " + target.string());
            if (!asset.data.empty()) {
                out.write(reinterpret_cast<const char*>(asset.data.data()), static_cast<std::streamsize>(asset.data.size()));
            }
            if (!out) throw std::runtime_error("Unable to write generated package asset: " + target.string());
        }
    }

    const std::filesystem::path infoPath = outputDir / "info.rtf";
    if (!std::filesystem::exists(infoPath)) {
        static constexpr const char kDefaultInfoRtf[] =
            "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Arial;}}"
            "\\viewkind4\\uc1\\pard\\f0\\fs20 Neo tool patch package.\\par}\r\n";
        std::ofstream info(infoPath, std::ios::binary | std::ios::trunc);
        if (!info) throw std::runtime_error("Unable to create TSLPatcher info.rtf: " + infoPath.string());
        info.write(kDefaultInfoRtf, static_cast<std::streamsize>(sizeof(kDefaultInfoRtf) - 1u));
        if (!info) throw std::runtime_error("Unable to write TSLPatcher info.rtf: " + infoPath.string());
    }

    writeIniFile(project, outputDir / "changes.ini", includeSettings);
}

void writeFragment(const PatchProject& project, const std::filesystem::path& outputIni) {
    writeIniFile(project, outputIni, false);
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
