#pragma once
#include <neoshared/model/MaterialAnimation.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

namespace neoshared::model {
// Contextual emitter state. Defaults follow the K2 PartEmitter constructor.
// Raw authored values are retained here; simulation limits are applied later.
enum class EmitterProperty : std::size_t {
    BirthRate, LifeExpectancy, Velocity, RandomVelocity, Spread, Gravity, Drag,
    Rotation, XSize, YSize, FrameStart, FrameEnd, FramesPerSecond,
    AlphaStart, AlphaMid, AlphaEnd, SizeStart, SizeMid, SizeEnd,
    SizeStartY, SizeMidY, SizeEndY, PercentStart, PercentMid, PercentEnd,
    ColorStart, ColorMid, ColorEnd, Mass, BounceCoefficient, BlurLength,
    RandomBirthRate, CombineTime, Bezier2, Bezier3, TargetThreshold,
    LightningDelay, LightningRadius, LightningScale, LightningSubdivisions, LightningZigzag,
    TargetSize, ControlPointCount, ControlPointRadius, ControlPointDelay, TangentSpread,
    TangentLength, Detonate, Count
};
struct NodeEmitterState {
    float birthRate{}, lifeExpectancy{}, velocity{}, randomVelocity{}, spread{};
    // grav and drag are point-to-point properties, not standard-particle gravity/drag.
    float gravity{}, drag{}, rotation{}, xSize{}, ySize{};
    float mass{}, bounceCoefficient{}, blurLength{1.0f}, randomBirthRate{};
    float combineTime{}, bezier2{}, bezier3{}, targetThreshold{};
    float lightningDelay{}, lightningRadius{}, lightningScale{}, lightningSubdivisions{}, lightningZigzag{};
    float targetSize{}, controlPointCount{}, controlPointRadius{}, controlPointDelay{}, tangentSpread{}, tangentLength{};
    float detonate{};
    float frameStart{}, frameEnd{}, framesPerSecond{};
    float alphaStart{}, alphaMid{}, alphaEnd{};
    float sizeStart{}, sizeMid{}, sizeEnd{};
    float sizeStartY{}, sizeMidY{}, sizeEndY{};
    float percentStart{255.0f}, percentMid{255.0f}, percentEnd{255.0f};
    std::array<float,3> colorStart{}, colorMid{}, colorEnd{};
    std::array<MaterialValueSource,static_cast<std::size_t>(EmitterProperty::Count)> sources{};
    bool authored(EmitterProperty property) const noexcept {
        return sources[static_cast<std::size_t>(property)] != MaterialValueSource::Default;
    }
};
struct EmitterScalarBinding {
    std::uint32_t id;
    float NodeEmitterState::*field;
    EmitterProperty property;
};
// IDs are binary controller offsets, NOT shared with light/mesh meanings.
inline constexpr EmitterScalarBinding kEmitterScalarBindings[] = {
    // ExplosionEmitter consumes this controller as a one-byte rising-edge
    // latch at the otherwise unaligned destination offset 0x1f6. Keep the
    // sampled value as a semantic Boolean rather than reproducing the binary's
    // float/byte storage aliasing in the portable model state.
    {502u, &NodeEmitterState::detonate, EmitterProperty::Detonate},
    {184u, &NodeEmitterState::lightningDelay, EmitterProperty::LightningDelay},
    {188u, &NodeEmitterState::lightningRadius, EmitterProperty::LightningRadius},
    {192u, &NodeEmitterState::lightningScale, EmitterProperty::LightningScale},
    {196u, &NodeEmitterState::lightningSubdivisions, EmitterProperty::LightningSubdivisions},
    {200u, &NodeEmitterState::lightningZigzag, EmitterProperty::LightningZigzag},
    {252u, &NodeEmitterState::targetSize, EmitterProperty::TargetSize},
    {256u, &NodeEmitterState::controlPointCount, EmitterProperty::ControlPointCount},
    {260u, &NodeEmitterState::controlPointRadius, EmitterProperty::ControlPointRadius},
    {264u, &NodeEmitterState::controlPointDelay, EmitterProperty::ControlPointDelay},
    {268u, &NodeEmitterState::tangentSpread, EmitterProperty::TangentSpread},
    {272u, &NodeEmitterState::tangentLength, EmitterProperty::TangentLength},

    {96u, &NodeEmitterState::combineTime, EmitterProperty::CombineTime},
    {128u, &NodeEmitterState::bezier2, EmitterProperty::Bezier2},
    {132u, &NodeEmitterState::bezier3, EmitterProperty::Bezier3},
    {164u, &NodeEmitterState::targetThreshold, EmitterProperty::TargetThreshold},
    {88u, &NodeEmitterState::birthRate, EmitterProperty::BirthRate},
    {124u, &NodeEmitterState::mass, EmitterProperty::Mass},
    {92u, &NodeEmitterState::bounceCoefficient, EmitterProperty::BounceCoefficient},
    {180u, &NodeEmitterState::blurLength, EmitterProperty::BlurLength},
    {240u, &NodeEmitterState::randomBirthRate, EmitterProperty::RandomBirthRate},
    {120u, &NodeEmitterState::lifeExpectancy, EmitterProperty::LifeExpectancy},
    {168u, &NodeEmitterState::velocity, EmitterProperty::Velocity},
    {140u, &NodeEmitterState::randomVelocity, EmitterProperty::RandomVelocity},
    {160u, &NodeEmitterState::spread, EmitterProperty::Spread},
    {116u, &NodeEmitterState::gravity, EmitterProperty::Gravity},
    {100u, &NodeEmitterState::drag, EmitterProperty::Drag},
    {136u, &NodeEmitterState::rotation, EmitterProperty::Rotation},
    {172u, &NodeEmitterState::xSize, EmitterProperty::XSize},
    {176u, &NodeEmitterState::ySize, EmitterProperty::YSize},
    {112u, &NodeEmitterState::frameStart, EmitterProperty::FrameStart},
    {108u, &NodeEmitterState::frameEnd, EmitterProperty::FrameEnd},
    {104u, &NodeEmitterState::framesPerSecond, EmitterProperty::FramesPerSecond},
    {84u, &NodeEmitterState::alphaStart, EmitterProperty::AlphaStart},
    {216u, &NodeEmitterState::alphaMid, EmitterProperty::AlphaMid},
    {80u, &NodeEmitterState::alphaEnd, EmitterProperty::AlphaEnd},
    {144u, &NodeEmitterState::sizeStart, EmitterProperty::SizeStart},
    {232u, &NodeEmitterState::sizeMid, EmitterProperty::SizeMid},
    {148u, &NodeEmitterState::sizeEnd, EmitterProperty::SizeEnd},
    {152u, &NodeEmitterState::sizeStartY, EmitterProperty::SizeStartY},
    {236u, &NodeEmitterState::sizeMidY, EmitterProperty::SizeMidY},
    {156u, &NodeEmitterState::sizeEndY, EmitterProperty::SizeEndY},
    {220u, &NodeEmitterState::percentStart, EmitterProperty::PercentStart},
    {224u, &NodeEmitterState::percentMid, EmitterProperty::PercentMid},
    {228u, &NodeEmitterState::percentEnd, EmitterProperty::PercentEnd},
};
inline constexpr std::uint32_t kControllerEmitterColorStart=392u;
inline constexpr std::uint32_t kControllerEmitterColorMid=284u;
inline constexpr std::uint32_t kControllerEmitterColorEnd=380u;
} // namespace neoshared::model
