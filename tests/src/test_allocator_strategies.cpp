/**
 * @file test_allocator_strategies.cpp
 * @brief Behavioural comparison of allocation strategies.
 *
 * Covers gaps identified during review:
 *  - Actual fragmentation outcome differences between BEST_FIT / FIRST_FIT /
 *    WORST_FIT / NEXT_FIT under controlled scenarios.
 *  - BEST_FIT leaving smaller leftover fragments than FIRST_FIT.
 *  - WORST_FIT leaving larger leftover fragments (helps future large allocs).
 *  - NEXT_FIT continuing from last position rather than always from head.
 *  - Strategy state persistence: allocator survives save/reload with the
 *    same strategy.
 *  - Maintenance auto-trigger verified with fragmentation threshold.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"

#include "test_util.hpp"

using namespace compio;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static compio_archive *open_archive_with_strategy(const char *fn,
                                                   compio_allocation_strategy s,
                                                   uint8_t frag_threshold = 100) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.allocation_strategy   = s;
    cfg.fragmentation_threshold = frag_threshold;
    cfg.fill_holes_with_zeros = false;
    return compio_open_archive(fn, "w+", &cfg);
}

// Build a specific fragmented layout:
//   Allocate slots of size `slot_size`, deallocate the even-indexed ones.
//   Returns the offsets of the remaining (odd-indexed) live blocks.
static std::vector<uint64_t> make_fragmented(block_allocator *alloc,
                                              int n_slots, uint64_t slot_size) {
    std::vector<uint64_t> offsets(n_slots);
    for (int i = 0; i < n_slots; ++i) {
        offsets[i] = alloc->allocate(slot_size);
        EXPECT_NE(offsets[i], static_cast<uint64_t>(UINT64_MAX))
            << "allocate() failed at slot " << i << "; fragmented layout not created";
    }

    std::vector<uint64_t> live;
    for (int i = 0; i < n_slots; ++i) {
        if (i % 2 == 0)
            alloc->deallocate(offsets[i], slot_size);
        else
            live.push_back(offsets[i]);
    }
    return live;
}

// ---------------------------------------------------------------------------
// StrategyFragmentationTest
// Verify that BEST_FIT wastes less residual space than FIRST_FIT when
// allocating into a fragmented free list that has holes of varying sizes.
// ---------------------------------------------------------------------------
class StrategyFragmentationTest : public ::testing::Test {
protected:
    char fn[256];
    compio_archive *archive = nullptr;

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }

    void TearDown() override {
        if (archive) compio_close_archive(archive);
        remove(fn);
    }
};

// Scenario: free holes of sizes 300, 100, 200 (NOT adjacent — separated by live sentinels).
// Allocating 150 bytes:
//   FIRST_FIT → picks first hole ≥ 150, which is 300 (lowest offset).
//   BEST_FIT  → picks smallest hole ≥ 150, which is 200.
// Their offsets must differ.
TEST_F(StrategyFragmentationTest, BestFitPreservesLargeHoles) {
    auto build = [&](compio_allocation_strategy s) -> uint64_t {
        if (archive) compio_close_archive(archive);
        archive = open_archive_with_strategy(fn, s);
        EXPECT_NE(archive, nullptr);
        if (!archive) {
            ADD_FAILURE() << "Failed to open archive in StrategyFragmentationTest::build";
            return 0;
        }
        block_allocator *alloc = archive->allocator;

        // Layout: [hole300][sent1][hole100][sent2][hole200][sent3]
        // Live sentinels between holes prevent merging on deallocation.
        uint64_t h1   = alloc->allocate(300);
        uint64_t sent1 = alloc->allocate(64);  (void)sent1; // live
        uint64_t h2   = alloc->allocate(100);
        uint64_t sent2 = alloc->allocate(64);  (void)sent2; // live
        uint64_t h3   = alloc->allocate(200);
        alloc->allocate(64); // trailing sentinel

        alloc->deallocate(h1, 300);
        alloc->deallocate(h2, 100);
        alloc->deallocate(h3, 200);

        // Allocate 150: FIRST_FIT takes h1 (lowest offset, 300 bytes).
        //               BEST_FIT  takes h3 (smallest sufficient, 200 bytes).
        return alloc->allocate(150);
    };

    uint64_t ff_off = build(COMPIO_ALLOC_FIRST_FIT);
    uint64_t bf_off = build(COMPIO_ALLOC_BEST_FIT);

    EXPECT_NE(ff_off, bf_off)
        << "FIRST_FIT and BEST_FIT should pick different holes for a 150-byte alloc "
           "when holes of 300, 100, 200 bytes exist (separated by live blocks)";
}

// WORST_FIT should pick the largest available hole.
TEST_F(StrategyFragmentationTest, WorstFitPicksLargestHole) {
    archive = open_archive_with_strategy(fn, COMPIO_ALLOC_WORST_FIT);
    ASSERT_NE(archive, nullptr);
    block_allocator *alloc = archive->allocator;

    // Layout: [small100][sentinel][large400][sentinel]
    uint64_t small_hole = alloc->allocate(100);
    alloc->allocate(64); // live sentinel — prevents merging
    uint64_t large_hole = alloc->allocate(400);
    alloc->allocate(64); // trailing sentinel

    alloc->deallocate(small_hole, 100);
    alloc->deallocate(large_hole, 400);

    // WORST_FIT should pick large_hole (400 bytes) for a 50-byte request.
    uint64_t off = alloc->allocate(50);
    EXPECT_NE(off, UINT64_MAX);

    EXPECT_EQ(off, large_hole)
        << "WORST_FIT should allocate from the largest (400-byte) hole, not the 100-byte one";
}

// BEST_FIT should pick the smallest sufficient hole.
TEST_F(StrategyFragmentationTest, BestFitPicksSmallestSufficientHole) {
    archive = open_archive_with_strategy(fn, COMPIO_ALLOC_BEST_FIT);
    ASSERT_NE(archive, nullptr);
    block_allocator *alloc = archive->allocator;

    uint64_t exact_hole   = alloc->allocate(200);
    alloc->allocate(64); // live sentinel — prevents exact_hole + larger_hole from merging
    uint64_t larger_hole  = alloc->allocate(400);
    alloc->allocate(64); // trailing sentinel

    alloc->deallocate(exact_hole,  200);
    alloc->deallocate(larger_hole, 400);

    // BEST_FIT should pick exact_hole (200) for a 200-byte request.
    uint64_t off = alloc->allocate(200);
    EXPECT_EQ(off, exact_hole)
        << "BEST_FIT should pick the 200-byte hole (exact fit) over the 400-byte hole";
}

// ---------------------------------------------------------------------------
// NextFitTest
// NEXT_FIT should continue searching from the last allocation point, not
// always from the head. This means the second allocation goes into the hole
// immediately after the first one (within the same traversal).
// ---------------------------------------------------------------------------
class NextFitTest : public ::testing::Test {
protected:
    char fn[256];
    compio_archive *archive = nullptr;

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }

    void TearDown() override {
        if (archive) compio_close_archive(archive);
        remove(fn);
    }
};

// NEXT_FIT continues from the last allocation point rather than restarting
// from the head. After consuming a hole, the cursor sits at the next hole.
// Restoring the first hole then allocating again must pick the second hole
// (cursor has advanced past the first), not the first (which FIRST_FIT would pick).
TEST_F(NextFitTest, CursorAdvancesPastPreviousAllocation) {
    archive = open_archive_with_strategy(fn, COMPIO_ALLOC_NEXT_FIT);
    ASSERT_NE(archive, nullptr);
    block_allocator *alloc = archive->allocator;

    // Create three non-adjacent free holes with live sentinels between them
    // to prevent merging on deallocation.
    uint64_t h1 = alloc->allocate(64);
    alloc->allocate(32); // live sentinel
    uint64_t h2 = alloc->allocate(64);
    alloc->allocate(32); // live sentinel
    uint64_t h3 = alloc->allocate(64);
    alloc->allocate(32); // trailing sentinel

    ASSERT_NE(h1, UINT64_MAX);
    ASSERT_NE(h2, UINT64_MAX);
    ASSERT_NE(h3, UINT64_MAX);

    // Free h1 first: it becomes the initial free list head, so last_alloc_
    // is initialised to h1's block.
    alloc->deallocate(h1, 64);
    alloc->deallocate(h2, 64);
    alloc->deallocate(h3, 64);
    // Free list (offset-ordered): h1 → h2 → h3. Cursor = h1.

    // First alloc: cursor is at h1 → picks h1. Cursor advances to h2.
    uint64_t alloc1 = alloc->allocate(64);
    ASSERT_NE(alloc1, UINT64_MAX);
    EXPECT_EQ(alloc1, h1) << "NEXT_FIT first alloc should pick h1 (cursor starts there)";

    // Restore h1. Free list: h1 → h2 → h3. Cursor is still at h2.
    alloc->deallocate(h1, 64);

    // Second alloc: NEXT_FIT starts at h2 (cursor has advanced past h1) and
    // picks h2. FIRST_FIT would restart from the head and pick h1 instead.
    uint64_t alloc2 = alloc->allocate(64);
    ASSERT_NE(alloc2, UINT64_MAX);
    EXPECT_EQ(alloc2, h2)
        << "NEXT_FIT should pick h2 (cursor is past h1), not h1 (which FIRST_FIT would pick)";
    EXPECT_NE(alloc2, h1)
        << "NEXT_FIT must not restart from head like FIRST_FIT would";
}

// NEXT_FIT wraps around the free list when the cursor is the only free entry.
// When 'a' is freed it becomes the sole free block and last_alloc_ is
// initialised to it; the next allocation reuses 'a' rather than extending the file.
TEST_F(NextFitTest, ReusesFreedBlockAfterCursorWrap) {
    archive = open_archive_with_strategy(fn, COMPIO_ALLOC_NEXT_FIT);
    ASSERT_NE(archive, nullptr);
    block_allocator *alloc = archive->allocator;

    uint64_t a = alloc->allocate(64);
    uint64_t b = alloc->allocate(64);
    uint64_t c = alloc->allocate(64);
    ASSERT_NE(a, UINT64_MAX);
    ASSERT_NE(b, UINT64_MAX);
    ASSERT_NE(c, UINT64_MAX);

    // Free 'a': it becomes the first (and only) entry in the free list, so
    // last_alloc_ is initialised to a's free block.
    alloc->deallocate(a, 64);

    // NEXT_FIT starts at last_alloc_ (== a) and finds it immediately;
    // the freed block is reused rather than the file being extended.
    uint64_t d = alloc->allocate(64);
    ASSERT_NE(d, UINT64_MAX) << "NEXT_FIT must find space";
    EXPECT_EQ(d, a) << "NEXT_FIT should reuse the freed block 'a'";

    // 'd' must not overlap the still-live blocks b and c.
    EXPECT_FALSE(d < b + 64 && d + 64 > b) << "d overlaps with b";
    EXPECT_FALSE(d < c + 64 && d + 64 > c) << "d overlaps with c";
}

// ---------------------------------------------------------------------------
// StrategyStatisticsTest
// Verify fragmentation stats fields are self-consistent after a known layout.
// ---------------------------------------------------------------------------
class StrategyStatisticsTest : public ::testing::Test {
protected:
    char fn[256];
    compio_archive *archive = nullptr;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));
        archive = open_archive_with_strategy(fn, COMPIO_ALLOC_FIRST_FIT, 100);
        ASSERT_NE(archive, nullptr);
    }

    void TearDown() override {
        if (archive) compio_close_archive(archive);
        remove(fn);
    }
};

TEST_F(StrategyStatisticsTest, StatsFieldsConsistency) {
    block_allocator *alloc = archive->allocator;

    // Create 4 non-adjacent free holes of sizes 100, 200, 50, 300.
    // Live sentinel blocks between holes prevent merging on deallocation.
    uint64_t h1 = alloc->allocate(100);
    alloc->allocate(32);  // live sentinel
    uint64_t h2 = alloc->allocate(200);
    alloc->allocate(32);  // live sentinel
    uint64_t h3 = alloc->allocate(50);
    alloc->allocate(32);  // live sentinel
    uint64_t h4 = alloc->allocate(300);
    alloc->allocate(32);  // trailing sentinel

    alloc->deallocate(h1, 100);
    alloc->deallocate(h2, 200);
    alloc->deallocate(h3, 50);
    alloc->deallocate(h4, 300);

    compio_fragmentation_stats stats{};
    ASSERT_EQ(compio_get_fragmentation_stats(archive, &stats), COMPIO_SUCCESS);

    EXPECT_EQ(stats.num_free_regions, 4u);
    EXPECT_EQ(stats.total_free_bytes, 100u + 200u + 50u + 300u);
    EXPECT_EQ(stats.largest_free_region, 300u);
    EXPECT_EQ(stats.smallest_free_region, 50u);

    double expected_avg = (100.0 + 200.0 + 50.0 + 300.0) / 4.0;
    EXPECT_NEAR(stats.avg_free_region_size, expected_avg, 1.0);

    EXPECT_LE(stats.fragmentation_percent, 100u);
}

TEST_F(StrategyStatisticsTest, StatsAfterCoalescing) {
    block_allocator *alloc = archive->allocator;

    // Allocate three adjacent blocks and free them all → should merge.
    uint64_t a = alloc->allocate(128);
    uint64_t b = alloc->allocate(128);
    uint64_t c = alloc->allocate(128);
    alloc->allocate(64); // sentinel to prevent merging with file end

    alloc->deallocate(a, 128);
    alloc->deallocate(b, 128);
    alloc->deallocate(c, 128);

    compio_fragmentation_stats stats{};
    ASSERT_EQ(compio_get_fragmentation_stats(archive, &stats), COMPIO_SUCCESS);

    // After freeing three adjacent blocks the allocator merges them.
    EXPECT_EQ(stats.num_free_regions, 1u) << "Three adjacent free blocks should merge into one";
    EXPECT_EQ(stats.total_free_bytes, 384u);
    EXPECT_EQ(stats.largest_free_region, 384u);
    EXPECT_EQ(stats.smallest_free_region, 384u);
    EXPECT_EQ(stats.fragmentation_percent, 0u) << "Single free region → 0% fragmentation";
}

TEST_F(StrategyStatisticsTest, NullArchiveReturnsError) {
    compio_fragmentation_stats stats{};
    EXPECT_EQ(compio_get_fragmentation_stats(nullptr, &stats), COMPIO_ERROR);
}

// ---------------------------------------------------------------------------
// MaintenanceAutoTriggerTest
// Verify that the allocator actually runs defragmentation when fragmentation
// exceeds the configured threshold (auto-maintenance path).
// ---------------------------------------------------------------------------
class MaintenanceAutoTriggerTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override { remove(fn); }
};

TEST_F(MaintenanceAutoTriggerTest, MaintenanceTriggersWhenThresholdExceeded) {
    // Low threshold so maintenance fires early.
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.fragmentation_threshold = 1;
    cfg.fill_holes_with_zeros   = false;

    compio_archive *ar = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(ar, nullptr);
    block_allocator *alloc = ar->allocator;

    // Create significant fragmentation.
    make_fragmented(alloc, 20, 128);

    uint8_t frag_before = alloc->get_fragmentation();

    // maintenance() should trigger defrag because threshold=1 is exceeded.
    alloc->maintenance();

    uint8_t frag_after = alloc->get_fragmentation();

    // Fragmentation should drop (or stay at 0) after maintenance.
    EXPECT_LE(frag_after, frag_before)
        << "maintenance() should reduce fragmentation when threshold is exceeded";

    compio_close_archive(ar);
}

TEST_F(MaintenanceAutoTriggerTest, MaintenanceDoesNotTriggerBelowThreshold) {
    // Very high threshold — maintenance should NOT defragment.
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.fragmentation_threshold = 99;
    cfg.fill_holes_with_zeros   = false;

    compio_archive *ar = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(ar, nullptr);
    block_allocator *alloc = ar->allocator;

    // Mild fragmentation — likely below threshold=99.
    make_fragmented(alloc, 6, 128);

    uint8_t frag_before = alloc->get_fragmentation();

    alloc->maintenance();

    uint8_t frag_after = alloc->get_fragmentation();

    // With threshold=99, fragmentation below 99% should NOT trigger defrag.
    EXPECT_EQ(frag_after, frag_before)
        << "maintenance() should not defragment when fragmentation is below threshold";

    compio_close_archive(ar);
}

// ---------------------------------------------------------------------------
// StatePersistencePerStrategyTest
// Allocator state survives save/reload regardless of strategy.
// ---------------------------------------------------------------------------
class StatePersistencePerStrategyTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override { remove(fn); }
};

TEST_F(StatePersistencePerStrategyTest, BestFitStateRoundTrip) {
    uint64_t hole_offset = UINT64_MAX;
    const uint64_t hole_size = 200;

    {
        compio_config cfg;
        compio_build_default_config(&cfg);
        cfg.allocation_strategy = COMPIO_ALLOC_BEST_FIT;
        compio_archive *ar = compio_open_archive(fn, "w+", &cfg);
        ASSERT_NE(ar, nullptr);

        hole_offset = ar->allocator->allocate(hole_size);
        ar->allocator->allocate(64); // sentinel
        ar->allocator->deallocate(hole_offset, hole_size);

        compio_close_archive(ar);
    }

    {
        compio_config cfg;
        compio_build_default_config(&cfg);
        cfg.allocation_strategy = COMPIO_ALLOC_BEST_FIT;
        compio_archive *ar = compio_open_archive(fn, "r+", &cfg);
        ASSERT_NE(ar, nullptr);

        // Reuse of the freed hole should still be possible.
        uint64_t reused = ar->allocator->allocate(hole_size);
        EXPECT_EQ(reused, hole_offset)
            << "BEST_FIT: freed hole should be reused after reload";

        compio_close_archive(ar);
    }
}
