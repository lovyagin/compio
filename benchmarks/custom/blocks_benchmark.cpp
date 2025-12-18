#ifdef COMPIO_BENCHMARK_BLOCKS_COUNTER
#include <fstream>
#include <iostream>
#include <random>
#include <vector>
#include <string>

#include "compio.h"
#include "sample_data.hpp"
#include "compio/storage_block_reader.hpp"

int main(int argc, char **argv) {
    if (argc < 5) {
        std::cerr << "usage: ./blocks_benchmark <n_operations> <target_mean> <target_stddev> <out_file>\n";
        return -1;
    }
    
    const std::size_t n_operations = std::atoi(argv[1]);
    const std::size_t target_mean = std::atoi(argv[2]);
    const std::size_t target_stddev = std::atoi(argv[3]);
    const char *out_file = argv[4];
    
    std::string fn = "tmp_blocks_benchmark_XXXXXX.compio";
    
    std::minstd_rand0 rng(0);
    std::normal_distribution<double> size_dist(target_mean, target_stddev);
    
    std::size_t max_size = target_mean + 5 * target_stddev;
    std::size_t html_data_size = sizeof(html_data);
    std::vector<char> big_buffer(max_size);
    for (std::size_t i = 0; i < max_size; ++i) {
        big_buffer[i] = html_data[i % html_data_size];
    }
    
    std::vector<long long> block_counts;
    block_counts.reserve(n_operations);
    
    compio_config config;
    compio_build_default_config(&config);
    config.cache_size__nodes = 4;
    config.cache_size__blocks = 0;
    
    compio_archive *archive = compio_open_archive(fn.c_str(), "w+", &config);
    if (!archive) {
        std::cerr << "compio_open_archive failed\n";
        return -1;
    }
    
    compio_file *file = compio_open_file("A", archive);
    if (!file) {
        compio_close_archive(archive);
        std::cerr << "compio_open_file failed\n";
        return -1;
    }
    
    auto bytes = compio_write(big_buffer.data(), target_mean, file);
    if (bytes != target_mean) {
        compio_close_file(file);
        compio_close_archive(archive);
        std::cerr << "initial write failed\n";
        return -1;
    }
    
    bool failed = false;
    for (std::size_t i = 0; i < n_operations; ++i) {
        size_t current_size = compio_get_size(file);
        long long target_size = std::max(0LL, (long long)size_dist(rng));
        
        if (target_size > current_size) {
            size_t insert_size = target_size - current_size;
            size_t pos = rng() % (current_size + 1);
            compio_seek(file, pos, COMPIO_SEEK_SET);
            auto bytes = compio_insert(big_buffer.data(), insert_size, file);
            if (bytes != insert_size) {
                failed = true;
                break;
            }
        } else if (target_size < current_size) {
            size_t erase_size = current_size - target_size;
            size_t pos = rng() % (current_size - erase_size + 1);
            compio_seek(file, pos, COMPIO_SEEK_SET);
            auto bytes = compio_erase(erase_size, file);
            if (bytes != erase_size) {
                failed = true;
                break;
            }
        }
        
        block_counts.push_back(compio::bm_n_blocks);
    }
    
    if (failed) {
        compio_close_file(file);
        compio_close_archive(archive);
        std::cerr << "operation failed\n";
        return -1;
    }
    
    compio_flush(archive);
    compio_close_file(file);
    compio_close_archive(archive);
    
    if (!block_counts.empty()) {
        std::ofstream csv(out_file);
        csv << "n_blocks\n";
        for (size_t i = 0; i < block_counts.size(); ++i) {
            csv << block_counts[i] << "\n";
        }
    }
    
    remove(fn.c_str());
    
    return 0;
}
#else
#include <iostream>

int main() {
    std::cerr << "define COMPIO_BENCHMARK_BLOCKS_COUNTER to record number of blocks in compio library" << std::endl;
    return -1;
}
#endif