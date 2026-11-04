#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neoshared::erf {

std::vector<std::uint8_t> decode_lzma_resource(
    const std::uint8_t* data, std::size_t packed_size, std::size_t size);
std::vector<std::uint8_t> encode_lzma_resource(
    const std::uint8_t* data, std::size_t size);

}
