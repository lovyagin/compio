#include "compio/file.hpp"

#include <cassert>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "compio/debug_print.hpp"
#include "compio.h"

using namespace compio;

#define lendian_fread_member(memb, file) lendian_fread(&(memb), sizeof(memb), 1, (file))
#define lendian_fwrite_member(memb, file) lendian_fwrite(&(memb), sizeof(memb), 1, (file))

static const uint8_t index_node_signature = 67;
static const uint8_t storage_block_signature = 171;

header::header()
    : magic_number(27110654),
      index_root(0),
      file_size(sizeof(header)),
      ftable(),
      allocator_state_offset(0),
      allocator_state_size(0),
      compression_type(COMPIO_COMPRESS_ZLIB) {}

void header::read_from(FILE *file, uint64_t addr) {
    DEBUG_PRINT("[R][header]addr=%lu\n", addr);
    if (fseek(file, addr, SEEK_SET))
        DEBUG_PRINT("warning: fseek failed\n");
    lendian_fread_member(magic_number, file);
    if (magic_number != COMPIO_MAGIC_NUMBER) {
        WARNING_PRINT("warning: header magic_number does not match\n");
        assert(false);
    }
    lendian_fread_member(index_root, file);
    lendian_fread_member(file_size, file);
    lendian_fread_member(allocator_state_offset, file);
    lendian_fread_member(allocator_state_size, file);
    lendian_fread_member(compression_type, file);
    lendian_fread_member(ftable.n_files, file);
    for (int i = 0; i < COMPIO_MAX_FILES; ++i) {
        lendian_fread(&ftable.files[i].name, 1, sizeof(ftable.files[i].name), file);
        lendian_fread_member(ftable.files[i].size, file);
    }
}

void header::write_to(FILE *file, uint64_t addr) const {
    DEBUG_PRINT("[W][header]addr=%lu;size=%lu\n", addr, sizeof(header));
    if (fseek(file, addr, SEEK_SET))
        DEBUG_PRINT("warning: fseek failed\n");
    lendian_fwrite_member(magic_number, file);
    lendian_fwrite_member(index_root, file);
    lendian_fwrite_member(file_size, file);
    lendian_fwrite_member(allocator_state_offset, file);
    lendian_fwrite_member(allocator_state_size, file);
    lendian_fwrite_member(compression_type, file);
    lendian_fwrite_member(ftable.n_files, file);
    for (int i = 0; i < COMPIO_MAX_FILES; ++i) {
        lendian_fwrite(&ftable.files[i].name, 1, sizeof(ftable.files[i].name), file);
        lendian_fwrite_member(ftable.files[i].size, file);
    }
}

void index_node::read_from(FILE *file, uint64_t addr) {
    DEBUG_PRINT("[R][index_node]addr=%lu\n", addr);
    if (fseek(file, addr, SEEK_SET))
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

void index_node::write_to(FILE *file, uint64_t addr) const {
    DEBUG_PRINT("[W][index_node]addr=%lu;size=%lu\n", addr, INDEX_NODE_SIZE(tree_degree));
    validate();
    if (fseek(file, addr, SEEK_SET))
        DEBUG_PRINT("warning: fseek failed\n");
    lendian_fwrite(&index_node_signature, sizeof(index_node_signature), 1, file);
    lendian_fwrite_member(is_leaf, file);
    const uint32_t actual_num_keys = keys.size();
    assert(num_keys == actual_num_keys);
    lendian_fwrite_member(actual_num_keys, file);
    for (auto &key : keys) {
        lendian_fwrite_member(key.hash, file);
        lendian_fwrite_member(key.pos, file);
    }
    for (auto &value : values) {
        lendian_fwrite_member(value.addr, file);
        lendian_fwrite_member(value.size, file);
    }
    if (!is_leaf) {
        lendian_fwrite(children.data(), sizeof(uint64_t), children.size(), file);
        lendian_fwrite(key_additions.data(), sizeof(int64_t), key_additions.size(), file);
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
                    DEBUG_PRINT("\t{%lu,%lu} -> {%lu,%lu}\n", keys[j].hash, keys[j].pos,
                                values[j].addr, values[j].size);
                }
            }
            assert(prev_end <= keys[i].pos);
        }
    }
#endif
}

void storage_block::read_from(FILE *file, uint64_t addr) {
    DEBUG_PRINT("[R][storage_block]addr=%lu\n", addr);
    assert(addr != 0);
    if (fseek(file, addr, SEEK_SET))
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
    lendian_fread_member(original_size, file);
    data = std::unique_ptr<uint8_t[]>(new uint8_t[size]);
    lendian_fread(data.get(), 1, size, file);
}

void storage_block::write_to(FILE *file, uint64_t addr) const {
    DEBUG_PRINT("[W][storage_block]addr=%lu;size=%lu\n", addr, STORAGE_BLOCK_METASIZE + size);
    assert(addr != 0);
    assert(size > 0);
    assert(original_size > 0);
    assert(is_compressed || size == original_size);
    if (fseek(file, addr, SEEK_SET))
        DEBUG_PRINT("warning: fseek failed\n");
    lendian_fwrite(&storage_block_signature, sizeof(storage_block_signature), 1, file);
    lendian_fwrite_member(is_compressed, file);
    lendian_fwrite_member(size, file);
    lendian_fwrite_member(original_size, file);
    lendian_fwrite(data.get(), 1, size, file);
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

files_table::files_table() : n_files(0) {}

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
    if (n_files >= COMPIO_MAX_FILES)
        return NULL;
    strncpy(files[n_files].name, name, COMPIO_FNAME_MAX_SIZE);
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