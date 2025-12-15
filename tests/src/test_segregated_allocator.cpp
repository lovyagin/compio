/**
 * @file test_segregated_allocator.cpp
 * @brief Unit tests for Segregated Free List integration in free_blocks_manager
 *
 * Tests the bucket-based size_index that replaced std::multimap
 */

#include <gtest/gtest.h>
#include "compio/allocator.hpp"

using namespace compio;

class SegregatedIndexTest : public ::testing::Test {
protected:
    uint64_t file_size = 1024 * 1024; // 1MB
    free_blocks_manager manager{&file_size};
};

TEST_F(SegregatedIndexTest, BasicAllocation) {
    manager.add_free_block(0, 4096);

    uint64_t offset = manager.allocate_block(1024, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset, 0);
}

TEST_F(SegregatedIndexTest, ExactFitAllocation) {
    manager.add_free_block(0, 1024);

    uint64_t offset = manager.allocate_block(1024, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset, 0);
}

TEST_F(SegregatedIndexTest, BlockMerging) {
    // Add two adjacent blocks - should be merged
    manager.add_free_block(0, 1024);
    manager.add_free_block(1024, 1024);

    // Should be able to allocate the full merged size
    uint64_t offset = manager.allocate_block(2048, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset, 0);
}

TEST_F(SegregatedIndexTest, BestFitStrategy) {
    // Add blocks of different sizes in different buckets
    manager.add_free_block(0, 512);      // Bucket 1 (257-512)
    manager.add_free_block(1024, 1024);  // Bucket 2 (513-1K)
    manager.add_free_block(4096, 4096);  // Bucket 4 (2K-4K)

    // Best fit for 500 should use the 512 block
    uint64_t offset = manager.allocate_block(500, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset, 0);

    // Best fit for 600 should use the 1024 block
    offset = manager.allocate_block(600, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset, 1024);
}

TEST_F(SegregatedIndexTest, FirstFitStrategy) {
    manager.add_free_block(0, 2048);
    manager.add_free_block(4096, 1024);

    // First fit searches buckets from smallest suitable
    uint64_t offset = manager.allocate_block(512, allocation_strategy::FIRST_FIT);
    EXPECT_NE(offset, UINT64_MAX);
}

TEST_F(SegregatedIndexTest, WorstFitStrategy) {
    manager.add_free_block(0, 1024);
    manager.add_free_block(2048, 4096);
    manager.add_free_block(8192, 2048);

    // Worst fit should return largest block
    uint64_t offset = manager.allocate_block(512, allocation_strategy::WORST_FIT);
    EXPECT_EQ(offset, 2048);
}

TEST_F(SegregatedIndexTest, AllocationFailure) {
    manager.add_free_block(0, 512);

    // Should fail - no block large enough
    uint64_t offset = manager.allocate_block(1024, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset, UINT64_MAX);
}

TEST_F(SegregatedIndexTest, IsRegionFree) {
    manager.add_free_block(1000, 500);

    EXPECT_TRUE(manager.is_region_free(1000, 500));
    EXPECT_TRUE(manager.is_region_free(1100, 100));
    EXPECT_FALSE(manager.is_region_free(0, 100));
}

TEST_F(SegregatedIndexTest, Serialization) {
    manager.add_free_block(100, 1024);
    manager.add_free_block(2048, 2048);
    manager.add_free_block(8192, 512);

    std::vector<uint8_t> buffer;
    uint32_t size = manager.serialize(buffer);

    EXPECT_GT(size, 0);

    // Create new manager and deserialize
    uint64_t new_file_size = 1024 * 1024;
    free_blocks_manager new_manager{&new_file_size};
    EXPECT_TRUE(new_manager.deserialize(buffer.data(), buffer.size()));
}

TEST_F(SegregatedIndexTest, StressTest) {
    // Add many blocks of various sizes
    for (uint64_t i = 0; i < 100; ++i) {
        manager.add_free_block(i * 10000, 256 + (i % 10) * 256);
    }

    // Perform many allocations
    int successful = 0;
    for (int i = 0; i < 50; ++i) {
        uint64_t offset = manager.allocate_block(512, allocation_strategy::BEST_FIT);
        if (offset != UINT64_MAX) successful++;
    }

    EXPECT_GT(successful, 0);
}

TEST_F(SegregatedIndexTest, BucketDistribution) {
    // Add blocks that should go to different buckets
    manager.add_free_block(0, 100);      // Bucket 0 (0-256)
    manager.add_free_block(1000, 400);   // Bucket 1 (257-512)
    manager.add_free_block(2000, 800);   // Bucket 2 (513-1K)
    manager.add_free_block(3000, 1500);  // Bucket 3 (1K-2K)
    manager.add_free_block(5000, 3000);  // Bucket 4 (2K-4K)
    manager.add_free_block(10000, 6000); // Bucket 5 (4K-8K)

    // Each allocation should come from appropriate bucket
    EXPECT_NE(manager.allocate_block(50, allocation_strategy::BEST_FIT), UINT64_MAX);
    EXPECT_NE(manager.allocate_block(300, allocation_strategy::BEST_FIT), UINT64_MAX);
    EXPECT_NE(manager.allocate_block(700, allocation_strategy::BEST_FIT), UINT64_MAX);
}

TEST_F(SegregatedIndexTest, SplitAndRebucket) {
    // Add large block
    manager.add_free_block(0, 8192);

    // Allocate small portion - remainder should move to different bucket
    manager.allocate_block(256, allocation_strategy::BEST_FIT);

    // Remaining ~8KB should still be allocatable
    uint64_t offset = manager.allocate_block(7000, allocation_strategy::BEST_FIT);
    EXPECT_NE(offset, UINT64_MAX);
}
