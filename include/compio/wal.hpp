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
    uint64_t transaction_id;
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

public:
    explicit WalManager(const std::string& archive_path);
    ~WalManager();

    // Open or create the WAL file
    bool open();

    // Close the WAL file
    void close();

    // Write a record to the WAL
    bool log_write(WalRecordType type, uint64_t addr, const void* data, uint64_t size);

    // Sync the WAL to disk
    bool sync();

    // Clear the WAL (truncate) after a successful checkpoint
    bool clear();

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
