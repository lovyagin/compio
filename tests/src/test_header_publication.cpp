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
#include "test_util.hpp"

namespace {

enum class failure { seek, short_write, flush, sync };

struct io_fault {
    FILE *stream = nullptr;
    int descriptor = -1;
    off_t target = 0;
    failure kind = failure::seek;
    int writes = 0;
    bool publishing = false;
    bool triggered = false;
};

io_fault fault;

} // namespace

extern "C" {
int __real_fseeko(FILE *, off_t, int);
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
int __real_fflush(FILE *);
int __real_fsync(int);

int __wrap_fseeko(FILE *stream, off_t offset, int whence) {
    if (stream == fault.stream && whence == SEEK_SET && offset == fault.target) {
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
    if (stream == fault.stream && fault.publishing &&
        fault.kind == failure::short_write && ++fault.writes == 2) {
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

TEST_P(HeaderPublicationTest, CompactionCallbackReportsPublicationFailure) {
    arm();
    EXPECT_FALSE(archive->publish_header());
    expect_old_slot();
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
