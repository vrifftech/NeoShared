#pragma once

#include <cstdint>
#include <vector>

namespace neoshared::glu {

// Tightly packed, interleaved unsigned-byte storage, in the caller's row order.
// The native callers supply positive dimensions and one to four components.
struct ByteImage {
    unsigned width{}, height{}, components{};
    std::vector<std::uint8_t> bytes;
};

// Android gluBuild2DMipmaps' power selection, including 3 -> 4.
unsigned nearestPower(unsigned value);

// The specialized unsigned-byte resizer used by Android gluBuild2DMipmaps.
// This is not public gluScaleImage, which converts through unsigned shorts.
ByteImage scaleUbyte(const ByteImage& source, unsigned width, unsigned height);

// Includes the base level and every subsequent level through 1x1.
// maxTextureSize is the caller's GL_MAX_TEXTURE_SIZE result, as in the binary.
std::vector<ByteImage> buildByteMipmaps(const ByteImage& source, unsigned maxTextureSize);

} // namespace neoshared::glu
