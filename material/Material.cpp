#include <neoshared/material/Material.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace neoshared::material {
namespace {

using Severity = texture::TxiIssueSeverity;

std::string lower(std::string value) {
    for (auto& ch : value) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return value;
}

void checkTextLimits(const std::string& text) {
    if (text.size() > kMaxMaterialTxiBytes)
        throw std::length_error("Material TXI exceeds the 1 MiB metadata limit");
    std::size_t lines = 0u;
    std::size_t lineBytes = 0u;
    for (const auto ch : text) {
        if (lineBytes == 0u && ++lines > kMaxMaterialTxiLines)
            throw std::length_error("Material TXI exceeds the line-count limit");
        if (ch == '\n') {
            lineBytes = 0u;
        } else if (++lineBytes > kMaxMaterialTxiLineBytes) {
            throw std::length_error("Material TXI exceeds the line-length limit");
        }
    }
}

template <typename T>
std::optional<T> parseNumber(const std::string& value) {
    // classic locale makes authored decimal points independent of the UI locale.
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    T number{};
    if (!(input >> number)) return std::nullopt;
    input >> std::ws;
    if (!input.eof()) return std::nullopt;
    return number;
}

std::optional<float> parseFloat(const std::string& value) {
    const auto number = parseNumber<float>(value);
    return number && std::isfinite(*number) ? number : std::nullopt;
}

std::optional<bool> parseBool(const std::string& value) {
    const auto folded = lower(value);
    if (folded == "true" || folded == "1") return true;
    if (folded == "false" || folded == "0") return false;
    return std::nullopt;
}

std::optional<std::array<float, 3>> parseVector(const std::string& value) {
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    std::array<float, 3> result{};
    if (!(input >> result[0] >> result[1] >> result[2])) return std::nullopt;
    input >> std::ws;
    if (!input.eof() || !std::all_of(result.begin(), result.end(),
                                    [](float v) { return std::isfinite(v); }))
        return std::nullopt;
    return result;
}

std::optional<ProcedureType> parseProcedure(const std::string& value) {
    static constexpr std::pair<const char*, ProcedureType> names[] = {
        {"water", ProcedureType::Water}, {"life", ProcedureType::Life},
        {"perlin", ProcedureType::Perlin}, {"arturo", ProcedureType::Arturo},
        {"wave", ProcedureType::Wave}, {"cycle", ProcedureType::Cycle},
        {"random", ProcedureType::Random}, {"ringtexdistort", ProcedureType::RingTexDistort},
        {"dirty", ProcedureType::Dirty}, {"dirty2", ProcedureType::Dirty2},
        {"dirty3", ProcedureType::Dirty3}
    };
    for (const auto& name : names) {
        if (value == name.first) return name.second;
    }
    return std::nullopt;
}

bool nullResource(const std::string& value) {
    const auto folded = lower(value);
    return folded == "null" || folded == "nullptr" || folded == "none";
}

void addIssue(TxiMetadata& result, const texture::TxiEntry& entry,
              Severity severity, const std::string& message) {
    result.issues.push_back({severity, entry.lineNumber, entry.key, message});
}

template <typename T>
MappingStatus assign(TxiMetadata& result, const texture::TxiEntry& entry,
                     std::optional<T>& field, const std::optional<T>& value) {
    if (!value) {
        addIssue(result, entry, Severity::Warning,
                 "Not mapped: invalid, unsupported, out-of-range or non-finite value; "
                 "any earlier valid value is retained.");
        return MappingStatus::Invalid;
    }
    field = *value;
    return MappingStatus::Mapped;
}

MappingStatus mapEntry(TxiMetadata& result, const texture::TxiEntry& entry) {
    auto& p = result.properties;
    const auto& key = entry.key;
    const auto& value = entry.value;

    // Table-driven members avoid reimplementing the shared tokenizer. No field
    // obtains a catalog default: absence and an explicit zero remain distinct.
    static constexpr std::pair<const char*, std::optional<bool> TxiProperties::*> booleans[] = {
        {"decal", &TxiProperties::decal}, {"isdiffusebumpmap", &TxiProperties::diffuseBump},
        {"isspecularbumpmap", &TxiProperties::specularBump},
        {"isenvironmentmapped", &TxiProperties::environmentMapped},
        {"useglobalalpha", &TxiProperties::useGlobalAlpha}, {"cube", &TxiProperties::cube},
        {"filter", &TxiProperties::filter}, {"mipmap", &TxiProperties::mipmap},
        {"maptexelstopixels", &TxiProperties::mapTexelsToPixels}
    };
    for (const auto& item : booleans) {
        if (key == item.first) return assign(result, entry, p.*item.second, parseBool(value));
    }
    static constexpr std::pair<const char*, std::optional<float> TxiProperties::*> floats[] = {
        {"wateralpha", &TxiProperties::waterAlpha}, {"bumpmapscaling", &TxiProperties::bumpMapScaling},
        {"bumpintensity", &TxiProperties::bumpIntensity},
        {"diffusebumpintensity", &TxiProperties::diffuseBumpIntensity},
        {"specularbumpintensity", &TxiProperties::specularBumpIntensity},
        {"envmapalpha", &TxiProperties::environmentAlpha}, {"alphamean", &TxiProperties::alphaMean},
        {"gamma", &TxiProperties::gamma}, {"fps", &TxiProperties::fps},
        {"scrollx", &TxiProperties::scrollX}, {"scrolly", &TxiProperties::scrollY}
    };
    for (const auto& item : floats) {
        if (key == item.first) return assign(result, entry, p.*item.second, parseFloat(value));
    }
    static constexpr std::pair<const char*, std::optional<std::int16_t> TxiProperties::*> shorts[] = {
        {"clamp", &TxiProperties::clamp}, {"numx", &TxiProperties::numX},
        {"numy", &TxiProperties::numY}, {"defaultwidth", &TxiProperties::defaultWidth},
        {"defaultheight", &TxiProperties::defaultHeight}
    };
    for (const auto& item : shorts) {
        if (key == item.first)
            return assign(result, entry, p.*item.second, parseNumber<std::int16_t>(value));
    }
    static constexpr std::pair<const char*, std::optional<std::string> TxiProperties::*> names[] = {
        {"bumpmaptexture", &TxiProperties::bumpMapTexture},
        {"envmaptexture", &TxiProperties::environmentMapTexture},
        {"bumpyshinytexture", &TxiProperties::bumpyShinyTexture}
    };
    for (const auto& item : names) {
        if (key == item.first) {
            std::optional<std::string> name;
            if (nullResource(value) || !textureResourceKey(value).empty()) name = value;
            return assign(result, entry, p.*item.second, name);
        }
    }
    if (key == "blending") {
        std::optional<BlendDirective> mode;
        // Match the existing TXI catalog's exact enum tokens. Do not turn an
        // unknown/mixed-case token into a guessed engine render state.
        if (value == "additive") mode = BlendDirective::Additive;
        else if (value == "punchthrough") mode = BlendDirective::PunchThrough;
        return assign(result, entry, p.blending, mode);
    }
    if (key == "proceduretype")
        return assign(result, entry, p.procedureType, parseProcedure(value));
    if (key == "isbumpmap") {
        std::optional<BumpMapKind> kind;
        const auto number = parseNumber<std::int32_t>(value);
        if (number && *number >= 0 && *number <= 2) kind = static_cast<BumpMapKind>(*number);
        return assign(result, entry, p.bumpMapKind, kind);
    }
    if (key == "renderbmlmtype")
        return assign(result, entry, p.renderBmLmType, parseNumber<std::int32_t>(value));
    if (key == "specularcolor") return assign(result, entry, p.specularColor, parseVector(value));
    // Includes catalog-known controller/font/compression fields not represented
    // yet. Preserve them and list rows in order; do not reinterpret their data.
    return MappingStatus::Unmapped;
}

TextureBinding bind(const std::string& resref, const TextureMetadataLookup& lookup) {
    TextureBinding binding;
    binding.resref = resref;
    binding.key = textureResourceKey(resref);
    if (lookup && !binding.key.empty()) binding.metadata = lookup(binding.key);
    return binding;
}

} // namespace

TxiMetadata parseMaterialTxi(const std::string& text) {
    checkTextLimits(text);
    TxiMetadata result;
    result.sourceText = text;
    result.issues = texture::validateTxiText(text);
    for(auto& issue:result.issues) if(issue.key=="scrollx" || issue.key=="scrolly") {
        issue.severity=Severity::Info;
        issue.message="NeoMDL preview extension; not established as an Odyssey TXI directive.";
    }
    for (auto entry : texture::parseTxiEntries(text)) {
        const auto status = entry.blankOrComment || entry.listData || entry.listTerminator
            ? MappingStatus::NonDirective : mapEntry(result, entry);
        result.entries.push_back({std::move(entry), status});
    }
    return result;
}

TextureMetadata makeTextureMetadata(const texture::TextureData& texture) {
    TextureMetadata result;
    result.txi = parseMaterialTxi(texture.txi);
    auto& facts = result.facts;
    facts.kind = texture.kind;
    facts.compression = texture.preferredCompression;
    facts.ddsDialect = texture.ddsDialect;
    facts.width = texture.canvasWidth;
    facts.height = texture.canvasHeight;
    facts.layerCount = texture.layers.size();
    if (!texture.layers.empty()) {
        facts.layerWidth = texture.layers.front().width;
        facts.layerHeight = texture.layers.front().height;
    }
    // Scan base levels once, across all frames: classification must not flicker
    // as animation advances. Alpha storage flags are not evidence of opacity.
    for (const auto& layer : texture.layers) {
        for (std::size_t i = 3; i < layer.rgba.size(); i += 4) {
            const auto alpha = layer.rgba[i];
            if (alpha != 0 && alpha != 255) {
                facts.pixelAlpha = PixelAlpha::Fractional;
                break;
            }
            if (alpha == 0) facts.pixelAlpha = PixelAlpha::Binary;
        }
        if (facts.pixelAlpha == PixelAlpha::Fractional) break;
    }
    facts.sourceMipMapCount = texture.sourceMipMapCount;
    facts.hasPixels = texture.hasPixels();
    facts.hasAlpha = texture.hasAlpha;
    facts.cubeMap = texture.cubeMap;
    facts.animated = texture.animated;
    facts.compressed = texture.compressed;
    const bool hasHeaderAlpha = texture.kind == texture::TextureFileKind::Tpc ||
        texture.kind == texture::TextureFileKind::Txb ||
        (texture.kind == texture::TextureFileKind::Dds &&
         texture.ddsDialect == texture::DdsDialect::Game);
    if (hasHeaderAlpha) {
        if (std::isfinite(texture.alphaBlending)) {
            facts.headerAlphaBlending = texture.alphaBlending;
        } else {
            result.txi.issues.push_back({Severity::Warning, 0u, "headerAlphaBlending",
                "Non-finite texture-header alpha metadata was not mapped."});
        }
    }
    return result;
}

std::string textureResourceKey(const std::string& resref) {
    if (resref.empty() || nullResource(resref)) return {};
    for (const auto ch : resref) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 32u || byte >= 127u || ch == '/' || ch == '\\' || ch == ':' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|' || ch == '*' || ch == '?')
            return {};
    }
    auto key = lower(resref);
    const auto dot = key.find_last_of('.');
    if (dot != std::string::npos) {
        const auto ext = key.substr(dot);
        if (ext == ".tpc" || ext == ".txb" || ext == ".tga" || ext == ".dds" || ext == ".txi")
            key.erase(dot);
    }
    if (key.empty() || key == "." || key == ".." || nullResource(key)) return {};
    return key;
}

Material makeMaterial(const std::string& diffuse, const std::string& lightmap,
                      const TextureMetadataLookup& lookup) {
    Material result;
    result.diffuse = bind(diffuse, lookup);
    result.lightmap = bind(lightmap, lookup);
    if (result.diffuse.metadata) {
        const auto& p = result.diffuse.metadata->txi.properties;
        if (p.bumpMapTexture) result.bumpMap = bind(*p.bumpMapTexture, lookup);
        if (p.environmentMapTexture) result.environmentMap = bind(*p.environmentMapTexture, lookup);
        if (p.bumpyShinyTexture) result.bumpyShiny = bind(*p.bumpyShinyTexture, lookup);
    }
    return result;
}

} // namespace neoshared::material
