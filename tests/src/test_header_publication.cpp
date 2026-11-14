#include <gtest/gtest.h>
#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
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
    int short_write_call = 1;
    bool publishing = false;
    bool triggered = false;
    bool next_seek = false;
    bool every_seek = false;
    int wal_record = -1;
};

io_fault fault;

// A journal record starts with one fwrite of type, epoch, addr and size.
constexpr size_t wal_record_fields_bytes = 1 + 8 + 8 + 8;

} // namespace

extern "C" {
int __real_fseeko(FILE *, off_t, int);
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
int __real_fflush(FILE *);
int __real_fsync(int);

int __wrap_fseeko(FILE *stream, off_t offset, int whence) {
    if (stream == fault.stream && whence == SEEK_SET && (offset == fault.target || fault.next_seek || fault.every_seek)) {
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
    if (fault.wal_record >= 0 && stream != fault.stream && size == 1 && count == wal_record_fields_bytes &&
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
        // Only the explicit flush and close under test may publish.
        cfg.wal_max_size_bytes = 0;
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
            const int expected = archive->is_broken() ? COMPIO_ERROR : COMPIO_SUCCESS;
            EXPECT_EQ(compio_close_archive(archive), expected);
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

    void prepare_compaction(bool on_close = false) {
        if (on_close) {
            auto cfg = archive->config;
            cfg.fragmentation_threshold = 0;
            ASSERT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
            archive = compio_open_archive(path, "w+", &cfg);
            ASSERT_NE(archive, nullptr);
        }
        const uint64_t hole = archive->allocator->allocate(65536);
        ASSERT_NE(hole, UINT64_MAX);
        write_file();
        compio_flush(archive);
        archive->allocator->deallocate(hole, 65536, false);
        compio_flush(archive);
        capture_header();
        auto *file = compio_open_file("data", archive);
        ASSERT_NE(file, nullptr);
        const auto value = archive->index->get({file->hash, 0});
        ASSERT_TRUE(value);
        ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
        arm();
        fault.target = value->addr;
        fault.short_write_call = 1;
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

    // A failed sync leaves the archive unable to promise anything about what it wrote:
    // retrying could succeed over pages the disk never got. It refuses to write or publish
    // and keeps the last published state for the next open.
    void expect_broken() {
        fault = {};
        clearerr(archive->file);
        EXPECT_TRUE(archive->is_broken());
        if (auto *file = compio_open_file("after_failure", archive)) {
            errno = 0;
            EXPECT_EQ(compio_write("x", 1, file), 0u);
            EXPECT_EQ(errno, EIO);
            compio_close_file(file);
        }
        compio_flush(archive);
        EXPECT_FALSE(archive->publish_header());
        EXPECT_EQ(archive->current_header_slot, slot);
        EXPECT_EQ(readonly(archive->header, compio::header)->sequence_id, sequence);

        const compio_config config = archive->config;
        errno = 0;
        EXPECT_EQ(compio_close_archive(archive), COMPIO_ERROR);
        EXPECT_EQ(errno, EIO);
        archive = nullptr;

        archive = compio_open_archive(path, wal_enabled() ? "r+" : "r", &config);
        ASSERT_NE(archive, nullptr);
    }

    void retry() {
        if (std::get<1>(GetParam()) == failure::sync) {
            expect_broken();
            return;
        }
        fault = {};
        clearerr(archive->file);
        ASSERT_TRUE(archive->publish_header());
        EXPECT_EQ(archive->current_header_slot, 1 - slot);
        EXPECT_EQ(readonly(archive->header, compio::header)->sequence_id, sequence + 1);
    }
};

TEST_P(HeaderPublicationTest, ReadOnlyFlushAndCloseDoNotPersist) {
    write_file();
    const auto config = archive->config;
    const int closed = compio_close_archive(archive);
    archive = nullptr;
    ASSERT_EQ(closed, COMPIO_SUCCESS);
    archive = compio_open_archive(path, "r", &config);
    ASSERT_NE(archive, nullptr);
    capture_header();
    arm();
    fault.publishing = true;
    fault.every_seek = true;

    compio_flush(archive);
    EXPECT_FALSE(fault.triggered);
    EXPECT_EQ(archive->current_header_slot, slot);
    EXPECT_EQ(readonly(archive->header, compio::header)->sequence_id, sequence);
    EXPECT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
    archive = nullptr;
    EXPECT_FALSE(fault.triggered);
    fault = {};
}

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
    if (std::get<1>(GetParam()) == failure::sync) {
        expect_broken();
        return;
    }
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
    if (std::get<1>(GetParam()) == failure::sync) {
        expect_broken();
        return;
    }
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
    const auto old_checksum = readonly(archive->header, compio::header)->files_table_checksum;
    prepare_table_failure();
    compio_flush(archive);
    expect_old_slot();
    expect_wal_retained();
    if (fault.kind != failure::sync) {
        EXPECT_EQ(readonly(archive->header, compio::header)->files_table_addr, old_addr);
        EXPECT_EQ(readonly(archive->header, compio::header)->files_table_checksum, old_checksum);
    }
    if (std::get<1>(GetParam()) == failure::sync) {
        expect_broken();
        return;
    }
    fault = {};
    clearerr(archive->file);
    compio_flush(archive);
    EXPECT_GT(readonly(archive->header, compio::header)->sequence_id, sequence);
    compio::files_table recovered;
    const auto &h = readonly(archive->header, compio::header);
    ASSERT_TRUE(recovered.read_from(archive->file, h->files_table_addr, h->files_table_size,
                                    h->ftable.files.size(), h->files_table_checksum, h->files_table_encoding));
    EXPECT_NE(recovered.find("empty"), nullptr);
}

TEST_P(HeaderPublicationTest, FilesTableFailureMakesBatchFail) {
    ASSERT_EQ(compio_begin_batch(archive), COMPIO_SUCCESS);
    prepare_table_failure();
    EXPECT_EQ(compio_end_batch(archive), COMPIO_ERROR);
    expect_old_slot();
    expect_wal_retained();
    if (std::get<1>(GetParam()) == failure::sync) {
        expect_broken();
        return;
    }
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
        if (std::get<1>(GetParam()) == failure::sync) {
            expect_broken();
            return;
        }
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
    if (std::get<1>(GetParam()) == failure::sync) {
        expect_broken();
        return;
    }
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
        if (std::get<1>(GetParam()) == failure::sync) {
            expect_broken();
            return;
        }
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
    if (std::get<1>(GetParam()) == failure::sync) {
        expect_broken();
        return;
    }
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
    if (std::get<1>(GetParam()) == failure::sync) {
        expect_broken();
        return;
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

TEST_P(HeaderPublicationTest, DefragmentReportsCompactionFailureWithoutFinalPublication) {
    prepare_compaction();
    EXPECT_EQ(compio_defragment(archive), COMPIO_ERROR);
    EXPECT_TRUE(fault.triggered);
    EXPECT_EQ(readonly(archive->header, compio::header)->sequence_id, sequence + 1);
    fault = {};
    clearerr(archive->file);
    // Consume any retained node write-back error before cleanup retries close.
    compio_flush(archive);
}

TEST_P(HeaderPublicationTest, UnreadableIndexAbortsCompactionWithoutFinalPublication) {
    if (std::get<1>(GetParam()) != failure::seek) GTEST_SKIP();
    write_file();
    compio_flush(archive);
    capture_header();
    arm();
    fault.target = readonly(archive->header, compio::header)->index_root;
    EXPECT_EQ(compio_defragment(archive), COMPIO_ERROR);
    EXPECT_TRUE(fault.triggered);
    EXPECT_EQ(readonly(archive->header, compio::header)->sequence_id, sequence + 1);
    fault = {};
    clearerr(archive->file);
}

TEST_P(HeaderPublicationTest, UnreadableIndexStopsReadWithoutAdvancingCursor) {
    if (std::get<1>(GetParam()) != failure::seek) GTEST_SKIP();
    auto cfg = archive->config;
    cfg.b_tree_degree = 2;
    ASSERT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
    archive = compio_open_archive(path, "w+", &cfg);
    ASSERT_NE(archive, nullptr);
    auto *file = compio_open_file("data", archive);
    ASSERT_NE(file, nullptr);
    const std::vector<unsigned char> data(32 * cfg.block_size, 'x');
    const compio::tree_key key{file->hash, 0};
    ASSERT_EQ(compio_write(data.data(), data.size(), file), data.size());
    ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    compio_flush(archive);
    auto leaf = archive->index->find_node(key);
    ASSERT_TRUE(leaf);
    const uint64_t leaf_addr = leaf.addr();
    leaf = {};
    const uint64_t root_addr = readonly(archive->header, compio::header)->index_root;
    ASSERT_NE(leaf_addr, root_addr);
    for (const uint64_t addr : {root_addr, leaf_addr}) {
        SCOPED_TRACE(addr);
        file = compio_open_file("data", archive);
        ASSERT_NE(file, nullptr);
        ASSERT_TRUE(archive->index->clear_cache());
        arm();
        fault.target = addr;
        std::vector<unsigned char> actual(data.size(), 0xA5);
        errno = 0;
        EXPECT_EQ(compio_read(actual.data(), actual.size(), file), 0u);
        EXPECT_TRUE(fault.triggered);
        EXPECT_EQ(errno, EIO);
        EXPECT_EQ(compio_tell(file), 0u);
        EXPECT_EQ(actual, std::vector<unsigned char>(data.size(), 0xA5));
        fault = {};
        clearerr(archive->file);
        EXPECT_EQ(compio_read(actual.data(), actual.size(), file), data.size());
        EXPECT_EQ(actual, data);
        ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    }
}

TEST_P(HeaderPublicationTest, UnreadableGapRangeCannotReplaceDataWithZeros) {
    if (std::get<1>(GetParam()) != failure::seek) GTEST_SKIP();
    auto cfg = archive->config;
    cfg.b_tree_degree = 2;
    ASSERT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
    archive = compio_open_archive(path, "w+", &cfg);
    ASSERT_NE(archive, nullptr);
    auto *file = compio_open_file("sparse", archive);
    ASSERT_NE(file, nullptr);
    const std::vector<unsigned char> block(cfg.block_size, 'x');
    std::vector<unsigned char> expected(63 * cfg.block_size, 0);
    for (uint64_t i = 0; i < 32; ++i) {
        const uint64_t pos = 2 * i * cfg.block_size;
        ASSERT_EQ(compio_seek(file, pos, COMPIO_SEEK_SET), COMPIO_SUCCESS);
        ASSERT_EQ(compio_write(block.data(), block.size(), file), block.size());
        std::copy(block.begin(), block.end(), expected.begin() + pos);
    }
    const uint64_t hash = file->hash;
    ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    compio_flush(archive);
    auto first = archive->index->find_node({hash, 0});
    ASSERT_TRUE(first);
    const auto &node = *readonly(first, index_node);
    ASSERT_FALSE(node.keys.empty());
    const uint64_t start = node.keys.back().pos + node.values.back().size;
    auto gap = archive->index->find_node({hash, start});
    ASSERT_TRUE(gap);
    ASSERT_EQ(gap.addr(), first.addr());
    auto last = archive->index->find_node({hash, UINT64_C(62) * cfg.block_size});
    ASSERT_TRUE(last);
    const uint64_t target = last.addr();
    ASSERT_NE(target, first.addr());
    first = {};
    gap = {};
    last = {};
    for (const uint64_t offset : {start, UINT64_C(0)}) {
        SCOPED_TRACE(offset);
        file = compio_open_file("sparse", archive);
        ASSERT_NE(file, nullptr);
        ASSERT_EQ(compio_seek(file, offset, COMPIO_SEEK_SET), COMPIO_SUCCESS);
        ASSERT_TRUE(archive->index->clear_cache());
        arm();
        fault.target = target;
        std::vector<unsigned char> actual(expected.size() - offset, 0xA5);
        const uint64_t prefix = start - offset;
        auto unchanged = actual;
        std::copy(expected.begin() + offset, expected.begin() + start, unchanged.begin());
        errno = 0;
        EXPECT_EQ(compio_read(actual.data(), actual.size(), file), prefix);
        EXPECT_TRUE(fault.triggered);
        EXPECT_EQ(errno, EIO);
        EXPECT_EQ(compio_tell(file), start);
        EXPECT_EQ(actual, unchanged);
        fault = {};
        clearerr(archive->file);
        EXPECT_EQ(compio_read(actual.data() + prefix, actual.size() - prefix, file), actual.size() - prefix);
        EXPECT_EQ(actual, std::vector<unsigned char>(expected.begin() + offset, expected.end()));
        ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    }
}

TEST_P(HeaderPublicationTest, UnreadableInsertIndexLeavesFileUnchanged) {
    if (std::get<1>(GetParam()) != failure::seek) GTEST_SKIP();
    auto cfg = archive->config;
    cfg.b_tree_degree = 2;
    ASSERT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
    archive = compio_open_archive(path, "w+", &cfg);
    ASSERT_NE(archive, nullptr);
    auto *file = compio_open_file("data", archive);
    ASSERT_NE(file, nullptr);
    const std::vector<unsigned char> data(32 * cfg.block_size, 'x');
    const tree_key key{file->hash, 0};
    ASSERT_EQ(compio_write(data.data(), data.size(), file), data.size());
    ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    compio_flush(archive);
    auto leaf = archive->index->find_node(key);
    ASSERT_TRUE(leaf);
    const uint64_t leaf_addr = leaf.addr();
    leaf = {};
    const uint64_t root_addr = readonly(archive->header, compio::header)->index_root;
    ASSERT_NE(root_addr, leaf_addr);
    auto snapshot = [&]() {
        std::vector<unsigned char> bytes(std::filesystem::file_size(path));
        EXPECT_EQ(pread(fileno(archive->file), bytes.data(), bytes.size(), 0),
                  static_cast<ssize_t>(bytes.size()));
        return bytes;
    };
    const auto before = snapshot();
    for (const uint64_t addr : {root_addr, leaf_addr}) {
        SCOPED_TRACE(addr);
        file = compio_open_file("data", archive);
        ASSERT_NE(file, nullptr);
        ASSERT_EQ(compio_seek(file, 1, COMPIO_SEEK_SET), COMPIO_SUCCESS);
        ASSERT_TRUE(archive->index->clear_cache());
        arm();
        fault.target = addr;
        errno = 0;
        EXPECT_EQ(compio_insert("y", 1, file), 0u);
        EXPECT_TRUE(fault.triggered);
        EXPECT_EQ(errno, EIO);
        EXPECT_EQ(compio_tell(file), 1u);
        EXPECT_EQ(file->size, data.size());
        fault = {};
        clearerr(archive->file);
        EXPECT_EQ(snapshot(), before);
        ASSERT_EQ(compio_seek(file, 0, COMPIO_SEEK_SET), COMPIO_SUCCESS);
        std::vector<unsigned char> actual(data.size());
        EXPECT_EQ(compio_read(actual.data(), actual.size(), file), actual.size());
        EXPECT_EQ(actual, data);
        ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    }
}

TEST_P(HeaderPublicationTest, UnreadableAppendIndexLeavesFileUnchangedAndRetries) {
    if (std::get<1>(GetParam()) != failure::seek) GTEST_SKIP();
    write_file();
    compio_flush(archive);
    auto *file = compio_open_file("data", archive);
    ASSERT_NE(file, nullptr);
    const uint64_t size = file->size;
    ASSERT_EQ(compio_seek(file, 0, COMPIO_SEEK_END), COMPIO_SUCCESS);
    ASSERT_TRUE(archive->index->clear_cache());
    arm();
    fault.target = readonly(archive->header, compio::header)->index_root;
    errno = 0;
    EXPECT_EQ(compio_write("y", 1, file), 0u);
    EXPECT_TRUE(fault.triggered);
    EXPECT_EQ(errno, EIO);
    EXPECT_EQ(compio_tell(file), size);
    EXPECT_EQ(file->size, size);
    fault = {};
    clearerr(archive->file);
    ASSERT_EQ(compio_seek(file, 0, COMPIO_SEEK_SET), COMPIO_SUCCESS);
    std::string actual(size, '\0');
    EXPECT_EQ(compio_read(actual.data(), actual.size(), file), size);
    EXPECT_EQ(actual, std::string(size, 'x'));
    ASSERT_EQ(compio_seek(file, 0, COMPIO_SEEK_END), COMPIO_SUCCESS);
    EXPECT_EQ(compio_write("y", 1, file), 1u);
    ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    const int close_result = compio_close_archive(archive);
    archive = nullptr;
    ASSERT_EQ(close_result, COMPIO_SUCCESS);
    archive = compio_open_archive(path, "r", nullptr);
    ASSERT_NE(archive, nullptr);
    file = compio_open_file("data", archive);
    ASSERT_NE(file, nullptr);
    actual.resize(size + 1);
    EXPECT_EQ(compio_read(actual.data(), actual.size(), file), size + 1);
    EXPECT_EQ(actual, std::string(size, 'x') + 'y');
    ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
}

TEST_P(HeaderPublicationTest, UnreadableRemovalBlockPreservesEarlierBlocksAndRetries) {
    if (std::get<1>(GetParam()) != failure::seek) GTEST_SKIP();
    auto *file = compio_open_file("data", archive);
    ASSERT_NE(file, nullptr);
    const std::vector<unsigned char> data(2 * archive->config.block_size, 'x');
    const uint64_t hash = file->hash;
    ASSERT_EQ(compio_write(data.data(), data.size(), file), data.size());
    ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    compio_flush(archive);
    const tree_key min{hash, 0};
    const tree_key max{hash, UINT64_MAX};
    const auto before = archive->index->get_range(min, max);
    ASSERT_TRUE(before);
    ASSERT_EQ(before->size(), 2u);
    for (const bool cache_first : {false, true}) {
        SCOPED_TRACE(cache_first);
        ASSERT_TRUE(archive->block_reader->clear_cache());
        if (cache_first) {
            file = compio_open_file("data", archive);
            ASSERT_NE(file, nullptr);
            std::vector<unsigned char> first(archive->config.block_size);
            ASSERT_EQ(compio_read(first.data(), first.size(), file), first.size());
            ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
        }
        arm();
        fault.target = before->back().second.addr;
        errno = 0;
        EXPECT_EQ(compio_remove_file(archive, "data"), -1);
        EXPECT_TRUE(fault.triggered);
        EXPECT_EQ(errno, EIO);
        fault = {};
        clearerr(archive->file);
        EXPECT_EQ(archive->index->get_range(min, max), before);
        file = compio_open_file("data", archive);
        ASSERT_NE(file, nullptr);
        EXPECT_EQ(file->size, data.size());
        std::vector<unsigned char> actual(data.size(), 0);
        EXPECT_EQ(compio_read(actual.data(), actual.size(), file), actual.size());
        EXPECT_EQ(actual, data);
        ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    }
    EXPECT_EQ(compio_remove_file(archive, "data"), COMPIO_SUCCESS);
    const int close_result = compio_close_archive(archive);
    archive = nullptr;
    ASSERT_EQ(close_result, COMPIO_SUCCESS);
    archive = compio_open_archive(path, "r", nullptr);
    ASSERT_NE(archive, nullptr);
    EXPECT_EQ(readonly(archive->header, compio::header)->ftable.find("data"), nullptr);
}

TEST_P(HeaderPublicationTest, CloseReportsCompactionFailureWithoutFinalPublication) {
    prepare_compaction(true);
    EXPECT_EQ(compio_close_archive(archive), COMPIO_ERROR);
    archive = nullptr;
    EXPECT_TRUE(fault.triggered);
    fault = {};
    FILE *stream = fopen(path, "rb");
    ASSERT_NE(stream, nullptr);
    std::vector<unsigned char> actual(saved_header.size());
    ASSERT_EQ(pread(fileno(stream), actual.data(), actual.size(), slot * actual.size()),
              static_cast<ssize_t>(actual.size()));
    EXPECT_EQ(actual, saved_header);
    EXPECT_EQ(fclose(stream), 0);
}

INSTANTIATE_TEST_SUITE_P(
    WalModesAndIoFailures, HeaderPublicationTest,
    ::testing::Combine(::testing::Bool(),
                       ::testing::Values(failure::seek, failure::short_write,
                                         failure::flush, failure::sync)));

class BlockWriteTest : public ::testing::TestWithParam<std::tuple<bool, bool, bool, int>> {
protected:
    char path[256];
    compio_archive *archive = nullptr;
    compio_config config{};
    compio::tree_key key{};
    std::shared_ptr<compio::block> block;
    std::string original = std::string(4096, 'a');
    std::string replacement = std::string(4096, 'b');
    int slot = 0;
    uint64_t sequence = 0;
    std::vector<unsigned char> saved_header;

    void SetUp() override {
        generate_tmp_fn(path, sizeof(path));
        compio_build_default_config(&config);
        config.max_files = 16;
        config.enable_wal = std::get<0>(GetParam());
        config.fill_holes_with_zeros = std::get<2>(GetParam());
        config.cache_size__blocks = std::get<3>(GetParam());
        config.auto_batch_size = 0;
        config.wal_max_size_bytes = 1;
        config.fragmentation_threshold = 100;
        compio_build_lz4_compressor(&config.compressor);
        if (!std::get<1>(GetParam())) {
            uint32_t state = 12345;
            for (auto *data : {&original, &replacement}) {
                for (char &byte : *data) {
                    state = state * 1664525u + 1013904223u;
                    byte = static_cast<char>(state >> 24);
                }
            }
        }
        archive = compio_open_archive(path, "w+", &config);
        ASSERT_NE(archive, nullptr);
        auto *file = compio_open_file("data", archive);
        ASSERT_NE(file, nullptr);
        key = {file->hash, 0};
        ASSERT_EQ(compio_write(original.data(), original.size(), file), original.size());
        ASSERT_EQ(compio_close_file(file), COMPIO_SUCCESS);
        compio_flush(archive);
        slot = archive->current_header_slot;
        sequence = readonly(archive->header, compio::header)->sequence_id;
        saved_header.resize(readonly(archive->header, compio::header)->disk_size());
        ASSERT_EQ(pread(fileno(archive->file), saved_header.data(), saved_header.size(),
                        slot * saved_header.size()), static_cast<ssize_t>(saved_header.size()));
        auto value = archive->index->get(key);
        ASSERT_TRUE(value.has_value());
        compio::storage_block stored;
        ASSERT_TRUE(stored.read_from(archive->file, value->addr));
        ASSERT_EQ(stored.is_compressed(), std::get<1>(GetParam()));
        block = archive->block_reader->read_block(value->addr, key);
        ASSERT_NE(block, nullptr);
        ASSERT_EQ(block->size(), replacement.size());
        memcpy(block->data(), replacement.data(), replacement.size());
    }

    void TearDown() override {
        fault = {};
        block.reset();
        if (archive) {
            EXPECT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
        }
        remove(path);
        remove((std::string(path) + ".wal").c_str());
    }

    void arm(failure kind, int write_call = 1) {
        fault = {};
        fault.stream = archive->file;
        fault.descriptor = fileno(archive->file);
        fault.target = -1;
        fault.next_seek = true;
        fault.kind = kind;
        fault.short_write_call = write_call;
    }

    void expect_checkpoint_unchanged() {
        EXPECT_TRUE(fault.triggered);
        EXPECT_EQ(archive->current_header_slot, slot);
        EXPECT_EQ(readonly(archive->header, compio::header)->sequence_id, sequence);
        std::vector<unsigned char> actual(saved_header.size());
        EXPECT_EQ(pread(fileno(archive->file), actual.data(), actual.size(), slot * actual.size()),
                  static_cast<ssize_t>(actual.size()));
        EXPECT_EQ(actual, saved_header);
        if (config.enable_wal) {
            EXPECT_GT(std::filesystem::file_size(std::string(path) + ".wal"), 0u);
        }
    }

    void retry_and_reopen() {
        fault = {};
        clearerr(archive->file);
        compio_flush(archive);
        EXPECT_GT(readonly(archive->header, compio::header)->sequence_id, sequence);
        block.reset();
        ASSERT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
        archive = compio_open_archive(path, "r", &config);
        ASSERT_NE(archive, nullptr);
        auto *file = compio_open_file("data", archive);
        ASSERT_NE(file, nullptr);
        std::string actual(replacement.size(), '\0');
        EXPECT_EQ(compio_read(actual.data(), actual.size(), file), actual.size());
        EXPECT_EQ(actual, replacement);
        EXPECT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    }

    void check_write_failure(failure kind, int write_call = 1) {
        ASSERT_EQ(fflush(archive->file), 0);
        const auto old_addr = block->addr();
        const auto old_size = block->c_size() + STORAGE_BLOCK_METASIZE;
        std::vector<unsigned char> old_bytes(old_size);
        ASSERT_EQ(pread(fileno(archive->file), old_bytes.data(), old_size, old_addr),
                  static_cast<ssize_t>(old_size));
        arm(kind, write_call);
        compio_flush(archive);
        expect_checkpoint_unchanged();
        EXPECT_EQ(block->addr(), old_addr);
        auto value = archive->index->get(key);
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(value->addr, old_addr);
        EXPECT_EQ(memcmp(block->data(), replacement.data(), replacement.size()), 0);
        std::vector<unsigned char> actual(old_size);
        ASSERT_EQ(pread(fileno(archive->file), actual.data(), old_size, old_addr),
                  static_cast<ssize_t>(old_size));
        EXPECT_EQ(actual, old_bytes);
        retry_and_reopen();
    }
};

TEST_P(BlockWriteTest, SeekFailurePreservesOldBlockAndRetries) {
    check_write_failure(failure::seek);
}

TEST_P(BlockWriteTest, MetadataWriteFailurePreservesOldBlockAndRetries) {
    check_write_failure(failure::short_write);
}

TEST_P(BlockWriteTest, PayloadWriteFailurePreservesOldBlockAndRetries) {
    check_write_failure(failure::short_write, 2);
}

TEST_P(BlockWriteTest, WalLogFailureBlocksPublicationAndRetries) {
    if (!config.enable_wal) GTEST_SKIP();
    const auto old_addr = block->addr();
    arm(failure::seek);
    fault.next_seek = false;
    fault.wal_record = static_cast<int>(compio::WalRecordType::BLOCK);
    compio_flush(archive);
    expect_checkpoint_unchanged();
    EXPECT_EQ(block->addr(), old_addr);
    retry_and_reopen();
}

TEST_P(BlockWriteTest, WalCommitFailurePreservesBlockAndBalancesTransaction) {
    if (!config.enable_wal) GTEST_SKIP();
    const auto old_addr = block->addr();
    arm(failure::seek);
    fault.next_seek = false;
    fault.wal_record = static_cast<int>(compio::WalRecordType::COMMIT);
    EXPECT_FALSE(block->write_back());
    EXPECT_TRUE(fault.triggered);
    EXPECT_EQ(block->addr(), old_addr);
    retry_and_reopen();
}

TEST_P(BlockWriteTest, BatchReportsBlockWriteFailure) {
    ASSERT_EQ(compio_begin_batch(archive), COMPIO_SUCCESS);
    arm(failure::seek);
    EXPECT_EQ(compio_end_batch(archive), COMPIO_ERROR);
    expect_checkpoint_unchanged();
    retry_and_reopen();
}

TEST_P(BlockWriteTest, CloseReportsBlockWriteFailureWithoutPublishing) {
    arm(failure::short_write, 2);
    EXPECT_EQ(compio_close_archive(archive), COMPIO_ERROR);
    archive = nullptr;
    EXPECT_TRUE(fault.triggered);
    fault = {};
    FILE *stream = fopen(path, "rb");
    ASSERT_NE(stream, nullptr);
    std::vector<unsigned char> actual(saved_header.size());
    EXPECT_EQ(pread(fileno(stream), actual.data(), actual.size(), slot * actual.size()),
              static_cast<ssize_t>(actual.size()));
    EXPECT_EQ(actual, saved_header);
    EXPECT_EQ(fclose(stream), 0);
    if (config.enable_wal) {
        EXPECT_GT(std::filesystem::file_size(std::string(path) + ".wal"), 0u);
    }
}

TEST_P(BlockWriteTest, EvictionFailureRetainsBlockAndReachesCheckpoint) {
    const auto old_addr = block->addr();
    block.reset();
    arm(failure::seek);
    fault.every_seek = true;
    for (uint64_t i = 1; i < 8 && !fault.triggered; ++i) {
        auto other = archive->block_reader->create_block(4096, {key.hash, i * 4096});
        memset(other->data(), 'x', other->size());
    }
    ASSERT_TRUE(fault.triggered);
    fault = {};
    clearerr(archive->file);
    block = archive->block_reader->read_block(old_addr, key);
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(memcmp(block->data(), replacement.data(), replacement.size()), 0);
    compio_flush(archive);
    EXPECT_EQ(archive->current_header_slot, slot);
    EXPECT_EQ(readonly(archive->header, compio::header)->sequence_id, sequence);
    retry_and_reopen();
}

TEST_P(BlockWriteTest, PinnedBlockStaysCanonicalAcrossEvictionAndClear) {
    for (uint64_t i = 1; i < 8; ++i) {
        auto other = archive->block_reader->create_block(4096, {key.hash, i * 4096});
        memset(other->data(), 'x', other->size());
    }
    ASSERT_TRUE(archive->block_reader->clear_cache());
    auto same = archive->block_reader->read_block(block->addr(), key);
    ASSERT_EQ(same.get(), block.get());
    memcpy(same->data(), replacement.data(), replacement.size());
    same.reset();
    retry_and_reopen();
}

TEST_P(BlockWriteTest, ClearFailureRemainsVisibleToCheckpoint) {
    arm(failure::seek);
    EXPECT_FALSE(archive->block_reader->clear_cache());
    EXPECT_TRUE(fault.triggered);
    EXPECT_TRUE(archive->block_reader->has_write_error());
    fault = {};
    clearerr(archive->file);
    compio_flush(archive);
    EXPECT_EQ(archive->current_header_slot, slot);
    EXPECT_EQ(readonly(archive->header, compio::header)->sequence_id, sequence);
    retry_and_reopen();
}

TEST_P(BlockWriteTest, EvictionFailurePreventsAutomaticWalCheckpoint) {
    if (!config.enable_wal) GTEST_SKIP();
    block.reset();
    arm(failure::seek);
    fault.every_seek = true;
    auto *file = compio_open_file("other", archive);
    ASSERT_NE(file, nullptr);
    const std::string data(8 * config.block_size, 'x');
    EXPECT_EQ(compio_write(data.data(), data.size(), file), data.size());
    EXPECT_TRUE(fault.triggered);
    EXPECT_TRUE(archive->block_reader->has_write_error());
    EXPECT_GT(std::filesystem::file_size(std::string(path) + ".wal"), 0u);
    EXPECT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    fault = {};
    clearerr(archive->file);
    // The commit of the write already reported the failure; the blocks stayed
    // dirty, so this flush retries them and publishes.
    compio_flush(archive);
    EXPECT_GT(readonly(archive->header, compio::header)->sequence_id, sequence);
    retry_and_reopen();
    file = compio_open_file("other", archive);
    ASSERT_NE(file, nullptr);
    std::string actual(data.size(), '\0');
    EXPECT_EQ(compio_read(actual.data(), actual.size(), file), actual.size());
    EXPECT_EQ(actual, data);
    EXPECT_EQ(compio_close_file(file), COMPIO_SUCCESS);
}

TEST_P(BlockWriteTest, FailedFlushPreventsAutomaticWalCheckpointUntilRetry) {
    if (!config.enable_wal) GTEST_SKIP();
    arm(failure::seek);
    compio_flush(archive);
    expect_checkpoint_unchanged();
    fault = {};
    clearerr(archive->file);
    EXPECT_TRUE(archive->block_reader->has_write_error());
    auto *file = compio_open_file("other", archive);
    ASSERT_NE(file, nullptr);
    const std::string data(128, 'x');
    EXPECT_EQ(compio_write(data.data(), data.size(), file), data.size());
    EXPECT_EQ(compio_close_file(file), COMPIO_SUCCESS);
    retry_and_reopen();
}

TEST_P(BlockWriteTest, UnpublishedReplacementSurvivesFailedRewrite) {
    ASSERT_TRUE(block->write_back());
    replacement = original;
    memcpy(block->data(), replacement.data(), replacement.size());
    check_write_failure(failure::short_write, 2);
}

TEST_P(BlockWriteTest, FlushFailurePreservesOldBlockAndRetries) {
    check_write_failure(failure::flush);
}

TEST_P(BlockWriteTest, RealStreamErrorCanRetryWithoutCallerClearingError) {
    const auto old_addr = block->addr();
    const int descriptor = fileno(archive->file);
    const int saved = dup(descriptor);
    ASSERT_GE(saved, 0);
    const int read_only = open(path, O_RDONLY);
    if (read_only < 0) {
        close(saved);
        FAIL() << "failed to open read-only descriptor";
    }
    const int replaced = dup2(read_only, descriptor);
    close(read_only);
    if (replaced < 0) {
        close(saved);
        FAIL() << "failed to replace descriptor";
    }
    EXPECT_FALSE(block->write_back());
    EXPECT_NE(ferror(archive->file), 0);
    EXPECT_EQ(block->addr(), old_addr);
    const int restored = dup2(saved, descriptor);
    close(saved);
    ASSERT_GE(restored, 0);
    // The C API exposes no FILE*: the writer must clear its own sticky error.
    compio_flush(archive);
    EXPECT_FALSE(archive->block_reader->has_write_error());
    retry_and_reopen();
}

INSTANTIATE_TEST_SUITE_P(
    WalCompressionReclamationAndCache, BlockWriteTest,
    ::testing::Combine(::testing::Bool(), ::testing::Bool(), ::testing::Bool(),
                       ::testing::Values(0, 1, 3)));

namespace {

// The descriptor stdio holds for `target`, found through /proc.
int descriptor_of(const std::string &target) {
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        std::error_code ec;
        if (std::filesystem::read_symlink(entry.path(), ec) == target) return std::stoi(entry.path().filename());
    }
    return -1;
}

std::string read_a(compio_archive *archive) {
    compio_file *file = compio_open_file("a", archive);
    if (!file) return "";
    char buf[16] = {};
    const size_t n = compio_read(buf, sizeof(buf), file);
    compio_close_file(file);
    return std::string(buf, n);
}

}  // namespace

TEST(WalSyncFailure, WalSyncFailureBreaksArchive) {
    char path[256];
    generate_tmp_fn(path, sizeof(path));
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.wal_sync_mode = COMPIO_WAL_SYNC_ALWAYS;
    compio_archive *archive = compio_open_archive(path, "w", &cfg);
    ASSERT_NE(archive, nullptr);
    compio_file *file = compio_open_file("a", archive);
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(compio_write("base", 4, file), 4u);
    compio_flush(archive);
    const uint64_t sequence = readonly(archive->header, compio::header)->sequence_id;

    fault = {};
    fault.descriptor = descriptor_of(std::string(std::filesystem::absolute(path)) + ".wal");
    ASSERT_GE(fault.descriptor, 0);
    fault.publishing = true;
    fault.kind = failure::sync;
    compio_seek(file, 0, COMPIO_SEEK_SET);
    compio_write("neww", 4, file);
    compio_flush(archive);
    EXPECT_TRUE(fault.triggered);
    EXPECT_TRUE(archive->is_broken());

    fault = {};
    errno = 0;
    EXPECT_EQ(compio_write("more", 4, file), 0u);
    EXPECT_EQ(errno, EIO);
    compio_flush(archive);
    EXPECT_EQ(readonly(archive->header, compio::header)->sequence_id, sequence);
    compio_close_file(file);
    EXPECT_EQ(compio_close_archive(archive), COMPIO_ERROR);

    archive = compio_open_archive(path, "r+", &cfg);
    ASSERT_NE(archive, nullptr);
    const std::string content = read_a(archive);
    EXPECT_TRUE(content == "base" || content == "neww") << content;
    EXPECT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
    remove(path);
    remove((std::string(path) + ".wal").c_str());
}
