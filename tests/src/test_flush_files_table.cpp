// Regression: compio_flush() called while a file is open must not corrupt the
// archive. Previously sync_files_table() relocated the files-table block via
// copy-on-write without growing header->file_size, then save_state() stamped
// file_size at the end of the allocator-state region (below the table). The
// persisted file_size was smaller than files_table_addr + table_size, so on
// reopen the bulk read of the table overran EOF and both header copies were
// rejected ("Both archive headers are corrupted").

#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "compio.h"
#include "test_util.hpp"

class FlushFilesTableTest : public ::testing::Test {
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

// Write records, flushing after each, then close cleanly and reopen.
// The archive must reopen and the data must read back byte-for-byte.
TEST_F(FlushFilesTableTest, FlushWhileWritingKeepsArchiveOpenable) {
    constexpr uint64_t RECSZ = 4096;
    constexpr uint64_t N = 5;

    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.block_size = 4096;

    {
        compio_archive* ar = compio_open_archive(fn, "w", &cfg);
        ASSERT_NE(ar, nullptr);
        compio_file* f = compio_open_file("data", ar);
        ASSERT_NE(f, nullptr);

        std::vector<uint8_t> buf(RECSZ);
        for (uint64_t i = 0; i < N; ++i) {
            std::fill(buf.begin(), buf.end(), static_cast<uint8_t>(i & 0xFF));
            ASSERT_EQ(compio_write(buf.data(), RECSZ, f), RECSZ);
            compio_flush(ar);  // mid-write durability primitive
        }
        compio_close_file(f);
        compio_close_archive(ar);
    }

    compio_archive* ar = compio_open_archive(fn, "r+", &cfg);
    ASSERT_NE(ar, nullptr) << "archive must reopen after mid-write flushes";
    compio_file* f = compio_open_file("data", ar);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(compio_get_size(f), RECSZ * N);

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
