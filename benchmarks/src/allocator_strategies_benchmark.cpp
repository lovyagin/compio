#ifndef COMPIO_SUCCESS
#define COMPIO_SUCCESS 0
#endif

#include <benchmark/benchmark.h>
#include "compio.h"
#include <random>

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

    state.SetBytesProcessed(state.iterations() * block_count * block_size);
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

    size_t total_bytes = operation_count * 1024 + (operation_count / 2) * 512;
    state.SetBytesProcessed(state.iterations() * total_bytes);
}

static void BM_AllocationStrategyCompression(benchmark::State& state) {
    const int strategy = state.range(0);
    const size_t compressible_blocks = state.range(1);
    const size_t random_blocks = state.range(2);
    const size_t block_size = state.range(3);

    std::string filename = "benchmark_mixed_" + std::to_string(strategy) + ".tmp";

    // Generate test data
    std::vector<uint8_t> compressible_data(block_size, 'A');  // Highly compressible (same byte repeated)
    std::vector<uint8_t> random_data(block_size);             // Random data (poorly compressible)

    // Fill random data
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, 255);
    for (auto& byte : random_data) {
        byte = distrib(gen);
    }

    size_t final_file_size = 0;

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

        // Phase 1: Write compressible data
        for (size_t i = 0; i < compressible_blocks; i++) {
            std::string name = "comp_file_" + std::to_string(i);
            compio_file* file = compio_open_file(name.c_str(), archive);
            if (file) {
                compio_write(compressible_data.data(), compressible_data.size(), file);
                compio_close_file(file);
            }
        }

        // Phase 2: Write random data
        for (size_t i = 0; i < random_blocks; i++) {
            std::string name = "rand_file_" + std::to_string(i);
            compio_file* file = compio_open_file(name.c_str(), archive);
            if (file) {
                compio_write(random_data.data(), random_data.size(), file);
                compio_close_file(file);
            }
        }

        compio_close_archive(archive);

        // Get final file size
        FILE* f = fopen(filename.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            final_file_size = ftell(f);
            fclose(f);
        }

        remove(filename.c_str());
    }

    // Report the file size as custom counter
    state.counters["FileSize"] = final_file_size;

    // Still report throughput for reference
    size_t total_bytes = (compressible_blocks + random_blocks) * block_size;
    state.SetBytesProcessed(state.iterations() * total_bytes);
}

static void BM_AllocationFragmentationResistance(benchmark::State& state) {
    const int strategy = state.range(0);
    const size_t cycle_count = state.range(1);

    std::string filename = "benchmark_frag_resist_" + std::to_string(strategy) + ".tmp";

    // Track internal fragmentation
    size_t wasted_space = 0;
    size_t file_size = 0;

    for (auto _ : state) {
        remove(filename.c_str());
        compio_config config = {};
        compio_build_default_config(&config);
        config.allocation_strategy = static_cast<compio_allocation_strategy>(strategy);

        compio_archive* archive = compio_open_archive(filename.c_str(), "w+", &config);

        // Create a pattern of mixed allocations and deallocations
        for (size_t cycle = 0; cycle < cycle_count; cycle++) {
            // Create files of various sizes
            for (size_t i = 0; i < 10; i++) {
                size_t size = 128 * (i + 1); // 128, 256, 384, ...
                std::string name = "file_" + std::to_string(cycle) + "_" + std::to_string(i);
                compio_file* file = compio_open_file(name.c_str(), archive);
                if (file) {
                    std::vector<uint8_t> data(size, 'A');
                    compio_write(data.data(), data.size(), file);
                    compio_close_file(file);
                }
            }

            // Delete some files to create fragmentation (delete odd numbered files)
            for (size_t i = 1; i < 10; i += 2) {
                std::string name = "file_" + std::to_string(cycle) + "_" + std::to_string(i);
                compio_remove_file(archive, name.c_str());
            }
        }

        // Get file size
        compio_close_archive(archive);
        FILE* f = fopen(filename.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            file_size = ftell(f);
            fclose(f);
        }

        remove(filename.c_str());
    }

    // Report metrics
    state.counters["FileSize"] = file_size;
    state.counters["FileSizePerOp"] = file_size / (double)(cycle_count * 10 / 2); // Size per remaining file
}

static void BM_ExtremeFragmentation(benchmark::State& state) {
    const int strategy = state.range(0);
    // Reduced number of files to avoid potential resource issues
    const size_t initial_files = 50;

    std::string filename = "benchmark_extreme_frag_" + std::to_string(strategy) + ".tmp";

    for (auto _ : state) {
        // Remove any existing file
        remove(filename.c_str());

        compio_config config = {};
        compio_build_default_config(&config);
        config.allocation_strategy = static_cast<compio_allocation_strategy>(strategy);

        compio_archive* archive = compio_open_archive(filename.c_str(), "w+", &config);
        if (!archive) {
            state.SkipWithError("Failed to open archive");
            continue;
        }

        // Phase 1: Create initial files
        for (size_t i = 0; i < initial_files; i++) {
            std::string name = "initial_" + std::to_string(i);
            compio_file* file = compio_open_file(name.c_str(), archive);
            if (!file) {
                state.SkipWithError("Failed to create file");
                compio_close_archive(archive);
                remove(filename.c_str());
                continue;
            }

            std::vector<uint8_t> data(1024, 'A');
            if (compio_write(data.data(), data.size(), file) != data.size()) {
                state.SkipWithError("Failed to write data");
            }
            compio_close_file(file);
        }

        // Phase 2: Delete some files to create fragmentation
        for (size_t i = 0; i < initial_files; i += 2) {
            std::string name = "initial_" + std::to_string(i);
            if (compio_remove_file(archive, name.c_str()) != COMPIO_SUCCESS) {
                // Just log the error but continue
                printf("Failed to remove file %s\n", name.c_str());
            }
        }

        // Phase 3: Add new files of different sizes
        size_t total_allocated = 0;
        size_t failed_allocations = 0;

        for (size_t size = 512; size <= 1536; size += 512) {
            for (size_t i = 0; i < 5; i++) {
                std::string name = "new_" + std::to_string(size) + "_" + std::to_string(i);
                compio_file* file = compio_open_file(name.c_str(), archive);
                if (file) {
                    std::vector<uint8_t> data(size, 'B');
                    compio_write(data.data(), data.size(), file);
                    compio_close_file(file);
                    total_allocated += size;
                } else {
                    failed_allocations++;
                }
            }
        }

        // Get file size
        size_t file_size = 0;
        compio_close_archive(archive);

        FILE* f = fopen(filename.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            file_size = ftell(f);
            fclose(f);
        }

        remove(filename.c_str());

        // Store metrics
        state.counters["FileSize"] = file_size;
        state.counters["TotalAllocated"] = total_allocated;
        state.counters["FailedAllocs"] = failed_allocations;
        if (file_size > 0) {
            state.counters["SpaceEfficiency"] = total_allocated / (double)file_size * 100.0;
        }
    }
}

BENCHMARK(BM_ExtremeFragmentation)
    ->Args({COMPIO_ALLOC_FIRST_FIT})
    ->Args({COMPIO_ALLOC_BEST_FIT})
    ->Args({COMPIO_ALLOC_WORST_FIT})
    ->Args({COMPIO_ALLOC_NEXT_FIT})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_AllocationStrategyCompression)
    ->Args({COMPIO_ALLOC_FIRST_FIT, 50, 50, 1024})
    ->Args({COMPIO_ALLOC_BEST_FIT, 50, 50, 1024})
    ->Args({COMPIO_ALLOC_WORST_FIT, 50, 50, 1024})
    ->Args({COMPIO_ALLOC_NEXT_FIT, 50, 50, 1024})
    ->Unit(benchmark::kMillisecond);

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