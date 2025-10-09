#include <algorithm>
#include <gtest/gtest.h>
#include <numeric>
#include <random>

#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"
#include "compio/utils.hpp"

#include "test_util.hpp"

using namespace compio;

// Tests for boundary conditions and edge cases
class BoundaryConditionTest : public ::testing::Test {
protected:
    char fn[256];
    FILE *file;
    compio_archive *archive;
    block_allocator *allocator;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));

        file = fopen(fn, "w+");
        ASSERT_TRUE(file != nullptr);

        auto *config = new compio_config();
        compio_build_default_config(config);
        config->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
        config->fragmentation_threshold = 30;
        config->fill_holes_with_zeros = false;

        archive = new compio_archive(file, mode_bit::w | mode_bit::r, config);
        allocator = new block_allocator(archive);
    }

    void TearDown() override {
        if (allocator)
            delete allocator;
        if (archive)
            delete archive;
        if (file)
            fclose(file);
        remove(fn);
    }
};

TEST_F(BoundaryConditionTest, AllocationAtFileStart) {
    // Test allocation immediately after header
    uint64_t offset = allocator->allocate(100);
    EXPECT_GE(offset, sizeof(header)) << "Allocation should be after header";
    EXPECT_LT(offset, sizeof(header) + 1000) << "First allocation should be near start";
}

TEST_F(BoundaryConditionTest, ExactFitAllocation) {
    // Create a hole of exact size and try to fit allocation
    uint64_t offset1 = allocator->allocate(100);
    uint64_t offset2 = allocator->allocate(200);
    uint64_t offset3 = allocator->allocate(100);

    ASSERT_NE(offset1, UINT64_MAX);
    ASSERT_NE(offset2, UINT64_MAX);
    ASSERT_NE(offset3, UINT64_MAX);

    // Free middle block
    allocator->deallocate(offset2, 200);

    // Allocate block of exact same size
    uint64_t exact_fit = allocator->allocate(200);
    EXPECT_EQ(exact_fit, offset2) << "Exact fit should reuse same space";
}

TEST_F(BoundaryConditionTest, SlightlyTooLargeAllocation) {
    // Create hole and try to allocate slightly larger block
    uint64_t offset1 = allocator->allocate(100);
    uint64_t offset2 = allocator->allocate(200);
    uint64_t offset3 = allocator->allocate(100);

    ASSERT_NE(offset1, UINT64_MAX);
    ASSERT_NE(offset2, UINT64_MAX);
    ASSERT_NE(offset3, UINT64_MAX);

    // Free middle block (200 bytes)
    allocator->deallocate(offset2, 200);

    // Try to allocate 201 bytes - should not fit in the hole
    uint64_t too_large = allocator->allocate(201);
    if (too_large != UINT64_MAX) {
        // If allocation succeeded, it should be at a different location
        EXPECT_NE(too_large, offset2) << "Too large allocation should not reuse exact hole";
    }
}

TEST_F(BoundaryConditionTest, MinimumSizeAllocation) {
    // Test allocation of 1 byte
    uint64_t offset = allocator->allocate(1);
    EXPECT_NE(offset, UINT64_MAX) << "Should be able to allocate 1 byte";

    if (offset != UINT64_MAX) {
        allocator->deallocate(offset, 1);

        // Verify we can reuse the 1-byte space
        uint64_t reuse_offset = allocator->allocate(1);
        EXPECT_EQ(reuse_offset, offset) << "Should reuse 1-byte space";
    }
}

// Tests for robustness under various conditions
class RobustnessTest : public ::testing::Test {
protected:
    char fn[256];
    FILE *file;
    compio_archive *archive;
    block_allocator *allocator;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));

        file = fopen(fn, "w+");
        ASSERT_TRUE(file != nullptr);

        auto *config = new compio_config();
        compio_build_default_config(config);
        config->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
        config->fragmentation_threshold = 30;
        config->fill_holes_with_zeros = false;

        archive = new compio_archive(file, mode_bit::w | mode_bit::r, config);
        allocator = new block_allocator(archive);
    }

    void TearDown() override {
        if (allocator)
            delete allocator;
        if (archive)
            delete archive;
        if (file)
            fclose(file);
        remove(fn);
    }
};

TEST_F(RobustnessTest, AllocateAfterFileGrowth) {
    // Force file to grow and test continued allocation
    std::vector<uint64_t> offsets;
    uint64_t initial_file_size = archive->header->file_size;

    // Allocate until file grows significantly
    for (int i = 0; i < 1000; i++) {
        uint64_t offset = allocator->allocate(1024);
        if (offset != UINT64_MAX) {
            offsets.push_back(offset);
        }

        if (archive->header->file_size > initial_file_size * 2) {
            break; // File has grown significantly
        }
    }

    EXPECT_GT(archive->header->file_size, initial_file_size)
        << "File should have grown during allocation";
    EXPECT_GT(offsets.size(), 0) << "Should have successful allocations";
}

TEST_F(RobustnessTest, DeallocateRandomOrder) {
    const int num_blocks = 50;
    std::vector<std::pair<uint64_t, size_t>> blocks;

    // Allocate blocks of varying sizes
    for (int i = 0; i < num_blocks; i++) {
        size_t size = 64 + (i % 10) * 32; // Sizes from 64 to 352
        uint64_t offset = allocator->allocate(size);
        if (offset != UINT64_MAX) {
            blocks.emplace_back(offset, size);
        }
    }

    // Shuffle the deallocation order
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(blocks.begin(), blocks.end(), g);

    // Deallocate in random order
    for (auto [offset, size] : blocks) {
        EXPECT_NO_THROW(allocator->deallocate(offset, size));
    }

    // After all deallocations, should be able to allocate large block
    uint64_t large_offset = allocator->allocate(num_blocks * 400);
    EXPECT_NE(large_offset, UINT64_MAX)
        << "Should be able to allocate large block after freeing everything";
}

TEST_F(RobustnessTest, AlternatingLargeSmallAllocations) {
    std::vector<std::pair<uint64_t, size_t>> allocations;

    // Alternate between large and small allocations
    for (int i = 0; i < 20; i++) {
        size_t size = (i % 2 == 0) ? 1024 : 32; // Large or small
        uint64_t offset = allocator->allocate(size);

        if (offset != UINT64_MAX) {
            allocations.emplace_back(offset, size);
        }
    }

    // Verify allocations don't overlap
    for (size_t i = 0; i < allocations.size(); i++) {
        for (size_t j = i + 1; j < allocations.size(); j++) {
            uint64_t start1 = allocations[i].first;
            uint64_t end1 = start1 + allocations[i].second;
            uint64_t start2 = allocations[j].first;
            uint64_t end2 = start2 + allocations[j].second;

            EXPECT_TRUE(end1 <= start2 || end2 <= start1) << "Overlapping allocations detected";
        }
    }

    EXPECT_GT(allocations.size(), 15) << "Should successfully allocate most blocks";
}

// Test specific allocation patterns that might cause issues
class AllocationPatternTest : public ::testing::Test {
protected:
    char fn[256];
    FILE *file;
    compio_archive *archive;
    block_allocator *allocator;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));

        file = fopen(fn, "w+");
        ASSERT_TRUE(file != nullptr);

        auto *config = new compio_config();
        compio_build_default_config(config);
        config->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
        config->fragmentation_threshold = 30;
        config->fill_holes_with_zeros = false;

        archive = new compio_archive(file, mode_bit::w | mode_bit::r, config);
        allocator = new block_allocator(archive);
    }

    void TearDown() override {
        if (allocator)
            delete allocator;
        if (archive)
            delete archive;
        if (file)
            fclose(file);
        remove(fn);
    }
};

TEST_F(AllocationPatternTest, GeometricSizeIncrease) {
    // Test allocation of geometrically increasing sizes
    std::vector<std::pair<uint64_t, size_t>> blocks;

    for (int i = 0; i < 15; i++) {
        size_t size = 16 << i; // 16, 32, 64, 128, ...
        uint64_t offset = allocator->allocate(size);

        if (offset != UINT64_MAX) {
            blocks.emplace_back(offset, size);
        } else {
            std::cout << "Failed to allocate " << size << " bytes at iteration " << i << std::endl;
            break;
        }
    }

    EXPECT_GT(blocks.size(), 10) << "Should handle geometric size increase";

    // Verify no overlaps
    for (size_t i = 0; i < blocks.size(); i++) {
        for (size_t j = i + 1; j < blocks.size(); j++) {
            uint64_t start1 = blocks[i].first;
            uint64_t end1 = start1 + blocks[i].second;
            uint64_t start2 = blocks[j].first;
            uint64_t end2 = start2 + blocks[j].second;

            EXPECT_TRUE(end1 <= start2 || end2 <= start1)
                << "Geometric allocation overlap: blocks " << i << " and " << j;
        }
    }
}

TEST_F(AllocationPatternTest, FibonacciSizeSequence) {
    // Test Fibonacci sequence sizes to create irregular pattern
    std::vector<size_t> fib_sizes = {1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610};
    std::vector<std::pair<uint64_t, size_t>> blocks;

    for (size_t size : fib_sizes) {
        uint64_t offset = allocator->allocate(size * 16); // Scale up for practical sizes
        if (offset != UINT64_MAX) {
            blocks.emplace_back(offset, size * 16);
        }
    }

    EXPECT_EQ(blocks.size(), fib_sizes.size()) << "Should allocate all Fibonacci sizes";

    // Free blocks in reverse order
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        EXPECT_NO_THROW(allocator->deallocate(it->first, it->second));
    }
}

// Test real-world usage patterns
class RealWorldPatternTest : public ::testing::Test {
protected:
    char fn[256];
    FILE *file;
    compio_archive *archive;
    block_allocator *allocator;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));

        file = fopen(fn, "w+");
        ASSERT_TRUE(file != nullptr);

        auto *config = new compio_config();
        compio_build_default_config(config);
        config->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
        config->fragmentation_threshold = 30;
        config->fill_holes_with_zeros = false;

        archive = new compio_archive(file, mode_bit::w | mode_bit::r, config);
        allocator = new block_allocator(archive);
    }

    void TearDown() override {
        if (allocator)
            delete allocator;
        if (archive)
            delete archive;
        if (file)
            fclose(file);
        remove(fn);
    }
};

TEST_F(RealWorldPatternTest, DatabaseLikePattern) {
    // Simulate database page allocation pattern
    const size_t page_size = 4096;
    const int num_pages = 100;
    std::vector<uint64_t> pages;

    // Allocate pages
    for (int i = 0; i < num_pages; i++) {
        uint64_t offset = allocator->allocate(page_size);
        if (offset != UINT64_MAX) {
            pages.push_back(offset);
        }
    }

    EXPECT_GT(pages.size(), num_pages * 0.8) << "Should allocate most pages";

    // Simulate random page deletions
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(pages.begin(), pages.end(), gen);

    // Free 30% of pages
    size_t pages_to_free = pages.size() * 0.3;
    for (size_t i = 0; i < pages_to_free; i++) {
        allocator->deallocate(pages[i], page_size);
    }

    // Allocate new pages - should reuse freed space
    std::vector<uint64_t> new_pages;
    for (size_t i = 0; i < pages_to_free; i++) {
        uint64_t offset = allocator->allocate(page_size);
        if (offset != UINT64_MAX) {
            new_pages.push_back(offset);
        }
    }

    EXPECT_EQ(new_pages.size(), pages_to_free) << "Should be able to reuse all freed pages";
}

TEST_F(RealWorldPatternTest, LogFilePattern) {
    // Simulate log file pattern: many small writes, occasional large writes
    std::vector<std::pair<uint64_t, size_t>> log_entries;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> size_dist(32, 256);

    // 90% small entries, 10% large entries
    for (int i = 0; i < 200; i++) {
        size_t size;
        if (i % 10 == 0) {
            size = 2048 + size_dist(gen); // Large entry
        } else {
            size = 32 + (size_dist(gen) % 128); // Small entry
        }

        uint64_t offset = allocator->allocate(size);
        if (offset != UINT64_MAX) {
            log_entries.emplace_back(offset, size);
        }
    }

    EXPECT_GT(log_entries.size(), 180) << "Should handle log pattern efficiently";

    // Simulate log rotation - free old entries
    size_t entries_to_free = log_entries.size() / 3;
    for (size_t i = 0; i < entries_to_free; i++) {
        allocator->deallocate(log_entries[i].first, log_entries[i].second);
    }

    // Continue adding new entries
    for (int i = 0; i < 50; i++) {
        size_t size = 64 + (i % 200);
        uint64_t offset = allocator->allocate(size);
        EXPECT_NE(offset, UINT64_MAX) << "Should handle continued allocation after rotation";
    }
}

// Test memory efficiency and fragmentation
class EfficiencyTest : public ::testing::Test {
protected:
    char fn[256];
    FILE *file;
    compio_archive *archive;
    block_allocator *allocator;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));

        file = fopen(fn, "w+");
        ASSERT_TRUE(file != nullptr);

        auto *config = new compio_config();
        compio_build_default_config(config);
        config->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
        config->fragmentation_threshold = 30;
        config->fill_holes_with_zeros = false;

        archive = new compio_archive(file, mode_bit::w | mode_bit::r, config);
        archive->allocator = allocator = new block_allocator(archive);
        archive->index = new btree(archive);
    }

    void TearDown() override {
        if (archive->index)
            delete archive->index;
        if (allocator)
            delete allocator;
        if (archive)
            delete archive;
        if (file)
            fclose(file);
        remove(fn);
    }
};

TEST_F(EfficiencyTest, SpaceUtilizationEfficiency) {
    const size_t target_data = 1024 * 1024; // 1MB of data
    const size_t block_size = 1024;
    const size_t expected_blocks = target_data / block_size;

    std::vector<uint64_t> blocks;

    // Allocate blocks until we reach target data size
    for (size_t i = 0; i < expected_blocks; i++) {
        uint64_t offset = allocator->allocate(block_size);
        if (offset != UINT64_MAX) {
            blocks.push_back(offset);
        }
    }

    uint64_t actual_file_size = archive->header->file_size;
    uint64_t expected_min_size = sizeof(header) + target_data;

    // Calculate overhead percentage
    double overhead_ratio = double(actual_file_size - expected_min_size) / target_data;

    std::cout << "Allocated " << blocks.size() << " blocks (" << target_data << " bytes)"
              << std::endl;
    std::cout << "File size: " << actual_file_size << " bytes" << std::endl;
    std::cout << "Overhead: " << (overhead_ratio * 100) << "%" << std::endl;

    EXPECT_LT(overhead_ratio, 0.1) << "Space overhead should be less than 10%";
    EXPECT_EQ(blocks.size(), expected_blocks) << "Should allocate all requested blocks";
}

TEST_F(EfficiencyTest, FragmentationMeasurement) {
    // Create controlled fragmentation scenario
    std::vector<uint64_t> blocks;
    const size_t block_size = 256;

    // Allocate many blocks
    for (int i = 0; i < 50; i++) {
        uint64_t offset = allocator->allocate(block_size);
        ASSERT_NE(offset, UINT64_MAX);
        blocks.push_back(offset);
    }

    // Free every third block to create specific fragmentation
    std::vector<uint64_t> freed_blocks;
    for (size_t i = 2; i < blocks.size(); i += 3) {
        allocator->deallocate(blocks[i], block_size);
        freed_blocks.push_back(blocks[i]);
    }

    // Measure fragmentation before and after defragmentation
    uint8_t frag_before = allocator->get_fragmentation();
    allocator->maintenance();
    uint8_t frag_after = allocator->get_fragmentation();

    std::cout << "Fragmentation before defrag: " << (int)frag_before << "%" << std::endl;
    std::cout << "Fragmentation after defrag: " << (int)frag_after << "%" << std::endl;

    EXPECT_LE(frag_after, frag_before) << "Defragmentation should not increase fragmentation";
}

// Integration test with all features
class IntegrationTest : public ::testing::Test {
protected:
    char fn[256];
    FILE *file;
    compio_archive *archive;
    block_allocator *allocator;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));

        file = fopen(fn, "w+");
        ASSERT_TRUE(file != nullptr);

        auto *config = new compio_config();
        compio_build_default_config(config);
        config->allocation_strategy = COMPIO_ALLOC_BEST_FIT;
        config->fragmentation_threshold = 25;
        config->fill_holes_with_zeros = true;

        archive = new compio_archive(file, mode_bit::w | mode_bit::r, config);
        archive->allocator = allocator = new block_allocator(archive);
        archive->index = new btree(archive);
    }

    void TearDown() override {
        if (archive->index)
            delete archive->index;
        if (allocator)
            delete allocator;
        if (archive)
            delete archive;
        if (file)
            fclose(file);
        remove(fn);
    }
};

TEST_F(IntegrationTest, CompleteLifecycleTest) {
    // Phase 1: Initial allocation burst
    std::vector<std::pair<uint64_t, size_t>> phase1_blocks;
    std::vector<size_t> sizes = {128, 256, 512, 1024, 64, 32, 2048};

    for (size_t size : sizes) {
        uint64_t offset = allocator->allocate(size);
        ASSERT_NE(offset, UINT64_MAX) << "Phase 1 allocation failed for size " << size;
        phase1_blocks.emplace_back(offset, size);
    }

    // Phase 2: Partial deallocation
    for (size_t i = 1; i < phase1_blocks.size(); i += 2) {
        allocator->deallocate(phase1_blocks[i].first, phase1_blocks[i].second);
    }

    // Phase 3: Mixed allocation/deallocation
    std::vector<std::pair<uint64_t, size_t>> phase3_blocks;
    for (int i = 0; i < 20; i++) {
        if (i % 3 == 0 && !phase3_blocks.empty()) {
            // Deallocate random block
            auto &block = phase3_blocks[i % phase3_blocks.size()];
            allocator->deallocate(block.first, block.second);
            phase3_blocks.erase(phase3_blocks.begin() + (i % phase3_blocks.size()));
        } else {
            // Allocate new block
            size_t size = 100 + (i * 50) % 500;
            uint64_t offset = allocator->allocate(size);
            if (offset != UINT64_MAX) {
                phase3_blocks.emplace_back(offset, size);
            }
        }
    }

    // Phase 4: Stress fragmentation and defragmentation
    uint8_t frag_level = allocator->get_fragmentation();
    std::cout << "Final fragmentation level: " << (int)frag_level << "%" << std::endl;

    allocator->maintenance();

    // Phase 5: Final large allocation test
    uint64_t final_offset = allocator->allocate(10000);
    EXPECT_NE(final_offset, UINT64_MAX) << "Should handle large allocation after full lifecycle";

    std::cout << "Integration test completed successfully" << std::endl;
}
