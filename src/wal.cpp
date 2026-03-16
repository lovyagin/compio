#include "compio/wal.hpp"
#include <unistd.h>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <vector>
#include <iostream>

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
    return true;
}

bool WalManager::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wal_file_) return false;
    
    if (fflush(wal_file_) != 0) return false;
    
    // fsync
    int fd = fileno(wal_file_);
#ifdef _WIN32
    // Windows equivalent? _commit(fd)
    // For now assume POSIX or standard
#else
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
    fseek(wal_in, 0, SEEK_END);
    long size = ftell(wal_in);
    rewind(wal_in);
    
    if (size == 0) {
        fclose(wal_in);
        return true;
    }

    while (true) {
        uint8_t type_u8;
        if (fread(&type_u8, sizeof(uint8_t), 1, wal_in) != 1) break; // EOF
        
        uint64_t addr;
        if (fread(&addr, sizeof(uint64_t), 1, wal_in) != 1) break; // Partial record
        
        uint64_t data_size;
        if (fread(&data_size, sizeof(uint64_t), 1, wal_in) != 1) break;
        
        uint32_t expected_checksum;
        if (fread(&expected_checksum, sizeof(uint32_t), 1, wal_in) != 1) break;
        
        std::vector<uint8_t> buffer(data_size);
        if (data_size > 0) {
            if (fread(buffer.data(), 1, data_size, wal_in) != data_size) break;
        }
        
        // Verify checksum
        if (calculate_checksum(buffer.data(), data_size) != expected_checksum) {
            fprintf(stderr, "[WAL] Corrupt record at addr %lu. Stopping recovery.\n", addr);
            break; // Stop or fail? Stop prevents writing bad data.
        }
        
        // Apply to archive
        fseek(archive_file, addr, SEEK_SET);
        if (fwrite(buffer.data(), 1, data_size, archive_file) != data_size) {
            fprintf(stderr, "[WAL] Failed to write recovered data to archive.\n");
            fclose(wal_in);
            return false;
        }
    }
    
    fclose(wal_in);
    
    // Clear WAL after successful recovery?
    // Usually yes, but only if we synced archive_file.
    fflush(archive_file);
    int fd = fileno(archive_file);
#ifndef _WIN32
    fsync(fd);
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
