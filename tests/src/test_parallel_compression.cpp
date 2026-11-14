#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "compio.h"
#include "test_util.hpp"

namespace {

std::vector<uint8_t> mixed(size_t size, unsigned seed) {
    std::vector<uint8_t> data(size);
    uint32_t x = seed + 1;
    for (size_t i = 0; i < size; ++i) {
        x = x * 1103515245u + 12345u;
        // Half of every 8 KiB is noise, so that some blocks are stored as is.
        data[i] = (i / 8192) % 2 ? static_cast<uint8_t>(x >> 16) : static_cast<uint8_t>(i / 97);
    }
    return data;
}

class ParallelCompressionTest : public ::testing::TestWithParam<compio_compression_type> {
protected:
    char fn[256];
    compio_config config;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));
        compio_build_default_config(&config);
        config.block_size = 4096;
        config.block_size__minimum = 1024;
        config.block_size__maximum = 8192;
        config.cache_size__blocks = 32;
        ASSERT_EQ(compio_build_compressor_by_type(&config.compressor, GetParam()), 0);
    }
};

// Blocks packed on several threads read back the same with one thread.
TEST_P(ParallelCompressionTest, ParallelWritesReadSequentially) {
    const auto data = mixed(3 << 20, 1);
    const auto patch = mixed(300000, 2);
    config.compression_threads = 4;
    compio_archive *archive = compio_open_archive(fn, "w+", &config);
    ASSERT_NE(archive, nullptr);
    compio_file *file = compio_open_file("f", archive);
    ASSERT_NE(file, nullptr);
    for (size_t pos = 0; pos < data.size(); pos += 100000) {
        const size_t n = std::min<size_t>(100000, data.size() - pos);
        ASSERT_EQ(compio_write(data.data() + pos, n, file), n);
    }
    ASSERT_EQ(compio_seek(file, 777777, COMPIO_SEEK_SET), 0);
    ASSERT_EQ(compio_write(patch.data(), patch.size(), file), patch.size());
    compio_close_file(file);
    ASSERT_EQ(compio_close_archive(archive), 0);

    auto expected = data;
    std::copy(patch.begin(), patch.end(), expected.begin() + 777777);

    config.compression_threads = 1;
    archive = compio_open_archive(fn, "r", &config);
    ASSERT_NE(archive, nullptr);
    file = compio_open_file("f", archive);
    ASSERT_NE(file, nullptr);
    std::vector<uint8_t> back(expected.size());
    ASSERT_EQ(compio_read(back.data(), back.size(), file), back.size());
    EXPECT_TRUE(back == expected);
    compio_close_file(file);
    compio_close_archive(archive);
}

INSTANTIATE_TEST_SUITE_P(Compressors, ParallelCompressionTest,
                         ::testing::Values(COMPIO_COMPRESS_LZ4, COMPIO_COMPRESS_ZSTD, COMPIO_COMPRESS_ZLIB,
                                           COMPIO_COMPRESS_BROTLI),
                         [](const auto &info) { return std::to_string(static_cast<int>(info.param)); });

TEST(ParallelCompressionConfig, NegativeThreadCountIsRejected) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));
    compio_config config;
    compio_build_default_config(&config);
    config.compression_threads = -1;
    EXPECT_EQ(compio_open_archive(fn, "w+", &config), nullptr);
}

} // namespace
