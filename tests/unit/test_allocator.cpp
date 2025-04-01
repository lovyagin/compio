#include "allocator.hpp"
#include "compio.h"
#include <gtest/gtest.h>
#include <compio_file.hpp>
#include <cstdint>

namespace compio {

class MockArchive : public compio_archive {
public:
    compio_config config_storage;
    uint64_t file_size = 0;
    FILE* file = nullptr;

    MockArchive() {
        config = &config_storage;
        config_storage.allocation_strategy = allocation_strategy::FIRST_FIT;
        config_storage.fragmentation_threshold = 50;
        config_storage.fill_holes_with_zeros = false;
    }
};

class AllocatorTest : public ::testing::Test {
protected:
    MockArchive archive;
    block_allocator allocator;

    AllocatorTest() : allocator(&archive) {}

    void SetUp() override {
        archive.file_size = 0;
    }
};

TEST_F(AllocatorTest, FirstFitStrategy) {
    archive.config->allocation_strategy = static_cast<compio_allocation_strategy>(allocation_strategy::FIRST_FIT);
    allocator.allocate(100);
    allocator.deallocate(0, 100);
    uint64_t offset = allocator.allocate(50);
    ASSERT_EQ(offset, 0);
}

TEST_F(AllocatorTest, BestFitStrategy) {
    archive.config->allocation_strategy = allocation_strategy::BEST_FIT;
    allocator.allocate(200);
    allocator.deallocate(0, 200);
    allocator.allocate(50);
    uint64_t offset = allocator.allocate(100);
    ASSERT_EQ(offset, 50);
}

TEST_F(AllocatorTest, WorstFitStrategy) {
    archive.config->allocation_strategy = allocation_strategy::WORST_FIT;
    allocator.allocate(100);
    allocator.allocate(200);
    allocator.deallocate(0, 100);
    allocator.deallocate(100, 200);
    uint64_t offset = allocator.allocate(50);
    ASSERT_EQ(offset, 0);
}

TEST_F(AllocatorTest, NextFitStrategy) {
    archive.config->allocation_strategy = allocation_strategy::NEXT_FIT;
    allocator.allocate(100);
    allocator.allocate(100);
    allocator.allocate(100);
    allocator.deallocate(100, 100);
    uint64_t offset = allocator.allocate(50);
    ASSERT_EQ(offset, 100);
}

TEST_F(AllocatorTest, ZeroSizeAllocation) {
    uint64_t offset = allocator.allocate(0);
    ASSERT_EQ(offset, UINT64_MAX);
}

TEST_F(AllocatorTest, DeallocateNonExistentBlock) {
    allocator.deallocate(1000, 500);
    SUCCEED();
}

TEST_F(AllocatorTest, DefragmentationTrigger) {
    allocator.allocate(100);
    allocator.allocate(200);
    allocator.deallocate(0, 100);
    allocator.deallocate(100, 200);
    ASSERT_GT(allocator.get_fragmentation(), 50);
    allocator.maintenance();
    ASSERT_LT(allocator.get_fragmentation(), 50);
}

} // namespace compio