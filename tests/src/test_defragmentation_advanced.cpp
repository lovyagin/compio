/**
 * @file test_defragmentation_advanced.cpp
 * @brief Advanced defragmentation tests covering gaps identified in review:
 *
 *  - Fragmentation stats API accuracy (num_free_regions, total_free_bytes,
 *    largest/smallest region, avg) after defragmentation.
 *  - Verify data blocks actually move (file shrinks in bytes after defrag).
 *  - Data integrity with incompressible (random) content.
 *  - Defragmentation on an archive with a single multi-block file.
 *  - Repeated write/erase/defrag cycles leave a healthy archive.
 *  - force_defragmentation() vs maintenance() — forced path works even when
 *    fragmentation is below the configured threshold.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"

#include "test_util.hpp"

using namespace compio;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static compio_archive *open_defrag_archive(const char *fn,
                                            uint8_t threshold = 100,
                                            bool zeros = false) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.fragmentation_threshold = threshold;
    cfg.fill_holes_with_zeros   = zeros;
    return compio_open_archive(fn, "w+", &cfg);
}

static std::vector<uint8_t> make_random_bytes(std::size_t n, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto &b : v) b = static_cast<uint8_t>(rng() & 0xFF);
    return v;
}

static std::vector<uint8_t> read_file_bytes(compio_file *f, std::size_t size) {
    std::vector<uint8_t> buf(size);
    compio_seek(f, 0, COMPIO_SEEK_SET);
    auto bytes_read = compio_read(buf.data(), size, f);
    EXPECT_EQ(bytes_read, size) << "Short read in read_file_bytes: expected " << size
                                << " bytes, got " << bytes_read;
    return buf;
}

// ---------------------------------------------------------------------------
// StatsAfterDefragTest
// Fragmentation stats should show 0 free regions and 0% fragmentation after
// a full defragmentation of an otherwise empty archive.
// ---------------------------------------------------------------------------
class StatsAfterDefragTest : public ::testing::Test {
protected:
    char fn[256];
    compio_archive *archive = nullptr;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));
        archive = open_defrag_archive(fn, 100 /*disable auto*/);
        ASSERT_NE(archive, nullptr);
    }

    void TearDown() override {
        if (archive) compio_close_archive(archive);
        remove(fn);
    }
};

TEST_F(StatsAfterDefragTest, ZeroFreeRegionsAfterFullDefrag) {
    // Write 4 files, erase 2, defrag.
    const int N = 4;
    const std::size_t SZ = 512;
    std::vector<uint8_t> payload(SZ, 0xAB);

    for (int i = 0; i < N; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "f%d", i);
        compio_file *f = compio_open_file(name, archive);
        ASSERT_NE(f, nullptr);
        compio_write(payload.data(), SZ, f);
        compio_close_file(f);
    }

    // Flush caches so blocks get real on-disk addresses before removal.
    // compio_remove_file skips deallocation for blocks with addr==0.
    compio_flush(archive);

    compio_remove_file(archive, "f1");
    compio_remove_file(archive, "f3");

    ASSERT_EQ(compio_defragment(archive), COMPIO_SUCCESS);

    compio_fragmentation_stats stats{};
    ASSERT_EQ(compio_get_fragmentation_stats(archive, &stats), COMPIO_SUCCESS);

    EXPECT_EQ(stats.fragmentation_percent, 0u)
        << "After defragmentation, fragmentation should be 0%";
    EXPECT_EQ(stats.total_free_bytes, 0u)
        << "After compacting, there should be no free bytes";
}

TEST_F(StatsAfterDefragTest, TotalFreeMatchesEraseBeforeDefrag) {
    // Use enough data to guarantee multiple storage blocks are allocated,
    // so that erasing leaves measurable free space tracked in the allocator.
    const std::size_t SZ = 8192; // 8 KB > default block_size (4 KB)
    // Use incompressible random data so blocks occupy real physical space on disk.
    auto payload = make_random_bytes(SZ, 77);

    compio_file *f1 = compio_open_file("a", archive);
    compio_file *f2 = compio_open_file("b", archive);
    ASSERT_NE(f1, nullptr);
    ASSERT_NE(f2, nullptr);
    compio_write(payload.data(), SZ, f1);
    compio_write(payload.data(), SZ, f2);
    compio_close_file(f1);
    compio_close_file(f2);

    // Flush the block cache so that blocks are written to disk with real
    // physical addresses. Without this, blocks stay in the cache with addr=0
    // and compio_remove_file skips deallocation for them.
    compio_flush(archive);

    compio_remove_file(archive, "a");

    compio_fragmentation_stats before{};
    ASSERT_EQ(compio_get_fragmentation_stats(archive, &before), COMPIO_SUCCESS);
    EXPECT_GT(before.total_free_bytes, 0u)
        << "Erasing an 8 KB file should add free bytes to the allocator";

    ASSERT_EQ(compio_defragment(archive), COMPIO_SUCCESS);

    compio_fragmentation_stats after{};
    ASSERT_EQ(compio_get_fragmentation_stats(archive, &after), COMPIO_SUCCESS);
    EXPECT_EQ(after.total_free_bytes, 0u)
        << "Defrag should reclaim all free bytes";
}

// ---------------------------------------------------------------------------
// FileShrinkTest
// After defragmenting a fragmented archive the physical file size reported in
// the header must be smaller than before (data blocks were compacted).
// ---------------------------------------------------------------------------
class FileShrinkTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override { remove(fn); }
};

TEST_F(FileShrinkTest, HeaderFileSizeShrinks) {
    compio_archive *ar = open_defrag_archive(fn);
    ASSERT_NE(ar, nullptr);

    // Use incompressible random data so that each file occupies significant
    // physical space. Highly compressible content (e.g. 0xCC) shrinks to
    // ~20 bytes per block, making the before/after difference negligible.
    const std::size_t SZ = 4096;

    for (int i = 0; i < 6; ++i) {
        auto payload = make_random_bytes(SZ, static_cast<uint32_t>(i + 1));
        char name[32];
        snprintf(name, sizeof(name), "file%d", i);
        compio_file *f = compio_open_file(name, ar);
        ASSERT_NE(f, nullptr);
        compio_write(payload.data(), SZ, f);
        compio_close_file(f);
    }

    // Force all cached blocks to disk so that blocks get real physical
    // addresses before we remove files. Without this, file_size before
    // defrag reflects an unwritten state and defrag would grow it.
    compio_flush(ar);

    compio_remove_file(ar, "file1");
    compio_remove_file(ar, "file3");
    compio_remove_file(ar, "file5");

    // Measure after erasing (free space now exists) but before defrag.
    uint64_t size_before_defrag = ar->header->file_size;

    ASSERT_EQ(compio_defragment(ar), COMPIO_SUCCESS);

    uint64_t size_after_defrag = ar->header->file_size;

    EXPECT_LT(size_after_defrag, size_before_defrag)
        << "Defragmenting should reduce the logical file size after erasing 3 of 6 files";

    compio_close_archive(ar);
}

// ---------------------------------------------------------------------------
// IncompressibleDataTest
// Defragmentation must preserve data integrity for truly incompressible
// (random) content, not just compressible patterns.
// ---------------------------------------------------------------------------
class IncompressibleDataTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override { remove(fn); }
};

TEST_F(IncompressibleDataTest, RandomDataIntactAfterDefrag) {
    compio_archive *ar = open_defrag_archive(fn);
    ASSERT_NE(ar, nullptr);

    const std::size_t SZ = 2048;

    // Write three files with distinct random content.
    auto d0 = make_random_bytes(SZ, 1);
    auto d1 = make_random_bytes(SZ, 2);
    auto d2 = make_random_bytes(SZ, 3);

    auto write_file = [&](const char *name, const std::vector<uint8_t> &data) {
        compio_file *f = compio_open_file(name, ar);
        ASSERT_NE(f, nullptr);
        compio_write(data.data(), data.size(), f);
        compio_close_file(f);
    };

    write_file("r0", d0);
    write_file("r1", d1);
    write_file("r2", d2);

    // Create fragmentation by erasing the middle file.
    // Flush first so blocks have real addresses and deallocation actually occurs.
    compio_flush(ar);
    compio_remove_file(ar, "r1");

    ASSERT_EQ(compio_defragment(ar), COMPIO_SUCCESS);

    // Verify remaining files.
    auto check = [&](const char *name, const std::vector<uint8_t> &expected) {
        compio_file *f = compio_open_file(name, ar);
        ASSERT_NE(f, nullptr) << "Could not open " << name;
        auto got = read_file_bytes(f, expected.size());
        EXPECT_EQ(got, expected) << "Data mismatch in " << name;
        compio_close_file(f);
    };

    check("r0", d0);
    check("r2", d2);

    compio_close_archive(ar);
}

// ---------------------------------------------------------------------------
// ForceDefragTest
// force_defragmentation() runs even when fragmentation is below the threshold
// that would stop maintenance() from acting.
// ---------------------------------------------------------------------------
class ForceDefragTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override { remove(fn); }
};

TEST_F(ForceDefragTest, ForcedDefragRunsRegardlessOfThreshold) {
    // Threshold = 99 → maintenance() will NOT auto-defrag mild fragmentation.
    compio_archive *ar = open_defrag_archive(fn, 99);
    ASSERT_NE(ar, nullptr);

    // Use incompressible random data of different sizes for "a" and "c" so
    // the two resulting free regions have different sizes, giving variance > 0
    // and thus fragmentation_percent > 0. File "b" is a live sentinel between them.
    auto data_a = make_random_bytes(256, 7);
    auto data_b = make_random_bytes(256, 8);  // sentinel — stays live
    auto data_c = make_random_bytes(512, 9);  // different size → unequal free regions

    auto write_file = [&](const char *name, const std::vector<uint8_t> &data) {
        compio_file *f = compio_open_file(name, ar);
        ASSERT_NE(f, nullptr);
        compio_write(data.data(), data.size(), f);
        compio_close_file(f);
    };

    write_file("a", data_a);
    write_file("b", data_b);
    write_file("c", data_c);

    // Flush so blocks have real on-disk addresses; without this,
    // compio_remove_file skips deallocation and fragmentation stays 0.
    compio_flush(ar);
    compio_remove_file(ar, "a");
    compio_remove_file(ar, "c");

    uint8_t frag_before = ar->allocator->get_fragmentation();
    EXPECT_GT(frag_before, 0u)
        << "Fragmentation must be non-zero: 'a' and 'c' create two differently-sized "
           "free regions around 'b', giving non-zero variance (precondition for this test)";

    // maintenance() should NOT defrag (threshold not exceeded).
    ar->allocator->maintenance();
    uint8_t frag_maintenance = ar->allocator->get_fragmentation();
    EXPECT_EQ(frag_maintenance, frag_before)
        << "maintenance() should not defrag below threshold";

    // force_defragmentation() MUST defrag unconditionally.
    ar->allocator->force_defragmentation();
    uint8_t frag_forced = ar->allocator->get_fragmentation();
    EXPECT_EQ(frag_forced, 0u)
        << "force_defragmentation() must always defrag";

    compio_close_archive(ar);
}

// ---------------------------------------------------------------------------
// MultiBlockFileDefragTest
// A large file that spans many storage blocks must be fully readable after
// defragmentation.
// ---------------------------------------------------------------------------
class MultiBlockFileDefragTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override { remove(fn); }
};

TEST_F(MultiBlockFileDefragTest, LargeFileIntactAfterDefragWithNeighbours) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.block_size            = 512;
    cfg.block_size__minimum   = 256;
    cfg.block_size__maximum   = 1024;
    cfg.fragmentation_threshold = 100;
    cfg.fill_holes_with_zeros = false;
    compio_archive *ar = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(ar, nullptr);

    // Large file: 16 KB (spans ~32 blocks of 512 bytes).
    const std::size_t LARGE = 16 * 1024;
    auto large_data = make_random_bytes(LARGE, 99);

    compio_file *big = compio_open_file("big", ar);
    ASSERT_NE(big, nullptr);
    compio_write(large_data.data(), LARGE, big);
    compio_close_file(big);

    // Neighbour files to create real fragmentation when erased.
    std::vector<uint8_t> small(256, 0xBB);
    compio_file *n1 = compio_open_file("n1", ar);
    compio_file *n2 = compio_open_file("n2", ar);
    ASSERT_NE(n1, nullptr);
    ASSERT_NE(n2, nullptr);
    compio_write(small.data(), small.size(), n1);
    compio_write(small.data(), small.size(), n2);
    compio_close_file(n1);
    compio_close_file(n2);

    // Flush cached blocks so every block has a real on-disk address.
    // This ensures compio_remove_file actually deallocates n1's blocks,
    // creating genuine fragmentation for defrag to compact.
    compio_flush(ar);

    ASSERT_EQ(compio_remove_file(ar, "n1"), COMPIO_SUCCESS);
    ASSERT_EQ(compio_defragment(ar), COMPIO_SUCCESS);

    compio_file *big2 = compio_open_file("big", ar);
    ASSERT_NE(big2, nullptr);
    EXPECT_EQ(compio_get_size(big2), LARGE);
    auto got = read_file_bytes(big2, LARGE);
    EXPECT_EQ(got, large_data) << "16 KB file data must be intact after defrag";
    compio_close_file(big2);

    compio_close_archive(ar);
}

// ---------------------------------------------------------------------------
// RepeatedCycleTest
// Ten write/erase/defrag cycles should leave a stable, fully readable archive.
// ---------------------------------------------------------------------------
class RepeatedCycleTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override { remove(fn); }
};

TEST_F(RepeatedCycleTest, TenCyclesDataIntact) {
    compio_archive *ar = open_defrag_archive(fn);
    ASSERT_NE(ar, nullptr);

    const std::size_t SZ = 256;
    const int FILES_PER_CYCLE = 4;

    for (int cycle = 0; cycle < 10; ++cycle) {
        // Write FILES_PER_CYCLE files.
        for (int i = 0; i < FILES_PER_CYCLE; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "c%di%d", cycle, i);
            std::vector<uint8_t> data(SZ, static_cast<uint8_t>(cycle * 10 + i));
            compio_file *f = compio_open_file(name, ar);
            ASSERT_NE(f, nullptr);
            compio_write(data.data(), SZ, f);
            compio_close_file(f);
        }

        // Flush cached blocks to disk before removing files, so that
        // compio_remove_file can deallocate real on-disk addresses and
        // defragementation actually moves data this cycle.
        compio_flush(ar);

        // Erase alternate files.
        for (int i = 0; i < FILES_PER_CYCLE; i += 2) {
            char name[32];
            snprintf(name, sizeof(name), "c%di%d", cycle, i);
            compio_remove_file(ar, name);
        }

        ASSERT_EQ(compio_defragment(ar), COMPIO_SUCCESS);

        // Verify surviving files.
        for (int i = 1; i < FILES_PER_CYCLE; i += 2) {
            char name[32];
            snprintf(name, sizeof(name), "c%di%d", cycle, i);
            std::vector<uint8_t> expected(SZ, static_cast<uint8_t>(cycle * 10 + i));
            compio_file *f = compio_open_file(name, ar);
            ASSERT_NE(f, nullptr) << "File " << name << " missing after cycle " << cycle;
            auto got = read_file_bytes(f, SZ);
            EXPECT_EQ(got, expected) << "Data mismatch in " << name;
            compio_close_file(f);
        }
    }

    compio_close_archive(ar);
}
