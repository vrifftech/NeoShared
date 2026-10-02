#pragma once

#include <array>
#include <cstdint>

namespace neoshared::model {

// These identifiers are contextual: lights and emitters reuse the same raw
// numbers for unrelated fields. Only mesh-target controllers use these meanings.
inline constexpr std::uint32_t kControllerSelfIllumColor = 100u;
inline constexpr std::uint32_t kControllerAlpha = 132u;

enum class MaterialValueSource : std::uint8_t {
    Default,
    ModelController,
    AnimationController,
};

// Per-node, per-pose state, never stored in a shared texture or material cache.
// Valid finite authored values are retained, including Bezier overshoot. The
// material runtime bounds them for display. Raw controller words stay intact.
struct NodeMaterialState {
    float alpha{1.0f};
    std::array<float, 3> selfIllumination{};
    MaterialValueSource alphaSource{MaterialValueSource::Default};
    MaterialValueSource selfIlluminationSource{MaterialValueSource::Default};
};

struct Node;
// Evaluate base-model material tracks at time zero. Missing, malformed and
// non-mesh tracks leave the defaults (alpha 1, self-illumination black).
NodeMaterialState bindNodeMaterial(const Node& node) noexcept;
const char* materialValueSourceName(MaterialValueSource source) noexcept;

} // namespace neoshared::model
