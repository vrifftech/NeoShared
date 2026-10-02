#include <neoshared/model/Trigger.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace neoshared::model {
namespace {

constexpr float kInsideTolerance = 1.0e-5f;
constexpr float kDegenerateNormalSquared = 1.0e-16f;
constexpr std::size_t kMaximumDecomposeDepth = 1024u;

bool finite(float value) noexcept { return std::isfinite(value); }
bool finite(Vec3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

Vec3 add(Vec3 left, Vec3 right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}
Vec3 subtract(Vec3 left, Vec3 right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}
Vec3 multiply(Vec3 value, float scalar) noexcept {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}
float dot(Vec3 left, Vec3 right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}
Vec3 cross(Vec3 left, Vec3 right) noexcept {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}
float lengthSquared(Vec3 value) noexcept { return dot(value, value); }

Vec3 newellNormal(const Vec3* points, std::size_t count) noexcept {
    Vec3 normal{};
    for (std::size_t index = 0u; index < count; ++index) {
        const auto& current = points[index];
        const auto& next = points[(index + 1u) % count];
        normal.x += (current.y - next.y) * (current.z + next.z);
        normal.y += (current.z - next.z) * (current.x + next.x);
        normal.z += (current.x - next.x) * (current.y + next.y);
    }
    const auto squared = static_cast<double>(lengthSquared(normal));
    if (!(squared > kDegenerateNormalSquared) || !std::isfinite(squared))
        return {};
    return multiply(normal, static_cast<float>(1.0 / std::sqrt(squared)));
}

void extend(Bounds& bounds, Vec3 point) noexcept {
    if (!finite(point)) return;
    if (!bounds.valid) {
        bounds.minimum = bounds.maximum = point;
        bounds.valid = true;
        return;
    }
    bounds.minimum.x = std::min(bounds.minimum.x, point.x);
    bounds.minimum.y = std::min(bounds.minimum.y, point.y);
    bounds.minimum.z = std::min(bounds.minimum.z, point.z);
    bounds.maximum.x = std::max(bounds.maximum.x, point.x);
    bounds.maximum.y = std::max(bounds.maximum.y, point.y);
    bounds.maximum.z = std::max(bounds.maximum.z, point.z);
}

Bounds boundsOf(const std::vector<Vec3>& points) noexcept {
    Bounds result;
    for (const auto point : points) extend(result, point);
    return result;
}

bool pointInsideProjectedTriangle(const std::array<Vec3, 3>& triangle,
                                  Vec3 point) noexcept {
    // Decompose calls polyhit with the candidate triangle flattened to Z=0 and
    // a vertical segment from +1000 to -1000. polyhit is one-sided, so only a
    // positive-Z candidate winding can contain another footprint point.
    const auto first = subtract(triangle[1], triangle[0]);
    const auto second = subtract(triangle[2], triangle[0]);
    const auto area = first.x * second.y - first.y * second.x;
    if (!(area > 0.0f) || !finite(area)) return false;

    const auto edge = [](Vec3 a, Vec3 b, Vec3 p) noexcept {
        return (b.x - a.x) * (p.y - a.y) -
               (b.y - a.y) * (p.x - a.x);
    };
    return edge(triangle[0], triangle[1], point) >= -kInsideTolerance &&
           edge(triangle[1], triangle[2], point) >= -kInsideTolerance &&
           edge(triangle[2], triangle[0], point) >= -kInsideTolerance;
}

std::vector<std::size_t> cyclicPath(const std::vector<std::size_t>& indices,
                                    std::size_t first,
                                    std::size_t last) {
    std::vector<std::size_t> result;
    if (indices.empty()) return result;
    auto position = first;
    for (std::size_t count = 0u; count <= indices.size(); ++count) {
        result.push_back(indices[position]);
        if (position == last) break;
        position = (position + 1u) % indices.size();
    }
    return result;
}

bool decompose(const std::vector<Vec3>& points,
               std::vector<std::size_t> indices,
               std::vector<Vec3>& triangles,
               std::size_t depth) {
    if (indices.size() < 3u) return true;
    if (depth > kMaximumDecomposeDepth) return false;

    while (indices.size() >= 3u) {
        // The binary starts with 1,000,000 and selects the first strict lower X.
        float minimumX = 1000000.0f;
        std::size_t currentPosition = indices.size();
        for (std::size_t position = 0u; position < indices.size(); ++position) {
            const auto x = points[indices[position]].x;
            if (x < minimumX) {
                minimumX = x;
                currentPosition = position;
            }
        }
        // Preserve useful behavior for finite coordinates outside the original
        // sentinel range rather than indexing the binary's -1 result.
        if (currentPosition == indices.size()) {
            currentPosition = static_cast<std::size_t>(std::min_element(
                indices.begin(), indices.end(), [&points](std::size_t left,
                                                         std::size_t right) {
                    return points[left].x < points[right].x;
                }) - indices.begin());
        }

        const auto previousPosition =
            (currentPosition + indices.size() - 1u) % indices.size();
        const auto nextPosition = (currentPosition + 1u) % indices.size();
        const auto previousIndex = indices[previousPosition];
        const auto currentIndex = indices[currentPosition];
        const auto nextIndex = indices[nextPosition];
        std::array<Vec3, 3> candidate{{points[previousIndex],
                                       points[currentIndex],
                                       points[nextIndex]}};
        for (auto& point : candidate) point.z = 0.0f;

        std::optional<std::size_t> interiorPosition;
        float interiorMinimumX = 1000000.0f;
        for (std::size_t position = 0u; position < indices.size(); ++position) {
            if (position == previousPosition || position == currentPosition ||
                position == nextPosition) {
                continue;
            }
            auto point = points[indices[position]];
            point.z = 0.0f;
            if (pointInsideProjectedTriangle(candidate, point) &&
                point.x < interiorMinimumX) {
                interiorMinimumX = point.x;
                interiorPosition = position;
            }
        }

        if (interiorPosition) {
            // The recovered routine splits at the selected contained vertex and
            // recursively decomposes one cyclic sub-list while continuing with
            // the other. Recursing both paths is the equivalent ownership-free
            // representation of those List<int> mutations.
            auto first = cyclicPath(indices, currentPosition, *interiorPosition);
            auto second = cyclicPath(indices, *interiorPosition, currentPosition);
            if (first.size() < 3u || second.size() < 3u ||
                first.size() >= indices.size() || second.size() >= indices.size()) {
                return false;
            }
            if (!decompose(points, std::move(first), triangles, depth + 1u))
                return false;
            return decompose(points, std::move(second), triangles, depth + 1u);
        }

        triangles.push_back(points[previousIndex]);
        triangles.push_back(points[currentIndex]);
        triangles.push_back(points[nextIndex]);
        indices.erase(indices.begin() + static_cast<std::ptrdiff_t>(currentPosition));
    }
    return true;
}

bool segmentIntersectsBounds(Vec3 start, Vec3 end,
                             const Bounds& bounds) noexcept {
    if (!bounds.valid) return false;
    float minimum = 0.0f;
    float maximum = 1.0f;
    const std::array<float, 3> origin{{start.x, start.y, start.z}};
    const std::array<float, 3> direction{{end.x - start.x,
                                          end.y - start.y,
                                          end.z - start.z}};
    const std::array<float, 3> lower{{bounds.minimum.x, bounds.minimum.y,
                                      bounds.minimum.z}};
    const std::array<float, 3> upper{{bounds.maximum.x, bounds.maximum.y,
                                      bounds.maximum.z}};
    for (std::size_t axis = 0u; axis < 3u; ++axis) {
        if (std::fabs(direction[axis]) <= 1.0e-20f) {
            if (origin[axis] < lower[axis] || origin[axis] > upper[axis])
                return false;
            continue;
        }
        const auto reciprocal = 1.0f / direction[axis];
        auto first = (lower[axis] - origin[axis]) * reciprocal;
        auto second = (upper[axis] - origin[axis]) * reciprocal;
        if (first > second) std::swap(first, second);
        minimum = std::max(minimum, first);
        maximum = std::min(maximum, second);
        if (minimum > maximum) return false;
    }
    return true;
}

std::optional<TriggerHit> triangleHit(const Vec3* triangle,
                                      std::size_t triangleIndex,
                                      Vec3 start, Vec3 end,
                                      std::uintptr_t owner) noexcept {
    const auto normal = newellNormal(triangle, 3u);
    if (lengthSquared(normal) <= kDegenerateNormalSquared) return std::nullopt;
    const auto plane = dot(normal, triangle[0]);
    const auto startDistance = dot(normal, start) - plane;
    const auto endDistance = dot(normal, end) - plane;
    if (!(startDistance >= 0.0f) || !(endDistance <= 0.0f) ||
        startDistance == endDistance) {
        return std::nullopt;
    }
    const auto fraction = startDistance / (startDistance - endDistance);
    if (!finite(fraction)) return std::nullopt;
    const auto position = add(start, multiply(subtract(end, start), fraction));

    for (std::size_t edge = 0u; edge < 3u; ++edge) {
        const auto& first = triangle[edge];
        const auto& second = triangle[(edge + 1u) % 3u];
        const auto side = dot(cross(subtract(second, first),
                                    subtract(position, first)), normal);
        if (side < -kInsideTolerance) return std::nullopt;
    }

    return TriggerHit{position, normal, triangleIndex, owner, 2.0f, fraction};
}

} // namespace

TriggerVolume makeK2TriggerVolume(std::vector<Vec3> footprint,
                                  std::array<float, 4> color,
                                  std::uintptr_t owner) {
    TriggerVolume result;
    result.footprint = std::move(footprint);
    result.color = color;
    result.owner = owner;
    result.bounds = boundsOf(result.footprint);
    result.highlight = {};

    if (result.footprint.size() < 3u) {
        result.diagnostic = "K2 trigger footprints require at least three points.";
        return result;
    }
    if (!std::all_of(result.footprint.begin(), result.footprint.end(),
                     [](Vec3 point) { return finite(point); })) {
        result.diagnostic = "K2 trigger footprint contains a non-finite point.";
        return result;
    }

    std::vector<std::size_t> indices(result.footprint.size());
    std::iota(indices.begin(), indices.end(), 0u);
    result.triangles.reserve((result.footprint.size() - 2u) * 3u);
    if (!decompose(result.footprint, std::move(indices), result.triangles, 0u) ||
        result.triangles.size() < 3u || result.triangles.size() % 3u != 0u) {
        result.triangles.clear();
        result.diagnostic =
            "K2 PartTrigger decomposition could not form a bounded triangle stream.";
        return result;
    }
    result.valid = true;
    return result;
}

void setK2TriggerHighlighted(TriggerVolume& volume, bool highlighted,
                             float height, bool group) {
    volume.highlighted = highlighted;
    volume.highlightGroup = group;
    volume.highlightHalfHeight = std::fabs(height);
    if (volume.footprint.size() < 2u || !highlighted) return;

    volume.sideStrip.clear();
    volume.top.clear();
    volume.bottom.clear();
    volume.sideStrip.reserve(volume.footprint.size() * 2u + 2u);
    volume.top.reserve(volume.footprint.size());
    volume.bottom.reserve(volume.footprint.size());
    for (const auto point : volume.footprint) {
        const Vec3 top{point.x, point.y, point.z + volume.highlightHalfHeight};
        const Vec3 bottom{point.x, point.y,
                          point.z - volume.highlightHalfHeight};
        volume.sideStrip.push_back(top);
        volume.top.push_back(top);
        volume.sideStrip.push_back(bottom);
    }
    // SetHighlighted emits the bottom polygon from the final footprint point
    // back to the first so the two caps have opposite winding.
    for (auto position = volume.footprint.rbegin();
         position != volume.footprint.rend(); ++position) {
        volume.bottom.push_back(
            {position->x, position->y,
             position->z - volume.highlightHalfHeight});
    }
    volume.sideStrip.push_back(volume.sideStrip[0]);
    volume.sideStrip.push_back(volume.sideStrip[1]);
    volume.bounds = boundsOf(volume.sideStrip);
}

void setK2TriggerHighlightParameters(
    TriggerVolume& volume, const TriggerHighlightParameters& parameters) {
    volume.highlight = parameters;
}

void setK2TriggerHighlightColor(TriggerVolume& volume, Vec3 color,
                                float alpha) {
    volume.highlight.color = color;
    volume.highlight.alpha = alpha;
}

float k2TriggerEffectiveBaseAlpha(const TriggerVolume& volume,
                                  float globalAlpha) noexcept {
    return volume.color[3] + globalAlpha;
}

float k2TriggerHighlightAlpha(const TriggerVolume& volume,
                              float scenePhase) noexcept {
    if (!volume.highlight.pulse) return volume.highlight.alpha;
    return volume.highlight.alpha * 0.5f *
        (std::sin(volume.highlight.pulseRate * scenePhase) + 0.75f);
}

std::optional<TriggerHit> hitTestK2Trigger(
    const TriggerVolume& volume, const Vec3& start, const Vec3& end,
    const TriggerHitOptions& options) {
    if (!options.geometryEnabled || !volume.valid ||
        !segmentIntersectsBounds(start, end, volume.bounds)) {
        return std::nullopt;
    }
    for (std::size_t offset = 0u; offset + 2u < volume.triangles.size();
         offset += 3u) {
        if (const auto hit = triangleHit(volume.triangles.data() + offset,
                                         offset / 3u, start, end,
                                         volume.owner)) {
            return hit;
        }
    }
    return std::nullopt;
}

bool k2TriggerOutsideFrustum(const TriggerVolume& volume,
                             const std::vector<TriggerPlane>& planes,
                             std::size_t& cachedPlane) noexcept {
    if (!volume.bounds.valid || planes.empty()) return false;
    cachedPlane %= planes.size();
    for (std::size_t offset = 0u; offset < planes.size(); ++offset) {
        const auto index = (cachedPlane + offset) % planes.size();
        const auto& plane = planes[index];
        const Vec3 corner{
            plane.normal.x > 0.0f ? volume.bounds.minimum.x
                                  : volume.bounds.maximum.x,
            plane.normal.y > 0.0f ? volume.bounds.minimum.y
                                  : volume.bounds.maximum.y,
            plane.normal.z > 0.0f ? volume.bounds.minimum.z
                                  : volume.bounds.maximum.z};
        const auto distance = dot(plane.normal, corner) + plane.distance;
        if (distance > 0.0f) {
            cachedPlane = offset;
            return true;
        }
    }
    return false;
}

} // namespace neoshared::model
