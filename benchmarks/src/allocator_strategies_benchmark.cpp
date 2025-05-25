#include <benchmark/benchmark.h>
#include "compio.h"

// Simple benchmark to test allocation strategy performance
static void BM_AllocationSpeed(benchmark::State& state) {
    const int strategy = state.range(0);
    const size_t block_count = state.range(1);
    const size_t block_size = state.range(2);

    std::string filename = "benchmark_alloc_" + std::to_string(strategy) + ".tmp";

    for (auto _ : state) {
        // Create a fresh archive with the specified allocation strategy
        remove(filename.c_str());
        compio_config config = {};
        compio_build_default_config(&config);
        config.allocation_strategy = static_cast<compio_allocation_strategy>(strategy);

        compio_archive* archive = compio_open_archive(filename.c_str(), "w+", &config);
        if (!archive) {
            state.SkipWithError("Failed to create archive");
            continue;
        }

        // Allocate blocks of specified size
        std::vector<compio_file*> files;
        for (size_t i = 0; i < block_count; i++) {
            std::string name = "file_" + std::to_string(i);
            compio_file* file = compio_open_file(name.c_str(), archive);
            if (file) {
                std::vector<uint8_t> data(block_size, 'A');
                compio_write(data.data(), data.size(), file);
                files.push_back(file);
            }
        }

        // Cleanup
        for (auto file : files) {
            compio_close_file(file);
        }
        compio_close_archive(archive);
        remove(filename.c_str());
    }
}

// Simple benchmark for fragmented allocation performance
static void BM_FragmentedAllocationSpeed(benchmark::State& state) {
    const int strategy = state.range(0);
    const size_t operation_count = state.range(1);

    std::string filename = "benchmark_frag_" + std::to_string(strategy) + ".tmp";

    for (auto _ : state) {
        // Setup
        remove(filename.c_str());
        compio_config config = {};
        compio_build_default_config(&config);
        config.allocation_strategy = static_cast<compio_allocation_strategy>(strategy);

        compio_archive* archive = compio_open_archive(filename.c_str(), "w+", &config);
        if (!archive) {
            state.SkipWithError("Failed to create archive");
            continue;
        }

        // Create files
        std::vector<compio_file*> files;
        for (size_t i = 0; i < operation_count; i++) {
            std::string name = "file_" + std::to_string(i);
            compio_file* file = compio_open_file(name.c_str(), archive);
            if (file) {
                std::vector<uint8_t> data(1024, 'A');
                compio_write(data.data(), data.size(), file);
                files.push_back(file);
            }
        }

        // Delete every other file to create fragmentation
        for (size_t i = 0; i < files.size(); i += 2) {
            if (i < files.size()) {
                std::string name = "file_" + std::to_string(i);
                compio_close_file(files[i]);
                compio_remove_file(archive, name.c_str());
                files[i] = nullptr;
            }
        }

        // Create new files that should fit into gaps
        for (size_t i = 0; i < operation_count/2; i++) {
            std::string name = "new_file_" + std::to_string(i);
            compio_file* file = compio_open_file(name.c_str(), archive);
            if (file) {
                std::vector<uint8_t> data(512, 'B');
                compio_write(data.data(), data.size(), file);
                files.push_back(file);
            }
        }

        // Cleanup
        for (auto file : files) {
            if (file) compio_close_file(file);
        }
        compio_close_archive(archive);
        remove(filename.c_str());
    }
}

BENCHMARK(BM_AllocationSpeed)
    ->Args({COMPIO_ALLOC_FIRST_FIT, 100, 1024})
    ->Args({COMPIO_ALLOC_BEST_FIT, 100, 1024})
    ->Args({COMPIO_ALLOC_WORST_FIT, 100, 1024})
    ->Args({COMPIO_ALLOC_NEXT_FIT, 100, 1024})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_FragmentedAllocationSpeed)
    ->Args({COMPIO_ALLOC_FIRST_FIT, 100})
    ->Args({COMPIO_ALLOC_BEST_FIT, 100})
    ->Args({COMPIO_ALLOC_WORST_FIT, 100})
    ->Args({COMPIO_ALLOC_NEXT_FIT, 100})
    ->Unit(benchmark::kMillisecond);