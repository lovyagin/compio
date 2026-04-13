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
#include "compio.h"

namespace compio {

enum class WalRecordType : uint8_t {
    BLOCK = 1,              ///< Data block write
    INDEX_NODE = 2,         ///< B-tree index node modification
    HEADER = 3,             ///< Archive header update
    ALLOCATOR = 4,          ///< Allocator state change
    COMMIT = 255            ///< Transaction commit marker
};

/**
 * @struct WalRecordHeader
 * @brief Header for a Write-Ahead Log record
 *
 * Each record in the WAL starts with this header containing metadata
 * about the record type, location, size, and integrity check.
 */
struct WalRecordHeader {
    WalRecordType type;     ///< Type of record (BLOCK, INDEX_NODE, HEADER, ALLOCATOR, COMMIT)
    uint64_t addr;          ///< Address/offset in the main archive file where data will be written
    uint64_t size;          ///< Size of the record payload in bytes
    uint32_t checksum;      ///< CRC32 checksum of payload for integrity verification
};

/**
 * @class WalManager
 * @brief Write-Ahead Log manager for crash recovery and transaction atomicity
 *
 * Manages a Write-Ahead Log (WAL) file alongside the main archive to ensure
 * data durability and crash recovery. All modifications to the archive are
 * first logged to WAL, then applied to the main archive.
 *
 * @par Features:
 * - Crash recovery: Uncommitted transactions are rolled back
 * - Atomic transactions: Nested transaction support
 * - Auto-checkpoint: Automatic WAL truncation based on size limits
 * - Configurable sync modes: ALWAYS, NORMAL, OFF for performance tuning
 * - Vectored I/O: Efficient batching of multiple buffers
 *
 * @par Thread Safety:
 * WalManager is thread-safe for concurrent transaction operations.
 * Protected by internal mutex for all state modifications.
 *
 * @par Usage Example:
 * @code
 * WalManager wal("archive.dat");
 * wal.open();
 * wal.set_sync_mode(COMPIO_WAL_SYNC_NORMAL);
 *
 * // Transactional write
 * wal.begin_transaction();
 * wal.log_write(WalRecordType::BLOCK, addr, data, size);
 * wal.log_write(WalRecordType::INDEX_NODE, addr2, node, size2);
 * wal.commit_transaction(archive_file, max_wal_size);
 *
 * // Or use RAII guard
 * {
 *     TransactionGuard guard(&wal);
 *     wal.log_write(WalRecordType::BLOCK, addr, data, size);
 *     guard.commit(archive_file);
 * }
 * @endcode
 */
class WalManager {
    std::string wal_path_;
    FILE* wal_file_;
    std::mutex mutex_;
    uint64_t current_transaction_id_;
    int transaction_depth_ = 0;
    int batch_depth_ = 0;
    uint64_t current_wal_size_ = 0;
    compio_wal_sync_mode sync_mode_ = COMPIO_WAL_SYNC_ALWAYS;

public:
    /**
     * @brief Constructs WalManager for the given archive path
     *
     * @param[in] archive_path Path to the main archive file
     *            WAL file will be created alongside as archive_path.wal
     */
    explicit WalManager(const std::string& archive_path);

    /**
     * @brief Destructor - closes WAL file if open
     *
     * If open(), implicitly closes the WAL file.
     * In-flight transactions are not committed.
     */
    ~WalManager();

    /**
     * @brief Opens or creates the WAL file
     *
     * @return true if successfully opened/created, false on I/O error
     *
     * @note Should be called once per WalManager instance before logging
     * @note If WAL file exists, it may contain records from previous crash
     */
    bool open();

    /**
     * @brief Sets the WAL synchronization mode
     *
     * Determines how aggressively WAL is synced to disk:
     * - COMPIO_WAL_SYNC_ALWAYS: fsync on every commit (safest, slowest)
     * - COMPIO_WAL_SYNC_NORMAL: fsync only on explicit checkpoint (balanced)
     * - COMPIO_WAL_SYNC_OFF: no fsync, rely on OS buffering (fastest, risky)
     *
     * @param[in] mode New sync mode to use
     *
     * @note Can be changed at any time, affects future commits
     */
    void set_sync_mode(compio_wal_sync_mode mode);

    /**
     * @brief Closes the WAL file
     *
     * @note Safe to call even if not open()
     */
    void close();

    /**
     * @brief Writes a record to the WAL
     *
     * Logs a single modification record. The record is written to the WAL file
     * but NOT immediately synced (sync deferred until commit_transaction).
     *
     * @param[in] type Record type (BLOCK, INDEX_NODE, HEADER, ALLOCATOR)
     * @param[in] addr Archive file offset where this data will be written
     * @param[in] data Payload data to be logged. Must not be nullptr if size > 0.
     * @param[in] size Payload size in bytes
     * @return true if successfully logged, false on I/O error or transaction not active
     *
     * @note Can only be called within a transaction (after begin_transaction)
     * @note Multiple calls accumulate in the transaction
     * @note COMMIT records are written automatically, do not call with COMMIT type
     */
    bool log_write(WalRecordType type, uint64_t addr, const void* data, uint64_t size);

    struct iovec_buf {
        const void* data;   ///< Pointer to buffer
        uint64_t size;      ///< Buffer size
    };

    /**
     * @brief Writes vectored record to WAL (batching multiple buffers)
     *
     * More efficient than multiple log_write() calls - combines buffers
     * without intermediate allocation.
     *
     * @param[in] type Record type
     * @param[in] addr Archive file offset
     * @param[in] buffers Vector of (data, size) pairs to write sequentially
     * @return true if successfully logged, false on I/O error
     *
     * @par Example:
     * @code
     * std::vector<WalManager::iovec_buf> bufs = {
     *     {data1, size1},
     *     {data2, size2},
     *     {data3, size3}
     * };
     * wal.log_write_vectored(WalRecordType::INDEX_NODE, addr, bufs);
     * @endcode
     */
    bool log_write_vectored(WalRecordType type, uint64_t addr, const std::vector<iovec_buf>& buffers);

    /**
     * @brief Begins a new transaction (supports nesting)
     *
     * Can be called multiple times - nesting is supported.
     * Requires matching number of commit_transaction() calls to finalize.
     *
     * @note Thread-safe: multiple threads can have independent transactions
     */
    void begin_transaction();

    /**
     * @brief Commits the current transaction
     *
     * Decrements transaction nesting depth. When reaching 0, writes COMMIT record
     * and syncs WAL per configured sync_mode.
     *
     * @param[in] archive_file Optional archive FILE* for auto-checkpoint
     * @param[in] max_wal_size Max WAL size threshold (0 = no auto-checkpoint)
     * @return true if successfully committed, false on I/O error
     *
     * @note Uses sync_mode set by set_sync_mode()
     * @note If WAL size exceeds max_wal_size, automatically checkpoints
     *
     * @see commit_transaction_explicit()
     */
    bool commit_transaction(FILE* archive_file = nullptr, uint64_t max_wal_size = 0);

    /**
     * @brief Commits with explicit sync mode override
     *
     * @param[in] sync_mode Override sync mode for this commit only
     * @param[in] archive_file Optional archive FILE* for auto-checkpoint
     * @param[in] max_wal_size Max WAL size threshold (0 = no auto-checkpoint)
     * @return true if successfully committed, false on I/O error
     *
     * @see commit_transaction()
     */
    bool commit_transaction_explicit(compio_wal_sync_mode sync_mode, FILE* archive_file = nullptr, uint64_t max_wal_size = 0);

    /**
     * @brief Rolls back current transaction without committing
     *
     * Decrements transaction nesting depth without writing COMMIT record.
     * All logged records in this transaction are discarded on next recovery.
     *
     * @note Buffered records are not immediately removed from WAL file
     * @note Useful for error handling: construct with TransactionGuard for RAII
     */
    void rollback_transaction();

    /**
     * @brief Syncs WAL to disk
     *
     * Forces fsync of the WAL file to ensure durability.
     *
     * @return true if sync successful, false on I/O error
     *
     * @note Usually not needed - sync is automatic on commit_transaction
     */
    bool sync();

    /**
     * @brief Clears (truncates) the WAL file
     *
     * Removes all records. Called internally after checkpoint.
     *
     * @return true if truncated successfully, false on error
     */
    bool clear();

    /**
     * @brief Checkpoints the WAL (sync archive, then truncate WAL)
     *
     * Assumes the main archive has been fully synced. Truncates WAL safely.
     *
     * @return true if checkpoint successful, false if transaction in progress or error
     *
     * @pre Transaction depth must be 0 (no active transactions)
     * @pre Main archive file must be fsync'd before calling this
     *
     * @note Called automatically by commit_transaction() when WAL size exceeds limit
     */
    bool checkpoint();

    /**
     * @brief Begins a batch operation (grouped transactions)
     *
     * Use with end_batch() for efficient bulk operations.
     * Multiple log_write() calls are accumulated before sync.
     *
     * @see end_batch()
     */
    void begin_batch();

    /**
     * @brief Ends batch and optionally triggers checkpoint
     *
     * @param[in] archive_file Optional archive FILE* for auto-checkpoint
     * @param[in] max_wal_size Max WAL size threshold
     * @return true if successful, false on error
     *
     * @see begin_batch()
     */
    bool end_batch(FILE* archive_file = nullptr, uint64_t max_wal_size = 0);
    
    /**
     * @brief Gets current batch nesting depth
     * @return Current batch depth (0 if not in batch)
     */
    int get_batch_depth() {
        std::lock_guard<std::mutex> lock(mutex_);
        return batch_depth_;
    }

    /**
     * @brief Recovers from WAL after crash
     *
     * Replays all committed transactions from WAL to the main archive file.
     * Should be called once at archive open time if WAL exists.
     *
     * @param[in] archive_file Opened archive file to replay WAL into
     * @return true if recovery successful or WAL empty, false on fatal error
     *
     * @note Uncommitted transactions are discarded
     * @note Integrity checked via record checksums
     * @note Idempotent: can be called multiple times safely
     *
     * @see has_pending_recovery()
     */
    bool recover(FILE* archive_file);

    /**
     * @brief Checks if WAL has pending recovery work
     *
     * @return true if WAL file exists and contains records needing replay
     *
     * @note Should be checked after open() to decide whether to call recover()
     */
    bool has_pending_recovery() const;

private:
    uint32_t calculate_checksum(const void* data, uint64_t size);
    
    // Internal implementations without mutex locking
    void begin_transaction_impl();
    bool commit_transaction_explicit_impl(compio_wal_sync_mode sync_mode, FILE* archive_file, uint64_t max_wal_size);
};

/**
 * @class TransactionGuard
 * @brief RAII guard for automatic transaction management
 *
 * Simplifies transaction handling by automatically calling begin_transaction()
 * on construction and commit_transaction() or rollback_transaction() on destruction.
 *
 * @par Usage:
 * @code
 * {
 *     TransactionGuard guard(&wal_manager);
 *     wal_manager.log_write(WalRecordType::BLOCK, addr, data, size);
 *     wal_manager.log_write(WalRecordType::INDEX_NODE, addr2, node, size2);
 *     guard.commit(archive_file);
 *     // On scope exit: if commit() was called, nothing happens
 *     // On scope exit: if commit() not called, rollback_transaction() is invoked
 * }
 * @endcode
 *
 * @note Non-copyable and non-movable for safety
 * @note Automatically rolls back if commit() not called before destruction
 */
class TransactionGuard {
    WalManager* wal_;
    bool committed_;

public:
    /**
     * @brief Constructs guard and begins transaction
     *
     * @param[in] wal Pointer to WalManager. Can be nullptr (safe no-op).
     */
    explicit TransactionGuard(WalManager* wal) : wal_(wal), committed_(false) {
        if (wal_) {
            wal_->begin_transaction();
        }
    }

    /**
     * @brief Destructor - rolls back if not committed
     *
     * If commit() or commit_explicit() was not called, calls rollback_transaction().
     */
    ~TransactionGuard() {
        if (wal_ && !committed_) {
            wal_->rollback_transaction();
        }
    }

    // Disable copy/move to keep it simple
    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;

    /**
     * @brief Commits transaction using WalManager's configured sync mode
     *
     * @param[in] archive_file Optional archive FILE* for auto-checkpoint
     * @param[in] max_wal_size Max WAL size threshold (0 = no auto-checkpoint)
     * @return true if committed successfully, false on error
     *
     * @note Calling commit() multiple times is idempotent (returns true on 2nd+ call)
     */
    bool commit(FILE* archive_file = nullptr, uint64_t max_wal_size = 0) {
        if (!wal_) return false;
        if (committed_) return true;
        
        bool result = wal_->commit_transaction(archive_file, max_wal_size);
        committed_ = true;
        return result;
    }

    /**
     * @brief Commits with explicit sync mode override
     *
     * @param[in] sync_mode Override sync mode for this commit
     * @param[in] archive_file Optional archive FILE* for auto-checkpoint
     * @param[in] max_wal_size Max WAL size threshold
     * @return true if committed successfully, false on error
     *
     * @see commit()
     */
    bool commit_explicit(compio_wal_sync_mode sync_mode, FILE* archive_file = nullptr, uint64_t max_wal_size = 0) {
        if (!wal_) return false;
        if (committed_) return true;
        
        bool result = wal_->commit_transaction_explicit(sync_mode, archive_file, max_wal_size);
        committed_ = true;
        return result;
    }
};

} // namespace compio

#endif // COMPIO_WAL_HPP_
