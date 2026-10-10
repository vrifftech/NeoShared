#pragma once

#include <neoshared/bif/JobControl.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

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
};
struct BwmFile {
    std::uint32_t type{};
    Vector position{};
    std::vector<Vector> vertices;
    std::vector<Face> faces;
    std::vector<AabbNode> nodes;
    std::int32_t root{};
    std::vector<Vector> faceNormals;
    std::vector<float> faceDistances;
};

// BWM V1.0 fields consumed by native room queries. Adjacency and transitions
// remain outside this decoder's current contract. Binary room
// positions/AABBs are already in world space; relocation is consumer policy.
BwmFile readBwm(const std::vector<std::uint8_t>& data,const std::string& sourceName,
                const bif::JobControl& job = {});

} // namespace neoshared::bwm
