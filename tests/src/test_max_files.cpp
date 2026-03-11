#include <gtest/gtest.h>

#include "compio.h"
#include "compio/compio_file.hpp"
#include "compio/file.hpp"

#include "test_util.hpp"

using namespace compio;

class MaxFilesTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }

    void TearDown() override { remove(fn); }
};

TEST_F(MaxFilesTest, DiskSizeReflectsMaxFiles) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.max_files = 128;

    compio_archive *ar = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(ar, nullptr);

    const uint64_t expected =
        4 + 8 + 8 + 8 + 8 + 4 + 4 + 8 +
        static_cast<uint64_t>(128) * (COMPIO_FNAME_MAX_SIZE + 8);
    EXPECT_EQ(ar->header->disk_size(), expected);
    EXPECT_EQ(ar->header->ftable.max_files, 128u);

    compio_close_archive(ar);
}

TEST_F(MaxFilesTest, MaxFilesPersistsAcrossReopen) {
    const uint32_t custom_max = 256;

    {
        compio_config cfg;
        compio_build_default_config(&cfg);
        cfg.max_files = static_cast<int>(custom_max);

        compio_archive *ar = compio_open_archive(fn, "w+", &cfg);
        ASSERT_NE(ar, nullptr);

        compio_file *f = compio_open_file("hello", ar);
        ASSERT_NE(f, nullptr);
        const char data[] = "world";
        compio_write(data, sizeof(data), f);
        compio_close_file(f);

        compio_close_archive(ar);
    }

    {
        compio_config cfg;
        compio_build_default_config(&cfg);

        compio_archive *ar = compio_open_archive(fn, "r", &cfg);
        ASSERT_NE(ar, nullptr);

        EXPECT_EQ(ar->header->ftable.max_files, custom_max)
            << "max_files should be read back from disk unchanged";

        const uint64_t expected =
            4 + 8 + 8 + 8 + 8 + 4 + 4 + 8 +
            static_cast<uint64_t>(custom_max) * (COMPIO_FNAME_MAX_SIZE + 8);
        EXPECT_EQ(ar->header->disk_size(), expected);

        compio_close_archive(ar);
    }
}

TEST_F(MaxFilesTest, DefaultMaxFilesMatchesConstant) {
    compio_config cfg;
    compio_build_default_config(&cfg);

    compio_archive *ar = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(ar, nullptr);

    EXPECT_EQ(ar->header->ftable.max_files, static_cast<uint32_t>(COMPIO_MAX_FILES));

    compio_close_archive(ar);
}
