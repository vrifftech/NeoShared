#pragma once

#include <neoshared/bif/JobControl.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <set>

namespace neoshared::bwm {

using Vector = std::array<float,3>;
struct Face {
    std::array<std::uint32_t,3> vertices{};
    std::uint32_t material{};
};
struct AabbNode {
    Vector minimum{},maximum{};
    std::int32_t face{-1};
    std::int32_t splitMask{};
    std::int32_t firstChild{-1},secondChild{-1};
    std::uint32_t reserved{};
};
struct Edge {
    std::uint32_t index{}; // face * 3 + corner
    std::int32_t transition{-1};
};
struct BwmFile {
    std::uint32_t type{};
    std::array<Vector,2> usePoints{},doorUsePoints{};
    Vector position{};
    std::vector<Vector> vertices;
    std::vector<Face> faces;
    std::vector<AabbNode> nodes;
    std::int32_t root{};
    std::vector<Vector> faceNormals;
    std::vector<float> faceDistances;
    std::vector<std::array<std::int32_t,3>> adjacency;
    std::vector<Edge> edges;
    std::vector<std::uint32_t> perimeters; // exclusive ends in edges
};

// Binary room vertices/AABBs are already in world space; position is retained
// header data, not an extra translation to apply during serialization.
BwmFile readBwm(const std::vector<std::uint8_t>& data,const std::string& sourceName,
                const bif::JobControl& job = {});
std::vector<std::uint8_t> writeBwm(const BwmFile& mesh,const bif::JobControl& job = {});

// Rebuilds planes and a complete balanced tree without changing face order.
void buildAabb(BwmFile& mesh,const bif::JobControl& job = {});
// Stable walkable-first face ordering, planes, adjacency, boundary loops and
// AABB. Existing edge links are remapped with their face/corner identities.
void buildBwm(BwmFile& mesh,const std::set<std::uint32_t>& walkableMaterials,const bif::JobControl& job = {});

} // namespace neoshared::bwm
