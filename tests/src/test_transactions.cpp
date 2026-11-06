#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <random>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "compio.h"
#include "test_util.hpp"

namespace {

// Incompressible, so that the bytes a transaction writes take space in the container.
std::vector<uint8_t> noise(size_t size, unsigned seed) {
    std::vector<uint8_t> data(size);
    uint32_t x = seed * 2654435761u + 1;
    for (auto &b : data) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = static_cast<uint8_t>(x);
    }
    return data;
}

std::vector<uint8_t> pattern(size_t size, unsigned seed) {
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>((i / 5) * 13 + seed * 7);
    return data;
}

uint64_t file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fclose(f);
    return static_cast<uint64_t>(size);
}

// Runs every case with and without the journal: a transaction must not depend on it.
class TransactionTest : public ::testing::TestWithParam<bool> {
protected:
    char fn[256];
    compio_config config;

    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));
        compio_build_default_config(&config);
        config.block_size = 4096;
        config.block_size__minimum = 1024;
        config.block_size__maximum = 8192;
        // A small cache makes the transaction write blocks and nodes out before it ends.
        config.cache_size__blocks = 4;
        config.cache_size__nodes = 8;
        config.b_tree_degree = 4;
        config.enable_wal = GetParam();
    }

    compio_archive *open(const char *mode) { return compio_open_archive(fn, mode, &config); }

    static void write_file(compio_archive *archive, const char *name, const std::vector<uint8_t> &data) {
        compio_file *file = compio_open_file(name, archive);
        ASSERT_NE(file, nullptr);
        ASSERT_EQ(compio_write(data.data(), data.size(), file), data.size());
        ASSERT_EQ(compio_close_file(file), 0);
    }

    static std::vector<uint8_t> read_file(compio_archive *archive, const char *name) {
        compio_file *file = compio_open_file(name, archive);
        if (!file) return {};
        std::vector<uint8_t> data(compio_get_size(file));
        const uint64_t got = compio_read(data.data(), data.size(), file);
        compio_close_file(file);
        data.resize(got);
        return data;
    }

    static bool has_file(compio_archive *archive, const char *name) {
        compio_file *file = compio_open_file(name, archive);
        if (!file) return false;
        compio_close_file(file);
        return true;
    }
};

TEST_P(TransactionTest, CommitIsDurable) {
    const auto data = pattern(50000, 1);
    compio_archive *archive = open("w+");
    ASSERT_NE(archive, nullptr);
    ASSERT_EQ(compio_begin_transaction(archive), COMPIO_SUCCESS);
    write_file(archive, "a", data);
    write_file(archive, "b", data);
    ASSERT_EQ(compio_commit_transaction(archive), COMPIO_SUCCESS);
    ASSERT_EQ(compio_close_archive(archive), 0);

    archive = open("r");
    ASSERT_NE(archive, nullptr);
    EXPECT_EQ(read_file(archive, "a"), data);
    EXPECT_EQ(read_file(archive, "b"), data);
    compio_close_archive(archive);
}

TEST_P(TransactionTest, AbortRestoresTheStartingState) {
    const auto kept = pattern(60000, 2);
    const auto doomed = noise(60000, 3);
    compio_archive *archive = open("w+");
    ASSERT_NE(archive, nullptr);
    write_file(archive, "kept", kept);
    write_file(archive, "removed", kept);
    compio_flush(archive);
    const uint64_t size_before = file_size(fn);

    ASSERT_EQ(compio_begin_transaction(archive), COMPIO_SUCCESS);
    compio_file *open_kept = compio_open_file("kept", archive);
    ASSERT_NE(open_kept, nullptr);
    ASSERT_EQ(compio_seek(open_kept, 1000, COMPIO_SEEK_SET), 0);
    ASSERT_EQ(compio_write(doomed.data(), doomed.size(), open_kept), doomed.size());
    compio_file *created = compio_open_file("created", archive);
    ASSERT_NE(created, nullptr);
    ASSERT_EQ(compio_write(doomed.data(), doomed.size(), created), doomed.size());
    ASSERT_EQ(compio_remove_file(archive, "removed"), 0);
    // A flush inside the transaction writes data out but publishes nothing.
    compio_flush(archive);
    write_file(archive, "late", doomed);

    ASSERT_EQ(compio_abort_transaction(archive), COMPIO_SUCCESS);

    // Open files follow the restored state.
    EXPECT_EQ(compio_get_size(open_kept), kept.size());
    std::vector<uint8_t> back(kept.size());
    ASSERT_EQ(compio_seek(open_kept, 0, COMPIO_SEEK_SET), 0);
    ASSERT_EQ(compio_read(back.data(), back.size(), open_kept), back.size());
    EXPECT_EQ(back, kept);
    EXPECT_EQ(compio_close_file(open_kept), 0);
    errno = 0;
    EXPECT_EQ(compio_write(doomed.data(), 10, created), 0u);
    EXPECT_EQ(errno, ENOENT);
    compio_close_file(created);

    EXPECT_EQ(read_file(archive, "removed"), kept);
    ASSERT_EQ(compio_close_archive(archive), 0);
    // The space the abandoned changes took past the container end is given back.
    EXPECT_LT(file_size(fn), size_before + doomed.size() / 4);

    archive = open("r");
    ASSERT_NE(archive, nullptr);
    EXPECT_EQ(read_file(archive, "kept"), kept);
    EXPECT_EQ(read_file(archive, "removed"), kept);
    EXPECT_FALSE(has_file(archive, "created"));
    EXPECT_FALSE(has_file(archive, "late"));
    compio_close_archive(archive);
}

TEST_P(TransactionTest, ArchiveStaysWritableAfterAbort) {
    const auto first = pattern(30000, 4);
    const auto second = pattern(45000, 5);
    compio_archive *archive = open("w+");
    ASSERT_NE(archive, nullptr);
    write_file(archive, "f", first);

    ASSERT_EQ(compio_begin_transaction(archive), COMPIO_SUCCESS);
    write_file(archive, "f", second);
    ASSERT_EQ(compio_abort_transaction(archive), COMPIO_SUCCESS);

    ASSERT_EQ(compio_begin_transaction(archive), COMPIO_SUCCESS);
    write_file(archive, "g", second);
    ASSERT_EQ(compio_commit_transaction(archive), COMPIO_SUCCESS);
    write_file(archive, "h", first);
    ASSERT_EQ(compio_close_archive(archive), 0);

    archive = open("r");
    ASSERT_NE(archive, nullptr);
    EXPECT_EQ(read_file(archive, "f"), first);
    EXPECT_EQ(read_file(archive, "g"), second);
    EXPECT_EQ(read_file(archive, "h"), first);
    compio_close_archive(archive);
}

TEST_P(TransactionTest, ClosingAbortsAnOpenTransaction) {
    const auto data = pattern(20000, 6);
    compio_archive *archive = open("w+");
    ASSERT_NE(archive, nullptr);
    write_file(archive, "before", data);
    ASSERT_EQ(compio_begin_transaction(archive), COMPIO_SUCCESS);
    write_file(archive, "inside", data);
    ASSERT_EQ(compio_close_archive(archive), 0);

    archive = open("r");
    ASSERT_NE(archive, nullptr);
    EXPECT_EQ(read_file(archive, "before"), data);
    EXPECT_FALSE(has_file(archive, "inside"));
    compio_close_archive(archive);
}

TEST_P(TransactionTest, MisuseIsRejected) {
    compio_archive *archive = open("w+");
    ASSERT_NE(archive, nullptr);

    errno = 0;
    EXPECT_EQ(compio_commit_transaction(archive), COMPIO_ERROR);
    EXPECT_EQ(errno, EINVAL);
    errno = 0;
    EXPECT_EQ(compio_abort_transaction(archive), COMPIO_ERROR);
    EXPECT_EQ(errno, EINVAL);

    ASSERT_EQ(compio_begin_transaction(archive), COMPIO_SUCCESS);
    errno = 0;
    EXPECT_EQ(compio_begin_transaction(archive), COMPIO_ERROR);
    EXPECT_EQ(errno, EBUSY);
    errno = 0;
    EXPECT_EQ(compio_defragment(archive), COMPIO_ERROR);
    EXPECT_EQ(errno, EBUSY);
    uint64_t rewritten = 0;
    errno = 0;
    EXPECT_EQ(compio_recompress(archive, &rewritten), COMPIO_ERROR);
    EXPECT_EQ(errno, EBUSY);

    ASSERT_EQ(compio_begin_batch(archive), COMPIO_SUCCESS);
    if (GetParam()) {
        errno = 0;
        EXPECT_EQ(compio_commit_transaction(archive), COMPIO_ERROR);
        EXPECT_EQ(errno, EBUSY);
    }
    ASSERT_EQ(compio_end_batch(archive), COMPIO_SUCCESS);
    if (GetParam()) {
        errno = 0;
        EXPECT_EQ(compio_end_batch(archive), COMPIO_ERROR);
        EXPECT_EQ(errno, EINVAL);
    }
    EXPECT_EQ(compio_commit_transaction(archive), COMPIO_SUCCESS);

    if (GetParam()) {
        ASSERT_EQ(compio_begin_batch(archive), COMPIO_SUCCESS);
        errno = 0;
        EXPECT_EQ(compio_begin_transaction(archive), COMPIO_ERROR);
        EXPECT_EQ(errno, EBUSY);
        ASSERT_EQ(compio_end_batch(archive), COMPIO_SUCCESS);
    }
    compio_close_archive(archive);
}

#ifndef _WIN32
// A child commits transactions that rewrite several files with one generation
// number and is killed at a random moment. Every reopened archive must hold one
// generation in all files.
TEST_P(TransactionTest, KillLeavesWholeTransactions) {
    constexpr int files = 4;
    constexpr size_t size = 24000;
    std::mt19937 rng(GetParam() ? 11 : 12);

    for (int trial = 0; trial < 12; ++trial) {
        remove(fn);
        remove((std::string(fn) + ".wal").c_str());
        {
            compio_archive *archive = open("w+");
            ASSERT_NE(archive, nullptr);
            for (int i = 0; i < files; ++i) {
                write_file(archive, ("f" + std::to_string(i)).c_str(), std::vector<uint8_t>(size, 0));
            }
            ASSERT_EQ(compio_close_archive(archive), 0);
        }

        const pid_t child = fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            compio_archive *archive = open("r+");
            if (!archive) _exit(2);
            for (uint8_t generation = 1;; ++generation) {
                if (compio_begin_transaction(archive) != COMPIO_SUCCESS) _exit(3);
                for (int i = 0; i < files; ++i) {
                    compio_file *file = compio_open_file(("f" + std::to_string(i)).c_str(), archive);
                    if (!file) _exit(4);
                    const std::vector<uint8_t> data(size, generation);
                    if (compio_write(data.data(), data.size(), file) != data.size()) _exit(5);
                    compio_close_file(file);
                    if (i == 1) compio_flush(archive);
                }
                if (compio_commit_transaction(archive) != COMPIO_SUCCESS) _exit(6);
            }
        }

        usleep(20000 + rng() % 200000);
        kill(child, SIGKILL);
        int status = 0;
        waitpid(child, &status, 0);
        ASSERT_TRUE(WIFSIGNALED(status)) << "child exited with " << WEXITSTATUS(status);

        compio_archive *archive = open("r+");
        ASSERT_NE(archive, nullptr) << "trial " << trial;
        int generation = -1;
        for (int i = 0; i < files; ++i) {
            const auto data = read_file(archive, ("f" + std::to_string(i)).c_str());
            ASSERT_EQ(data.size(), size) << "trial " << trial << " file " << i;
            for (const uint8_t byte : data) ASSERT_EQ(byte, data[0]) << "trial " << trial << " file " << i;
            if (generation < 0) generation = data[0];
            EXPECT_EQ(data[0], generation) << "trial " << trial << " file " << i;
        }
        compio_close_archive(archive);
    }
}
#endif

INSTANTIATE_TEST_SUITE_P(Journal, TransactionTest, ::testing::Bool(),
                         [](const auto &info) { return info.param ? "Wal" : "NoWal"; });

} // namespace
