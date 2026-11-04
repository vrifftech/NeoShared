#include <neoshared/erf/LzmaResource.hpp>

#include <lzma.h>
#include <array>
#include <stdexcept>

namespace neoshared::erf {

std::vector<std::uint8_t> decode_lzma_resource(
    const std::uint8_t* data, std::size_t packed_size, std::size_t size) {
    lzma_filter filters[] = {{LZMA_FILTER_LZMA1, nullptr}, {LZMA_VLI_UNKNOWN, nullptr}};
    const auto properties_size = packed_size < 5 ? packed_size : 5;
    if (lzma_properties_decode(&filters[0], nullptr, data, properties_size) != LZMA_OK)
        throw std::runtime_error("Invalid resource LZMA properties");
    auto& options = *static_cast<lzma_options_lzma*>(filters[0].options);
    options.ext_flags = LZMA_LZMA1EXT_ALLOW_EOPM;
    lzma_set_ext_size(options, size);
    filters[0].id = LZMA_FILTER_LZMA1EXT;
    std::vector<std::uint8_t> output(size);
    std::size_t input_pos = 5, output_pos = 0;
    const auto result = lzma_raw_buffer_decode(filters, nullptr, data, &input_pos,
                                              packed_size, output.data(), &output_pos, size);
    lzma_filters_free(filters, nullptr);
    if (result != LZMA_OK)
        throw std::runtime_error("Unable to decompress resource LZMA payload");
    return output;
}

std::vector<std::uint8_t> encode_lzma_resource(const std::uint8_t* data, std::size_t size) {
    lzma_options_lzma options{};
    lzma_lzma_preset(&options, LZMA_PRESET_DEFAULT);
    options.ext_flags = 0;
    lzma_filter filters[] = {{LZMA_FILTER_LZMA1EXT, &options}, {LZMA_VLI_UNKNOWN, nullptr}};
    std::vector<std::uint8_t> output(5);
    if (lzma_properties_encode(&filters[0], output.data()) != LZMA_OK)
        throw std::runtime_error("Unable to encode resource LZMA properties");
    lzma_stream stream = LZMA_STREAM_INIT;
    if (lzma_raw_encoder(&stream, filters) != LZMA_OK)
        throw std::runtime_error("Unable to initialize resource LZMA encoder");
    stream.next_in = data;
    stream.avail_in = size;
    std::array<std::uint8_t, 65536> chunk;
    lzma_ret result;
    do {
        stream.next_out = chunk.data();
        stream.avail_out = chunk.size();
        result = lzma_code(&stream, LZMA_FINISH);
        output.insert(output.end(), chunk.data(), chunk.data() + chunk.size() - stream.avail_out);
    } while (result == LZMA_OK);
    lzma_end(&stream);
    if (result != LZMA_STREAM_END)
        throw std::runtime_error("Unable to compress resource LZMA payload");
    return output;
}

}
