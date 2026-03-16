#include "compio/wal.hpp"
#include "compio/compio_file.hpp"
#include <vector>
#include <iostream>
#include <cinttypes> // for PRIu64

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#endif

// Use existing utils if possible, but FNV-1a is simple enough to include here
// Or use utils.hpp
#include "compio/utils.hpp"

namespace compio {

WalManager::WalManager(const std::string& archive_path)
    : wal_path_(archive_path + ".wal"),
      wal_file_(nullptr),
      current_transaction_id_(0) {}

WalManager::~WalManager() {
    close();
}

bool WalManager::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wal_file_) return true;

    // Open for append/update, binary
    wal_file_ = fopen(wal_path_.c_str(), "ab+");
    if (!wal_file_) {
        // Maybe try to create it? "ab+" should create if not exists.
        return false;
    }
    return true;
}

void WalManager::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wal_file_) {
        fclose(wal_file_);
        wal_file_ = nullptr;
    }
}

bool WalManager::log_write(WalRecordType type, uint64_t addr, const void* data, uint64_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wal_file_) return false;

    // Checksum
    uint32_t checksum = calculate_checksum(data, size);

    // Prepare Header
    // We serialize type as uint8_t
    uint8_t type_u8 = static_cast<uint8_t>(type);
    
    // Write Header Fields
    // Format: [Type(1)][Addr(8)][Size(8)][Checksum(4)]
    // Transaction ID? Maybe skip for now, just simple log.
    
    if (fwrite(&type_u8, sizeof(uint8_t), 1, wal_file_) != 1) return false;
    if (fwrite(&addr, sizeof(uint64_t), 1, wal_file_) != 1) return false;
    if (fwrite(&size, sizeof(uint64_t), 1, wal_file_) != 1) return false;
    if (fwrite(&checksum, sizeof(uint32_t), 1, wal_file_) != 1) return false;
    
    // Write Data
    if (size > 0) {
        if (fwrite(data, 1, size, wal_file_) != size) return false;
    }
    
    // Flush to OS buffer (not disk sync yet, caller calls sync())
    if (fflush(wal_file_) != 0) return false;

    return true;
}

void WalManager::begin_transaction() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wal_file_) return;

    if (transaction_depth_ == 0) {
        current_transaction_id_++;
    }
    transaction_depth_++;
}

bool WalManager::commit_transaction() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wal_file_) return false;

    if (transaction_depth_ == 0) return false;

    transaction_depth_--;
    if (transaction_depth_ > 0) return true; // Nested commit, defer actual commit

    // Write COMMIT record
    // Type=255, Addr=0, Size=0, Checksum=0
    uint8_t type = static_cast<uint8_t>(WalRecordType::COMMIT);
    uint64_t zero = 0;
    uint32_t zero32 = 0;
    
    if (fwrite(&type, sizeof(uint8_t), 1, wal_file_) != 1) return false;
    if (fwrite(&zero, sizeof(uint64_t), 1, wal_file_) != 1) return false; // Addr
    if (fwrite(&zero, sizeof(uint64_t), 1, wal_file_) != 1) return false; // Size
    if (fwrite(&zero32, sizeof(uint32_t), 1, wal_file_) != 1) return false; // Checksum
    
    if (fflush(wal_file_) != 0) return false;

    // Force sync for durability
#ifdef _WIN32
    int fd = _fileno(wal_file_);
    if (_commit(fd) != 0) return false;
#else
    int fd = fileno(wal_file_);
    if (fsync(fd) != 0) return false;
#endif

    return true;
}

void WalManager::rollback_transaction() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (transaction_depth_ > 0) {
        transaction_depth_--;
    }
}

bool WalManager::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wal_file_) return false;
    
    if (transaction_depth_ > 0) {
        // Defer sync until outermost commit
        return true;
    }
    
    if (fflush(wal_file_) != 0) return false;
    
    // fsync
#ifdef _WIN32
    int fd = _fileno(wal_file_);
    if (_commit(fd) != 0) return false;
#else
    int fd = fileno(wal_file_);
    if (fsync(fd) != 0) return false;
#endif
    return true;
}

bool WalManager::clear() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (wal_file_) {
        fclose(wal_file_);
        wal_file_ = nullptr;
    }
    
    // Truncate file
    wal_file_ = fopen(wal_path_.c_str(), "wb"); // 'w' truncates
    if (!wal_file_) return false;
    fclose(wal_file_);
    wal_file_ = nullptr;
    
    // Reopen in append mode
    lock.unlock();
    return open();
}

bool WalManager::has_pending_recovery() const {
    struct stat st;
    if (stat(wal_path_.c_str(), &st) == 0) {
        return st.st_size > 0;
    }
    return false;
}

bool WalManager::recover(FILE* archive_file) {
    std::unique_lock<std::mutex> lock(mutex_);
    // Close current handle if open, to read from start
    if (wal_file_) {
        fclose(wal_file_);
        wal_file_ = nullptr;
    }

    FILE* wal_in = fopen(wal_path_.c_str(), "rb");
    if (!wal_in) return true; // No WAL, nothing to recover

    // Check size
    fseek64(wal_in, 0, SEEK_END);
    int64_t size = ftell64(wal_in);
    rewind(wal_in);
    
    if (size == 0) {
        fclose(wal_in);
        return true;
    }

    // Pass 1: Scan for valid transactions
    uint64_t valid_limit = 0;
    while (true) {
        // current_pos was unused
        
        uint8_t type_u8;
        if (fread(&type_u8, sizeof(uint8_t), 1, wal_in) != 1) break;
        
        uint64_t addr;
        if (fread(&addr, sizeof(uint64_t), 1, wal_in) != 1) break;
        
        uint64_t data_size;
        if (fread(&data_size, sizeof(uint64_t), 1, wal_in) != 1) break;
        
        // Sanity check for data size to prevent OOM on corrupt WAL
        // 1GB limit seems reasonable for a single record? Or even smaller.
        // Block size is usually 4KB-64KB. Header is small.
        if (data_size > 1024 * 1024 * 1024) { // 1GB
             break;
        }

        uint32_t expected_checksum;
        if (fread(&expected_checksum, sizeof(uint32_t), 1, wal_in) != 1) break;
        
        if (data_size > 0) {
            if (fseek64(wal_in, data_size, SEEK_CUR) != 0) break;
        }
        
        // Note: In Pass 1 we strictly rely on COMMIT record to mark valid boundary.
        // But for backward compatibility (allocator records without COMMIT), we might need to be smarter.
        // However, we just updated allocator to use COMMIT.
        // What if we have old WALs? They will be ignored!
        // This is a risk. But typically WAL is empty on upgrade.
        // Let's assume we require COMMIT for durability.
        
        if (type_u8 == static_cast<uint8_t>(WalRecordType::COMMIT)) {
            // Validate COMMIT record structure: addr=0, size=0, checksum=0
            if (addr == 0 && data_size == 0 && expected_checksum == 0) {
                valid_limit = ftell64(wal_in);
            }
        }
    }
    
    // Pass 2: Replay up to valid_limit
    rewind(wal_in);
    bool success = true;
    
    // Buffer of pending records for the current transaction. These are only
    // applied to the archive when a COMMIT record is encountered.
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> pending_records;

    while (ftell64(wal_in) < valid_limit) {
        uint8_t type_u8;
        if (fread(&type_u8, sizeof(uint8_t), 1, wal_in) != 1) { success = false; break; }
        
        uint64_t addr;
        if (fread(&addr, sizeof(uint64_t), 1, wal_in) != 1) { success = false; break; }
        
        uint64_t data_size;
        if (fread(&data_size, sizeof(uint64_t), 1, wal_in) != 1) { success = false; break; }
        
        uint32_t expected_checksum;
        if (fread(&expected_checksum, sizeof(uint32_t), 1, wal_in) != 1) { success = false; break; }
        
        std::vector<uint8_t> buffer(data_size);
        if (data_size > 0) {
            if (fread(buffer.data(), 1, data_size, wal_in) != data_size) { success = false; break; }
        }
        
        // Verify checksum before doing anything with the record
        if (calculate_checksum(buffer.data(), data_size) != expected_checksum) {
            fprintf(stderr, "[WAL] Corrupt record at addr %" PRIu64 ". Stopping recovery.\n", addr);
            success = false;
            break;
        }

        // On COMMIT, apply all buffered records atomically to the archive.
        if (type_u8 == static_cast<uint8_t>(WalRecordType::COMMIT)) {
            for (const auto &rec : pending_records) {
                uint64_t rec_addr = rec.first;
                const std::vector<uint8_t> &rec_data = rec.second;
                if (fseek64(archive_file, rec_addr, SEEK_SET) != 0) {
                    fprintf(stderr, "[WAL] Failed to seek during recovery.\n");
                    success = false;
                    break;
                }
                if (!rec_data.empty() &&
                    fwrite(rec_data.data(), 1, rec_data.size(), archive_file) != rec_data.size()) {
                    fprintf(stderr, "[WAL] Failed to write recovered data to archive.\n");
                    success = false;
                    break;
                }
            }
            if (!success) {
                break;
            }
            // Successfully applied this transaction; clear buffer for next one.
            pending_records.clear();
            continue;
        }
        
        // Non-COMMIT records are buffered until we see a COMMIT.
        pending_records.emplace_back(addr, std::move(buffer));
    }
    
    fclose(wal_in);
    
    if (!success) {
        fprintf(stderr, "[WAL] Recovery failed or incomplete. WAL file preserved.\n");
        // Reopen for append? Or leave closed?
        // Open logic usually expects WAL to be ready if we return true.
        // Return false to signal failure.
        return false;
    }

    // Clear WAL after successful recovery
    // Sync archive first
    if (fflush(archive_file) != 0) return false;
#ifdef _WIN32
    int fd = _fileno(archive_file);
    if (_commit(fd) != 0) return false;
#else
    int fd = fileno(archive_file);
    if (fsync(fd) != 0) return false;
#endif

    // Now clear WAL
    FILE* wal_trunc = fopen(wal_path_.c_str(), "wb");
    if (wal_trunc) fclose(wal_trunc);

    // Reopen for append
    lock.unlock();
    return open();
}

uint32_t WalManager::calculate_checksum(const void* data, uint64_t size) {
    // FNV-1a 32-bit
    return fnv1a_32(static_cast<const uint8_t*>(data), size);
}

} // namespace compio
