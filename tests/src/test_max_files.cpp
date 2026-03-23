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

    void TearDown() override { remove(fn); remove((std::string(fn) + ".wal").c_str()); }
};

TEST_F(MaxFilesTest, DiskSizeReflectsMaxFiles) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.max_files = 128;

    compio_archive *ar = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(ar, nullptr);

    bool is_v5 = (ar->header->magic_number == COMPIO_MAGIC_NUMBER);
    if (is_v5) {
        // v5: header size is fixed (files table is external).
        // Check that capacity is set correctly.
        EXPECT_EQ(ar->header->files_table_capacity, 128u);
        
        // Header size is constant for v5 (calculated from fields)
        // 4 (magic) + 32 (checksum) + 8 (seq) + 8 (root) + 8 (size) + 
        // 8 (alloc_off) + 8 (alloc_sz) + 4 (comp) + 4 (block) + 4 (degree) +
        // 8 (ft_addr) + 4 (ft_cap) + 8 (n_files) = 108
        const uint64_t v5_header_size = 4 + 32 + 8 + 8 + 8 + 8 + 8 + 4 + 4 + 4 + 8 + 4 + 8;
        EXPECT_EQ(ar->header->disk_size(), v5_header_size);
    } else {
        const uint64_t expected =
            4 + 32 + 8 + 8 + 8 + 8 + 8 + 4 + 4 + 4 + 4 + 8 +
            static_cast<uint64_t>(128) * (COMPIO_FNAME_MAX_SIZE + 8);
        EXPECT_EQ(ar->header->disk_size(), expected);
        EXPECT_EQ(ar->header->ftable.max_files, 128u);
    }

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
        cfg.max_files = 0; // Use smart open to detect max_files

        compio_archive *ar = compio_open_archive(fn, "r", &cfg);
        ASSERT_NE(ar, nullptr);

        bool is_v5 = (ar->header->magic_number == COMPIO_MAGIC_NUMBER);
        if (is_v5) {
             EXPECT_EQ(ar->header->files_table_capacity, custom_max);
             const uint64_t v5_header_size = 4 + 32 + 8 + 8 + 8 + 8 + 8 + 4 + 4 + 4 + 8 + 4 + 8;
             EXPECT_EQ(ar->header->disk_size(), v5_header_size);
        } else {
             EXPECT_EQ(ar->header->ftable.max_files, custom_max)
                 << "max_files should be read back from disk unchanged";

             const uint64_t expected =
                 4 + 32 + 8 + 8 + 8 + 8 + 8 + 4 + 4 + 4 + 4 + 8 +
                 static_cast<uint64_t>(custom_max) * (COMPIO_FNAME_MAX_SIZE + 8);
             EXPECT_EQ(ar->header->disk_size(), expected);
        }

        compio_close_archive(ar);
    }
}

TEST_F(MaxFilesTest, CreateWithZeroUsesDefaultLimit) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.max_files = 0; // Set to 0 to instruct library to use default limit

    compio_archive *ar = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(ar, nullptr) << "Should succeed with max_files=0 (default)";

    // Should default to COMPIO_MAX_FILES (which is 4096 now)
    EXPECT_EQ(ar->header->ftable.max_files, static_cast<uint32_t>(COMPIO_MAX_FILES));

    compio_close_archive(ar);
}

TEST_F(MaxFilesTest, DefaultMaxFilesMatchesConstant) {
    compio_config cfg;
    compio_build_default_config(&cfg);

    compio_archive *ar = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(ar, nullptr);

    EXPECT_EQ(ar->header->ftable.max_files, static_cast<uint32_t>(COMPIO_MAX_FILES));

    compio_close_archive(ar);
}
