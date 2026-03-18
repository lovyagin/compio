/**
 * @file wal.hpp
 * @brief Write-Ahead Log (WAL) for crash recovery and atomicity
 */

#ifndef COMPIO_WAL_HPP_
#define COMPIO_WAL_HPP_

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <cstdio>

namespace compio {

enum class WalRecordType : uint8_t {
    BLOCK = 1,
    INDEX_NODE = 2,
    HEADER = 3,
    ALLOCATOR = 4,
    COMMIT = 255
};

struct WalRecordHeader {
    WalRecordType type;
    uint64_t addr; // Address in the main archive file
    uint64_t size; // Size of the payload
    uint32_t checksum; // Checksum of the payload
};

class WalManager {
    std::string wal_path_;
    FILE* wal_file_;
    std::mutex mutex_;
    uint64_t current_transaction_id_;
    int transaction_depth_ = 0;

public:
    explicit WalManager(const std::string& archive_path);
    ~WalManager();

    // Open or create the WAL file
    bool open();

    // Close the WAL file
    void close();

    // Write a record to the WAL
    bool log_write(WalRecordType type, uint64_t addr, const void* data, uint64_t size);

    // Begin a new transaction
    void begin_transaction();

    // Commit the current transaction
    bool commit_transaction();

    // Rollback transaction (decrements depth without writing COMMIT record)
    void rollback_transaction();

    // Sync the WAL to disk
    bool sync();

    // Clear the WAL (truncate) after a successful checkpoint
    bool clear();

    // Checkpoint the WAL (sync main archive, then truncate WAL)
    // Only works if transaction depth is 0.
    // returns true on success, false if busy or error.
    bool checkpoint();

    // Recover from WAL (replay records to the main archive file)
    // Returns true if recovery was successful or unnecessary (empty WAL)
    bool recover(FILE* archive_file);

    // Check if WAL exists and is not empty
    bool has_pending_recovery() const;

private:
    uint32_t calculate_checksum(const void* data, uint64_t size);
};

} // namespace compio

#endif // COMPIO_WAL_HPP_
