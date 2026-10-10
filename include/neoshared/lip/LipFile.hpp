#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace neoshared::lip {

struct LipKeyframe {
    float timeSeconds = 0.0f;
    std::uint8_t shape = 0;
};

struct LipFile {
    float durationSeconds = 0.0f;
    std::vector<LipKeyframe> frames;
};

// Decode the counted LIP V1.0 records from an owned resource snapshot.
// Duration, key order and viseme interpretation belong to the consumer.
LipFile readLip(const std::vector<std::uint8_t>& data, const std::string& sourceName);

} // namespace neoshared::lip
