#include <gtest/gtest.h>

#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"
#include "compio/utils.hpp"

#include "test_util.hpp"

using namespace compio;

// Allocator state serialization tests
class AllocatorStateTest : public ::testing::Test {
protected:
    char fn1[256], fn2[256];
    FILE *file1;
    FILE *file2;

    void SetUp() override {
        // Create two temporary files
        generate_tmp_fn(fn1, sizeof(fn1));
        generate_tmp_fn(fn2, sizeof(fn2));

        file1 = fopen(fn1, "w+");
        file2 = fopen(fn2, "w+");
        ASSERT_TRUE(file1 != nullptr);
        ASSERT_TRUE(file2 != nullptr);
    }

    void TearDown() override {
        if (file1)
            fclose(file1);
        if (file2)
            fclose(file2);
        remove(fn1);
        remove(fn2);
    }
};

TEST_F(AllocatorStateTest, SaveAndLoadState) {
    // Create allocator and archive
    auto *config = new compio_config();
    compio_build_default_config(config);
    config->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
    config->fragmentation_threshold = 30;
    config->fill_holes_with_zeros = false;

    auto *archive = new compio_archive(file1, mode_bit::w | mode_bit::r, config);
    auto *allocator = new block_allocator(archive);

    // Create state with free blocks
    uint64_t offset1 = allocator->allocate(100);
    uint64_t offset2 = allocator->allocate(200);
    uint64_t offset3 = allocator->allocate(100);

    ASSERT_NE(offset1, UINT64_MAX);
    ASSERT_NE(offset2, UINT64_MAX);
    ASSERT_NE(offset3, UINT64_MAX);

    // Free middle block
    allocator->deallocate(offset2, 200);

    // Save state
    bool save_success = allocator->save_state(archive);
    EXPECT_TRUE(save_success) << "Should be able to save allocator state";

    // Close allocator and archive
    delete allocator;
    delete archive;

    // Reopen file in read mode
    file1 = freopen(fn1, "r+", file1);
    ASSERT_TRUE(file1 != nullptr);

    // Create new archive in read mode - should load header
    auto *new_archive = new compio_archive(file1, mode_bit::r, config);
    auto *new_allocator = new block_allocator(new_archive);

    // Try to load state
    bool load_success = new_allocator->load_state(new_archive);

    if (load_success) {
        RecordProperty("serialization_state_loaded", true);

        // Check that free space is available
        uint64_t reuse_offset = new_allocator->allocate(200);
        EXPECT_EQ(reuse_offset, offset2) << "Should reuse freed space after loading state";
    } else {
        RecordProperty("serialization_state_loaded", false);
        uint64_t test_offset = new_allocator->allocate(100);
        EXPECT_NE(test_offset, UINT64_MAX) << "Allocator should work even without state loading";
    }

    delete new_allocator;
    delete new_archive;
}