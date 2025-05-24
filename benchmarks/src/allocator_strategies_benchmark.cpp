#ifndef COMPIO_ALLOCATION_FIRST_FIT
#define COMPIO_ALLOCATION_FIRST_FIT 0
#define COMPIO_ALLOCATION_BEST_FIT 1
#define COMPIO_ALLOCATION_WORST_FIT 2
#define COMPIO_ALLOCATION_NEXT_FIT 3
#endif

#include "compio.h"
#include "allocator.hpp"
#include "sample_data.hpp"

#include <benchmark/benchmark.h>
#include <random>
#include <string>
#include <vector>
#include <iostream>

using namespace compio;


// Helper function to create an archive with a specific allocation strategy
compio_archive* create_test_archive(const char* filename, int strategy) {
    compio_config config;
    compio_build_default_config(&config);

    // Convert int to proper allocation strategy type
    config.allocation_strategy = static_cast<compio_allocation_strategy>(strategy);
    config.cache_size = 0;  // Disable cache to focus on allocation strategy
    config.block_cache_size = 0;

    return compio_open_archive(filename, "w+", &config);
}

// Simulate fragmentation by writing and freeing blocks in a pattern
void create_fragmentation(compio_archive* archive, size_t block_count, size_t min_size, size_t max_size) {
    std::minstd_rand0 rng(0);
    std::uniform_int_distribution<size_t> size_dist(min_size, max_size);
    std::vector<compio_file*> files;
    std::vector<std::string> filenames;

    // Create multiple files to track them
    for (size_t i = 0; i < block_count; i++) {
        std::string name = "file_" + std::to_string(i);
        filenames.push_back(name);
        compio_file* file = compio_open_file(name.c_str(), archive);
        files.push_back(file);

        size_t size = size_dist(rng);
        std::vector<char> data(size, 'A' + (i % 26));
        compio_write(data.data(), size, file);
    }

    // Delete half of the files to create free blocks
    for (size_t i = 0; i < block_count; i += 2) {
        compio_close_file(files[i]);
        // Fixed parameter order - archive first, then filename
        compio_remove_file(archive, filenames[i].c_str());
    }

    for (size_t i = 1; i < block_count; i += 2) {
        compio_close_file(files[i]);
    }
}

static void BM_AllocationStrategy(benchmark::State& state) {
    const int strategy = state.range(0);
    const size_t block_count = state.range(1);
    const size_t min_size = 1024;
    const size_t max_size = 8192;

    char filename[L_tmpnam];
    tmpnam(filename);

    // Prepare archive with fragmentation
    {
        compio_archive* archive = create_test_archive(filename, strategy);
        create_fragmentation(archive, block_count, min_size, max_size);
        compio_close_archive(archive);
    }

    // Benchmark allocation operations
    for (auto _ : state) {
        compio_archive* archive = compio_open_archive(filename, "r+", nullptr);
        if (!archive) {
            state.SkipWithError("Failed to open archive");
            break;
        }

        // Allocate blocks of varying sizes
        std::minstd_rand0 rng(1);
        std::uniform_int_distribution<size_t> size_dist(min_size, max_size);

        compio_file* file = compio_open_file("benchmark_file", archive);
        if (!file) {
            compio_close_archive(archive);
            state.SkipWithError("Failed to open file");
            break;
        }

        // Perform write operations that will trigger allocations
        for (size_t i = 0; i < 100; i++) {
            size_t size = size_dist(rng);
            std::vector<char> data(size, 'X');
            compio_seek(file, i * max_size * 2, COMP_SEEK_SET);
            compio_write(data.data(), size, file);
        }

        compio_close_file(file);
        compio_close_archive(archive);
    }

    remove(filename);
}

BENCHMARK(BM_AllocationStrategy)
    ->Args({COMPIO_ALLOCATION_FIRST_FIT, 100})
    ->Args({COMPIO_ALLOCATION_BEST_FIT, 100})
    ->Args({COMPIO_ALLOCATION_WORST_FIT, 100})
    ->Args({COMPIO_ALLOCATION_NEXT_FIT, 100})
    ->Args({COMPIO_ALLOCATION_FIRST_FIT, 500})
    ->Args({COMPIO_ALLOCATION_BEST_FIT, 500})
    ->Args({COMPIO_ALLOCATION_WORST_FIT, 500})
    ->Args({COMPIO_ALLOCATION_NEXT_FIT, 500})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

static void BM_FragmentedAllocation(benchmark::State& state) {
    const int strategy = state.range(0);

    char filename[L_tmpnam];
    tmpnam(filename);

    // Create highly fragmented archive
    {
        compio_archive* archive = create_test_archive(filename, strategy);
        // Create many small allocations with gaps
        create_fragmentation(archive, 1000, 64, 256);
        compio_close_archive(archive);
    }

    // Benchmark allocation of large blocks in fragmented space
    for (auto _ : state) {
        compio_archive* archive = compio_open_archive(filename, "r+", nullptr);
        compio_file* file = compio_open_file("large_file", archive);

        // Try to allocate larger blocks that will require finding appropriate free space
        std::vector<char> data(4096, 'L');
        for (int i = 0; i < 20; i++) {
            compio_seek(file, i * 8192, COMP_SEEK_SET);
            compio_write(data.data(), data.size(), file);
        }

        compio_close_file(file);
        compio_close_archive(archive);
    }

    remove(filename);
}

BENCHMARK(BM_FragmentedAllocation)
    ->Args({COMPIO_ALLOCATION_FIRST_FIT})
    ->Args({COMPIO_ALLOCATION_BEST_FIT})
    ->Args({COMPIO_ALLOCATION_WORST_FIT})
    ->Args({COMPIO_ALLOCATION_NEXT_FIT})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();