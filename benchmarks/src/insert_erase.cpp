#include "benchmark_util.hpp"
#include "compio.h"
#include "sample_data.hpp"
#include <benchmark/benchmark.h>
#include <random>
#include <fstream>

#ifdef COMPIO_BENCHMARK_BLOCKS_COUNTER
#include "compio/storage_block_reader.hpp"
#endif

extern compio_config config;

static void BM_InsertErase(benchmark::State &state) {
    const size_t n_blocks = state.range(0);
    const size_t target_mean = state.range(1);
    const size_t target_stddev = state.range(2);
    
    std::string fn = get_temporary_filename();
    
    std::minstd_rand0 rng(0);
    std::normal_distribution<double> size_dist(target_mean, target_stddev);
    
    size_t max_size = target_mean + 5 * target_stddev;
    size_t html_data_size = sizeof(html_data);
    char *big_buffer = new char[max_size];
    for (size_t i = 0; i < max_size; ++i) {
        big_buffer[i] = html_data[i % html_data_size];
    }
    
    bool first_iteration = true;
#ifdef COMPIO_BENCHMARK_BLOCKS_COUNTER
    std::vector<long long> block_counts;
#endif
    
    for (auto _ : state) {
        compio_archive *archive = compio_open_archive(fn.c_str(), "w+", &config);
        if (!archive) {
            state.SkipWithError("compio_open_archive failed");
            break;
        }

        compio_file *file = compio_open_file("A", archive);
        if (!file) {
            compio_close_archive(archive);
            state.SkipWithError("compio_open_file failed");
            break;
        }

        auto bytes = compio_write(big_buffer, target_mean, file);
        if (bytes != target_mean) {
            compio_close_file(file);
            compio_close_archive(archive);
            state.SkipWithError("initial write failed");
            break;
        }

#ifdef COMPIO_BENCHMARK_BLOCKS_COUNTER
        if (first_iteration) {
            block_counts.reserve(n_blocks);
        }
#endif

        bool failed = false;
        for (std::size_t i = 0; i < n_blocks; ++i) {
            auto op_start = std::chrono::high_resolution_clock::now();
            
            size_t current_size = compio_get_size(file);
            long long target_size = std::max(0LL, (long long)size_dist(rng));
            
            if (target_size > current_size) {
                size_t insert_size = target_size - current_size;
                size_t pos = current_size ? (rng() % current_size) : 0;
                compio_seek(file, pos, COMPIO_SEEK_SET);
                auto bytes = compio_insert(big_buffer, insert_size, file);
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
            
            auto op_end = std::chrono::high_resolution_clock::now();
            auto op_time = std::chrono::duration_cast<std::chrono::nanoseconds>(op_end - op_start);
            state.SetIterationTime(op_time.count() * 1e-9);
            
#ifdef COMPIO_BENCHMARK_BLOCKS_COUNTER
            if (first_iteration) {
                block_counts.push_back(compio::bm_n_blocks);
            }
#endif
        }

        if (failed) {
            compio_close_file(file);
            compio_close_archive(archive);
            state.SkipWithError("operation failed");
            break;
        }

        compio_flush(archive);
        compio_close_file(file);
        compio_close_archive(archive);
        
        first_iteration = false;
    }

    state.SetBytesProcessed(state.iterations() * n_blocks * target_mean);
    state.counters["final_file_size"] = benchmark::Counter(
        get_file_size(fn.c_str()), benchmark::Counter::kDefaults, benchmark::Counter::kIs1024);

#ifdef COMPIO_BENCHMARK_BLOCKS_COUNTER
    if (!block_counts.empty()) {
        std::ofstream csv("block_counts.csv");
        csv << "operation,n_blocks\n";
        for (size_t i = 0; i < block_counts.size(); ++i) {
            csv << i << "," << block_counts[i] << "\n";
        }
    }
#endif

    delete[] big_buffer;
    remove(fn.c_str());
}

BENCHMARK(BM_InsertErase)
    ->Args({1 << 10, 1 << 18, 1 << 15})
    ->Unit(benchmark::kMillisecond)
    ->UseManualTime();
