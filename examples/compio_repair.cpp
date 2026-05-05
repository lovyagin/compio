#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

#include "compio/btree.hpp"
#include "compio/compio_file.hpp" // for compio_archive definition
#include "compio.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <archive_path>\n";
        return 1;
    }

    const char *path = argv[1];

    // Create a default configuration (cannot pass nullptr)
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.block_size = 4096 / 4;
    cfg.block_size__maximum = 4096;
    cfg.block_size__minimum = 4096 / 4 / 4;
    // For read‑only access, no need to adjust allocation strategy etc.

    // Open archive read‑only
    compio_archive *arch = compio_open_archive(path, "r", &cfg);
    if (!arch) {
        std::perror("compio_open_archive");
        return 1;
    }

    // Access internal B‑tree pointer
    compio::btree *index = arch->index;
    if (!index) {
        std::cerr << "Archive has no B‑tree index (corrupted?)\n";
        compio_close_archive(arch);
        return 1;
    }

    // Retrieve all blocks using the two‑field tree_key: (pos, offset)
    auto result =
        index->get_range(compio::tree_key{0, 0}, compio::tree_key{UINT64_MAX, UINT64_MAX});
    if (!result) {
        std::cerr << "Failed to read B‑tree\n";
        compio_close_archive(arch);
        return 1;
    }

    // Count blocks by their uncompressed size (tree_val::size)
    std::map<uint64_t, size_t> size_counts;
    for (const auto &kv : *result) {
        uint64_t block_size = kv.second.size; // tree_val.size is u64
        size_counts[block_size]++;
    }

    // Print statistics
    std::cout << "Block size counts (uncompressed bytes):\n";
    for (const auto &[size, count] : size_counts) {
        std::cout << "  " << size << " bytes : " << count << " block(s)\n";
    }
    std::cout << "Total blocks: " << result->size() << "\n";

    compio_close_archive(arch);
    return 0;
}