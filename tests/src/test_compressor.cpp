#include <gtest/gtest.h>
#include <tuple>
#include <vector>

#include "compio.h"

#include "sample_data.hpp"

class CompressorTest
    : public ::testing::TestWithParam<std::tuple<compio_compression_type, uint64_t, uint64_t>> {};

TEST_P(CompressorTest, RoundTripTest) {
    auto [compression_type, src_size, zeroes_percentage] = GetParam();
    compio_compressor compressor;
    compio_build_compressor_by_type(&compressor, compression_type);

    std::vector<char> dec_data(src_size, 0);
    std::copy_n(std::begin(html_data), src_size / zeroes_percentage, dec_data.begin());

    uint64_t buffer_size = compressor.get_bufsize(src_size);
    std::vector<char> c_buffer(buffer_size);

    int ret = compressor.compress(c_buffer.data(), &buffer_size, dec_data.data(), src_size);
    ASSERT_EQ(ret, 0);

    std::vector<char> dec_buffer(src_size);
    ret = compressor.decompress(dec_buffer.data(), &src_size, c_buffer.data(), buffer_size);
    ASSERT_EQ(ret, 0);

    for (std::size_t i = 0; i < src_size; ++i) {
        ASSERT_EQ(dec_data[i], dec_buffer[i]);
    }
}

INSTANTIATE_TEST_CASE_P(
    CompressorTests, CompressorTest,
    ::testing::Combine(::testing::Values(compio_compression_type::COMPIO_COMPRESS_ZLIB,
                                         compio_compression_type::COMPIO_COMPRESS_LZ4,
                                         compio_compression_type::COMPIO_COMPRESS_ZSTD,
                                         compio_compression_type::COMPIO_COMPRESS_BROTLI),
                       ::testing::Values(128, 4096, sizeof(html_data)),
                       ::testing::Values(1, 2, 100, 10000000)));
