#include <gtest/gtest.h>
#include <sys/stat.h>
#include "compio.h"
#include "compio/wal.hpp"
#include "test_util.hpp"
#include "compio/compio_file.hpp"

// We can't easily verify fsync calls without mocking, but we can verify that:
// 1. The configuration is accepted
// 2. The system operates correctly (no crashes, data is readable) in all modes
// 3. Different modes allow creating and reading archives

class WalSyncModeTest : public ::testing::Test {
protected:
    void SetUp() override {
        char fn[256];
        generate_tmp_fn(fn, sizeof(fn));
        test_file = fn;
        test_wal = test_file + ".wal";
    }

    void TearDown() override {
        remove(test_file.c_str());
        remove(test_wal.c_str());
    }

    std::string test_file;
    std::string test_wal;
};

TEST_F(WalSyncModeTest, ModeAlwaysWorks) {
    compio_config config;
    compio_build_default_config(&config);
    config.wal_sync_mode = COMPIO_WAL_SYNC_ALWAYS;

    compio_archive* archive = compio_open_archive(test_file.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);

    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);

    const char* data = "test_data";
    uint64_t written = compio_write(data, strlen(data), file);
    ASSERT_EQ(written, strlen(data));

    compio_close_file(file);
    compio_close_archive(archive);

    // Verify data
    config.wal_sync_mode = COMPIO_WAL_SYNC_ALWAYS;
    archive = compio_open_archive(test_file.c_str(), "r", &config);
    ASSERT_NE(archive, nullptr);
    
    file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    char buffer[16] = {0};
    compio_read(buffer, strlen(data), file);
    EXPECT_STREQ(buffer, data);
    
    compio_close_file(file);
    compio_close_archive(archive);
}

TEST_F(WalSyncModeTest, ModeNormalWorks) {
    compio_config config;
    compio_build_default_config(&config);
    config.wal_sync_mode = COMPIO_WAL_SYNC_NORMAL;

    compio_archive* archive = compio_open_archive(test_file.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);

    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);

    const char* data = "test_data_normal";
    uint64_t written = compio_write(data, strlen(data), file);
    ASSERT_EQ(written, strlen(data));

    // Force flush to ensure WAL is synced despite lazy mode
    compio_flush(archive);

    compio_close_file(file);
    compio_close_archive(archive);

    // Verify data
    config.wal_sync_mode = COMPIO_WAL_SYNC_NORMAL;
    archive = compio_open_archive(test_file.c_str(), "r", &config);
    ASSERT_NE(archive, nullptr);
    
    file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    char buffer[32] = {0};
    compio_read(buffer, strlen(data), file);
    EXPECT_STREQ(buffer, data);
    
    compio_close_file(file);
    compio_close_archive(archive);
}

TEST_F(WalSyncModeTest, ModeOffWorks) {
    compio_config config;
    compio_build_default_config(&config);
    config.wal_sync_mode = COMPIO_WAL_SYNC_OFF;

    compio_archive* archive = compio_open_archive(test_file.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);

    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);

    const char* data = "test_data_off";
    uint64_t written = compio_write(data, strlen(data), file);
    ASSERT_EQ(written, strlen(data));

    compio_close_file(file);
    compio_close_archive(archive);

    // Verify data
    config.wal_sync_mode = COMPIO_WAL_SYNC_OFF;
    archive = compio_open_archive(test_file.c_str(), "r", &config);
    ASSERT_NE(archive, nullptr);
    
    file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    
    char buffer[32] = {0};
    compio_read(buffer, strlen(data), file);
    EXPECT_STREQ(buffer, data);
    
    compio_close_file(file);
    compio_close_archive(archive);
}

// enable_wal=false: no journal is maintained, yet a cleanly closed archive must
// still round-trip its data (writes go straight to the archive, header published
// on batch end / close). Also verifies no .wal file is left behind.
TEST_F(WalSyncModeTest, JournalDisabledRoundTrips) {
    compio_config config;
    compio_build_default_config(&config);
    config.enable_wal = false;

    // Enough data to span several blocks and trigger multiple auto-batch ends.
    std::string data(64 * 1024, '\0');
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<char>('A' + (i % 26));

    compio_archive* archive = compio_open_archive(test_file.c_str(), "w+", &config);
    ASSERT_NE(archive, nullptr);
    compio_file* file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(compio_write(data.data(), data.size(), file), data.size());
    compio_close_file(file);
    compio_close_archive(archive);

    // No journal should exist for a disabled-WAL archive.
    struct stat st;
    EXPECT_NE(stat(test_wal.c_str(), &st), 0);

    archive = compio_open_archive(test_file.c_str(), "r", &config);
    ASSERT_NE(archive, nullptr);
    file = compio_open_file("test", archive);
    ASSERT_NE(file, nullptr);
    std::string buffer(data.size(), '\0');
    ASSERT_EQ(compio_read(&buffer[0], buffer.size(), file), data.size());
    EXPECT_EQ(buffer, data);
    compio_close_file(file);
    compio_close_archive(archive);
}

// Regression test: Verify that reopening with a different sync mode doesn't break anything
TEST_F(WalSyncModeTest, ModeSwitching) {
    compio_config config;
    compio_build_default_config(&config);
    config.wal_sync_mode = COMPIO_WAL_SYNC_NORMAL;

    // Create with NORMAL
    {
        compio_archive* archive = compio_open_archive(test_file.c_str(), "w+", &config);
        ASSERT_NE(archive, nullptr);
        compio_file* file = compio_open_file("test", archive);
        ASSERT_NE(file, nullptr);
        compio_write("data", 4, file);
        compio_close_file(file);
        compio_close_archive(archive);
    }

    // Reopen with ALWAYS
    config.wal_sync_mode = COMPIO_WAL_SYNC_ALWAYS;
    {
        compio_archive* archive = compio_open_archive(test_file.c_str(), "r+", &config);
        ASSERT_NE(archive, nullptr);
        compio_file* file = compio_open_file("test", archive);
        ASSERT_NE(file, nullptr);
        char buffer[5] = {0};
        compio_read(buffer, 4, file);
        EXPECT_STREQ(buffer, "data");
        compio_close_file(file);
        compio_close_archive(archive);
    }
}