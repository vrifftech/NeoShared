#pragma once
#include <neoshared/model/Model.hpp>
#include <neoshared/model/Wind.hpp>
#include <functional>

namespace neoshared::model {
inline constexpr unsigned kParticleRuntimeVersion=9;
NodeEmitterState bindNodeEmitter(const Node& node) noexcept;

enum class EmitterBlend { Normal, PunchThrough, Additive };
enum class EmitterUpdate { Fountain, Single, Explosion, Lightning };
enum class EmitterRender {
    Normal, BillboardWorldZ, BillboardLocalZ, AlignedWorldZ, AlignedParticleDirection,
    MotionBlur, Linked
};
namespace emitter_flags {
inline constexpr std::uint32_t PointToPoint=0x1, PointToPointBezier=0x2,
    AffectedByWind=0x4, Tinted=0x8, Bounce=0x10, RandomFrame=0x20,
    Inherit=0x40, InheritVelocity=0x80, InheritLocal=0x100, Splat=0x200,
    InheritPart=0x400, DepthTexture=0x800, RandomOrder=0x1000;
}
struct EmitterPreviewSupport {
    bool supported{};
    EmitterBlend blend{EmitterBlend::Normal};
    std::string reason;
    EmitterUpdate update{EmitterUpdate::Fountain};
    EmitterRender render{EmitterRender::Normal};
};
// Supported modes are explicit: unsupported world-dependent effects are never
// silently interpreted as a normal Fountain.
EmitterPreviewSupport emitterPreviewSupport(const EmitterNodeData& data);
std::vector<std::string> emitterPreviewWarnings(const EmitterNodeData& data,
                                               const NodeEmitterState& state);
enum class ParticleQuality { Full, Reduced };
struct ParticleLimits {
    std::size_t maximumParticles{1024};
    float maximumBirthRate{4096.0f};
    float maximumLifetime{120.0f}; // negative authored lifetime remains immortal
    ParticleQuality quality{ParticleQuality::Full};
};
// Exact reduced-quality birth-rate constraint recovered from the K2 runtime.
// Full quality returns the authored rate unchanged. Viewer safety limits are
// applied separately by advanceEmitter().
float emitterBirthRateForQuality(const EmitterNodeData& data,
    const NodeEmitterState& state, EmitterUpdate update, ParticleQuality quality) noexcept;
// The binaries use the Win-compatible 15-bit LCG, shared by scene effects.
// An explicit stream makes replay reproducible without process-global state.
struct ParticleRandom {
    std::uint32_t state{1};
    std::uint32_t next() noexcept;
};
// Scene queries are explicit: the MDL does not contain a collision world or a
// target object. Wind returns DISPLACEMENT for this frame, not acceleration.
struct ParticleContact {
    Vec3 normal{0,0,1};
    float fraction{1}; // earliest segment hit, in [0,1]
};
enum class ParticleBounceMode { Legacy=1, Reflection=2 };
struct ParticleEnvironment {
    std::optional<Mat4> target;
    // Particle::randomDirection evaluates dead-space against the active camera.
    // Hosts without a camera may leave this unset; only dead-space correction is
    // skipped in that case.
    std::optional<Vec3> cameraPosition;
    // Inherit-local advances the emitter's stored source position by the owning
    // object's translation delta before birth/distance calculations. The MDL
    // node transform itself is still supplied separately as `world`.
    Vec3 ownerTranslationDelta{};
    std::function<Vec3(Vec3,float)> windDisplacement;
    std::function<std::optional<ParticleContact>(Vec3,Vec3)> trace;
    ParticleBounceMode bounceMode{ParticleBounceMode::Legacy};
    Vec3 objectTint{1,1,1}; // used only by emitters with the Tinted flag
    float objectAlpha{1};   // Gob alpha multiplier
    float tileAlpha{1};     // caller-resolved tile/fade multiplier
};
// K2 scene-space emitter support recovered from PartEmitter::CalculateRadius,
// AddEmitterToRooms, PartOutside and EmitterOrderCmp. These records are kept
// independent of NeoMDL so archive viewers or future area tools can supply an
// actual room/portal graph without reimplementing the runtime rules.
struct EmitterScenePlane {
    Vec3 normal{};
    float distance{};
};
struct EmitterSceneRoom {
    std::uint64_t identity{};
    float minimumX{}, minimumY{}, maximumX{}, maximumY{};
    std::vector<EmitterScenePlane> visibilityPlanes;
    bool active{true};
};
struct EmitterAttachedObjectSphere {
    Vec3 center{};
    float radius{};
};
struct EmitterRadiusInput {
    const NodeEmitterState* state{};
    Vec3 emitterPosition{};
    Vec3 globalWind{};
    std::optional<Vec3> targetPosition;
    std::vector<EmitterAttachedObjectSphere> attachedObjects;
    // The binary checks an owner-model runtime flag before every other branch.
    // Its setter has not been identified, so hosts must opt into it explicitly.
    bool forceUnbounded{};
};
float calculateK2EmitterRadius(const EmitterRadiusInput& input) noexcept;

// When the owner has not moved, K2 reuses only its first cached room. A moved
// owner is tested against every room's inclusive XY bounds; no match means all
// rooms. Room Z extents are not consulted by AddEmitterToRooms.
std::vector<std::size_t> k2EmitterRoomMembership(
    Vec3 currentOwnerPosition, Vec3 previousOwnerPosition,
    const std::vector<std::size_t>& cachedOwnerRooms,
    const std::vector<EmitterSceneRoom>& rooms);

struct EmitterCullPolicy {
    // Android K2 rejects emitters whose projected-radius proxy becomes tiny.
    // The macOS implementation omits this preliminary gate.
    bool screenSizeGate{true};
    float maximumScreenRadius{40.0f};
    float nearSurfaceDistance{15.0f};
    float minimumRadiusRatio{0.05f};
};
bool k2EmitterOutside(Vec3 center, float radius,
                      std::optional<Vec3> cameraPosition,
                      const std::vector<EmitterScenePlane>& planes,
                      const EmitterCullPolicy& policy = {}) noexcept;

struct EmitterBucketEntry {
    std::size_t emitterIndex{};
    std::uint64_t identity{};
    std::uint64_t ownerIdentity{};
    bool hasOwner{};
    std::int16_t renderOrder{};
    std::int32_t category{};
    Vec3 ownerPosition{};
    Vec3 emitterPosition{};
};
int compareK2EmitterBucketEntries(const EmitterBucketEntry& left,
                                  const EmitterBucketEntry& right,
                                  Vec3 cameraPosition) noexcept;
void sortK2EmitterBucket(std::vector<EmitterBucketEntry>& entries,
                         Vec3 cameraPosition);

struct ParticleBlast {
    Vec3 center{};
    float radius{}, remaining{}, strength{1};
};
// Compatibility record retained for hosts using the particle API directly.
// The calculation now follows the exact K2 WindManager point-source path.
Vec3 particleBlastDisplacement(const ParticleBlast& blast, Vec3 point) noexcept;
struct ParticleTriangle { Vec3 a{},b{},c{}; };
// Bounded, point-segment collision proxy. This does not claim equivalence to
// the game's area/walkmesh collision categories or swept-volume queries.
class ParticleCollisionMesh {
public:
    bool build(std::vector<ParticleTriangle> triangles);
    std::optional<ParticleContact> trace(Vec3 from,Vec3 to) const noexcept;
    std::size_t triangleCount() const noexcept { return triangles_.size(); }
private:
    struct Branch {Vec3 minimum{},maximum{};std::size_t start{},count{},left{},right{};};
    std::size_t buildBranch(std::size_t start,std::size_t count);
    std::vector<ParticleTriangle> triangles_;
    std::vector<Branch> branches_;
};
std::optional<ParticleContact> traceParticleGround(Vec3 from,Vec3 to,float height) noexcept;
struct EmitterParticle {
    Vec3 position{}, velocity{}; // scene space; no attachment transform on draw
    float age{}, lifetime{}, rotation{}, sourceScale{1.0f};
    std::uint64_t serial{};
    Vec3 tail{}, inheritedDirection{}, direction{0,0,1}, right{1,0,0}, up{0,1,0};
    float inheritedSpeed{};
    std::int32_t frame{};
    float frameOffset{}, lastFrameAge{};
    bool stopped{};
    Vec3 contactNormal{0,0,1};
};
// A K2 lightning branch is a one-level child LightningEmitter. Its source is
// attached to a main-ribbon particle while its target is either the main
// target object or a free endpoint retained until the next branch refresh.
struct LightningBranchState {
    std::size_t attachment{}, particleCount{};
    float scale{1}, lightningAge{};
    Vec3 target{}, tangentStart{}, tangentEnd{};
    bool targetsMain{};
    std::vector<Vec3> offsets;
};
struct EmitterSimulation {
    std::vector<EmitterParticle> particles;
    double accumulator{}; // accumulated simulated time, not a hidden substep timer
    float birthRemainder{}; // engine time-emission accumulator, reset on emission
    std::uint64_t seed{}, nextSerial{}, droppedBirths{}, invalidEnvironmentSamples{}, detonations{};
    ParticleRandom random;
    Mat4 world=identityMatrix(), previousWorld=identityMatrix();
    Vec3 distanceRemainder{};
    bool initialized{}, previousDetonate{};
    std::size_t freeParticles{};
    std::uint32_t pendingBursts{};
    // Lightning holds a persistent ribbon and two control-point states. Each
    // path starts a new strip; branch boundaries must never generate joins.
    std::vector<std::size_t> ribbonStarts;
    std::vector<Vec3> lightningPrevious, lightningNext, lightningOffsets;
    std::vector<Vec3> tangentPrevious, tangentNext;
    std::vector<LightningBranchState> lightningBranches;
    float lightningAge{}, controlAge{};
    std::size_t lightningMainCount{}, lightningActiveBranches{};
    bool lightningReady{};
    Vec3 renderTint{1,1,1};
    float renderAlpha{1};
    void reset(std::uint64_t newSeed=0) noexcept;
    void detonate() noexcept; // explicit runtime event, not an invented periodic burst
};
// One caller-supplied simulation frame. Update order, birth accounting, motion,
// lifetime curves and frame stepping follow the inspected K2 paths. Safety caps
// remain viewer policy. dt is bounded to 0.25 s; there are no invisible 60 Hz
// subdivisions. Pass a shared RNG to reproduce cross-emitter call ordering.
void advanceEmitter(const EmitterNodeData& data, const NodeEmitterState& state,
                    const Mat4& world, double elapsedSeconds,
                    EmitterSimulation& simulation, const ParticleLimits& limits={},
                    ParticleRandom* sharedRandom=nullptr,
                    const ParticleEnvironment* environment=nullptr);
// Rendering advances texture frames in the original engine. Keeping this
// separate prevents shadow passes, bounds queries or inspector reads from
// consuming RNG/advancing animation. Invoke once per presented effect frame.
void updateEmitterParticleFrames(const EmitterNodeData& data,
    const NodeEmitterState& state, EmitterSimulation& simulation,
    ParticleRandom* sharedRandom=nullptr) noexcept;
// Does not advance age, spawn particles, or consume random numbers. The
// environment callbacks must not mutate the population being traversed.
void moveEmitterParticle(EmitterParticle& particle,const NodeEmitterState& state,
    const Mat4& previous,const Mat4& current,std::uint32_t flags,float dt,
    const ParticleEnvironment* environment=nullptr);
struct ParticleAppearance {
    std::array<float,4> color{};
    Vec2 size{};
    std::uint32_t frame{}, columns{1}, rows{1};
};
ParticleAppearance emitterParticleAppearance(const EmitterNodeData& data,
    const NodeEmitterState& state, const EmitterParticle& particle) noexcept;
// Bounded equivalent of the K2 recursive-fractal primitive. Degenerate
// amplitudes that cause a divide-by-zero in the binary are handled finitely.
void lightningFractal(std::vector<Vec3>& offsets, std::size_t index, std::size_t span,
    Vec3 direction, float inherited, float amplitude, float radius, ParticleRandom& random);
struct ParticleFrameSample { std::int32_t frame{-1}; float weight{}; };
// Base, next, previous, next-next, previous-previous. The binary's five-pass
// weights and endpoint behavior are intentionally not normalized to a crossfade.
std::array<ParticleFrameSample,5> emitterParticleFrames(const EmitterNodeData& data,
    const NodeEmitterState& state, const EmitterParticle& particle) noexcept;
// Indexed quads/strips in EYE space. pass=0 is the base image; frame blending
// uses passes 1..4 in order. Standard quads are newest-first; linked and motion-blurred
// strips use their original forward order, not a camera-dependent sort. Texture coordinates use the original grid
// edges (no half-texel inset). width/height retained for source compatibility.
void buildEmitterMesh(const EmitterNodeData& data, const NodeEmitterState& state,
    const EmitterSimulation& simulation, const Mat4& view, Mesh& output,
    std::uint32_t textureWidth=0, std::uint32_t textureHeight=0,
    unsigned pass=0);
} // namespace neoshared::model
