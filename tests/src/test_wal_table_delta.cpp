#ifndef _WIN32

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "compio.h"
#include "test_util.hpp"

namespace {

constexpr uint64_t SIZE = 5000;

std::vector<uint8_t> pattern(uint8_t seed) { return std::vector<uint8_t>(SIZE, seed); }

}  // namespace

class WalTableDeltaTest : public ::testing::Test {
protected:
    char fn[256];
    std::string wal_fn;
    void SetUp() override {
        generate_tmp_fn(fn, sizeof(fn));
        wal_fn = std::string(fn) + ".wal";
    }
    void TearDown() override {
        remove(fn);
        remove(wal_fn.c_str());
    }
};

// Operations after the last published header change the files table in three ways: an
// entry grows, one is created, one is removed. The journal carries only those entries,
// so recovery must rebuild the whole table from them plus the published table.
TEST_F(WalTableDeltaTest, RecoveryAppliesChangedEntriesToPublishedTable) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.wal_sync_mode = COMPIO_WAL_SYNC_ALWAYS;

    {
        compio_archive *ar = compio_open_archive(fn, "w", &cfg);
        ASSERT_NE(ar, nullptr);
        for (const char *name : {"keep", "grow", "gone"}) {
            compio_file *f = compio_open_file(name, ar);
            ASSERT_NE(f, nullptr);
            const auto data = pattern(1);
            ASSERT_EQ(compio_write(data.data(), SIZE, f), SIZE);
            compio_close_file(f);
        }
        compio_close_archive(ar);
    }

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        compio_archive *ar = compio_open_archive(fn, "r+", &cfg);
        if (!ar) _exit(11);
        compio_file *grow = compio_open_file("grow", ar);
        compio_file *fresh = compio_open_file("fresh", ar);
        if (!grow || !fresh) _exit(12);
        const auto data = pattern(2);
        compio_seek(grow, 0, COMPIO_SEEK_END);
        if (compio_write(data.data(), SIZE, grow) != SIZE) _exit(13);
        if (compio_remove_file(ar, "gone") != 0) _exit(15);
        if (compio_write(data.data(), SIZE, fresh) != SIZE) _exit(14);
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    compio_archive *ar = compio_open_archive(fn, "r+", &cfg);
    ASSERT_NE(ar, nullptr);

    auto expect_file = [&](const char *name, uint64_t size) {
        compio_file *f = compio_open_file(name, ar);
        ASSERT_NE(f, nullptr) << name;
        EXPECT_EQ(compio_get_size(f), size) << name;
        compio_close_file(f);
    };
    expect_file("keep", SIZE);
    expect_file("grow", 2 * SIZE);
    expect_file("fresh", SIZE);

    compio_file *gone = compio_open_file("gone", ar);
    ASSERT_NE(gone, nullptr);
    EXPECT_EQ(compio_get_size(gone), 0u) << "removed file must come back empty";
    compio_close_file(gone);

    compio_close_archive(ar);
}

#endif
