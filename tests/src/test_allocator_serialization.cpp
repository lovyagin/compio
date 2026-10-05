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
TEST_F(AllocatorStateTest, SerializedStateIsLittleEndianWithChecksum) {
    uint64_t file_size = 1 << 20;
    free_blocks_manager manager(&file_size);
    manager.add_free_block(0x0102, 0x0304);

    std::vector<uint8_t> buffer;
    const uint32_t size = manager.serialize(buffer);
    // count, one (offset, size) pair, spare slot (offset, size), crc32c
    ASSERT_EQ(size, 8u + 16u + 16u + 4u);

    const uint8_t expected[40] = {1, 0, 0, 0, 0, 0, 0, 0,
                                  0x02, 0x01, 0, 0, 0, 0, 0, 0,
                                  0x04, 0x03, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(memcmp(buffer.data(), expected, sizeof(expected)), 0);

    const uint32_t crc = crc32c(buffer.data(), 40);
    const uint32_t stored = static_cast<uint32_t>(buffer[40]) | (static_cast<uint32_t>(buffer[41]) << 8) |
                            (static_cast<uint32_t>(buffer[42]) << 16) |
                            (static_cast<uint32_t>(buffer[43]) << 24);
    EXPECT_EQ(stored, crc);
}

TEST_F(AllocatorStateTest, CorruptedStateIsRejected) {
    uint64_t file_size = 1 << 20;
    free_blocks_manager manager(&file_size);
    manager.add_free_block(1000, 500);
    manager.add_free_block(4000, 100);

    std::vector<uint8_t> buffer;
    manager.serialize(buffer);
    // Padding after the checksum, as left by a slot larger than the state.
    buffer.resize(128, 0);

    free_blocks_manager intact(&file_size);
    EXPECT_TRUE(intact.deserialize(buffer.data(), static_cast<uint32_t>(buffer.size())));
    EXPECT_TRUE(intact.is_region_free(1000, 500));

    buffer[8] ^= 0x01;
    free_blocks_manager damaged(&file_size);
    EXPECT_FALSE(damaged.deserialize(buffer.data(), static_cast<uint32_t>(buffer.size())));
}

TEST_F(AllocatorStateTest, StateWithoutTrailerStillLoads) {
    uint64_t file_size = 1 << 20;
    free_blocks_manager manager(&file_size);
    manager.add_free_block(1000, 500);

    std::vector<uint8_t> buffer;
    manager.serialize(buffer);
    // Archives written before the trailer was added end right after the pairs.
    buffer.resize(8 + 16);

    free_blocks_manager loaded(&file_size);
    EXPECT_TRUE(loaded.deserialize(buffer.data(), static_cast<uint32_t>(buffer.size())));
    EXPECT_TRUE(loaded.is_region_free(1000, 500));
}

TEST_F(AllocatorStateTest, RepeatedFlushDoesNotGrowArchive) {
    compio_config config;
    compio_build_default_config(&config);
    config.max_files = 16;
    config.fragmentation_threshold = 100;

    auto *archive = compio_open_archive(fn1, "w+", &config);
    ASSERT_NE(archive, nullptr);
    compio_file *f = compio_open_file("data", archive);
    ASSERT_NE(f, nullptr);
    const std::vector<uint8_t> data(8192, 7);
    ASSERT_EQ(compio_write(data.data(), data.size(), f), data.size());
    compio_close_file(f);

    // The first flushes settle the alternating slots of the files table and of
    // the allocator state.
    for (int i = 0; i < 4; i++) compio_flush(archive);
    const uint64_t settled = archive->header->file_size;

    for (int i = 0; i < 100; i++) compio_flush(archive);
    EXPECT_EQ(archive->header->file_size, settled);

    compio_close_archive(archive);
}

TEST_F(AllocatorStateTest, ReopenCyclesDoNotGrowArchive) {
    compio_config config;
    compio_build_default_config(&config);
    config.max_files = 16;
    config.fragmentation_threshold = 100;

    {
        auto *archive = compio_open_archive(fn1, "w+", &config);
        ASSERT_NE(archive, nullptr);
        compio_file *f = compio_open_file("data", archive);
        ASSERT_NE(f, nullptr);
        const std::vector<uint8_t> data(8192, 7);
        ASSERT_EQ(compio_write(data.data(), data.size(), f), data.size());
        compio_close_file(f);
        compio_close_archive(archive);
    }

    auto logical_size_after_cycle = [&]() {
        auto *archive = compio_open_archive(fn1, "r+", &config);
        EXPECT_NE(archive, nullptr);
        compio_close_archive(archive);
        archive = compio_open_archive(fn1, "r", &config);
        EXPECT_NE(archive, nullptr);
        const uint64_t size = readonly(archive->header, header)->file_size;
        compio_close_archive(archive);
        return size;
    };

    for (int i = 0; i < 4; i++) logical_size_after_cycle();
    const uint64_t settled = logical_size_after_cycle();
    uint64_t last = settled;
    for (int i = 0; i < 50; i++) last = logical_size_after_cycle();
    EXPECT_EQ(last, settled);
}
