#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include <string>
#include <random>
#include <cstdio>

#include "compio.h"
#include "test_util.hpp"

class RangeCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize config with small block size for testing
        compio_build_default_config(&config);
        config.block_size = 1024;
        config.cache_size__nodes = 64;
        config.wal_sync_mode = COMPIO_WAL_SYNC_NORMAL;
        
        // Generate unique archive path
        char tmp_path[512];
        generate_tmp_fn(tmp_path, sizeof(tmp_path));
        archive_path = std::string(tmp_path);
    }

    void TearDown() override {
        std::remove(archive_path.c_str());
    }

    compio_config config;
    std::string archive_path;
};

TEST_F(RangeCacheTest, BasicRangeCaching) {
    // Test range caching for write operations 
    compio_archive* archive = compio_open_archive(archive_path.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);
    
    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    // Write multiple blocks worth of data to create an initial state
    std::vector<char> data1(512, 'A');
    std::vector<char> data2(512, 'B'); 
    std::vector<char> data3(512, 'C');
    
    EXPECT_GT(compio_write(data1.data(), data1.size(), file), 0);
    EXPECT_GT(compio_write(data2.data(), data2.size(), file), 0);
    EXPECT_GT(compio_write(data3.data(), data3.size(), file), 0);
    
    // Now overwrite a section multiple times - this should benefit from range caching
    compio_seek(file, 256, COMPIO_SEEK_SET);
    
    std::vector<char> overwrite_data(256, 'X');
    EXPECT_GT(compio_write(overwrite_data.data(), overwrite_data.size(), file), 0);
    EXPECT_GT(compio_write(overwrite_data.data(), overwrite_data.size(), file), 0); // Second write should use cache
    
    // Verify data was written correctly
    compio_seek(file, 256, COMPIO_SEEK_SET);
    std::vector<char> read_buffer(512);
    EXPECT_EQ(compio_read(read_buffer.data(), 512, file), 512);
    EXPECT_EQ(std::memcmp(read_buffer.data(), overwrite_data.data(), 256), 0);
    EXPECT_EQ(std::memcmp(read_buffer.data() + 256, overwrite_data.data(), 256), 0);
    
    compio_close_file(file);
    compio_close_archive(archive);
}

TEST_F(RangeCacheTest, CacheInvalidationOnWrite) {
    compio_archive* archive = compio_open_archive(archive_path.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);
    
    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    // Write initial data
    std::vector<char> data(1024, 'A');
    EXPECT_GT(compio_write(data.data(), data.size(), file), 0);
    
    // Write to same range multiple times - first write populates cache, second uses it
    compio_seek(file, 0, COMPIO_SEEK_SET);
    std::vector<char> data1(512, 'B');
    EXPECT_GT(compio_write(data1.data(), data1.size(), file), 0);  // Populates cache
    
    compio_seek(file, 0, COMPIO_SEEK_SET);  
    std::vector<char> data2(512, 'C');
    EXPECT_GT(compio_write(data2.data(), data2.size(), file), 0);  // Should use cache (but cache gets invalidated after)
    
    // Verify final data is correct
    compio_seek(file, 0, COMPIO_SEEK_SET);
    std::vector<char> read_buffer(1024);
    EXPECT_EQ(compio_read(read_buffer.data(), 1024, file), 1024);
    
    // Check that first part is 'C' and second part is still 'A'
    EXPECT_EQ(read_buffer[256], 'C');
    EXPECT_EQ(read_buffer[512], 'A');
    
    compio_close_file(file);
    compio_close_archive(archive);
}

TEST_F(RangeCacheTest, CacheInvalidationOnInsert) {
    compio_archive* archive = compio_open_archive(archive_path.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);
    
    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    // Write initial data
    std::vector<char> data(512, 'A');
    EXPECT_GT(compio_write(data.data(), data.size(), file), 0);
    
    // Reset and read to populate cache  
    compio_seek(file, 0, COMPIO_SEEK_SET);
    std::vector<char> read_buffer(512);
    EXPECT_EQ(compio_read(read_buffer.data(), 512, file), 512);
    
    // Insert data - should invalidate cache
    compio_seek(file, 256, COMPIO_SEEK_SET);
    std::vector<char> insert_data(128, 'B');
    EXPECT_GT(compio_insert(insert_data.data(), insert_data.size(), file), 0);
    
    // Read back and verify structure changed
    compio_seek(file, 0, COMPIO_SEEK_SET);  
    EXPECT_EQ(compio_read(read_buffer.data(), 512, file), 512);
    
    // First 256 bytes should be 'A', next 128 should be 'B'
    EXPECT_EQ(read_buffer[200], 'A');
    EXPECT_EQ(read_buffer[256], 'B');
    EXPECT_EQ(read_buffer[300], 'B');
    
    compio_close_file(file);
    compio_close_archive(archive);
}

TEST_F(RangeCacheTest, CacheInvalidationOnErase) {
    compio_archive* archive = compio_open_archive(archive_path.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);
    
    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    // Write initial data with pattern
    std::vector<char> data1(256, 'A');
    std::vector<char> data2(256, 'B');
    std::vector<char> data3(256, 'C');
    
    EXPECT_GT(compio_write(data1.data(), data1.size(), file), 0);
    EXPECT_GT(compio_write(data2.data(), data2.size(), file), 0);
    EXPECT_GT(compio_write(data3.data(), data3.size(), file), 0);
    
    // Reset and read to populate cache
    compio_seek(file, 0, COMPIO_SEEK_SET);
    std::vector<char> read_buffer(768);
    EXPECT_EQ(compio_read(read_buffer.data(), 768, file), 768);
    
    // Erase middle section - should invalidate cache
    compio_seek(file, 256, COMPIO_SEEK_SET);
    EXPECT_GT(compio_erase(256, file), 0); // Erase the 'B' section
    
    // Read back - should now have A directly followed by C
    compio_seek(file, 0, COMPIO_SEEK_SET);
    std::vector<char> new_read_buffer(512);
    EXPECT_EQ(compio_read(new_read_buffer.data(), 512, file), 512);
    
    EXPECT_EQ(new_read_buffer[200], 'A');  // Still A
    EXPECT_EQ(new_read_buffer[256], 'C');  // Now C instead of B
    EXPECT_EQ(new_read_buffer[300], 'C');  // Still C
    
    compio_close_file(file);
    compio_close_archive(archive);
}