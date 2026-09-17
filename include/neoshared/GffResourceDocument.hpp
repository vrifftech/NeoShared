#pragma once

#include "neoshared/PathUtf8.hpp"
#include "ResourceDocument.hpp"
#include <gff/AppModel.hpp>
#include <algorithm>
#include <cctype>

namespace neoshared {
inline bool sameGffResourceType(std::string actual, std::string expected) {
    const auto trim=[](std::string& value) {
        while(!value.empty() && (value.back()==' ' || value.back()=='\0')) value.pop_back();
    };
    trim(actual);trim(expected);return actual==expected;
}
// Only a hosting boundary: the existing GFF parser still owns all decoding.
inline void loadGffResource(const ResourceDocument& source, neogff::GffFile& file,
                            const std::string& requiredType = {}) {
    if (source.identity.empty() || source.fileName.empty())
        throw std::runtime_error("Missing resource identity or filename.");
    if (source.bytes.size() < 8)
        throw std::runtime_error("The selected resource is not a complete GFF file.");
    // Use the real parser's type: GFF4 containers need not begin with the resource tag.
    // Callers supply a fresh candidate model and commit it only after this succeeds.
    file.LoadBytes(source.bytes);
    if (!requiredType.empty() && !sameGffResourceType(file.filetype(), requiredType))
        throw std::runtime_error("The selected resource has the wrong GFF file type for this editor.");
}
inline void checkGffOutputType(const std::filesystem::path& path,
                               const std::string& requiredExtension = {}) {
    std::string extension = neoshared::pathToUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if ((!requiredExtension.empty() && extension != requiredExtension) ||
        (requiredExtension.empty() && !neogff::isKnownGffResourceExtension(extension)))
        throw std::runtime_error("Choose a separate GFF resource file with the appropriate extension, not an archive.");
}
} // namespace neoshared
