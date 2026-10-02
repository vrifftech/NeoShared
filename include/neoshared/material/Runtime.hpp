#pragma once
#include <neoshared/material/Material.hpp>

namespace neoshared::material {
// Explicit viewer policy, separate from authored metadata. No graphics API.
enum class RenderPass { Opaque, PunchThrough, Translucent, Additive };
struct SurfaceInputs {
    float opacity{1.0f}; // evaluated node alpha
    // Explicit caller/instance multiplier. This is not an interpretation of
    // the unresolved game TXI useglobalalpha flag.
    float instanceOpacity{1.0f};
    std::array<float, 3> selfIllumination{};
    bool vertexAlpha{};
    bool uvAnimation{};
    std::array<float, 2> uvVelocity{};
};
struct UvState {
    // For unsplit atlases: repeat locally before scale/offset, with half-texel
    // inset applied by the shader. Split NeoTPC layers must NOT be tiled twice.
    std::array<float, 2> scroll{};
    std::array<float, 2> scale{{1,1}};
    std::array<float, 2> offset{};
    std::array<float, 2> texel{};
    std::size_t frame{};
    std::size_t layer{};
    bool atlas{};
    bool clamp{};
};
struct RuntimeMaterial {
    RenderPass pass{RenderPass::Opaque};
    float opacity{1.0f};
    std::array<float, 3> selfIllumination{};
    float alphaCutoff{0.35f}; // K2 Material::SetBlendingMode reference; strict >
    bool alphaTest{};
    bool textureAlphaIsOpacity{true};
    BumpMapKind bumpKind{BumpMapKind::None};
    float bumpScale{1.0f};
    bool diffuseBump{true};
    bool specularBump{};
    float diffuseBumpIntensity{1.0f};
    float specularBumpIntensity{1.0f};
    std::array<float,3> specularColor{{1,1,1}};
    float environmentStrength{1.0f};
    UvState diffuseUv, bumpUv;
};
UvState evaluateUv(const TextureBinding& binding, double seconds,
                   std::array<float,2> meshVelocity = {});
RuntimeMaterial evaluateMaterial(const Material& material, const SurfaceInputs& surface,
                                 double seconds);
struct QueueKey {
    RenderPass pass{RenderPass::Opaque};
    float eyeDepth{};
    // Particle emitters owned by one game object use their signed authored
    // render order before the object's distance. Zero means no authored group.
    std::uint64_t authoredOrderGroup{};
    std::int32_t authoredOrder{};
};
// Scene-wide stable order: solid passes, then far-to-near alpha, then additive.
// Same-owner particle emitters use the binary-observed signed authored order.
// Cross-owner and mesh/particle interleaving remains the viewer scene policy.
std::vector<std::size_t> renderQueueOrder(const std::vector<QueueKey>& keys);
} // namespace neoshared::material
