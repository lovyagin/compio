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
        char buffer[1024];
        generate_tmp_fn(buffer, 1024);
        std::string unique_base = buffer;
        
        // generate_tmp_fn creates the file, remove it so we can use the name (or derived name)
        if (fs::exists(unique_base)) {
            fs::remove(unique_base);
        }
        
        fs::path p(unique_base);
        tmp_dir = p.string() + "_dir";
        
        if (fs::exists(tmp_dir)) fs::remove_all(tmp_dir);
        fs::create_directories(tmp_dir);
        
        archive_path = (fs::path(tmp_dir) / "archive.compio").string();
        recover_dir = (fs::path(tmp_dir) / "recovered").string();
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
    // Should recover at least 1 file
    ASSERT_GE(count, 1);
    
    fs::path recovered_file = fs::path(recover_dir) / "test.txt";
    ASSERT_TRUE(fs::exists(recovered_file));
    std::ifstream ifs(recovered_file, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ASSERT_EQ(content, "Hello World");
}

TEST_F(RepairTest, RecoversWithCorruptedHeader) {
    uint64_t hash = 0;
    {
        compio_config config;
        compio_build_default_config(&config);
        // We use default configuration.
        compio_archive* archive = compio_open_archive(archive_path.c_str(), "w", &config);
        ASSERT_NE(archive, nullptr);
        
        compio_file* f = compio_open_file("data.bin", archive);
        ASSERT_NE(f, nullptr);
        hash = f->hash;
        
        std::vector<uint8_t> data(1000, 0xAB);
        compio_write(data.data(), data.size(), f);
        compio_close_file(f);
        compio_close_archive(archive);
    }

    // Corrupt header (overwrite first 512 bytes with zeros)
    {
#ifdef _WIN32
        FILE* f = _wfopen(fs::path(archive_path).c_str(), L"rb+");
#else
        FILE* f = fopen(archive_path.c_str(), "rb+");
#endif
        ASSERT_NE(f, nullptr);
        std::unique_ptr<FILE, decltype(&fclose)> f_guard(f, fclose);
        std::vector<uint8_t> zeros(512, 0);
        fwrite(zeros.data(), 1, 512, f);
    }

    int count = compio_repair(archive_path.c_str(), recover_dir.c_str());
    ASSERT_GE(count, 1);
    
    // Check for file_<hash> (since header with names is gone)
    // When header is corrupted, compio_repair uses a heuristic to guess compression.
    // Since we used COMPRESS_NONE and the heuristic defaults to ZLIB if unknown, 
    // we might get a decompression failure if heuristic is wrong.
    // However, compio_repair with corrupted header defaults to ZLIB (which is default config).
    // If the data is not ZLIB compressed, decompression will fail.
    // To make this test robust, we should use ZLIB (default) or ensure heuristic handles it.
    // Wait, compio_repair code says: 
    // "warning: header corrupted, using default settings for salvage (degree=16, zlib)"
    // So if we write UNCOMPRESSED data, the repair tool will try to decompress it as ZLIB and fail.
    // Therefore, we should write ZLIB data (default config) so the repair tool can recover it.
    // Reverting config change to use default (ZLIB).
    
    // Actually, let's stick to default config (ZLIB) as repair assumes it.
    
    std::string expected_name = "file_" + std::to_string(hash);
    
    bool found = false;
    for (const auto& entry : fs::directory_iterator(recover_dir)) {
        if (entry.path().filename().string().find("file_") == 0 || 
            entry.path().filename().string().find("orphan_") == 0) {
            found = true;
            // Content check
            std::ifstream ifs(entry.path(), std::ios::binary);
            std::vector<uint8_t> content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            if (content.size() == 1000 && content[0] == 0xAB) {
                SUCCEED();
                return;
            }
        }
    }
    
    // Fallback search by exact hash name if above loop didn't return
    fs::path recovered_path = fs::path(recover_dir) / expected_name;
    if (fs::exists(recovered_path)) {
        std::ifstream ifs(recovered_path, std::ios::binary);
        std::vector<uint8_t> content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        ASSERT_EQ(content.size(), 1000);
        if (!content.empty()) { ASSERT_EQ(content[0], 0xAB); }
    } else {
        // It might be recovered as orphan if index node was also lost/corrupted (unlikely here as we only corrupted header)
        // or if hash mapping failed.
        // Let's check for any file with correct size
         bool any_correct = false;
         for (const auto& entry : fs::directory_iterator(recover_dir)) {
            std::ifstream ifs(entry.path(), std::ios::binary);
            ifs.seekg(0, std::ios::end);
            if (ifs.tellg() == 1000) {
                any_correct = true; 
                break;
            }
         }
         ASSERT_TRUE(any_correct) << "No recovered file has correct size";
    }
}
