#include <gtest/gtest.h>
#include "compio.h"
#include "compio/file.hpp" // for header details (optional)
#include "compio/compio_file.hpp"

// We need access to internal archive struct for verification if needed, 
// but integration test via public API is sufficient for validation check.

class HeaderValidationTest : public ::testing::Test {
protected:
    std::string filename = "test_header_val.compio";

    void SetUp() override {
        remove(filename.c_str());
    }

    void TearDown() override {
        // cleanup
        remove(filename.c_str());
    }
};

TEST_F(HeaderValidationTest, ValidateMaxFilesMismatch) {
    compio_config config;
    compio_build_default_config(&config);
    config.max_files = 10;
    config.block_size = 4096;

    // Create archive
    auto* archive = compio_open_archive(filename.c_str(), "w", &config);
    ASSERT_NE(archive, nullptr);
    compio_close_archive(archive);

    // Try open with different max_files
    config.max_files = 64;
    archive = compio_open_archive(filename.c_str(), "r", &config);
    
    // Expect failure due to mismatch
    EXPECT_EQ(archive, nullptr);
    if (archive) compio_close_archive(archive);
}

TEST_F(HeaderValidationTest, ValidateBlockSizeMismatch) {
    compio_config config;
    compio_build_default_config(&config);
    config.max_files = 10;
    config.block_size = 4096;

    // Create archive
    auto* archive = compio_open_archive(filename.c_str(), "w", &config);
    ASSERT_NE(archive, nullptr);
    compio_close_archive(archive);

    // Try open with different block_size
    config.block_size = 1024;
    archive = compio_open_archive(filename.c_str(), "r", &config);
    
    // Expect failure due to mismatch
    EXPECT_EQ(archive, nullptr);
    if (archive) compio_close_archive(archive);
}

TEST_F(HeaderValidationTest, ValidateBTreeDegreeMismatch) {
    compio_config config;
    compio_build_default_config(&config);
    config.b_tree_degree = 16;
    
    // Create archive
    auto* archive = compio_open_archive(filename.c_str(), "w", &config);
    ASSERT_NE(archive, nullptr);
    compio_close_archive(archive);

    // Try open with different degree
    config.b_tree_degree = 8;
    archive = compio_open_archive(filename.c_str(), "r", &config);
    
    // Expect failure due to mismatch
    EXPECT_EQ(archive, nullptr);
    if (archive) compio_close_archive(archive);
}
