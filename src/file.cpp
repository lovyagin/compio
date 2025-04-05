#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <stdexcept>

#include "file.hpp"

using namespace compio;

#define lendian_fread_member(memb, file) lendian_fread(&(memb), sizeof(memb), 1, (file))
#define lendian_fwrite_member(memb, file) lendian_fwrite(&(memb), sizeof(memb), 1, (file))

header::header() : magic_number(0), file_size(sizeof(header)), index_root(0), ftable() {}

void header::read_from(FILE* file, uint64_t addr) {
    fprintf(stderr, "[R][header]addr=%llu\n", addr);
    if (fseek(file, addr, SEEK_SET))
        fprintf(stderr, "warning: fseek failed\n");
    lendian_fread_member(magic_number, file);
    lendian_fread_member(index_root, file);
    lendian_fread_member(file_size, file);
    lendian_fread_member(ftable.n_files, file);
    for (int i = 0; i < COMPIO_MAX_FILES; ++i) {
        lendian_fread(&ftable.files[i].name, 1, sizeof(ftable.files[i].name), file);
        lendian_fread_member(ftable.files[i].size, file);
    }
}

void header::write_to(FILE* file, uint64_t addr) const {
    fprintf(stderr, "[W][header]addr=%llu\n", addr);
    if (fseek(file, addr, SEEK_SET))
        fprintf(stderr, "warning: fseek failed\n");
    lendian_fwrite_member(magic_number, file);
    lendian_fwrite_member(index_root, file);
    lendian_fwrite_member(file_size, file);
    lendian_fwrite_member(ftable.n_files, file);
    for (int i = 0; i < COMPIO_MAX_FILES; ++i) {
        lendian_fwrite(&ftable.files[i].name, 1, sizeof(ftable.files[i].name), file);
        lendian_fwrite_member(ftable.files[i].size, file);
    }
}

void index_node::read_from(FILE* file, uint64_t addr) {
    fprintf(stderr, "[R][index_node]addr=%llu\n", addr);
    if (fseek(file, addr, SEEK_SET))
        fprintf(stderr, "warning: fseek failed\n");
    lendian_fread_member(is_leaf, file);
    lendian_fread_member(num_keys, file);
    keys.resize(2 * tree_degree - 1);
    values.resize(2 * tree_degree - 1);
    children.resize(2 * tree_degree);
    for (int i = 0; i < 2 * tree_degree - 1; ++i) {
        lendian_fread_member(keys[i].hash, file);
        lendian_fread_member(keys[i].pos, file);
    }
    for (int i = 0; i < 2 * tree_degree - 1; ++i) {
        lendian_fread_member(values[i].addr, file);
        lendian_fread_member(values[i].size, file);
    }
    lendian_fread(children.data(), sizeof(uint64_t), 2 * tree_degree, file);
}

void index_node::write_to(FILE* file, uint64_t addr) const {
    fprintf(stderr, "[W][index_node]addr=%llu\n", addr);
    if (fseek(file, addr, SEEK_SET))
        fprintf(stderr, "warning: fseek failed\n");
    lendian_fwrite_member(is_leaf, file);
    lendian_fwrite_member(num_keys, file);
    for (int i = 0; i < 2 * tree_degree - 1; ++i) {
        lendian_fwrite_member(keys[i].hash, file);
        lendian_fwrite_member(keys[i].pos, file);
    }
    for (int i = 0; i < 2 * tree_degree - 1; ++i) {
        lendian_fwrite_member(values[i].addr, file);
        lendian_fwrite_member(values[i].size, file);
    }
    lendian_fwrite(children.data(), sizeof(uint64_t), 2 * tree_degree, file);
}

void storage_block::read_from(FILE* file, uint64_t addr) {
    fprintf(stderr, "[R][storage_block]addr=%llu\n", addr);
    if (fseek(file, addr, SEEK_SET))
        fprintf(stderr, "warning: fseek failed\n");
    lendian_fread_member(is_compressed, file);
    lendian_fread_member(size, file);
    lendian_fread_member(original_size, file);
    lendian_fread_member(index_key.hash, file);
    lendian_fread_member(index_key.pos, file);
    data.resize(size);
    lendian_fread(data.data(), 1, size, file);
}

void storage_block::write_to(FILE* file, uint64_t addr) const {
    fprintf(stderr, "[W][storage_block]addr=%llu\n", addr);
    if (fseek(file, addr, SEEK_SET))
        fprintf(stderr, "warning: fseek failed\n");
    lendian_fwrite_member(is_compressed, file);
    lendian_fwrite_member(size, file);
    lendian_fwrite_member(original_size, file);
    lendian_fwrite_member(index_key.hash, file);
    lendian_fwrite_member(index_key.pos, file);
    lendian_fwrite(data.data(), 1, size, file);
}

index_node::index_node(int tree_degree)
    : is_leaf(true),
      num_keys(0),
      tree_degree(tree_degree),
      keys(2 * tree_degree - 1),
      values(2 * tree_degree - 1),
      children(2 * tree_degree) {}

storage_block::storage_block() {}

  /*
index_node::index_node(FILE* file, uint64_t addr, bool swap_endianness, int tree_degree) : index_node(tree_degree) {
    if (fseek(file, addr, SEEK_SET))
        throw std::runtime_error("Invalid addr while reading index node from file");

    if (fread(this, INDEX_NODE_METASIZE, 1, file) < 1)
        throw std::runtime_error("Failed to read index node metadata from file");

    int count = 0;
    count += fread(keys.data(), sizeof(tree_key), keys.size(), file);
    count += fread(values.data(), sizeof(tree_val), values.size(), file);
    count += fread(children.data(), sizeof(uint64_t), children.size(), file);

    if (count < keys.size() + values.size() + children.size())
        throw std::runtime_error("Failed to read index node data from file");

    if (swap_endianness)
        _swap_endianness(false);

    printf("[LOAD] ADDR: %llu\n", addr);
    printf("\tkeys: ");
    for (int i = 0; i < num_keys; ++i)
        printf("%llu-%llu, ", keys[i].hash, keys[i].pos);
    printf("\n");
    printf("\tvalues: ");
    for (int i = 0; i < num_keys; ++i)
        printf("%llu(%llu), ", values[i].addr, values[i].size);
    printf("\n");
    printf("\tchildren: ");
    for (int i = 0; i <= num_keys; ++i)
        printf("%llu, ", children[i]);
    printf("\n");
}
*/


storage_block::storage_block(std::vector<uint8_t>&& data)
    : is_compressed(0),
      size(data.size()),
      original_size(0),
      index_key({0, 0}),
      data(data) {}

storage_block::storage_block(uint64_t size) : storage_block(std::vector<uint8_t>(size)) {}

files_table::files_table() : n_files(0) {}

const files_table::file* files_table::find(const char* name) const {
    for (int i = 0; i < n_files; ++i)
        if (!strncmp(name, files[i].name, COMPIO_FNAME_MAX_SIZE))
            return &files[i];
    return NULL;
/*
    data.resize(size);
    int count = fread(data.data(), sizeof(uint8_t), size, file);
    if (count < size)
        throw std::runtime_error("Failed to read storage block data from file");
*/        
}
/*
void header::write(FILE* file, bool swap_endianness) {
    fseek(file, 0, SEEK_SET);
    if (swap_endianness)
        _swap_endianness(true);
    fwrite(this, sizeof(header), 1, file);
    if (swap_endianness)
        _swap_endianness(false);
}

void index_node::write(FILE* file, uint64_t addr, bool swap_endianness) {
    if (fseek(file, addr, SEEK_SET))
        throw std::runtime_error("Invalid addr while reading index node from file");

    printf("[SAVE] ADDR: %llu\n", addr);
    printf("\tkeys: ");
    for (int i = 0; i < num_keys; ++i)
        printf("%llu-%llu, ", keys[i].hash, keys[i].pos);
    printf("\n");
    printf("\tvalues: ");
    for (int i = 0; i < num_keys; ++i)
        printf("%llu(%llu), ", values[i].addr, values[i].size);
    printf("\n");
    printf("\tchildren: ");
    for (int i = 0; i <= num_keys; ++i)
        printf("%llu, ", children[i]);
    printf("\n");

    if (swap_endianness)
        _swap_endianness(true);

    if (fwrite(this, INDEX_NODE_METASIZE, 1, file) < 1)
        throw std::runtime_error("Failed to write index node metadata to file");

    int count = 0;
    count += fwrite(keys.data(), sizeof(tree_key), 2 * tree_degree - 1, file);
    count += fwrite(values.data(), sizeof(tree_val), 2 * tree_degree - 1, file);
    count += fwrite(children.data(), sizeof(uint64_t), 2 * tree_degree, file);

    if (count < keys.size() + values.size() + children.size())
        throw std::runtime_error("Failed to write index node data to file");
    
    if (swap_endianness)
        _swap_endianness(false);
}

void storage_block::write(FILE* file, uint64_t addr, bool swap_endianness) {
    if (fseek(file, addr, SEEK_SET))
        throw std::runtime_error("Invalid addr while reading storage block from file");

    if (swap_endianness)
        _swap_endianness(true);
    if (fwrite(this, STORAGE_BLOCK_METASIZE, 1, file) < 1)
        throw std::runtime_error("Failed to write storage block metadata to file");
    if (swap_endianness)
        _swap_endianness(false);

    if (fwrite(data.data(), sizeof(uint8_t), size, file) < size)
        throw std::runtime_error("Failed to write storage block data to file");
}
*/


files_table::file* files_table::find(const char* name) {
    for (int i = 0; i < n_files; ++i)
        if (!strncmp(name, files[i].name, COMPIO_FNAME_MAX_SIZE))
            return &files[i];
    return NULL;
}

files_table::file* files_table::add(const char* name) {
    if (n_files >= COMPIO_MAX_FILES)
        return NULL;
    strncpy(files[n_files].name, name, COMPIO_FNAME_MAX_SIZE);
    return &files[n_files++];
}

int files_table::remove(const char* name) {
    for (int i = 0; i < n_files; ++i) {
        if (!strncmp(files[i].name, name, COMPIO_FNAME_MAX_SIZE)) {
            memmove(&files[i], &files[i + 1], (--n_files - i) * sizeof(files_table::file));
            return 0;
        }
    }
    return -1;
}