#pragma once

#include <neoshared/model/MaterialAnimation.hpp>
#include <neoshared/model/EmitterState.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <stdexcept>
#include <vector>

namespace neoshared::model {

inline constexpr std::uint16_t kNodeBase = 0x0001;
inline constexpr std::uint16_t kNodeLight = 0x0002;
inline constexpr std::uint16_t kNodeEmitter = 0x0004;
inline constexpr std::uint16_t kNodeCamera = 0x0008;
inline constexpr std::uint16_t kNodeReference = 0x0010;
inline constexpr std::uint16_t kNodeMesh = 0x0020;
inline constexpr std::uint16_t kNodeSkin = 0x0040;
inline constexpr std::uint16_t kNodeAnimation = 0x0080;
inline constexpr std::uint16_t kNodeDangly = 0x0100;
inline constexpr std::uint16_t kNodeAabb = 0x0200;
inline constexpr std::uint16_t kNodeTrigger = 0x0400;
inline constexpr std::uint16_t kNodeLightsaber = 0x0800;

// MdlNodeAABB split masks used by PartAABB::HitCheckGeom. Equality on a
// segment component selects the negative direction, matching the K2 binary.
inline constexpr std::uint32_t kAabbPositiveX = 0x01u;
inline constexpr std::uint32_t kAabbPositiveY = 0x02u;
inline constexpr std::uint32_t kAabbPositiveZ = 0x04u;
inline constexpr std::uint32_t kAabbNegativeX = 0x08u;
inline constexpr std::uint32_t kAabbNegativeY = 0x10u;
inline constexpr std::uint32_t kAabbNegativeZ = 0x20u;
inline constexpr std::uint32_t kAabbDirectionMask = 0x3fu;

// Raw controller field identifiers used by the 32-bit binary MDL records.
// The 64-bit game ports translate these to pointer-sized runtime offsets.
inline constexpr std::uint32_t kControllerPosition = 8u;
inline constexpr std::uint32_t kControllerOrientation = 20u;
inline constexpr std::uint32_t kControllerScale = 36u;

// Flags accepted by the binary-observed animation event dispatcher.
inline constexpr std::uint32_t kAnimationEventReverse = 0x01u;
inline constexpr std::uint32_t kAnimationEventFullRange = 0x02u;

enum class GameVersion { Kotor1, Kotor2 };

struct Vec2 {
    float x{};
    float y{};
};

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct Quaternion {
    float x{};
    float y{};
    float z{};
    float w{1.0f};
};

// Column-major, consumable by OpenGL matrix uniforms without transposition.
struct Mat4 {
    std::array<float, 16> values{};
};

struct Bounds {
    Vec3 minimum{};
    Vec3 maximum{};
    bool valid{};
};

// On disk: bitangent, tangent, normal; three float3 vectors (36 bytes).
// The source vectors are retained verbatim when finite. valid additionally
// requires a non-degenerate basis; repair/generation belongs to the consumer.
struct TangentBasis {
    Vec3 bitangent{};
    Vec3 tangent{};
    Vec3 normal{};
    bool valid{};
};

struct MeshUvAnimation {
    std::uint32_t enabledRaw{};
    Vec2 direction{};
    float jitter{};
    float jitterSpeed{};
};

struct K2MeshMaterialFields {
    std::uint8_t dirtEnabledRaw{};
    std::int16_t dirtTexture{};
    std::int16_t dirtWorldspace{};
    std::uint8_t hologramDoNotDrawRaw{};
};

struct Vertex {
    Vec3 position{};
    Vec3 normal{};
    Vec2 texcoord0{};
    Vec2 texcoord1{};
    Vec2 texcoord2{};
    Vec2 texcoord3{};
    std::array<std::uint8_t, 4> color{{255u, 255u, 255u, 255u}};
    TangentBasis tangentBasis{};
};

// K2 MaxFace is 32 bytes. The AABB hit path consumes all fields retained here:
// the authored plane, surface-material index, and three vertex indices.
struct MeshFace {
    Vec3 normal{};
    float planeDistance{};
    std::uint32_t surfaceMaterial{};
    std::array<std::int16_t, 3> adjacentFaces{{-1, -1, -1}};
    // PartAABB sign-extends these 16-bit fields before indexing the vertex array.
    std::array<std::int16_t, 3> indices{};
};

struct AabbTreeNode {
    Vec3 minimum{};
    Vec3 maximum{};
    std::int32_t firstChild{-1};
    std::int32_t secondChild{-1};
    std::int32_t faceIndex{-1};
    std::uint32_t splitMask{};
    std::uint32_t sourceOffset{};

    bool leaf() const noexcept { return faceIndex != -1; }
};

struct AabbNodeData {
    std::vector<AabbTreeNode> nodes;
    std::optional<std::size_t> root;
    bool valid{};
};

struct AabbHitOptions {
    // CHitInfo supplies a 32-bit material mask. A face is eligible when the
    // bit corresponding to its authored surface-material index is set.
    std::uint32_t surfaceMask{0xffffffffu};
    float scale{1.0f};
    // K2's aabb_testopposite debug global reverses the split-guided child order.
    bool testOpposite{};
};

struct AabbHit {
    Vec3 position{};
    Vec3 normal{};
    std::uint32_t surfaceMaterial{};
    std::uint32_t surfaceMask{};
    std::size_t faceIndex{};
    std::size_t treeNodeIndex{};
    std::size_t intersectionCount{};
    float segmentFraction{};
};

enum class ControllerValueKind : std::uint8_t {
    Unknown = 0u,
    Float = 1u,
    PackedQuaternion = 2u,
    Vector = 3u,
    Quaternion = 4u,
};

enum class ControllerInterpolation : std::uint8_t {
    Linear = 0u,
    Bezier = 1u,
    Unknown = 0xffu,
};

struct Controller {
    std::uint32_t type{};
    std::int16_t sourceOffset{};
    std::uint16_t rowCount{};
    std::uint16_t timeIndex{};
    std::uint16_t dataIndex{};
    std::uint8_t rawFlags{};
    ControllerValueKind valueKind{ControllerValueKind::Unknown};
    ControllerInterpolation interpolation{ControllerInterpolation::Unknown};
};

struct SkinInfluence {
    std::array<float, 4> weights{};
    std::array<std::uint16_t, 4> boneIndices{};
};

// K2 MdlNodeDanglyMesh32 appends this authored block to MdlNodeTriMesh32.
// InternalGenVertices copies the source constraint verbatim onto each unique
// generated runtime vertex. PartDanglyMesh::Animate uses those raw values; it
// does not normalize the common 0..255 authoring range.
struct DanglyMeshData {
    std::vector<float> constraints;
    float displacement{};
    float tightness{};
    float period{};
};

// Defaults follow the supplied K2 PartLight constructors. Authored finite
// values are retained; rendering clamps only its own display inputs.
struct NodeLightState {
    std::array<float,3> color{};
    float radius{};
    float shadowRadius{-1.0f};
    float verticalDisplacement{};
    float multiplier{1.0f};
    std::array<MaterialValueSource,5> sources{};
};

struct LightNodeData {
    float flareRadius{};
    std::vector<std::string> textureNames;
    std::vector<float> flareSizes;
    std::vector<float> flarePositions;
    std::vector<Vec3> flareColorShifts;
    // K2 stores an integer dynamic-light classification rather than a single
    // boolean. dynamic is retained as a convenience for callers.
    std::int32_t dynamicType{};
    bool ambientOnly{};
    bool castsShadow{};
    bool dynamic{};
    bool affectDynamic{};
    std::int32_t priority{};
    bool generateFlare{};
    bool fading{};
};

struct EmitterNodeData {
    float deadSpace{};
    float blastRadius{};
    float blastLength{};
    std::uint32_t branchCount{};
    bool controlPointSmoothing{};
    std::uint32_t xGrid{};
    std::uint32_t yGrid{};
    std::uint32_t spawnType{};
    std::string updateMode;
    std::string renderMode;
    std::string blendMode;
    std::string texture;
    std::string chunkName;
    bool twoSidedTexture{};
    bool loop{};
    std::uint16_t renderOrder{};
    bool frameBlending{};
    std::string depthTexture;
    // The binary record contains additional engine-specific flags and values.
    // Keeping the complete 224-byte extension as words makes diagnostics and
    // later controller/particle parity work possible without unsafe aliasing.
    std::array<std::uint32_t, 56> rawWords{};
};

struct CameraNodeData {
    // Camera nodes have no additional authored K2 binary payload. Their base
    // node transform defines the preview camera glyph.
};

struct TriggerNodeData {
    // The supplied K2 implementation has a base-node-only MdlNodeTrigger:
    // InternalParseField is empty and InternalCreateInstance returns null.
    // Runtime PartTrigger volumes are created separately from game-object
    // footprint points through AurPartTriggerCreate.
    bool binaryVerified{};
    bool hasPrivatePayload{};
    bool createsRuntimePart{};
};

struct ReferenceNodeData {
    std::string model;
    bool reattachable{};
};

struct LightsaberNodeData {
    static constexpr std::uint16_t frontSegments = 20u;
    static constexpr std::uint16_t backSegments = 20u;
    static constexpr std::uint16_t pieceVertices = 8u;
    static constexpr std::uint16_t segmentVertices = 4u;
    static constexpr std::size_t frontPieceOffset = 0u;
    static constexpr std::size_t frontSegmentOffset = pieceVertices;
    static constexpr std::size_t backPieceOffset =
        frontSegmentOffset + static_cast<std::size_t>(frontSegments) * segmentVertices;
    static constexpr std::size_t backSegmentOffset = backPieceOffset + pieceVertices;
    static constexpr std::size_t generatedVertexCount =
        backSegmentOffset + static_cast<std::size_t>(backSegments) * segmentVertices;

    // K2 MdlNodeLightsaber::InternalPostProcess builds four parallel arrays.
    // positions/normals/texcoords contain the generated 176-vertex stream;
    // originalPositions mirrors the engine's count+1 baseline position array.
    std::vector<Vec3> positions;
    std::vector<Vec3> originalPositions;
    std::vector<Vec3> normals;
    std::vector<Vec2> texcoords;
    bool postprocessed{};
};

struct Mesh {
    std::size_t nodeIndex{};
    std::string texture0;
    std::string texture1;
    Vec3 diffuse{1.0f, 1.0f, 1.0f};
    Vec3 ambient{0.2f, 0.2f, 0.2f};
    std::uint32_t transparencyHint{};
    std::uint32_t mdxBitmap{};
    std::uint32_t mdxRowSize{};
    std::uint32_t mdxColorOffset{0xffffffffu};
    std::array<std::uint32_t, 4> mdxTexcoordOffsets{{
        0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu}};
    std::uint32_t mdxTangentOffset{0xffffffffu};
    std::size_t validTangentVertices{};
    bool hasVertexColors{};
    MeshUvAnimation uvAnimation{};
    std::uint16_t textureCount{};
    bool rotateTexture{};
    bool beaming{};
    float totalArea{};
    std::optional<K2MeshMaterialFields> k2Material;
    // Full common mesh extension, including unknown words and padding. Kept
    // for inspection; it is not a lossless MDL/MDX serialization API.
    std::vector<std::uint8_t> rawMeshHeader;
    std::uint32_t mdxWeightOffset{0xffffffffu};
    std::uint32_t mdxBoneIndexOffset{0xffffffffu};
    bool lightmapped{};
    bool castsShadow{};
    bool backgroundGeometry{};
    bool render{true};
    bool skinned{};
    bool hasSkinChannels{};
    bool hasSkinBoneMap{};
    bool hasInverseBindPose{};
    std::optional<DanglyMeshData> dangly;
    std::array<std::int16_t, 16> skinBoneMap{};
    std::vector<SkinInfluence> skinInfluences;
    // K2 MdlNodeSkin32 stores model-space inverse-bind transforms as separate
    // quaternion and translation arrays. Quaternions are converted from the
    // engine's w,x,y,z storage order to this API's x,y,z,w order.
    std::vector<Quaternion> inverseBindOrientations;
    std::vector<Vec3> inverseBindTranslations;
    // Parsed for diagnostics and future parity work. The supplied K2 binaries
    // do not consume this array in the normal RenderSkinned path.
    std::vector<std::int32_t> boneConstantIndices;
    std::vector<MeshFace> faces;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct Node {
    std::uint16_t flags{};
    std::uint16_t id{};
    std::string name;
    std::optional<std::size_t> parent;
    Vec3 position{};
    Quaternion orientation{};
    std::vector<std::size_t> children;
    std::optional<std::size_t> mesh;
    std::optional<LightNodeData> light;
    std::optional<EmitterNodeData> emitter;
    std::optional<CameraNodeData> camera;
    std::optional<TriggerNodeData> trigger;
    std::optional<ReferenceNodeData> reference;
    std::optional<LightsaberNodeData> lightsaber;
    std::optional<AabbNodeData> aabb;
    std::vector<Controller> controllers;
    // Controller values are retained as raw 32-bit words because compact
    // quaternion keys occupy the same array but are not IEEE-754 floats.
    std::vector<std::uint32_t> controllerDataWords;
};

struct AnimationEvent {
    float time{};
    std::string name;
};

struct Animation {
    std::string name;
    float length{};
    float transitionTime{};
    std::string root;
    std::vector<Node> nodes;
    std::vector<AnimationEvent> events;
};

// A resolved clip in a derived-model -> supermodel hierarchy. modelIndex is an
// index into the hierarchy supplied to findAnimation()/enumerateAnimations().
// positionScale is the product of animationScale values on the descendants
// traversed before the clip owner, matching FindAnimScale in the K2 binaries.
struct AnimationReference {
    std::size_t modelIndex{};
    std::size_t animationIndex{};
    std::size_t inheritanceDepth{};
    float positionScale{1.0f};
    bool defaultFallback{};
};

struct AnimationEventOccurrence {
    std::size_t eventIndex{};
    float time{};
    std::string name;
};

struct Model {
    GameVersion game{GameVersion::Kotor1};
    std::string name;
    std::string supermodel;
    std::uint16_t classification{};
    std::uint8_t subclassification{};
    bool fog{};
    float animationScale{1.0f};
    std::uint32_t declaredAnimationCount{};
    std::vector<Node> nodes;
    std::vector<Mesh> meshes;
    std::vector<Animation> animations;
    std::vector<Mat4> worldTransforms;
    Bounds bounds{};
    std::vector<std::string> warnings;
};

struct DeformedMesh {
    bool valid{};
    // PartDanglyMesh streams only positions; its authored normal/tangent
    // channels remain static and are transformed with the current part basis.
    // Skin deformation leaves preserveTangentBasis false and regenerates the
    // tangent frame from its posed positions and normals.
    bool preserveTangentBasis{};
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec3> tangents;
    std::vector<Vec3> bitangents;
};

// Semantic counterpart of K2 PartDanglyMesh plus VertexPrimitiveDangly.
// The supplied macOS x86-64/i386 implementations store the previous part
// position/orientation on PartDanglyMesh, generated local positions on the
// part, and one persistent local velocity per vertex on VertexPrimitiveDangly.
struct DanglyMeshState {
    bool initialized{};
    Vec3 previousPartPosition{};
    Quaternion previousPartOrientation{};
    std::vector<Vec3> localPositions;
    std::vector<Vec3> localVelocities;
};

struct DanglySimulationState {
    std::vector<DanglyMeshState> meshes;
};

// World inputs supplied by Scene::GetWindVector and the owning CAurObject.
// windDisplacement returns a displacement/impulse for the clamped frame time,
// not an acceleration. A missing objectWorld means identity.
struct DanglyEnvironment {
    std::optional<Mat4> objectWorld;
    std::function<Vec3(Vec3, float)> windDisplacement;
};

struct Pose {
    float time{};
    // When Animation::root names a valid animation node, this is the matching
    // target-model node. Only that node and its descendants receive animation
    // controllers; the engine does not extract its translation into a separate
    // actor-motion channel in the paths observed here.
    std::optional<std::size_t> controlledRootNode;
    std::vector<Vec3> positions;
    std::vector<Quaternion> orientations;
    std::vector<float> scales;
    // Same indexing as Model::nodes, including defaults for non-mesh nodes.
    std::vector<NodeMaterialState> nodeMaterials;
    std::vector<NodeLightState> nodeLights;
    std::vector<NodeEmitterState> nodeEmitters;
    std::vector<Mat4> worldTransforms;
    // Same indexing as Model::meshes. A valid entry contains model-space CPU
    // skinning output and must not receive the mesh-node transform a second time.
    std::vector<DeformedMesh> deformedMeshes;
    Bounds bounds{};
};

struct ParseLimits {
    std::size_t maximumMdlBytes{512u * 1024u * 1024u};
    std::size_t maximumMdxBytes{1024u * 1024u * 1024u};
    std::size_t maximumNodes{200000u};
    std::size_t maximumMeshes{200000u};
    std::size_t maximumVertices{10000000u};
    std::size_t maximumFaces{10000000u};
    std::size_t maximumAabbNodes{20000000u};
    std::size_t maximumAnimations{10000u};
    std::size_t maximumAnimationNodes{1000000u};
    std::size_t maximumAnimationEvents{100000u};
    std::size_t maximumControllers{1000000u};
    std::size_t maximumControllerValues{10000000u};
    std::size_t maximumNameLength{1024u};
    std::size_t maximumRecursionDepth{512u};
};

class ModelError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Model decodeModelBytes(const std::vector<std::uint8_t>& mdlBytes,
                       const std::vector<std::uint8_t>& mdxBytes = {},
                       const std::string& sourceLabel = {},
                       const ParseLimits& limits = {});

Model readModelFile(const std::filesystem::path& mdlPath,
                    const std::optional<std::filesystem::path>& mdxPath = std::nullopt,
                    const ParseLimits& limits = {});

Mat4 identityMatrix() noexcept;
Mat4 multiply(const Mat4& left, const Mat4& right) noexcept;
Mat4 nodeTransform(const Vec3& position, const Quaternion& orientation) noexcept;
Mat4 nodeTransform(const Vec3& position, const Quaternion& orientation, float uniformScale) noexcept;
Vec3 transformPoint(const Mat4& matrix, const Vec3& point) noexcept;
Vec3 transformDirection(const Mat4& matrix, const Vec3& direction) noexcept;
Pose bindPose(const Model& model);

// Reproduces PartAABB::HitCheckGeom in node-local coordinates. The segment is
// directed: only front-to-back crossings of authored face planes are accepted.
// Traversal shortens the endpoint after every accepted leaf and returns the
// nearest surviving hit together with the number of accepted intersections.
std::optional<AabbHit> hitTestAabb(const Model& model, std::size_t nodeIndex,
                                   const Vec3& start, const Vec3& end,
                                   const AabbHitOptions& options = {});

// Reproduces the K2 PartDanglyMesh runtime recovered from the supplied macOS
// x86-64/i386 executable, using the Android binary to cross-check the node and
// part layouts. K1 behavior is intentionally left unchanged/uninferred.
DanglySimulationState makeDanglySimulationState(const Model& model, const Pose& pose);
void resetDanglySimulation(const Model& model, const Pose& pose,
                           DanglySimulationState& state);
void simulateDanglyMeshes(const Model& model, Pose& pose,
                          DanglySimulationState& state, float elapsedSeconds,
                          const DanglyEnvironment& environment);

// Compatibility/viewer convenience: treats windVelocity as a constant world
// wind and converts it to the displacement returned by Scene::GetWindVector.
// The downward default preserves NeoMDL's existing standalone preview policy;
// callers with a scene wind manager should use DanglyEnvironment instead.
void simulateDanglyMeshes(const Model& model, Pose& pose,
                          DanglySimulationState& state, float elapsedSeconds,
                          Vec3 windVelocity = {0.0f, 0.0f, -9.80665f});

// hierarchy[0] is the most-derived model. Lookup checks local animations first,
// then each supermodel, and finally the terminal model's "default" animation.
std::optional<AnimationReference> findAnimation(
    const std::vector<const Model*>& hierarchy, const std::string& name);

// Returns the visible animation list with case-insensitive descendant overrides
// removed. References retain the inherited position-scale product.
std::vector<AnimationReference> enumerateAnimations(
    const std::vector<const Model*>& hierarchy);

// Reproduces AnimateEvents interval and wrap behavior. Forward intervals are
// (startTime, endTime]; reverse intervals are [endTime, startTime). Events are
// returned in the same order in which the engine dispatches them.
std::vector<AnimationEventOccurrence> animationEventsBetween(
    const Animation& animation, float startTime, float endTime,
    std::uint32_t flags = 0u);

Pose evaluateAnimation(const Model& model, std::size_t animationIndex, float time,
                       bool loop = true);
Pose evaluateAnimation(const Model& targetModel, const Model& animationModel,
                       std::size_t animationIndex, float time,
                       float positionScale, bool loop = true);
std::string gameVersionName(GameVersion game);
std::string nodeFlagsText(std::uint16_t flags);
std::string controllerTypeName(std::uint32_t type);
std::string modelSummary(const Model& model);

} // namespace neoshared::model
