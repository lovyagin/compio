#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <thread>
#include "compio.h"
#include "compio/wal.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;

class WalRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        generate_tmp_fn(filename, sizeof(filename));
        wal_filename = std::string(filename) + ".wal";
    }

    void TearDown() override {
        if (fs::exists(filename)) {
            fs::remove(filename);
        }
        if (fs::exists(wal_filename)) {
            fs::remove(wal_filename);
        }
    }

    char filename[256];
    std::string wal_filename;
};

TEST_F(WalRecoveryTest, RecoversDataFromWal) {
    // 1. Create an empty archive
    compio_config config;
    compio_build_default_config(&config);
    config.block_size = 4096;
    compio_archive* archive = compio_open_archive(filename, "w", &config);
    ASSERT_NE(archive, nullptr);
    compio_close_archive(archive);

    uint64_t safe_offset = 1024 * 1024; // 1MB

    // 2. Simulate a crash: manually write to WAL without updating the archive file
    {
        compio::WalManager wal(filename);
        ASSERT_TRUE(wal.open());

        // Prepare dummy data for a block
        std::vector<uint8_t> data(config.block_size, 'A');
        
        // Extend file first
        FILE* f = fopen(filename, "r+b");
        ASSERT_NE(f, nullptr);
        fseek(f, safe_offset + config.block_size, SEEK_SET);
        fputc(0, f);
        fclose(f);

        wal.begin_transaction();
        ASSERT_TRUE(wal.log_write(compio::WalRecordType::BLOCK, safe_offset, data.data(), data.size()));
        wal.commit_transaction(); // Implicitly syncs
        wal.close();
    }

    // 3. Open the archive again.
    archive = compio_open_archive(filename, "r+", &config);
    ASSERT_NE(archive, nullptr);
    compio_close_archive(archive);

    std::ifstream file(filename, std::ios::binary);
    file.seekg(safe_offset);
    std::vector<uint8_t> read_data(config.block_size);
    file.read(reinterpret_cast<char*>(read_data.data()), config.block_size);

    for (int i = 0; i < config.block_size; ++i) {
        ASSERT_EQ(read_data[i], 'A') << "Mismatch at index " << i;
    }
}

TEST_F(WalRecoveryTest, ClearsWalAfterSuccessfulOpen) {
    // 1. Create and close archive
    compio_config config;
    compio_build_default_config(&config);
    compio_archive* archive = compio_open_archive(filename, "w", &config);
    ASSERT_NE(archive, nullptr);
    compio_close_archive(archive);

    // 2. Create a WAL with some entries
    {
        compio::WalManager wal(filename);
        ASSERT_TRUE(wal.open());
        std::vector<uint8_t> data(10, 'B');
        
        // Ensure file is big enough
        FILE* f = fopen(filename, "r+b");
        ASSERT_NE(f, nullptr);
        fseek(f, 2000000, SEEK_SET);
        fputc(0, f);
        fclose(f);

        wal.begin_transaction();
        wal.log_write(compio::WalRecordType::BLOCK, 1024 * 1024, data.data(), data.size());
        wal.commit_transaction();
        wal.close();
    }
    
    ASSERT_TRUE(fs::file_size(wal_filename) > 0);

    struct ScopedArchive {
        compio_archive* archive;
        ScopedArchive(compio_archive* a) : archive(a) {}
        ~ScopedArchive() { if (archive) compio_close_archive(archive); }
    };

    // 3. Open archive (triggers recovery and clear)
    archive = compio_open_archive(filename, "r+", &config);
    ASSERT_NE(archive, nullptr);
    ScopedArchive guard(archive);
    
    // We check WAL size while archive is open - it should be empty (cleared)
    uintmax_t size = fs::file_size(wal_filename);
    ASSERT_EQ(size, 0);

    // compio_close_archive(archive); // Handled by ScopedArchive
}
