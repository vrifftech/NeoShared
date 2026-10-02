#pragma once

#include <neoshared/model/Model.hpp>

namespace neoshared::model {

// Contextual light controller IDs. Never dispatch these on mesh/emitter nodes.
inline constexpr std::uint32_t kControllerLightColor = 76u;
inline constexpr std::uint32_t kControllerLightRadius = 88u;
inline constexpr std::uint32_t kControllerLightShadowRadius = 96u;
inline constexpr std::uint32_t kControllerLightVerticalDisplacement = 100u;
inline constexpr std::uint32_t kControllerLightMultiplier = 140u;

NodeLightState bindNodeLight(const Node& node) noexcept;

struct SceneLight {
    std::uint64_t id{};
    Vec3 position{};
    Vec3 color{}; // evaluated RGB * multiplier, finite/nonnegative display range
    float radius{};
    float shadowRadius{};
    float verticalDisplacement{}; // shadow-source world-Z offset; illumination stays at node position
    std::int32_t priority{};
    std::int32_t dynamicType{}; // retained; engine's scene categories not inferred
    bool ambientOnly{};
    bool affectDynamic{};
    bool castsShadow{};
    bool fading{};
};

// Appends finite, enabled lights in model node order. base transforms referenced
// instances into the same scene. IDs must be unique across instances.
void appendSceneLights(const Model& model, const Pose& pose, const Mat4& base,
                       std::uint64_t instanceId, std::vector<SceneLight>& output);
// Viewer selection policy: bounded influence, affectDynamic filtering, highest
// priority first, then nearest surface. Stable ties preserve traversal order.
std::vector<std::size_t> selectSceneLights(const std::vector<SceneLight>& lights,
    Vec3 center, float surfaceRadius, bool dynamicSurface, std::size_t maximum = 8);
// Smooth finite-radius attenuation (viewer equation, not reverse-engineered).
float lightAttenuation(float distance, float radius, bool fading) noexcept;

enum class FogMode : std::uint8_t { Disabled, Linear, Exponential, ExponentialSquared };
struct FogSettings {
    FogMode mode{FogMode::Disabled};
    Vec3 color{0.15f,0.16f,0.18f};
    float start{10.0f};
    float end{100.0f};
    float density{0.025f};
    bool respectModelFlag{true};
};
bool validFog(const FogSettings& fog) noexcept;
FogSettings fogForModel(const FogSettings& scene, bool modelFog) noexcept;
// Transmittance, not opacity. Distance is nonnegative eye-axis depth.
float fogTransmittance(const FogSettings& fog, float distance) noexcept;

} // namespace neoshared::model
