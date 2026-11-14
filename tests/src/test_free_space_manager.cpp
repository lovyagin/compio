/**
 * @file test_free_space_manager.cpp
 * @brief Unit tests for free_blocks_manager: merging, placement strategies, bookkeeping
 */

#include <gtest/gtest.h>

#include <vector>

#include "compio/allocator.hpp"

using namespace compio;

class FreeSpaceManagerTest : public ::testing::Test {
protected:
    uint64_t file_size = 1024 * 1024; // 1MB
    free_blocks_manager manager{&file_size};
};

TEST_F(FreeSpaceManagerTest, BasicAllocation) {
    manager.add_free_block(0, 4096);

    uint64_t offset = manager.allocate_block(1024, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset, 0);
}

TEST_F(FreeSpaceManagerTest, ExactFitAllocation) {
    manager.add_free_block(0, 1024);

    uint64_t offset = manager.allocate_block(1024, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset, 0);
    EXPECT_EQ(manager.free_region_count(), 0u);
}

TEST_F(FreeSpaceManagerTest, BlockMerging) {
    // Add two adjacent blocks - should be merged
    manager.add_free_block(0, 1024);
    manager.add_free_block(1024, 1024);
    EXPECT_EQ(manager.free_region_count(), 1u);

    // Should be able to allocate the full merged size
    uint64_t offset = manager.allocate_block(2048, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset, 0);
}

TEST_F(FreeSpaceManagerTest, MergesWithBothNeighbours) {
    manager.add_free_block(0, 100);
    manager.add_free_block(200, 100);
    EXPECT_EQ(manager.free_region_count(), 2u);

    uint64_t merged_offset = 0;
    uint64_t merged_size = 0;
    EXPECT_TRUE(manager.add_free_block(100, 100, &merged_offset, &merged_size));
    EXPECT_EQ(merged_offset, 0u);
    EXPECT_EQ(merged_size, 300u);
    EXPECT_EQ(manager.free_region_count(), 1u);
    EXPECT_EQ(manager.get_fragmentation_stats().total_free_bytes, 300u);
}

TEST_F(FreeSpaceManagerTest, RejectsRegionThatIsAlreadyFree) {
    ASSERT_TRUE(manager.add_free_block(1000, 500));

    EXPECT_FALSE(manager.add_free_block(1000, 500)); // the same region
    EXPECT_FALSE(manager.add_free_block(1200, 100)); // inside
    EXPECT_FALSE(manager.add_free_block(900, 200));  // overlaps the start
    EXPECT_FALSE(manager.add_free_block(1400, 200)); // overlaps the end
    EXPECT_FALSE(manager.add_free_block(900, 700));  // covers it
    EXPECT_FALSE(manager.add_free_block(2000, 0));
    EXPECT_FALSE(manager.add_free_block(UINT64_MAX - 10, 100));

    EXPECT_EQ(manager.free_region_count(), 1u);
    EXPECT_EQ(manager.get_fragmentation_stats().total_free_bytes, 500u);
}

TEST_F(FreeSpaceManagerTest, BestFitStrategy) {
    manager.add_free_block(0, 512);
    manager.add_free_block(1024, 1024);
    manager.add_free_block(4096, 4096);

    // Best fit for 500 should use the 512 block
    uint64_t offset = manager.allocate_block(500, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset, 0);

    // Best fit for 600 should use the 1024 block
    offset = manager.allocate_block(600, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset, 1024);
}

TEST_F(FreeSpaceManagerTest, BestFitTakesLowestAmongEqualRegions) {
    manager.add_free_block(9000, 700);
    manager.add_free_block(3000, 700);
    manager.add_free_block(6000, 700);
    manager.add_free_block(0, 2000);

    EXPECT_EQ(manager.allocate_block(700, allocation_strategy::BEST_FIT), 3000u);
    EXPECT_EQ(manager.allocate_block(700, allocation_strategy::BEST_FIT), 6000u);
    EXPECT_EQ(manager.allocate_block(700, allocation_strategy::BEST_FIT), 9000u);
    EXPECT_EQ(manager.allocate_block(700, allocation_strategy::BEST_FIT), 0u);
}

TEST_F(FreeSpaceManagerTest, FirstFitTakesLowestRegionThatFits) {
    manager.add_free_block(0, 256);
    manager.add_free_block(1000, 2048);
    manager.add_free_block(4096, 1024);

    EXPECT_EQ(manager.allocate_block(512, allocation_strategy::FIRST_FIT), 1000u);
    EXPECT_EQ(manager.allocate_block(100, allocation_strategy::FIRST_FIT), 0u);
    EXPECT_EQ(manager.allocate_block(4000, allocation_strategy::FIRST_FIT), UINT64_MAX);
}

TEST_F(FreeSpaceManagerTest, WorstFitStrategy) {
    manager.add_free_block(0, 1024);
    manager.add_free_block(2048, 4096);
    manager.add_free_block(8192, 2048);

    // Worst fit should return largest block
    uint64_t offset = manager.allocate_block(512, allocation_strategy::WORST_FIT);
    EXPECT_EQ(offset, 2048);
    EXPECT_EQ(manager.allocate_block(4096, allocation_strategy::WORST_FIT), UINT64_MAX);
}

TEST_F(FreeSpaceManagerTest, NextFitContinuesAfterLastAllocationAndWraps) {
    manager.add_free_block(0, 1000);
    manager.add_free_block(2000, 1000);
    manager.add_free_block(4000, 1000);

    EXPECT_EQ(manager.allocate_block(400, allocation_strategy::NEXT_FIT), 0u);
    // The remainder of the region just used comes first.
    EXPECT_EQ(manager.allocate_block(400, allocation_strategy::NEXT_FIT), 400u);
    // 200 bytes are left there: too small, so the search moves on.
    EXPECT_EQ(manager.allocate_block(900, allocation_strategy::NEXT_FIT), 2000u);
    EXPECT_EQ(manager.allocate_block(900, allocation_strategy::NEXT_FIT), 4000u);
    // Nothing behind the last allocation fits: wrap to the beginning.
    EXPECT_EQ(manager.allocate_block(150, allocation_strategy::NEXT_FIT), 800u);
    EXPECT_EQ(manager.allocate_block(150, allocation_strategy::NEXT_FIT), UINT64_MAX);
}

TEST_F(FreeSpaceManagerTest, AllocationFailure) {
    manager.add_free_block(0, 512);

    // Should fail - no block large enough
    uint64_t offset = manager.allocate_block(1024, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset, UINT64_MAX);
}

TEST_F(FreeSpaceManagerTest, IsRegionFree) {
    manager.add_free_block(1000, 500);

    EXPECT_TRUE(manager.is_region_free(1000, 500));
    EXPECT_TRUE(manager.is_region_free(1100, 100));
    EXPECT_FALSE(manager.is_region_free(0, 100));
    EXPECT_FALSE(manager.is_region_free(900, 200));
    EXPECT_FALSE(manager.is_region_free(1400, 200));
    EXPECT_FALSE(manager.is_region_free(1500, 1));
}

TEST_F(FreeSpaceManagerTest, Serialization) {
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
    EXPECT_EQ(new_manager.free_region_count(), 3u);
    EXPECT_TRUE(new_manager.is_region_free(2048, 2048));
}

TEST_F(FreeSpaceManagerTest, StatsReportExtremes) {
    manager.add_free_block(0, 100);
    manager.add_free_block(1000, 400);
    manager.add_free_block(5000, 3000);

    const auto stats = manager.get_fragmentation_stats();
    EXPECT_EQ(stats.num_free_regions, 3u);
    EXPECT_EQ(stats.total_free_bytes, 3500u);
    EXPECT_EQ(stats.smallest_free_region, 100u);
    EXPECT_EQ(stats.largest_free_region, 3000u);
}

TEST_F(FreeSpaceManagerTest, SplitKeepsRemainderFree) {
    manager.add_free_block(0, 8192);

    EXPECT_EQ(manager.allocate_block(256, allocation_strategy::BEST_FIT), 0u);
    EXPECT_EQ(manager.free_region_count(), 1u);
    EXPECT_TRUE(manager.is_region_free(256, 8192 - 256));
    EXPECT_EQ(manager.allocate_block(7000, allocation_strategy::BEST_FIT), 256u);
}

// With a list scanned on every operation this takes minutes; the ordered
// indexes make it a fraction of a second.
TEST(FreeSpaceManagerScaleTest, ManyRegions) {
    constexpr uint64_t REGIONS = 200000;
    constexpr uint64_t STEP = 4096;
    uint64_t big_file = REGIONS * STEP;
    free_blocks_manager manager{&big_file};

    // Highest offsets first, sizes cycling through 64 distinct values.
    for (uint64_t i = REGIONS; i-- > 0;) {
        ASSERT_TRUE(manager.add_free_block(i * STEP, 64 + (i % 64) * 16));
    }
    ASSERT_EQ(manager.free_region_count(), REGIONS);

    std::vector<uint8_t> buffer;
    manager.serialize(buffer);
    free_blocks_manager loaded{&big_file};
    ASSERT_TRUE(loaded.deserialize(buffer.data(), static_cast<uint32_t>(buffer.size())));
    ASSERT_EQ(loaded.free_region_count(), REGIONS);

    // Every region of the smallest size is consumed exactly, lowest offset first.
    for (uint64_t i = 0; i < REGIONS; i += 64) {
        ASSERT_EQ(loaded.allocate_block(64, allocation_strategy::BEST_FIT), i * STEP);
    }
    EXPECT_EQ(loaded.free_region_count(), REGIONS - REGIONS / 64);

    // Give them back: each one is isolated, so nothing merges.
    for (uint64_t i = 0; i < REGIONS; i += 64) {
        ASSERT_TRUE(loaded.add_free_block(i * STEP, 64));
    }
    EXPECT_EQ(loaded.free_region_count(), REGIONS);
}
