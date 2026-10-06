#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <string>

#include "compio.h"
#include "compio/file.hpp"
#include "test_util.hpp"

class ApiLimitsTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override {
        remove(fn);
        remove((std::string(fn) + ".wal").c_str());
    }

    compio_archive *open(const char *mode) {
        compio_config cfg;
        compio_build_default_config(&cfg);
        cfg.max_files = 16;
        return compio_open_archive(fn, mode, &cfg);
    }
};

TEST_F(ApiLimitsTest, LongestNameIsFieldSizeMinusOne) {
    compio_archive *ar = open("w");
    ASSERT_NE(ar, nullptr);

    const std::string longest(COMPIO_FNAME_MAX_SIZE - 1, 'a');
    compio_file *f = compio_open_file(longest.c_str(), ar);
    ASSERT_NE(f, nullptr);
    compio_close_file(f);

    // One character more used to be cut silently and alias the file above.
    const std::string too_long = longest + "b";
    errno = 0;
    EXPECT_EQ(compio_open_file(too_long.c_str(), ar), nullptr);
    EXPECT_EQ(errno, ENAMETOOLONG);

    errno = 0;
    EXPECT_NE(compio_remove_file(ar, too_long.c_str()), 0);
    EXPECT_EQ(errno, ENAMETOOLONG);

    EXPECT_EQ(compio_remove_file(ar, longest.c_str()), 0);
    compio_close_archive(ar);
}

TEST_F(ApiLimitsTest, FilesTableRefusesToGrowPastHardLimit) {
    compio::files_table table(16);
    // Pretend the table is full at the hard limit without allocating 10M slots:
    // add() must refuse before it touches the storage.
    table.max_files = COMPIO_MAX_FILES_LIMIT;
    table.n_files = COMPIO_MAX_FILES_LIMIT;
    EXPECT_EQ(table.add("one_more", true), nullptr);
    EXPECT_EQ(table.max_files, static_cast<uint32_t>(COMPIO_MAX_FILES_LIMIT));
    table.n_files = 0;
}

TEST_F(ApiLimitsTest, InvalidOpenModeIsRejected) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    errno = 0;
    EXPECT_EQ(compio_open_archive(fn, "rw", &cfg), nullptr);
    EXPECT_EQ(errno, EINVAL);
}

TEST_F(ApiLimitsTest, DefragmentWorksInReadPlusMode) {
    {
        compio_archive *ar = open("w");
        ASSERT_NE(ar, nullptr);
        compio_file *f = compio_open_file("data", ar);
        ASSERT_NE(f, nullptr);
        const std::string payload(4096, 'x');
        ASSERT_EQ(compio_write(payload.data(), payload.size(), f), payload.size());
        compio_close_file(f);
        compio_close_archive(ar);
    }

    compio_archive *ar = open("r+");
    ASSERT_NE(ar, nullptr);
    EXPECT_EQ(compio_defragment(ar), COMPIO_SUCCESS);
    compio_close_archive(ar);

    ar = open("r");
    ASSERT_NE(ar, nullptr);
    EXPECT_EQ(compio_defragment(ar), COMPIO_ERROR);
    compio_close_archive(ar);
}

// Every entry point that takes a handle must reject a null one instead of
// dereferencing it.
TEST(ApiNullHandleTest, NullHandlesAreRejected) {
    char buffer[16] = {0};
    compio_fragmentation_stats stats;

    EXPECT_EQ(compio_open_file("name", nullptr), nullptr);
    EXPECT_EQ(compio_write(buffer, sizeof(buffer), nullptr), 0u);
    EXPECT_EQ(compio_read(buffer, sizeof(buffer), nullptr), 0u);
    EXPECT_EQ(compio_insert(buffer, sizeof(buffer), nullptr), 0u);
    EXPECT_EQ(compio_erase(4, nullptr), 0u);

    errno = 0;
    EXPECT_EQ(compio_seek(nullptr, 0, COMPIO_SEEK_SET), -1);
    EXPECT_EQ(errno, EINVAL);
    errno = 0;
    EXPECT_EQ(compio_tell(nullptr), 0u);
    EXPECT_EQ(errno, EINVAL);
    errno = 0;
    EXPECT_EQ(compio_get_size(nullptr), 0u);
    EXPECT_EQ(errno, EINVAL);

    compio_flush(nullptr);
    EXPECT_NE(compio_remove_file(nullptr, "name"), 0);
    EXPECT_NE(compio_close_file(nullptr), 0);
    EXPECT_NE(compio_close_archive(nullptr), 0);
    EXPECT_NE(compio_get_fragmentation_stats(nullptr, &stats), 0);
    EXPECT_NE(compio_defragment(nullptr), 0);
    EXPECT_NE(compio_begin_batch(nullptr), 0);
    EXPECT_NE(compio_end_batch(nullptr), 0);
    EXPECT_EQ(compio_is_auto_batching(nullptr), 0);
    EXPECT_EQ(compio_get_auto_batch_count(nullptr), 0);
}

TEST(ApiNullHandleTest, NullArgumentsAreRejected) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));
    compio_config cfg;
    compio_build_default_config(&cfg);

    EXPECT_EQ(compio_open_archive(nullptr, "w", &cfg), nullptr);
    EXPECT_EQ(compio_open_archive(fn, nullptr, &cfg), nullptr);

    // A null configuration means the defaults.
    compio_archive *ar = compio_open_archive(fn, "w", nullptr);
    ASSERT_NE(ar, nullptr);
    EXPECT_EQ(compio_open_file(nullptr, ar), nullptr);
    EXPECT_NE(compio_remove_file(ar, nullptr), 0);
    EXPECT_NE(compio_get_fragmentation_stats(ar, nullptr), 0);

    compio_file *f = compio_open_file("data", ar);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(compio_write(nullptr, 8, f), 0u);
    EXPECT_EQ(compio_insert(nullptr, 8, f), 0u);
    EXPECT_EQ(compio_read(nullptr, 8, f), 0u);
    EXPECT_EQ(compio_get_size(f), 0u);

    compio_close_file(f);
    compio_close_archive(ar);
    remove(fn);
    remove((std::string(fn) + ".wal").c_str());
}
