#include "GffTypeNames.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace neogff {
namespace {

struct GffTypeNameEntry {
    std::uint32_t type;
    const char* display;
    const char* xmlTag;
    bool editable;
};

constexpr GffTypeNameEntry kGffTypes[] = {
    {FIELD_TYPE_BYTE, "Byte", "byte", true},
    {FIELD_TYPE_CHAR, "Char", "char", true},
    {FIELD_TYPE_WORD, "Word", "uint16_t", true},
    {FIELD_TYPE_SHORT, "Short", "sint16", true},
    {FIELD_TYPE_DWORD, "DWORD", "uint32_t", true},
    {FIELD_TYPE_INT, "Int", "sint32", true},
    {FIELD_TYPE_DWORD64, "DWORD64", "uint64_t", true},
    {FIELD_TYPE_INT64, "Int64", "sint64", true},
    {FIELD_TYPE_FLOAT, "Float", "float", true},
    {FIELD_TYPE_DOUBLE, "Double", "double", true},
    {FIELD_TYPE_CEXOSTRING, "CExoString", "exostring", true},
    {FIELD_TYPE_RESREF, "CResRef", "resref", true},
    {FIELD_TYPE_CEXOLOCSTRING, "CExoLocString", "locstring", false},
    {FIELD_TYPE_VOID, "Void", "data", false},
    {FIELD_TYPE_STRUCT, "Struct", "struct", false},
    {FIELD_TYPE_LIST, "List", "list", false},
    {FIELD_TYPE_ORIENTATION, "Orientation", "orientation", true},
    {FIELD_TYPE_POSITION, "Position", "vector", true},
    {FIELD_TYPE_JADE_STRREF, "JadeStringRef", "strref", true},
};

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::string trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

const GffTypeNameEntry* findByType(std::uint32_t type) noexcept {
    for (const auto& entry : kGffTypes) {
        if (entry.type == type) return &entry;
    }
    return nullptr;
}

const GffTypeNameEntry* findByDisplayOrAlias(const std::string& typeName) noexcept {
    const std::string key = lowerAscii(trim(typeName));
    for (const auto& entry : kGffTypes) {
        if (lowerAscii(entry.display) == key || lowerAscii(entry.xmlTag) == key) return &entry;
    }
    if (key == "uint16") return findByType(FIELD_TYPE_WORD);
    if (key == "sint16" || key == "int16") return findByType(FIELD_TYPE_SHORT);
    if (key == "uint32") return findByType(FIELD_TYPE_DWORD);
    if (key == "sint32" || key == "int32") return findByType(FIELD_TYPE_INT);
    if (key == "uint64") return findByType(FIELD_TYPE_DWORD64);
    if (key == "sint64") return findByType(FIELD_TYPE_INT64);
    if (key == "string") return findByType(FIELD_TYPE_CEXOSTRING);
    if (key == "binary") return findByType(FIELD_TYPE_VOID);
    if (key == "position") return findByType(FIELD_TYPE_POSITION);
    if (key == "jadestrref" || key == "jadetext" || key == "tlkstring") return findByType(FIELD_TYPE_JADE_STRREF);
    return nullptr;
}

} // namespace

std::string fieldTypeName(std::uint32_t type) {
    if (const auto* entry = findByType(type)) return entry->display;
    return "Unknown(" + std::to_string(type) + ")";
}

std::uint32_t fieldTypeFromName(const std::string& typeName) {
    if (const auto* entry = findByDisplayOrAlias(typeName)) return entry->type;
    throw std::invalid_argument("Unknown GFF field type: " + typeName);
}

bool isEditableFieldType(std::uint32_t type) {
    if (const auto* entry = findByType(type)) return entry->editable;
    return false;
}

std::vector<std::string> supportedFieldTypeNames() {
    std::vector<std::string> out;
    out.reserve(sizeof(kGffTypes) / sizeof(kGffTypes[0]));
    for (const auto& entry : kGffTypes) out.emplace_back(entry.display);
    return out;
}

std::string gffXmlTagForFieldType(std::uint32_t type) {
    if (const auto* entry = findByType(type)) return entry->xmlTag;
    return "filetype" + std::to_string(type);
}

std::string gffFieldTypeNameFromXmlTag(const std::string& tag) {
    if (const auto* entry = findByDisplayOrAlias(tag)) return entry->display;
    return tag;
}

std::string gffXmlTagFromFieldTypeName(const std::string& typeName) {
    if (const auto* entry = findByDisplayOrAlias(typeName)) return entry->xmlTag;
    throw std::invalid_argument("Unsupported GFF JSON field type: " + typeName);
}

} // namespace neogff
