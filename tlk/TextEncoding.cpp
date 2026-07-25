// SPDX-License-Identifier: GPL-3.0-or-later

#include "neotlk/TextEncoding.hpp"

#include <array>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace neotlk {
namespace {

constexpr std::array<std::uint32_t, 128> kWindows1252 = {{
    0x20ACu, 0x0081u, 0x201Au, 0x0192u, 0x201Eu, 0x2026u, 0x2020u, 0x2021u,
    0x02C6u, 0x2030u, 0x0160u, 0x2039u, 0x0152u, 0x008Du, 0x017Du, 0x008Fu,
    0x0090u, 0x2018u, 0x2019u, 0x201Cu, 0x201Du, 0x2022u, 0x2013u, 0x2014u,
    0x02DCu, 0x2122u, 0x0161u, 0x203Au, 0x0153u, 0x009Du, 0x017Eu, 0x0178u,
    0x00A0u, 0x00A1u, 0x00A2u, 0x00A3u, 0x00A4u, 0x00A5u, 0x00A6u, 0x00A7u,
    0x00A8u, 0x00A9u, 0x00AAu, 0x00ABu, 0x00ACu, 0x00ADu, 0x00AEu, 0x00AFu,
    0x00B0u, 0x00B1u, 0x00B2u, 0x00B3u, 0x00B4u, 0x00B5u, 0x00B6u, 0x00B7u,
    0x00B8u, 0x00B9u, 0x00BAu, 0x00BBu, 0x00BCu, 0x00BDu, 0x00BEu, 0x00BFu,
    0x00C0u, 0x00C1u, 0x00C2u, 0x00C3u, 0x00C4u, 0x00C5u, 0x00C6u, 0x00C7u,
    0x00C8u, 0x00C9u, 0x00CAu, 0x00CBu, 0x00CCu, 0x00CDu, 0x00CEu, 0x00CFu,
    0x00D0u, 0x00D1u, 0x00D2u, 0x00D3u, 0x00D4u, 0x00D5u, 0x00D6u, 0x00D7u,
    0x00D8u, 0x00D9u, 0x00DAu, 0x00DBu, 0x00DCu, 0x00DDu, 0x00DEu, 0x00DFu,
    0x00E0u, 0x00E1u, 0x00E2u, 0x00E3u, 0x00E4u, 0x00E5u, 0x00E6u, 0x00E7u,
    0x00E8u, 0x00E9u, 0x00EAu, 0x00EBu, 0x00ECu, 0x00EDu, 0x00EEu, 0x00EFu,
    0x00F0u, 0x00F1u, 0x00F2u, 0x00F3u, 0x00F4u, 0x00F5u, 0x00F6u, 0x00F7u,
    0x00F8u, 0x00F9u, 0x00FAu, 0x00FBu, 0x00FCu, 0x00FDu, 0x00FEu, 0x00FFu,
}};

constexpr std::array<std::uint32_t, 128> kWindows1250 = {{
    0x20ACu, 0x0081u, 0x201Au, 0x0083u, 0x201Eu, 0x2026u, 0x2020u, 0x2021u,
    0x0088u, 0x2030u, 0x0160u, 0x2039u, 0x015Au, 0x0164u, 0x017Du, 0x0179u,
    0x0090u, 0x2018u, 0x2019u, 0x201Cu, 0x201Du, 0x2022u, 0x2013u, 0x2014u,
    0x0098u, 0x2122u, 0x0161u, 0x203Au, 0x015Bu, 0x0165u, 0x017Eu, 0x017Au,
    0x00A0u, 0x02C7u, 0x02D8u, 0x0141u, 0x00A4u, 0x0104u, 0x00A6u, 0x00A7u,
    0x00A8u, 0x00A9u, 0x015Eu, 0x00ABu, 0x00ACu, 0x00ADu, 0x00AEu, 0x017Bu,
    0x00B0u, 0x00B1u, 0x02DBu, 0x0142u, 0x00B4u, 0x00B5u, 0x00B6u, 0x00B7u,
    0x00B8u, 0x0105u, 0x015Fu, 0x00BBu, 0x013Du, 0x02DDu, 0x013Eu, 0x017Cu,
    0x0154u, 0x00C1u, 0x00C2u, 0x0102u, 0x00C4u, 0x0139u, 0x0106u, 0x00C7u,
    0x010Cu, 0x00C9u, 0x0118u, 0x00CBu, 0x011Au, 0x00CDu, 0x00CEu, 0x010Eu,
    0x0110u, 0x0143u, 0x0147u, 0x00D3u, 0x00D4u, 0x0150u, 0x00D6u, 0x00D7u,
    0x0158u, 0x016Eu, 0x00DAu, 0x0170u, 0x00DCu, 0x00DDu, 0x0162u, 0x00DFu,
    0x0155u, 0x00E1u, 0x00E2u, 0x0103u, 0x00E4u, 0x013Au, 0x0107u, 0x00E7u,
    0x010Du, 0x00E9u, 0x0119u, 0x00EBu, 0x011Bu, 0x00EDu, 0x00EEu, 0x010Fu,
    0x0111u, 0x0144u, 0x0148u, 0x00F3u, 0x00F4u, 0x0151u, 0x00F6u, 0x00F7u,
    0x0159u, 0x016Fu, 0x00FAu, 0x0171u, 0x00FCu, 0x00FDu, 0x0163u, 0x02D9u,
}};

const std::array<std::uint32_t, 128>& codePage(TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Windows1250:
        return kWindows1250;
    case TextEncoding::Windows1252:
        return kWindows1252;
    case TextEncoding::Utf8:
        break;
    }
    throw std::invalid_argument("UTF-8 does not use a single-byte code-page table.");
}

void appendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7Fu) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FFu) {
        output.push_back(static_cast<char>(0xC0u | (codePoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else if (codePoint <= 0xFFFFu) {
        if (codePoint >= 0xD800u && codePoint <= 0xDFFFu) {
            throw std::invalid_argument("Text contains an invalid Unicode surrogate code point.");
        }
        output.push_back(static_cast<char>(0xE0u | (codePoint >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else if (codePoint <= 0x10FFFFu) {
        output.push_back(static_cast<char>(0xF0u | (codePoint >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else {
        throw std::invalid_argument("Text contains a Unicode code point beyond U+10FFFF.");
    }
}

bool decodeOneUtf8(std::string_view text, std::size_t& offset, std::uint32_t& codePoint) noexcept {
    if (offset >= text.size()) {
        return false;
    }
    const auto byte = [&](std::size_t index) {
        return static_cast<unsigned char>(text[index]);
    };
    const unsigned char b0 = byte(offset);
    if (b0 <= 0x7Fu) {
        codePoint = b0;
        ++offset;
        return true;
    }
    if (b0 >= 0xC2u && b0 <= 0xDFu) {
        if (offset + 1u >= text.size()) return false;
        const unsigned char b1 = byte(offset + 1u);
        if ((b1 & 0xC0u) != 0x80u) return false;
        codePoint = (static_cast<std::uint32_t>(b0 & 0x1Fu) << 6u) |
                    static_cast<std::uint32_t>(b1 & 0x3Fu);
        offset += 2u;
        return true;
    }
    if (b0 >= 0xE0u && b0 <= 0xEFu) {
        if (offset + 2u >= text.size()) return false;
        const unsigned char b1 = byte(offset + 1u);
        const unsigned char b2 = byte(offset + 2u);
        if ((b2 & 0xC0u) != 0x80u) return false;
        if (b0 == 0xE0u) {
            if (b1 < 0xA0u || b1 > 0xBFu) return false;
        } else if (b0 == 0xEDu) {
            if (b1 < 0x80u || b1 > 0x9Fu) return false;
        } else if ((b1 & 0xC0u) != 0x80u) {
            return false;
        }
        codePoint = (static_cast<std::uint32_t>(b0 & 0x0Fu) << 12u) |
                    (static_cast<std::uint32_t>(b1 & 0x3Fu) << 6u) |
                    static_cast<std::uint32_t>(b2 & 0x3Fu);
        offset += 3u;
        return true;
    }
    if (b0 >= 0xF0u && b0 <= 0xF4u) {
        if (offset + 3u >= text.size()) return false;
        const unsigned char b1 = byte(offset + 1u);
        const unsigned char b2 = byte(offset + 2u);
        const unsigned char b3 = byte(offset + 3u);
        if ((b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u) return false;
        if (b0 == 0xF0u) {
            if (b1 < 0x90u || b1 > 0xBFu) return false;
        } else if (b0 == 0xF4u) {
            if (b1 < 0x80u || b1 > 0x8Fu) return false;
        } else if ((b1 & 0xC0u) != 0x80u) {
            return false;
        }
        codePoint = (static_cast<std::uint32_t>(b0 & 0x07u) << 18u) |
                    (static_cast<std::uint32_t>(b1 & 0x3Fu) << 12u) |
                    (static_cast<std::uint32_t>(b2 & 0x3Fu) << 6u) |
                    static_cast<std::uint32_t>(b3 & 0x3Fu);
        offset += 4u;
        return true;
    }
    return false;
}

std::vector<std::uint32_t> decodeUtf8CodePoints(std::string_view text) {
    std::vector<std::uint32_t> result;
    result.reserve(text.size());
    std::size_t offset = 0;
    while (offset < text.size()) {
        std::uint32_t codePoint = 0;
        if (!decodeOneUtf8(text, offset, codePoint)) {
            throw std::invalid_argument("Text is not valid UTF-8.");
        }
        result.push_back(codePoint);
    }
    return result;
}

int centralEuropeanEvidence(std::string_view bytes) noexcept {
    int score = 0;
    for (const char rawByte : bytes) {
        const unsigned char byte = static_cast<unsigned char>(rawByte);
        switch (byte) {
        // Highly distinctive Polish letters in Windows-1250. Their
        // Windows-1252 interpretations are uncommon symbols in normal prose.
        case 0xA3u: case 0xB3u: // Ł ł
        case 0xA5u: case 0xB9u: // Ą ą
        case 0xC6u: case 0xE6u: // Ć ć
        case 0x8Fu: case 0x9Fu: // Ź ź
        case 0xAFu: case 0xBFu: // Ż ż
            score += 4;
            break;
        // Useful but individually ambiguous with Western-European letters.
        case 0x8Cu: case 0x9Cu: // Ś ś / Œ œ
        case 0xD1u: case 0xF1u: // Ń ń / Ñ ñ
            score += 2;
            break;
        case 0xCAu: case 0xEAu: // Ę ę / Ê ê
            score += 1;
            break;
        default:
            break;
        }
    }
    return score;
}

TextEncoding legacyEncodingFor(std::string_view rawPayload,
                               TextEncoding preferredEncoding,
                               std::uint32_t languageId) noexcept {
    if (languageId == 5u) {
        return TextEncoding::Windows1250;
    }
    const int evidence = centralEuropeanEvidence(rawPayload);
    if (evidence >= 4) {
        return TextEncoding::Windows1250;
    }
    if (preferredEncoding == TextEncoding::Windows1250) {
        return TextEncoding::Windows1250;
    }
    return TextEncoding::Windows1252;
}

} // namespace

std::string textEncodingName(TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Utf8:
        return "UTF-8";
    case TextEncoding::Windows1252:
        return "Windows-1252";
    case TextEncoding::Windows1250:
        return "Windows-1250";
    }
    return "Unknown";
}

TextEncoding parseTextEncoding(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (char raw : value) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        if (std::isalnum(ch) != 0) normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (normalized == "utf8" || normalized == "unicode") return TextEncoding::Utf8;
    if (normalized == "windows1252" || normalized == "cp1252" || normalized == "1252") {
        return TextEncoding::Windows1252;
    }
    if (normalized == "windows1250" || normalized == "cp1250" || normalized == "1250") {
        return TextEncoding::Windows1250;
    }
    throw std::invalid_argument("Unknown TLK text encoding: " + std::string(value));
}

bool isAsciiText(std::string_view bytes) noexcept {
    for (const char rawByte : bytes) {
        const unsigned char byte = static_cast<unsigned char>(rawByte);
        if (byte >= 0x80u) return false;
    }
    return true;
}

bool isValidUtf8(std::string_view bytes) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::uint32_t codePoint = 0;
        if (!decodeOneUtf8(bytes, offset, codePoint)) return false;
    }
    return true;
}

std::string decodeTextBytes(std::string_view bytes, TextEncoding encoding) {
    if (encoding == TextEncoding::Utf8) {
        if (!isValidUtf8(bytes)) {
            throw std::invalid_argument("TLK string payload is not valid UTF-8.");
        }
        return std::string(bytes);
    }

    const auto& table = codePage(encoding);
    std::string output;
    output.reserve(bytes.size() * 2u);
    for (const char rawByte : bytes) {
        const unsigned char byte = static_cast<unsigned char>(rawByte);
        if (byte < 0x80u) {
            output.push_back(static_cast<char>(byte));
        } else {
            appendUtf8(output, table[static_cast<std::size_t>(byte - 0x80u)]);
        }
    }
    return output;
}

std::string encodeTextBytes(std::string_view utf8, TextEncoding encoding) {
    if (encoding == TextEncoding::Utf8) {
        if (!isValidUtf8(utf8)) {
            throw std::invalid_argument("Edited TLK text is not valid UTF-8.");
        }
        return std::string(utf8);
    }

    const auto& table = codePage(encoding);
    const std::vector<std::uint32_t> codePoints = decodeUtf8CodePoints(utf8);
    std::string output;
    output.reserve(codePoints.size());
    for (const std::uint32_t codePoint : codePoints) {
        if (codePoint <= 0x7Fu) {
            output.push_back(static_cast<char>(codePoint));
            continue;
        }
        bool found = false;
        for (std::size_t i = 0; i < table.size(); ++i) {
            if (table[i] == codePoint) {
                output.push_back(static_cast<char>(0x80u + static_cast<unsigned int>(i)));
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::invalid_argument(
                "Edited TLK text contains a character that cannot be represented in " +
                textEncodingName(encoding) + ". Convert the file/entry to UTF-8 or use a representable character.");
        }
    }
    return output;
}

TextEncoding detectClassicPreferredEncoding(std::uint32_t languageId,
                                            bool jadeV40,
                                            const std::vector<std::string>& rawPayloads) {
    if (jadeV40 || languageId >= 10u) {
        return TextEncoding::Utf8;
    }

    std::size_t validUtf8NonAsciiBytes = 0;
    std::size_t legacyNonAsciiBytes = 0;
    int centralEvidence = 0;
    for (const std::string& payload : rawPayloads) {
        if (payload.empty() || isAsciiText(payload)) continue;
        if (isValidUtf8(payload)) {
            validUtf8NonAsciiBytes += payload.size();
        } else {
            legacyNonAsciiBytes += payload.size();
            centralEvidence += centralEuropeanEvidence(payload);
        }
    }

    if (validUtf8NonAsciiBytes != 0u &&
        (legacyNonAsciiBytes == 0u || validUtf8NonAsciiBytes >= legacyNonAsciiBytes)) {
        return TextEncoding::Utf8;
    }
    if (languageId == 5u || centralEvidence >= 8) {
        return TextEncoding::Windows1250;
    }
    return TextEncoding::Windows1252;
}

TextEncoding detectClassicEntryEncoding(std::string_view rawPayload,
                                        TextEncoding preferredEncoding,
                                        std::uint32_t languageId) {
    if (rawPayload.empty() || isAsciiText(rawPayload)) {
        return preferredEncoding;
    }
    if (isValidUtf8(rawPayload)) {
        return TextEncoding::Utf8;
    }
    return legacyEncodingFor(rawPayload, preferredEncoding, languageId);
}

} // namespace neotlk
