#pragma once

#include "GFFFile.hpp"
#include <neotlk/TlkLookup.hpp>

namespace neogff {

inline neotlk::TextEncoding gffTextEncoding(std::uint32_t language, bool unicode = false) {
    return neotlk::detectClassicPreferredEncoding(language, unicode, {});
}

inline std::string decodeGffText(const std::string& text, std::uint32_t language = 0,
                               bool unicode = false) {
    return neotlk::decodeTextBytes(text, gffTextEncoding(language, unicode));
}

inline std::string encodeGffText(const std::string& text, std::uint32_t language = 0,
                               bool unicode = false) {
    return neotlk::encodeTextBytes(text, gffTextEncoding(language, unicode));
}

inline std::optional<std::string> resolveGffText(const GffLocalizedStringField& field,
                                               const neotlk::TlkLookup* tlk,
                                               bool kotor = true) {
    const auto language = tlk ? tlk->languageId() : 0u;
    for (const auto& text : field.substrings)
        if (text.stringid == static_cast<std::int32_t>(language * 2u))
            return neotlk::decodeTextBytes(text.GetString(), tlk ? tlk->textEncoding() : gffTextEncoding(language));
    return tlk ? (kotor ? tlk->resolveKotor(field.strref) : tlk->resolve(field.strref)) : std::nullopt;
}

} // namespace neogff
