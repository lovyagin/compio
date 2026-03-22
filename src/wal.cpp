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
#include <sys/types.h>
#endif

// Use existing utils if possible, but FNV-1a is simple enough to include here
// Or use utils.hpp
#include "compio/utils.hpp"

namespace compio {

// Helper for Little-Endian I/O
static bool write_u64_le(FILE* f, uint64_t v) {
    if (is_big_endian()) swap_uint64(&v);
    return fwrite(&v, sizeof(v), 1, f) == 1;
}

static bool write_u32_le(FILE* f, uint32_t v) {
    if (is_big_endian()) swap_uint32(&v);
    return fwrite(&v, sizeof(v), 1, f) == 1;
}

static bool read_u64_le(FILE* f, uint64_t* v) {
    if (fread(v, sizeof(*v), 1, f) != 1) return false;
    if (is_big_endian()) swap_uint64(v);
    return true;
}

static bool read_u32_le(FILE* f, uint32_t* v) {
    if (fread(v, sizeof(*v), 1, f) != 1) return false;
    if (is_big_endian()) swap_uint32(v);
    return true;
}

WalManager::WalManager(const std::string& archive_path)
    : wal_path_(archive_path + ".wal"),
      wal_file_(nullptr),
      current_transaction_id_(0),
      current_wal_size_(0) {}

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
    
    // Initialize current WAL size
    if (fseek64(wal_file_, 0, SEEK_END) != 0) {
        fclose(wal_file_);
        wal_file_ = nullptr;
        return false;
    }
    int64_t size = ftell64(wal_file_);
    if (size < 0) {
        fclose(wal_file_);
        wal_file_ = nullptr;
        return false;
    }
    current_wal_size_ = static_cast<uint64_t>(size);
    
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

    // Checksum: always use the same convention as recovery/commit logic
    uint32_t checksum = calculate_checksum(data, size);

    // Prepare Header
    // We serialize type as uint8_t
    uint8_t type_u8 = static_cast<uint8_t>(type);
    
    // Write Header Fields
    // Format: [Type(1)][Addr(8)][Size(8)][Checksum(4)]
    // Transaction ID? Maybe skip for now, just simple log.
    
    if (fwrite(&type_u8, sizeof(uint8_t), 1, wal_file_) != 1) return false;
    if (!write_u64_le(wal_file_, addr)) return false;
    if (!write_u64_le(wal_file_, size)) return false;
    if (!write_u32_le(wal_file_, checksum)) return false;
    
    // Write Data
    if (size > 0) {
        if (fwrite(data, 1, size, wal_file_) != size) return false;
    }
    
    // Update size tracker
    // Record size = 1 (type) + 8 (addr) + 8 (size) + 4 (checksum) + data_size
    current_wal_size_ += (1 + 8 + 8 + 4 + size);

    // Flush to OS buffer is DEFERRED until commit or sync
    // This improves performance for batched writes.
    // if (fflush(wal_file_) != 0) return false;

    return true;
}

bool WalManager::log_write_vectored(WalRecordType type, uint64_t addr, const std::vector<iovec_buf>& buffers) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wal_file_) return false;

    uint64_t total_size = 0;
    for (const auto& buf : buffers) {
        total_size += buf.size;
    }

    // Checksum: initialize using the same zero-length convention as calculate_checksum(nullptr, 0)
    uint32_t checksum = calculate_checksum(nullptr, 0);
    for (const auto& buf : buffers) {
        if (buf.size > 0) {
            checksum = fnv1a_32_continue(
                checksum,
                static_cast<const uint8_t*>(buf.data),
                buf.size
            );
        }
    }

    // Prepare Header
    uint8_t type_u8 = static_cast<uint8_t>(type);
    
    if (fwrite(&type_u8, sizeof(uint8_t), 1, wal_file_) != 1) return false;
    if (!write_u64_le(wal_file_, addr)) return false;
    if (!write_u64_le(wal_file_, total_size)) return false;
    if (!write_u32_le(wal_file_, checksum)) return false;
    
    // Write Data
    for (const auto& buf : buffers) {
        if (buf.size > 0) {
            if (fwrite(buf.data, 1, buf.size, wal_file_) != buf.size) return false;
        }
    }
    
    // Update size tracker
    current_wal_size_ += (1 + 8 + 8 + 4 + total_size);

    return true;
}

void WalManager::begin_transaction() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!wal_file_) {
        // We allow begin_transaction on unopened WAL to support read-only archives 
        // that might call it via guards but never write.
        // But if we intend to write, log_write will fail.
        // We should track depth anyway to balance with commit/rollback.
        transaction_depth_++;
        return;
    }

    if (transaction_depth_ == 0) {
        current_transaction_id_++;
    }
    transaction_depth_++;
}

bool WalManager::commit_transaction(FILE* archive_file, uint64_t max_wal_size, compio_wal_sync_mode sync_mode) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (transaction_depth_ > 0) {
        transaction_depth_--;
    } else {
        return false;
    }

    if (!wal_file_) return true; // No-op success for read-only scenario

    if (transaction_depth_ > 0) return true; // Nested commit, defer actual commit

    // Write COMMIT record
    // Type=255, Addr=0, Size=0, Checksum=FNV1a(empty)
    uint8_t type = static_cast<uint8_t>(WalRecordType::COMMIT);
    uint64_t zero = 0;
    
    // Compute checksum of empty data for consistency
    uint32_t checksum = calculate_checksum(nullptr, 0);
    
    if (fwrite(&type, sizeof(uint8_t), 1, wal_file_) != 1) return false;
    if (!write_u64_le(wal_file_, zero)) return false; // Addr
    if (!write_u64_le(wal_file_, zero)) return false; // Size
    if (!write_u32_le(wal_file_, checksum)) return false; // Checksum
    
    // Update size for COMMIT record (1+8+8+4 = 21 bytes)
    current_wal_size_ += 21;

    // Based on sync_mode, decide whether to fsync, fflush, or nothing
    if (sync_mode != COMPIO_WAL_SYNC_OFF) {
        if (fflush(wal_file_) != 0) return false;
    }

    if (sync_mode == COMPIO_WAL_SYNC_ALWAYS) {
        // Force sync for durability
#ifdef _WIN32
        int fd = _fileno(wal_file_);
        if (_commit(fd) != 0) return false;
#else
        int fd = fileno(wal_file_);
        if (fsync(fd) != 0) return false;
#endif
    }

    // Auto-Checkpoint if size exceeds limit and archive_file is provided
    if (archive_file && max_wal_size > 0 && current_wal_size_ >= max_wal_size) {
        // Must release lock for a moment? No, checkpoint takes lock.
        // Wait, checkpoint takes unique_lock. But we already hold lock.
        // We need an internal checkpoint function or recursive mutex?
        // Or just implement logic here.
        // Checkpoint logic: Sync Archive -> Flush WAL -> Truncate WAL -> Seek 0 -> Sync WAL.
        
        // 1. Sync Archive
        if (fflush(archive_file) != 0) return false;
#ifdef _WIN32
        int arch_fd = _fileno(archive_file);
        if (_commit(arch_fd) != 0) return false;
#else
        int arch_fd = fileno(archive_file);
        if (fsync(arch_fd) != 0) return false;
#endif

        // 2. Truncate WAL (reuse checkpoint logic but without locking again)
        // We can extract checkpoint logic to a private method `checkpoint_locked`.
        // Or just copy it here since it is short.
        
        if (fflush(wal_file_) != 0) return false;

#ifdef _WIN32
        int fd = _fileno(wal_file_);
        if (_chsize_s(fd, 0) != 0) return false;
        if (_lseek(fd, 0, SEEK_SET) == -1) return false;
        if (_commit(fd) != 0) return false;
#else
        int fd = fileno(wal_file_);
        if (ftruncate(fd, 0) != 0) return false;
        if (lseek(fd, 0, SEEK_SET) < 0) return false;
        if (fsync(fd) != 0) return false;
#endif
        rewind(wal_file_);
        current_wal_size_ = 0;
    }

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

bool WalManager::checkpoint() {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // Can only checkpoint if no active transaction
    if (transaction_depth_ > 0) return false;
    
    if (!wal_file_) return true; // Nothing to truncate

    if (fflush(wal_file_) != 0) return false;

#ifdef _WIN32
    int fd = _fileno(wal_file_);
    if (_chsize_s(fd, 0) != 0) return false;
    // Seek to beginning for subsequent writes
    if (_lseek(fd, 0, SEEK_SET) == -1) return false;
    if (_commit(fd) != 0) return false;
#else
    int fd = fileno(wal_file_);
    if (ftruncate(fd, 0) != 0) return false;
    // Seek to beginning for subsequent writes
    if (lseek(fd, 0, SEEK_SET) < 0) return false;
    if (fsync(fd) != 0) return false;
#endif

    // Update stdio buffer position as well
    rewind(wal_file_);
    current_wal_size_ = 0;
    
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
        if (!read_u64_le(wal_in, &addr)) break;
        
        uint64_t data_size;
        if (!read_u64_le(wal_in, &data_size)) break;
        
        // Sanity check for data size to prevent OOM on corrupt WAL
        // 1GB limit seems reasonable for a single record? Or even smaller.
        // Block size is usually 4KB-64KB. Header is small.
        int64_t current_pos = ftell64(wal_in);
        int64_t remaining = size - current_pos;
        if (data_size > static_cast<uint64_t>(remaining)) {
            // Record claims to be larger than remaining file size -> corrupt
            break;
        }
        if (data_size > 64 * 1024 * 1024) { // 64MB hard limit for sanity
             break;
        }

        uint32_t expected_checksum;
        if (!read_u32_le(wal_in, &expected_checksum)) break;
        
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
            // Validate COMMIT record structure: addr=0, size=0, checksum=valid
            // And ensure we have seen valid records leading up to this commit.
            // Since we don't track transaction boundaries in Pass 1, we just check
            // that the COMMIT record itself is structurally valid.
            uint32_t valid_checksum = calculate_checksum(nullptr, 0);
            if (addr == 0 && data_size == 0 && expected_checksum == valid_checksum) {
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

    while (true) {
        int64_t pos = ftell64(wal_in);
        if (pos < 0) {
            fprintf(stderr, "[WAL] ftell64 failed during recovery.\n");
            success = false;
            break;
        }
        if (static_cast<uint64_t>(pos) >= valid_limit) {
            break;
        }
        uint8_t type_u8;
        if (fread(&type_u8, sizeof(uint8_t), 1, wal_in) != 1) { success = false; break; }
        
        uint64_t addr;
        if (!read_u64_le(wal_in, &addr)) { success = false; break; }
        
        uint64_t data_size;
        if (!read_u64_le(wal_in, &data_size)) { success = false; break; }
        
        uint32_t expected_checksum;
        if (!read_u32_le(wal_in, &expected_checksum)) { success = false; break; }
        
        std::vector<uint8_t> buffer(data_size);
        if (data_size > 0) {
            if (fread(buffer.data(), 1, data_size, wal_in) != data_size) { success = false; break; }
        }
        
        // Verify checksum before doing anything with the record
        uint32_t computed_checksum = calculate_checksum(buffer.data(), data_size);
            
        if (computed_checksum != expected_checksum) {
            fprintf(stderr, "[WAL] Corrupt record at addr %" PRIu64 ". Type=%u, Size=%" PRIu64 ", Expected Checksum=%u, Computed Checksum=%u\n",
                    addr, type_u8, data_size, expected_checksum, computed_checksum);
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

    // Now clear WAL (durable truncation)
    FILE* wal_trunc = fopen(wal_path_.c_str(), "wb");
    if (wal_trunc) {
        if (fflush(wal_trunc) == 0) {
#ifdef _WIN32
            _commit(_fileno(wal_trunc));
#else
            fsync(fileno(wal_trunc));
#endif
        }
        fclose(wal_trunc);
    }

    // Reopen for append
    lock.unlock();
    return open();
}

uint32_t WalManager::calculate_checksum(const void* data, uint64_t size) {
    // FNV-1a 32-bit
    return fnv1a_32(static_cast<const uint8_t*>(data), size);
}

} // namespace compio
