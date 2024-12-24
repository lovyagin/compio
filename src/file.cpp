#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "file.hpp"

using namespace compio;

header::header() : magic_number(0), file_size(sizeof(header)), index_root(0), ftable() {}

header::header(FILE* file, bool swap_endianness) {
    fseek(file, 0, SEEK_SET);
    int count = fread(this, sizeof(header), 1, file);
    if (count < 1)
        throw std::runtime_error("Failed to read header from file");
    if (swap_endianness)
        _swap_endianness(false);
}

index_node::index_node(int tree_degree)
    : is_leaf(true),
      num_keys(0),
      tree_degree(tree_degree),
      keys(2 * tree_degree - 1),
      values(2 * tree_degree - 1),
      children(2 * tree_degree) {}

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

    printf("[LOAD] ADDR: %ld\n", addr);
    printf("\tkeys: ");
    for (int i = 0; i < num_keys; ++i)
        printf("%ld-%ld, ", keys[i].hash, keys[i].pos);
    printf("\n");
    printf("\tvalues: ");
    for (int i = 0; i < num_keys; ++i)
        printf("%ld(%ld), ", values[i].addr, values[i].size);
    printf("\n");
    printf("\tchildren: ");
    for (int i = 0; i <= num_keys; ++i)
        printf("%ld, ", children[i]);
    printf("\n");
}

storage_block::storage_block(uint64_t size)
    : is_compressed(0),
      size(size),
      original_size(0),
      index_key({0, 0}),
      data(size) {}

storage_block::storage_block(FILE* file, uint64_t addr, bool swap_endianness) {
    if (fseek(file, addr, SEEK_SET))
        throw std::runtime_error("Invalid addr while reading storage block from file");

    if (fread(this, STORAGE_BLOCK_METASIZE, 1, file) < 1)
        throw std::runtime_error("Failed to read storage block metadata from file");

    if (swap_endianness)
        _swap_endianness(false);

    data.resize(size);
    int count = fread(data.data(), sizeof(uint8_t), size, file);
    if (count < size)
        throw std::runtime_error("Failed to read storage block data from file");
}

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

    printf("[SAVE] ADDR: %ld\n", addr);
    printf("\tkeys: ");
    for (int i = 0; i < num_keys; ++i)
        printf("%ld-%ld, ", keys[i].hash, keys[i].pos);
    printf("\n");
    printf("\tvalues: ");
    for (int i = 0; i < num_keys; ++i)
        printf("%ld(%ld), ", values[i].addr, values[i].size);
    printf("\n");
    printf("\tchildren: ");
    for (int i = 0; i <= num_keys; ++i)
        printf("%ld, ", children[i]);
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

files_table::files_table() : n_files(0) { memset(files, 0, sizeof(files)); }

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

void files_table::_swap_endianness(bool from_valid) {
    if (!from_valid)
        n_files = __bswap_64(n_files);
    for (int i = 0; i < n_files; ++i)
        files[i].size = __bswap_64(files[i].size);
    if (from_valid)
        n_files = __bswap_64(n_files);
}

void header::_swap_endianness(bool from_valid) {
    magic_number = __bswap_32(magic_number);
    file_size = __bswap_64(file_size);
    index_root = __bswap_64(index_root);
    ftable._swap_endianness(from_valid);
}

void index_node::_swap_endianness(bool from_valid) {
    num_keys = __bswap_32(num_keys);
    for (int i = 0; i < keys.size(); ++i) {
        keys[i].hash = __bswap_64(keys[i].hash);
        keys[i].pos = __bswap_64(keys[i].pos);
    }
    for (int i = 0; i < values.size(); ++i) {
        values[i].addr = __bswap_64(values[i].addr);
        values[i].size = __bswap_64(values[i].size);
    }
    for (int i = 0; i < children.size(); ++i) {
        children[i] = __bswap_64(children[i]);
    }
}

void storage_block::_swap_endianness(bool from_valid) {
    size = __bswap_64(size);
    original_size = __bswap_64(original_size);
    index_key.hash = __bswap_64(index_key.hash);
    index_key.pos = __bswap_64(index_key.pos);
}