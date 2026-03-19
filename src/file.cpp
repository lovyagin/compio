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

static const uint8_t index_node_signature = 67;
static const uint8_t storage_block_signature = 171;

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
    
    for (uint32_t i = 0; i < ftable.max_files; ++i) {
        lendian_fread(&ftable.files[i].name, 1, sizeof(ftable.files[i].name), file);
        lendian_fread_member(ftable.files[i].size, file);
    }
    
    // Validate Checksum
    uint8_t computed[32];
    compute_checksum(computed);
    if (memcmp(checksum, computed, 32) != 0) {
         WARNING_PRINT("warning: header checksum mismatch! Archive header may be corrupted.\n");
         return false;
    }
    return true;
}

void header::read_from(FILE *file, uint64_t addr) {
    if (!load_and_validate(file, addr)) {
        // Assert on failure as per original contract, but now we have validated it explicitly.
        // In recovery paths, we will use load_and_validate directly.
        assert(false);
    }
}

void header::write_to(FILE *file, uint64_t addr, compio::WalManager* wal_manager) const {
    DEBUG_PRINT("[W][header]addr=%" PRIu64 ";size=%" PRIu64 "\n", addr, disk_size());
    
    // Auto-update checksum before writing
    // We cast away const because we want the on-disk structure to be correct, 
    // and updating the checksum member is logically part of the serialization process.
    const_cast<header*>(this)->compute_checksum(const_cast<uint8_t*>(checksum));

    if (wal_manager) {
        wal_manager->begin_transaction();
        // Serialize to buffer for WAL
        std::vector<uint8_t> buffer;
        buffer.reserve(4096); 
        
        auto push_u32 = [&](uint32_t v) { 
            for(int i=0; i<4; ++i) buffer.push_back(static_cast<uint8_t>(v >> (i*8))); 
        };
        auto push_u64 = [&](uint64_t v) { 
            for(int i=0; i<8; ++i) buffer.push_back(static_cast<uint8_t>(v >> (i*8))); 
        };
        
        // Serialize header fields
        // magic(4)
        for(int i=0; i<4; ++i) buffer.push_back(static_cast<uint8_t>(magic_number >> (i*8)));
        // checksum(32)
        buffer.insert(buffer.end(), checksum, checksum + 32);
        // sequence_id(8)
        push_u64(sequence_id);
        // index_root(8)
        push_u64(index_root);
        // file_size(8)
        push_u64(file_size);
        // allocator_state_offset(8)
        push_u64(allocator_state_offset);
        // allocator_state_size(8)
        push_u64(allocator_state_size);
        // compression_type(4)
        push_u32(compression_type);
        // block_size(4)
        push_u32(block_size);
        // b_tree_degree(4)
        push_u32(b_tree_degree);
        // max_files(4)
        push_u32(ftable.max_files);
        // n_files(8)
        push_u64(ftable.n_files);
        
        for (uint32_t i = 0; i < ftable.max_files; ++i) {
            // name(32)
            buffer.insert(buffer.end(), ftable.files[i].name, ftable.files[i].name + COMPIO_FNAME_MAX_SIZE);
            // size(8)
            push_u64(ftable.files[i].size);
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
    
    for (uint32_t i = 0; i < ftable.max_files; ++i) {
        lendian_fwrite(&ftable.files[i].name, 1, sizeof(ftable.files[i].name), file);
        lendian_fwrite_member(ftable.files[i].size, file);
    }
}

void index_node::read_from(FILE *file, uint64_t addr) {
    DEBUG_PRINT("[R][index_node]addr=%" PRIu64 "\n", addr);
    if (fseek64(file, addr, SEEK_SET))
        DEBUG_PRINT("warning: fseek failed\n");
    uint8_t signature;
    lendian_fread(&signature, sizeof(signature), 1, file);
    if (signature != index_node_signature) {
        WARNING_PRINT("warning: index_node signature does not match\n");
        assert(false);
    }
    lendian_fread_member(is_leaf, file);
    lendian_fread_member(num_keys, file);
    keys.resize(num_keys);
    values.resize(num_keys);
    if (!is_leaf) {
        children.resize(num_keys + 1);
        key_additions.resize(num_keys + 1);
    }

    for (auto &key : keys) {
        lendian_fread_member(key.hash, file);
        lendian_fread_member(key.pos, file);
    }
    for (auto &value : values) {
        lendian_fread_member(value.addr, file);
        lendian_fread_member(value.size, file);
    }
    if (!is_leaf) {
        lendian_fread(children.data(), sizeof(uint64_t), children.size(), file);
        lendian_fread(key_additions.data(), sizeof(int64_t), key_additions.size(), file);
    }
    validate();
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

void storage_block::read_from(FILE *file, uint64_t addr) {
    DEBUG_PRINT("[R][storage_block]addr=%" PRIu64 "\n", addr);
    assert(addr != 0);
    if (fseek64(file, addr, SEEK_SET))
        DEBUG_PRINT("warning: fseek failed\n");
    uint8_t signature;
    lendian_fread(&signature, sizeof(signature), 1, file);
    if (signature != storage_block_signature) {
        WARNING_PRINT("warning: storage_block signature does not match\n");
        assert(false);
    }
    lendian_fread_member(is_compressed, file);
    lendian_fread_member(size, file);
    assert(size != 0);

    // Cap block size to prevent OOM on corrupted files
    static constexpr uint64_t MAX_BLOCK_SIZE = 256ULL * 1024 * 1024; // 256 MB
    if (size > MAX_BLOCK_SIZE) {
        WARNING_PRINT("warning: storage_block size %llu exceeds limit at addr=%llu\n", 
                      (unsigned long long)size, (unsigned long long)addr);
        size = 0;
        return;
    }

    lendian_fread_member(original_size, file);
    lendian_fread_member(checksum, file);

    data = std::unique_ptr<uint8_t[]>(new uint8_t[size]);
    lendian_fread(data.get(), 1, size, file);

    if (!verify_checksum()) {
        WARNING_PRINT("warning: storage_block checksum verification failed at addr=%llu\n", (unsigned long long)addr);
    }
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

    push_u8(storage_block_signature);
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

storage_block::storage_block() : data(nullptr) {}

storage_block::storage_block(std::unique_ptr<uint8_t[]> &&data, uint64_t size)
    : is_compressed(0),
      size(size),
      original_size(0),
      data(std::move(data)) {}

storage_block::storage_block(uint64_t size)
    : storage_block(std::unique_ptr<uint8_t[]>(new uint8_t[size]), size) {}

files_table::files_table() : n_files(0), max_files(COMPIO_MAX_FILES), files(COMPIO_MAX_FILES) {}

files_table::files_table(uint32_t max_files)
    : n_files(0), max_files(max_files), files(max_files) {}

const files_table::file *files_table::find(const char *name) const {
    for (uint64_t i = 0; i < n_files; ++i)
        if (!strncmp(name, files[i].name, COMPIO_FNAME_MAX_SIZE))
            return &files[i];
    return NULL;
}

files_table::file *files_table::find(const char *name) {
    for (uint64_t i = 0; i < n_files; ++i)
        if (!strncmp(name, files[i].name, COMPIO_FNAME_MAX_SIZE))
            return &files[i];
    return NULL;
}

files_table::file *files_table::add(const char *name) {
    if (n_files >= max_files)
        return NULL;
    strncpy(files[n_files].name, name, COMPIO_FNAME_MAX_SIZE - 1);
    files[n_files].name[COMPIO_FNAME_MAX_SIZE - 1] = '\0';
    files[n_files].size = 0;
    return &files[n_files++];
}

int files_table::remove(const char *name) {
    for (uint64_t i = 0; i < n_files; ++i) {
        if (!strncmp(files[i].name, name, COMPIO_FNAME_MAX_SIZE)) {
            memmove(&files[i], &files[i + 1], (--n_files - i) * sizeof(files_table::file));
            return 0;
        }
    }
    return -1;
}

void storage_block::calculate_checksum() {
    if (!data || size == 0) {
        checksum = 0;
        return;
    }

    checksum = fnv1a_32(data.get(), size);
}

bool storage_block::verify_checksum() const {
    if (!data || size == 0) {
        return checksum == 0;
    }

    uint32_t computed = fnv1a_32(data.get(), size);
    return checksum == computed;
}
