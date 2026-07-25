#pragma once

#include <string>

namespace neogff {


std::string gffXmlToJson(const std::string& gffXml);
std::string gffJsonToXml(const std::string& jsonText);

} // namespace neogff
