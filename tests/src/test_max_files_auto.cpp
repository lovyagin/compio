#include <gtest/gtest.h>
#include "compio.h"
#include "compio/compio_file.hpp"
#include "test_util.hpp"

class MaxFilesAutoTest : public ::testing::Test {
protected:
    char fn[256];
    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override { remove(fn); remove((std::string(fn) + ".wal").c_str()); }
};

TEST_F(MaxFilesAutoTest, CreateWithZeroDefaultsToConstant) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.max_files = 0; // Explicitly zero

    compio_archive *ar = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(ar, nullptr) << "Should succeed with max_files=0 (auto)";
    
    // Should default to COMPIO_MAX_FILES
    EXPECT_EQ(ar->header->ftable.max_files, static_cast<uint32_t>(COMPIO_MAX_FILES));
    
    compio_close_archive(ar);
}
