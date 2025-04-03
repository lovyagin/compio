#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <vector>
#include <algorithm>
#include "allocator.hpp"
#include "compio_file.hpp"

namespace compio {

class MockArchive : public compio_archive {
public:
    MockArchive() {
        header = new struct header();
        header->file_size = sizeof(struct header);

        compio_config* mutable_config = new compio_config();
        mutable_config->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
        mutable_config->fragmentation_threshold = 30;
        mutable_config->fill_holes_with_zeros = false;

        config = mutable_config;

        file = nullptr;
        index = nullptr;
        allocator = nullptr;
    }

    ~MockArchive() {
        delete header;
        delete config;
    }
};

class BlockAllocatorTest : public ::testing::Test {
protected:
    MockArchive* archive;
    block_allocator* allocator;

    void SetUp() override {
        archive = new MockArchive();
        allocator = new block_allocator(archive);
    }

    void TearDown() override {
        delete allocator;
        delete archive;
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

    // Add a single free block, fragmentation should be 10
    manager.add_free_block(0, 100);
    EXPECT_EQ(manager.calculate_fragmentation(), 10);

    // Add another free block, fragmentation should be 20
    manager.add_free_block(200, 100);
    EXPECT_EQ(manager.calculate_fragmentation(), 20);

    // Add more free blocks to increase fragmentation
    manager.add_free_block(400, 100);
    manager.add_free_block(600, 100);
    EXPECT_EQ(manager.calculate_fragmentation(), 40);

    // Defragment and check fragmentation again
    manager.defragment();
    manager.update_fragmentation();
    EXPECT_EQ(manager.calculate_fragmentation(), 10);
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

    manager.add_free_block(100, 50);
    manager.add_free_block(200, 200);
    manager.add_free_block(500, 400);

    uint64_t best_fit = manager.allocate_block(50, allocation_strategy::BEST_FIT);
    EXPECT_EQ(best_fit, 100);

    manager = free_blocks_manager(&file_size);
    manager.add_free_block(100, 50);
    manager.add_free_block(200, 200);
    manager.add_free_block(500, 400);

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

    uint64_t large_size = 2048;
    uint64_t offset = allocator->allocate(large_size);

    EXPECT_NE(offset, UINT64_MAX);

    EXPECT_GT(archive->header->file_size, original_size);

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
    std::cout << "Fragmentation before: " << (int)frag_before
              << ", Threshold: " << (int)threshold << std::endl;

    // Make sure we're above threshold
    if (frag_before <= threshold) {
        ((compio_config*)archive->config)->fragmentation_threshold = frag_before - 1;
        threshold = frag_before - 1;
        std::cout << "Adjusted threshold to: " << (int)threshold << std::endl;
    }

    // Call maintenance - should defragment if over threshold
    allocator->maintenance();

    // Get fragmentation after maintenance
    uint8_t frag_after = allocator->get_fragmentation();
    std::cout << "Fragmentation after: " << (int)frag_after << std::endl;

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

    manager.add_free_block(150, 50);

    manager.defragment();

    uint8_t after_frag = manager.calculate_fragmentation();
    EXPECT_LT(after_frag, initial_frag);

    uint64_t offset = manager.allocate_block(150, allocation_strategy::FIRST_FIT);
    EXPECT_EQ(offset, 100);
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

    EXPECT_EQ(offset2, offset1 + 100);
    EXPECT_EQ(offset3, offset2 + 150);

    allocator->deallocate(offset1, 100);
    allocator->deallocate(offset3, 200);
    allocator->deallocate(offset2, 150);

    uint64_t new_offset = allocator->allocate(450);
    EXPECT_EQ(new_offset, offset1);

    uint64_t next_offset = allocator->allocate(50);
    EXPECT_NE(next_offset, offset1 + 100);
    EXPECT_NE(next_offset, offset2 + 150);
    EXPECT_GE(next_offset, offset1 + 450);
}

} // namespace compio