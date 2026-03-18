#include <gtest/gtest.h>
#include <filesystem>
#include <vector>
#include <cstring>
#include <cstdlib>
#include "compio.h"
#include "test_util.hpp"

namespace fs = std::filesystem;

class AutoCheckpointTest : public ::testing::Test {
protected:
    char archive_path[256];
    std::string wal_path;

    void SetUp() override {
        generate_tmp_fn(archive_path, sizeof(archive_path));
        wal_path = std::string(archive_path) + ".wal";
    }

    void TearDown() override {
        if (fs::exists(archive_path)) fs::remove(archive_path);
        if (fs::exists(wal_path)) fs::remove(wal_path);
    }
    
    uint64_t get_wal_size() {
        if (fs::exists(wal_path)) {
            return fs::file_size(wal_path);
        }
        return 0;
    }
};

TEST_F(AutoCheckpointTest, CheckpointOnSizeLimit) {
    compio_config config;
    compio_build_default_config(&config);
    
    // Set small limit: 1KB
    config.wal_max_size_bytes = 1024;
    config.block_size = 512;
    // Set cache size to 1 to force eviction (Write-Back -> Write-Through-ish)
    // This ensures data hits WAL during write, not just at flush.
    config.cache_size__blocks = 1;

    compio_archive* archive = compio_open_archive(archive_path, "w+", &config);
    ASSERT_NE(archive, nullptr);
    
    compio_file* file = compio_open_file("data", archive);
    ASSERT_NE(file, nullptr);

    // Use random data to ensure it doesn't compress much
    std::vector<uint8_t> data(2000);
    for(size_t i=0; i<data.size(); ++i) data[i] = static_cast<uint8_t>(rand() % 256);
    
    // Write 2000 bytes. ~4 blocks.
    // With cache=1, 3 blocks evicted.
    // 3 * 512 = 1536 bytes.
    // WAL size > 1536 > 1024.
    // So checkpoint SHOULD trigger.
    
    uint64_t written = compio_write(data.data(), data.size(), file);
    ASSERT_EQ(written, data.size());
    
    // Check WAL size. Should be 0.
    uint64_t wal_size = get_wal_size();
    EXPECT_EQ(wal_size, 0);
    
    // Verify data
    compio_seek(file, 0, COMPIO_SEEK_SET);
    std::vector<uint8_t> read_buf(2000);
    ASSERT_EQ(compio_read(read_buf.data(), 2000, file), 2000);
    EXPECT_EQ(std::memcmp(read_buf.data(), data.data(), 2000), 0);
    
    compio_close_file(file);
    compio_close_archive(archive);
}

TEST_F(AutoCheckpointTest, CheckpointDisabledWithZeroLimit) {
    compio_config config;
    compio_build_default_config(&config);
    config.wal_max_size_bytes = 0;
    config.block_size = 512; // Must set small block size to force eviction with cache_size=1
    config.cache_size__blocks = 1; // Force WAL writes
    
    compio_archive* archive = compio_open_archive(archive_path, "w+", &config);
    ASSERT_NE(archive, nullptr);
    
    compio_file* file = compio_open_file("data", archive);
    ASSERT_NE(file, nullptr);
    
    // Write incompressible data
    std::vector<uint8_t> data(2000);
    for(size_t i=0; i<data.size(); ++i) data[i] = static_cast<uint8_t>(rand() % 256);
    
    uint64_t written = compio_write(data.data(), data.size(), file);
    ASSERT_EQ(written, data.size());
    
    // WAL size should be > 0 (no checkpoint)
    // 2000 bytes written -> ~2000 bytes in WAL (metadata overhead + data)
    
    uint64_t wal_size = get_wal_size();
    EXPECT_GT(wal_size, 1000); 
    
    compio_close_file(file);
    compio_close_archive(archive);
}
