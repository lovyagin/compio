#include "gtest/gtest.h"
#include "compio.h"
#include <vector>
#include <string>
#include <cstdio>
#include <fstream>
#include <random>

// Include test utility helpers
namespace {
    std::string generate_tmp_fn() {
        std::string s = "tmp_";
        s += std::to_string(std::rand());
        return s;
    }
}

class ChecksumTest : public ::testing::Test {
protected:
    std::string filename;
    compio_archive* archive = nullptr;

    void SetUp() override {
        filename = generate_tmp_fn();
    }

    void TearDown() override {
        if (archive) {
            compio_close_archive(archive);
            archive = nullptr;
        }
        std::remove(filename.c_str());
    }
};

TEST_F(ChecksumTest, MixedChecksums) {
    compio_config config;
    
    // 1. Create archive with FNV1a explicitly
    compio_build_default_config(&config);
    config.checksum_type = COMPIO_CHECKSUM_FNV1A;
    config.wal_max_size_bytes = 0; // Disable WAL for simplicity if needed, but default is fine
    
    archive = compio_open_archive(filename.c_str(), "w", &config); // "w" creates new
    ASSERT_NE(archive, nullptr);
    
    compio_file* f1 = compio_open_file("fnv1a_file", archive);
    ASSERT_NE(f1, nullptr);
    std::string data1 = "This is FNV1a data";
    ASSERT_EQ(compio_write(data1.data(), data1.size(), f1), data1.size());
    compio_close_file(f1);
    
    compio_close_archive(archive);
    archive = nullptr;
    
    // 2. Open with default config (should be CRC32C now)
    compio_build_default_config(&config);
    ASSERT_EQ(config.checksum_type, COMPIO_CHECKSUM_CRC32C); 
    
    archive = compio_open_archive(filename.c_str(), "r+", &config); // "r+" opens existing
    ASSERT_NE(archive, nullptr);
    
    // 3. Read FNV1a data (should work via backward compatibility)
    f1 = compio_open_file("fnv1a_file", archive);
    ASSERT_NE(f1, nullptr);
    std::vector<char> buf1(data1.size());
    ASSERT_EQ(compio_read(buf1.data(), buf1.size(), f1), buf1.size());
    // Null terminate for comparison if using string constructor? No, vector constructor handles size.
    std::string read_s1(buf1.begin(), buf1.end());
    EXPECT_EQ(read_s1, data1);
    compio_close_file(f1);
    
    // 4. Write new file (should use CRC32C)
    compio_file* f2 = compio_open_file("crc32c_file", archive);
    ASSERT_NE(f2, nullptr);
    std::string data2 = "This is CRC32C data";
    ASSERT_EQ(compio_write(data2.data(), data2.size(), f2), data2.size());
    compio_close_file(f2);
    
    compio_close_archive(archive);
    archive = nullptr;
    
    // 5. Open again and read both
    archive = compio_open_archive(filename.c_str(), "r", &config);
    ASSERT_NE(archive, nullptr);
    
    f1 = compio_open_file("fnv1a_file", archive);
    ASSERT_NE(f1, nullptr);
    ASSERT_EQ(compio_read(buf1.data(), buf1.size(), f1), buf1.size());
    read_s1 = std::string(buf1.begin(), buf1.end());
    EXPECT_EQ(read_s1, data1);
    compio_close_file(f1);
    
    f2 = compio_open_file("crc32c_file", archive);
    ASSERT_NE(f2, nullptr);
    std::vector<char> buf2(data2.size());
    ASSERT_EQ(compio_read(buf2.data(), buf2.size(), f2), buf2.size());
    std::string read_s2(buf2.begin(), buf2.end());
    EXPECT_EQ(read_s2, data2);
    compio_close_file(f2);
}
