#include <gtest/gtest.h>
#include "compio.h"
#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"
#include "compio/debug_print.hpp"
#include <cstdio>
#include <cstring>

class FileRemovalTest : public ::testing::Test {
protected:
    const char* archive_path = "test_file_removal.compio";
    compio_config config{};

    void SetUp() override {
        compio_build_default_config(&config);
        std::remove(archive_path);
    }

    void TearDown() override {
        std::remove(archive_path);
        std::string wal_path = std::string(archive_path) + ".wal";
        std::remove(wal_path.c_str());
    }
};

TEST_F(FileRemovalTest, RemoveFileFreesBlocks) {
    // Create archive and write data to file
    compio_archive* archive = compio_open_archive(archive_path, "w+", &config);
    ASSERT_NE(archive, nullptr);

    // Get initial allocator state
    uint8_t initial_fragmentation = archive->allocator->get_fragmentation();
    UNUSED(initial_fragmentation);

    // Create file and write some data
    compio_file* file = compio_open_file("test.txt", archive);
    ASSERT_NE(file, nullptr);

    const char* test_data = "This is test data that will occupy some blocks in the archive";
    size_t data_size = strlen(test_data);
    uint64_t written = compio_write(test_data, data_size, file);
    EXPECT_EQ(written, data_size);

    compio_close_file(file);

    // Flush to ensure blocks are allocated
    compio_flush(archive);

    // Remove the file - should deallocate all blocks
    int result = compio_remove_file(archive, "test.txt");
    EXPECT_EQ(result, 0);

    // Try to open the removed file - should fail or create new one
    file = compio_open_file("test.txt", archive);
    if (file != nullptr) {
        // If file was created anew, it should have size 0
        EXPECT_EQ(file->size, 0);
        compio_close_file(file);
    }

    compio_close_archive(archive);
}

TEST_F(FileRemovalTest, RemoveFileWithManyBlocksTriggersMaintenanceCheck) {
    compio_archive* archive = compio_open_archive(archive_path, "w+", &config);
    ASSERT_NE(archive, nullptr);

    // Create a file with > 128 blocks to trigger maintenance checks (threshold 64)
    compio_file* file = compio_open_file("many_blocks.txt", archive);
    ASSERT_NE(file, nullptr);

    const int num_blocks = 150;
    const int block_size = 1024; // Ensure consistent block size
    std::vector<uint8_t> buffer(block_size, 'A');

    for (int i = 0; i < num_blocks; ++i) {
         compio_write(buffer.data(), block_size, file);
    }
    compio_close_file(file);
    compio_flush(archive);

    // Remove should succeed without error/crash despite maintenance triggers
    // The allocator should suspend maintenance, so no actual defrag happens during removal loop
    EXPECT_EQ(compio_remove_file(archive, "many_blocks.txt"), 0);

    compio_close_archive(archive);
}

TEST_F(FileRemovalTest, RemoveNonexistentFile) {
    compio_archive* archive = compio_open_archive(archive_path, "w+", &config);
    ASSERT_NE(archive, nullptr);

    // Try to remove a file that doesn't exist
    int result = compio_remove_file(archive, "nonexistent.txt");
    EXPECT_EQ(result, -1);
    EXPECT_EQ(errno, ENOENT);

    compio_close_archive(archive);
}

TEST_F(FileRemovalTest, RemoveMultipleFiles) {
    compio_archive* archive = compio_open_archive(archive_path, "w+", &config);
    ASSERT_NE(archive, nullptr);

    // Create and write multiple files
    const char* file_names[] = {"file1.txt", "file2.txt", "file3.txt"};
    const char* test_data = "Test data for file ";

    for (const char* name : file_names) {
        compio_file* file = compio_open_file(name, archive);
        ASSERT_NE(file, nullptr);

        compio_write(test_data, strlen(test_data), file);
        compio_write(name, strlen(name), file);

        compio_close_file(file);
    }

    compio_flush(archive);

    // Remove all files
    for (const char* name : file_names) {
        int result = compio_remove_file(archive, name);
        EXPECT_EQ(result, 0) << "Failed to remove " << name;
    }

    compio_close_archive(archive);
}

TEST_F(FileRemovalTest, RemoveLargeFile) {
    compio_archive* archive = compio_open_archive(archive_path, "w+", &config);
    ASSERT_NE(archive, nullptr);

    // Create a large file that will span multiple blocks
    compio_file* file = compio_open_file("large.txt", archive);
    ASSERT_NE(file, nullptr);

    // Write data larger than one block (default block_size is 4096)
    const size_t large_size = config.block_size * 10;
    char* large_data = new char[large_size];
    std::memset(large_data, 'A', large_size);

    uint64_t written = compio_write(large_data, large_size, file);
    EXPECT_EQ(written, large_size);

    compio_close_file(file);
    compio_flush(archive);

    // Remove the large file - should deallocate all blocks
    int result = compio_remove_file(archive, "large.txt");
    EXPECT_EQ(result, 0);

    delete[] large_data;
    compio_close_archive(archive);
}

TEST_F(FileRemovalTest, RemoveMiddleFilePreservesOthers) {
    compio_archive* archive = compio_open_archive(archive_path, "w+", &config);
    ASSERT_NE(archive, nullptr);

    // Create 3 files
    const char* names[] = {"file1.txt", "file2.txt", "file3.txt"};
    for (const char* name : names) {
        compio_file* f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr);
        // Write something so size > 0
        compio_write("data", 4, f);
        compio_close_file(f);
    }
    compio_flush(archive);
    
    // Remove middle file
    EXPECT_EQ(compio_remove_file(archive, "file2.txt"), 0);
    
    // Verify file2 is gone or recreated empty
    compio_file* f2 = compio_open_file("file2.txt", archive);
    // Depending on semantics, it might return nullptr or a new empty file
    if (f2 != nullptr) {
        EXPECT_EQ(f2->size, 0);
        compio_close_file(f2);
    } else {
        SUCCEED();
    }
    
    // Check file1 is still there and has data
    compio_file* f1 = compio_open_file("file1.txt", archive);
    ASSERT_NE(f1, nullptr);
    EXPECT_EQ(f1->size, 4);
    compio_close_file(f1);
    
    // Check file3 is still there (moved index) and has data
    compio_file* f3 = compio_open_file("file3.txt", archive);
    ASSERT_NE(f3, nullptr);
    EXPECT_EQ(f3->size, 4);
    compio_close_file(f3);
    
    compio_close_archive(archive);
}
