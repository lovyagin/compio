#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

#include "compio.h"
#include "compio/compio_file.hpp"
#include "compio/wal.hpp"
#include "test_util.hpp"

using compio::index_node;
using compio::tree_key;
using compio::tree_val;

namespace {

enum class failure { seek, short_write, flush, sync };

struct io_fault {
    FILE *stream = nullptr;
    int descriptor = -1;
    off_t target = 0;
    failure kind = failure::seek;
    int writes = 0;
    int short_write_call = 2;
    bool publishing = false;
    bool triggered = false;
    bool next_seek = false;
    int wal_record = -1;
};

io_fault fault;

} // namespace

extern "C" {
int __real_fseeko(FILE *, off_t, int);
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
int __real_fflush(FILE *);
int __real_fsync(int);

int __wrap_fseeko(FILE *stream, off_t offset, int whence) {
    if (stream == fault.stream && whence == SEEK_SET && (offset == fault.target || fault.next_seek)) {
        fault.next_seek = false;
        fault.publishing = true;
        if (fault.kind == failure::seek) {
            fault.triggered = true;
            errno = EIO;
            return -1;
        }
    }
    return __real_fseeko(stream, offset, whence);
}

size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *stream) {
    if (fault.wal_record >= 0 && stream != fault.stream && size == 1 && count == 1 &&
        *static_cast<const unsigned char *>(data) == fault.wal_record) {
        fault.triggered = true;
        errno = ENOSPC;
        return 0;
    }
    if (stream == fault.stream && fault.publishing &&
        fault.kind == failure::short_write && ++fault.writes == fault.short_write_call) {
        fault.triggered = true;
        errno = ENOSPC;
        return __real_fwrite(data, size, count - 1, stream);
    }
    return __real_fwrite(data, size, count, stream);
}

int __wrap_fflush(FILE *stream) {
    if (stream == fault.stream && fault.publishing && fault.kind == failure::flush) {
        fault.triggered = true;
        errno = EIO;
        return EOF;
    }
    return __real_fflush(stream);
}

int __wrap_fsync(int descriptor) {
    if (descriptor == fault.descriptor && fault.publishing && fault.kind == failure::sync) {
        fault.triggered = true;
        errno = EIO;
        return -1;
    }
    return __real_fsync(descriptor);
}
}

class IndexNodeWriteTest : public ::testing::Test {
protected:
    char path[256];
    FILE *stream = nullptr;

    void SetUp() override {
        generate_tmp_fn(path, sizeof(path));
        stream = fopen(path, "w+b");
        ASSERT_NE(stream, nullptr);
    }

    void TearDown() override {
        fault = {};
        if (stream) {
            EXPECT_EQ(fclose(stream), 0);
        }
        remove(path);
        remove((std::string(path) + ".wal").c_str());
    }
};

TEST_F(IndexNodeWriteTest, WalCommitFailureKeepsDirtyNodeForRetry) {
    compio::WalManager wal(path);
    ASSERT_TRUE(wal.open());
    compio::shared_node node(stream, 4096, new compio::index_node(3), nullptr, &wal);
    node->keys.push_back({42, 0});
    node->values.push_back({1234, 16});
    node->num_keys = 1;
    fault.stream = stream;
    fault.wal_record = static_cast<int>(compio::WalRecordType::COMMIT);
    EXPECT_FALSE(node.flush());
    EXPECT_TRUE(fault.triggered);
    EXPECT_EQ(std::filesystem::file_size(path), 0u);
    fault = {};
    EXPECT_TRUE(node.flush());
    EXPECT_TRUE(wal.checkpoint()); // A failed nested operation must balance its transaction.
    compio::index_node recovered(3);
    ASSERT_TRUE(recovered.read_from(stream, 4096));
    ASSERT_EQ(recovered.keys.size(), 1u);
    EXPECT_EQ(recovered.keys.front(), (compio::tree_key{42, 0}));
}

TEST_F(IndexNodeWriteTest, PinnedNodeRemainsCanonicalAcrossEvictionAndClear) {
    for (uint64_t capacity : {0, 1, 3}) {
        compio::node_reader reader(stream, 3, capacity, nullptr);
        auto node = reader.create_node(4096);
        node->keys.push_back({42, 0});
        node->values.push_back({1234, 16});
        node->num_keys = 1;
        for (uint64_t i = 1; i <= 4; ++i) reader.create_node(4096 * (i + 1));
        ASSERT_TRUE(reader.clear_cache());
        auto same = reader.read_node(4096);
        ASSERT_TRUE(same);
        EXPECT_EQ(&*readonly(node, compio::index_node), &*readonly(same, compio::index_node));
        node->values.front() = {4321, 32};
        ASSERT_TRUE(reader.flush_cache());
        compio::index_node recovered(3);
        ASSERT_TRUE(recovered.read_from(stream, 4096));
        ASSERT_EQ(recovered.values.size(), 1u);
        EXPECT_EQ(recovered.values.front(), (compio::tree_val{4321, 32}));
    }
}

class HeaderPublicationTest : public ::testing::TestWithParam<std::tuple<bool, failure>> {
protected:
    char path[256];
    compio_archive *archive = nullptr;
    int slot = 0;
    uint64_t sequence = 0;
    std::vector<unsigned char> saved_header;

    bool wal_enabled() const { return std::get<0>(GetParam()); }

    void SetUp() override {
        generate_tmp_fn(path, sizeof(path));
        compio_config cfg;
        compio_build_default_config(&cfg);
        cfg.max_files = 16;
        cfg.enable_wal = wal_enabled();
        cfg.wal_max_size_bytes = 1;
        cfg.auto_batch_size = 0;
        cfg.fragmentation_threshold = 100;
        archive = compio_open_archive(path, "w", &cfg);
        ASSERT_NE(archive, nullptr);
        compio_flush(archive);
        capture_header();
    }

    void capture_header() {
        slot = archive->current_header_slot;
        sequence = readonly(archive->header, compio::header)->sequence_id;
        saved_header.resize(readonly(archive->header, compio::header)->disk_size());
        ASSERT_EQ(pread(fileno(archive->file), saved_header.data(), saved_header.size(),
                        slot * saved_header.size()), static_cast<ssize_t>(saved_header.size()));
    }

    void TearDown() override {
        fault = {};
        if (archive) {
            EXPECT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
        }
        remove(path);
        remove((std::string(path) + ".wal").c_str());
    }

    void arm() {
        fault = {};
        fault.stream = archive->file;
        fault.descriptor = fileno(archive->file);
        fault.target = (1 - slot) * saved_header.size();
        fault.kind = std::get<1>(GetParam());
    }

    void write_file() {
        auto *file = compio_open_file("data", archive);
        ASSERT_NE(file, nullptr);
        const std::string data(4096, 'x');
        ASSERT_EQ(compio_write(data.data(), data.size(), file), data.size());
        ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    }

    void prepare_table_failure() {
        // An empty file changes only the table; clean caches do no I/O before it.
        auto *file = compio_open_file("empty", archive);
        ASSERT_NE(file, nullptr);
        ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
        arm();
        fault.target = -1;
        fault.next_seek = true;
        fault.short_write_call = 1;
    }

    void expect_old_slot() {
        EXPECT_TRUE(fault.triggered);
        EXPECT_EQ(archive->current_header_slot, slot);
        EXPECT_EQ(readonly(archive->header, compio::header)->sequence_id, sequence);
        std::vector<unsigned char> actual(saved_header.size());
        ASSERT_EQ(pread(fileno(archive->file), actual.data(), actual.size(), slot * actual.size()),
                  static_cast<ssize_t>(actual.size()));
        EXPECT_EQ(actual, saved_header);
    }

    void expect_wal_retained() {
        if (wal_enabled()) {
            EXPECT_GT(std::filesystem::file_size(std::string(path) + ".wal"), 0u);
        }
    }

    void retry() {
        fault = {};
        clearerr(archive->file);
        ASSERT_TRUE(archive->publish_header());
        EXPECT_EQ(archive->current_header_slot, 1 - slot);
        EXPECT_EQ(readonly(archive->header, compio::header)->sequence_id, sequence + 1);
    }
};

TEST_P(HeaderPublicationTest, BatchFailureKeepsSlotAndAllowsRetry) {
    ASSERT_EQ(compio_begin_batch(archive), COMPIO_SUCCESS);
    write_file();
    compio_flush(archive);
    // With WAL disabled, a flush inside a batch already publishes a header.
    capture_header();
    arm();
    archive->truncate_after_publish = true;
    EXPECT_EQ(compio_end_batch(archive), COMPIO_ERROR);
    expect_old_slot();
    expect_wal_retained();
    EXPECT_TRUE(archive->truncate_after_publish);
    // The flag is synthetic; do not request a real truncation during cleanup.
    archive->truncate_after_publish = false;
    retry();
}

TEST_P(HeaderPublicationTest, FlushFailureKeepsWalAndSkipsTruncation) {
    write_file();
    arm();
    archive->truncate_after_publish = true;
    compio_flush(archive);
    expect_old_slot();
    expect_wal_retained();
    EXPECT_TRUE(archive->truncate_after_publish);
    archive->truncate_after_publish = false;
    retry();
}

TEST_P(HeaderPublicationTest, IndexFailureKeepsCheckpointAndRetriesNode) {
    const compio::tree_key key{42, 0};
    const compio::tree_val value{1234, 16};
    archive->index->insert(key, value);
    arm();
    fault.target = readonly(archive->header, compio::header)->index_root;
    fault.short_write_call = 1;
    compio_flush(archive);
    expect_old_slot();
    expect_wal_retained();
    fault = {};
    clearerr(archive->file);
    compio_flush(archive);
    EXPECT_GT(readonly(archive->header, compio::header)->sequence_id, sequence);
    compio::index_node recovered(archive->config.b_tree_degree);
    ASSERT_TRUE(recovered.read_from(archive->file, readonly(archive->header, compio::header)->index_root));
    ASSERT_EQ(recovered.keys.size(), 1u);
    EXPECT_EQ(recovered.keys.front(), key);
    EXPECT_EQ(recovered.values.front(), value);
}

TEST_P(HeaderPublicationTest, IndexFailureMakesBatchFail) {
    ASSERT_EQ(compio_begin_batch(archive), COMPIO_SUCCESS);
    archive->index->insert({42, 0}, {1234, 16});
    arm();
    fault.target = readonly(archive->header, compio::header)->index_root;
    fault.short_write_call = 1;
    EXPECT_EQ(compio_end_batch(archive), COMPIO_ERROR);
    expect_old_slot();
    expect_wal_retained();
    fault = {};
    clearerr(archive->file);
    compio_flush(archive);
    EXPECT_GT(readonly(archive->header, compio::header)->sequence_id, sequence);
}

TEST_P(HeaderPublicationTest, IndexFailureMakesCloseFail) {
    archive->index->insert({42, 0}, {1234, 16});
    arm();
    fault.target = readonly(archive->header, compio::header)->index_root;
    fault.short_write_call = 1;
    EXPECT_EQ(compio_close_archive(archive), COMPIO_ERROR);
    archive = nullptr;
    EXPECT_TRUE(fault.triggered);
    fault = {};
    expect_wal_retained();
    FILE *stream = fopen(path, "rb");
    ASSERT_NE(stream, nullptr);
    std::vector<unsigned char> actual(saved_header.size());
    EXPECT_EQ(pread(fileno(stream), actual.data(), actual.size(), slot * actual.size()),
              static_cast<ssize_t>(actual.size()));
    EXPECT_EQ(actual, saved_header);
    EXPECT_EQ(fclose(stream), 0);
}

TEST_P(HeaderPublicationTest, IndexWalLogFailureBlocksPublicationAndRetries) {
    if (!wal_enabled()) GTEST_SKIP();
    archive->index->insert({42, 0}, {1234, 16});
    arm();
    fault.target = -1;
    fault.wal_record = static_cast<int>(compio::WalRecordType::INDEX_NODE);
    compio_flush(archive);
    expect_old_slot();
    expect_wal_retained();
    fault = {};
    compio_flush(archive);
    EXPECT_GT(readonly(archive->header, compio::header)->sequence_id, sequence);
    compio::index_node recovered(archive->config.b_tree_degree);
    ASSERT_TRUE(recovered.read_from(archive->file, readonly(archive->header, compio::header)->index_root));
    ASSERT_EQ(recovered.keys.size(), 1u);
    EXPECT_EQ(recovered.keys.front(), (compio::tree_key{42, 0}));
}

TEST_P(HeaderPublicationTest, EvictionFailureKeepsNodeAndBlocksCheckpoint) {
    if (std::get<1>(GetParam()) != failure::seek &&
        std::get<1>(GetParam()) != failure::short_write) GTEST_SKIP();
    compio::node_reader reader(archive->file, 3, 1, &archive->io_mutex,
                               wal_enabled() ? archive->wal.get() : nullptr);
    const auto addr = archive->allocator->allocate(INDEX_NODE_SIZE(3));
    {
        auto node = reader.create_node(addr);
        node->keys.push_back({42, 0});
        node->values.push_back({1234, 16});
        node->num_keys = 1;
    }
    arm();
    fault.target = addr;
    fault.short_write_call = 1;
    const auto other_addr = archive->allocator->allocate(INDEX_NODE_SIZE(3));
    auto other = reader.create_node(other_addr);
    EXPECT_TRUE(fault.triggered);
    fault = {};
    clearerr(archive->file);
    auto retained = reader.read_node(addr);
    ASSERT_TRUE(retained);
    ASSERT_EQ(readonly(retained, compio::index_node)->keys.size(), 1u);
    EXPECT_EQ(readonly(retained, compio::index_node)->keys.front(), (compio::tree_key{42, 0}));
    // The first flush reports the earlier eviction error even if the retry succeeds.
    EXPECT_FALSE(reader.flush_cache());
    EXPECT_TRUE(reader.flush_cache());
    compio::index_node recovered(3);
    ASSERT_TRUE(recovered.read_from(archive->file, addr));
    ASSERT_EQ(recovered.keys.size(), 1u);
    EXPECT_EQ(recovered.keys.front(), (compio::tree_key{42, 0}));
}

TEST_P(HeaderPublicationTest, EarlierEvictionFailureBlocksCheckpointUntilRetry) {
    if (std::get<1>(GetParam()) != failure::seek &&
        std::get<1>(GetParam()) != failure::short_write) GTEST_SKIP();
    compio_config cfg = archive->config;
    cfg.b_tree_degree = 2;
    cfg.cache_size__nodes = 4;
    ASSERT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
    archive = compio_open_archive(path, "w+", &cfg);
    ASSERT_NE(archive, nullptr);
    for (uint64_t i = 0; i < 30; ++i) archive->index->insert({42, i}, {1234 + i, 1});
    compio_flush(archive);
    capture_header();
    ASSERT_TRUE(archive->index->update({42, 0}, {4321, 1}));
    auto node = archive->index->find_node({42, 0});
    ASSERT_TRUE(node);
    const auto addr = node.addr();
    node = {};
    arm();
    fault.target = addr;
    fault.short_write_call = 1;
    for (uint64_t i = 30; i < 60; ++i) archive->index->insert({42, i}, {1234 + i, 1});
    ASSERT_TRUE(fault.triggered);
    fault = {};
    clearerr(archive->file);
    compio_flush(archive);
    EXPECT_EQ(archive->current_header_slot, slot);
    EXPECT_EQ(readonly(archive->header, compio::header)->sequence_id, sequence);
    expect_wal_retained();
    compio_flush(archive);
    EXPECT_GT(readonly(archive->header, compio::header)->sequence_id, sequence);
    ASSERT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
    archive = compio_open_archive(path, "r+", &cfg);
    ASSERT_NE(archive, nullptr);
    auto value = archive->index->get({42, 0});
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, (compio::tree_val{4321, 1}));
    for (uint64_t i = 1; i < 60; ++i) {
        value = archive->index->get({42, i});
        ASSERT_TRUE(value.has_value()) << i;
        EXPECT_EQ(*value, (compio::tree_val{1234 + i, 1}));
    }
}

TEST_P(HeaderPublicationTest, FilesTableFailureKeepsCheckpointAndRetriesTable) {
    const auto old_addr = readonly(archive->header, compio::header)->files_table_addr;
    const auto old_checksum = archive->files_table_checksum;
    prepare_table_failure();
    compio_flush(archive);
    expect_old_slot();
    expect_wal_retained();
    if (fault.kind != failure::sync) {
        EXPECT_EQ(readonly(archive->header, compio::header)->files_table_addr, old_addr);
        EXPECT_EQ(archive->files_table_checksum, old_checksum);
    }
    fault = {};
    clearerr(archive->file);
    compio_flush(archive);
    EXPECT_GT(readonly(archive->header, compio::header)->sequence_id, sequence);
    compio::files_table recovered;
    const auto &h = readonly(archive->header, compio::header);
    ASSERT_TRUE(recovered.read_from(archive->file, h->files_table_addr,
                                    h->files_table_capacity, h->ftable.n_files));
    EXPECT_NE(recovered.find("empty"), nullptr);
}

TEST_P(HeaderPublicationTest, FilesTableFailureMakesBatchFail) {
    ASSERT_EQ(compio_begin_batch(archive), COMPIO_SUCCESS);
    prepare_table_failure();
    EXPECT_EQ(compio_end_batch(archive), COMPIO_ERROR);
    expect_old_slot();
    expect_wal_retained();
    fault = {};
    clearerr(archive->file);
    compio_flush(archive);
    EXPECT_GT(readonly(archive->header, compio::header)->sequence_id, sequence);
}

TEST_P(HeaderPublicationTest, FilesTableFailureMakesCloseFail) {
    prepare_table_failure();
    EXPECT_EQ(compio_close_archive(archive), COMPIO_ERROR);
    archive = nullptr;
    EXPECT_TRUE(fault.triggered);
    fault = {};
    expect_wal_retained();
    FILE *stream = fopen(path, "rb");
    ASSERT_NE(stream, nullptr);
    std::vector<unsigned char> actual(saved_header.size());
    EXPECT_EQ(pread(fileno(stream), actual.data(), actual.size(), slot * actual.size()),
              static_cast<ssize_t>(actual.size()));
    EXPECT_EQ(actual, saved_header);
    EXPECT_EQ(fclose(stream), 0);
}

TEST_P(HeaderPublicationTest, CompactionCallbackReportsPublicationFailure) {
    arm();
    EXPECT_FALSE(archive->publish_header());
    expect_old_slot();
    retry();
}

TEST_P(HeaderPublicationTest, FailedCheckpointKeepsDeferredBlocksReservedUntilRetry) {
    const std::vector<unsigned char> original(4096, 0xAB);
    const uint64_t offset = archive->allocator->allocate(original.size());
    ASSERT_NE(offset, UINT64_MAX);
    ASSERT_EQ(pwrite(fileno(archive->file), original.data(), original.size(), offset),
              static_cast<ssize_t>(original.size()));
    compio_flush(archive);
    capture_header();
    archive->allocator->deallocate(offset, original.size(), false);
    for (int i = 0; i < 3; ++i) {
        arm();
        compio_flush(archive);
        expect_old_slot();
        expect_wal_retained();
        std::vector<unsigned char> actual(original.size());
        ASSERT_EQ(pread(fileno(archive->file), actual.data(), actual.size(), offset),
                  static_cast<ssize_t>(actual.size()));
        EXPECT_EQ(actual, original);
        fault = {};
        clearerr(archive->file);
        EXPECT_NE(archive->allocator->allocate(original.size()), offset);
    }
    compio_flush(archive);
    EXPECT_EQ(archive->allocator->allocate(original.size()), offset);
}

TEST_P(HeaderPublicationTest, SnapshotFailureKeepsDeferredBlocksReservedUntilRetry) {
    const std::vector<unsigned char> original(4096, 0xAB);
    const uint64_t offset = archive->allocator->allocate(original.size());
    ASSERT_NE(offset, UINT64_MAX);
    ASSERT_EQ(pwrite(fileno(archive->file), original.data(), original.size(), offset),
              static_cast<ssize_t>(original.size()));
    compio_flush(archive);
    capture_header();
    ASSERT_TRUE(archive->allocator->save_state(archive));
    archive->allocator->deallocate(offset, original.size(), false);
    arm();
    fault.target = readonly(archive->header, compio::header)->allocator_state_offset;
    fault.short_write_call = 1;
    const bool saved = archive->allocator->save_state(archive);
    EXPECT_FALSE(saved && archive->publish_header());
    expect_old_slot();
    std::vector<unsigned char> actual(original.size());
    ASSERT_EQ(pread(fileno(archive->file), actual.data(), actual.size(), offset),
              static_cast<ssize_t>(actual.size()));
    EXPECT_EQ(actual, original);
    fault = {};
    clearerr(archive->file);
    EXPECT_NE(archive->allocator->allocate(original.size()), offset);
    compio_flush(archive);
    EXPECT_EQ(archive->allocator->allocate(original.size()), offset);
}

TEST_P(HeaderPublicationTest, FailedCloseDoesNotReleaseDeferredBlocks) {
    const std::vector<unsigned char> original(4096, 0xAB);
    const uint64_t offset = archive->allocator->allocate(original.size());
    ASSERT_NE(offset, UINT64_MAX);
    ASSERT_EQ(pwrite(fileno(archive->file), original.data(), original.size(), offset),
              static_cast<ssize_t>(original.size()));
    compio_flush(archive);
    capture_header();
    archive->allocator->deallocate(offset, original.size(), false);
    arm();
    EXPECT_EQ(compio_close_archive(archive), COMPIO_ERROR);
    archive = nullptr;
    EXPECT_TRUE(fault.triggered);
    fault = {};
    FILE *file = fopen(path, "rb");
    ASSERT_NE(file, nullptr);
    std::vector<unsigned char> actual(original.size());
    EXPECT_EQ(pread(fileno(file), actual.data(), actual.size(), offset),
              static_cast<ssize_t>(actual.size()));
    EXPECT_EQ(actual, original);
    EXPECT_EQ(fclose(file), 0);
}

TEST_P(HeaderPublicationTest, RepeatedFlushFailuresPreserveAllocatorSnapshot) {
    const uint64_t free_block = archive->allocator->allocate(4096);
    ASSERT_NE(free_block, UINT64_MAX);
    archive->allocator->deallocate(free_block, 4096, false);
    for (int i = 0; i < 3; ++i) compio_flush(archive);
    capture_header();
    const auto &h = readonly(archive->header, compio::header);
    const uint64_t offset = h->allocator_state_offset;
    std::vector<unsigned char> saved(h->allocator_state_size);
    ASSERT_EQ(pread(fileno(archive->file), saved.data(), saved.size(), offset),
              static_cast<ssize_t>(saved.size()));
    for (int i = 0; i < 3; ++i) {
        fault = {};
        clearerr(archive->file);
        const uint64_t block = archive->allocator->allocate(17);
        ASSERT_NE(block, UINT64_MAX);
        arm();
        compio_flush(archive);
        expect_old_slot();
        expect_wal_retained();
        std::vector<unsigned char> actual(saved.size());
        ASSERT_EQ(pread(fileno(archive->file), actual.data(), actual.size(), offset),
                  static_cast<ssize_t>(actual.size()));
        EXPECT_EQ(actual, saved);
    }
    retry();
}

TEST_P(HeaderPublicationTest, AllocatorWriteFailureAllowsRetry) {
    for (int i = 0; i < 3; ++i) compio_flush(archive);
    capture_header();
    ASSERT_TRUE(archive->allocator->save_state(archive));
    const uint64_t pending_offset = readonly(archive->header, compio::header)->allocator_state_offset;
    const uint64_t pending_size = readonly(archive->header, compio::header)->allocator_state_size;
    arm();
    fault.target = pending_offset;
    fault.short_write_call = 1;
    // Without WAL, save_state leaves the durability barrier to publication.
    const bool saved = archive->allocator->save_state(archive);
    EXPECT_FALSE(saved && archive->publish_header());
    expect_old_slot();
    EXPECT_EQ(readonly(archive->header, compio::header)->allocator_state_offset, pending_offset);
    EXPECT_EQ(readonly(archive->header, compio::header)->allocator_state_size, pending_size);
    fault = {};
    clearerr(archive->file);
    ASSERT_TRUE(archive->allocator->save_state(archive));
    EXPECT_EQ(readonly(archive->header, compio::header)->allocator_state_offset, pending_offset);
    retry();
}

TEST_P(HeaderPublicationTest, AllocatorGrowthFailureAllowsRetry) {
    for (int i = 0; i < 3; ++i) compio_flush(archive);
    capture_header();
    ASSERT_TRUE(archive->allocator->save_state(archive));
    const uint64_t pending_offset = readonly(archive->header, compio::header)->allocator_state_offset;
    const uint64_t pending_size = readonly(archive->header, compio::header)->allocator_state_size;
    for (int i = 0; i < 12; ++i) {
        const uint64_t block = archive->allocator->allocate(32);
        ASSERT_NE(block, UINT64_MAX);
        archive->allocator->deallocate(block, 7, false);
    }
    const auto before = archive->allocator->get_fragmentation_stats();
    arm();
    fault.target = readonly(archive->header, compio::header)->file_size;
    fault.short_write_call = 1;
    const bool saved = archive->allocator->save_state(archive);
    EXPECT_FALSE(saved && archive->publish_header());
    expect_old_slot();
    if (fault.kind != failure::sync) {
        EXPECT_EQ(readonly(archive->header, compio::header)->allocator_state_offset, pending_offset);
        EXPECT_EQ(readonly(archive->header, compio::header)->allocator_state_size, pending_size);
        const auto after = archive->allocator->get_fragmentation_stats();
        EXPECT_EQ(after.total_free_bytes, before.total_free_bytes);
        EXPECT_EQ(after.num_free_regions, before.num_free_regions);
    }
    fault = {};
    clearerr(archive->file);
    ASSERT_TRUE(archive->allocator->save_state(archive));
    retry();
}

TEST_P(HeaderPublicationTest, CloseReportsFailureAndPreservesWalAndOldHeader) {
    write_file();
    arm();
    EXPECT_EQ(compio_close_archive(archive), COMPIO_ERROR);
    archive = nullptr;
    EXPECT_TRUE(fault.triggered);
    fault = {};
    expect_wal_retained();

    FILE *stream = fopen(path, "rb");
    ASSERT_NE(stream, nullptr);
    std::vector<unsigned char> actual(saved_header.size());
    EXPECT_EQ(pread(fileno(stream), actual.data(), actual.size(), slot * actual.size()),
              static_cast<ssize_t>(actual.size()));
    EXPECT_EQ(actual, saved_header);
    fclose(stream);
}

TEST_P(HeaderPublicationTest, CloseReportsFailureOfFinalPublication) {
    write_file();
    arm();
    // Close publishes twice: fail only after its initial flush succeeded.
    fault.target = slot * saved_header.size();
    EXPECT_EQ(compio_close_archive(archive), COMPIO_ERROR);
    archive = nullptr;
    EXPECT_TRUE(fault.triggered);
    fault = {};
    expect_wal_retained();

    FILE *stream = fopen(path, "rb");
    ASSERT_NE(stream, nullptr);
    compio::header published;
    EXPECT_TRUE(published.load_and_validate(stream, (1 - slot) * saved_header.size()));
    EXPECT_EQ(published.sequence_id, sequence + 1);
    fclose(stream);
}

TEST_P(HeaderPublicationTest, DefragmentReportsInitialPublicationFailure) {
    arm();
    EXPECT_EQ(compio_defragment(archive), COMPIO_ERROR);
    expect_old_slot();
    retry();
}

INSTANTIATE_TEST_SUITE_P(
    WalModesAndIoFailures, HeaderPublicationTest,
    ::testing::Combine(::testing::Bool(),
                       ::testing::Values(failure::seek, failure::short_write,
                                         failure::flush, failure::sync)));
