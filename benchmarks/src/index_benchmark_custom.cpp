#include <iostream>
#include <vector>
#include <chrono>
#include <string>
#include "compio/file.hpp"

using namespace compio;

void benchmark_rebuild_index(uint32_t n_files) {
    std::cout << "Benchmarking rebuild_index with " << n_files << " files..." << std::endl;
    
    files_table ftable;
    ftable.max_files = n_files;
    ftable.files.resize(n_files);
    
    // Pre-populate directly to avoid map overhead during add (which we benchmark separately)
    for (uint32_t i = 0; i < n_files; ++i) {
        std::string name = "file_" + std::to_string(i) + ".dat";
        std::strncpy(ftable.files[i].name, name.c_str(), COMPIO_FNAME_MAX_SIZE - 1);
        ftable.files[i].name[COMPIO_FNAME_MAX_SIZE - 1] = '\0';
        ftable.files[i].size = 0;
        ftable.n_files++;
    }
    
    // Clear any existing map
    ftable.index_map_.clear();
    
    auto start = std::chrono::high_resolution_clock::now();
    ftable.rebuild_index();
    auto end = std::chrono::high_resolution_clock::now();
    
    std::chrono::duration<double> diff = end - start;
    std::cout << "Time to rebuild index (" << n_files << "): " << diff.count() << " s" << std::endl;
    std::cout << "Average per file: " << (diff.count() * 1e9 / n_files) << " ns" << std::endl;
}

int main() {
    benchmark_rebuild_index(100000); // 100k
    benchmark_rebuild_index(1000000); // 1M
    // benchmark_rebuild_index(5000000); // 5M - might be slow
    return 0;
}
