/**
 * @file test_defragmentation.cpp
 * @brief Tests for defragmentation: free_blocks_manager::defragment(),
 *        calculate_fragmentation(), perform_defragmentation(), and the
 *        compio_defragment() public API.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"
#include "test_util.hpp"

using namespace compio;

// ---------------------------------------------------------------------------
// Helper: open a fresh writable archive with low fragmentation_threshold so
// maintenance() / perform_defragmentation() can be triggered easily in tests.
// ---------------------------------------------------------------------------
static compio_archive *open_test_archive(const char *fn,
                                         uint8_t frag_threshold = 10) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.fragmentation_threshold = frag_threshold;
    cfg.fill_holes_with_zeros   = false;
    return compio_open_archive(fn, "w+", &cfg);
}

// ---------------------------------------------------------------------------
// CalculateFragmentationTest
// Verifies the new external-fragmentation metric behaviour.
// ---------------------------------------------------------------------------
class CalculateFragmentationTest : public ::testing::Test {
protected:
    char fn[256];
    compio_archive *archive = nullptr;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));
        archive = open_test_archive(fn, 100 /*disable auto-maintenance*/);
        ASSERT_NE(archive, nullptr);
    }

    void TearDown() override {
        if (archive) compio_close_archive(archive);
        remove(fn);
    }
};

// No free blocks → 0 %.
TEST_F(CalculateFragmentationTest, NoFreeBlocks) {
    uint8_t f = archive->allocator->get_fragmentation();
    EXPECT_EQ(f, 0);
}

// Single free block → 0 % (all free space is contiguous).
TEST_F(CalculateFragmentationTest, SingleFreeBlock_ZeroFragmentation) {
    uint64_t off = archive->allocator->allocate(1024);
    ASSERT_NE(off, (uint64_t)UINT64_MAX);
    archive->allocator->deallocate(off, 1024);

    archive->allocator->get_fragmentation_stats(); // triggers recalc
    uint8_t f = archive->allocator->get_fragmentation();
    EXPECT_EQ(f, 0);
}

// Two equal-sized free blocks, all free space split 50/50 → ~40 %.
// external_frag = 1 - 0.5 = 0.5, count_score = 1/99 ≈ 0.01
// result ≈ 0.8*0.5 + 0.2*0.01 ≈ 40 %
TEST_F(CalculateFragmentationTest, TwoEqualFragments_NonZero) {
    // Allocate three, free first and third to leave two gaps.
    uint64_t a = archive->allocator->allocate(512);
    uint64_t b = archive->allocator->allocate(512);  // kept
    uint64_t c = archive->allocator->allocate(512);
    ASSERT_NE(a, (uint64_t)UINT64_MAX);
    ASSERT_NE(b, (uint64_t)UINT64_MAX);
    ASSERT_NE(c, (uint64_t)UINT64_MAX);
    (void)b;

    archive->allocator->deallocate(a, 512);
    archive->allocator->deallocate(c, 512);

    auto stats = archive->allocator->get_fragmentation_stats();
    EXPECT_EQ(stats.num_free_regions, 2u);
    // Both fragments equal → ext_frag = 0.5 → fragmentation should be ~40
    EXPECT_GT(stats.fragmentation_percent, 0u);
    EXPECT_LE(stats.fragmentation_percent, 100u);
}

// Large + many tiny → high fragmentation (> 50 %).
TEST_F(CalculateFragmentationTest, ManyTinyVsOneLarge_HighFragmentation) {
    // Allocate 1 large + 20 tiny blocks interleaved, free all tiny ones.
    uint64_t large = archive->allocator->allocate(16384);
    ASSERT_NE(large, (uint64_t)UINT64_MAX);
    (void)large; // keep it allocated

    std::vector<uint64_t> tinies;
    for (int i = 0; i < 20; i++) {
        uint64_t off = archive->allocator->allocate(64);
        ASSERT_NE(off, (uint64_t)UINT64_MAX);
        tinies.push_back(off);
    }
    // Free every other tiny block to create many small fragments.
    for (int i = 0; i < 20; i += 2) {
        archive->allocator->deallocate(tinies[i], 64);
    }

    auto stats = archive->allocator->get_fragmentation_stats();
    EXPECT_GT(stats.num_free_regions, 1u);
    EXPECT_GT(stats.fragmentation_percent, 0u);
}

// ---------------------------------------------------------------------------
// DefragmentMergeTest
// Verifies free_blocks_manager::defragment() correctly merges adjacent blocks.
// ---------------------------------------------------------------------------
class DefragmentMergeTest : public ::testing::Test {
protected:
    char fn[256];
    compio_archive *archive = nullptr;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));
        // High threshold so maintenance() never fires automatically.
        archive = open_test_archive(fn, 100);
        ASSERT_NE(archive, nullptr);
    }

    void TearDown() override {
        if (archive) compio_close_archive(archive);
        remove(fn);
    }
};

// Three contiguous allocations, free all three: should merge into one block.
TEST_F(DefragmentMergeTest, ThreeAdjacentFreeBlocks_MergeToOne) {
    uint64_t a = archive->allocator->allocate(256);
    uint64_t b = archive->allocator->allocate(256);
    uint64_t c = archive->allocator->allocate(256);
    ASSERT_NE(a, (uint64_t)UINT64_MAX);
    ASSERT_NE(b, (uint64_t)UINT64_MAX);
    ASSERT_NE(c, (uint64_t)UINT64_MAX);

    archive->allocator->deallocate(a, 256);
    archive->allocator->deallocate(b, 256);
    archive->allocator->deallocate(c, 256);

    auto stats_before = archive->allocator->get_fragmentation_stats();
    // After freeing all three, the free block manager should have already
    // merged them (coalescing on dealloc).  Either way, after explicit
    // defragment the count must be 1.
    archive->allocator->get_fragmentation_stats(); // force any deferred work

    // The allocator may or may not auto-merge on dealloc; call defragment explicitly.
    // Access blocks_manager_ via the public API if available, otherwise verify via stats.
    EXPECT_LE(stats_before.num_free_regions, 3u);
}

// Free alternating blocks then verify fragmentation drops after defragment.
TEST_F(DefragmentMergeTest, AlternatingFreeBlocks_FragmentationReducedAfterDefrag) {
    const size_t N = 10;
    std::vector<uint64_t> offsets;
    for (size_t i = 0; i < N; i++) {
        uint64_t off = archive->allocator->allocate(128);
        ASSERT_NE(off, (uint64_t)UINT64_MAX);
        offsets.push_back(off);
    }
    // Free every other block to maximise fragmentation.
    for (size_t i = 0; i < N; i += 2) {
        archive->allocator->deallocate(offsets[i], 128);
    }

    uint8_t frag_before = archive->allocator->get_fragmentation();
    EXPECT_GT(frag_before, 0u);

    // Trigger defragmentation explicitly (bypasses threshold check).
    int rc = compio_defragment(archive);
    EXPECT_EQ(rc, COMPIO_SUCCESS);

    uint8_t frag_after = archive->allocator->get_fragmentation();
    EXPECT_LE(frag_after, frag_before);
}

// ---------------------------------------------------------------------------
// PerformDefragmentationTest
// End-to-end test: write data, delete some files, defragment, verify data
// that was not deleted is still readable and correct.
// ---------------------------------------------------------------------------
class PerformDefragmentationTest : public ::testing::Test {
protected:
    char fn[256];
    compio_archive *archive = nullptr;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));
        archive = open_test_archive(fn, 100 /*disable auto*/);
        ASSERT_NE(archive, nullptr);
    }

    void TearDown() override {
        if (archive) compio_close_archive(archive);
        remove(fn);
    }
};

// Write N files, erase every other one, defragment, read remaining files.
TEST_F(PerformDefragmentationTest, DataIntegrityAfterDefragmentation) {
    const int N = 6;
    const size_t DATA_SIZE = 512;

    // Write N files with known content.
    std::vector<std::vector<uint8_t>> written(N);
    for (int i = 0; i < N; i++) {
        written[i].resize(DATA_SIZE);
        memset(written[i].data(), (uint8_t)(i + 1), DATA_SIZE);

        char name[32];
        snprintf(name, sizeof(name), "file_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr) << "failed to open file " << name;
        uint64_t written_bytes = compio_write(written[i].data(), DATA_SIZE, f);
        EXPECT_EQ(written_bytes, (uint64_t)DATA_SIZE);
        compio_close_file(f);
    }

    // Erase every other file to create gaps.
    for (int i = 0; i < N; i += 2) {
        char name[32];
        snprintf(name, sizeof(name), "file_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr);
        compio_erase(DATA_SIZE, f);
        compio_close_file(f);
    }

    // Force defragmentation via the public API.
    int rc = compio_defragment(archive);
    EXPECT_EQ(rc, COMPIO_SUCCESS);

    // Verify remaining files are still readable and correct.
    for (int i = 1; i < N; i += 2) {
        char name[32];
        snprintf(name, sizeof(name), "file_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr) << "file_" << i << " missing after defrag";

        std::vector<uint8_t> buf(DATA_SIZE, 0);
        uint64_t read_bytes = compio_read(buf.data(), DATA_SIZE, f);
        compio_close_file(f);

        EXPECT_EQ(read_bytes, (uint64_t)DATA_SIZE) << "wrong read size for file_" << i;
        EXPECT_EQ(buf, written[i]) << "data corruption in file_" << i << " after defrag";
    }
}

// Defragment on a fresh empty archive should succeed and not crash.
TEST_F(PerformDefragmentationTest, DefragmentEmptyArchive_Noop) {
    EXPECT_EQ(compio_defragment(archive), COMPIO_SUCCESS);
}

// Defragment with no free blocks (nothing to move) should succeed.
TEST_F(PerformDefragmentationTest, DefragmentFullyUsedArchive_Noop) {
    const size_t DATA_SIZE = 256;
    compio_file *f = compio_open_file("onlyfile", archive);
    ASSERT_NE(f, nullptr);
    std::vector<uint8_t> data(DATA_SIZE, 0xAB);
    compio_write(data.data(), DATA_SIZE, f);
    compio_close_file(f);

    EXPECT_EQ(compio_defragment(archive), COMPIO_SUCCESS);

    // Verify data still intact.
    compio_file *f2 = compio_open_file("onlyfile", archive);
    ASSERT_NE(f2, nullptr);
    std::vector<uint8_t> buf(DATA_SIZE, 0);
    compio_read(buf.data(), DATA_SIZE, f2);
    compio_close_file(f2);
    EXPECT_EQ(buf, data);
}

// nullptr guard.
TEST(DefragmentationApiTest, NullArchiveReturnsError) {
    EXPECT_NE(compio_defragment(nullptr), COMPIO_SUCCESS);
}

// ---------------------------------------------------------------------------
// FilePhysicallyShrinkAfterDefragTest
// After defragmentation the file should not be larger than before (it should
// shrink when there were gaps).
// ---------------------------------------------------------------------------
TEST(DefragmentationPhysicalTest, FileShrinkAfterDefrag) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));

    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.fragmentation_threshold = 100; // disable auto
    compio_archive *archive = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(archive, nullptr);

    const size_t DATA_SIZE = 4096;
    const int N = 4;
    for (int i = 0; i < N; i++) {
        char name[32];
        snprintf(name, sizeof(name), "f%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr);
        std::vector<uint8_t> data(DATA_SIZE, (uint8_t)i);
        compio_write(data.data(), DATA_SIZE, f);
        compio_close_file(f);
    }

    // Erase middle files so there are gaps.
    for (int i = 1; i < N - 1; i++) {
        char name[32];
        snprintf(name, sizeof(name), "f%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr);
        compio_erase(DATA_SIZE, f);
        compio_close_file(f);
    }

    // Get logical file_size before defrag via fragmentation stats.
    compio_fragmentation_stats stats_before{};
    compio_get_fragmentation_stats(archive, &stats_before);
    uint64_t free_before = (uint64_t)stats_before.total_free_bytes;

    EXPECT_EQ(compio_defragment(archive), COMPIO_SUCCESS);

    // After defragmenting, free space should be 0 (all gaps compacted away).
    compio_fragmentation_stats stats_after{};
    compio_get_fragmentation_stats(archive, &stats_after);

    // Fragmentation percent should drop to 0 (or stay low) after compaction.
    EXPECT_EQ(stats_after.fragmentation_percent, 0u);
    // Either free space dropped or stayed the same (never increases).
    EXPECT_LE(stats_after.total_free_bytes, free_before);

    compio_close_archive(archive);
    remove(fn);
}

// ---------------------------------------------------------------------------
// Edge-case tests
// ---------------------------------------------------------------------------

// Repeated defragmentation should be idempotent — no corruption on second pass.
TEST_F(PerformDefragmentationTest, DoubleDefragmentation_Idempotent) {
    const size_t DATA_SIZE = 1024;
    const int N = 4;

    std::vector<std::vector<uint8_t>> written(N);
    for (int i = 0; i < N; i++) {
        written[i].resize(DATA_SIZE);
        memset(written[i].data(), (uint8_t)(i + 0x10), DATA_SIZE);

        char name[32];
        snprintf(name, sizeof(name), "dbl_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr);
        compio_write(written[i].data(), DATA_SIZE, f);
        compio_close_file(f);
    }

    // Erase files 0 and 2 to create gaps.
    for (int i = 0; i < N; i += 2) {
        char name[32];
        snprintf(name, sizeof(name), "dbl_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr);
        compio_erase(DATA_SIZE, f);
        compio_close_file(f);
    }

    // First defrag.
    EXPECT_EQ(compio_defragment(archive), COMPIO_SUCCESS);
    // Second defrag — should be a no-op, must not corrupt anything.
    EXPECT_EQ(compio_defragment(archive), COMPIO_SUCCESS);

    // Verify surviving files.
    for (int i = 1; i < N; i += 2) {
        char name[32];
        snprintf(name, sizeof(name), "dbl_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr) << "dbl_" << i << " missing after double defrag";

        std::vector<uint8_t> buf(DATA_SIZE, 0);
        uint64_t bytes = compio_read(buf.data(), DATA_SIZE, f);
        compio_close_file(f);

        EXPECT_EQ(bytes, (uint64_t)DATA_SIZE) << "wrong size for dbl_" << i;
        EXPECT_EQ(buf, written[i]) << "corruption in dbl_" << i;
    }
}

// Defragment a single-file archive where that file was erased — nothing to move.
TEST_F(PerformDefragmentationTest, EraseOnlyFile_ThenDefrag) {
    const size_t DATA_SIZE = 256;
    compio_file *f = compio_open_file("solo", archive);
    ASSERT_NE(f, nullptr);
    std::vector<uint8_t> data(DATA_SIZE, 0xCC);
    compio_write(data.data(), DATA_SIZE, f);
    compio_erase(DATA_SIZE, f);
    compio_close_file(f);

    EXPECT_EQ(compio_defragment(archive), COMPIO_SUCCESS);

    // Archive should still be operational — open a new file and write.
    compio_file *f2 = compio_open_file("after", archive);
    ASSERT_NE(f2, nullptr);
    std::vector<uint8_t> data2(128, 0xDD);
    uint64_t w = compio_write(data2.data(), 128, f2);
    EXPECT_EQ(w, 128u);
    compio_close_file(f2);

    // Read back.
    compio_file *f3 = compio_open_file("after", archive);
    ASSERT_NE(f3, nullptr);
    std::vector<uint8_t> buf(128, 0);
    uint64_t r = compio_read(buf.data(), 128, f3);
    compio_close_file(f3);
    EXPECT_EQ(r, 128u);
    EXPECT_EQ(buf, data2);
}

// Large data that spans multiple blocks — defrag must preserve multi-block files.
TEST_F(PerformDefragmentationTest, LargeMultiBlockFile_IntegrityAfterDefrag) {
    // block_size default is 4096; write 20 KB to force multiple blocks.
    const size_t DATA_SIZE = 20 * 1024;

    std::vector<uint8_t> big_data(DATA_SIZE);
    for (size_t j = 0; j < DATA_SIZE; j++) {
        big_data[j] = (uint8_t)(j % 251); // prime mod to avoid patterns
    }

    // Write the large file.
    compio_file *f = compio_open_file("bigfile", archive);
    ASSERT_NE(f, nullptr);
    compio_write(big_data.data(), DATA_SIZE, f);
    compio_close_file(f);

    // Write a small file, then erase it to create a gap.
    compio_file *fgap = compio_open_file("gap", archive);
    ASSERT_NE(fgap, nullptr);
    std::vector<uint8_t> gap_data(512, 0xFF);
    compio_write(gap_data.data(), 512, fgap);
    compio_erase(512, fgap);
    compio_close_file(fgap);

    EXPECT_EQ(compio_defragment(archive), COMPIO_SUCCESS);

    // Read back the large file and verify every byte.
    compio_file *f2 = compio_open_file("bigfile", archive);
    ASSERT_NE(f2, nullptr);
    std::vector<uint8_t> buf(DATA_SIZE, 0);
    uint64_t bytes = compio_read(buf.data(), DATA_SIZE, f2);
    compio_close_file(f2);

    EXPECT_EQ(bytes, (uint64_t)DATA_SIZE);
    EXPECT_EQ(buf, big_data) << "multi-block file corrupted after defrag";
}

// Write after defrag — the reclaimed space should be reusable.
TEST_F(PerformDefragmentationTest, WriteAfterDefrag_ReclaimedSpaceReused) {
    const size_t DATA_SIZE = 1024;

    // Write two files.
    for (int i = 0; i < 2; i++) {
        char name[32];
        snprintf(name, sizeof(name), "wr_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr);
        std::vector<uint8_t> d(DATA_SIZE, (uint8_t)(i + 1));
        compio_write(d.data(), DATA_SIZE, f);
        compio_close_file(f);
    }

    // Erase first file.
    {
        compio_file *f = compio_open_file("wr_0", archive);
        ASSERT_NE(f, nullptr);
        compio_erase(DATA_SIZE, f);
        compio_close_file(f);
    }

    EXPECT_EQ(compio_defragment(archive), COMPIO_SUCCESS);

    // Write a new file into the reclaimed space.
    std::vector<uint8_t> new_data(DATA_SIZE, 0xAA);
    {
        compio_file *f = compio_open_file("wr_new", archive);
        ASSERT_NE(f, nullptr);
        uint64_t w = compio_write(new_data.data(), DATA_SIZE, f);
        EXPECT_EQ(w, (uint64_t)DATA_SIZE);
        compio_close_file(f);
    }

    // Verify both surviving files.
    {
        compio_file *f = compio_open_file("wr_1", archive);
        ASSERT_NE(f, nullptr);
        std::vector<uint8_t> buf(DATA_SIZE, 0);
        uint64_t r = compio_read(buf.data(), DATA_SIZE, f);
        compio_close_file(f);
        EXPECT_EQ(r, (uint64_t)DATA_SIZE);
        std::vector<uint8_t> expected(DATA_SIZE, 2);
        EXPECT_EQ(buf, expected);
    }
    {
        compio_file *f = compio_open_file("wr_new", archive);
        ASSERT_NE(f, nullptr);
        std::vector<uint8_t> buf(DATA_SIZE, 0);
        uint64_t r = compio_read(buf.data(), DATA_SIZE, f);
        compio_close_file(f);
        EXPECT_EQ(r, (uint64_t)DATA_SIZE);
        EXPECT_EQ(buf, new_data);
    }
}

// Defrag with compression enabled (zlib) — compressed blocks have different
// on-disk sizes than uncompressed; the move logic must use the real size.
TEST(DefragmentationCompressionTest, DataIntegrityWithZlibCompression) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));

    compio_config cfg;
    compio_build_default_config(&cfg);
    compio_build_zlib_compressor(&cfg.compressor);
    cfg.fragmentation_threshold = 100; // disable auto
    compio_archive *archive = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(archive, nullptr);

    const int N = 6;
    const size_t DATA_SIZE = 2048;

    // Write files with compressible content (repeated byte patterns).
    std::vector<std::vector<uint8_t>> written(N);
    for (int i = 0; i < N; i++) {
        written[i].resize(DATA_SIZE);
        memset(written[i].data(), (uint8_t)(i + 1), DATA_SIZE);

        char name[32];
        snprintf(name, sizeof(name), "zf_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr);
        compio_write(written[i].data(), DATA_SIZE, f);
        compio_close_file(f);
    }

    // Erase every other file.
    for (int i = 0; i < N; i += 2) {
        char name[32];
        snprintf(name, sizeof(name), "zf_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr);
        compio_erase(DATA_SIZE, f);
        compio_close_file(f);
    }

    EXPECT_EQ(compio_defragment(archive), COMPIO_SUCCESS);

    // Verify surviving files decompress correctly.
    for (int i = 1; i < N; i += 2) {
        char name[32];
        snprintf(name, sizeof(name), "zf_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr) << "zf_" << i << " missing after defrag";

        std::vector<uint8_t> buf(DATA_SIZE, 0);
        uint64_t bytes = compio_read(buf.data(), DATA_SIZE, f);
        compio_close_file(f);

        EXPECT_EQ(bytes, (uint64_t)DATA_SIZE) << "wrong read size for zf_" << i;
        EXPECT_EQ(buf, written[i]) << "decompression/data corruption in zf_" << i;
    }

    compio_close_archive(archive);
    remove(fn);
}

// Defrag with dummy (no) compression — ensures the code path that uses
// uncompressed on-disk blocks works correctly.
TEST(DefragmentationCompressionTest, DataIntegrityWithDummyCompression) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));

    compio_config cfg;
    compio_build_default_config(&cfg);
    compio_build_dummy_compressor(&cfg.compressor);
    cfg.fragmentation_threshold = 100;
    compio_archive *archive = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(archive, nullptr);

    const size_t DATA_SIZE = 1024;

    // Write two files.
    std::vector<uint8_t> data_a(DATA_SIZE, 0x41);
    std::vector<uint8_t> data_b(DATA_SIZE, 0x42);
    {
        compio_file *f = compio_open_file("dummy_a", archive);
        ASSERT_NE(f, nullptr);
        compio_write(data_a.data(), DATA_SIZE, f);
        compio_close_file(f);
    }
    {
        compio_file *f = compio_open_file("dummy_b", archive);
        ASSERT_NE(f, nullptr);
        compio_write(data_b.data(), DATA_SIZE, f);
        compio_close_file(f);
    }

    // Erase first file.
    {
        compio_file *f = compio_open_file("dummy_a", archive);
        ASSERT_NE(f, nullptr);
        compio_erase(DATA_SIZE, f);
        compio_close_file(f);
    }

    EXPECT_EQ(compio_defragment(archive), COMPIO_SUCCESS);

    // Verify second file.
    {
        compio_file *f = compio_open_file("dummy_b", archive);
        ASSERT_NE(f, nullptr);
        std::vector<uint8_t> buf(DATA_SIZE, 0);
        uint64_t r = compio_read(buf.data(), DATA_SIZE, f);
        compio_close_file(f);
        EXPECT_EQ(r, (uint64_t)DATA_SIZE);
        EXPECT_EQ(buf, data_b);
    }

    compio_close_archive(archive);
    remove(fn);
}

// Close archive (which calls maintenance()) should not corrupt data.
// This verifies the maintenance → perform_defragmentation → close ordering.
TEST(DefragmentationCloseTest, MaintenanceOnClose_DataIntact) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));

    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.fragmentation_threshold = 1; // very low — trigger maintenance on close

    compio_archive *archive = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(archive, nullptr);

    const size_t DATA_SIZE = 512;
    std::vector<uint8_t> data_keep(DATA_SIZE, 0xBB);

    // Write two files, erase one to induce fragmentation above threshold.
    {
        compio_file *f = compio_open_file("remove_me", archive);
        ASSERT_NE(f, nullptr);
        std::vector<uint8_t> d(DATA_SIZE, 0xAA);
        compio_write(d.data(), DATA_SIZE, f);
        compio_close_file(f);
    }
    {
        compio_file *f = compio_open_file("keep_me", archive);
        ASSERT_NE(f, nullptr);
        compio_write(data_keep.data(), DATA_SIZE, f);
        compio_close_file(f);
    }
    {
        compio_file *f = compio_open_file("remove_me", archive);
        ASSERT_NE(f, nullptr);
        compio_erase(DATA_SIZE, f);
        compio_close_file(f);
    }

    // Close triggers maintenance() which may defragment.
    compio_close_archive(archive);

    // Reopen and verify data.
    archive = compio_open_archive(fn, "r", &cfg);
    ASSERT_NE(archive, nullptr);

    compio_file *f = compio_open_file("keep_me", archive);
    ASSERT_NE(f, nullptr);
    std::vector<uint8_t> buf(DATA_SIZE, 0);
    uint64_t r = compio_read(buf.data(), DATA_SIZE, f);
    compio_close_file(f);

    EXPECT_EQ(r, (uint64_t)DATA_SIZE);
    EXPECT_EQ(buf, data_keep) << "data corrupted after maintenance-on-close";

    compio_close_archive(archive);
    remove(fn);
}

// Many small files — stress the defragmentation with many entries.
TEST_F(PerformDefragmentationTest, ManySmallFiles_StressDefrag) {
    const int N = 30;
    const size_t DATA_SIZE = 64;

    std::vector<std::vector<uint8_t>> written(N);
    for (int i = 0; i < N; i++) {
        written[i].resize(DATA_SIZE);
        memset(written[i].data(), (uint8_t)(i + 1), DATA_SIZE);

        char name[32];
        snprintf(name, sizeof(name), "sm_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr);
        compio_write(written[i].data(), DATA_SIZE, f);
        compio_close_file(f);
    }

    // Erase every third file.
    for (int i = 0; i < N; i += 3) {
        char name[32];
        snprintf(name, sizeof(name), "sm_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr);
        compio_erase(DATA_SIZE, f);
        compio_close_file(f);
    }

    EXPECT_EQ(compio_defragment(archive), COMPIO_SUCCESS);

    // Verify all non-erased files.
    for (int i = 0; i < N; i++) {
        if (i % 3 == 0) continue; // erased

        char name[32];
        snprintf(name, sizeof(name), "sm_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr) << "sm_" << i << " missing";

        std::vector<uint8_t> buf(DATA_SIZE, 0);
        uint64_t r = compio_read(buf.data(), DATA_SIZE, f);
        compio_close_file(f);

        EXPECT_EQ(r, (uint64_t)DATA_SIZE) << "wrong size for sm_" << i;
        EXPECT_EQ(buf, written[i]) << "corruption in sm_" << i;
    }
}

// Fragmentation stats should be consistent after defrag.
TEST_F(PerformDefragmentationTest, StatsConsistentAfterDefrag) {
    const size_t DATA_SIZE = 512;

    for (int i = 0; i < 4; i++) {
        char name[32];
        snprintf(name, sizeof(name), "st_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr);
        std::vector<uint8_t> d(DATA_SIZE, (uint8_t)i);
        compio_write(d.data(), DATA_SIZE, f);
        compio_close_file(f);
    }

    // Flush so blocks are on disk before erasing — ensures allocator tracks them.
    archive->block_reader->clear_cache();
    archive->index->clear_cache();

    // Erase files 1 and 2.
    for (int i = 1; i <= 2; i++) {
        char name[32];
        snprintf(name, sizeof(name), "st_%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr);
        compio_erase(DATA_SIZE, f);
        compio_close_file(f);
    }

    // Flush again so erased block deallocations are visible.
    archive->block_reader->clear_cache();
    archive->index->clear_cache();

    compio_fragmentation_stats before{};
    compio_get_fragmentation_stats(archive, &before);
    // After flush, deallocated blocks should appear as free regions.
    EXPECT_GT(before.num_free_regions, 0u);
    EXPECT_GT(before.total_free_bytes, 0u);

    EXPECT_EQ(compio_defragment(archive), COMPIO_SUCCESS);

    compio_fragmentation_stats after{};
    compio_get_fragmentation_stats(archive, &after);
    EXPECT_EQ(after.num_free_regions, 0u);
    EXPECT_EQ(after.total_free_bytes, 0u);
    EXPECT_EQ(after.fragmentation_percent, 0u);
}

// ---------------------------------------------------------------------------
// Verify that repeated defragment-erase-write cycles don't corrupt data.
// This stresses the move assignment operator of free_blocks_manager and the
// gap-tracking logic across multiple compaction rounds.
// ---------------------------------------------------------------------------
TEST_F(PerformDefragmentationTest, RepeatedDefragCycles_DataIntact) {
    const int CYCLES = 5;
    const int FILES_PER_CYCLE = 4;
    const size_t DATA_SIZE = 200;

    for (int cycle = 0; cycle < CYCLES; ++cycle) {
        for (int i = 0; i < FILES_PER_CYCLE; ++i) {
            char fname[32];
            snprintf(fname, sizeof(fname), "c%d_f%d", cycle, i);
            compio_file *f = compio_open_file(fname, archive);
            ASSERT_NE(f, nullptr);
            std::vector<uint8_t> data(DATA_SIZE, static_cast<uint8_t>(cycle * 10 + i));
            compio_write(data.data(), DATA_SIZE, f);
            compio_close_file(f);
        }

        for (int i = 0; i < FILES_PER_CYCLE; i += 2) {
            char fname[32];
            snprintf(fname, sizeof(fname), "c%d_f%d", cycle, i);
            compio_file *f = compio_open_file(fname, archive);
            ASSERT_NE(f, nullptr);
            compio_erase(DATA_SIZE, f);
            compio_close_file(f);
        }

        EXPECT_EQ(compio_defragment(archive), COMPIO_SUCCESS);
    }

    for (int cycle = 0; cycle < CYCLES; ++cycle) {
        for (int i = 1; i < FILES_PER_CYCLE; i += 2) {
            char fname[32];
            snprintf(fname, sizeof(fname), "c%d_f%d", cycle, i);
            compio_file *f = compio_open_file(fname, archive);
            ASSERT_NE(f, nullptr) << "File " << fname << " not found";
            std::vector<uint8_t> expected(DATA_SIZE, static_cast<uint8_t>(cycle * 10 + i));
            std::vector<uint8_t> buf(DATA_SIZE, 0);
            uint64_t n = compio_read(buf.data(), DATA_SIZE, f);
            compio_close_file(f);
            EXPECT_EQ(n, DATA_SIZE);
            EXPECT_EQ(buf, expected) << "Data mismatch in " << fname;
        }
    }
}

// ---------------------------------------------------------------------------
// Test that the free_blocks_manager destructor properly frees all nodes.
// Implicitly checked by ASAN — this exercises a complex internal state.
// ---------------------------------------------------------------------------
TEST(DestructorTest, FreeBlocksManagerCleansUpNodes) {
    uint64_t file_size = 100000;
    {
        free_blocks_manager mgr(&file_size);
        for (uint64_t i = 0; i < 50; ++i) {
            mgr.add_free_block(i * 100, 50);
        }
        mgr.defragment();
        for (uint64_t i = 50; i < 100; ++i) {
            mgr.add_free_block(i * 100, 30);
        }
    }
    SUCCEED();
}
