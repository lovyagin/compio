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
    uint64_t current_wal_size_ = 0;

public:
    explicit WalManager(const std::string& archive_path);
    ~WalManager();

    // Open or create the WAL file
    bool open();

    // Close the WAL file
    void close();

    // Write a record to the WAL
    bool log_write(WalRecordType type, uint64_t addr, const void* data, uint64_t size);

    struct iovec_buf {
        const void* data;
        uint64_t size;
    };
    // Write a vectored record to the WAL (prevents allocation for combining buffers)
    bool log_write_vectored(WalRecordType type, uint64_t addr, const std::vector<iovec_buf>& buffers);

    // Begin a new transaction
    void begin_transaction();

    // Commit the current transaction
    // Optional: provide archive_file and max_wal_size to trigger auto-checkpoint
    bool commit_transaction(FILE* archive_file = nullptr, uint64_t max_wal_size = 0);

    // Rollback transaction (decrements depth without writing COMMIT record)
    void rollback_transaction();

    // Sync the WAL to disk
    bool sync();

    // Clear the WAL (truncate) after a successful checkpoint
    bool clear();

    // Checkpoint the WAL (truncate WAL after archive sync)
    // Only works if transaction depth is 0.
    // NOTE: The caller MUST ensure the main archive file is fully synced (fsync/flush)
    // BEFORE calling this method. This method only truncates the WAL.
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

// RAII Guard for WAL Transactions
class TransactionGuard {
    WalManager* wal_;
    bool committed_;

public:
    explicit TransactionGuard(WalManager* wal) : wal_(wal), committed_(false) {
        if (wal_) {
            wal_->begin_transaction();
        }
    }

    ~TransactionGuard() {
        if (wal_ && !committed_) {
            wal_->rollback_transaction();
        }
    }

    // Disable copy/move to keep it simple
    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;

    bool commit(FILE* archive_file = nullptr, uint64_t max_wal_size = 0) {
        if (!wal_) return false;
        if (committed_) return true; // Already committed/attempted
        
        // Mark as committed before calling, or assume commit_transaction 
        // handles its own state. 
        // Current implementation of commit_transaction decrements depth unconditionally.
        // So we must mark as committed regardless of result to avoid double decrement 
        // (one in commit_transaction, one in ~TransactionGuard via rollback).
        bool result = wal_->commit_transaction(archive_file, max_wal_size);
        committed_ = true;
        return result;
    }
};

} // namespace compio

#endif // COMPIO_WAL_HPP_
