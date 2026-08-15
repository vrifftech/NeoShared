#pragma once

#include "TabularData.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neotsl {

struct KeyValue {
    std::string key;
    std::string value;
};

struct IniSection {
    std::string name;
    std::vector<KeyValue> entries;
};

struct StagedAsset {
    std::filesystem::path source;
    std::string targetName;
    std::vector<std::uint8_t> data;

    bool generated() const noexcept { return source.empty(); }
};

struct PatchProject {
    std::vector<IniSection> sections;
    std::vector<StagedAsset> assets;
    std::vector<std::string> warnings;
    std::vector<std::string> unsupported;

    IniSection& section(const std::string& name);
    const IniSection* findSection(const std::string& name) const;
    void add(const std::string& sectionName, std::string key, std::string value);
    bool emptyInstructions() const;
};

struct DiffOptions {
    std::string patchFilename;
    std::string destination = "override";
    bool includeSettings = true;
    bool copyBaselineAsset = true;
    bool failOnUnsupported = true;
};

struct IniMergeReport {
    std::filesystem::path iniPath;
    bool mergedExisting = false;
    std::size_t sectionsAdded = 0;
    std::size_t entriesAdded = 0;
    std::size_t entriesUpdated = 0;
    std::vector<std::string> notes;
};

std::string sanitizeSectionName(std::string value);
std::string basenameForPatch(const std::filesystem::path& path);
std::string writeIniText(const PatchProject& project, bool includeSettings = true);

// Returns paste-ready generated instructions without [Settings], package files,
// merge-time renumbering, or generated comments.
std::string writeIniFragmentText(const PatchProject& project);

// Writes a new INI or merges the generated instructions into an existing INI.
// Unrelated sections, keys, comments, and install-option content are retained.
IniMergeReport writeIniFile(const PatchProject& project,
                            const std::filesystem::path& path,
                            bool includeSettings = true);

// Validates and plans a merge without changing the INI or package assets.
IniMergeReport preflightIniMerge(const PatchProject& project,
                                 const std::filesystem::path& path,
                                 bool includeSettings = true);

// Writes a package to outputDir/changes.ini. Existing INI content and identical
// payload files are retained; conflicting payload files are rejected.
IniMergeReport writePackage(const PatchProject& project,
                            const std::filesystem::path& outputDir,
                            bool includeSettings = true);

// Writes a package using the selected INI path. The INI filename may be any
// portable .ini name, allowing multiple installer options in one tslpatchdata
// directory.
IniMergeReport writePackageToIni(const PatchProject& project,
                                 const std::filesystem::path& outputIni,
                                 bool includeSettings = true);

// Writes the generated fragment to a new INI file. Fragment output never
// merges with or overwrites an existing file and never stages package assets.
IniMergeReport writeFragment(const PatchProject& project,
                             const std::filesystem::path& outputIni);

std::vector<KeyValue> readIniSectionEntries(const std::filesystem::path& path,
                                            const std::string& sectionName);
std::optional<std::string> readIniValue(const std::filesystem::path& path,
                                        const std::string& sectionName,
                                        const std::string& key);

void throwIfUnsupported(const PatchProject& project);
void printReport(const PatchProject& project);


PatchProject diffTwoDA(const neotabular::Table& original,
                       const neotabular::Table& modified,
                       const std::string& patchFilename,
                       bool copyBaselineAsset = true,
                       const std::filesystem::path& baselineAsset = {});

PatchProject diffSsfTable(const neotabular::Table& original,
                          const neotabular::Table& modified,
                          const std::string& patchFilename,
                          bool copyBaselineAsset = true,
                          const std::filesystem::path& baselineAsset = {});

PatchProject diffGffFlatTable(const neotabular::Table& original,
                              const neotabular::Table& modified,
                              const std::string& patchFilename,
                              bool copyBaselineAsset = true,
                              const std::filesystem::path& baselineAsset = {});

} // namespace neotsl
