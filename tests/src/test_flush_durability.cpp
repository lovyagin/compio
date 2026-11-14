// Regression: compio_flush() is documented to "ensure durability of all prior
// operations". Previously flush_header_double_buffered() was gated on
// !(mode_b & mode_bit::r), which is also set for "r+" (read-write existing
// archive). So in r+ mode flush() never wrote the header; metadata only reached
// disk via the header object's in-place destructor write at close_archive. A
// crash (or _exit) after flush but before a clean close lost ALL metadata even
// though the data blocks were durably on disk -- the file read back as empty.
//
// This test writes records, flushes after each, then terminates the writer
// WITHOUT a clean close (child _exit), and verifies a fresh open sees the data.

#ifndef _WIN32

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "compio.h"
#include "test_util.hpp"

class FlushDurabilityTest : public ::testing::Test {
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

TEST_F(FlushDurabilityTest, FlushIsDurableWithoutCleanClose) {
    constexpr uint64_t RECSZ = 4096;
    constexpr uint64_t N = 10;

    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.block_size = 4096;
    cfg.wal_sync_mode = COMPIO_WAL_SYNC_ALWAYS;

    {
        compio_archive* ar = compio_open_archive(fn, "w", &cfg);
        ASSERT_NE(ar, nullptr);
        compio_close_archive(ar);
    }

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // Writer: flush after each record, then terminate WITHOUT clean close.
        compio_archive* ar = compio_open_archive(fn, "r+", &cfg);
        if (!ar) _exit(11);
        compio_file* f = compio_open_file("data", ar);
        if (!f) _exit(12);
        std::vector<uint8_t> buf(RECSZ);
        for (uint64_t i = 0; i < N; ++i) {
            std::fill(buf.begin(), buf.end(), static_cast<uint8_t>(i & 0xFF));
            if (compio_write(buf.data(), RECSZ, f) != RECSZ) _exit(13);
            compio_flush(ar);
        }
        _exit(0);  // durability must come from flush, not close
    }

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    compio_archive* ar = compio_open_archive(fn, "r+", &cfg);
    ASSERT_NE(ar, nullptr);
    compio_file* f = compio_open_file("data", ar);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(compio_get_size(f), RECSZ * N) << "flushed records must survive a crash before close";

    std::vector<uint8_t> buf(RECSZ);
    for (uint64_t i = 0; i < N; ++i) {
        compio_seek(f, static_cast<int64_t>(i * RECSZ), COMPIO_SEEK_SET);
        ASSERT_EQ(compio_read(buf.data(), RECSZ, f), RECSZ);
        for (uint64_t k = 0; k < RECSZ; ++k)
            ASSERT_EQ(buf[k], static_cast<uint8_t>(i & 0xFF)) << "record " << i << " byte " << k;
    }
    compio_close_file(f);
    compio_close_archive(ar);
}

#endif // _WIN32
