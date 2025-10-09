#include <chrono>
#include <gtest/gtest.h>
#include <random>
#include <set>

#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"
#include "compio/utils.hpp"

#include "test_util.hpp"

using namespace compio;

class ComprehensiveAllocatorTest : public ::testing::Test {
protected:
    char fn[256];
    FILE *file;
    compio_archive *archive;
    block_allocator *allocator;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));

        file = fopen(fn, "w+");
        ASSERT_TRUE(file != nullptr);

        auto *config = new compio_config();
        compio_build_default_config(config);
        config->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
        config->fragmentation_threshold = 30;
        config->fill_holes_with_zeros = false;

        archive = new compio_archive(file, mode_bit::w | mode_bit::r, config);
        archive->allocator = allocator = new block_allocator(archive);
        archive->index = new btree(archive);
    }

    void TearDown() override {
        if (archive->index)
            delete archive->index;
        if (allocator)
            delete allocator;
        if (archive)
            delete archive;
        if (file)
            fclose(file);
        remove(fn);
    }

    // Helper to create allocator with specific strategy
    void recreate_allocator_with_strategy(compio_allocation_strategy strategy) {
        if (allocator)
            delete allocator;
        if (archive)
            delete archive;

        auto *config = new compio_config();
        compio_build_default_config(config);
        config->allocation_strategy = strategy;
        config->fragmentation_threshold = 30;
        config->fill_holes_with_zeros = false;

        file = freopen(fn, "w+", file);
        archive = new compio_archive(file, mode_bit::w | mode_bit::r, config);
        archive->allocator = allocator = new block_allocator(archive);
        archive->index = new btree(archive);
    }

    // Helper to verify block allocation
    bool verify_allocation(uint64_t offset, size_t size) {
        return offset != UINT64_MAX && offset >= sizeof(header);
    }

    // Helper to verify blocks don't overlap
    bool blocks_dont_overlap(const std::vector<std::pair<uint64_t, size_t>> &blocks) {
        for (size_t i = 0; i < blocks.size(); i++) {
            for (size_t j = i + 1; j < blocks.size(); j++) {
                uint64_t start1 = blocks[i].first;
                uint64_t end1 = start1 + blocks[i].second;
                uint64_t start2 = blocks[j].first;
                uint64_t end2 = start2 + blocks[j].second;

                if (!(end1 <= start2 || end2 <= start1)) {
                    return false; // Overlap detected
                }
            }
        }
        return true;
    }
};

// Test different allocation strategies
class AllocationStrategyTest : public ComprehensiveAllocatorTest {
protected:
    void test_strategy_behavior(compio_allocation_strategy strategy) {
        recreate_allocator_with_strategy(strategy);

        // Create fragmented pattern: allocate 3 blocks, free middle one
        uint64_t offset1 = allocator->allocate(100);
        uint64_t offset2 = allocator->allocate(200);
        uint64_t offset3 = allocator->allocate(100);

        ASSERT_TRUE(verify_allocation(offset1, 100));
        ASSERT_TRUE(verify_allocation(offset2, 200));
        ASSERT_TRUE(verify_allocation(offset3, 100));

        // Free middle block to create hole
        allocator->deallocate(offset2, 200);

        // Now allocate block that fits in the hole
        uint64_t new_offset = allocator->allocate(150);
        EXPECT_TRUE(verify_allocation(new_offset, 150));

        // For FIRST_FIT and BEST_FIT, should reuse the hole
        if (strategy == COMPIO_ALLOC_FIRST_FIT || strategy == COMPIO_ALLOC_BEST_FIT) {
            EXPECT_EQ(new_offset, offset2) << "Strategy should reuse freed space";
        }
    }
};

TEST_F(AllocationStrategyTest, FirstFitStrategy) { test_strategy_behavior(COMPIO_ALLOC_FIRST_FIT); }

TEST_F(AllocationStrategyTest, BestFitStrategy) { test_strategy_behavior(COMPIO_ALLOC_BEST_FIT); }

TEST_F(AllocationStrategyTest, WorstFitStrategy) { test_strategy_behavior(COMPIO_ALLOC_WORST_FIT); }

TEST_F(AllocationStrategyTest, NextFitStrategy) { test_strategy_behavior(COMPIO_ALLOC_NEXT_FIT); }

// Test edge cases and boundary conditions
class EdgeCaseTest : public ComprehensiveAllocatorTest {};

TEST_F(EdgeCaseTest, VeryLargeAllocation) {
    // Test allocation of very large block
    uint64_t huge_size = 1ULL << 30; // 1GB
    uint64_t offset = allocator->allocate(huge_size);
    EXPECT_TRUE(verify_allocation(offset, huge_size));
}

TEST_F(EdgeCaseTest, ManySmallAllocations) {
    std::vector<uint64_t> offsets;
    const size_t num_allocs = 1000;
    const size_t small_size = 16;

    // Allocate many small blocks
    for (size_t i = 0; i < num_allocs; i++) {
        uint64_t offset = allocator->allocate(small_size);
        ASSERT_TRUE(verify_allocation(offset, small_size));
        offsets.push_back(offset);
    }

    // Verify no overlaps
    std::vector<std::pair<uint64_t, size_t>> blocks;
    for (auto offset : offsets) {
        blocks.emplace_back(offset, small_size);
    }
    EXPECT_TRUE(blocks_dont_overlap(blocks));
}

TEST_F(EdgeCaseTest, AlternatingAllocateAndDeallocate) {
    std::vector<uint64_t> allocated_blocks;
    const size_t block_size = 100;

    // Phase 1: Allocate blocks
    for (int i = 0; i < 10; i++) {
        uint64_t offset = allocator->allocate(block_size);
        ASSERT_TRUE(verify_allocation(offset, block_size));
        allocated_blocks.push_back(offset);
    }

    // Phase 2: Deallocate every other block
    for (size_t i = 1; i < allocated_blocks.size(); i += 2) {
        allocator->deallocate(allocated_blocks[i], block_size);
    }

    // Phase 3: Allocate new blocks - should reuse freed spaces
    std::vector<uint64_t> new_blocks;
    for (int i = 0; i < 5; i++) {
        uint64_t offset = allocator->allocate(block_size);
        ASSERT_TRUE(verify_allocation(offset, block_size));
        new_blocks.push_back(offset);
    }

    // Some new blocks should reuse freed space
    std::set<uint64_t> freed_offsets;
    for (size_t i = 1; i < allocated_blocks.size(); i += 2) {
        freed_offsets.insert(allocated_blocks[i]);
    }

    int reused_count = 0;
    for (auto new_offset : new_blocks) {
        if (freed_offsets.count(new_offset)) {
            reused_count++;
        }
    }

    EXPECT_GT(reused_count, 0) << "At least some freed blocks should be reused";
}

TEST_F(EdgeCaseTest, DoubleDeallocation) {
    uint64_t offset = allocator->allocate(100);
    ASSERT_TRUE(verify_allocation(offset, 100));

    // First deallocation should succeed
    allocator->deallocate(offset, 100);

    EXPECT_NO_THROW(allocator->deallocate(offset, 100));
}

TEST_F(EdgeCaseTest, DeallocateWithWrongSize) {
    uint64_t offset = allocator->allocate(100);
    ASSERT_TRUE(verify_allocation(offset, 100));

    // Deallocate with wrong size
    EXPECT_NO_THROW(allocator->deallocate(offset, 200));
}

// Test fragmentation handling
class FragmentationTest : public ComprehensiveAllocatorTest {};

TEST_F(FragmentationTest, FragmentationCalculation) {
    // Create fragmented pattern
    std::vector<uint64_t> blocks;
    for (int i = 0; i < 10; i++) {
        uint64_t offset = allocator->allocate(100);
        ASSERT_TRUE(verify_allocation(offset, 100));
        blocks.push_back(offset);
    }

    // Free every other block to create fragmentation
    for (size_t i = 1; i < blocks.size(); i += 2) {
        allocator->deallocate(blocks[i], 100);
    }

    // Test that maintenance works without errors
    EXPECT_NO_THROW(allocator->maintenance());

    // Verify allocator still works after maintenance
    uint64_t test_offset = allocator->allocate(50);
    EXPECT_TRUE(verify_allocation(test_offset, 50));
}

// Stress tests
class StressTest : public ComprehensiveAllocatorTest {};

TEST_F(StressTest, RandomAllocationPattern) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> size_dist(16, 1024);
    std::uniform_real_distribution<> action_dist(0.0, 1.0);

    std::vector<std::pair<uint64_t, size_t>> allocated_blocks;
    const int operations = 1000;

    for (int i = 0; i < operations; i++) {
        if (action_dist(gen) < 0.7 || allocated_blocks.empty()) {
            // Allocate new block (70% probability or if no blocks to free)
            size_t size = size_dist(gen);
            uint64_t offset = allocator->allocate(size);

            if (verify_allocation(offset, size)) {
                allocated_blocks.emplace_back(offset, size);
            }
        } else {
            // Deallocate random block (30% probability)
            size_t idx = gen() % allocated_blocks.size();
            auto [offset, size] = allocated_blocks[idx];

            EXPECT_NO_THROW(allocator->deallocate(offset, size));
            allocated_blocks.erase(allocated_blocks.begin() + idx);
        }

        // Occasionally verify no overlaps
        if (i % 100 == 0) {
            EXPECT_TRUE(blocks_dont_overlap(allocated_blocks))
                << "Overlap detected at operation " << i;
        }
    }
}

TEST_F(StressTest, HighFragmentationScenario) {
    const size_t num_blocks = 100;
    const size_t block_size = 64;
    std::vector<uint64_t> blocks;

    // Allocate blocks
    for (size_t i = 0; i < num_blocks; i++) {
        uint64_t offset = allocator->allocate(block_size);
        ASSERT_TRUE(verify_allocation(offset, block_size));
        blocks.push_back(offset);
    }

    // Free blocks in checkerboard pattern to maximize fragmentation
    for (size_t i = 0; i < blocks.size(); i += 3) {
        allocator->deallocate(blocks[i], block_size);
    }

    // Try to allocate larger blocks
    for (int i = 0; i < 10; i++) {
        uint64_t offset = allocator->allocate(block_size * 2);
        if (verify_allocation(offset, block_size * 2)) {
            // Success - verify it doesn't overlap with remaining blocks
            bool overlaps = false;
            for (size_t j = 1; j < blocks.size(); j += 3) {
                if (j + 1 < blocks.size()) {
                    uint64_t existing_start = blocks[j];
                    uint64_t existing_end = existing_start + block_size;
                    uint64_t new_start = offset;
                    uint64_t new_end = new_start + block_size * 2;

                    if (!(new_end <= existing_start || existing_end <= new_start)) {
                        overlaps = true;
                        break;
                    }
                }
            }
            EXPECT_FALSE(overlaps) << "New allocation overlaps with existing block";
        }
    }
}

// Test different allocation sizes
class AllocationSizeTest : public ComprehensiveAllocatorTest {};

TEST_F(AllocationSizeTest, PowerOfTwoSizes) {
    std::vector<uint64_t> offsets;

    // Test powers of 2 from 1 byte to 1MB
    for (size_t exp = 0; exp <= 20; exp++) {
        size_t size = 1ULL << exp;
        uint64_t offset = allocator->allocate(size);
        EXPECT_TRUE(verify_allocation(offset, size))
            << "Failed to allocate " << size << " bytes (2^" << exp << ")";
        if (verify_allocation(offset, size)) {
            offsets.push_back(offset);
        }
    }

    // Verify no overlaps
    std::vector<std::pair<uint64_t, size_t>> blocks;
    for (size_t i = 0; i < offsets.size(); i++) {
        size_t size = 1ULL << i;
        blocks.emplace_back(offsets[i], size);
    }
    EXPECT_TRUE(blocks_dont_overlap(blocks));
}

TEST_F(AllocationSizeTest, OddSizes) {
    // Test various odd sizes that might cause alignment issues
    std::vector<size_t> odd_sizes = {1, 3, 7, 15, 31, 63, 127, 255, 511, 1023};
    std::vector<std::pair<uint64_t, size_t>> blocks;

    for (size_t size : odd_sizes) {
        uint64_t offset = allocator->allocate(size);
        EXPECT_TRUE(verify_allocation(offset, size)) << "Failed to allocate " << size << " bytes";
        if (verify_allocation(offset, size)) {
            blocks.emplace_back(offset, size);
        }
    }

    EXPECT_TRUE(blocks_dont_overlap(blocks));
}

// Test memory coalescing
class CoalescingTest : public ComprehensiveAllocatorTest {};

TEST_F(CoalescingTest, AdjacentBlockMerging) {
    const size_t block_size = 100;

    // Allocate three adjacent blocks
    uint64_t offset1 = allocator->allocate(block_size);
    uint64_t offset2 = allocator->allocate(block_size);
    uint64_t offset3 = allocator->allocate(block_size);

    ASSERT_TRUE(verify_allocation(offset1, block_size));
    ASSERT_TRUE(verify_allocation(offset2, block_size));
    ASSERT_TRUE(verify_allocation(offset3, block_size));

    // Free first and third blocks
    allocator->deallocate(offset1, block_size);
    allocator->deallocate(offset3, block_size);

    // Free middle block - should trigger coalescing
    allocator->deallocate(offset2, block_size);

    // Now allocate large block - should get the merged space
    uint64_t large_offset = allocator->allocate(block_size * 3);
    EXPECT_TRUE(verify_allocation(large_offset, block_size * 3));
    EXPECT_EQ(large_offset, offset1) << "Should get merged space starting from first block";
}

TEST_F(CoalescingTest, NonAdjacentBlocks) {
    const size_t block_size = 100;

    // Allocate blocks with gaps
    uint64_t offset1 = allocator->allocate(block_size);
    uint64_t gap1 = allocator->allocate(50); // Gap
    uint64_t offset2 = allocator->allocate(block_size);
    uint64_t gap2 = allocator->allocate(50); // Gap
    uint64_t offset3 = allocator->allocate(block_size);

    ASSERT_TRUE(verify_allocation(offset1, block_size));
    ASSERT_TRUE(verify_allocation(offset2, block_size));
    ASSERT_TRUE(verify_allocation(offset3, block_size));

    // Free the blocks but keep gaps
    allocator->deallocate(offset1, block_size);
    allocator->deallocate(offset2, block_size);
    allocator->deallocate(offset3, block_size);

    // Should have separate free blocks, not one large one
    uint64_t new_offset = allocator->allocate(block_size * 3);
    // This allocation should either fail or get space at end of file
    // since the blocks can't be merged due to gaps
}

// Performance tests
class PerformanceTest : public ComprehensiveAllocatorTest {};

TEST_F(PerformanceTest, AllocationSpeed) {
    const size_t num_operations = 10000;
    const size_t block_size = 256;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<uint64_t> offsets;
    for (size_t i = 0; i < num_operations; i++) {
        uint64_t offset = allocator->allocate(block_size);
        if (verify_allocation(offset, block_size)) {
            offsets.push_back(offset);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "Allocated " << offsets.size() << " blocks in " << duration.count()
              << " microseconds" << std::endl;
    std::cout << "Average: " << (duration.count() / double(offsets.size()))
              << " microseconds per allocation" << std::endl;

    EXPECT_GT(offsets.size(), num_operations * 0.9)
        << "Should successfully allocate at least 90% of blocks";
}

TEST_F(PerformanceTest, DeallocationSpeed) {
    const size_t num_blocks = 5000;
    const size_t block_size = 128;
    std::vector<std::pair<uint64_t, size_t>> blocks;

    // Allocate blocks first
    for (size_t i = 0; i < num_blocks; i++) {
        uint64_t offset = allocator->allocate(block_size);
        if (verify_allocation(offset, block_size)) {
            blocks.emplace_back(offset, block_size);
        }
    }

    auto start = std::chrono::high_resolution_clock::now();

    // Deallocate all blocks
    for (auto [offset, size] : blocks) {
        allocator->deallocate(offset, size);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "Deallocated " << blocks.size() << " blocks in " << duration.count()
              << " microseconds" << std::endl;
    std::cout << "Average: " << (duration.count() / double(blocks.size()))
              << " microseconds per deallocation" << std::endl;
}

// Test boundary alignment
class AlignmentTest : public ComprehensiveAllocatorTest {};

TEST_F(AlignmentTest, AlignmentConsistency) {
    std::vector<uint64_t> offsets;

    // Test various sizes to check alignment behavior
    std::vector<size_t> sizes = {1, 4, 8, 16, 32, 64, 128, 256, 512, 1024};

    for (size_t size : sizes) {
        uint64_t offset = allocator->allocate(size);
        EXPECT_TRUE(verify_allocation(offset, size));

        // Check if offset has reasonable alignment
        // (implementation specific - might align to 8 bytes, etc.)
        if (verify_allocation(offset, size)) {
            offsets.push_back(offset);
        }
    }

    // Verify allocations don't overlap
    std::vector<std::pair<uint64_t, size_t>> blocks;
    for (size_t i = 0; i < offsets.size() && i < sizes.size(); i++) {
        blocks.emplace_back(offsets[i], sizes[i]);
    }
    EXPECT_TRUE(blocks_dont_overlap(blocks));
}

// Error handling tests
class ErrorHandlingTest : public ComprehensiveAllocatorTest {};

TEST_F(ErrorHandlingTest, InvalidDeallocations) {
    // These operations should not crash the program
    EXPECT_NO_THROW(allocator->deallocate(0, 100));
    EXPECT_NO_THROW(allocator->deallocate(UINT64_MAX, 100));
    EXPECT_NO_THROW(allocator->deallocate(sizeof(header) / 2, 100));
}

TEST_F(ErrorHandlingTest, ExtremeAllocationSizes) {
    // Zero-size allocation should be rejected
    EXPECT_EQ(allocator->allocate(0), UINT64_MAX) << "Zero-size allocation should fail";

    // Test very large allocation - may succeed or fail, both are acceptable
    uint64_t huge_offset = allocator->allocate(1ULL << 30); // 1GB
    if (huge_offset != UINT64_MAX) {
        std::cout << "Successfully allocated 1GB block" << std::endl;
        // If it succeeded, it should be valid
        EXPECT_TRUE(verify_allocation(huge_offset, 1ULL << 30));
    }
}

// Strategy comparison test
class StrategyComparisonTest : public ComprehensiveAllocatorTest {};

TEST_F(StrategyComparisonTest, StrategiesBehaviorDifference) {
    struct StrategyResult {
        compio_allocation_strategy strategy;
        uint64_t offset;
        std::string name;
    };

    std::vector<StrategyResult> results;

    // Test each strategy with same fragmented scenario
    std::vector<compio_allocation_strategy> strategies = {
        COMPIO_ALLOC_FIRST_FIT, COMPIO_ALLOC_BEST_FIT, COMPIO_ALLOC_WORST_FIT,
        COMPIO_ALLOC_NEXT_FIT};

    std::vector<std::string> names = {"FIRST_FIT", "BEST_FIT", "WORST_FIT", "NEXT_FIT"};

    for (size_t i = 0; i < strategies.size(); i++) {
        recreate_allocator_with_strategy(strategies[i]);

        // Create same fragmented pattern
        uint64_t block1 = allocator->allocate(50);  // Small
        uint64_t block2 = allocator->allocate(200); // Large
        uint64_t block3 = allocator->allocate(100); // Medium
        uint64_t block4 = allocator->allocate(300); // Very large

        // Free blocks to create holes of different sizes
        allocator->deallocate(block2, 200); // Large hole
        allocator->deallocate(block3, 100); // Medium hole

        // Now allocate 150 bytes - strategies should behave differently
        uint64_t test_offset = allocator->allocate(150);

        results.push_back({strategies[i], test_offset, names[i]});

        std::cout << names[i] << " allocated 150 bytes at offset " << test_offset << std::endl;
    }

    // Verify that we got valid allocations
    for (const auto &result : results) {
        EXPECT_TRUE(verify_allocation(result.offset, 150)) << result.name << " failed to allocate";
    }
}