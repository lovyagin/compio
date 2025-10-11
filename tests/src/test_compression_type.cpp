#include <gtest/gtest.h>

#include "compio.h"

#include "test_util.hpp"

TEST(CompressionTypeTest, ExistingArchive) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));

    // create archive with some specific compression_type
    compio_config config;
    compio_build_default_config(&config);

    compio_build_zlib_compressor(&config.compressor);
    compio_compression_type type = config.compressor.compression_type;

    auto archive = compio_open_archive(fn, "w+", &config);
    ASSERT_NE(archive, nullptr);

    auto file = compio_open_file("A", archive);
    ASSERT_NE(file, nullptr);

    ASSERT_EQ(compio_close_file(file), 0);
    ASSERT_EQ(compio_close_archive(archive), 0);

    // check, that get_compression_type returns same compression_type
    compio_compression_type new_type;
    compio_get_compression_type(fn, &new_type);

    ASSERT_EQ(type, new_type);

    // check, that opening this archive with correct compression type is fine
    archive = compio_open_archive(fn, "r", &config);
    ASSERT_NE(archive, nullptr);
    ASSERT_EQ(compio_close_archive(archive), 0);

    // check, that opening this archive with another compression type will lead to an error
    compio_build_lz4_compressor(&config.compressor);
    ASSERT_EQ(compio_open_archive(fn, "r", &config), nullptr);

    remove(fn);
}
