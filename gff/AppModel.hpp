#pragma once

#include "GFFFile.hpp"
#include "GffTypeNames.hpp"
#include <neotlk/TlkLookup.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "TabularData.hpp"

namespace neogff {

struct GffFieldRow {
    std::string path;
    std::string label;
    std::string type;
    std::string value;
    std::string resolved;
    bool editable = false;
    bool deletable = false;
};

class GffModel {
public:
    void newFile(const std::string& fileType = "UTC");
    void load(const std::filesystem::path& file);
    void save(const std::filesystem::path& file = {});

    void loadTlk(const std::filesystem::path& file);
    void clearTlk();
    const neotlk::TlkLookup& tlk() const noexcept { return tlk_; }

    std::vector<GffFieldRow> rows() const;
    neotabular::Table toTable() const;
    std::string toXml() const;
    void importXml(const std::string& xmlText);

    void setValue(const std::string& path, const std::string& value);
    void addField(const std::string& parentPath,
                  const std::string& label,
                  const std::string& type,
                  const std::string& value = {},
                  std::uint32_t structTypeId = 0);
    void deleteField(const std::string& path);

    const std::filesystem::path& filename() const noexcept { return file_.filename(); }
    std::string fileType() const { return file_.filetype(); }
    std::string version() const { return file_.version(); }
    bool loaded() const noexcept { return file_.loaded(); }
    bool dirty() const noexcept { return file_.dirty(); }

    GffFile& gff() noexcept { return file_; }
    const GffFile& gff() const noexcept { return file_; }

private:
    GffFile file_;
    neotlk::TlkLookup tlk_;
};

std::unique_ptr<GffField> createField(const std::string& label,
                                      const std::string& typeName,
                                      const std::string& value,
                                      std::uint32_t structTypeId = 0);
// File-extension helpers used by NeoGFF's open/save dialogs and CLI. These
// describe GFF-backed resources only; Jade's LYT, VIS, ART, BIP, AMP, and
// NDB resources and Dragon Age's BNK/VLM/shader/text payloads are different
// formats and intentionally do not appear in the GFF extension sets.
const std::vector<std::string>& knownGffResourceExtensions();
const std::vector<std::string>& jadeEmpireGffResourceExtensions();
const std::vector<std::string>& dragonAgeGff4ResourceExtensions();
bool isKnownGffResourceExtension(std::string extension);
bool isKnownDragonAgeGff4ResourceExtension(std::string extension);
std::string defaultGffTypeForExtension(const std::filesystem::path& file);
std::string preferredGffExtensionForType(std::string fileType);

} // namespace neogff
