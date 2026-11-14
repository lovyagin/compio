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

// Record: [type u8][epoch u64][addr u64][size u64][checksum u32][payload]. The
// checksum covers the 25 header bytes before it and the payload, so a torn or
// bit-rotted header can never redirect a valid payload to another address.
static constexpr size_t kRecordFieldsBytes = 1 + 8 + 8 + 8;
static constexpr uint64_t kRecordHeaderBytes = kRecordFieldsBytes + 4;
static constexpr uint64_t kMaxRecordPayload = 64ULL * 1024 * 1024;

static void put_u64_le(uint8_t* out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(v >> (8 * i));
}

static uint64_t get_u64_le(const uint8_t* in) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(in[i]) << (8 * i);
    return v;
}

static void encode_record_fields(uint8_t* out, WalRecordType type, uint64_t epoch, uint64_t addr, uint64_t size) {
    out[0] = static_cast<uint8_t>(type);
    put_u64_le(out + 1, epoch);
    put_u64_le(out + 9, addr);
    put_u64_le(out + 17, size);
}

namespace {

struct ParsedRecord {
    uint8_t type = 0;
    uint64_t epoch = 0;
    uint64_t addr = 0;
    std::vector<uint8_t> payload;
};

// Reads the record at the current position. False when it is truncated, claims
// more than the file holds, or fails its checksum.
bool read_record(FILE* f, int64_t file_size, ParsedRecord* rec) {
    uint8_t fields[kRecordFieldsBytes];
    if (fread(fields, 1, sizeof(fields), f) != sizeof(fields)) return false;
    uint32_t expected;
    if (!read_u32_le(f, &expected)) return false;

    const uint64_t size = get_u64_le(fields + 17);
    const int64_t pos = ftell64(f);
    if (pos < 0 || size > static_cast<uint64_t>(file_size - pos) || size > kMaxRecordPayload) return false;

    rec->payload.assign(size, 0);
    if (size > 0 && fread(rec->payload.data(), 1, size, f) != size) return false;

    uint32_t computed = fnv1a_32(fields, sizeof(fields));
    if (size > 0) computed = fnv1a_32_continue(computed, rec->payload.data(), size);
    if (computed != expected) return false;

    rec->type = fields[0];
    rec->epoch = get_u64_le(fields + 1);
    rec->addr = get_u64_le(fields + 9);
    return true;
}

}  // namespace

bool WalManager::sync_wal() {
    if (failed_) return false;
    if (fflush(wal_file_) != 0) return false;
    if (!compio::sync_file(wal_file_)) {
        failed_ = true;
        return false;
    }
    return true;
}

bool WalManager::write_record(WalRecordType type, uint64_t addr, const iovec_buf* buffers, size_t count) {
    if (failed_) return false;
    uint64_t size = 0;
    for (size_t i = 0; i < count; ++i) size += buffers[i].size;

    uint8_t fields[kRecordFieldsBytes];
    encode_record_fields(fields, type, epoch_, addr, size);
    uint32_t checksum = fnv1a_32(fields, sizeof(fields));
    for (size_t i = 0; i < count; ++i) {
        if (buffers[i].size > 0) {
            checksum = fnv1a_32_continue(checksum, static_cast<const uint8_t*>(buffers[i].data), buffers[i].size);
        }
    }

    if (fwrite(fields, 1, sizeof(fields), wal_file_) != sizeof(fields)) return false;
    if (!write_u32_le(wal_file_, checksum)) return false;
    for (size_t i = 0; i < count; ++i) {
        if (buffers[i].size > 0 && fwrite(buffers[i].data, 1, buffers[i].size, wal_file_) != buffers[i].size) {
            return false;
        }
    }
    current_wal_size_ += kRecordHeaderBytes + size;
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

    struct stat existing;
    const bool existed = stat(wal_path_.c_str(), &existing) == 0;

    // Open for append/update, binary
    wal_file_ = fopen(wal_path_.c_str(), "ab+");
    if (!wal_file_) {
        // Maybe try to create it? "ab+" should create if not exists.
        return false;
    }
    if (!existed && !compio::sync_parent_directory(wal_path_)) {
        fclose(wal_file_);
        wal_file_ = nullptr;
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
    const iovec_buf buffer{data, size};
    // Flush to the OS is deferred until commit or sync: batched writes stay cheap.
    return write_record(type, addr, &buffer, 1);
}

bool WalManager::log_write_vectored(WalRecordType type, uint64_t addr, const std::vector<iovec_buf>& buffers) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wal_file_) return false;
    return write_record(type, addr, buffers.data(), buffers.size());
}

void WalManager::begin_transaction() {
    std::unique_lock<std::mutex> lock(mutex_);
    begin_transaction_impl();
}

void WalManager::begin_transaction_impl() {
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

void WalManager::set_sync_mode(compio_wal_sync_mode mode) {
    std::unique_lock<std::mutex> lock(mutex_);
    sync_mode_ = mode;
}

bool WalManager::commit_transaction() {
    std::unique_lock<std::mutex> lock(mutex_);
    return commit_transaction_impl(sync_mode_);
}

bool WalManager::commit_transaction_impl(compio_wal_sync_mode sync_mode) {
    if (transaction_depth_ > 0) {
        transaction_depth_--;
    } else {
        return false;
    }

    if (!wal_file_) return true; // No-op success for read-only scenario

    if (transaction_depth_ > 0) return true; // Nested commit, defer actual commit

    if (!write_record(WalRecordType::COMMIT, 0, nullptr, 0)) return false;

    // Based on sync_mode, decide whether to fsync, fflush, or nothing
    if (sync_mode == COMPIO_WAL_SYNC_ALWAYS) {
        if (!sync_wal()) return false;
    } else if (sync_mode != COMPIO_WAL_SYNC_OFF) {
        if (fflush(wal_file_) != 0) return false;
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
    
    return sync_wal();
}

bool WalManager::checkpoint() {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // Can only checkpoint if no active transaction
    if (transaction_depth_ > 0) return false;
    
    if (!wal_file_) return true; // Nothing to truncate
    if (failed_) return false;   // The journal may hold the only copy of lost pages

    if (fflush(wal_file_) != 0) return false;

#ifdef _WIN32
    int fd = _fileno(wal_file_);
    if (_chsize_s(fd, 0) != 0) return false;
    // Seek to beginning for subsequent writes
    if (_lseek(fd, 0, SEEK_SET) == -1) return false;
#else
    int fd = fileno(wal_file_);
    if (ftruncate(fd, 0) != 0) return false;
    // Seek to beginning for subsequent writes
    if (lseek(fd, 0, SEEK_SET) < 0) return false;
#endif
    if (!sync_wal()) return false;

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

bool WalManager::recover(FILE* archive_file, uint64_t epoch, std::vector<uint8_t>* meta) {
    std::unique_lock<std::mutex> lock(mutex_);
    // Close current handle if open, to read from start
    if (wal_file_) {
        fclose(wal_file_);
        wal_file_ = nullptr;
    }

    FILE* wal_in = fopen(wal_path_.c_str(), "rb");
    if (!wal_in) return true; // No WAL, nothing to recover

    fseek64(wal_in, 0, SEEK_END);
    const int64_t size = ftell64(wal_in);
    rewind(wal_in);

    if (size == 0) {
        fclose(wal_in);
        return true;
    }

    // Pass 1: the committed prefix is every record up to the last COMMIT, as long
    // as each record is intact and extends the header of the given epoch. A torn
    // tail, a corrupt record or a journal left over from an older publication ends
    // the scan; what follows is never replayed.
    uint64_t valid_limit = 0;
    ParsedRecord rec;
    while (epoch != genesis_epoch && read_record(wal_in, size, &rec) && rec.epoch == epoch) {
        if (rec.type == static_cast<uint8_t>(WalRecordType::COMMIT)) {
            if (rec.addr != 0 || !rec.payload.empty()) break;
            valid_limit = ftell64(wal_in);
        }
    }

    // Pass 2: Replay up to valid_limit
    rewind(wal_in);
    bool success = true;

    // Buffer of pending records for the current transaction. These are only
    // applied to the archive when a COMMIT record is encountered.
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> pending_records;
    std::vector<uint8_t> pending_meta;
    bool has_pending_meta = false;
    bool has_meta = false;

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
        if (!read_record(wal_in, size, &rec)) {
            fprintf(stderr, "[WAL] Record changed between recovery passes at offset %" PRId64 ".\n", pos);
            success = false;
            break;
        }

        // On COMMIT, apply all buffered records atomically to the archive.
        if (rec.type == static_cast<uint8_t>(WalRecordType::COMMIT)) {
            for (const auto &pending : pending_records) {
                const std::vector<uint8_t> &rec_data = pending.second;
                if (fseek64(archive_file, pending.first, SEEK_SET) != 0) {
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
            if (has_pending_meta && meta) {
                *meta = std::move(pending_meta);
                has_meta = true;
            }
            has_pending_meta = false;
            continue;
        }

        // Non-COMMIT records are buffered until we see a COMMIT.
        if (rec.type == static_cast<uint8_t>(WalRecordType::META)) {
            pending_meta = std::move(rec.payload);
            has_pending_meta = true;
            continue;
        }
        pending_records.emplace_back(rec.addr, std::move(rec.payload));
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
    if (!compio::sync_file(archive_file)) return false;

    // A META record is applied by the caller, which truncates the WAL once the
    // state it describes is published.
    if (has_meta) {
        lock.unlock();
        return open();
    }

    // Now clear WAL (durable truncation)
    FILE* wal_trunc = fopen(wal_path_.c_str(), "wb");
    if (wal_trunc) {
        compio::sync_file(wal_trunc);
        fclose(wal_trunc);
    }

    // Reopen for append
    lock.unlock();
    return open();
}

void WalManager::set_epoch(uint64_t epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    epoch_ = epoch;
}

void WalManager::begin_batch() {
    std::lock_guard<std::mutex> lock(mutex_);
    batch_depth_++;
}

bool WalManager::discard_batch() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch_depth_ = 0;
        transaction_depth_ = 0;
    }
    return checkpoint();
}

bool WalManager::end_batch() {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (batch_depth_ <= 0) {
        return false;
    }
    
    batch_depth_--;
    
    if (batch_depth_ > 0) {
        return true;
    }
    
    if (failed_) return false;
    if (!wal_file_) return true;
    
    if (transaction_depth_ != 0) {
        return true;
    }
    
    compio_wal_sync_mode mode = sync_mode_;
    
    // Call internal version without lock to avoid deadlock
    begin_transaction_impl();
    return commit_transaction_impl(mode);
}
} // namespace compio
