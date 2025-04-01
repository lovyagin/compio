#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstdint>
#include "btree.hpp"
#include "allocator.hpp"

struct fake_compio_config {
    compio::allocation_strategy allocation_strategy;
    uint8_t fragmentation_threshold; // e.g. 100 to avoid defragmentation
    bool fill_holes_with_zeros;
};

struct fake_compio_header {
    uint64_t file_size;
};

struct fake_index {
    // Dummy get_range that returns no used blocks.
    void get_range(const compio::tree_key&, const compio::tree_key&, std::vector<std::pair<compio::tree_key, compio::tree_val>>& range) {
        range.clear();
    }
    // Always update successfully.
    bool update(const compio::tree_key&, const compio::tree_val&) {
        return true;
    }
};

// Minimal fake compio_archive structure.
struct fake_compio_archive {
    fake_compio_config* config;
    fake_compio_header* header;
    fake_index* index;
    FILE* file;
};

// Helper function to create a temporary file.
FILE* create_temp_file() {
    FILE* fp = std::tmpfile();
    if (!fp) {
        std::abort();
    }
    return fp;
}

// Test fixture to set up and tear down a fake archive.
class AllocatorTestFixture : public ::testing::Test {
protected:
    fake_compio_config config;
    fake_compio_header header;
    fake_index index;
    fake_compio_archive archive;
    compio::block_allocator* allocator;

    void SetUp() override {
        // Setup fake config: use FIRST_FIT, high threshold to avoid defragmentation,
        // and disable zero-filling.
        config.allocation_strategy = compio::allocation_strategy::FIRST_FIT;
        config.fragmentation_threshold = 100;
        config.fill_holes_with_zeros = false;
        header.file_size = sizeof(header);  // initial file size

        archive.config = &config;
        archive.header = &header;
        archive.index = &index;
        archive.file = create_temp_file();

        // Create block_allocator with the fake archive.
        allocator = new compio::block_allocator(reinterpret_cast<compio_archive*>(&archive));
    }

    void TearDown() override {
        delete allocator;
        if (archive.file) {
            fclose(archive.file);
        }
    }
};

// Test that allocation increases file size when no free block is available.
TEST_F(AllocatorTestFixture, AllocateIncreaseFileSize) {
    uint64_t allocSize = 256;
    uint64_t offset = allocator->allocate(allocSize);
    // Since there are no free blocks, allocation should start at initial file size.
    EXPECT_EQ(offset, sizeof(header));
    // File size should be increased by allocSize.
    EXPECT_EQ(header.file_size, sizeof(header) + allocSize);
}

// Test that deallocation creates a free block and subsequent allocation reuses it.
TEST_F(AllocatorTestFixture, DeallocateAndReuseFreeBlock) {
    uint64_t allocSize = 128;
    // Allocate a block.
    uint64_t offset1 = allocator->allocate(allocSize);
    EXPECT_NE(offset1, UINT64_MAX);
    // Deallocate the block.
    allocator->deallocate(offset1, allocSize);
    // A subsequent allocation of the same size should reuse the free block.
    uint64_t offset2 = allocator->allocate(allocSize);
    EXPECT_EQ(offset1, offset2);
}

// Test that allocating 0 returns UINT64_MAX.
TEST_F(AllocatorTestFixture, ZeroSizeAllocation) {
    uint64_t offset = allocator->allocate(0);
    EXPECT_EQ(offset, UINT64_MAX);
}

// Dummy definitions to resolve unresolved external symbols.
namespace compio {

void flush_header(compio_archive* /*archive*/) {
    // Stub: do nothing.
}

// Provide definitions for the member functions of compio::btree
void btree::get_range(tree_key /*key_min*/, tree_key /*key_max*/, std::vector<std::pair<tree_key, tree_val>>& range) {
    range.clear();
}

bool btree::update(tree_key /*key*/, tree_val /*new_value*/) {
    return true;
}
}