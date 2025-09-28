#include "allocator.hpp"
#include "compio_file.hpp"
#include "utils.hpp"
#include <gtest/gtest.h>

using namespace compio;

class BasicAllocatorTest : public ::testing::Test {
protected:
    char fn[32];
    FILE* file;
    compio_archive* archive;
    block_allocator* allocator;

    void SetUp() override {
        strcpy(fn, "/tmp/compio_alloc_XXXXXX");
        int fd = mkstemp(fn);
        if (fd != -1) close(fd);

        file = fopen(fn, "w+");
        ASSERT_TRUE(file != nullptr);

        auto* config = new compio_config();
        config->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
        config->fragmentation_threshold = 30;
        config->fill_holes_with_zeros = false;

        archive = new compio_archive(file, mode_bit::w | mode_bit::r, config);
        allocator = new block_allocator(archive);
    }

    void TearDown() override {
        if (allocator) delete allocator;
        if (archive) delete archive;
        if (file) fclose(file);
        remove(fn);
    }

    // Helper to verify block allocation
    bool verify_allocation(uint64_t offset, size_t size) {
        return offset != UINT64_MAX && offset >= sizeof(header);
    }
};

TEST_F(BasicAllocatorTest, BasicAllocation) {
    uint64_t offset = allocator->allocate(100);
    EXPECT_TRUE(verify_allocation(offset, 100));
}

TEST_F(BasicAllocatorTest, AllocationAndDeallocation) {
    const size_t alloc_size = 100;

    // First allocation
    uint64_t offset = allocator->allocate(alloc_size);
    ASSERT_TRUE(verify_allocation(offset, alloc_size));
    uint64_t initial_offset = offset;
    uint64_t size_after_first = archive->header->file_size;

    // Deallocate
    allocator->deallocate(offset, alloc_size);

    // Second allocation of same size should reuse the space
    uint64_t new_offset = allocator->allocate(alloc_size);
    ASSERT_TRUE(verify_allocation(new_offset, alloc_size));

    EXPECT_EQ(new_offset, initial_offset)
        << "Expected new allocation to reuse freed space at offset "
        << initial_offset << " but got " << new_offset;
    EXPECT_EQ(archive->header->file_size, size_after_first)
        << "File size changed after reallocation";
}

TEST_F(BasicAllocatorTest, ZeroSizeAllocation) {
    EXPECT_EQ(allocator->allocate(0), UINT64_MAX);
}

TEST_F(BasicAllocatorTest, MultipleAllocations) {
    std::vector<uint64_t> offsets;
    const size_t block_size = 100;

    // Allocate multiple blocks
    for (int i = 0; i < 3; i++) {
        uint64_t offset = allocator->allocate(block_size);
        ASSERT_TRUE(verify_allocation(offset, block_size));
        offsets.push_back(offset);
    }

    // Verify allocations are unique and properly ordered
    for (size_t i = 0; i < offsets.size() - 1; i++) {
        EXPECT_LT(offsets[i], offsets[i + 1])
            << "Allocations should have increasing offsets";
        EXPECT_GE(offsets[i + 1] - offsets[i], block_size)
            << "Blocks should not overlap";
    }
}

TEST_F(BasicAllocatorTest, FragmentedDeallocation) {
    // Allocate blocks with different sizes to prevent accidental merging
    const size_t size1 = 50;
    const size_t size2 = 100;
    const size_t size3 = 75;

    uint64_t offset1 = allocator->allocate(size1);
    uint64_t offset2 = allocator->allocate(size2);
    uint64_t offset3 = allocator->allocate(size3);

    ASSERT_TRUE(verify_allocation(offset1, size1));
    ASSERT_TRUE(verify_allocation(offset2, size2));
    ASSERT_TRUE(verify_allocation(offset3, size3));

    // Remember the middle block's position
    uint64_t middle_pos = offset2;

    // Free only the middle block
    allocator->deallocate(offset2, size2);

    // Allocate a block that fits in the freed space
    uint64_t new_offset = allocator->allocate(size2);
    EXPECT_EQ(new_offset, middle_pos)
        << "New allocation should reuse the freed middle block";

    // Original blocks should still be valid
    EXPECT_NE(new_offset, offset1);
    EXPECT_NE(new_offset, offset3);
}