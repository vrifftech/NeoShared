#pragma once

#include <cstdint>

namespace neoshared::texture::detail {
void decodeBc4(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int pixelSize, bool isSigned);
void decodeBc7(const void* compressedBlock, void* decompressedBlock, int destinationPitch);
void decodePvrtc4(const std::uint8_t* data, std::uint32_t width, std::uint32_t height, std::uint8_t* rgba);
} // namespace neoshared::texture::detail
