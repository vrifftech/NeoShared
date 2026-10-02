#pragma once

#include <neoshared/model/Model.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace neoshared::model {

// Runtime record stored by KOTOR II WindManager. The manager owns point winds
// by value; there is no source owner or public per-source removal handle in the
// inspected implementations. Expiry is handled by updateTimer().
struct PointSourceWind {
    Vec3 position{};
    float radius{};
    float remainingLifetime{};
    float strength{1.0f};
};

// Exact K2 point-source calculation. Distance is Chebyshev/max-axis distance,
// the radius comparison is strict, and a query at the source center uses the
// engine normalize() fallback direction of positive X.
Vec3 pointSourceWindVector(const PointSourceWind& source, Vec3 position) noexcept;

struct K2AreaWindPreset {
    Vec3 globalWind{};
    float maximumDeviationZDegrees{};
    float maximumDeviationXDegrees{};
};

// CSWCArea::SetWind accepts only 0, 1 and 2. The returned values are those sent
// to the current Scene's WindManager; rooms do not own independent managers.
std::optional<K2AreaWindPreset> k2AreaWindPreset(std::uint8_t mode) noexcept;

class WindManager final {
public:
    using NoiseField = std::array<std::array<float, 16>, 16>;
    using RadiusCallback = std::function<void()>;
    // rand_wincompatible is process-global in K2. Hosts can supply the same
    // shared 15-bit stream used by their other scene effects.
    using RandomCallback = std::function<std::uint32_t()>;

    WindManager();
    WindManager(Vec3 globalWind, float maximumDeviationZRadians,
                float maximumDeviationXRadians, float interpolationPeriod,
                std::uint32_t randomSeed = 1u);

    // Reconstructs both noise fields and resets temporal/point-source state.
    // Registered emitters remain registered and are notified once, matching a
    // subsequent SetGlobalWind call. The explicit seed drives the standalone
    // stream; a supplied RandomCallback instead consumes the host's shared
    // rand_wincompatible sequence, as the game does.
    void reset(Vec3 globalWind = {}, float maximumDeviationZRadians = 0.0f,
               float maximumDeviationXRadians = 0.0f,
               float interpolationPeriod = 2.0f,
               std::uint32_t randomSeed = 1u);

    void setRandomSource(RandomCallback source) { randomSource_ = std::move(source); }
    void clearRandomSource() noexcept { randomSource_ = {}; }
    void setGlobalWind(Vec3 wind);
    void setMaximumDeviation(float zRadians, float xRadians) noexcept;
    bool setInterpolationPeriod(float seconds) noexcept;
    // CSWCArea::SetWind mutates the active Scene manager in this order:
    // global vector first, then angular variation. Invalid modes leave state unchanged.
    bool applyK2AreaWind(std::uint8_t mode);

    const Vec3& globalWind() const noexcept { return globalWind_; }
    const Vec3& frameGlobalWind() const noexcept { return frameGlobalWind_; }
    float maximumDeviationZ() const noexcept { return maximumDeviationZ_; }
    float maximumDeviationX() const noexcept { return maximumDeviationX_; }
    float interpolation() const noexcept { return interpolation_; }
    float timer() const noexcept { return timer_; }
    float interpolationPeriod() const noexcept { return interpolationPeriod_; }
    std::uint32_t randomState() const noexcept { return randomState_; }

    // Called once by Scene after part and emitter wind consumers for the rendered
    // frame. The resulting timer/noise state is used by the following frame.
    // A strict timer > period swap discards overshoot and performs at most one
    // field transition, matching the K2 implementation.
    void updateTimer(float elapsedSeconds);

    // Both calls return scene-space DISPLACEMENT for this caller update. Point
    // winds are not multiplied by elapsedSeconds. GetGlobalPointWind's elapsed
    // argument is absent because the binary ignores it.
    Vec3 getGlobalWind(Vec3 position, float elapsedSeconds) const noexcept;
    Vec3 getGlobalWindNoPoint(Vec3 position, float elapsedSeconds) const noexcept;
    Vec3 getGlobalPointWind(Vec3 position) const noexcept;

    bool addPointSourceWind(Vec3 position, float radius, float lifetime,
                            float strength);
    void clearPointSourceWinds() noexcept { pointSources_.clear(); }
    const std::vector<PointSourceWind>& pointSources() const noexcept {
        return pointSources_;
    }

    // SetGlobalWind recalculates every uniquely registered affected-by-wind
    // emitter's radius in the game. NeoMDL supplies an equivalent callback
    // because its renderer does not expose PartEmitter objects directly.
    bool registerWindyEmitter(const void* identity, RadiusCallback calculateRadius);
    bool deregisterWindyEmitter(const void* identity) noexcept;
    void clearWindyEmitters() noexcept { windyEmitters_.clear(); }
    std::size_t registeredWindyEmitters() const noexcept {
        return windyEmitters_.size();
    }

private:
    struct RegisteredEmitter {
        const void* identity{};
        RadiusCallback calculateRadius;
    };

    std::uint32_t nextRandom() noexcept;
    void fillNoiseField(NoiseField& field) noexcept;
    float sample(std::size_t x, std::size_t y) const noexcept;
    Vec3 applyDeviation(Vec3 displacement, std::size_t x,
                        std::size_t y) const noexcept;
    static std::size_t fieldIndex(float coordinate) noexcept;

    std::vector<PointSourceWind> pointSources_;
    Vec3 globalWind_{};
    Vec3 frameGlobalWind_{};
    std::array<NoiseField, 2> fields_{};
    std::size_t activeField_{};
    float maximumDeviationZ_{};
    float maximumDeviationX_{};
    float interpolation_{};
    float timer_{};
    float interpolationPeriod_{2.0f};
    std::uint32_t randomState_{1u};
    RandomCallback randomSource_;
    std::vector<RegisteredEmitter> windyEmitters_;
};

} // namespace neoshared::model
