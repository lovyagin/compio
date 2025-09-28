#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <vector>
#include <algorithm>
#include "../include/allocator.hpp"
#include "../include/compio_file.hpp"
#include "../include/utils.hpp"
#include "random"

using namespace compio;

class MockArchive : public compio_archive {
public:
    MockArchive(FILE* file, uint64_t mode_b, const compio_config* config) : compio_archive(file, mode_b, config) {
        header = smart_infile_object<compio::header>(file, 0, new struct header());

        index = nullptr;
        allocator = nullptr;
    }

    ~MockArchive() {
        delete config;
    }
};

class BlockAllocatorTest : public ::testing::Test {
protected:
    char fn[32];
    FILE* file;
    MockArchive* archive;
    block_allocator* allocator;

    void SetUp() override {
        strcpy(fn, "/tmp/compio_alloc_XXXXXX");
        int fd = mkstemp(fn);
        if (fd != -1) close(fd);

        file = fopen(fn, "w+");
        if (!file) {
            throw std::runtime_error("failed to create/open file for testing");
        }

        compio_config* mutable_config = new compio_config();
        mutable_config->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
        mutable_config->fragmentation_threshold = 30;
        mutable_config->fill_holes_with_zeros = false;

        archive = new MockArchive(file, mode_bit::w & mode_bit::r, mutable_config);
        allocator = new block_allocator(archive);
    }

    void TearDown() override {
        delete allocator;
        delete archive;

        fclose(file);
        remove(fn);
    }
};

TEST(FreeBlocksManagerTest, AddFreeBlock) {
    uint64_t file_size = 1024;
    free_blocks_manager manager(&file_size);

    manager.add_free_block(100, 50);
    manager.add_free_block(150, 50);
    manager.add_free_block(250, 50);

    uint64_t offset = manager.allocate_block(50, allocation_strategy::FIRST_FIT);
    EXPECT_EQ(offset, 100);

    offset = manager.allocate_block(50, allocation_strategy::FIRST_FIT);
    EXPECT_EQ(offset, 150);

    offset = manager.allocate_block(50, allocation_strategy::FIRST_FIT);
    EXPECT_EQ(offset, 250);
}

TEST(FreeBlocksManagerTest, CalculateFragmentation) {
    uint64_t file_size = 1000;
    compio::free_blocks_manager manager(&file_size);

    // Initially, no free blocks, fragmentation should be 0
    EXPECT_EQ(manager.calculate_fragmentation(), 0);

    // Add a single free block, fragmentation should be 0 (ideal state)
    manager.add_free_block(0, 100);
    EXPECT_EQ(manager.calculate_fragmentation(), 0);

    // Add another free block, fragmentation should be ~11%
    // (2-1)/(10-1)*100 = 1/9*100 = 11.11...%
    manager.add_free_block(200, 100);
    EXPECT_EQ(manager.calculate_fragmentation(), 11);

    // Add more free blocks to increase fragmentation
    // (4-1)/(10-1)*100 = 3/9*100 = 33.33...%
    manager.add_free_block(400, 100);
    manager.add_free_block(600, 100);
    EXPECT_EQ(manager.calculate_fragmentation(), 33);

    // Defragment should consolidate blocks if they can be merged
    manager.defragment();
    manager.update_fragmentation();

    // If blocks can be consolidated, fragmentation should decrease
    // If all blocks are separate (cannot be merged), fragmentation remains the same
    // Given the positions (0, 200, 400, 600) they cannot be merged, so still 33%
    EXPECT_EQ(manager.calculate_fragmentation(), 33);
}

TEST(FreeBlocksManagerTest, BasicAllocation) {
    uint64_t file_size = 1024;
    free_blocks_manager manager(&file_size);

    manager.add_free_block(100, 100);

    uint64_t offset = manager.allocate_block(50, allocation_strategy::FIRST_FIT);
    EXPECT_EQ(offset, 100);

    uint64_t offset2 = manager.allocate_block(50, allocation_strategy::FIRST_FIT);
    EXPECT_EQ(offset2, 150);

    uint64_t offset3 = manager.allocate_block(100, allocation_strategy::FIRST_FIT);
    EXPECT_EQ(offset3, UINT64_MAX);
}

TEST(FreeBlocksManagerTest, AllocationStrategies) {
    uint64_t file_size = 1024;
    free_blocks_manager manager(&file_size);

    manager.add_free_block(100, 50);   // Small block
    manager.add_free_block(200, 200);  // Medium block
    manager.add_free_block(500, 400);  // Large block

    // Test BEST_FIT: Should select the smallest block that fits the request (50-byte block)
    uint64_t best_fit = manager.allocate_block(50, allocation_strategy::BEST_FIT);
    EXPECT_EQ(best_fit, 100);

    manager = free_blocks_manager(&file_size);
    manager.add_free_block(100, 50);   // Small block
    manager.add_free_block(200, 200);  // Medium block
    manager.add_free_block(500, 400);  // Large block

    // Test WORST_FIT: Should select the largest block available (400-byte block)
    uint64_t worst_fit = manager.allocate_block(50, allocation_strategy::WORST_FIT);
    EXPECT_EQ(worst_fit, 500);
}

TEST(FreeBlocksManagerTest, FirstFitStrategy) {
    uint64_t file_size = 1024;
    free_blocks_manager manager(&file_size);

    manager.add_free_block(100, 50);   // Block 1
    manager.add_free_block(200, 100);  // Block 2 (larger)
    manager.add_free_block(400, 200);  // Block 3 (largest)
    manager.add_free_block(700, 75);   // Block 4

    uint64_t offset1 = manager.allocate_block(40, allocation_strategy::FIRST_FIT);
    EXPECT_EQ(offset1, 100);

    uint64_t offset2 = manager.allocate_block(10, allocation_strategy::FIRST_FIT);
    EXPECT_EQ(offset2, 140);

    uint64_t offset3 = manager.allocate_block(60, allocation_strategy::FIRST_FIT);
    EXPECT_EQ(offset3, 200);

    uint64_t offset4 = manager.allocate_block(40, allocation_strategy::FIRST_FIT);
    EXPECT_EQ(offset4, 260);

    uint64_t offset5 = manager.allocate_block(150, allocation_strategy::FIRST_FIT);
    EXPECT_EQ(offset5, 400);

    uint64_t offset6 = manager.allocate_block(300, allocation_strategy::FIRST_FIT);
    EXPECT_EQ(offset6, UINT64_MAX);
}

TEST(FreeBlocksManagerTest, BestFitStrategy) {
    uint64_t file_size = 1024;
    free_blocks_manager manager(&file_size);

    manager.add_free_block(100, 50);   // Small block
    manager.add_free_block(200, 100);  // Medium block
    manager.add_free_block(400, 200);  // Large block
    manager.add_free_block(700, 60);   // Another small-ish block

    uint64_t offset1 = manager.allocate_block(40, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset1, 100);

    uint64_t offset2 = manager.allocate_block(55, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset2, 700);

    uint64_t offset3 = manager.allocate_block(90, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset3, 200);

    uint64_t offset4 = manager.allocate_block(150, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset4, 400);

    uint64_t offset5 = manager.allocate_block(100, allocation_strategy::BEST_FIT);
    EXPECT_EQ(offset5, UINT64_MAX);
}

TEST(FreeBlocksManagerTest, WorstFitStrategy) {
    uint64_t file_size = 1024;
    free_blocks_manager manager(&file_size);

    manager.add_free_block(100, 50);   // Small block
    manager.add_free_block(200, 100);  // Medium block
    manager.add_free_block(400, 200);  // Large block
    manager.add_free_block(700, 150);  // Another medium block

    uint64_t offset1 = manager.allocate_block(40, allocation_strategy::WORST_FIT);
    EXPECT_EQ(offset1, 400);

    uint64_t offset2 = manager.allocate_block(55, allocation_strategy::WORST_FIT);
    EXPECT_EQ(offset2, 440);

    uint64_t offset3 = manager.allocate_block(90, allocation_strategy::WORST_FIT);
    EXPECT_EQ(offset3, 700);

    uint64_t offset4 = manager.allocate_block(30, allocation_strategy::WORST_FIT);
    EXPECT_EQ(offset4, 495);

    uint64_t offset5 = manager.allocate_block(100, allocation_strategy::WORST_FIT);
    EXPECT_EQ(offset5, 200);
}

TEST(FreeBlocksManagerTest, NextFitStrategy) {
    uint64_t file_size = 1024;
    free_blocks_manager manager(&file_size);

    manager.add_free_block(100, 50);   // Block 1
    manager.add_free_block(200, 50);   // Block 2
    manager.add_free_block(300, 50);   // Block 3

    uint64_t offset1 = manager.allocate_block(40, allocation_strategy::NEXT_FIT);
    EXPECT_EQ(offset1, 100);

    uint64_t offset2 = manager.allocate_block(30, allocation_strategy::NEXT_FIT);
    EXPECT_EQ(offset2, 200);

    uint64_t offset3 = manager.allocate_block(20, allocation_strategy::NEXT_FIT);
    EXPECT_EQ(offset3, 300);

    uint64_t offset4 = manager.allocate_block(10, allocation_strategy::NEXT_FIT);
    EXPECT_EQ(offset4, 140);
}

TEST_F(BlockAllocatorTest, AllocateWithFileExtension) {
    uint64_t original_size = archive->header->file_size;

    // Request allocation larger than available free space
    // This should trigger file extension
    uint64_t large_size = 2048;
    uint64_t offset = allocator->allocate(large_size);

    // Allocation should succeed (not return UINT64_MAX)
    EXPECT_NE(offset, UINT64_MAX);

    // File size should have increased to accommodate the allocation
    EXPECT_GT(archive->header->file_size, original_size);

    // New file size should be sufficient to contain the allocation
    EXPECT_GE(archive->header->file_size, offset + large_size);
}

TEST_F(BlockAllocatorTest, DeallocateWithZeroFilling) {
    ((compio_config*)archive->config)->fill_holes_with_zeros = true;

    uint64_t size = 100;
    uint64_t offset = allocator->allocate(size);

    allocator->deallocate(offset, size);

    uint64_t new_offset = allocator->allocate(size);
    EXPECT_EQ(new_offset, offset);
}

TEST_F(BlockAllocatorTest, MaintenanceThresholdBased) {
    // Set a specific fragmentation threshold
    uint8_t threshold = 20;
    ((compio_config*)archive->config)->fragmentation_threshold = threshold;

    // Create several allocations in sequence
    std::vector<std::pair<uint64_t, uint64_t>> allocations;
    for (int i = 0; i < 10; i++) {
        uint64_t size = 50;  // Fixed size for predictability
        uint64_t offset = allocator->allocate(size);
        allocations.push_back({offset, size});
    }

    // Free adjacent blocks to create mergeable free regions
    allocator->deallocate(allocations[2].first, allocations[2].second);
    allocator->deallocate(allocations[3].first, allocations[3].second);
    allocator->deallocate(allocations[5].first, allocations[5].second);
    allocator->deallocate(allocations[6].first, allocations[6].second);
    allocator->deallocate(allocations[8].first, allocations[8].second);

    // Get current fragmentation level
    uint8_t frag_before = allocator->get_fragmentation();

    // Print the values for debugging
    // std::cout << "Fragmentation before: " << (int)frag_before
    //        << ", Threshold: " << (int)threshold << std::endl;

    // Make sure we're above threshold
    if (frag_before <= threshold) {
        ((compio_config*)archive->config)->fragmentation_threshold = frag_before - 1;
        threshold = frag_before - 1;
        // std::cout << "Adjusted threshold to: " << (int)threshold << std::endl;
    }

    allocator->maintenance();

    uint8_t frag_after = allocator->get_fragmentation();
    // std::cout << "Fragmentation after: " << (int)frag_after << std::endl;

    // If fragmentation was above threshold, it should be reduced
    if (frag_before > threshold) {
        EXPECT_LT(frag_after, frag_before);
    } else {
        // Otherwise it should remain the same
        EXPECT_EQ(frag_after, frag_before);
    }
}

TEST(FreeBlocksManagerTest, Defragmentation) {
    uint64_t file_size = 1024;
    free_blocks_manager manager(&file_size);

    manager.add_free_block(100, 50);
    manager.add_free_block(200, 50);
    manager.add_free_block(300, 50);

    uint8_t initial_frag = manager.calculate_fragmentation();

    // Add a block that can be merged with an existing one
    manager.add_free_block(150, 50);

    manager.defragment();

    // Fragmentation should decrease after merging blocks
    uint8_t after_frag = manager.calculate_fragmentation();
    EXPECT_LT(after_frag, initial_frag);

    // Should now be able to allocate a larger contiguous block
    uint64_t offset = manager.allocate_block(150, allocation_strategy::FIRST_FIT);
    EXPECT_EQ(offset, 100);  // Should get the merged block at offset 100
}

TEST_F(BlockAllocatorTest, BasicAllocation) {
    uint64_t offset = allocator->allocate(100);
    EXPECT_NE(offset, UINT64_MAX);
    EXPECT_GE(offset, sizeof(struct header));
}

TEST_F(BlockAllocatorTest, Deallocation) {
    uint64_t offset = allocator->allocate(100);
    uint64_t initial_size = archive->header->file_size;

    allocator->deallocate(offset, 100);

    uint64_t new_offset = allocator->allocate(100);
    EXPECT_EQ(new_offset, offset);

    EXPECT_EQ(archive->header->file_size, initial_size);
}

TEST_F(BlockAllocatorTest, Fragmentation) {
    std::vector<std::pair<uint64_t, uint64_t>> allocations;
    for (int i = 0; i < 10; i++) {
        uint64_t size = 50 + i * 10;
        uint64_t offset = allocator->allocate(size);
        allocations.push_back({offset, size});
    }

    for (size_t i = 0; i < allocations.size(); i += 2) {
        allocator->deallocate(allocations[i].first, allocations[i].second);
    }

    uint8_t frag = allocator->get_fragmentation();
    EXPECT_GT(frag, 0);

    allocator->maintenance();

    uint64_t offset = allocator->allocate(100);
    EXPECT_NE(offset, UINT64_MAX);
}

TEST_F(BlockAllocatorTest, EdgeCases) {
    uint64_t offset = allocator->allocate(0);
    EXPECT_EQ(offset, UINT64_MAX);

    allocator->deallocate(UINT64_MAX, 100);
    allocator->deallocate(100, 0);
}

TEST_F(BlockAllocatorTest, AllocateWithNoSuitableBlock) {
    uint64_t offset1 = allocator->allocate(100);
    EXPECT_NE(offset1, UINT64_MAX);

    allocator->deallocate(offset1, 100);

    uint64_t offset2 = allocator->allocate(200);
    EXPECT_NE(offset2, UINT64_MAX);

    EXPECT_GE(offset2, archive->header->file_size - 200);

    uint64_t all_space = allocator->allocate(100);
    EXPECT_EQ(all_space, offset1);

    uint64_t previous_size = archive->header->file_size;
    uint64_t offset3 = allocator->allocate(50);
    EXPECT_NE(offset3, UINT64_MAX);
    EXPECT_GE(offset3, previous_size);
    EXPECT_EQ(archive->header->file_size, previous_size + 50);
}

TEST_F(BlockAllocatorTest, DeallocateAdjacentBlocks) {
    uint64_t offset1 = allocator->allocate(100);
    uint64_t offset2 = allocator->allocate(150);
    uint64_t offset3 = allocator->allocate(200);

    // Verify blocks are contiguous
    EXPECT_EQ(offset2, offset1 + 100);
    EXPECT_EQ(offset3, offset2 + 150);

    // Deallocate in non-sequential order to test merging logic
    // First and third blocks are deallocated, then the middle one
    allocator->deallocate(offset1, 100);
    allocator->deallocate(offset3, 200);
    allocator->deallocate(offset2, 150);

    // All blocks should be merged into one contiguous free block
    // Verify by allocating a block that spans the entire freed region
    uint64_t new_offset = allocator->allocate(450);
    EXPECT_EQ(new_offset, offset1);

    uint64_t next_offset = allocator->allocate(50);
    EXPECT_NE(next_offset, offset1 + 100);
    EXPECT_NE(next_offset, offset2 + 150);
    EXPECT_GE(next_offset, offset1 + 450);
}

TEST_F(BlockAllocatorTest, ZeroSizeAllocationRequest) {
    uint64_t offset = allocator->allocate(0);
    EXPECT_EQ(offset, UINT64_MAX);

    uint64_t valid_offset = allocator->allocate(100);
    EXPECT_NE(valid_offset, UINT64_MAX);
}

TEST_F(BlockAllocatorTest, HighlyFragmentedAllocation) {
    std::vector<uint64_t> offsets;

    for (int i = 0; i < 10; i++) {
        uint64_t offset = allocator->allocate(10);
        offsets.push_back(offset);
        allocator->allocate(10);
    }

    for (auto offset : offsets) {
        allocator->deallocate(offset, 10);
    }

    uint64_t large_offset = allocator->allocate(15);
    EXPECT_NE(large_offset, UINT64_MAX);

    EXPECT_GE(large_offset, archive->header->file_size - 15);
}

TEST_F(BlockAllocatorTest, MaxSizeAllocation) {
    uint64_t max_size = UINT16_MAX;
    uint64_t offset = allocator->allocate(max_size);
    EXPECT_NE(offset, UINT64_MAX);
    EXPECT_GE(archive->header->file_size, offset + max_size);
}

TEST_F(BlockAllocatorTest, VerySmallAllocation) {
    uint64_t offset = allocator->allocate(1);
    EXPECT_NE(offset, UINT64_MAX);

    allocator->deallocate(offset, 1);
    uint64_t new_offset = allocator->allocate(1);
    EXPECT_EQ(new_offset, offset);
}

TEST_F(BlockAllocatorTest, InvalidSizeDeallocation) {
    uint64_t offset = allocator->allocate(100);
    uint64_t initial_frag = allocator->get_fragmentation();

    allocator->deallocate(offset, 50);

    uint64_t new_offset = allocator->allocate(100);
    EXPECT_NE(new_offset, offset);
}

TEST_F(BlockAllocatorTest, RepeatedDeallocation) {
    // First allocation
    uint64_t offset = allocator->allocate(100);
    uint64_t initial_frag = allocator->get_fragmentation();

    // First deallocation (valid)
    allocator->deallocate(offset, 100);
    uint64_t frag_after_first = allocator->get_fragmentation();

    // Second deallocation of the same block (should be ignored)
    // This tests the protection against double-free errors
    allocator->deallocate(offset, 100);

    // Fragmentation shouldn't change after invalid second deallocation
    EXPECT_EQ(allocator->get_fragmentation(), frag_after_first);

    // Should be able to allocate the same block again
    uint64_t new_offset = allocator->allocate(100);
    EXPECT_EQ(new_offset, offset);

    // Test partial overlapping deallocations
    allocator->deallocate(new_offset, 50);  // Deallocate first half
    allocator->deallocate(new_offset, 100); // Attempt to deallocate the whole block (partially overlaps)

    // Try reallocating to ensure we can still get the block
    uint64_t final_offset = allocator->allocate(100);
    EXPECT_NE(final_offset, UINT64_MAX);
}

TEST_F(BlockAllocatorTest, StressTest) {
    const int num_operations = 1000;
    std::vector<std::pair<uint64_t, uint64_t>> active_allocations;

    for (int i = 0; i < num_operations; i++) {
        // 70% chance to allocate, 30% chance to deallocate
        if (active_allocations.empty() || (rand() % 100) < 70) {
            // Allocate random size between 1-200 bytes
            uint64_t size = 1 + (rand() % 200);
            uint64_t offset = allocator->allocate(size);
            EXPECT_NE(offset, UINT64_MAX);
            active_allocations.push_back({offset, size});
        } else {
            // Deallocate a random existing allocation
            int index = rand() % active_allocations.size();
            allocator->deallocate(active_allocations[index].first,
                                  active_allocations[index].second);
            active_allocations.erase(active_allocations.begin() + index);
        }

        if (i % 100 == 0) {
            allocator->maintenance();
        }
    }

    for (const auto& [offset, size] : active_allocations) {
        allocator->deallocate(offset, size);
    }
}

TEST_F(BlockAllocatorTest, RandomAllocationPatterns) {
    std::vector<size_t> sizes = {8, 16, 32, 64, 128, 256, 512, 1024, 2048};
    std::vector<std::pair<uint64_t, uint64_t>> allocations;

    // First round: allocate with different sizes
    for (int i = 0; i < 20; i++) {
        size_t size_index = rand() % sizes.size();
        uint64_t size = sizes[size_index];
        uint64_t offset = allocator->allocate(size);
        EXPECT_NE(offset, UINT64_MAX);
        allocations.push_back({offset, size});
    }

    // Second round: deallocate in random order
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(allocations.begin(), allocations.end(), g);

    for (size_t i = 0; i < allocations.size() / 2; i++) {
        allocator->deallocate(allocations[i].first, allocations[i].second);
    }

    // Third round: allocate again with different sizes
    for (int i = 0; i < 10; i++) {
        size_t size_index = rand() % sizes.size();
        uint64_t size = sizes[size_index];
        uint64_t offset = allocator->allocate(size);
        EXPECT_NE(offset, UINT64_MAX);
    }

    uint8_t frag_before = allocator->get_fragmentation();
    allocator->maintenance();
    uint8_t frag_after = allocator->get_fragmentation();

    EXPECT_LE(frag_after, frag_before);
}

TEST_F(BlockAllocatorTest, FragmentationDegradation) {
    std::vector<uint8_t> fragmentation_values;
    std::vector<std::pair<uint64_t, uint64_t>> allocations;

    // First stage: allocate blocks
    for (int i = 0; i < 20; i++) {
        uint64_t size = 50 + (i % 5) * 10;
        uint64_t offset = allocator->allocate(size);
        allocations.push_back({offset, size});
        fragmentation_values.push_back(allocator->get_fragmentation());
    }

    // Second stage: deallocate every other block
    for (size_t i = 0; i < allocations.size(); i += 2) {
        allocator->deallocate(allocations[i].first, allocations[i].second);
        fragmentation_values.push_back(allocator->get_fragmentation());
    }

    // Third stage: allocate smaller blocks
    for (int i = 0; i < 10; i++) {
        uint64_t size = 20 + (i % 3) * 5;
        uint64_t offset = allocator->allocate(size);
        fragmentation_values.push_back(allocator->get_fragmentation());
    }

    // Check for increasing trend in fragmentation
    bool increasing_detected = false;
    for (size_t i = 1; i < fragmentation_values.size(); i++) {
        if (fragmentation_values[i] > fragmentation_values[i-1]) {
            increasing_detected = true;
            break;
        }
    }

    EXPECT_TRUE(increasing_detected);

    uint8_t before_maintenance = allocator->get_fragmentation();
    allocator->maintenance();
    uint8_t after_maintenance = allocator->get_fragmentation();

    EXPECT_LE(after_maintenance, before_maintenance);
}

TEST_F(BlockAllocatorTest, WorstCaseScenario) {
    std::vector<std::pair<uint64_t, uint64_t>> allocations;

    // Create highly interleaved pattern of small and large blocks
    for (int i = 0; i < 10; i++) {
        uint64_t small_offset = allocator->allocate(8);
        uint64_t large_offset = allocator->allocate(64);
        allocations.push_back({small_offset, 8});
        allocations.push_back({large_offset, 64});
    }

    // Free all small blocks - creates many small holes
    for (size_t i = 0; i < allocations.size(); i += 2) {
        allocator->deallocate(allocations[i].first, allocations[i].second);
    }

    // Try to allocate a medium block - should fail to find contiguous space
    uint64_t offset = allocator->allocate(32);
    EXPECT_NE(offset, UINT64_MAX); // Should still succeed but by extending file

    uint8_t frag = allocator->get_fragmentation();
    EXPECT_GT(frag, 0);

    allocator->maintenance();

    // After maintenance, should be able to reuse space more efficiently
    uint64_t new_offset = allocator->allocate(24);
    EXPECT_NE(new_offset, UINT64_MAX);
}

TEST(FreeBlocksManagerTest, Serialize) {
    uint64_t file_size = 1024;
    compio::free_blocks_manager manager(&file_size);

    manager.add_free_block(100, 50);
    manager.add_free_block(200, 100);
    manager.add_free_block(400, 150);

    // Serialize the free blocks
    std::vector<uint8_t> buffer;
    uint32_t serialized_size = manager.serialize(buffer);

    // Verify the serialized size
    EXPECT_EQ(serialized_size, sizeof(uint64_t) + 3 * 2 * sizeof(uint64_t));

    // Verify the buffer content
    const uint64_t* data = reinterpret_cast<const uint64_t*>(buffer.data());
    EXPECT_EQ(data[0], 3);
    EXPECT_EQ(data[1], 100);
    EXPECT_EQ(data[2], 50);
    EXPECT_EQ(data[3], 200);
    EXPECT_EQ(data[4], 100);
    EXPECT_EQ(data[5], 400);
    EXPECT_EQ(data[6], 150);
}

TEST(FreeBlocksManagerTest, Deserialize) {
    uint64_t file_size = 1024;
    compio::free_blocks_manager manager(&file_size);

    // Create a buffer with serialized data
    std::vector<uint8_t> buffer(sizeof(uint64_t) + 3 * 2 * sizeof(uint64_t));
    uint64_t* data = reinterpret_cast<uint64_t*>(buffer.data());
    data[0] = 3;
    data[1] = 100;
    data[2] = 50;
    data[3] = 200;
    data[4] = 100;
    data[5] = 400;
    data[6] = 150;

    // Deserialize the buffer
    EXPECT_TRUE(manager.deserialize(buffer.data(), static_cast<uint32_t>(buffer.size())));

    // Verify the deserialized free blocks by allocating them in order
    // Use WORST_FIT to predictably get the largest block first
    EXPECT_EQ(manager.allocate_block(150, allocation_strategy::WORST_FIT), 400);
    EXPECT_EQ(manager.allocate_block(100, allocation_strategy::WORST_FIT), 200);
    EXPECT_EQ(manager.allocate_block(50, allocation_strategy::WORST_FIT), 100);

    // All blocks should be allocated now
    EXPECT_EQ(manager.allocate_block(1, allocation_strategy::FIRST_FIT), UINT64_MAX);
}

TEST(FreeBlocksManagerTest, SerializeAndDeserialize) {
    uint64_t file_size = 1024;
    compio::free_blocks_manager manager(&file_size);

    // Add known blocks in a specific pattern
    manager.add_free_block(100, 50);   // Block 1: offset 100, size 50
    manager.add_free_block(200, 150);  // Block 2: offset 200, size 150
    manager.add_free_block(400, 75);   // Block 3: offset 400, size 75

    // Serialize to buffer
    std::vector<uint8_t> buffer;
    uint32_t size = manager.serialize(buffer);

    // Create a new manager and deserialize into it
    uint64_t new_file_size = 1024;
    compio::free_blocks_manager new_manager(&new_file_size);
    ASSERT_TRUE(new_manager.deserialize(buffer.data(), size));

    // Verify both managers have identical state by allocating in order of decreasing size
    EXPECT_EQ(manager.allocate_block(150, allocation_strategy::BEST_FIT),
              new_manager.allocate_block(150, allocation_strategy::BEST_FIT));

    EXPECT_EQ(manager.allocate_block(75, allocation_strategy::BEST_FIT),
              new_manager.allocate_block(75, allocation_strategy::BEST_FIT));

    EXPECT_EQ(manager.allocate_block(50, allocation_strategy::BEST_FIT),
              new_manager.allocate_block(50, allocation_strategy::BEST_FIT));

    // Both should be empty now
    EXPECT_EQ(manager.allocate_block(1, allocation_strategy::FIRST_FIT), UINT64_MAX);
    EXPECT_EQ(new_manager.allocate_block(1, allocation_strategy::FIRST_FIT), UINT64_MAX);
}
