// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace neotlk {

enum class TextEncoding {
    Utf8,
    Windows1252,
    Windows1250,
};

std::string textEncodingName(TextEncoding encoding);
TextEncoding parseTextEncoding(std::string_view value);
bool isAsciiText(std::string_view bytes) noexcept;
bool isValidUtf8(std::string_view bytes) noexcept;
std::string decodeTextBytes(std::string_view bytes, TextEncoding encoding);
std::string encodeTextBytes(std::string_view utf8, TextEncoding encoding);
// These legacy names now select an explicit file-wide policy, not a byte heuristic.
// Other native languages require a caller-selected, supported interpretation.
TextEncoding detectClassicPreferredEncoding(std::uint32_t languageId,
                                            bool jadeV40,
                                            const std::vector<std::string>& rawPayloads);
TextEncoding detectClassicEntryEncoding(std::string_view rawPayload,
                                        TextEncoding preferredEncoding,
                                        std::uint32_t languageId);

} // namespace neotlk
