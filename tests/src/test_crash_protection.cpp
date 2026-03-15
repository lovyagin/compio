#include <gtest/gtest.h>
#include <fstream>
#include <vector>
#include <iostream>
#include "compio.h"
#include "compio/file.hpp"

class CrashProtectionTest : public ::testing::Test {
protected:
    std::string filename = "test_crash_edge.compio";

    void SetUp() override {
        remove(filename.c_str());
    }

    void TearDown() override {
        remove(filename.c_str());
    }

    // Helper to read entire file into vector
    std::vector<char> read_file(const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return {};
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<char> buffer(size);
        if (file.read(buffer.data(), size))
            return buffer;
        return {};
    }

    // Helper to write bytes to file at offset
    void write_bytes(const std::string& path, size_t offset, const std::vector<char>& data) {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!file.is_open()) return;
        file.seekp(offset, std::ios::beg);
        file.write(data.data(), data.size());
    }
};

TEST_F(CrashProtectionTest, FailFastOnDoubleCorruption) {
    compio_config config;
    compio_build_default_config(&config);
    config.max_files = 10;
    
    // 1. Create valid archive
    auto* archive = compio_open_archive(filename.c_str(), "w", &config);
    ASSERT_NE(archive, nullptr);
    compio_close_archive(archive);

    // Calculate disk_size for max_files=10
    // We can instantiate a header to get disk_size.
    compio::header h(10);
    uint64_t disk_size = h.disk_size();

    // 2. Corrupt Slot A (Offset 0)
    std::vector<char> garbage(10, '\xFF');
    write_bytes(filename, 0, garbage); // Corrupt magic number

    // 3. Corrupt Slot B (Offset disk_size)
    write_bytes(filename, disk_size, garbage); // Corrupt magic number of second header

    // 4. Try to open
    archive = compio_open_archive(filename.c_str(), "r", &config);
    EXPECT_EQ(archive, nullptr) << "Should return nullptr when both headers are corrupt";
    
    if (archive) compio_close_archive(archive);
}

TEST_F(CrashProtectionTest, ReadOnlyDoesNotModifyFile) {
    compio_config config;
    compio_build_default_config(&config);
    
    // 1. Create valid archive
    auto* archive = compio_open_archive(filename.c_str(), "w", &config);
    ASSERT_NE(archive, nullptr);
    compio_close_archive(archive);

    // 2. Snapshot file content
    std::vector<char> original_content = read_file(filename);
    ASSERT_FALSE(original_content.empty());

    // 3. Open in Read-Only mode
    archive = compio_open_archive(filename.c_str(), "r", &config);
    ASSERT_NE(archive, nullptr);
    
    // 4. Close immediately
    compio_close_archive(archive);

    // 5. Compare content
    std::vector<char> new_content = read_file(filename);
    EXPECT_EQ(original_content, new_content) << "Read-only open/close should not modify file content";
}
