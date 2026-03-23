#include "compio/file.hpp"
#include "compio/wal.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <inttypes.h>
#include <vector>

#include "compio/debug_print.hpp"
#include "compio/utils.hpp"
#include "compio.h"

using namespace compio;

#define lendian_fread_member(memb, file) lendian_fread(&(memb), sizeof(memb), 1, (file))
#define lendian_fwrite_member(memb, file) lendian_fwrite(&(memb), sizeof(memb), 1, (file))

static size_t portable_strnlen(const char *s, size_t maxlen) {
    const char *end = (const char *)memchr(s, '\0', maxlen);
    return end ? (size_t)(end - s) : maxlen;
}

static const uint8_t index_node_signature = 67;

bool files_table::read_from(FILE *file, uint64_t addr, uint32_t capacity, uint64_t n_files_in) {
    if (fseek64(file, addr, SEEK_SET) != 0) {
        WARNING_PRINT("warning: fseek failed in files_table::read_from\n");
        return false;
    }

    this->max_files = capacity;
    this->n_files = n_files_in;
    this->files.resize(capacity);

    const size_t ENTRY_SIZE = COMPIO_FNAME_MAX_SIZE + sizeof(uint64_t);
    static_assert(sizeof(files_table::file) == ENTRY_SIZE, "struct file must be packed");

    bool is_be = is_big_endian();

    // Optimization: Bulk read if Little Endian
    if (!is_be) {
        if (fread(this->files.data(), ENTRY_SIZE, capacity, file) != capacity) {
            WARNING_PRINT("warning: short read in files table (bulk)\n");
            return false;
        }
        rebuild_index();
        return true;
    }

    const size_t BATCH_SIZE = 4096;
    std::vector<uint8_t> buffer(BATCH_SIZE * ENTRY_SIZE);

    size_t remaining = capacity; // We read capacity slots
    size_t current_idx = 0;

    while (remaining > 0) {
        size_t count = std::min(remaining, BATCH_SIZE);
        size_t bytes_to_read = count * ENTRY_SIZE;

        size_t read_count = lendian_fread(buffer.data(), 1, bytes_to_read, file);
        if (read_count != bytes_to_read) {
            WARNING_PRINT("warning: short read in files table\n");
            return false;
        }

        for (size_t i = 0; i < count; ++i) {
            files_table::file& f = this->files[current_idx + i];
            size_t offset = i * ENTRY_SIZE;
            std::memcpy(f.name, &buffer[offset], COMPIO_FNAME_MAX_SIZE);
            std::memcpy(&f.size, &buffer[offset + COMPIO_FNAME_MAX_SIZE], sizeof(uint64_t));
            if (is_be) swap_uint64(&f.size);
        }
        current_idx += count;
        remaining -= count;
    }
    rebuild_index();
    return true;
}

void files_table::write_to(FILE *file, uint64_t addr) const {
    if (fseek64(file, addr, SEEK_SET) != 0) {
        WARNING_PRINT("warning: fseek failed in files_table::write_to\n");
        return;
    }

    const size_t ENTRY_SIZE = COMPIO_FNAME_MAX_SIZE + sizeof(uint64_t);
    static_assert(sizeof(files_table::file) == ENTRY_SIZE, "struct file must be packed");

    bool is_be = is_big_endian();

    // Optimization: Bulk write if Little Endian
    if (!is_be) {
        assert(this->files.size() >= this->max_files);
        size_t entries_to_write = this->max_files;
        if (fwrite(this->files.data(), ENTRY_SIZE, entries_to_write, file) != entries_to_write) {
            WARNING_PRINT("warning: short write in files table (bulk)\n");
        }
        return;
    }

    const size_t BATCH_SIZE = 4096;
    std::vector<uint8_t> buffer(BATCH_SIZE * ENTRY_SIZE);

    size_t remaining = this->max_files; // write capacity slots
    size_t current_idx = 0;

    while (remaining > 0) {
        size_t count = std::min(remaining, BATCH_SIZE);
        
        for (size_t i = 0; i < count; ++i) {
            const files_table::file& f = this->files[current_idx + i];
            size_t offset = i * ENTRY_SIZE;
            std::memcpy(&buffer[offset], f.name, COMPIO_FNAME_MAX_SIZE);
            uint64_t s = f.size;
            if (is_be) swap_uint64(&s);
            std::memcpy(&buffer[offset + COMPIO_FNAME_MAX_SIZE], &s, sizeof(uint64_t));
        }

        size_t bytes_to_write = count * ENTRY_SIZE;
        if (lendian_fwrite(buffer.data(), 1, bytes_to_write, file) != bytes_to_write) {
            WARNING_PRINT("warning: short write in files table\n");
            return;
        }
        current_idx += count;
        remaining -= count;
    }
}

// Helper to batch read files table (Legacy v4 support)
static bool read_files_batched_legacy(FILE* file, files_table& ftable) {
    // Legacy reads from CURRENT position (part of header stream)
    // Reuse new implementation logic but read from current pos?
    // Or just copy-paste for safety.
    // Actually we can use ftable.read_from if we know the address.
    // But header::read_from(v4) calls it inline.
    // So we can pass `ftell(file)` as address?
    // But header::read_from calls fseek at start, then reads sequentially.
    // So current file pos is correct.
    // ftable.read_from calls fseek.
    // So we can use `ftell`.
    long pos = ftell(file);
    if (pos < 0) return false;
    return ftable.read_from(file, static_cast<uint64_t>(pos), ftable.max_files, ftable.n_files);
}


header::header()
    : magic_number(COMPIO_MAGIC_NUMBER),
      index_root(0),
      file_size(0),
      files_table_addr(0),
      files_table_capacity(COMPIO_MAX_FILES),
      ftable(COMPIO_MAX_FILES),
      allocator_state_offset(0),
      allocator_state_size(0),
      compression_type(COMPIO_COMPRESS_ZLIB),
      block_size(0),
      b_tree_degree(0),
      sequence_id(0) {
    memset(checksum, 0, sizeof(checksum));
    file_size = disk_size();
}

header::header(uint32_t max_files)
    : magic_number(COMPIO_MAGIC_NUMBER),
      index_root(0),
      file_size(0),
      files_table_addr(0),
      files_table_capacity(max_files),
      ftable(max_files),
      allocator_state_offset(0),
      allocator_state_size(0),
      compression_type(COMPIO_COMPRESS_ZLIB),
      block_size(0),
      b_tree_degree(0),
      sequence_id(0) {
    memset(checksum, 0, sizeof(checksum));
    file_size = disk_size();
}

uint64_t header::disk_size() const {
    if (magic_number == COMPIO_MAGIC_NUMBER) { // v5
        return 4 + 32 + 8 + 8 + 8 + 8 + 8 + 4 + 4 + 4 + 8 + 4 + 8;
    }
    // v4 and older
    return 4 + 32 + 8 + 8 + 8 + 8 + 8 + 4 + 4 + 4 + 4 + 8 +
           static_cast<uint64_t>(ftable.max_files) * (COMPIO_FNAME_MAX_SIZE + 8);
}

void header::compute_checksum(uint8_t *out_hash) const {
    SHA256 ctx;
    ctx.update(reinterpret_cast<const uint8_t*>(&magic_number), sizeof(magic_number));
    ctx.update(reinterpret_cast<const uint8_t*>(&sequence_id), sizeof(sequence_id));
    ctx.update(reinterpret_cast<const uint8_t*>(&index_root), sizeof(index_root));
    ctx.update(reinterpret_cast<const uint8_t*>(&file_size), sizeof(file_size));
    ctx.update(reinterpret_cast<const uint8_t*>(&allocator_state_offset), sizeof(allocator_state_offset));
    ctx.update(reinterpret_cast<const uint8_t*>(&allocator_state_size), sizeof(allocator_state_size));
    ctx.update(reinterpret_cast<const uint8_t*>(&compression_type), sizeof(compression_type));
    ctx.update(reinterpret_cast<const uint8_t*>(&block_size), sizeof(block_size));
    ctx.update(reinterpret_cast<const uint8_t*>(&b_tree_degree), sizeof(b_tree_degree));

    if (magic_number == COMPIO_MAGIC_NUMBER) { // v5
        ctx.update(reinterpret_cast<const uint8_t*>(&files_table_addr), sizeof(files_table_addr));
        ctx.update(reinterpret_cast<const uint8_t*>(&files_table_capacity), sizeof(files_table_capacity));
        ctx.update(reinterpret_cast<const uint8_t*>(&ftable.n_files), sizeof(ftable.n_files));
        
        // For v5, do not include the external files table contents in the header checksum.
        // The table is stored and updated independently; hashing it here would break
        // the double-buffered header crash-safety guarantees.
    } else { // v4
        ctx.update(reinterpret_cast<const uint8_t*>(&files_table_capacity), sizeof(files_table_capacity));
        ctx.update(reinterpret_cast<const uint8_t*>(&ftable.n_files), sizeof(ftable.n_files));
        
        for (uint32_t i = 0; i < files_table_capacity; ++i) {
            ctx.update(reinterpret_cast<const uint8_t*>(&ftable.files[i].name), sizeof(ftable.files[i].name));
            ctx.update(reinterpret_cast<const uint8_t*>(&ftable.files[i].size), sizeof(ftable.files[i].size));
        }
    }
    
    ctx.finalize(out_hash);
}

bool header::load_and_validate(FILE *file, uint64_t addr) {
    DEBUG_PRINT("[R][header]addr=%" PRIu64 "\n", addr);
    if (fseek64(file, addr, SEEK_SET)) {
        DEBUG_PRINT("warning: fseek failed\n");
        return false;
    }
        
    lendian_fread_member(magic_number, file);
    bool is_v5 = (magic_number == COMPIO_MAGIC_NUMBER);
    bool is_v4 = (magic_number == 27110661);

    if (!is_v5 && !is_v4) {
        WARNING_PRINT("warning: header magic_number does not match "
                      "(expected %d or %d, got %d). "
                      "The archive may have been created with an incompatible format version.\n",
                      COMPIO_MAGIC_NUMBER, 27110661, magic_number);
        return false;
    }
    
    lendian_fread(checksum, 1, sizeof(checksum), file);
    lendian_fread_member(sequence_id, file);
    
    lendian_fread_member(index_root, file);
    lendian_fread_member(file_size, file);
    lendian_fread_member(allocator_state_offset, file);
    lendian_fread_member(allocator_state_size, file);
    lendian_fread_member(compression_type, file);
    lendian_fread_member(block_size, file);
    lendian_fread_member(b_tree_degree, file);

    if (is_v5) {
        lendian_fread_member(files_table_addr, file);
        lendian_fread_member(files_table_capacity, file);
        lendian_fread_member(ftable.n_files, file);
        
        if (files_table_capacity == 0 || files_table_capacity > COMPIO_MAX_FILES_LIMIT) {
             WARNING_PRINT("warning: header files_table_capacity=%u is out of valid range\n", files_table_capacity);
             return false;
        }

        if (ftable.n_files > files_table_capacity) {
             WARNING_PRINT("warning: header n_files=%lu > capacity=%u\n", ftable.n_files, files_table_capacity);
             return false;
        }
        
        // Read table from external address.
        // If addr is 0 (uninitialized) and there are files, this is invalid.
        // If addr is 0 and n_files==0, treat it as an empty external table and skip reading.
        if (files_table_addr == 0) {
            if (ftable.n_files > 0) {
                WARNING_PRINT("warning: files_table_addr is 0 but table is not empty\n");
                return false;
            }
            // Empty table: no bytes to read from disk, but capacity is still meaningful.
            ftable.max_files = files_table_capacity;
        } else {
            ftable.max_files = files_table_capacity;
            if (!ftable.read_from(file, files_table_addr, files_table_capacity, ftable.n_files)) {
                return false;
            }
        }

    } else { // v4
        uint32_t max_files_v4;
        lendian_fread(&max_files_v4, 1, 4, file);
        if (is_big_endian()) swap_uint32(&max_files_v4);
        
        lendian_fread_member(ftable.n_files, file);
        
        files_table_capacity = max_files_v4;
        ftable.max_files = max_files_v4;
        
        if (max_files_v4 == 0 || max_files_v4 > COMPIO_MAX_FILES_LIMIT) {
            WARNING_PRINT("warning: header max_files=%u is out of valid range\n", max_files_v4);
            return false;
        }
        
        // Read inline
        long pos = ftell(file);
        if (pos < 0) return false;
        if (!ftable.read_from(file, static_cast<uint64_t>(pos), max_files_v4, ftable.n_files)) {
             return false;
        }
        files_table_addr = 0; // Inline
    }

    // Verify checksum
    uint8_t calc_checksum[32];
    compute_checksum(calc_checksum);
    if (memcmp(checksum, calc_checksum, 32) != 0) {
        WARNING_PRINT("warning: header checksum mismatch\n");
        return false;
    }
    
    return true;
}

bool header::read_from(FILE *file, uint64_t addr) {
    return load_and_validate(file, addr);
}

void header::write_to(FILE *file, uint64_t addr, compio::WalManager* wal_manager) const {
    DEBUG_PRINT("[W][header]addr=%" PRIu64 ";size=%" PRIu64 "\n", addr, disk_size());
    
    // Auto-update checksum before writing
    // We cast away const because we want the on-disk structure to be correct, 
    // and updating the checksum member is logically part of the serialization process.
    const_cast<header*>(this)->compute_checksum(const_cast<uint8_t*>(checksum));

    if (wal_manager) {
        wal_manager->begin_transaction();
        
        // Optimize: Pre-allocate full buffer to avoid reallocations
        uint64_t total_size = disk_size();
        std::vector<uint8_t> buffer(total_size);
        uint8_t* ptr = buffer.data();
        bool is_be = is_big_endian();
        
        auto write_u32 = [&](uint32_t v) {
            if (is_be) { uint8_t* p = (uint8_t*)&v; std::swap(p[0], p[3]); std::swap(p[1], p[2]); }
            std::memcpy(ptr, &v, 4); ptr += 4;
        };
        auto write_u64 = [&](uint64_t v) {
            if (is_be) swap_uint64(&v);
            std::memcpy(ptr, &v, 8); ptr += 8;
        };
        
        // Serialize header fields
        write_u32(magic_number);
        std::memcpy(ptr, checksum, 32); ptr += 32;
        write_u64(sequence_id);
        write_u64(index_root);
        write_u64(file_size);
        write_u64(allocator_state_offset);
        write_u64(allocator_state_size);
        write_u32(compression_type);
        write_u32(block_size);
        write_u32(b_tree_degree);

        if (magic_number == COMPIO_MAGIC_NUMBER) { // v5
            write_u64(files_table_addr);
            write_u32(files_table_capacity);
            write_u64(ftable.n_files);
        } else {
             // Fallback for v4 (only partial, cannot write inline table anymore)
             write_u32(ftable.max_files);
             write_u64(ftable.n_files);
        }
        
        // Files Table is NOT written here for v5 (external)
        // For v4 it was inline, but we removed support for inline writing.
        
        if (!wal_manager->log_write(WalRecordType::HEADER, addr, buffer.data(), buffer.size())) {
            WARNING_PRINT("error: WAL log_write failed for header at addr=%" PRIu64 "\n", addr);
        }
        
        if (!wal_manager->commit_transaction()) {
            WARNING_PRINT("error: WAL commit failed for header at addr=%" PRIu64 "\n", addr);
        }
    }

    if (fseek64(file, addr, SEEK_SET))
        DEBUG_PRINT("warning: fseek failed\n");
        
    lendian_fwrite_member(magic_number, file);
    lendian_fwrite(checksum, 1, sizeof(checksum), file);
    lendian_fwrite_member(sequence_id, file);
    
    lendian_fwrite_member(index_root, file);
    lendian_fwrite_member(file_size, file);
    lendian_fwrite_member(allocator_state_offset, file);
    lendian_fwrite_member(allocator_state_size, file);
    lendian_fwrite_member(compression_type, file);
    lendian_fwrite_member(block_size, file);
    lendian_fwrite_member(b_tree_degree, file);

    if (magic_number == COMPIO_MAGIC_NUMBER) { // v5
        lendian_fwrite_member(files_table_addr, file);
        lendian_fwrite_member(files_table_capacity, file);
        lendian_fwrite_member(ftable.n_files, file);
    } else {
        // Non-v5 (legacy) archives are not supported for writing in this version.
        // The previous implementation here was explicitly marked as "Broken v4 write"
        // and did not serialize the inline files table correctly, which could lead
        // to stale or inconsistent metadata on subsequent opens.
        WARNING_PRINT("error: attempting to write header for non-v5 archive (magic=%" PRIu32 "); "
                      "writing legacy/v4 archives is not supported\n", magic_number);
        // Fail fast in debug builds to surface incorrect usage early.
        assert(false && "writing legacy/v4 archives is not supported");
        // In release builds, return after logging to avoid performing an incomplete
        // or inconsistent v4-specific header update.
        return;
    }
}

bool index_node::read_from(FILE *file, uint64_t addr) {
    DEBUG_PRINT("[R][index_node]addr=%" PRIu64 "\n", addr);
    if (fseek64(file, addr, SEEK_SET)) {
        DEBUG_PRINT("warning: fseek failed\n");
        return false;
    }

    // Optimization: Read header (6 bytes)
    uint8_t header_buf[6];
    if (fread(header_buf, 1, 6, file) != 6) return false;

    uint8_t signature = header_buf[0];
    if (signature != index_node_signature) {
        WARNING_PRINT("warning: index_node signature does not match\n");
        return false;
    }
    is_leaf = header_buf[1];
    
    // Read num_keys (LE)
    num_keys = static_cast<uint32_t>(header_buf[2]) |
               (static_cast<uint32_t>(header_buf[3]) << 8) |
               (static_cast<uint32_t>(header_buf[4]) << 16) |
               (static_cast<uint32_t>(header_buf[5]) << 24);

    // Sanity check num_keys
    if (num_keys > 2 * (uint32_t)tree_degree - 1) {
        WARNING_PRINT("error: index_node num_keys %u exceeds max %u (degree=%d)\n", 
                      num_keys, 2 * tree_degree - 1, tree_degree);
        return false;
    }

    keys.resize(num_keys);
    values.resize(num_keys);
    if (!is_leaf) {
        children.resize(num_keys + 1);
        key_additions.resize(num_keys + 1);
    }

    bool is_be = is_big_endian();

    // Batch read keys (num_keys * 16 bytes)
    if (num_keys > 0) {
        if (fread(keys.data(), sizeof(tree_key), num_keys, file) != num_keys) return false;
        if (is_be) {
            for (auto &key : keys) {
                swap_uint64(&key.hash);
                swap_uint64(&key.pos);
            }
        }
    }

    // Batch read values (num_keys * 16 bytes)
    if (num_keys > 0) {
        if (fread(values.data(), sizeof(tree_val), num_keys, file) != num_keys) return false;
        if (is_be) {
            for (auto &val : values) {
                swap_uint64(&val.addr);
                swap_uint64(&val.size);
            }
        }
    }

    if (!is_leaf) {
        // Batch read children
        if (fread(children.data(), sizeof(uint64_t), children.size(), file) != children.size()) return false;
        if (is_be) {
            for (auto &child : children) swap_uint64(&child);
        }

        // Batch read key_additions
        if (fread(key_additions.data(), sizeof(int64_t), key_additions.size(), file) != key_additions.size()) return false;
        if (is_be) {
            for (auto &add : key_additions) {
                uint64_t tmp;
                std::memcpy(&tmp, &add, sizeof(int64_t));
                swap_uint64(&tmp);
                std::memcpy(&add, &tmp, sizeof(int64_t));
            }
        }
    }

    validate();
    return true;
}

void index_node::write_to(FILE *file, uint64_t addr, compio::WalManager* wal_manager) const {
    DEBUG_PRINT("[W][index_node]addr=%" PRIu64 ";size=%" PRIu64 "\n", addr, (uint64_t)INDEX_NODE_SIZE(tree_degree));
    validate();
    
    // Calculate total size needed
    size_t total_size = INDEX_NODE_METASIZE;
    total_size += keys.size() * sizeof(tree_key);
    total_size += values.size() * sizeof(tree_val);
    if (!is_leaf) {
        total_size += children.size() * sizeof(uint64_t);
        total_size += key_additions.size() * sizeof(int64_t);
    }

    std::vector<uint8_t> buffer;
    buffer.reserve(total_size); 

    // Header (manually serialized to ensure LE)
    auto push_u8 = [&](uint8_t v) { buffer.push_back(v); };
    auto push_u32 = [&](uint32_t v) { 
        for(int i=0; i<4; ++i) buffer.push_back(static_cast<uint8_t>(v >> (i*8))); 
    };

    push_u8(index_node_signature);
    push_u8(is_leaf);
    
    const uint32_t actual_num_keys = keys.size();
    assert(num_keys == actual_num_keys);
    push_u32(actual_num_keys);
    
    bool is_be = is_big_endian();

    // Helper to append vector data
    auto append_vector = [&](const void* data, size_t size, size_t count, bool swap_64) {
        size_t byte_count = size * count;
        size_t current_pos = buffer.size();
        buffer.resize(current_pos + byte_count);
        std::memcpy(buffer.data() + current_pos, data, byte_count);
        
        if (is_be && swap_64) {
            // Swap 64-bit values in place without assuming alignment or strict aliasing
            uint8_t* base = buffer.data() + current_pos;
            size_t u64_count = byte_count / 8;
            for (size_t i = 0; i < u64_count; ++i) {
                uint64_t tmp;
                std::memcpy(&tmp, base + i * 8, sizeof(tmp));
                swap_uint64(&tmp);
                std::memcpy(base + i * 8, &tmp, sizeof(tmp));
            }
        }
    };

    if (!keys.empty()) {
        append_vector(keys.data(), sizeof(tree_key), keys.size(), true);
    }
    if (!values.empty()) {
        append_vector(values.data(), sizeof(tree_val), values.size(), true);
    }
    
    if (!is_leaf) {
        if (!children.empty()) {
            append_vector(children.data(), sizeof(uint64_t), children.size(), true);
        }
        if (!key_additions.empty()) {
            append_vector(key_additions.data(), sizeof(int64_t), key_additions.size(), true);
        }
    }

    if (wal_manager) {
        wal_manager->begin_transaction();
        if (!wal_manager->log_write(WalRecordType::INDEX_NODE, addr, buffer.data(), buffer.size())) {
            WARNING_PRINT("error: WAL log_write failed for index_node at addr=%" PRIu64 "\n", addr);
        }
        if (!wal_manager->commit_transaction()) {
            WARNING_PRINT("error: WAL commit failed for index_node at addr=%" PRIu64 "\n", addr);
        }
    }

    if (fseek64(file, addr, SEEK_SET))
        DEBUG_PRINT("warning: fseek failed\n");
    
    if (fwrite(buffer.data(), 1, buffer.size(), file) != buffer.size()) {
        DEBUG_PRINT("warning: fwrite failed\n");
    }
}

#define DEBUG_VAL (INT64_MAX - 123)

void index_node::validate() const {
#ifndef NDEBUG
    assert(keys.size() == num_keys);
    assert(values.size() == num_keys);
    for (const auto &key : keys) {
        assert(key.hash != DEBUG_VAL);
        assert(key.pos != DEBUG_VAL);
    }
    for (const auto &value : values) {
        assert(value.addr != DEBUG_VAL);
        assert(value.size != DEBUG_VAL);
    }
    if (!is_leaf) {
        assert(children.size() == num_keys + 1);
        assert(key_additions.size() == num_keys + 1);
        for (const auto &child : children) {
            assert(child != DEBUG_VAL);
        }
        for (const auto &key_addition : key_additions) {
            assert(key_addition != DEBUG_VAL);
        }
    }
    for (std::size_t i = 1; i < keys.size(); i++) {
        assert(keys[i - 1] < keys[i]);
    }
    for (std::size_t i = 1; i < keys.size(); i++) {
        if (keys[i - 1].hash == keys[i].hash) {
            uint64_t prev_end = keys[i - 1].pos + values[i - 1].size;
            if (prev_end > keys[i].pos) {
                DEBUG_PRINT("[NODE_VALIDATE]: node state:\n");
                for (std::size_t j = 0; j < num_keys; ++j) {
                    DEBUG_PRINT("\t{%" PRIu64 ",%" PRIu64 "} -> {%" PRIu64 ",%" PRIu64 "}\n", keys[j].hash, keys[j].pos,
                                values[j].addr, values[j].size);
                }
            }
            assert(prev_end <= keys[i].pos);
        }
    }
#endif
}

bool storage_block::read_from(FILE *file, uint64_t addr) {
    DEBUG_PRINT("[R][storage_block]addr=%" PRIu64 "\n", addr);
    assert(addr != 0);
    if (fseek64(file, addr, SEEK_SET)) {
        DEBUG_PRINT("warning: fseek failed\n");
        return false;
    }

    // Optimization: Read all metadata in one go (22 bytes)
    uint8_t meta_buffer[STORAGE_BLOCK_METASIZE];
    if (lendian_fread(meta_buffer, 1, STORAGE_BLOCK_METASIZE, file) != STORAGE_BLOCK_METASIZE) {
        return false;
    }

    size_t meta_idx = 0;
    auto read_u8 = [&]() { return meta_buffer[meta_idx++]; };
    auto read_u32 = [&]() {
        uint32_t v = 0;
        for(int i=0; i<4; ++i) v |= (static_cast<uint32_t>(meta_buffer[meta_idx++]) << (i*8));
        return v;
    };
    auto read_u64 = [&]() {
        uint64_t v = 0;
        for(int i=0; i<8; ++i) v |= (static_cast<uint64_t>(meta_buffer[meta_idx++]) << (i*8));
        return v;
    };

    uint8_t signature = read_u8();
    if (signature == storage_block::signature) {
        checksum_type = COMPIO_CHECKSUM_FNV1A;
    } else if (signature == storage_block::signature_crc32c) {
        checksum_type = COMPIO_CHECKSUM_CRC32C;
    } else {
        WARNING_PRINT("warning: storage_block signature does not match (got %d)\n", signature);
        return false;
    }

    is_compressed = read_u8();
    if (is_compressed > 1) {
        WARNING_PRINT("error: storage_block is_compressed invalid (%u) at addr=%" PRIu64 "\n", is_compressed, addr);
        return false;
    }

    size = read_u64();
    if (size == 0) {
        WARNING_PRINT("error: storage_block size is 0 at addr=%" PRIu64 "\n", addr);
        return false;
    }

    // Cap block size to prevent OOM on corrupted files
    static constexpr uint64_t MAX_BLOCK_SIZE = 256ULL * 1024 * 1024; // 256 MB
    if (size > MAX_BLOCK_SIZE) {
        WARNING_PRINT("warning: storage_block size %llu exceeds limit at addr=%llu\n", 
                      (unsigned long long)size, (unsigned long long)addr);
        size = 0;
        return false;
    }

    original_size = read_u64();
    checksum = read_u32();

    assert(meta_idx == STORAGE_BLOCK_METASIZE);

    data = std::unique_ptr<uint8_t[]>(new uint8_t[size]);
    if (lendian_fread(data.get(), 1, size, file) != size) {
        return false;
    }

    if (!verify_checksum()) {
        WARNING_PRINT("warning: storage_block checksum verification failed at addr=%llu\n", (unsigned long long)addr);
        return false;
    }
    return true;
}

void storage_block::write_to(FILE *file, uint64_t addr, compio::WalManager* wal_manager) const {
    DEBUG_PRINT("[W][storage_block]addr=%" PRIu64 ";size=%" PRIu64 "\n", addr, STORAGE_BLOCK_METASIZE + size);
    assert(addr != 0);
    assert(size > 0);
    assert(original_size > 0);
    assert(is_compressed || size == original_size);

    // Calculate checksum before writing (non-const, so we cast)
    const_cast<storage_block*>(this)->calculate_checksum();

    // Prepare metadata buffer
    uint8_t meta_buffer[STORAGE_BLOCK_METASIZE];
    size_t meta_idx = 0;

    auto push_u8 = [&](uint8_t v) { meta_buffer[meta_idx++] = v; };
    auto push_u32 = [&](uint32_t v) { 
        for(int i=0; i<4; ++i) meta_buffer[meta_idx++] = static_cast<uint8_t>(v >> (i*8)); 
    };
    auto push_u64 = [&](uint64_t v) { 
        for(int i=0; i<8; ++i) meta_buffer[meta_idx++] = static_cast<uint8_t>(v >> (i*8)); 
    };

    uint8_t sig = (checksum_type == COMPIO_CHECKSUM_CRC32C) ? storage_block::signature_crc32c : storage_block::signature;
    push_u8(sig);
    push_u8(is_compressed);
    push_u64(size);
    push_u64(original_size);
    push_u32(checksum);
    
    assert(meta_idx == STORAGE_BLOCK_METASIZE);

    if (wal_manager) {
        wal_manager->begin_transaction();
        
        std::vector<compio::WalManager::iovec_buf> buffers;
        buffers.push_back({meta_buffer, STORAGE_BLOCK_METASIZE});
        if (size > 0 && data) {
            buffers.push_back({data.get(), size});
        }
        
        if (!wal_manager->log_write_vectored(WalRecordType::BLOCK, addr, buffers)) {
            WARNING_PRINT("error: WAL log_write failed for storage_block at addr=%" PRIu64 "\n", addr);
        }
        if (!wal_manager->commit_transaction()) {
            WARNING_PRINT("error: WAL commit failed for storage_block at addr=%" PRIu64 "\n", addr);
        }
    }

    if (fseek64(file, addr, SEEK_SET))
        DEBUG_PRINT("warning: fseek failed\n");
        
    if (fwrite(meta_buffer, 1, STORAGE_BLOCK_METASIZE, file) != STORAGE_BLOCK_METASIZE) {
        DEBUG_PRINT("warning: fwrite failed for metadata\n");
    }
    if (size > 0 && data) {
        if (fwrite(data.get(), 1, size, file) != size) {
            DEBUG_PRINT("warning: fwrite failed for dataBody\n");
        }
    }
}

index_node::index_node(int tree_degree)
    : is_leaf(true),
      num_keys(0),
      keys(2 * tree_degree - 1, tree_key{DEBUG_VAL, DEBUG_VAL}),
      values(2 * tree_degree - 1, tree_val{DEBUG_VAL, DEBUG_VAL}),
      children(2 * tree_degree, DEBUG_VAL),
      key_additions(2 * tree_degree, DEBUG_VAL),
      tree_degree(tree_degree) {
    keys.clear();
    values.clear();
    children.clear();
    key_additions.clear();
}

storage_block::storage_block() : data(nullptr), checksum_type(COMPIO_CHECKSUM_FNV1A) {}

storage_block::storage_block(std::unique_ptr<uint8_t[]> &&data, uint64_t size)
    : is_compressed(0),
      size(size),
      original_size(0),
      data(std::move(data)),
      checksum_type(COMPIO_CHECKSUM_FNV1A) {}

storage_block::storage_block(uint64_t size)
    : storage_block(std::unique_ptr<uint8_t[]>(new uint8_t[size]), size) {
        checksum_type = COMPIO_CHECKSUM_FNV1A;
    }

files_table::files_table() : n_files(0), max_files(COMPIO_MAX_FILES), files(COMPIO_MAX_FILES) {
}

files_table::files_table(uint32_t max_files)
    : n_files(0), max_files(max_files), files(max_files) {
}

files_table::files_table(const files_table& other)
    : n_files(other.n_files), max_files(other.max_files), files(other.files) {
    // Index map is transient, but we must rebuild it so the new copy is usable for lookups
    rebuild_index();
}

files_table& files_table::operator=(const files_table& other) {
    if (this != &other) {
        n_files = other.n_files;
        max_files = other.max_files;
        files = other.files;
        // Rebuild index in the target
        rebuild_index();
    }
    return *this;
}

files_table::files_table(files_table&& other) noexcept
    : n_files(other.n_files), max_files(other.max_files),
      files(std::move(other.files)), index_map_(std::move(other.index_map_)) {
    other.n_files = 0;
    other.max_files = 0;
}

files_table& files_table::operator=(files_table&& other) noexcept {
    if (this != &other) {
        n_files = other.n_files;
        max_files = other.max_files;
        files = std::move(other.files);
        index_map_ = std::move(other.index_map_);
        
        other.n_files = 0;
        other.max_files = 0;
    }
    return *this;
}

void files_table::rebuild_index() {
    index_map_.clear();
    index_map_.reserve(n_files);
    for (uint32_t i = 0; i < n_files; ++i) {
        // Insert if not exists to preserve "first match" semantics for legacy archives
        size_t len = portable_strnlen(files[i].name, COMPIO_FNAME_MAX_SIZE);
        if (len >= COMPIO_FNAME_MAX_SIZE) {
            len = COMPIO_FNAME_MAX_SIZE - 1;
        }
        std::string_view key(files[i].name, len);
        if (!index_map_.find(key)) {
            index_map_.emplace(key, i);
        }
    }
}

const files_table::file *files_table::find(const char *name) const {
    if (n_files == 0) return nullptr;
    if (!name) return nullptr;
    
    // Construct lookup key with truncation logic
    size_t len = portable_strnlen(name, COMPIO_FNAME_MAX_SIZE);
    std::string_view key;
    if (len >= COMPIO_FNAME_MAX_SIZE) {
        key = std::string_view(name, COMPIO_FNAME_MAX_SIZE - 1);
    } else {
        key = std::string_view(name, len);
    }
    
    auto ptr = index_map_.find(key);
    if (ptr) {
        return &files[*ptr];
    }
    
    return nullptr;
}

files_table::file *files_table::find(const char *name) {
    // Cast constness away to reuse implementation
    return const_cast<files_table::file*>(
        static_cast<const files_table*>(this)->find(name)
    );
}

files_table::file *files_table::add(const char *name, bool allow_resize) {
    if (n_files >= max_files) {
        if (!allow_resize) return NULL;
        
        // Dynamically resize the files table
        uint32_t new_max = (max_files == 0) ? 16 : max_files * 2;
        // Cap at some reasonable limit if needed, e.g. 1M files?
        // But for now let it grow.
        
        // Resize vector. This invalidates all pointers and string_views in index_map_.
        files.resize(new_max);
        max_files = new_max;
        
        // Rebuild the index map from scratch with new pointers
        rebuild_index();
    }
    
    if (!name) return NULL;
    
    // Create bounded string_view
    size_t len = portable_strnlen(name, COMPIO_FNAME_MAX_SIZE);
    std::string_view key;
    if (len >= COMPIO_FNAME_MAX_SIZE) {
        key = std::string_view(name, COMPIO_FNAME_MAX_SIZE - 1);
    } else {
        key = std::string_view(name, len);
    }
    
    // Check if it already exists using string_view lookup
    // If so, return existing entry to prevent duplicates.
    auto ptr = index_map_.find(key);
    if (ptr) {
        return &files[*ptr];
    }
        
    strncpy(files[n_files].name, name, COMPIO_FNAME_MAX_SIZE - 1);
    files[n_files].name[COMPIO_FNAME_MAX_SIZE - 1] = '\0';
    files[n_files].size = 0;
    
    // Update index
    std::string_view stored_key(files[n_files].name, key.length());
    index_map_.emplace(stored_key, n_files);
    
    return &files[n_files++];
}

int files_table::remove(const char *name) {
    if (!name) return -1;
    
    // Construct lookup key
    size_t len = portable_strnlen(name, COMPIO_FNAME_MAX_SIZE);
    std::string_view key;
    if (len >= COMPIO_FNAME_MAX_SIZE) {
        key = std::string_view(name, COMPIO_FNAME_MAX_SIZE - 1);
    } else {
        key = std::string_view(name, len);
    }
    
    auto ptr = index_map_.find(key);
    if (!ptr) {
        return -1;
    }
    
    uint32_t i = *ptr;
    
    // 1. Remove the entry for the file being deleted.
    index_map_.erase(key);

    if (i != n_files - 1) {
        uint32_t last_idx = n_files - 1;
        const char* last_name_ptr = files[last_idx].name;
        
        size_t last_len = portable_strnlen(last_name_ptr, COMPIO_FNAME_MAX_SIZE);
        std::string_view last_key(last_name_ptr, last_len < COMPIO_FNAME_MAX_SIZE ? last_len : COMPIO_FNAME_MAX_SIZE - 1);
        
        auto last_ptr = index_map_.find(last_key);
        
        bool update_index = false;
        if (last_ptr) {
             if (*last_ptr == last_idx) {
                 index_map_.erase(last_key);
                 update_index = true;
             }
        } else {
             update_index = true;
        }

        files[i] = files[last_idx];
        
        if (update_index) {
            std::string_view new_key(files[i].name, last_key.length());
            index_map_.insert_or_assign(new_key, i);
        }
    } else {
        // Removing the last element. Just decrement count.
        // index_map_ entry already erased.
    }
    
    // Decrease count
    --n_files;
    
    // Zero out the slot that is now unused to avoid leaking deleted data
    // and ensure deterministic content for checksums (though existing archives may have garbage).
    memset(&files[n_files], 0, sizeof(files_table::file));
    
    return 0;
}

void storage_block::calculate_checksum() {
    if (!data || size == 0) {
        checksum = 0;
        return;
    }

    if (checksum_type == COMPIO_CHECKSUM_CRC32C) {
        checksum = crc32c(data.get(), size);
    } else {
        checksum = fnv1a_32(data.get(), size);
    }
}

bool storage_block::verify_checksum() const {
    if (!data || size == 0) {
        return checksum == 0;
    }

    uint32_t computed;
    if (checksum_type == COMPIO_CHECKSUM_CRC32C) {
        computed = crc32c(data.get(), size);
    } else {
        computed = fnv1a_32(data.get(), size);
    }
    return checksum == computed;
}
