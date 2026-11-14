#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <thread>
#include "compio.h"
#include "compio/file.hpp"
#include "compio/wal.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;

// sequence_id of the newest valid header slot: the epoch a journal must carry.
static uint64_t published_epoch(const char *path) {
    FILE *f = fopen(path, "rb");
    uint64_t epoch = 0;
    for (uint64_t slot : {uint64_t{0}, compio::header::disk_size_bytes}) {
        compio::header h;
        if (h.load_and_validate(f, slot, true) && h.sequence_id > epoch) epoch = h.sequence_id;
    }
    fclose(f);
    return epoch;
}

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
        wal.set_epoch(published_epoch(filename));

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
        wal.set_epoch(published_epoch(filename));
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
        wal.set_epoch(published_epoch(filename));

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
        wal.set_epoch(published_epoch(filename));

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

// Sequential writes are batched automatically, and a batch defers the commit
// and the header. A flush must still make everything written so far durable:
// the state of the files right after it is what a crash would leave behind.
TEST_F(WalRecoveryTest, FlushIsDurableDuringSequentialWrites) {
    for (const bool wal_enabled : {true, false}) {
        compio_config config;
        compio_build_default_config(&config);
        config.block_size = 4096;
        config.max_files = 16;
        config.enable_wal = wal_enabled;

        const std::vector<uint8_t> chunk(3000, 0xC7);
        constexpr int CHUNKS = 6; // enough consecutive writes to start a batch

        compio_archive* archive = compio_open_archive(filename, "w", &config);
        ASSERT_NE(archive, nullptr);
        compio_file* file = compio_open_file("data", archive);
        ASSERT_NE(file, nullptr);
        for (int i = 0; i < CHUNKS; i++) {
            ASSERT_EQ(compio_write(chunk.data(), chunk.size(), file), chunk.size());
        }
        compio_flush(archive);

        // What is on disk now, as a crash would leave it.
        const std::string crashed = std::string(filename) + ".crashed";
        fs::copy_file(filename, crashed, fs::copy_options::overwrite_existing);
        if (fs::exists(wal_filename)) {
            fs::copy_file(wal_filename, crashed + ".wal", fs::copy_options::overwrite_existing);
        }
        compio_close_file(file);
        compio_close_archive(archive);

        compio_archive* reopened = compio_open_archive(crashed.c_str(), "r+", &config);
        ASSERT_NE(reopened, nullptr) << "wal=" << wal_enabled;
        compio_file* data = compio_open_file("data", reopened);
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(compio_get_size(data), CHUNKS * chunk.size()) << "wal=" << wal_enabled;
        std::vector<uint8_t> back(CHUNKS * chunk.size());
        EXPECT_EQ(compio_read(back.data(), back.size(), data), back.size());
        EXPECT_EQ(back, std::vector<uint8_t>(back.size(), 0xC7));
        compio_close_file(data);
        compio_close_archive(reopened);

        fs::remove(crashed);
        fs::remove(crashed + ".wal");
        fs::remove(filename);
        fs::remove(wal_filename);
    }
}

namespace {

std::string slurp(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void spit(const std::string &path, const std::string &bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_whole_file(compio_archive *archive, const char *name) {
    compio_file *f = compio_open_file(name, archive);
    if (!f) return "";
    char buf[64] = {};
    const size_t n = compio_read(buf, sizeof(buf), f);
    compio_close_file(f);
    return std::string(buf, n);
}

void write_whole_file(compio_archive *archive, const char *name, const std::string &text) {
    compio_file *f = compio_open_file(name, archive);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(compio_write(text.data(), text.size(), f), text.size());
    compio_close_file(f);
}

}  // namespace

// Raw archive file plus a one-block committed journal stamped with `epoch`.
static void make_journal(const char *archive_path, uint64_t epoch) {
    {
        std::ofstream out(archive_path, std::ios::binary | std::ios::trunc);
        const std::string zeros(8192, '\0');
        out.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    compio::WalManager wal(archive_path);
    ASSERT_TRUE(wal.open());
    wal.set_epoch(epoch);
    wal.begin_transaction();
    ASSERT_TRUE(wal.log_write(compio::WalRecordType::BLOCK, 4096, "ABCD", 4));
    ASSERT_TRUE(wal.commit_transaction());
    wal.close();
}

static void flip_wal_bit(const std::string &wal_path, std::streamoff offset) {
    std::string bytes = slurp(wal_path);
    ASSERT_LT(static_cast<size_t>(offset), bytes.size());
    bytes[static_cast<size_t>(offset)] ^= 0x01;
    spit(wal_path, bytes);
}

static std::string recovered_bytes(const char *archive_path, long offset, size_t n) {
    std::ifstream in(archive_path, std::ios::binary);
    in.seekg(offset);
    std::string out(n, '\0');
    in.read(&out[0], static_cast<std::streamsize>(n));
    return out;
}

// Offsets inside a record: type(1) epoch(8) addr(8) size(8) checksum(4).
TEST_F(WalRecoveryTest, CorruptedRecordAddressIsNotReplayed) {
    make_journal(filename, 7);
    flip_wal_bit(wal_filename, 9);

    FILE *f = fopen(filename, "r+b");
    ASSERT_NE(f, nullptr);
    compio::WalManager wal(filename);
    EXPECT_TRUE(wal.recover(f, 7));
    fclose(f);

    EXPECT_EQ(recovered_bytes(filename, 4096, 4), std::string(4, '\0'));
    EXPECT_EQ(recovered_bytes(filename, 4097, 4), std::string(4, '\0'));
}

TEST_F(WalRecoveryTest, CorruptedRecordSizeStopsReplay) {
    make_journal(filename, 7);
    flip_wal_bit(wal_filename, 17);

    FILE *f = fopen(filename, "r+b");
    ASSERT_NE(f, nullptr);
    compio::WalManager wal(filename);
    EXPECT_TRUE(wal.recover(f, 7));
    fclose(f);

    EXPECT_EQ(recovered_bytes(filename, 4096, 4), std::string(4, '\0'));
}

TEST_F(WalRecoveryTest, StaleEpochJournalIsIgnored) {
    make_journal(filename, 5);

    FILE *f = fopen(filename, "r+b");
    ASSERT_NE(f, nullptr);
    compio::WalManager wal(filename);
    EXPECT_TRUE(wal.recover(f, 6));
    fclose(f);

    EXPECT_EQ(recovered_bytes(filename, 4096, 4), std::string(4, '\0'));
    EXPECT_FALSE(wal.has_pending_recovery());
}

TEST_F(WalRecoveryTest, MatchingEpochJournalIsReplayed) {
    make_journal(filename, 6);

    FILE *f = fopen(filename, "r+b");
    ASSERT_NE(f, nullptr);
    compio::WalManager wal(filename);
    EXPECT_TRUE(wal.recover(f, 6));
    fclose(f);

    EXPECT_EQ(recovered_bytes(filename, 4096, 4), "ABCD");
}

// Power cut after the header was published but before the journal truncation reached
// the disk: the journal still describes the state before that publication.
TEST_F(WalRecoveryTest, StaleJournalAfterPublishIsNotReplayed) {
    compio_config config;
    compio_build_default_config(&config);
    config.wal_sync_mode = COMPIO_WAL_SYNC_ALWAYS;
    compio_archive *ar = compio_open_archive(filename, "w", &config);
    ASSERT_NE(ar, nullptr);
    write_whole_file(ar, "a", "base");
    compio_flush(ar);

    write_whole_file(ar, "a", "oldd");
    const std::string stale = slurp(wal_filename);
    ASSERT_FALSE(stale.empty());
    compio_flush(ar);
    write_whole_file(ar, "a", "neww");
    ASSERT_EQ(compio_close_archive(ar), COMPIO_SUCCESS);

    spit(wal_filename, stale);
    ar = compio_open_archive(filename, "r+", &config);
    ASSERT_NE(ar, nullptr);
    EXPECT_EQ(read_whole_file(ar, "a"), "neww");
    compio_close_archive(ar);
}

TEST_F(WalRecoveryTest, JournalWithoutPublishedHeaderIsDiscarded) {
    compio_config config;
    compio_build_default_config(&config);
    config.wal_sync_mode = COMPIO_WAL_SYNC_ALWAYS;
    compio_archive *ar = compio_open_archive(filename, "w", &config);
    ASSERT_NE(ar, nullptr);
    write_whole_file(ar, "a", "base");
    compio_flush(ar);
    write_whole_file(ar, "a", "old");
    const std::string journal = slurp(wal_filename);
    ASSERT_FALSE(journal.empty());
    ASSERT_EQ(compio_close_archive(ar), COMPIO_SUCCESS);

    spit(filename, "");
    spit(wal_filename, journal);
    ar = compio_open_archive(filename, "a", &config);
    ASSERT_NE(ar, nullptr);
    EXPECT_EQ(read_whole_file(ar, "a"), "");
    compio_close_archive(ar);
}

// A new archive killed before its first header was published: nothing exists for the
// journal to extend, so replaying it would only scribble over an empty file.
TEST_F(WalRecoveryTest, JournalOfUnpublishedNewArchiveIsDiscarded) {
    make_journal(filename, compio::WalManager::genesis_epoch);

    FILE *f = fopen(filename, "r+b");
    ASSERT_NE(f, nullptr);
    compio::WalManager wal(filename);
    EXPECT_TRUE(wal.recover(f, compio::WalManager::genesis_epoch));
    fclose(f);

    EXPECT_EQ(recovered_bytes(filename, 4096, 4), std::string(4, '\0'));
    EXPECT_FALSE(wal.has_pending_recovery());
}

// A new archive killed before its first flush: it must open, and what was committed to
// the journal must be there.
TEST_F(WalRecoveryTest, NewArchiveKilledBeforeFirstFlushKeepsCommittedWrites) {
    compio_config config;
    compio_build_default_config(&config);
    config.wal_sync_mode = COMPIO_WAL_SYNC_ALWAYS;
    compio_archive *ar = compio_open_archive(filename, "w", &config);
    ASSERT_NE(ar, nullptr);
    write_whole_file(ar, "a", "fresh");

    const std::string image = std::string(filename) + ".image";
    spit(image, slurp(filename));
    spit(image + ".wal", slurp(wal_filename));
    ASSERT_EQ(compio_close_archive(ar), COMPIO_SUCCESS);

    compio_archive *copy = compio_open_archive(image.c_str(), "r+", &config);
    ASSERT_NE(copy, nullptr) << "a new archive became unopenable after a kill";
    EXPECT_EQ(read_whole_file(copy, "a"), "fresh");
    compio_close_archive(copy);
    remove(image.c_str());
    remove((image + ".wal").c_str());
}

// Both headers unreadable: the archive is refused, and the journal that may hold the only
// copy of recent writes is left alone.
TEST_F(WalRecoveryTest, UnreadableHeadersDoNotConsumeTheJournal) {
    make_journal(filename, 3);
    const std::string journal = slurp(wal_filename);
    compio_config config;
    compio_build_default_config(&config);
    EXPECT_EQ(compio_open_archive(filename, "r+", &config), nullptr);
    EXPECT_TRUE(slurp(wal_filename) == journal);
}
