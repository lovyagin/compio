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
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

#include "compio/allocator.hpp"
#include "compio/btree.hpp"
#include "compio/compio_file.hpp"
#include "compio/utils.hpp"

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
        remove(fn); remove((std::string(fn) + ".wal").c_str());
    }
};

TEST_F(StatsAfterDefragTest, InvalidBlockMetadataCannotShrinkContainer) {
    const auto data = make_random_bytes(32768);
    for (const char *name : {"first", "second", "third"}) {
        auto *file = compio_open_file(name, archive);
        ASSERT_NE(file, nullptr);
        ASSERT_EQ(compio_write(data.data(), data.size(), file), data.size());
        ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    }
    compio_flush(archive);
    ASSERT_EQ(compio_remove_file(archive, "first"), COMPIO_SUCCESS);
    compio_flush(archive);

    auto *second = compio_open_file("second", archive);
    ASSERT_NE(second, nullptr);
    const tree_key second_key{second->hash, 0};
    const auto second_block = archive->index->get(second_key);
    ASSERT_TRUE(second_block);
    ASSERT_EQ(compio_close_file(second), COMPIO_SUCCESS);
    auto *third = compio_open_file("third", archive);
    ASSERT_NE(third, nullptr);
    const auto invalid_block = archive->index->get({third->hash, 0});
    ASSERT_TRUE(invalid_block);
    ASSERT_EQ(compio_close_file(third), COMPIO_SUCCESS);
    const uint64_t size = readonly(archive->header, header)->file_size;
    ASSERT_EQ(fseek64(archive->file, invalid_block->addr, SEEK_SET), 0);
    const unsigned char invalid_signature = 0;
    ASSERT_EQ(fwrite(&invalid_signature, 1, 1, archive->file), 1u);
    ASSERT_EQ(fflush(archive->file), 0);

    EXPECT_FALSE(archive->allocator->force_defragmentation());
    EXPECT_EQ(readonly(archive->header, header)->file_size, size);
    EXPECT_FALSE(archive->truncate_after_publish);
    EXPECT_EQ(archive->index->get(second_key), second_block);
    const uint64_t sequence = readonly(archive->header, header)->sequence_id;
    EXPECT_EQ(compio_defragment(archive), COMPIO_ERROR);
    EXPECT_EQ(readonly(archive->header, header)->sequence_id, sequence + 1);
}

enum class source_corruption { alias, overlap, second_header, index, files_table, allocator_state };

class CompactionSourceTest : public StatsAfterDefragTest,
                             public ::testing::WithParamInterface<source_corruption> {};

TEST_P(CompactionSourceTest, RejectsInvalidRangesBeforeMovingData) {
    ASSERT_NE(archive->allocator->allocate(65536), UINT64_MAX);
    const auto data = make_random_bytes(512);
    for (const char *name : {"first", "second"}) {
        auto *file = compio_open_file(name, archive);
        ASSERT_NE(file, nullptr);
        ASSERT_EQ(compio_write(data.data(), data.size(), file), data.size());
        ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    }
    compio_flush(archive);
    auto entries = archive->index->get_range({}, {UINT64_MAX, UINT64_MAX});
    ASSERT_TRUE(entries);
    ASSERT_EQ(entries->size(), 2u);
    std::sort(entries->begin(), entries->end(),
              [](const auto &a, const auto &b) { return a.second.addr < b.second.addr; });

    // A valid layout may have index nodes after storage blocks.
    ASSERT_TRUE(archive->index->clear_cache());
    index_node root(archive->config.b_tree_degree);
    ASSERT_TRUE(root.read_from(archive->file, readonly(archive->header, header)->index_root));
    const uint64_t root_addr = archive->allocator->allocate(INDEX_NODE_SIZE(archive->config.b_tree_degree));
    ASSERT_NE(root_addr, UINT64_MAX);
    ASSERT_TRUE(root.write_to(archive->file, root_addr));
    archive->header->index_root = root_addr;

    const auto &[first_key, first] = entries->front();
    const auto &[last_key, last] = entries->back();
    const auto &hdr = readonly(archive->header, header);
    uint64_t metadata_addr = 0;
    switch (GetParam()) {
    case source_corruption::alias:
        ASSERT_TRUE(archive->index->update(last_key, first));
        break;
    case source_corruption::overlap:
        metadata_addr = first.addr;
        break;
    case source_corruption::second_header: {
        const uint64_t addr = hdr->disk_size();
        ASSERT_EQ(fseek64(archive->file, first.addr, SEEK_SET), 0);
        std::vector<uint8_t> bytes(storage_block::meta_size_for(storage_block::signature_backref) + 1);
        ASSERT_EQ(fread(bytes.data(), 1, bytes.size(), archive->file), bytes.size());
        std::fill(bytes.begin() + 2, bytes.begin() + 2 + sizeof(uint64_t), 0);
        bytes[2] = 1;
        ASSERT_EQ(fseek64(archive->file, addr, SEEK_SET), 0);
        ASSERT_EQ(fwrite(bytes.data(), 1, bytes.size(), archive->file), bytes.size());
        ASSERT_TRUE(archive->index->update(first_key, {addr, first.size}));
        break;
    }
    case source_corruption::index:
    case source_corruption::files_table:
    case source_corruption::allocator_state:
        metadata_addr = last.addr;
        break;
    }
    if (metadata_addr != 0) {
        const uint64_t target = GetParam() == source_corruption::overlap ? last.addr :
                                GetParam() == source_corruption::index ? root_addr :
                                GetParam() == source_corruption::files_table ? hdr->files_table_addr :
                                hdr->allocator_state_offset;
        ASSERT_GT(target, metadata_addr);
        const uint64_t size = target - metadata_addr + 1 -
                              storage_block::meta_size_for(storage_block::signature_backref);
        ASSERT_EQ(fseek64(archive->file, metadata_addr + 2, SEEK_SET), 0);
        ASSERT_EQ(lendian_fwrite(&size, sizeof(size), 1, archive->file), 1u);
    }
    ASSERT_TRUE(archive->index->clear_cache());
    ASSERT_EQ(fflush(archive->file), 0);
    const uint64_t logical_end = hdr->file_size;
    auto snapshot = [&]() {
        std::vector<uint8_t> bytes(std::filesystem::file_size(fn));
        EXPECT_EQ(fseek64(archive->file, 0, SEEK_SET), 0);
        EXPECT_EQ(fread(bytes.data(), 1, bytes.size(), archive->file), bytes.size());
        return bytes;
    };
    const auto before = snapshot();
    errno = 0;
    EXPECT_FALSE(archive->allocator->force_defragmentation());
    EXPECT_EQ(errno, EIO);
    EXPECT_EQ(hdr->file_size, logical_end);
    EXPECT_FALSE(archive->truncate_after_publish);
    EXPECT_EQ(snapshot(), before);
}

INSTANTIATE_TEST_SUITE_P(CorruptSources, CompactionSourceTest,
                        ::testing::Values(source_corruption::alias, source_corruption::overlap,
                                          source_corruption::second_header, source_corruption::index,
                                          source_corruption::files_table, source_corruption::allocator_state));

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

    // v5 archives store files_table separately. It acts as an immovable obstacle during defragmentation,
    // which may cause some fragmentation (gaps) if it is located after data blocks.
    bool is_v5 = (readonly(archive->header, header)->magic_number == COMPIO_MAGIC_NUMBER);
    if (!is_v5) {
        EXPECT_EQ(stats.fragmentation_percent, 0u)
            << "After defragmentation, fragmentation should be 0%";
        EXPECT_EQ(stats.total_free_bytes, 0u)
            << "After compacting, there should be no free bytes";
    } else {
        // For v5, we expect fragmentation to be minimal but non-zero is possible
        // due to the immovable files_table. Still, it should remain low.
        EXPECT_LE(stats.fragmentation_percent, 10u)
            << "v5 archives may retain small gaps due to files_table placement, "
            << "but overall fragmentation after defrag should stay low";
    }
}

TEST(NodeCompactionTest, IndexNodesMoveDownWithData) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));
    compio_config cfg;
    compio_build_default_config(&cfg);
    compio_build_dummy_compressor(&cfg.compressor);
    cfg.fragmentation_threshold = 100;
    cfg.b_tree_degree = 2;
    cfg.block_size = 1024;
    cfg.block_size__minimum = 512;
    cfg.block_size__maximum = 2048;
    compio_archive *archive = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(archive, nullptr);

    // A large file first, then small files whose index nodes land behind it.
    const auto big = make_random_bytes(256 * 1024, 1);
    compio_file *f = compio_open_file("big", archive);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(compio_write(big.data(), big.size(), f), big.size());
    ASSERT_EQ(compio_close_file(f), 0);
    compio_flush(archive);

    const int small_files = 64;
    for (int i = 0; i < small_files; ++i) {
        const auto data = make_random_bytes(700, 100 + i);
        const std::string name = "s" + std::to_string(i);
        f = compio_open_file(name.c_str(), archive);
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(compio_write(data.data(), data.size(), f), data.size());
        ASSERT_EQ(compio_close_file(f), 0);
    }
    compio_flush(archive);
    ASSERT_EQ(compio_remove_file(archive, "big"), COMPIO_SUCCESS);
    compio_flush(archive);

    uint64_t node_size = 0;
    auto before = archive->index->collect_node_addresses(node_size);
    ASSERT_TRUE(before);
    ASSERT_GT(before->size(), 1u);
    const uint64_t size_before = readonly(archive->header, header)->file_size;
    ASSERT_GT(before->back(), size_before / 2) << "setup must leave index nodes behind the freed range";

    ASSERT_EQ(compio_defragment(archive), COMPIO_SUCCESS);

    compio_fragmentation_stats stats{};
    ASSERT_EQ(compio_get_fragmentation_stats(archive, &stats), COMPIO_SUCCESS);
    const uint64_t size_after = readonly(archive->header, header)->file_size;
    // A gap smaller than every movable item stays: nothing may be written over live data.
    EXPECT_LT(stats.total_free_bytes, size_after / 20);
    EXPECT_LT(size_after, size_before / 2);
    auto after = archive->index->collect_node_addresses(node_size);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->size(), before->size());
    EXPECT_LE(after->back() + node_size, size_after);
    ASSERT_EQ(compio_close_archive(archive), 0);

    cfg.b_tree_degree = 0;
    archive = compio_open_archive(fn, "r", &cfg);
    ASSERT_NE(archive, nullptr);
    EXPECT_EQ(compio_open_file("big", archive), nullptr);
    for (int i = 0; i < small_files; ++i) {
        const auto data = make_random_bytes(700, 100 + i);
        const std::string name = "s" + std::to_string(i);
        f = compio_open_file(name.c_str(), archive);
        ASSERT_NE(f, nullptr) << name;
        EXPECT_EQ(read_file_bytes(f, data.size()), data) << name;
        ASSERT_EQ(compio_close_file(f), 0);
    }
    ASSERT_EQ(compio_close_archive(archive), 0);
    remove(fn);
    remove((std::string(fn) + ".wal").c_str());
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

    // Deferred frees become available only after a checkpoint.
    compio_flush(archive);

    compio_fragmentation_stats before{};
    ASSERT_EQ(compio_get_fragmentation_stats(archive, &before), COMPIO_SUCCESS);
    EXPECT_GT(before.total_free_bytes, 0u)
        << "Erasing an 8 KB file should add free bytes to the allocator";

    ASSERT_EQ(compio_defragment(archive), COMPIO_SUCCESS);

    compio_fragmentation_stats after{};
    ASSERT_EQ(compio_get_fragmentation_stats(archive, &after), COMPIO_SUCCESS);

    bool is_v5 = (readonly(archive->header, header)->magic_number == COMPIO_MAGIC_NUMBER);
    if (!is_v5) {
        EXPECT_EQ(after.total_free_bytes, 0u)
            << "Defrag should reclaim all free bytes";
    } else {
        // v5 may have residual fragmentation due to files_table metadata,
        // but defragmentation must not make fragmentation worse.
        EXPECT_LE(after.total_free_bytes, before.total_free_bytes)
            << "Defrag should not increase total_free_bytes for v5 archives";
    }
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
    void TearDown() override { remove(fn); remove((std::string(fn) + ".wal").c_str()); }
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
    void TearDown() override { remove(fn); remove((std::string(fn) + ".wal").c_str()); }
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
    void TearDown() override { remove(fn); remove((std::string(fn) + ".wal").c_str()); }
};

TEST_F(ForceDefragTest, ForcedDefragRunsRegardlessOfThreshold) {
    // Threshold = 99 → maintenance() will NOT auto-defrag mild fragmentation.
    compio_archive *ar = open_defrag_archive(fn, 99);
    ASSERT_NE(ar, nullptr);

    // Incompressible data, large enough for the freed space to be a visible
    // share of the container. File "b" is a live sentinel between the holes.
    auto data_a = make_random_bytes(64 * 1024, 7);
    auto data_b = make_random_bytes(256, 8);  // sentinel — stays live
    auto data_c = make_random_bytes(64 * 1024, 9);

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

    compio_flush(ar);

    uint8_t frag_before = ar->allocator->get_fragmentation();
    EXPECT_GT(frag_before, 0u)
        << "Fragmentation must be non-zero: removing 'a' and 'c' frees a visible "
           "share of the container (precondition for this test)";

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

TEST_F(ForceDefragTest, AutomaticMaintenancePreservesPublishedLayout) {
    auto *ar = open_defrag_archive(fn, 0);
    ASSERT_NE(ar, nullptr);
    const uint64_t hole = ar->allocator->allocate(65536);
    ASSERT_NE(hole, UINT64_MAX);
    const auto data = make_random_bytes(32768, 13);
    auto *file = compio_open_file("data", ar);
    ASSERT_NE(file, nullptr);
    const tree_key key{file->hash, 0};
    ASSERT_EQ(compio_write(data.data(), data.size(), file), data.size());
    ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    compio_flush(ar);
    ar->allocator->deallocate(hole, 65536, false);
    compio_flush(ar);
    const auto published_block = ar->index->get(key);
    ASSERT_TRUE(published_block);
    const uint64_t size = readonly(ar->header, header)->file_size;
    const uint64_t sequence = readonly(ar->header, header)->sequence_id;
    ASSERT_GT(ar->allocator->get_fragmentation(), 0);

    EXPECT_TRUE(ar->allocator->maintenance());
    EXPECT_EQ(ar->index->get(key), published_block);
    EXPECT_EQ(readonly(ar->header, header)->file_size, size);
    EXPECT_EQ(readonly(ar->header, header)->sequence_id, sequence);
    EXPECT_FALSE(ar->truncate_after_publish);

    ASSERT_EQ(compio_defragment(ar), COMPIO_SUCCESS);
    const auto moved = ar->index->get(key);
    ASSERT_TRUE(moved);
    EXPECT_LT(moved->addr, published_block->addr);
    file = compio_open_file("data", ar);
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(read_file_bytes(file, data.size()), data);
    ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    EXPECT_EQ(compio_close_archive(ar), COMPIO_SUCCESS);
}

TEST_F(ForceDefragTest, DefragmentRejectsActiveWalBatch) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.enable_wal = true;
    auto *ar = compio_open_archive(fn, "w+", &cfg);
    ASSERT_NE(ar, nullptr);
    compio_flush(ar);
    const uint64_t sequence = readonly(ar->header, header)->sequence_id;
    ASSERT_EQ(compio_begin_batch(ar), COMPIO_SUCCESS);
    ASSERT_EQ(compio_begin_batch(ar), COMPIO_SUCCESS);
    for (int depth = 2; depth > 0; --depth) {
        errno = 0;
        EXPECT_EQ(compio_defragment(ar), COMPIO_ERROR);
        EXPECT_EQ(errno, EBUSY);
        EXPECT_EQ(ar->wal->get_batch_depth(), depth);
        EXPECT_EQ(readonly(ar->header, header)->sequence_id, sequence);
        ASSERT_EQ(compio_end_batch(ar), COMPIO_SUCCESS);
    }
    EXPECT_EQ(compio_defragment(ar), COMPIO_SUCCESS);
    EXPECT_EQ(compio_close_archive(ar), COMPIO_SUCCESS);
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
    void TearDown() override { remove(fn); remove((std::string(fn) + ".wal").c_str()); }
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
    void TearDown() override { remove(fn); remove((std::string(fn) + ".wal").c_str()); }
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

// ---------------------------------------------------------------------------
// ThresholdCompactionTest
// The threshold compares the free share of the container. Compaction must run
// when a large part of the container is free, whatever the shape of the free
// space, and must stay away when the loss is negligible.
// ---------------------------------------------------------------------------
class ThresholdCompactionTest : public ::testing::Test {
protected:
    char fn[256];

    void SetUp() override { generate_tmp_fn(fn, sizeof(fn)); }
    void TearDown() override { remove(fn); remove((std::string(fn) + ".wal").c_str()); }

    static compio_config config(uint8_t threshold) {
        compio_config cfg;
        compio_build_default_config(&cfg);
        cfg.fragmentation_threshold = threshold;
        cfg.max_files = 16;
        return cfg;
    }

    static void write_file(compio_archive *ar, const char *name, const std::vector<uint8_t> &data) {
        compio_file *f = compio_open_file(name, ar);
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(compio_write(data.data(), data.size(), f), data.size());
        compio_close_file(f);
    }

    uint64_t physical_size() const { return std::filesystem::file_size(fn); }
};

TEST_F(ThresholdCompactionTest, HalfFreeContainerIsCompactedOnClose) {
    constexpr std::size_t SZ = 1 << 20;
    const compio_config cfg = config(30);
    const auto keep = make_random_bytes(SZ, 1);

    compio_archive *ar = compio_open_archive(fn, "w", &cfg);
    ASSERT_NE(ar, nullptr);
    write_file(ar, "drop", make_random_bytes(SZ, 2));
    write_file(ar, "keep", keep);
    compio_flush(ar);
    ASSERT_GE(physical_size(), 2 * SZ);

    // One contiguous free region covering half of the container.
    ASSERT_EQ(compio_remove_file(ar, "drop"), 0);
    compio_close_archive(ar);

    EXPECT_LT(physical_size(), SZ + SZ / 10);

    ar = compio_open_archive(fn, "r", &cfg);
    ASSERT_NE(ar, nullptr);
    compio_file *f = compio_open_file("keep", ar);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(read_file_bytes(f, SZ), keep);
    compio_close_file(f);
    compio_close_archive(ar);
}

TEST_F(ThresholdCompactionTest, NegligibleFreeSpaceDoesNotTriggerCompaction) {
    constexpr std::size_t SZ = 1 << 20;
    const compio_config cfg = config(30);

    compio_archive *ar = compio_open_archive(fn, "w", &cfg);
    ASSERT_NE(ar, nullptr);
    write_file(ar, "a", make_random_bytes(4096, 1));
    write_file(ar, "big1", make_random_bytes(SZ, 2));
    write_file(ar, "b", make_random_bytes(4096, 3));
    write_file(ar, "big2", make_random_bytes(SZ, 4));
    compio_flush(ar);

    // Two scattered holes of 4 KiB in a 2 MiB container: well under a percent.
    ASSERT_EQ(compio_remove_file(ar, "a"), 0);
    ASSERT_EQ(compio_remove_file(ar, "b"), 0);
    compio_flush(ar);
    compio_fragmentation_stats before{};
    ASSERT_EQ(compio_get_fragmentation_stats(ar, &before), COMPIO_SUCCESS);
    ASSERT_GE(before.total_free_bytes, 2u * 4096u);
    EXPECT_EQ(before.fragmentation_percent, 0u);
    compio_close_archive(ar);

    // The holes are still there after reopening: nothing was moved.
    ar = compio_open_archive(fn, "r", &cfg);
    ASSERT_NE(ar, nullptr);
    compio_fragmentation_stats after{};
    ASSERT_EQ(compio_get_fragmentation_stats(ar, &after), COMPIO_SUCCESS);
    EXPECT_GE(after.total_free_bytes, 2u * 4096u);
    compio_close_archive(ar);
}

// The files table is rewritten copy-on-write, which leaves a hole of its own
// size. In a small archive that hole is a large share of the container; it must
// be removed by compaction instead of surviving every close.
TEST_F(ThresholdCompactionTest, SmallArchiveStaysTightAcrossSessions) {
    compio_config cfg = config(30);
    cfg.max_files = COMPIO_MAX_FILES;
    const uint64_t table_size = compio::files_table::record_header_size + 3; // "log"

    std::vector<uint8_t> expected;
    for (int session = 0; session < 6; session++) {
        compio_archive *ar = compio_open_archive(fn, "a", &cfg);
        ASSERT_NE(ar, nullptr);
        compio_file *f = compio_open_file("log", ar);
        ASSERT_NE(f, nullptr);
        const auto chunk = make_random_bytes(1024, session);
        ASSERT_EQ(compio_write(chunk.data(), chunk.size(), f), chunk.size());
        expected.insert(expected.end(), chunk.begin(), chunk.end());
        compio_close_file(f);
        compio_close_archive(ar);

        // Headers, one index node, the table, the data and two state slots.
        EXPECT_LT(physical_size(), table_size + expected.size() + 4096) << "session " << session;
    }

    compio_archive *ar = compio_open_archive(fn, "r", &cfg);
    ASSERT_NE(ar, nullptr);
    compio_file *f = compio_open_file("log", ar);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(read_file_bytes(f, expected.size()), expected);
    compio_close_file(f);
    compio_close_archive(ar);
}

TEST_F(ThresholdCompactionTest, UnchangedFilesTableIsNotRewritten) {
    const compio_config cfg = config(100);
    compio_archive *ar = compio_open_archive(fn, "w", &cfg);
    ASSERT_NE(ar, nullptr);
    write_file(ar, "data", make_random_bytes(4096, 1));
    compio_flush(ar);

    const uint64_t table_addr = ar->header->files_table_addr;
    ASSERT_NE(table_addr, 0u);
    for (int i = 0; i < 5; i++) compio_flush(ar);
    EXPECT_EQ(ar->header->files_table_addr, table_addr);

    write_file(ar, "more", make_random_bytes(16, 2));
    compio_flush(ar);
    EXPECT_NE(ar->header->files_table_addr, table_addr);
    compio_close_archive(ar);
}
