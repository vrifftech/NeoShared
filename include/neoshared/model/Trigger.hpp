#pragma once

#include <neoshared/model/Model.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neoshared::model {

// Runtime PartTrigger data is not stored on MdlNodeTrigger. The K2 Android
// binary creates it from game-object footprint points through
// AurPartTriggerCreate. These structures expose that binary-derived runtime to
// model-aware tools without inventing a private MDL trigger payload.
struct TriggerHighlightParameters {
    Vec3 color{1.0f, 1.0f, 1.0f};
    float alpha{0.5f};
    bool pulse{};
    float pulseRate{1.0f};
    bool additive{};
};

struct TriggerVolume {
    std::vector<Vec3> footprint;
    // PartTrigger::Decompose stores an unindexed GL_TRIANGLES stream.
    std::vector<Vec3> triangles;
    // SetHighlighted builds these only while highlighted. sideStrip includes
    // the first top/bottom pair again to close GL_TRIANGLE_STRIP.
    std::vector<Vec3> sideStrip;
    std::vector<Vec3> top;
    std::vector<Vec3> bottom;
    Bounds bounds{};
    std::array<float, 4> color{{1.0f, 1.0f, 1.0f, 1.0f}};
    std::uintptr_t owner{};
    bool highlighted{};
    float highlightHalfHeight{};
    bool highlightGroup{};
    TriggerHighlightParameters highlight{};
    bool valid{};
    std::string diagnostic;
};

struct TriggerHitOptions {
    // PartTrigger::HitCheckGeom returns immediately unless CHitInfo flag 0x40
    // is set. Keeping the gate explicit lets embedding tools reproduce that
    // contract while the default remains useful for direct inspection.
    bool geometryEnabled{true};
};

struct TriggerHit {
    Vec3 position{};
    Vec3 normal{};
    std::size_t triangleIndex{};
    std::uintptr_t owner{};
    float hitClass{2.0f};
    float segmentFraction{};
};

struct TriggerPlane {
    Vec3 normal{};
    float distance{};
};

// Constructs the K2 PartTrigger equivalent from the footprint, base RGBA and
// owner token accepted by AurPartTriggerCreate. The supplied points remain in
// their authored order.
TriggerVolume makeK2TriggerVolume(
    std::vector<Vec3> footprint,
    std::array<float, 4> color = {{1.0f, 1.0f, 1.0f, 1.0f}},
    std::uintptr_t owner = 0u);

// PartTrigger::SetHighlighted. height is converted to abs(height). Disabling
// highlighting leaves the previously generated volume arrays and expanded
// bounds intact, matching the runtime object.
void setK2TriggerHighlighted(TriggerVolume& volume, bool highlighted,
                             float height, bool group = false);
void setK2TriggerHighlightParameters(
    TriggerVolume& volume, const TriggerHighlightParameters& parameters);
void setK2TriggerHighlightColor(TriggerVolume& volume, Vec3 color,
                                float alpha);

// PartTrigger::Draw adds the process-global alpha to the authored base alpha.
float k2TriggerEffectiveBaseAlpha(const TriggerVolume& volume,
                                  float globalAlpha) noexcept;
// Scene::DoHighlightTriggers uses the Scene phase directly. Pulse alpha is
// alpha*0.5*(sin(pulseRate*scenePhase)+0.75), without clamping.
float k2TriggerHighlightAlpha(const TriggerVolume& volume,
                              float scenePhase) noexcept;

// PartTrigger::HitCheckGeom in trigger/world coordinates. It performs the box
// broad phase, tests triangles in decomposition order and returns the first
// one-sided front-to-back hit rather than searching for the nearest triangle.
std::optional<TriggerHit> hitTestK2Trigger(
    const TriggerVolume& volume, const Vec3& start, const Vec3& end,
    const TriggerHitOptions& options = {});

// PartOutside(PartTrigger*, List<Plane>&). Planes are visited beginning at the
// caller-provided cached index. BoxAbovePlane selects minimum coordinates for
// positive normal components and maximum coordinates otherwise; a positive
// result means the trigger is outside and updates the cache.
bool k2TriggerOutsideFrustum(const TriggerVolume& volume,
                             const std::vector<TriggerPlane>& planes,
                             std::size_t& cachedPlane) noexcept;

} // namespace neoshared::model
