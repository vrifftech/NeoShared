#pragma once

#include <neoshared/texture/Image.hpp>
#include <neoshared/texture/Txi.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neoshared::material {

// Authored directives, not OpenGL states. An absent value does not imply an
// engine default, an opaque render pass, or a particular blend function.
enum class BlendDirective { Additive, PunchThrough };
enum class BumpMapKind { None = 0, Height = 1, AuthoredNormal = 2 };
enum class ProcedureType {
    Water, Life, Perlin, Arturo, Wave, Cycle, Random, RingTexDistort,
    Dirty, Dirty2, Dirty3
};

struct TxiProperties {
    std::optional<std::string> bumpMapTexture;
    std::optional<std::string> environmentMapTexture;
    std::optional<std::string> bumpyShinyTexture;
    std::optional<BlendDirective> blending;
    std::optional<bool> decal;
    std::optional<std::int32_t> renderBmLmType;
    std::optional<float> waterAlpha;

    std::optional<BumpMapKind> bumpMapKind;
    // Engine Parse<int> keeps the complete initialized integer, including
    // values outside the UI enum. Processed textures accept any nonzero kind.
    std::optional<std::int32_t> engineBumpMapKind;
    std::optional<bool> diffuseBump;
    std::optional<bool> specularBump;
    std::optional<float> bumpMapScaling;
    std::optional<float> bumpIntensity;
    std::optional<float> diffuseBumpIntensity;
    std::optional<float> specularBumpIntensity;
    std::optional<std::array<float, 3>> specularColor;
    std::optional<bool> environmentMapped;
    std::optional<float> environmentAlpha;
    std::optional<bool> useGlobalAlpha;
    std::optional<float> alphaMean;

    std::optional<bool> cube;
    std::optional<bool> filter;
    std::optional<bool> mipmap;
    std::optional<std::int16_t> clamp;
    std::optional<float> gamma;
    std::optional<bool> mapTexelsToPixels;

    // Authored values only. Directive order remains in TxiMetadata::entries;
    // this snapshot does NOT execute procedural-controller construction.
    std::optional<ProcedureType> procedureType;
    std::optional<std::int16_t> numX;
    std::optional<std::int16_t> numY;
    std::optional<std::int16_t> defaultWidth;
    std::optional<std::int16_t> defaultHeight;
    std::optional<float> fps;
};

enum class MappingStatus { Mapped, Unmapped, Invalid, NonDirective };
struct MappedTxiEntry {
    neoshared::texture::TxiEntry source;
    MappingStatus status{MappingStatus::NonDirective};
};

struct EngineMaterialTextureNames {
    std::string bumpMap;
    std::string bumpyShiny;
    std::string environmentMap;
};

struct TxiMetadata {
    // Exact input to this mapper; the texture decoder may already have sanitized
    // the original footer/sidecar. Nothing here rewrites the caller's TXI.
    std::string sourceText;
    TxiProperties properties;
    std::vector<MappedTxiEntry> entries;
    std::vector<neoshared::texture::TxiValidationIssue> issues;
    // Material::Init has its own first-token parser. Keep that initialized
    // binding state separate from the editor's complete directive values.
    std::optional<EngineMaterialTextureNames> engineTextureNames;
};

TxiMetadata parseMaterialTxi(const std::string& text);


TxiMetadata parseAsciiMaterialTxi(const std::string& text, bool kotor1);

enum class PixelAlpha { Opaque, Packed, Fractional };

struct TextureFacts {
    neoshared::texture::TextureFileKind kind{neoshared::texture::TextureFileKind::Unknown};
    neoshared::texture::TextureCompression compression{neoshared::texture::TextureCompression::Auto};
    neoshared::texture::DdsDialect ddsDialect{neoshared::texture::DdsDialect::Auto};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t layerWidth{};
    std::uint32_t layerHeight{};
    PixelAlpha pixelAlpha{PixelAlpha::Opaque};
    std::size_t layerCount{};
    std::uint32_t sourceMipMapCount{};
    bool hasPixels{};
    bool hasAlpha{};
    bool cubeMap{};
    bool animated{};
    bool compressed{};
    // Source storage, before the decoder expands display pixels to RGBA.
    // Zero means the engine source byte layout has not been established.
    std::uint8_t engineBytesPerPixel{};
    bool engineProcessed{};
    // Only populated for a TPC/TXB or game-dialect DDS with a finite header
    // value. NOT pixel opacity, material alpha, or an alpha-test threshold.
    std::optional<float> headerAlphaBlending;
};

struct TextureMetadata {
    TextureFacts facts;
    TxiMetadata txi;
};

// Copies small metadata only, never pixels or mipmaps. Does not mutate texture.
TextureMetadata makeTextureMetadata(const neoshared::texture::TextureData& texture);
using TextureMetadataPtr = std::shared_ptr<const TextureMetadata>;
using TextureMetadataLookup = std::function<TextureMetadataPtr(const std::string&)>;

struct TextureBinding {
    std::string resref; // Authored spelling, including an explicit null sentinel.
    std::string key;    // Normalized lookup key; empty means no usable binding.
    TextureMetadataPtr metadata; // May be null even when key is nonempty.
};

struct Material {
    TextureBinding diffuse;
    TextureBinding lightmap;
    TextureBinding bumpMap;
    TextureBinding environmentMap;
    TextureBinding bumpyShiny;
};

// ASCII case-fold, known texture-extension removal and null-sentinel handling.
// Path-bearing names are rejected rather than rewritten to another resource.
std::string textureResourceKey(const std::string& resref);

// Mesh-provided diffuse/lightmap slots remain distinct. Auxiliary resource
// names come ONLY from diffuse texture metadata. This never loads files,
// recursively follows TXI references, evaluates time, or classifies draw passes.
Material makeMaterial(const std::string& diffuse, const std::string& lightmap,
                      const TextureMetadataLookup& lookup = {});

} // namespace neoshared::material
