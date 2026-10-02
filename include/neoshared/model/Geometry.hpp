#pragma once
#include "neoshared/model/Model.hpp"

namespace neoshared::model {

// API-neutral interleaved vertex stream. No GL or wxWidgets dependency.
struct RenderVertex {
    Vec3 position{};
    Vec3 normal{};
    Vec2 texcoord0{};
    Vec2 texcoord1{};
    Vec3 tangent{};
    float tangentHandedness{1.0f};
    std::array<float, 4> color{{1.0f, 1.0f, 1.0f, 1.0f}};
};
struct RenderGeometry {
    std::vector<RenderVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::size_t authoredTangents{};
    std::size_t generatedTangents{};
    std::size_t skippedTriangles{};
};

// Source data remains unchanged. Static authored tangents are orthogonalized;
// missing/invalid bases are generated. Deformed positions/normals are model
// space, so their tangent frames are regenerated in the same space, never
// silently copied from a bind-pose basis. This is a viewer fallback, not an
// implementation of the game's tangent-generation or skinning algorithm.
RenderGeometry prepareRenderGeometry(const Mesh& mesh,
                                     const DeformedMesh* deformed = nullptr);

// Inverse-transpose of the upper-left 3x3, column-major. Singular/non-finite
// input returns false and identity, avoiding NaN uniforms during zero scales.
bool makeNormalMatrix(const Mat4& matrix, std::array<float, 9>& result) noexcept;
float transformHandedness(const Mat4& matrix) noexcept;

} // namespace neoshared::model
