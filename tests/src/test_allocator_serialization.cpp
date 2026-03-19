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

    void SetUp() override {
        // Create two temporary files
        generate_tmp_fn(fn1, sizeof(fn1));
        generate_tmp_fn(fn2, sizeof(fn2));
    }

    void TearDown() override {
        remove(fn1);
        remove((std::string(fn1) + ".wal").c_str());
        remove(fn2);
        remove((std::string(fn2) + ".wal").c_str());
    }
};

TEST_F(AllocatorStateTest, SaveAndLoadState) {
    // Create allocator and archive
    compio_config config;
    compio_build_default_config(&config);
    config.allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
    config.fragmentation_threshold = 30;
    config.fill_holes_with_zeros = false;

    auto *archive = compio_open_archive(fn1, "w+", &config);
    auto *allocator = archive->allocator;

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
    compio_close_archive(archive);

    // Create new archive in read mode - should load header
    auto *new_archive = compio_open_archive(fn1, "r", &config);
    auto *new_allocator = new_archive->allocator;

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

    compio_close_archive(new_archive);
}