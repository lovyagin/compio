#include <iostream>
#include <vector>
#include <chrono>
#include <string>
#include "compio/file.hpp"

using namespace compio;

void benchmark_rebuild_index(uint32_t n_files) {
    std::cout << "Benchmarking files table load with " << n_files << " files..." << std::endl;

    files_table source;
    for (uint32_t i = 0; i < n_files; ++i) {
        source.add(("file_" + std::to_string(i) + ".dat").c_str());
    }
    const std::vector<uint8_t> bytes = source.serialize();

    // Parsing rebuilds the name index, as on archive open.
    files_table ftable;
    auto start = std::chrono::high_resolution_clock::now();
    const bool parsed = ftable.parse(bytes.data(), bytes.size(), n_files);
    auto end = std::chrono::high_resolution_clock::now();
    if (!parsed) {
        std::cerr << "parse failed" << std::endl;
        return;
    }

    std::chrono::duration<double> diff = end - start;
    std::cout << "Time to load table (" << n_files << "): " << diff.count() << " s" << std::endl;
    std::cout << "Average per file: " << (diff.count() * 1e9 / n_files) << " ns" << std::endl;
}

int main() {
    benchmark_rebuild_index(100000); // 100k
    benchmark_rebuild_index(1000000); // 1M
    // benchmark_rebuild_index(5000000); // 5M - might be slow
    return 0;
}
