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
        ASSERT_TRUE(wal.commit_transaction()); // Implicitly syncs
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
        ASSERT_TRUE(wal.log_write(compio::WalRecordType::BLOCK, 1024 * 1024, data.data(), data.size()));
        ASSERT_TRUE(wal.commit_transaction());
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

TEST_F(WalRecoveryTest, IgnoresIncompleteTransactions) {
    compio_config config;
    compio_build_default_config(&config);
    compio_archive* archive = compio_open_archive(filename, "w", &config);
    compio_close_archive(archive);

    uint64_t addr = 1000;
    std::vector<uint8_t> data(100, 'X');

    // Ensure file is big enough
    FILE* f_init = fopen(filename, "r+b");
    if (f_init) {
        fseek(f_init, 2000, SEEK_SET);
        fputc(0, f_init);
        fclose(f_init);
    }

    {
        compio::WalManager wal(filename);
        ASSERT_TRUE(wal.open());

        // Transaction 1: Valid
        wal.begin_transaction();
        ASSERT_TRUE(wal.log_write(compio::WalRecordType::BLOCK, addr, data.data(), data.size()));
        ASSERT_TRUE(wal.commit_transaction());
        wal.close();

        // Transaction 2: Partial/Corrupt (append to file)
        FILE* f = fopen(wal_filename.c_str(), "ab");
        ASSERT_NE(f, nullptr);
        uint8_t garbage[] = {0xAA, 0xBB, 0xCC};
        fwrite(garbage, 1, sizeof(garbage), f);
        fclose(f);
    }

    // Open archive - should recover Trans 1 and ignore garbage
    archive = compio_open_archive(filename, "r+", &config);
    ASSERT_NE(archive, nullptr);
    compio_close_archive(archive);

    // Verify data from Trans 1
    std::ifstream file(filename, std::ios::binary);
    file.seekg(addr);
    std::vector<uint8_t> read_data(100);
    file.read(reinterpret_cast<char*>(read_data.data()), 100);
    for (int i=0; i<100; ++i) ASSERT_EQ(read_data[i], 'X');
}

TEST_F(WalRecoveryTest, NestedTransactionsAreAtomic) {
    // 1. Create and close archive
    compio_config config;
    compio_build_default_config(&config);
    compio_archive* archive = compio_open_archive(filename, "w", &config);
    ASSERT_NE(archive, nullptr);
    compio_close_archive(archive);
    
    uint64_t addr1 = 2000;
    uint64_t addr2 = 3000;
    
    // 2. Ensure file is large enough
    FILE* f = fopen(filename, "r+b");
    fseek(f, 4000, SEEK_SET);
    fputc(0, f);
    fclose(f);

    {
        compio::WalManager wal(filename);
        ASSERT_TRUE(wal.open());

        wal.begin_transaction(); // Outer
        
        const char* data1 = "OUTER";
        wal.log_write(compio::WalRecordType::BLOCK, addr1, data1, 5);
        
        wal.begin_transaction(); // Inner
        const char* data2 = "INNER";
        wal.log_write(compio::WalRecordType::BLOCK, addr2, data2, 5);
        
        // Commit inner - should NOT write COMMIT record to disk yet
        ASSERT_TRUE(wal.commit_transaction());
        
        // Close without outer commit
        wal.close(); 
    }

    // Open archive - verify NOTHING recovered
    archive = compio_open_archive(filename, "r+", &config);
    ASSERT_NE(archive, nullptr);
    compio_close_archive(archive);

    std::ifstream file(filename, std::ios::binary);
    file.seekg(addr1);
    char buf[6] = {0};
    file.read(buf, 5);
    // Should check that it is NOT "OUTER"
    // Since we initialized with zeros (implicitly or explicitly), it should be zero
    // Or at least not "OUTER"
    ASSERT_NE(std::string(buf), "OUTER"); 
    
    file.seekg(addr2);
    file.read(buf, 5);
    ASSERT_NE(std::string(buf), "INNER"); 
}

TEST_F(WalRecoveryTest, CompioFlushTruncatesWal) {
    // 1. Create archive
    compio_config config;
    compio_build_default_config(&config);
    config.block_size = 1024; // Small blocks
    config.cache_size__blocks = 2; // Small cache to force evictions if we write many
    
    compio_archive* archive = compio_open_archive(filename, "w", &config);
    ASSERT_NE(archive, nullptr);
    
    // 2. Write enough data to trigger WAL activity
    // Writing multiple blocks should cause cache evictions (writes to WAL)
    // or at least put them in cache.
    // compio_write itself doesn't guarantee WAL write until eviction or flush.
    // But flush guarantees everything.
    
    compio_file* f = compio_open_file("test_file", archive);
    ASSERT_NE(f, nullptr);
    
    std::vector<uint8_t> data(config.block_size * 5, 'D');
    ASSERT_EQ(compio_write(data.data(), data.size(), f), data.size());
    
    // At this point, WAL might be empty or not, depending on cache eviction.
    // But we want to test that flush truncates it.
    
    // Force a flush. This writes everything to WAL, syncs, then checkpoints (truncates).
    compio_flush(archive);
    
    // Verify WAL is empty
    ASSERT_TRUE(fs::exists(wal_filename));
    ASSERT_EQ(fs::file_size(wal_filename), 0);
    
    // Verify data is still readable
    std::vector<uint8_t> read_buf(data.size());
    compio_seek(f, 0, COMPIO_SEEK_SET);
    ASSERT_EQ(compio_read(read_buf.data(), read_buf.size(), f), data.size());
    ASSERT_EQ(read_buf, data);
    
    compio_close_file(f);
    compio_close_archive(archive);
}
