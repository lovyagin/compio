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

// Helper to batch read files table
static bool read_files_batched(FILE* file, files_table& ftable) {
    const size_t BATCH_SIZE = 4096; // 4096 files * 40 bytes = ~160KB buffer
    const size_t ENTRY_SIZE = COMPIO_FNAME_MAX_SIZE + sizeof(uint64_t); // 32 + 8 = 40
    
    // Ensure packing assumptions hold (no padding)
    static_assert(sizeof(files_table::file) == ENTRY_SIZE, "struct file must be packed");
    
    std::vector<uint8_t> buffer(BATCH_SIZE * ENTRY_SIZE);
    
    // Read all max_files slots because the checksum covers the entire table
    // (including unused slots), and we must advance the file pointer correctly.
    size_t remaining = ftable.max_files;
    size_t current_idx = 0;
    bool is_be = is_big_endian();
    
    while (remaining > 0) {
        size_t count = std::min(remaining, BATCH_SIZE);
        size_t bytes_to_read = count * ENTRY_SIZE;
        
        // Use lendian_fread with size=1 to read bytes directly into buffer
        // (endian swapping is handled manually below). 
        // Checks ferror and updates metrics.
        size_t read_count = lendian_fread(buffer.data(), 1, bytes_to_read, file);
        if (read_count != bytes_to_read) {
            WARNING_PRINT("warning: short read in files table (expected %zu, got %zu)\n", 
                          bytes_to_read, read_count);
            return false;
        }
        
        for (size_t i = 0; i < count; ++i) {
            files_table::file& f = ftable.files[current_idx + i];
            size_t offset = i * ENTRY_SIZE;
            
            // Copy name
            std::memcpy(f.name, &buffer[offset], COMPIO_FNAME_MAX_SIZE);
            
            // Copy size
            std::memcpy(&f.size, &buffer[offset + COMPIO_FNAME_MAX_SIZE], sizeof(uint64_t));
            
            // Swap if Big Endian (data on disk is Little Endian)
            if (is_be) {
                swap_uint64(&f.size);
            }
        }
        
        current_idx += count;
        remaining -= count;
    }
    return true;
}

// Helper to batch write files table
static bool write_files_batched(FILE* file, const files_table& ftable) {
    const size_t BATCH_SIZE = 4096;
    const size_t ENTRY_SIZE = COMPIO_FNAME_MAX_SIZE + sizeof(uint64_t);
    
    std::vector<uint8_t> buffer(BATCH_SIZE * ENTRY_SIZE);
    
    size_t remaining = ftable.max_files; // write_to loops over max_files, not n_files
    size_t current_idx = 0;
    bool is_be = is_big_endian();
    
    while (remaining > 0) {
        size_t count = std::min(remaining, BATCH_SIZE);
        
        for (size_t i = 0; i < count; ++i) {
            const files_table::file& f = ftable.files[current_idx + i];
            size_t offset = i * ENTRY_SIZE;
            
            // Copy name
            std::memcpy(&buffer[offset], f.name, COMPIO_FNAME_MAX_SIZE);
            
            // Copy size
            uint64_t s = f.size;
            if (is_be) {
                swap_uint64(&s);
            }
            std::memcpy(&buffer[offset + COMPIO_FNAME_MAX_SIZE], &s, sizeof(uint64_t));
        }
        
        size_t bytes_to_write = count * ENTRY_SIZE;
        // Use lendian_fwrite with size=1 to write bytes directly from buffer.
        // Checks ferror and updates metrics.
        if (lendian_fwrite(buffer.data(), 1, bytes_to_write, file) != bytes_to_write) {
            WARNING_PRINT("warning: short write in files table\n");
            return false;
        }
        
        current_idx += count;
        remaining -= count;
    }
    return true;
}

header::header()
    : magic_number(COMPIO_MAGIC_NUMBER),
      index_root(0),
      file_size(0),
      ftable(),
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
    // magic(4) + checksum(32) + sequence_id(8) + index_root(8) + file_size(8) 
    // + allocator_state_offset(8) + allocator_state_size(8) + compression_type(4) 
    // + block_size(4) + b_tree_degree(4) + max_files(4) + n_files(8)
    // + files[max_files] * (32 + 8)
    return 4 + 32 + 8 + 8 + 8 + 8 + 8 + 4 + 4 + 4 + 4 + 8 +
           static_cast<uint64_t>(ftable.max_files) * (COMPIO_FNAME_MAX_SIZE + 8);
}

void header::compute_checksum(uint8_t *out_hash) const {
    SHA256 ctx;
    // Digest fields in order, SKIPPING the checksum field itself
    ctx.update(reinterpret_cast<const uint8_t*>(&magic_number), sizeof(magic_number));
    // Skip checksum (32 bytes)
    ctx.update(reinterpret_cast<const uint8_t*>(&sequence_id), sizeof(sequence_id));
    ctx.update(reinterpret_cast<const uint8_t*>(&index_root), sizeof(index_root));
    ctx.update(reinterpret_cast<const uint8_t*>(&file_size), sizeof(file_size));
    ctx.update(reinterpret_cast<const uint8_t*>(&allocator_state_offset), sizeof(allocator_state_offset));
    ctx.update(reinterpret_cast<const uint8_t*>(&allocator_state_size), sizeof(allocator_state_size));
    ctx.update(reinterpret_cast<const uint8_t*>(&compression_type), sizeof(compression_type));
    ctx.update(reinterpret_cast<const uint8_t*>(&block_size), sizeof(block_size));
    ctx.update(reinterpret_cast<const uint8_t*>(&b_tree_degree), sizeof(b_tree_degree));
    ctx.update(reinterpret_cast<const uint8_t*>(&ftable.max_files), sizeof(ftable.max_files));
    ctx.update(reinterpret_cast<const uint8_t*>(&ftable.n_files), sizeof(ftable.n_files));
    
    for (uint32_t i = 0; i < ftable.max_files; ++i) {
        ctx.update(reinterpret_cast<const uint8_t*>(&ftable.files[i].name), sizeof(ftable.files[i].name));
        ctx.update(reinterpret_cast<const uint8_t*>(&ftable.files[i].size), sizeof(ftable.files[i].size));
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
    if (magic_number != COMPIO_MAGIC_NUMBER) {
        WARNING_PRINT("warning: header magic_number does not match "
                      "(expected %d, got %d). "
                      "The archive may have been created with an incompatible format version.\n",
                      COMPIO_MAGIC_NUMBER, magic_number);
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
    lendian_fread_member(ftable.max_files, file);
    
    if (ftable.max_files == 0 || ftable.max_files > COMPIO_MAX_FILES_LIMIT) {
        WARNING_PRINT("warning: header max_files=%u is out of valid range [1, %u]\n",
                      ftable.max_files, COMPIO_MAX_FILES_LIMIT);
        return false;
    }
    
    ftable.files.resize(ftable.max_files);
    lendian_fread_member(ftable.n_files, file);
    
    if (ftable.n_files > ftable.max_files) {
        WARNING_PRINT("warning: header n_files=%llu exceeds max_files=%u\n",
                      (unsigned long long)ftable.n_files, ftable.max_files);
        return false;
    }
    
    // Batched read for performance (O(N) -> O(N/BATCH))
    if (!read_files_batched(file, ftable)) {
        return false;
    }
    
    // Rebuild the lookup map since we bypassed add()
    ftable.rebuild_index();
    
    // Validate Checksum
    uint8_t computed[32];
    compute_checksum(computed);
    if (memcmp(checksum, computed, 32) != 0) {
         WARNING_PRINT("warning: header checksum mismatch! Archive header may be corrupted.\n");
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
        write_u32(ftable.max_files);
        write_u64(ftable.n_files);
        
        // Files Table
        for (uint32_t i = 0; i < ftable.max_files; ++i) {
            const auto& f = ftable.files[i];
            std::memcpy(ptr, f.name, COMPIO_FNAME_MAX_SIZE); ptr += COMPIO_FNAME_MAX_SIZE;
            uint64_t s = f.size;
            if (is_be) swap_uint64(&s);
            std::memcpy(ptr, &s, 8); ptr += 8;
        }
        
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
    lendian_fwrite_member(ftable.max_files, file);
    lendian_fwrite_member(ftable.n_files, file);
    
    // Batched write for performance
    if (!write_files_batched(file, ftable)) {
         DEBUG_PRINT("warning: write_files_batched failed\n");
    }
}

bool index_node::read_from(FILE *file, uint64_t addr) {
    DEBUG_PRINT("[R][index_node]addr=%" PRIu64 "\n", addr);
    if (fseek64(file, addr, SEEK_SET)) {
        DEBUG_PRINT("warning: fseek failed\n");
        return false;
    }
    uint8_t signature;
    if (lendian_fread(&signature, sizeof(signature), 1, file) != 1) return false;
    if (signature != index_node_signature) {
        WARNING_PRINT("warning: index_node signature does not match\n");
        return false;
    }
    if (lendian_fread_member(is_leaf, file) != 1) return false;
    if (lendian_fread_member(num_keys, file) != 1) return false;
    
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

    for (auto &key : keys) {
        if (lendian_fread_member(key.hash, file) != 1) return false;
        if (lendian_fread_member(key.pos, file) != 1) return false;
    }
    for (auto &value : values) {
        if (lendian_fread_member(value.addr, file) != 1) return false;
        if (lendian_fread_member(value.size, file) != 1) return false;
    }
    if (!is_leaf) {
        if (lendian_fread(children.data(), sizeof(uint64_t), children.size(), file) != children.size()) return false;
        if (lendian_fread(key_additions.data(), sizeof(int64_t), key_additions.size(), file) != key_additions.size()) return false;
    }
    validate();
    return true;
}

void index_node::write_to(FILE *file, uint64_t addr, compio::WalManager* wal_manager) const {
    DEBUG_PRINT("[W][index_node]addr=%" PRIu64 ";size=%" PRIu64 "\n", addr, (uint64_t)INDEX_NODE_SIZE(tree_degree));
    validate();
    
    std::vector<uint8_t> buffer;
    buffer.reserve(4096); 

    auto push_u8 = [&](uint8_t v) { buffer.push_back(v); };
    auto push_u32 = [&](uint32_t v) { 
        for(int i=0; i<4; ++i) buffer.push_back(static_cast<uint8_t>(v >> (i*8))); 
    };
    auto push_u64 = [&](uint64_t v) { 
        for(int i=0; i<8; ++i) buffer.push_back(static_cast<uint8_t>(v >> (i*8))); 
    };
    auto push_i64 = [&](int64_t v) { 
        uint64_t uv = static_cast<uint64_t>(v);
        for(int i=0; i<8; ++i) buffer.push_back(static_cast<uint8_t>(uv >> (i*8))); 
    };

    push_u8(index_node_signature);
    push_u8(is_leaf);
    
    const uint32_t actual_num_keys = keys.size();
    assert(num_keys == actual_num_keys);
    push_u32(actual_num_keys);
    
    for (auto &key : keys) {
        push_u64(key.hash);
        push_u64(key.pos);
    }
    for (auto &value : values) {
        push_u64(value.addr);
        push_u64(value.size);
    }
    
    if (!is_leaf) {
        for (auto &child : children) {
            push_u64(child);
        }
        for (auto &add : key_additions) {
            push_i64(add);
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
    uint8_t signature;
    if (lendian_fread(&signature, sizeof(signature), 1, file) != 1) {
        return false;
    }
    if (signature == storage_block::signature) {
        checksum_type = COMPIO_CHECKSUM_FNV1A;
    } else if (signature == storage_block::signature_crc32c) {
        checksum_type = COMPIO_CHECKSUM_CRC32C;
    } else {
        WARNING_PRINT("warning: storage_block signature does not match (got %d)\n", signature);
        return false;
    }
    if (lendian_fread_member(is_compressed, file) != 1) return false;
    if (is_compressed > 1) {
        WARNING_PRINT("error: storage_block is_compressed invalid (%u) at addr=%" PRIu64 "\n", is_compressed, addr);
        return false;
    }
    if (lendian_fread_member(size, file) != 1) return false;
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

    if (lendian_fread_member(original_size, file) != 1) return false;
    if (lendian_fread_member(checksum, file) != 1) return false;

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

files_table::file *files_table::add(const char *name) {
    if (n_files >= max_files)
        return NULL;
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
