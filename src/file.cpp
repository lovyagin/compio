#include "compio/file.hpp"
#include "compio/wal.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <inttypes.h>
#include <unordered_set>
#include <vector>

#include "compio/debug_print.hpp"
#include "compio/utils.hpp"
#include "compio.h"

using namespace compio;

#define lendian_fread_member(memb, file) lendian_fread(&(memb), sizeof(memb), 1, (file))
#define lendian_fwrite_member(memb, file) lendian_fwrite(&(memb), sizeof(memb), 1, (file))

static const uint8_t index_node_signature = 68; // 67: nodes without a checksum

static void put_le(std::vector<uint8_t> &out, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

static uint64_t get_le(const uint8_t *in, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) value |= static_cast<uint64_t>(in[i]) << (8 * i);
    return value;
}

uint64_t files_table::serialized_size() const {
    uint64_t size = 0;
    for (const auto &f : files) size += record_header_size + f.name.size();
    return size;
}

std::vector<uint8_t> files_table::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(serialized_size()));
    for (const auto &f : files) {
        put_le(out, f.name.size(), sizeof(uint16_t));
        put_le(out, f.size, sizeof(uint64_t));
        put_le(out, f.file_id, sizeof(uint64_t));
        out.insert(out.end(), f.name.begin(), f.name.end());
    }
    return out;
}

bool files_table::parse(const uint8_t *data, uint64_t size, uint64_t count) {
    std::vector<file> parsed;
    parsed.reserve(static_cast<size_t>(std::min<uint64_t>(count, size / (record_header_size + 1))));
    std::unordered_set<uint64_t> ids;
    uint64_t pos = 0;
    while (pos < size) {
        if (size - pos < record_header_size) return false;
        const uint64_t name_len = get_le(data + pos, sizeof(uint16_t));
        file f;
        f.size = get_le(data + pos + 2, sizeof(uint64_t));
        f.file_id = get_le(data + pos + 10, sizeof(uint64_t));
        pos += record_header_size;
        if (name_len == 0 || name_len >= COMPIO_FNAME_MAX_SIZE || size - pos < name_len) return false;
        f.name.assign(reinterpret_cast<const char *>(data + pos), static_cast<size_t>(name_len));
        pos += name_len;
        // Names come from C strings; an embedded NUL could never be opened.
        if (f.name.find('\0') != std::string::npos) return false;
        if (!ids.insert(f.file_id).second) return false;
        parsed.push_back(std::move(f));
    }
    if (parsed.size() != count) return false;

    files = std::move(parsed);
    rebuild_index();
    return index_map_.size() == files.size(); // duplicate names
}

bool files_table::read_from(FILE *file, uint64_t addr, uint64_t size, uint64_t count,
                            uint32_t checksum) {
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size > 0) {
        if (fseek64(file, addr, SEEK_SET) != 0) {
            WARNING_PRINT("warning: fseek failed in files_table::read_from\n");
            return false;
        }
        if (fread(data.data(), 1, data.size(), file) != data.size()) {
            WARNING_PRINT("warning: short read in files table\n");
            return false;
        }
    }
    if (crc32c(data.data(), data.size()) != checksum) {
        WARNING_PRINT("warning: files table checksum mismatch\n");
        return false;
    }
    if (!parse(data.data(), size, count)) {
        WARNING_PRINT("warning: malformed files table\n");
        return false;
    }
    return true;
}

bool files_table::write_to(FILE *file, uint64_t addr) const {
    if (fseek64(file, addr, SEEK_SET) != 0) {
        WARNING_PRINT("warning: fseek failed in files_table::write_to\n");
        return false;
    }
    const std::vector<uint8_t> data = serialize();
    if (fwrite(data.data(), 1, data.size(), file) != data.size()) {
        WARNING_PRINT("warning: short write in files table\n");
        if (!errno) errno = EIO;
        return false;
    }
    return true;
}

header::header() : header(COMPIO_MAX_FILES) {}

header::header(uint32_t expected_files)
    : magic_number(COMPIO_MAGIC_NUMBER),
      index_root(0),
      file_size(0),
      ftable(expected_files),
      allocator_state_offset(0),
      allocator_state_size(0),
      compression_type(COMPIO_COMPRESS_ZLIB),
      block_size(0),
      b_tree_degree(0),
      sequence_id(0),
      files_table_addr(0),
      files_table_size(0),
      files_table_checksum(crc32c(nullptr, 0)),
      files_table_encoding(0) {
    memset(checksum, 0, sizeof(checksum));
    file_size = disk_size();
}

void header::compute_checksum(uint8_t *out_hash, uint64_t n_files) const {
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
    ctx.update(reinterpret_cast<const uint8_t*>(&files_table_addr), sizeof(files_table_addr));
    ctx.update(reinterpret_cast<const uint8_t*>(&files_table_size), sizeof(files_table_size));
    ctx.update(reinterpret_cast<const uint8_t*>(&n_files), sizeof(n_files));
    // The table itself is covered by its CRC, which the header hash covers in turn.
    ctx.update(reinterpret_cast<const uint8_t*>(&files_table_checksum), sizeof(files_table_checksum));
    ctx.update(reinterpret_cast<const uint8_t*>(&files_table_encoding), sizeof(files_table_encoding));
    ctx.finalize(out_hash);
}

bool header::load_and_validate(FILE *file, uint64_t addr) {
    DEBUG_PRINT("[R][header]addr=%" PRIu64 "\n", addr);
    if (fseek64(file, addr, SEEK_SET)) {
        DEBUG_PRINT("warning: fseek failed\n");
        return false;
    }

    if (lendian_fread_member(magic_number, file) != 1) return false;
    if (magic_number != COMPIO_MAGIC_NUMBER) {
        WARNING_PRINT("warning: unsupported archive format (magic %d, expected %d for format v6)\n",
                      magic_number, COMPIO_MAGIC_NUMBER);
        return false;
    }

    uint64_t n_files = 0;
    const bool complete =
        lendian_fread(checksum, 1, sizeof(checksum), file) == sizeof(checksum) &&
        lendian_fread_member(sequence_id, file) == 1 &&
        lendian_fread_member(index_root, file) == 1 &&
        lendian_fread_member(file_size, file) == 1 &&
        lendian_fread_member(allocator_state_offset, file) == 1 &&
        lendian_fread_member(allocator_state_size, file) == 1 &&
        lendian_fread_member(compression_type, file) == 1 &&
        lendian_fread_member(block_size, file) == 1 &&
        lendian_fread_member(b_tree_degree, file) == 1 &&
        lendian_fread_member(files_table_addr, file) == 1 &&
        lendian_fread_member(files_table_size, file) == 1 &&
        lendian_fread_member(n_files, file) == 1 &&
        lendian_fread_member(files_table_checksum, file) == 1 &&
        lendian_fread_member(files_table_encoding, file) == 1;
    if (!complete) return false;

    uint8_t calc_checksum[32];
    compute_checksum(calc_checksum, n_files);
    if (memcmp(checksum, calc_checksum, 32) != 0) {
        WARNING_PRINT("warning: header checksum mismatch\n");
        return false;
    }
    if (n_files > COMPIO_MAX_FILES_LIMIT) {
        WARNING_PRINT("warning: header declares %" PRIu64 " files\n", n_files);
        return false;
    }

    if (files_table_encoding != 0) {
        WARNING_PRINT("warning: unsupported files table encoding %u\n", files_table_encoding);
        return false;
    }
    if ((files_table_addr == 0) != (files_table_size == 0) ||
        files_table_size < n_files * (files_table::record_header_size + 1) ||
        files_table_size > n_files * (files_table::record_header_size + COMPIO_FNAME_MAX_SIZE - 1)) {
        WARNING_PRINT("warning: files table bounds are inconsistent with %" PRIu64 " files\n", n_files);
        return false;
    }
    return ftable.read_from(file, files_table_addr, files_table_size, n_files, files_table_checksum);
}

bool header::read_from(FILE *file, uint64_t addr) {
    return load_and_validate(file, addr);
}

// The header is never journaled: its two slots already make the update atomic.
bool header::write_to(FILE *file, uint64_t addr, compio::WalManager*) const {
    return write_checked(file, addr);
}

bool header::write_checked(FILE *file, uint64_t addr) const {
    if (ferror(file) || fseek64(file, addr, SEEK_SET) != 0) return false;

    const uint64_t n_files = ftable.files.size();
    const_cast<header*>(this)->compute_checksum(const_cast<uint8_t*>(checksum), n_files);
    return lendian_fwrite_member(magic_number, file) == 1 &&
           lendian_fwrite(checksum, 1, sizeof(checksum), file) == sizeof(checksum) &&
           lendian_fwrite_member(sequence_id, file) == 1 &&
           lendian_fwrite_member(index_root, file) == 1 &&
           lendian_fwrite_member(file_size, file) == 1 &&
           lendian_fwrite_member(allocator_state_offset, file) == 1 &&
           lendian_fwrite_member(allocator_state_size, file) == 1 &&
           lendian_fwrite_member(compression_type, file) == 1 &&
           lendian_fwrite_member(block_size, file) == 1 &&
           lendian_fwrite_member(b_tree_degree, file) == 1 &&
           lendian_fwrite_member(files_table_addr, file) == 1 &&
           lendian_fwrite_member(files_table_size, file) == 1 &&
           lendian_fwrite_member(n_files, file) == 1 &&
           lendian_fwrite_member(files_table_checksum, file) == 1 &&
           lendian_fwrite_member(files_table_encoding, file) == 1 && !ferror(file);
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

    if (is_leaf > 1) {
        WARNING_PRINT("error: index_node is_leaf invalid (%u) at addr=%" PRIu64 "\n", is_leaf, addr);
        return false;
    }

    const size_t body_size = sizeof(tree_key) * num_keys + sizeof(tree_val) * num_keys +
                             (is_leaf ? 0 : (sizeof(uint64_t) + sizeof(int64_t)) * (num_keys + 1));
    std::vector<uint8_t> body(body_size);
    if (body_size > 0 && fread(body.data(), 1, body_size, file) != body_size) return false;
    uint8_t crc_buf[4];
    if (fread(crc_buf, 1, sizeof(crc_buf), file) != sizeof(crc_buf)) return false;
    const uint32_t stored_crc = static_cast<uint32_t>(crc_buf[0]) | (static_cast<uint32_t>(crc_buf[1]) << 8) |
                                (static_cast<uint32_t>(crc_buf[2]) << 16) | (static_cast<uint32_t>(crc_buf[3]) << 24);
    if (stored_crc != crc32c_continue(crc32c(header_buf, sizeof(header_buf)), body.data(), body.size())) {
        WARNING_PRINT("warning: index_node checksum mismatch at addr=%" PRIu64 "\n", addr);
        return false;
    }

    keys.resize(num_keys);
    values.resize(num_keys);
    if (!is_leaf) {
        children.resize(num_keys + 1);
        key_additions.resize(num_keys + 1);
    }

    bool is_be = is_big_endian();
    const uint8_t *cursor = body.data();
    auto take = [&](void *dst, size_t bytes) {
        std::memcpy(dst, cursor, bytes);
        cursor += bytes;
    };

    if (num_keys > 0) {
        take(keys.data(), sizeof(tree_key) * num_keys);
        take(values.data(), sizeof(tree_val) * num_keys);
        if (is_be) {
            for (auto &key : keys) {
                swap_uint64(&key.hash);
                swap_uint64(&key.pos);
            }
            for (auto &val : values) {
                swap_uint64(&val.addr);
                swap_uint64(&val.size);
            }
        }
    }

    if (!is_leaf) {
        take(children.data(), sizeof(uint64_t) * children.size());
        take(key_additions.data(), sizeof(int64_t) * key_additions.size());
        if (is_be) {
            for (auto &child : children) swap_uint64(&child);
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

bool index_node::write_to(FILE *file, uint64_t addr, compio::WalManager* wal_manager) const {
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

    push_u32(crc32c(buffer.data(), buffer.size()));

    if (wal_manager) {
        wal_manager->begin_transaction();
        if (!wal_manager->log_write(WalRecordType::INDEX_NODE, addr, buffer.data(), buffer.size())) {
            wal_manager->rollback_transaction();
            if (!errno) errno = EIO;
            return false;
        }
        if (!wal_manager->commit_transaction()) {
            if (!errno) errno = EIO;
            return false;
        }
    }

    clearerr(file);
    if (fseek64(file, addr, SEEK_SET) != 0) return false;
    if (fwrite(buffer.data(), 1, buffer.size(), file) != buffer.size() || ferror(file)) {
        if (!errno) errno = EIO;
        return false;
    }
    return fflush(file) == 0;
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

            // not performing this assert because it fails in compio_erase post-merge when growing
            // the left block before shrinking/removing the right one
            //
            // assert(prev_end <= keys[i].pos);
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

    uint8_t meta_buffer[STORAGE_BLOCK_METASIZE];
    if (lendian_fread(meta_buffer, 1, sizeof(meta_buffer), file) != sizeof(meta_buffer)) {
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

    const uint8_t sig = read_u8();
    if (sig == storage_block::signature) {
        checksum_type = COMPIO_CHECKSUM_FNV1A;
    } else if (sig == storage_block::signature_crc32c) {
        checksum_type = COMPIO_CHECKSUM_CRC32C;
    } else {
        WARNING_PRINT("warning: storage_block signature does not match (got %d)\n", sig);
        return false;
    }

    codec = read_u32();

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
    src_key.hash = read_u64();
    src_key.pos = read_u64();
    checksum = read_u32();
    data_checksum = read_u32();

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

bool storage_block::write_to(FILE *file, uint64_t addr, compio::WalManager* wal_manager) const {
    DEBUG_PRINT("[W][storage_block]addr=%" PRIu64 ";size=%" PRIu64 "\n", addr, STORAGE_BLOCK_METASIZE + size);
    assert(addr != 0);
    assert(size > 0);
    assert(original_size > 0);
    assert(is_compressed() || size == original_size);

    // Calculate checksum before writing (non-const, so we cast)
    const_cast<storage_block*>(this)->calculate_checksum();

    uint8_t meta_buffer[STORAGE_BLOCK_METASIZE];
    size_t meta_idx = 0;

    auto push_u8 = [&](uint8_t v) { meta_buffer[meta_idx++] = v; };
    auto push_u32 = [&](uint32_t v) {
        for(int i=0; i<4; ++i) meta_buffer[meta_idx++] = static_cast<uint8_t>(v >> (i*8));
    };
    auto push_u64 = [&](uint64_t v) {
        for(int i=0; i<8; ++i) meta_buffer[meta_idx++] = static_cast<uint8_t>(v >> (i*8));
    };

    push_u8(checksum_type == COMPIO_CHECKSUM_CRC32C ? storage_block::signature_crc32c : storage_block::signature);
    push_u32(codec);
    push_u64(size);
    push_u64(original_size);
    push_u64(src_key.hash);
    push_u64(src_key.pos);
    push_u32(checksum);
    push_u32(data_checksum);

    assert(meta_idx == STORAGE_BLOCK_METASIZE);

    if (wal_manager) {
        wal_manager->begin_transaction();
        std::vector<compio::WalManager::iovec_buf> buffers;
        buffers.push_back({meta_buffer, STORAGE_BLOCK_METASIZE});
        if (size > 0 && data) buffers.push_back({data.get(), size});
        if (!wal_manager->log_write_vectored(WalRecordType::BLOCK, addr, buffers)) {
            wal_manager->rollback_transaction();
            if (!errno) errno = EIO;
            return false;
        }
        if (!wal_manager->commit_transaction()) {
            if (!errno) errno = EIO;
            return false;
        }
    }

    clearerr(file);
    if (fseek64(file, addr, SEEK_SET) != 0) return false;
    if (fwrite(meta_buffer, 1, STORAGE_BLOCK_METASIZE, file) != STORAGE_BLOCK_METASIZE ||
        (size > 0 && data && fwrite(data.get(), 1, size, file) != size) || ferror(file)) {
        if (!errno) errno = EIO;
        return false;
    }
    return fflush(file) == 0;
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

storage_block::storage_block()
    : codec(codec_stored), size(0), original_size(0), data(nullptr), checksum(0), data_checksum(0),
      checksum_type(COMPIO_CHECKSUM_FNV1A) {}

storage_block::storage_block(std::unique_ptr<uint8_t[]> &&data, uint64_t size)
    : codec(codec_stored),
      size(size),
      original_size(0),
      data(std::move(data)),
      checksum(0),
      data_checksum(0),
      checksum_type(COMPIO_CHECKSUM_FNV1A) {}

storage_block::storage_block(uint64_t size)
    : storage_block(std::unique_ptr<uint8_t[]>(new uint8_t[size]), size) {}

files_table::files_table(uint32_t expected_files) {
    files.reserve(expected_files);
}

files_table::files_table(const files_table& other)
    : next_file_id(other.next_file_id), files(other.files) {
    rebuild_index();
}

files_table& files_table::operator=(const files_table& other) {
    if (this != &other) {
        next_file_id = other.next_file_id;
        files = other.files;
        rebuild_index();
    }
    return *this;
}

void files_table::rebuild_index() {
    index_map_.clear();
    index_map_.reserve(files.size());
    uint64_t max_id = 0;
    for (uint32_t i = 0; i < files.size(); ++i) {
        const std::string_view key(files[i].name);
        if (!index_map_.find(key)) index_map_.emplace(key, i);
        max_id = std::max(max_id, files[i].file_id);
    }
    // Ids are never reused, so a reopened table continues after the largest one.
    if (next_file_id <= max_id) next_file_id = max_id + 1;
}

const files_table::file *files_table::find(const char *name) const {
    if (!name) return nullptr;
    const uint32_t *index = index_map_.find(std::string_view(name));
    return index ? &files[*index] : nullptr;
}

files_table::file *files_table::find(const char *name) {
    return const_cast<files_table::file *>(static_cast<const files_table *>(this)->find(name));
}

files_table::file *files_table::add(const char *name) {
    if (!name) return nullptr;
    if (file *existing = find(name)) return existing;
    if (files.size() >= COMPIO_MAX_FILES_LIMIT) return nullptr;

    const bool reallocates = files.size() == files.capacity();
    files.push_back({name, 0, next_file_id++});
    if (reallocates) {
        rebuild_index();
    } else {
        index_map_.emplace(std::string_view(files.back().name),
                           static_cast<uint32_t>(files.size() - 1));
    }
    return &files.back();
}

int files_table::remove(const char *name) {
    if (!name) return -1;
    const uint32_t *found = index_map_.find(std::string_view(name));
    if (!found) return -1;

    const uint32_t i = *found;
    const uint32_t last = static_cast<uint32_t>(files.size() - 1);
    index_map_.erase(std::string_view(files[i].name));
    if (i != last) {
        // Fill the hole with the last entry; its key must point at its new place.
        index_map_.erase(std::string_view(files[last].name));
        files[i] = std::move(files[last]);
        index_map_.emplace(std::string_view(files[i].name), i);
    }
    files.pop_back();
    return 0;
}

// The owning key is serialized little-endian, so that the checksum covers the same
// bytes on every platform.
static void key_bytes(const tree_key &key, uint8_t out[16]) {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(key.hash >> (i * 8));
    for (int i = 0; i < 8; ++i) out[8 + i] = static_cast<uint8_t>(key.pos >> (i * 8));
}

uint32_t storage_block::compute_checksum() const {
    uint8_t kb[16];
    key_bytes(src_key, kb);
    if (checksum_type == COMPIO_CHECKSUM_CRC32C) {
        return crc32c_continue(crc32c(kb, sizeof(kb)), data.get(), size);
    }
    return fnv1a_32_continue(fnv1a_32_continue(0x811c9dc5, kb, sizeof(kb)), data.get(), size);
}

uint32_t storage_block::compute_data_checksum(compio_checksum_type type, const uint8_t *plain, uint64_t plain_size) {
    if (type == COMPIO_CHECKSUM_CRC32C) return crc32c(plain, plain_size);
    return fnv1a_32_continue(0x811c9dc5, plain, plain_size);
}

void storage_block::calculate_checksum() {
    if (!data || size == 0) {
        checksum = 0;
        return;
    }
    checksum = compute_checksum();
}

bool storage_block::verify_checksum() const {
    if (!data || size == 0) {
        return checksum == 0;
    }
    return checksum == compute_checksum();
}
