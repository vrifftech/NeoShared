#pragma once

#include "GFFFile.hpp"

#include <string>

namespace neogff {

std::string ToGffXml(const GffFile& gff);
void LoadGffXml(GffFile& gff, const std::string& xmlText);

} // namespace neogff
