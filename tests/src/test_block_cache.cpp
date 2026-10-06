#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "compio.h"
#include "compio/btree.hpp"
#include "compio/compio_file.hpp"
#include "compio/lru_2q_cache.hpp"
#include "compio/sharded_lru_2q_cache.hpp"
#include "test_util.hpp"

namespace {

struct by_first {
    size_t operator()(const std::pair<int, int> &key) const { return static_cast<size_t>(key.first); }
};
using test_cache = cache::sharded_lru_2q_cache<std::pair<int, int>, int, std::less<>, by_first>;

void put_and_touch(test_cache &c, int shard, int item) {
    c.put({shard, item}, item);
    ASSERT_TRUE(c.get({shard, item}).has_value());
}

using plain_cache = cache::lru_2q_cache<int, int>;

// What a reader does: look the entry up, load it on a miss.
bool access(plain_cache &c, int key) {
    if (c.get(key).has_value()) return true;
    c.put(key, key);
    return false;
}

std::atomic<uint64_t> compress_calls{0};
std::atomic<uint64_t> decompress_calls{0};
compio_compressor inner;

int counting_compress(const compio_compressor *, void *dst, uint64_t *dst_size, const void *src,
                      uint64_t src_size) {
    ++compress_calls;
    return inner.compress(&inner, dst, dst_size, src, src_size);
}
int counting_decompress(const compio_compressor *, void *dst, uint64_t *dst_size, const void *src,
                        uint64_t src_size) {
    ++decompress_calls;
    return inner.decompress(&inner, dst, dst_size, src, src_size);
}
uint64_t counting_bufsize(const compio_compressor *, uint64_t src_size) {
    return inner.get_bufsize(&inner, src_size);
}

compio_config counting_config() {
    compio_config cfg;
    compio_build_default_config(&cfg);
    compio_build_lz4_compressor(&inner);
    cfg.compressor.compress = counting_compress;
    cfg.compressor.decompress = counting_decompress;
    cfg.compressor.get_bufsize = counting_bufsize;
    cfg.compressor.compression_type = COMPIO_COMPRESS_CUSTOM;
    cfg.max_files = 16;
    cfg.block_size = 4096;
    cfg.block_size__minimum = 1024;
    cfg.block_size__maximum = 8192;
    cfg.cache_size__blocks = 1024;
    cfg.fragmentation_threshold = 100;
    return cfg;
}

} // namespace

TEST(TwoQueueCacheTest, KeepsEverythingWhileThereIsRoom) {
    plain_cache c(100);
    for (int i = 0; i < 100; i++) c.put(i, i);

    EXPECT_EQ(c.size(), 100u);
    for (int i = 0; i < 100; i++) EXPECT_TRUE(c.exists(i)) << i;
}

TEST(TwoQueueCacheTest, EntriesSeenOnceLeaveInArrivalOrder) {
    plain_cache c(8);
    for (int i = 0; i < 8; i++) c.put(i, i);
    // Hits do not reorder the queue of entries seen once.
    EXPECT_TRUE(c.get(0).has_value());
    EXPECT_TRUE(c.get(0).has_value());

    c.put(8, 8);
    EXPECT_FALSE(c.exists(0));
    c.put(9, 9);
    EXPECT_FALSE(c.exists(1));
    EXPECT_TRUE(c.exists(2));
    EXPECT_EQ(c.size(), 8u);
}

TEST(TwoQueueCacheTest, ReturningEntryOutlivesAStreamOfNewOnes) {
    plain_cache c(8);
    for (int i = 0; i < 9; i++) c.put(i, i); // 0 is dropped, its key is remembered
    ASSERT_FALSE(c.exists(0));

    c.put(0, 0); // comes back: belongs to the main queue now
    for (int i = 100; i < 200; i++) {
        c.put(i, i);
        ASSERT_TRUE(c.exists(0)) << "evicted while inserting " << i;
    }
    EXPECT_EQ(c.size(), 8u);
}

TEST(TwoQueueCacheTest, ForgottenKeyStartsOver) {
    plain_cache c(8);
    for (int i = 0; i < 9; i++) c.put(i, i);
    ASSERT_FALSE(c.exists(0));

    // Far more new keys than the cache remembers: 0 is forgotten.
    for (int i = 100; i < 200; i++) c.put(i, i);
    c.put(0, 0);
    for (int i = 200; i < 208; i++) c.put(i, i);
    EXPECT_FALSE(c.exists(0));
}

TEST(TwoQueueCacheTest, MainQueueIsLeastRecentlyUsed) {
    plain_cache c(16);
    for (int i = 0; i < 16; i++) c.put(i, i);
    for (int i = 100; i < 116; i++) c.put(i, i); // 0..15 dropped, the last eight remembered
    for (int i = 8; i < 16; i++) c.put(i, i);    // they return: main queue, 8 is the oldest
    for (int i = 8; i < 16; i++) ASSERT_TRUE(c.exists(i)) << i;

    EXPECT_TRUE(c.get(8).has_value()); // now 9 is the oldest

    // More returning entries shrink the seen-once queue to its quarter; from
    // then on the main queue gives up its least recently used entry.
    for (int i = 100; i < 104; i++) c.put(i, i);
    for (int i = 8; i < 16; i++) ASSERT_TRUE(c.exists(i)) << i;

    c.put(104, 104);
    EXPECT_FALSE(c.exists(9));
    c.put(105, 105);
    EXPECT_FALSE(c.exists(10));
    EXPECT_TRUE(c.exists(8));
    EXPECT_TRUE(c.exists(11));
    EXPECT_EQ(c.size(), 16u);
}

TEST(TwoQueueCacheTest, RepeatedPassOverSetThatFitsIsAllHits) {
    plain_cache c(1024);
    for (int i = 0; i < 1024; i++) EXPECT_FALSE(access(c, i));
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < 1024; i++) ASSERT_TRUE(access(c, i)) << "pass " << pass << " key " << i;
    }
}

TEST(TwoQueueCacheTest, HotSetSurvivesOneOffScan) {
    plain_cache c(64);
    // A hot set that settles in the main queue...
    for (int round = 0; round < 6; round++) {
        for (int i = 0; i < 32; i++) access(c, i);
        for (int i = 1000 + round * 100; i < 1000 + round * 100 + 40; i++) access(c, i);
    }
    for (int i = 0; i < 32; i++) ASSERT_TRUE(c.exists(i)) << i;

    // ...is still there after a scan ten times the size of the cache.
    for (int i = 5000; i < 5640; i++) access(c, i);
    for (int i = 0; i < 32; i++) EXPECT_TRUE(c.exists(i)) << i;
}

TEST(TwoQueueCacheTest, ShiftedRangeForgetsRememberedKeys) {
    plain_cache c(8);
    for (int i = 0; i < 9; i++) c.put(i, i);
    ASSERT_FALSE(c.exists(0));

    // Keys 0..3 now name other entries; what was known about key 0 is void.
    c.add_to_range(1000, 0, 3);
    EXPECT_TRUE(c.exists(1001));

    c.put(0, 0);
    for (int i = 100; i < 108; i++) c.put(i, i);
    EXPECT_FALSE(c.exists(0)) << "key 0 was treated as a returning entry";
}

TEST(SharedCacheBudgetTest, SingleShardCanUseWholeCache) {
    test_cache c(128);
    for (int i = 0; i < 128; i++) put_and_touch(c, 7, i);

    EXPECT_EQ(c.size(), 128u);
    for (int i = 0; i < 128; i++) EXPECT_TRUE(c.exists({7, i})) << i;
}

TEST(SharedCacheBudgetTest, TotalSizeNeverExceedsBudget) {
    test_cache c(128);
    for (int shard = 0; shard < 8; shard++) {
        for (int i = 0; i < 100; i++) {
            put_and_touch(c, shard, i);
            ASSERT_LE(c.size(), 128u);
        }
    }
    EXPECT_EQ(c.size(), 128u);
}

TEST(SharedCacheBudgetTest, LeastRecentlyUsedShardIsEvictedFirst) {
    test_cache c(128);
    for (int i = 0; i < 64; i++) put_and_touch(c, 1, i); // goes idle
    for (int i = 0; i < 64; i++) put_and_touch(c, 2, i);

    // Shard 2 keeps working and needs room: it is taken from the idle shard.
    for (int i = 64; i < 128; i++) put_and_touch(c, 2, i);

    for (int i = 0; i < 128; i++) EXPECT_TRUE(c.exists({2, i})) << i;
    for (int i = 0; i < 64; i++) EXPECT_FALSE(c.exists({1, i})) << i;
}

// One logical file used to get 1/64 of cache_size__blocks, because the cache is
// sharded by file and every shard had a fixed share.
TEST(BlockCacheTest, SingleFileUsesWholeBlockCache) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));

    const compio_config cfg = counting_config();

    constexpr size_t TOTAL = 600 * 4096;
    std::vector<uint8_t> data(TOTAL);
    for (size_t i = 0; i < TOTAL; i++) data[i] = static_cast<uint8_t>((i / 7) ^ (i >> 9));

    compio_archive *ar = compio_open_archive(fn, "w", &cfg);
    ASSERT_NE(ar, nullptr);
    compio_file *f = compio_open_file("data", ar);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(compio_write(data.data(), TOTAL, f), TOTAL);
    compio_flush(ar); // empties the cache: the reads below start cold

    auto read_all = [&]() {
        std::vector<uint8_t> chunk(1024);
        compio_seek(f, 0, COMPIO_SEEK_SET);
        for (size_t off = 0; off < TOTAL; off += chunk.size()) {
            ASSERT_EQ(compio_read(chunk.data(), chunk.size(), f), chunk.size());
            ASSERT_EQ(memcmp(chunk.data(), data.data() + off, chunk.size()), 0);
        }
    };

    decompress_calls = 0;
    read_all();
    const uint64_t cold = decompress_calls;
    EXPECT_GE(cold, 600u);
    EXPECT_LE(cold, 1024u) << "more blocks than the cache holds: the test needs retuning";

    decompress_calls = 0;
    read_all();
    EXPECT_EQ(decompress_calls, 0u) << "every block of the file must still be cached";

    compio_close_file(f);
    compio_close_archive(ar);
    remove(fn);
    remove((std::string(fn) + ".wal").c_str());
}

// The file fits in the cache and every block is touched once per pass. Blocks
// seen once used to be limited to a quarter of the cache, so the second pass
// found none of the blocks from the first three quarters of the file.
TEST(BlockCacheTest, RepeatedSequentialReadIsServedFromCache) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));
    const compio_config cfg = counting_config();

    constexpr size_t BLOCKS = 800;
    constexpr size_t TOTAL = BLOCKS * 4096;
    std::vector<uint8_t> data(TOTAL);
    for (size_t i = 0; i < TOTAL; i++) data[i] = static_cast<uint8_t>((i / 3) ^ (i >> 10));

    compio_archive *ar = compio_open_archive(fn, "w", &cfg);
    ASSERT_NE(ar, nullptr);
    compio_file *f = compio_open_file("data", ar);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(compio_write(data.data(), TOTAL, f), TOTAL);
    compio_close_file(f);
    compio_close_archive(ar);

    ar = compio_open_archive(fn, "r", &cfg);
    ASSERT_NE(ar, nullptr);
    f = compio_open_file("data", ar);
    ASSERT_NE(f, nullptr);

    auto read_all = [&]() {
        std::vector<uint8_t> chunk(4096);
        compio_seek(f, 0, COMPIO_SEEK_SET);
        for (size_t off = 0; off < TOTAL; off += chunk.size()) {
            ASSERT_EQ(compio_read(chunk.data(), chunk.size(), f), chunk.size());
            ASSERT_EQ(memcmp(chunk.data(), data.data() + off, chunk.size()), 0);
        }
    };

    decompress_calls = 0;
    read_all();
    EXPECT_EQ(decompress_calls, BLOCKS);

    decompress_calls = 0;
    read_all();
    EXPECT_EQ(decompress_calls, 0u);

    compio_close_file(f);
    compio_close_archive(ar);
    remove(fn);
    remove((std::string(fn) + ".wal").c_str());
}

// An insert shifts the positions of all following blocks. The cached ones used
// to be marked modified by that and were compressed and rewritten again,
// although their data had not changed.
TEST(BlockCacheTest, InsertDoesNotRewriteShiftedBlocks) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));
    const compio_config cfg = counting_config();

    constexpr size_t BLOCKS = 100;
    constexpr size_t TOTAL = BLOCKS * 4096;
    std::vector<uint8_t> data(TOTAL);
    for (size_t i = 0; i < TOTAL; i++) data[i] = static_cast<uint8_t>((i / 5) ^ (i >> 8));

    compio_archive *ar = compio_open_archive(fn, "w", &cfg);
    ASSERT_NE(ar, nullptr);
    compio_file *f = compio_open_file("data", ar);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(compio_write(data.data(), TOTAL, f), TOTAL);
    compio_flush(ar);

    // Bring every block into the cache.
    std::vector<uint8_t> back(TOTAL);
    compio_seek(f, 0, COMPIO_SEEK_SET);
    ASSERT_EQ(compio_read(back.data(), TOTAL, f), TOTAL);

    compress_calls = 0;
    const uint8_t byte = 0xEE;
    compio_seek(f, 0, COMPIO_SEEK_SET);
    ASSERT_EQ(compio_insert(&byte, 1, f), 1u);
    compio_flush(ar);
    EXPECT_LE(compress_calls, 2u) << "only the block that received the byte has new data";

    data.insert(data.begin(), byte);
    back.resize(data.size());
    compio_seek(f, 0, COMPIO_SEEK_SET);
    ASSERT_EQ(compio_read(back.data(), back.size(), f), back.size());
    EXPECT_EQ(back, data);
    compio_close_file(f);
    compio_close_archive(ar);

    // The shifted blocks must also be readable from a cold start.
    ar = compio_open_archive(fn, "r", &cfg);
    ASSERT_NE(ar, nullptr);
    f = compio_open_file("data", ar);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(compio_read(back.data(), back.size(), f), back.size());
    EXPECT_EQ(back, data);
    compio_close_file(f);
    compio_close_archive(ar);

    remove(fn);
    remove((std::string(fn) + ".wal").c_str());
}

// Blocks written out by one flush used to land in arbitrary (in practice
// reverse) order, so a freshly written file read backwards on disk.
TEST(BlockCacheTest, FlushPlacesNewBlocksInLogicalOrder) {
    char fn[256];
    generate_tmp_fn(fn, sizeof(fn));
    const compio_config cfg = counting_config();

    constexpr size_t TOTAL = 64 * 4096;
    std::vector<uint8_t> data(TOTAL);
    for (size_t i = 0; i < TOTAL; i++) data[i] = static_cast<uint8_t>((i / 3) ^ (i >> 10));

    compio_archive *ar = compio_open_archive(fn, "w", &cfg);
    ASSERT_NE(ar, nullptr);
    compio_file *f = compio_open_file("data", ar);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(compio_write(data.data(), TOTAL, f), TOTAL);
    compio_flush(ar);

    auto range = ar->index->get_range({f->hash, 0}, {f->hash, UINT64_MAX});
    ASSERT_TRUE(range.has_value());
    ASSERT_GE(range->size(), 32u);
    for (size_t i = 1; i < range->size(); i++) {
        EXPECT_LT((*range)[i - 1].second.addr, (*range)[i].second.addr)
            << "block at position " << (*range)[i].first.pos << " is placed before its predecessor";
    }

    compio_close_file(f);
    compio_close_archive(ar);
    remove(fn);
    remove((std::string(fn) + ".wal").c_str());
}
