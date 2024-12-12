#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "file.hpp"

using namespace compio;

header::header() : magic_number(0), file_size(sizeof(header)), index_root(0), ftable() {}

header::header(FILE* file) {
    fseek(file, 0, SEEK_SET);
    int count = fread(this, sizeof(header), 1, file);
    if (count < 1)
        throw std::runtime_error("Failed to read header from file");
}

index_node::index_node(int tree_degree)
    : is_leaf(true),
      num_keys(0),
      tree_degree(tree_degree),
      keys(2 * tree_degree - 1),
      values(2 * tree_degree - 1),
      children(2 * tree_degree) {}

index_node::index_node(FILE* file, uint64_t addr, int tree_degree) : index_node(tree_degree) {
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

    printf("[LOAD] ADDR: %ld\n", addr);
    printf("\tkeys: ");
    for (auto el : keys)
        printf("%ld, ", el);
    printf("\n");
    printf("\tvalues: ");
    for (auto el : values)
        printf("%ld(%ld), ", el.addr, el.size);
    printf("\n");
    printf("\tchildren: ");
    for (auto el : children)
        printf("%ld, ", el);
    printf("\n");
}

storage_block::storage_block() : is_compressed(0), size(0), original_size(0), index_key(0) {}

storage_block::storage_block(FILE* file, uint64_t addr) {
    if (fseek(file, addr, SEEK_SET))
        throw std::runtime_error("Invalid addr while reading storage block from file");

    if (fread(this, STORAGE_BLOCK_METASIZE, 1, file) < 1)
        throw std::runtime_error("Failed to read storage block metadata from file");

    data.resize(size);
    int count = fread(data.data(), sizeof(uint8_t), size, file);
    if (count < size)
        throw std::runtime_error("Failed to read storage block data from file");
}

void header::write(FILE* file) {
    fseek(file, 0, SEEK_SET);
    fwrite(this, sizeof(header), 1, file);
}

void index_node::write(FILE* file, uint64_t addr) {
    if (fseek(file, addr, SEEK_SET))
        throw std::runtime_error("Invalid addr while reading index node from file");

    if (fwrite(this, INDEX_NODE_METASIZE, 1, file) < 1)
        throw std::runtime_error("Failed to write index node metadata to file");

    printf("[SAVE] ADDR: %ld\n", addr);
    printf("\tkeys: ");
    for (auto el : keys)
        printf("%ld, ", el);
    printf("\n");
    printf("\tvalues: ");
    for (auto el : values)
        printf("%ld(%ld), ", el.addr, el.size);
    printf("\n");
    printf("\tchildren: ");
    for (auto el : children)
        printf("%ld, ", el);
    printf("\n");

    int count = 0;
    count += fwrite(keys.data(), sizeof(tree_key), keys.size(), file);
    count += fwrite(values.data(), sizeof(tree_val), values.size(), file);
    count += fwrite(children.data(), sizeof(uint64_t), children.size(), file);

    if (count < keys.size() + values.size() + children.size())
        throw std::runtime_error("Failed to write index node data to file");
}

void storage_block::write(FILE* file, uint64_t addr) {
    if (fseek(file, addr, SEEK_SET))
        throw std::runtime_error("Invalid addr while reading storage block from file");

    if (fwrite(this, STORAGE_BLOCK_METASIZE, 1, file) < 1)
        throw std::runtime_error("Failed to write storage block metadata to file");

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