#include <gtest/gtest.h>

#include <string>
#include <tuple>
#include <vector>

#include "compio/compio_file.hpp"
#include "compio/utils.hpp"
#include "test_util.hpp"

class DeferredFreeTest : public ::testing::TestWithParam<std::tuple<bool, bool>> {
protected:
    char path[256];
    compio_archive *archive = nullptr;
    compio_config config{};
    const std::vector<unsigned char> original = std::vector<unsigned char>(4096, 0xAB);
    uint64_t offset = 0;

    void SetUp() override {
        generate_tmp_fn(path, sizeof(path));
        compio_build_default_config(&config);
        config.max_files = 1;
        config.enable_wal = std::get<0>(GetParam());
        config.fill_holes_with_zeros = std::get<1>(GetParam());
        config.fragmentation_threshold = 100;
        config.auto_batch_size = 0;
        config.wal_max_size_bytes = 0;
        archive = compio_open_archive(path, "w+", &config);
        ASSERT_NE(archive, nullptr);
        compio_flush(archive);
        offset = archive->allocator->allocate(original.size());
        ASSERT_NE(offset, UINT64_MAX);
        ASSERT_EQ(compio::fseek64(archive->file, offset, SEEK_SET), 0);
        ASSERT_EQ(fwrite(original.data(), 1, original.size(), archive->file), original.size());
        compio_flush(archive);
    }

    void TearDown() override {
        if (archive) {
            EXPECT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
        }
        remove(path);
        remove((std::string(path) + ".wal").c_str());
    }

    std::vector<unsigned char> read_region() {
        std::vector<unsigned char> data(original.size());
        EXPECT_EQ(compio::fseek64(archive->file, offset, SEEK_SET), 0);
        EXPECT_EQ(fread(data.data(), 1, data.size(), archive->file), data.size());
        return data;
    }

    void defer() { archive->allocator->deallocate(offset, original.size(), false); }

    void expect_released() {
        if (config.fill_holes_with_zeros) {
            EXPECT_EQ(read_region(), std::vector<unsigned char>(original.size(), 0));
        }
        EXPECT_EQ(archive->allocator->allocate(original.size()), offset);
    }
};

TEST_P(DeferredFreeTest, KeepsBytesAndPreventsReuseUntilCheckpoint) {
    defer();
    EXPECT_EQ(read_region(), original);
    EXPECT_NE(archive->allocator->allocate(original.size()), offset);
    compio_flush(archive);
    expect_released();
}

TEST_P(DeferredFreeTest, HeaderOnlyPublicationDoesNotReleaseDeferredBlocks) {
    defer();
    ASSERT_TRUE(archive->publish_header());
    EXPECT_EQ(read_region(), original);
    EXPECT_NE(archive->allocator->allocate(original.size()), offset);
    compio_flush(archive);
    expect_released();
}

TEST_P(DeferredFreeTest, OuterBatchCheckpointReleasesDeferredBlocks) {
    ASSERT_EQ(compio_begin_batch(archive), COMPIO_SUCCESS);
    ASSERT_EQ(compio_begin_batch(archive), COMPIO_SUCCESS);
    defer();
    if (config.enable_wal) {
        compio_flush(archive);
        EXPECT_EQ(read_region(), original);
        ASSERT_EQ(compio_end_batch(archive), COMPIO_SUCCESS);
        EXPECT_EQ(read_region(), original);
        EXPECT_NE(archive->allocator->allocate(original.size()), offset);
    } else {
        ASSERT_EQ(compio_end_batch(archive), COMPIO_SUCCESS);
    }
    ASSERT_EQ(compio_end_batch(archive), COMPIO_SUCCESS);
    expect_released();
}

TEST_P(DeferredFreeTest, ClosePersistsFreeSpace) {
    defer();
    ASSERT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
    archive = compio_open_archive(path, "r+", &config);
    ASSERT_NE(archive, nullptr);
    expect_released();
}

TEST_P(DeferredFreeTest, BatchWithoutExplicitFlushPersistsFreeSpace) {
    ASSERT_EQ(compio_begin_batch(archive), COMPIO_SUCCESS);
    defer();
    ASSERT_EQ(compio_end_batch(archive), COMPIO_SUCCESS);
    // Load the published snapshot independently, before close can save it again.
    compio::free_blocks_manager recovered(&readonly(archive->header, compio::header)->file_size);
    ASSERT_TRUE(recovered.load_from_file(archive));
    EXPECT_EQ(recovered.allocate_block(original.size(), compio::allocation_strategy::FIRST_FIT), offset);
    expect_released();
}

TEST_P(DeferredFreeTest, FreesAfterSnapshotWaitForAnotherCheckpoint) {
    const uint64_t second = archive->allocator->allocate(original.size());
    ASSERT_NE(second, UINT64_MAX);
    compio_flush(archive);
    defer();
    ASSERT_TRUE(archive->allocator->save_state(archive));
    archive->allocator->deallocate(second, original.size(), false);
    ASSERT_TRUE(archive->publish_header());
    archive->allocator->commit_state();
    expect_released();
    EXPECT_NE(archive->allocator->allocate(original.size()), second);
    compio_flush(archive);
    EXPECT_EQ(archive->allocator->allocate(original.size()), second);
}

TEST_P(DeferredFreeTest, CompactionWaitsForDeferredFrees) {
    defer();
    const uint64_t end = readonly(archive->header, compio::header)->file_size;
    archive->allocator->force_defragmentation();
    EXPECT_EQ(readonly(archive->header, compio::header)->file_size, end);
    EXPECT_FALSE(archive->truncate_after_publish);
    EXPECT_EQ(read_region(), original);
    compio_flush(archive);
    expect_released();
}

TEST_P(DeferredFreeTest, DuplicateAndOverlappingFreesDoNotExposeLiveBytes) {
    defer();
    defer();
    // This overlaps the deferred block and the following allocator state slot.
    archive->allocator->deallocate(offset + 1, original.size(), false);
    compio_flush(archive);
    expect_released();
    EXPECT_NE(archive->allocator->allocate(original.size()), offset);
}

INSTANTIATE_TEST_SUITE_P(WalAndRelease, DeferredFreeTest,
                        ::testing::Combine(::testing::Bool(), ::testing::Bool()));
