#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "file.hpp"

using namespace compio;

header::header() : magic_number(0), index_root(0), ftable() {}

header::header(FILE* file) {
    fseek(file, 0, SEEK_SET);
    int count = fread(this, sizeof(header), 1, file);
    if (count < 1)
        throw std::runtime_error("Failed to read header from file");
}

index_node::index_node(int tree_order)
    : is_leaf(true), n_keys(0), tree_order(tree_order), keys(new uint64_t[tree_order]),
      blocks(new uint64_t[tree_order]), children(new uint64_t[tree_order]) {}

index_node::index_node(FILE* file, uint64_t addr, int tree_order)
    : tree_order(tree_order), keys(new uint64_t[tree_order]), blocks(new uint64_t[tree_order]),
      children(new uint64_t[tree_order]) {

    if (fseek(file, addr, SEEK_SET))
        throw std::runtime_error("Invalid addr while reading index node from file");

    int count = fread(this, INDEX_NODE_METASIZE, 1, file);
    if (count < 1)
        throw std::runtime_error("Failed to read index node metadata from file");

    count = fread(keys, sizeof(uint64_t), tree_order, file);
    if (count < tree_order)
        throw std::runtime_error("Failed to read index node keys from file");

    count = fread(blocks, sizeof(uint64_t), tree_order, file);
    if (count < tree_order)
        throw std::runtime_error("Failed to read index node blocks from file");

    count = fread(children, sizeof(uint64_t), tree_order, file);
    if (count < tree_order)
        throw std::runtime_error("Failed to read index node children from file");
}

index_node::~index_node() {
    delete keys;
    delete blocks;
    delete children;
}

storage_block::storage_block()
    : is_compressed(0), size(0), original_size(0), index_key(0), data(0) {}

storage_block::storage_block(FILE* file, uint64_t addr) {
    if (fseek(file, addr, SEEK_SET))
        throw std::runtime_error("Invalid addr while reading storage block from file");

    // size of metadata (independent of tree_order)
    int count = fread(this, STORAGE_BLOCK_METASIZE, 1, file);
    if (count < 1)
        throw std::runtime_error("Failed to read storage block metadata from file");
    // read data, as we already know its size
    data = new uint8_t[size];
    count = fread(data, sizeof(uint8_t), size, file);
    if (count < size)
        throw std::runtime_error("Failed to read storage block data from file");
}

storage_block::~storage_block() { delete data; }

void header::write(FILE* file) {
    fseek(file, SEEK_SET, 0);
    fwrite(this, sizeof(header), 1, file);
}

void index_node::write(FILE* file, uint64_t addr) {
    fseek(file, SEEK_SET, addr);
    int count = fwrite(this, INDEX_NODE_METASIZE, 1, file);
    if (count < 1)
        throw std::runtime_error("Failed to write index node metadata to file");
    count = fwrite(keys, sizeof(uint64_t), tree_order, file);
    if (count < tree_order)
        throw std::runtime_error("Failed to write index node keys to file");
    count = fwrite(blocks, sizeof(uint64_t), tree_order, file);
    if (count < tree_order)
        throw std::runtime_error("Failed to write index node blocks to file");
    count = fwrite(children, sizeof(uint64_t), tree_order, file);
    if (count < tree_order)
        throw std::runtime_error("Failed to write index node children to file");
}

void storage_block::write(FILE* file, uint64_t addr) {
    fseek(file, SEEK_SET, addr);
    int count = fwrite(this, STORAGE_BLOCK_METASIZE, 1, file);
    if (count < 1)
        throw std::runtime_error("Failed to write storage block metadata to file");
    count = fwrite(data, sizeof(uint8_t), size, file);
    if (count < size)
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