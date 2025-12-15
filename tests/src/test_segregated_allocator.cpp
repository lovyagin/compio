/**
 * @file test_segregated_allocator.cpp
 * @brief Unit tests for Segregated Free List implementation
 */

#include <gtest/gtest.h>
#include "compio/segregated_allocator.hpp"

using namespace compio;

class SegregatedAllocatorTest : public ::testing::Test {
protected:
    segregated_free_list allocator;
};

TEST_F(SegregatedAllocatorTest, BasicAllocation) {
    // Add a free block
    allocator.add_free_block(0, 4096);
    EXPECT_EQ(allocator.total_free(), 4096);
    EXPECT_EQ(allocator.block_count(), 1);

    // Allocate from it
    uint64_t offset = allocator.allocate_best_fit(1024);
    EXPECT_EQ(offset, 0);
    EXPECT_EQ(allocator.total_free(), 3072);
}

TEST_F(SegregatedAllocatorTest, ExactFitAllocation) {
    allocator.add_free_block(0, 1024);

    uint64_t offset = allocator.allocate_best_fit(1024);
    EXPECT_EQ(offset, 0);
    EXPECT_EQ(allocator.total_free(), 0);
    EXPECT_EQ(allocator.block_count(), 0);
}

TEST_F(SegregatedAllocatorTest, BlockMerging) {
    // Add two adjacent blocks
    allocator.add_free_block(0, 1024);
    allocator.add_free_block(1024, 1024);

    // Should be merged into one
    EXPECT_EQ(allocator.block_count(), 1);
    EXPECT_EQ(allocator.total_free(), 2048);

    // Should be able to allocate the full size
    uint64_t offset = allocator.allocate_best_fit(2048);
    EXPECT_EQ(offset, 0);
}

TEST_F(SegregatedAllocatorTest, BestFitStrategy) {
    // Add blocks of different sizes
    allocator.add_free_block(0, 512);      // Bucket 1 (257-512)
    allocator.add_free_block(1024, 1024);  // Bucket 2 (513-1K)
    allocator.add_free_block(4096, 4096);  // Bucket 4 (2K-4K)

    // Best fit for 500 should use the 512 block
    uint64_t offset = allocator.allocate_best_fit(500);
    EXPECT_EQ(offset, 0);

    // Best fit for 600 should use the 1024 block
    offset = allocator.allocate_best_fit(600);
    EXPECT_EQ(offset, 1024);
}

TEST_F(SegregatedAllocatorTest, FirstFitStrategy) {
    allocator.add_free_block(0, 2048);     // Bucket 3 (1K-2K)
    allocator.add_free_block(4096, 1024);  // Bucket 2 (513-1K)

    // First fit in SFL searches by bucket, not by offset
    // For 512 bytes, starts in bucket 1 (257-512), no match
    // Then bucket 2 (513-1K) - finds 1024 block at offset 4096
    uint64_t offset = allocator.allocate_first_fit(512);
    // Should find the smallest suitable bucket first
    EXPECT_NE(offset, UINT64_MAX);
    EXPECT_EQ(allocator.total_free(), 2048 + 1024 - 512);
}

TEST_F(SegregatedAllocatorTest, WorstFitStrategy) {
    allocator.add_free_block(0, 1024);
    allocator.add_free_block(2048, 4096);
    allocator.add_free_block(8192, 2048);

    // Worst fit should return largest block
    uint64_t offset = allocator.allocate_worst_fit(512);
    EXPECT_EQ(offset, 2048);
}

TEST_F(SegregatedAllocatorTest, AllocationFailure) {
    allocator.add_free_block(0, 512);

    // Should fail - no block large enough
    uint64_t offset = allocator.allocate_best_fit(1024);
    EXPECT_EQ(offset, UINT64_MAX);
}

TEST_F(SegregatedAllocatorTest, IsRegionFree) {
    allocator.add_free_block(1000, 500);

    EXPECT_TRUE(allocator.is_region_free(1000, 500));
    EXPECT_TRUE(allocator.is_region_free(1100, 100));
    EXPECT_FALSE(allocator.is_region_free(0, 100));
    EXPECT_FALSE(allocator.is_region_free(1000, 600)); // Extends beyond
}

TEST_F(SegregatedAllocatorTest, Serialization) {
    allocator.add_free_block(100, 1024);
    allocator.add_free_block(2048, 2048);
    allocator.add_free_block(8192, 512);

    std::vector<uint8_t> buffer;
    uint32_t size = allocator.serialize(buffer);

    EXPECT_GT(size, 0);

    // Create new allocator and deserialize
    segregated_free_list new_allocator;
    EXPECT_TRUE(new_allocator.deserialize(buffer.data(), buffer.size()));

    EXPECT_EQ(new_allocator.block_count(), allocator.block_count());
    EXPECT_EQ(new_allocator.total_free(), allocator.total_free());
}

TEST_F(SegregatedAllocatorTest, StressTest) {
    // Add many blocks of various sizes
    for (uint64_t i = 0; i < 100; ++i) {
        allocator.add_free_block(i * 10000, 256 + (i % 10) * 256);
    }

    // Perform many allocations
    int successful = 0;
    for (int i = 0; i < 50; ++i) {
        uint64_t offset = allocator.allocate_best_fit(512);
        if (offset != UINT64_MAX) successful++;
    }

    EXPECT_GT(successful, 0);
}

TEST_F(SegregatedAllocatorTest, BucketDistribution) {
    // Add blocks that should go to different buckets
    allocator.add_free_block(0, 100);      // Bucket 0 (0-256)
    allocator.add_free_block(1000, 400);   // Bucket 1 (257-512)
    allocator.add_free_block(2000, 800);   // Bucket 2 (513-1K)
    allocator.add_free_block(3000, 1500);  // Bucket 3 (1K-2K)
    allocator.add_free_block(5000, 3000);  // Bucket 4 (2K-4K)
    allocator.add_free_block(10000, 6000); // Bucket 5 (4K-8K)

    EXPECT_EQ(allocator.block_count(), 6);

    // Each allocation should come from appropriate bucket
    EXPECT_NE(allocator.allocate_best_fit(50), UINT64_MAX);   // From bucket 0
    EXPECT_NE(allocator.allocate_best_fit(300), UINT64_MAX);  // From bucket 1
    EXPECT_NE(allocator.allocate_best_fit(700), UINT64_MAX);  // From bucket 2
}

TEST_F(SegregatedAllocatorTest, SplitAndRebucket) {
    // Add large block
    allocator.add_free_block(0, 8192);
    EXPECT_EQ(allocator.block_count(), 1);

    // Allocate small portion - remainder should move to different bucket
    allocator.allocate_best_fit(256);

    EXPECT_EQ(allocator.block_count(), 1);
    EXPECT_EQ(allocator.total_free(), 8192 - 256);
}
