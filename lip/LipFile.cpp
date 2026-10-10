#include <neoshared/lip/LipFile.hpp>

#include <cstring>
#include <stdexcept>

namespace neoshared::lip {
namespace {

std::uint32_t readU32LE(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

float readF32LE(const unsigned char* p) {
    std::uint32_t u = readU32LE(p);
    float f;
    static_assert(sizeof(float) == 4, "float must be 32-bit");
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

} // namespace

LipFile readLip(const std::vector<std::uint8_t>& data, const std::string& sourceName) {
    if (data.size() < 16) throw std::runtime_error("LIP file too short: " + sourceName);
    if (std::memcmp(data.data(), "LIP V1.0", 8) != 0) {
        throw std::runtime_error("invalid LIP signature: " + sourceName);
    }

    LipFile lip;
    lip.durationSeconds = readF32LE(data.data() + 8);
    const std::uint32_t recordCount = readU32LE(data.data() + 12);
    if (recordCount > (data.size() - 16U) / 5U) {
        throw std::runtime_error("truncated LIP record table: " + sourceName);
    }

    lip.frames.reserve(recordCount);
    const unsigned char* record = data.data() + 16;
    for (std::uint32_t i = 0; i < recordCount; ++i, record += 5) {
        lip.frames.push_back({readF32LE(record), record[4]});
    }
    return lip;
}

} // namespace neoshared::lip
