#ifndef _WIN32

#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>

#include "compio.h"
#include "compio/compio_file.hpp"
#include "test_util.hpp"

class BatchDurabilityTest : public ::testing::TestWithParam<bool> {
protected:
    char path[256];

    void SetUp() override { generate_tmp_fn(path, sizeof(path)); }

    void TearDown() override {
        remove(path);
        remove((std::string(path) + ".wal").c_str());
    }
};

TEST_P(BatchDurabilityTest, EndBatchReportsArchiveSyncFailure) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.max_files = 16;
    cfg.enable_wal = GetParam();
    cfg.wal_max_size_bytes = 0;

    auto *archive = compio_open_archive(path, "w", &cfg);
    ASSERT_NE(archive, nullptr);
    ASSERT_EQ(compio_begin_batch(archive), COMPIO_SUCCESS);

    int descriptors[2];
    ASSERT_EQ(pipe(descriptors), 0);
    FILE *pipe_stream = fdopen(descriptors[1], "w");
    ASSERT_NE(pipe_stream, nullptr);

    // A pipe accepts fflush but cannot be fsynced. Restore the real stream
    // before closing the archive so cleanup uses the normal persistence path.
    FILE *original = archive->file;
    archive->file = pipe_stream;
    errno = 0;
    const int result = compio_end_batch(archive);
    const int sync_error = errno;
    archive->file = original;
    fclose(pipe_stream);
    close(descriptors[0]);

    EXPECT_EQ(result, COMPIO_ERROR);
    EXPECT_NE(sync_error, 0);
    EXPECT_EQ(compio_close_archive(archive), COMPIO_SUCCESS);
}

INSTANTIATE_TEST_SUITE_P(WalModes, BatchDurabilityTest, ::testing::Bool());

#endif // _WIN32
