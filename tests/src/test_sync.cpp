#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include <sys/stat.h>

#include "compio/utils.hpp"

#include "test_util.hpp"

namespace {

class SyncTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override { remove(fn); }
};

TEST_F(SyncTest, SyncFileWritesStdioBufferThrough) {
    FILE *f = fopen(fn, "wb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(fwrite("12345", 1, 5, f), 5u);
    EXPECT_TRUE(compio::sync_file(f));
    struct stat st;
    ASSERT_EQ(stat(fn, &st), 0);
    EXPECT_EQ(st.st_size, 5);
    fclose(f);
}

TEST_F(SyncTest, SyncParentDirectoryOfExistingFile) {
    FILE *f = fopen(fn, "wb");
    ASSERT_NE(f, nullptr);
    fclose(f);
    EXPECT_TRUE(compio::sync_parent_directory(fn));
}

#ifndef _WIN32
TEST_F(SyncTest, SyncParentDirectoryOfMissingDirectoryFails) {
    EXPECT_FALSE(compio::sync_parent_directory("/nonexistent-compio-dir/x"));
}
#endif

}  // namespace
