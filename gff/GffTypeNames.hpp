#pragma once

#include "GFFFile.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace neogff {

std::string fieldTypeName(std::uint32_t type);
std::uint32_t fieldTypeFromName(const std::string& typeName);
bool isEditableFieldType(std::uint32_t type);

inline std::string gffFieldTypeName(std::uint32_t type) { return fieldTypeName(type); }
inline bool gffFieldTypeEditable(std::uint32_t type) { return isEditableFieldType(type); }
std::vector<std::string> supportedFieldTypeNames();
std::string gffXmlTagForFieldType(std::uint32_t type);
std::string gffFieldTypeNameFromXmlTag(const std::string& tag);
std::string gffXmlTagFromFieldTypeName(const std::string& typeName);

} // namespace neogff
