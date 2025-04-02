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

} // namespace compio