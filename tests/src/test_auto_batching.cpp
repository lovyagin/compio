#include <gtest/gtest.h>
#include "compio.h"
#include <cstring>
#include <string>
#include "test_util.hpp"

class AutoBatchingTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmp_path[512];
        generate_tmp_fn(tmp_path, sizeof(tmp_path));
        archive_path = std::string(tmp_path);
        
        compio_build_default_config(&config);
        config.auto_batch_size = 4; // Small batch size for testing
        config.wal_sync_mode = COMPIO_WAL_SYNC_NORMAL;
    }
    
    void TearDown() override {
        remove(archive_path.c_str());
    }
    
    std::string archive_path;
    compio_config config;
};

TEST_F(AutoBatchingTest, StartsAfterThreeSequentialOps) {
    // Test that auto-batching starts after 3 consecutive sequential operations
    compio_archive* archive = compio_open_archive(archive_path.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);
    
    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    char data[32] = "test";
    
    // First 3 operations should not be batched yet
    EXPECT_FALSE(compio_is_auto_batching(file));
    EXPECT_EQ(compio_write(data, strlen(data), file), strlen(data));
    EXPECT_FALSE(compio_is_auto_batching(file));
    
    EXPECT_EQ(compio_write(data, strlen(data), file), strlen(data));
    EXPECT_FALSE(compio_is_auto_batching(file));
    
    EXPECT_EQ(compio_write(data, strlen(data), file), strlen(data));
    EXPECT_TRUE(compio_is_auto_batching(file)); // Should start auto-batching now
    
    compio_close_file(file);
    compio_close_archive(archive);
}

TEST_F(AutoBatchingTest, EndsAtConfiguredBatchSize) {
    // Test that auto-batch ends when auto_batch_size is reached
    compio_archive* archive = compio_open_archive(archive_path.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);
    
    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    char data[32] = "test";
    
    // Write enough operations to trigger and complete auto-batch
    for (int i = 0; i < config.auto_batch_size + 2; i++) {
        compio_write(data, strlen(data), file);
    }
    
    // Should have ended the auto-batch after batch_size operations
    EXPECT_FALSE(compio_is_auto_batching(file));
    
    compio_close_file(file);
    compio_close_archive(archive);
}

TEST_F(AutoBatchingTest, BreaksOnSeekOperation) {
    // Test that seek operations break auto-batching
    compio_archive* archive = compio_open_archive(archive_path.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);
    
    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    char data[32] = "test";
    
    // Start auto-batching
    for (int i = 0; i < 4; i++) {
        compio_write(data, strlen(data), file);
    }
    EXPECT_TRUE(compio_is_auto_batching(file));
    
    // Seek should break auto-batching
    compio_seek(file, 0, COMPIO_SEEK_SET);
    EXPECT_FALSE(compio_is_auto_batching(file));
    
    compio_close_file(file);
    compio_close_archive(archive);
}

TEST_F(AutoBatchingTest, EndsOnFileClose) {
    // Test that file close ends active auto-batching
    compio_archive* archive = compio_open_archive(archive_path.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);
    
    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    char data[32] = "test";
    
    // Start auto-batching but don't complete it
    for (int i = 0; i < 4; i++) {
        compio_write(data, strlen(data), file);
    }
    EXPECT_TRUE(compio_is_auto_batching(file));
    
    // Close should end auto-batching gracefully
    EXPECT_EQ(compio_close_file(file), 0);
    
    compio_close_archive(archive);
}

TEST_F(AutoBatchingTest, DisabledWhenConfigZero) {
    // Test that auto-batching is disabled when auto_batch_size = 0
    config.auto_batch_size = 0; // Disable auto-batching
    
    compio_archive* archive = compio_open_archive(archive_path.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);
    
    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    char data[32] = "test";
    
    // Many sequential operations should never trigger auto-batching
    for (int i = 0; i < 10; i++) {
        compio_write(data, strlen(data), file);
        EXPECT_FALSE(compio_is_auto_batching(file));
    }
    
    compio_close_file(file);
    compio_close_archive(archive);
}

TEST_F(AutoBatchingTest, EnabledByDefaultWithNewConfig) {
    // Test that auto-batching is enabled by default with new config
    compio_config default_config;
    compio_build_default_config(&default_config);
    default_config.wal_sync_mode = COMPIO_WAL_SYNC_NORMAL;
    
    compio_archive* archive = compio_open_archive(archive_path.c_str(), "w+", &default_config);
    ASSERT_NE(archive, nullptr);
    
    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    char data[32] = "test";
    
    // Should trigger auto-batching with default config
    EXPECT_GT(default_config.auto_batch_size, 0);
    
    for (int i = 0; i < 5; i++) {
        compio_write(data, strlen(data), file);
    }
    
    EXPECT_TRUE(compio_is_auto_batching(file));
    
    compio_close_file(file);
    compio_close_archive(archive);
}

TEST_F(AutoBatchingTest, WorksWithInsertOperations) {
    // Test that auto-batching works with insert operations
    compio_archive* archive = compio_open_archive(archive_path.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);
    
    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    char data[32] = "test";
    
    // Sequential inserts should trigger auto-batching
    EXPECT_FALSE(compio_is_auto_batching(file));
    
    for (int i = 0; i < 4; i++) {
        compio_insert(data, strlen(data), file);
    }
    
    EXPECT_TRUE(compio_is_auto_batching(file));
    
    compio_close_file(file);
    compio_close_archive(archive);
}

TEST_F(AutoBatchingTest, BreaksOnNonSequentialAccess) {
    // Test that non-sequential operations break auto-batching
    compio_archive* archive = compio_open_archive(archive_path.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);
    
    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    char data[32] = "test";
    
    // Start with sequential operations
    for (int i = 0; i < 4; i++) {
        compio_write(data, strlen(data), file);
    }
    EXPECT_TRUE(compio_is_auto_batching(file));
    
    // Jump to different offset - should break batching
    compio_seek(file, 1000, COMPIO_SEEK_SET);
    compio_write(data, strlen(data), file);
    
    EXPECT_FALSE(compio_is_auto_batching(file));
    
    compio_close_file(file);
    compio_close_archive(archive);
}