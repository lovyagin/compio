#ifdef COMPIO_BENCHMARK_BLOCKS_COUNTER
#include <iostream>
#include <random>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>

#include "compio.h"
#include "benchmark_util.hpp"      // for load_webster_data()
#include "compio/storage_block_reader.hpp"

int main(int argc, char **argv) {
    if (argc < 7) {
        std::cerr << "usage: ./blocks_benchmark <n_operations> <target_mean> <target_stddev> <seed> <n_switch> <region_size>\n";
        return -1;
    }

#ifndef NDEBUG
    std::cerr << "WARNING: the program was built in Debug mode, so performance measurements might be unreliable\n";
#endif

    const std::size_t n_operations = std::atoi(argv[1]);
    const std::size_t target_mean = std::atoi(argv[2]);   // initial file size & mean of size distribution
    const std::size_t target_stddev = std::atoi(argv[3]);
    const std::size_t seed = std::atoi(argv[4]);
    const std::size_t n_switch = std::atoi(argv[5]);
    const std::size_t region_size = std::atoi(argv[6]);

    std::string fn = "tmp_blocks_benchmark_XXXXXX.compio";

    // Load Webster sample data
    auto [sample_data, sample_data_size] = load_webster_data();
    if (!sample_data) {
        std::cerr << "failed to load Webster sample data\n";
        return -1;
    }

    // Prepare a buffer that can hold up to target_mean + 5*target_stddev bytes
    std::size_t max_size = target_mean + 5 * target_stddev;
    std::vector<char> big_buffer(max_size);
    for (std::size_t i = 0; i < max_size; ++i) {
        big_buffer[i] = sample_data[i % sample_data_size];
    }

    std::minstd_rand0 rng(seed);
    std::normal_distribution<double> size_dist(target_mean, target_stddev);

    std::vector<long long> block_counts;
    block_counts.reserve(n_operations);

    std::vector<double> insert_bps;
    std::vector<double> erase_bps;
    insert_bps.reserve(n_operations);
    erase_bps.reserve(n_operations);

    double last_insert_bps = 0.0;
    double last_erase_bps = 0.0;

    compio_config config;
    compio_build_default_config(&config);
    // config.cache_size__nodes = 128;
    // config.cache_size__blocks = 1024;

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

    // Initial write to reach target_mean bytes
    auto bytes = compio_write(big_buffer.data(), target_mean, file);
    if (bytes != target_mean) {
        compio_close_file(file);
        compio_close_archive(archive);
        std::cerr << "initial write failed\n";
        return -1;
    }

    // Region management
    std::size_t region_start = 0;
    std::size_t ops_since_switch = 0;

    bool failed = false;
    std::size_t until_progress = 1 + n_operations / 20;
    for (std::size_t i = 0; i < n_operations; ++i) {
        if (!--until_progress) {
            std::cerr << ".";
            until_progress = 1 + n_operations / 20;
        }

        // Switch region every n_switch operations
        if (ops_since_switch == 0) {
            std::size_t current_size = compio_get_size(file);
            if (current_size >= region_size) {
                std::uniform_int_distribution<std::size_t> region_dist(0, current_size - region_size);
                region_start = region_dist(rng);
            } else {
                region_start = 0;   // file smaller than region, fallback to whole file
            }
        }
        ops_since_switch = (ops_since_switch + 1) % n_switch;

        std::size_t current_size = compio_get_size(file);
        long long target_size = std::max(0LL, (long long)size_dist(rng));

        if (target_size > current_size) {
            // INSERT operation
            std::size_t insert_size = target_size - current_size;
            // Choose position inside current region, clamped to [0, current_size]
            std::size_t max_pos = std::min(region_start + region_size, current_size);
            if (region_start > max_pos) {
                // region is completely beyond EOF – write at end
                max_pos = current_size;
            }
            std::uniform_int_distribution<std::size_t> pos_dist(region_start, max_pos);
            std::size_t pos = pos_dist(rng);

            compio_seek(file, pos, COMPIO_SEEK_SET);

            auto start_time = std::chrono::high_resolution_clock::now();
            auto bytes_written = compio_insert(big_buffer.data(), insert_size, file);
            auto end_time = std::chrono::high_resolution_clock::now();

            if (bytes_written != insert_size) {
                failed = true;
                break;
            }

            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
            double bps = (bytes_written > 0) ? (bytes_written * 1000000.0) / duration.count() : 0.0;
            last_insert_bps = bps;

            block_counts.push_back(compio::bm_n_blocks);
            insert_bps.push_back(bps);
            erase_bps.push_back(last_erase_bps);

        } else if (target_size < current_size) {
            // ERASE operation
            std::size_t erase_size = current_size - target_size;
            // Choose position inside current region, ensuring room for erase_size
            std::size_t max_start = std::min(region_start + region_size, current_size - erase_size);
            if (region_start > max_start) {
                // region lies beyond the point where we can erase – use end region
                max_start = (current_size > erase_size) ? current_size - erase_size : 0;
            }
            std::uniform_int_distribution<std::size_t> pos_dist(region_start, max_start);
            std::size_t pos = pos_dist(rng);

            compio_seek(file, pos, COMPIO_SEEK_SET);

            auto start_time = std::chrono::high_resolution_clock::now();
            auto bytes_erased = compio_erase(erase_size, file);
            auto end_time = std::chrono::high_resolution_clock::now();

            if (bytes_erased != erase_size) {
                failed = true;
                break;
            }

            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
            double bps = (bytes_erased > 0) ? (bytes_erased * 1000000.0) / duration.count() : 0.0;
            last_erase_bps = bps;

            block_counts.push_back(compio::bm_n_blocks);
            insert_bps.push_back(last_insert_bps);
            erase_bps.push_back(bps);

        } else {
            // No size change – just record current block count
            block_counts.push_back(compio::bm_n_blocks);
            insert_bps.push_back(last_insert_bps);
            erase_bps.push_back(last_erase_bps);
        }
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
        std::cout << "n_blocks,insert_bps,erase_bps\n";
        for (size_t i = 0; i < block_counts.size(); ++i) {
            std::cout << block_counts[i] << "," << insert_bps[i] << "," << erase_bps[i] << "\n";
        }
    }

    remove(fn.c_str());
    return 0;
}

#else
#include <iostream>

int main() {
    std::cerr << "define COMPIO_BENCHMARK_BLOCKS_COUNTER to record number of blocks in compio library\n";
    return -1;
}
#endif