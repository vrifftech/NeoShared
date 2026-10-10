#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace neoshared::audio {
// Interleaved decoded PCM. Sample rate and channel count are source properties;
// dialogue timing and any analysis downmix remain with the consuming tool.
struct PcmData {
    unsigned sampleRate = 0, channels = 0;
    std::vector<float> samples;
    std::size_t frameCount() const noexcept { return channels ? samples.size()/channels : 0; }
    double durationSeconds() const noexcept {
        return sampleRate ? double(frameCount())/sampleRate : 0.0;
    }
};
using Checkpoint = std::function<void()>;
PcmData decodeCompressed(const unsigned char* bytes, std::size_t size,
                         const Checkpoint& checkpoint = {});
std::shared_ptr<const PcmData> decodeBytes(const std::vector<std::uint8_t>& bytes,
                                         const Checkpoint& checkpoint = {});
std::shared_ptr<const PcmData> decodeFile(const std::filesystem::path& path,
                                        const Checkpoint& checkpoint = {});
} // namespace neoshared::audio
