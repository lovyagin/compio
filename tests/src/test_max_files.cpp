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

TEST_F(MaxFilesTest, HeaderSizeDoesNotDependOnMaxFiles) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.max_files = 128;

    compio_archive *ar = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(ar, nullptr);
    EXPECT_EQ(ar->header->disk_size(), 4096u);
    EXPECT_GE(ar->header->ftable.files.capacity(), 128u);
    compio_close_archive(ar);
}

TEST_F(MaxFilesTest, ReopenIgnoresMaxFilesOfCreation) {
    {
        compio_config cfg;
        compio_build_default_config(&cfg);
        cfg.max_files = 2;
        compio_archive *ar = compio_open_archive(fn, "w+", &cfg);
        ASSERT_NE(ar, nullptr);
        // The hint is not a limit: the table grows past it.
        for (const char *name : {"a", "b", "c", "d"}) {
            compio_file *f = compio_open_file(name, ar);
            ASSERT_NE(f, nullptr) << name;
            compio_close_file(f);
        }
        compio_close_archive(ar);
    }
    for (int max_files : {0, 1, 4096}) {
        compio_config cfg;
        compio_build_default_config(&cfg);
        cfg.max_files = max_files;
        compio_archive *ar = compio_open_archive(fn, "r", &cfg);
        ASSERT_NE(ar, nullptr) << max_files;
        EXPECT_EQ(ar->header->ftable.files.size(), 4u);
        compio_close_archive(ar);
    }
}

TEST_F(MaxFilesTest, CreateWithZeroReservesDefault) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.max_files = 0;

    compio_archive *ar = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(ar, nullptr) << "Should succeed with max_files=0 (default)";
    EXPECT_GE(ar->header->ftable.files.capacity(), static_cast<size_t>(COMPIO_MAX_FILES));
    compio_close_archive(ar);
}
