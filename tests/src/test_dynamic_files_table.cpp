#include <gtest/gtest.h>
#include <compio.h>
#include <filesystem>
#include <vector>
#include <string>

namespace fs = std::filesystem;

class DynamicFilesTableTest : public ::testing::Test {
protected:
    std::string test_dir = "test_dynamic_ftable";
    std::string db_path = test_dir + "/archive.compio";

    void SetUp() override {
        if (fs::exists(test_dir)) {
            fs::remove_all(test_dir);
        }
        fs::create_directories(test_dir);
    }

    void TearDown() override {
        // Cleanup handled by SetUp
        if (fs::exists(test_dir)) {
            fs::remove_all(test_dir);
        }
    }
};

TEST_F(DynamicFilesTableTest, ResizesAutomatically) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.max_files = 4; // Start very small
    
    // Check path for creation
    fs::path p(db_path);
    if (!fs::exists(p.parent_path())) {
        fs::create_directories(p.parent_path());
    }

    // Create archive
    compio_archive* archive = compio_open_archive(db_path.c_str(), "wb+", &cfg); // wb+ creates file
    ASSERT_NE(archive, nullptr);
    
    // Add 10 files (more than 4)
    for (int i = 0; i < 10; ++i) {
        std::string name = "file_" + std::to_string(i);
        compio_file* f = compio_open_file(name.c_str(), archive);
        ASSERT_NE(f, nullptr);
        
        std::string data = "content_" + std::to_string(i);
        ASSERT_EQ(compio_write(data.data(), data.size(), f), data.size());
        
        compio_close_file(f);
    }
    
    // Flush to force table write and resize
    compio_flush(archive);
    compio_close_archive(archive);
    
    // Reopen and verify
    archive = compio_open_archive(db_path.c_str(), "rb", &cfg);
    ASSERT_NE(archive, nullptr);
    
    for (int i = 0; i < 10; ++i) {
        std::string name = "file_" + std::to_string(i);
        compio_file* f = compio_open_file(name.c_str(), archive);
        ASSERT_NE(f, nullptr) << "Failed to find file " << name;
        
        std::string expected = "content_" + std::to_string(i);
        std::vector<char> buf(expected.size());
        ASSERT_EQ(compio_read(buf.data(), buf.size(), f), buf.size());
        ASSERT_EQ(std::string(buf.begin(), buf.end()), expected);
        
        compio_close_file(f);
    }
    
    compio_close_archive(archive);
}
