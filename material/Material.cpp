#include <neoshared/material/Material.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace neoshared::material {
namespace {

using Severity = neoshared::texture::TxiIssueSeverity;

std::string lower(std::string value) {
    for (auto& ch : value) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return value;
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

void addIssue(TxiMetadata& result, const neoshared::texture::TxiEntry& entry,
              Severity severity, const std::string& message) {
    result.issues.push_back({severity, entry.lineNumber, entry.key, message});
}

template <typename T>
MappingStatus assign(TxiMetadata& result, const neoshared::texture::TxiEntry& entry,
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

std::pair<std::string, std::string> engineTxiField(std::string_view line) {
    // Init passes a C string, including when the resource has NUL padding.
    // Keep the boolean fallback bounded identically to sscanf below.
    line = line.substr(0u, line.find('\0'));
    // Init skips only SPACE/TAB, and engine firstword excludes VT/FF from
    // its delimiters. The shared editor tokenizer accepts broader whitespace;
    // use the original span so that spelling cannot enable compiler tangents.
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.remove_prefix(1u);
    const auto end = line.find_first_of(std::string_view(" \t\r\n\0", 5u));
    return {lower(std::string(line.substr(0u, end))),
        std::string(line.substr(end == std::string_view::npos ? line.size() : end))};
}

void parseEngineMaterialField(TxiMetadata& metadata, std::string_view line) {
    const auto [keyword, arguments] = engineTxiField(line);
    auto& names = *metadata.engineTextureNames;
    auto* name = keyword == "bumpmaptexture" ? &names.bumpMap :
        keyword == "bumpyshinytexture" ? &names.bumpyShiny :
        keyword == "envmaptexture" ? &names.environmentMap : nullptr;
    if (name) {
        // Material::ParseField starts a fresh empty temporary, scans one %s
        // token, and replaces the retained name only on a nonempty result.
        std::istringstream input(arguments);
        input.imbue(std::locale::classic());
        std::string token;
        if (input >> token) *name = std::move(token);
    }
}

void parseEngineBumpField(TxiMetadata& metadata, std::string_view line) {
    const auto [keyword, arguments] = engineTxiField(line);
    if (keyword != "isbumpmap") return;
    auto& properties = metadata.properties;
    // CAurTextureBasic::ParseField delegates to Parse<int> in all four
    // references: numeric prefixes, then exact TRUE/FALSE tokens on failure.
    int engineKind;
    if (std::sscanf(arguments.c_str(), "%d", &engineKind) == 1)
        properties.engineBumpMapKind = static_cast<std::int32_t>(engineKind);
    else {
        std::istringstream input(arguments);
        std::string token;
        input >> token;
        token = lower(token);
        if (token == "true") properties.engineBumpMapKind = 1;
        else if (token == "false") properties.engineBumpMapKind = 0;
    }
}

MappingStatus mapEntry(TxiMetadata& result, const neoshared::texture::TxiEntry& entry) {
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
        {"gamma", &TxiProperties::gamma}, {"fps", &TxiProperties::fps}
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
        // The display enum remains strict; engine parsing uses the original
        // source span separately so editor token normalization cannot alter it.
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

static TxiMetadata parseMaterialTxiImpl(const std::string& text, bool crOrLf) {
    TxiMetadata result;
    result.sourceText = text;
    // A parsed engine texture starts at zero, even when an editor-normalized
    // directive maps to a UI enum but its original keyword is not recognized.
    result.properties.engineBumpMapKind = 0;
    result.engineTextureNames.emplace();
    // Material::Init owns a physical-line reader independent of the basic
    // texture/controller parser. Counted text rows and packed channel payloads
    // do not hide a subsequent physical material directive from this reader.
    for (std::size_t begin = 0u; begin < text.size();) {
        const auto end = text.find_first_of(!crOrLf ? "\n" : "\r\n", begin);
        parseEngineMaterialField(result, std::string_view(text).substr(begin,
            end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1u;
    }
    result.issues = neoshared::texture::validateTxiText(text);
    const auto spans = neoshared::texture::scanTxiStream(text);
    std::size_t spanIndex{};
    for (auto entry : neoshared::texture::parseTxiEntries(text)) {
        if (!entry.blankOrComment && !entry.listData && !entry.listTerminator &&
            entry.key == "isbumpmap") {
            while (spanIndex < spans.size() && spans[spanIndex].lineNumber < entry.lineNumber)
                ++spanIndex;
            if (spanIndex < spans.size() && spans[spanIndex].lineNumber == entry.lineNumber) {
                const auto& span = spans[spanIndex];
                // Editor records split at CR too. A record after a bare CR is
                // still inside the preceding engine LF line and cannot begin
                // another field. Conversely CR inside the first field remains
                // available to Parse<int> as argument whitespace.
                // A shared packed-payload span can also end between engine
                // delimiters. That position alone does not start a field.
                if (span.begin == 0u || text[span.begin - 1u] == '\n' ||
                    (crOrLf && text[span.begin - 1u] == '\r')) {
                    const auto end = !crOrLf
                        ? text.find('\n', span.begin) : span.end;
                    parseEngineBumpField(result,
                        std::string_view(text).substr(span.begin,
                            end == std::string::npos ? end : end - span.begin));
                }
            }
        }
        const auto status = entry.blankOrComment || entry.listData || entry.listTerminator
            ? MappingStatus::NonDirective : mapEntry(result, entry);
        result.entries.push_back({std::move(entry), status});
    }
    return result;
}

TxiMetadata parseMaterialTxi(const std::string& text) {
    return parseMaterialTxiImpl(text, false);
}

TxiMetadata parseAsciiMaterialTxi(const std::string& text, bool kotor1) {
    return parseMaterialTxiImpl(text, kotor1);
}

TextureMetadata makeTextureMetadata(const neoshared::texture::TextureData& texture) {
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
            if (alpha == 0) facts.pixelAlpha = PixelAlpha::Packed;
        }
        if (facts.pixelAlpha == PixelAlpha::Fractional) break;
    }
    facts.sourceMipMapCount = texture.sourceMipMapCount;
    facts.hasPixels = texture.hasPixels();
    facts.hasAlpha = texture.hasAlpha;
    facts.cubeMap = texture.cubeMap;
    facts.animated = texture.animated;
    facts.compressed = texture.compressed;
    if (texture.processedPayload) {
        facts.engineProcessed = true;
        facts.engineBytesPerPixel = texture.processedPayload->bytesPerPixel;
    } else if (texture.tgaSourceLayout) {
        const auto& layout = *texture.tgaSourceLayout;
        // Palette indices and unsupported narrow color layouts are not engine
        // grayscale samples. These are the established CResTGA source forms.
        if (((layout.imageType == 2 || layout.imageType == 10) &&
                (layout.pixelBits == 24 || layout.pixelBits == 32)) ||
            (layout.imageType == 3 && (layout.pixelBits == 8 || layout.pixelBits == 16)))
            facts.engineBytesPerPixel = layout.pixelBits / 8;
    } else {
        const auto& encoding = texture.sourceEncoding;
        if (encoding == "raw grayscale" || encoding == "8-bit TGA")
            facts.engineBytesPerPixel = 1;
        else if (encoding == "raw RGB" || encoding.rfind("24-bit TGA", 0) == 0 ||
                 encoding == "BioWare DDS DXT1")
            facts.engineBytesPerPixel = 3;
        else if (encoding == "raw RGBA" || encoding.rfind("32-bit TGA", 0) == 0 ||
                 encoding == "K1 procedural RGBA" || encoding == "K2 procedural RGBA" ||
                 encoding == "BioWare DDS DXT5")
            facts.engineBytesPerPixel = 4;
    }
    const bool hasHeaderAlpha = texture.kind == neoshared::texture::TextureFileKind::Tpc ||
        texture.kind == neoshared::texture::TextureFileKind::Txb ||
        (texture.kind == neoshared::texture::TextureFileKind::Dds &&
         texture.ddsDialect == neoshared::texture::DdsDialect::Game);
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
        const auto& txi = result.diffuse.metadata->txi;
        if (txi.engineTextureNames) {
            result.bumpMap = bind(txi.engineTextureNames->bumpMap, lookup);
            result.bumpyShiny = bind(txi.engineTextureNames->bumpyShiny, lookup);
            result.environmentMap = bind(txi.engineTextureNames->environmentMap, lookup);
        } else {
            const auto& p = txi.properties;
            if (p.bumpMapTexture) result.bumpMap = bind(*p.bumpMapTexture, lookup);
            if (p.environmentMapTexture) result.environmentMap = bind(*p.environmentMapTexture, lookup);
            if (p.bumpyShinyTexture) result.bumpyShiny = bind(*p.bumpyShinyTexture, lookup);
        }
    }
    return result;
}

} // namespace neoshared::material
