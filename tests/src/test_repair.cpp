#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cinttypes>

#include "compio.h"
#include "compio/compio_file.hpp"
#include "compio/utils.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;

class RepairTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir = "repair_test_dir";
        if (fs::exists(tmp_dir)) fs::remove_all(tmp_dir);
        fs::create_directories(tmp_dir);
        archive_path = tmp_dir + "/archive.compio";
        recover_dir = tmp_dir + "/recovered";
    }

    void TearDown() override {
        if (fs::exists(tmp_dir)) fs::remove_all(tmp_dir);
    }

    std::string tmp_dir;
    std::string archive_path;
    std::string recover_dir;
};

TEST_F(RepairTest, RecoversFromValidArchive) {
    {
        compio_config config;
        compio_build_default_config(&config);
        compio_archive* archive = compio_open_archive(archive_path.c_str(), "w", &config);
        ASSERT_NE(archive, nullptr);

        compio_file* f = compio_open_file("test.txt", archive);
        ASSERT_NE(f, nullptr);
        std::string data = "Hello World";
        compio_write(data.c_str(), data.size(), f);
        compio_close_file(f);
        compio_close_archive(archive);
    }

    int count = compio_repair(archive_path.c_str(), recover_dir.c_str());
    // Should recover at least 1 file (test.txt)
    ASSERT_GE(count, 1);
    
    fs::path recovered_file = fs::path(recover_dir) / "test.txt";
    ASSERT_TRUE(fs::exists(recovered_file));
    std::ifstream ifs(recovered_file, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ASSERT_EQ(content, "Hello World");
}

TEST_F(RepairTest, RecoversWithCorruptedHeader) {
    uint64_t hash = compio::fnv1a("data.bin");
    {
        compio_config config;
        compio_build_default_config(&config);
        compio_archive* archive = compio_open_archive(archive_path.c_str(), "w", &config);
        ASSERT_NE(archive, nullptr);
        
        compio_file* f = compio_open_file("data.bin", archive);
        ASSERT_NE(f, nullptr);
        // hash = f->hash; // Or use fnv1a("data.bin") manually if f is opaque
        std::vector<uint8_t> data(1000, 0xAB);
        compio_write(data.data(), data.size(), f);
        compio_close_file(f);
        compio_close_archive(archive);
    }

    // Corrupt header (first 512 bytes)
    {
        FILE* f = fopen(archive_path.c_str(), "rb+");
        if (f) {
            std::vector<uint8_t> zeros(512, 0);
            fwrite(zeros.data(), 1, 512, f);
            fclose(f);
        }
    }

    int count = compio_repair(archive_path.c_str(), recover_dir.c_str());
    ASSERT_GE(count, 1);
    
    // Check for file_<hash>
    // Since header is lost, we don't know name "data.bin".
    // We expect "file_" + hash.
    std::string expected_name = "file_" + std::to_string(hash);
    fs::path recovered_path = fs::path(recover_dir) / expected_name;
    ASSERT_TRUE(fs::exists(recovered_path)) << "Expected file: " << expected_name;
    
    std::ifstream ifs(recovered_path, std::ios::binary);
    std::vector<uint8_t> content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ASSERT_EQ(content.size(), 1000);
    if (!content.empty()) ASSERT_EQ(content[0], 0xAB);
}
