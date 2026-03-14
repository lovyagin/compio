#include <gtest/gtest.h>
#include "compio.h"
#include <string>
#include <vector>
#include <cstdio>
#include "compio/compio_file.hpp"

// We need to define Test class
class ScalabilityTest : public ::testing::Test {
protected:
    std::string filename = "scalability_test_large.compio";

    void SetUp() override {
        remove(filename.c_str());
    }

    void TearDown() override {
        remove(filename.c_str());
    }
};

TEST_F(ScalabilityTest, HandleLargeFileCount) {
    const int NUM_FILES = 500; // Demonstrating significantly more than 64. 500 is enough to prove scalability without taking too long.
    
    compio_config config;
    compio_build_default_config(&config);
    config.max_files = NUM_FILES + 10; 
    config.block_size = 4096;
    config.b_tree_degree = 8;
    
    // Create archive
    auto* archive = compio_open_archive(filename.c_str(), "w", &config);
    ASSERT_NE(archive, nullptr) << "Failed to create archive with max_files=" << config.max_files;

    std::vector<std::string> filenames;
    for (int i = 0; i < NUM_FILES; ++i) {
        std::string name = "file_" + std::to_string(i) + ".txt";
        
        auto* file = compio_open_file(name.c_str(), archive);
        ASSERT_NE(file, nullptr) << "Failed to open inner file " << name;
        
        std::string content = "Content of " + name;
        ssize_t written = compio_write(content.c_str(), content.size(), file);
        EXPECT_EQ(written, content.size());
        
        compio_close_file(file);
        filenames.push_back(name);
    }
    compio_close_archive(archive);

    // Reopen using Smart Open (auto-detect max_files=0)
    config.max_files = 0; 
    archive = compio_open_archive(filename.c_str(), "r", &config);
    ASSERT_NE(archive, nullptr) << "Failed to reopen archive with Smart Open";
    
    // Verify auto-detected value
    EXPECT_EQ(archive->config.max_files, NUM_FILES + 10);

    // Verify content of a few files to be sure
    // Check first, middle, last
    std::vector<int> check_indices = {0, NUM_FILES/2, NUM_FILES-1};
    
    for (int idx : check_indices) {
        std::string name = filenames[idx];
        auto* file = compio_open_file(name.c_str(), archive);
        ASSERT_NE(file, nullptr) << "Failed to open inner file " << name;
        
        char buffer[100];
        ssize_t bytes_read = compio_read(buffer, sizeof(buffer), file);
        if (bytes_read >= 0) {
            buffer[bytes_read] = '\0';
            std::string expected = "Content of " + name;
            EXPECT_EQ(std::string(buffer), expected);
        } else {
            ADD_FAILURE() << "Read failed for " << name;
        }
        
        compio_close_file(file);
    }
    
    compio_close_archive(archive);
}