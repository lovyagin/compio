#include <benchmark/benchmark.h>
#include <vector>
#include <string>
#include <random>
#include <cstdio>

#include "compio.h"

#ifndef COMPIO_SUCCESS
#define COMPIO_SUCCESS 0
#endif

// Helper to generate random incompressible data
static void fill_random_data(std::vector<uint8_t>& data, std::mt19937& gen) {
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    for (auto& byte : data) {
        byte = dist(gen);
    }
}

// Helper function to get file size
static size_t get_file_size(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fclose(f);
    return size;
}


// =============================================================================
// BENCHMARK 1: Simple Sequential Allocation Speed
// Tests raw allocation performance without fragmentation
// Expected: All strategies should perform similarly
// =============================================================================
static void BM_SimpleAllocationSpeed(benchmark::State& state) {
    const int strategy = static_cast<int>(state.range(0));
    const size_t file_count = 100;
    const size_t file_size = 1024;

    std::string filename = "bench_simple_" + std::to_string(strategy) + ".tmp";

    for ([[maybe_unused]] auto _ : state) {
        state.PauseTiming();
        remove(filename.c_str());

        compio_config config = {};
        compio_build_default_config(&config);
        config.allocation_strategy = static_cast<compio_allocation_strategy>(strategy);

        compio_archive* archive = compio_open_archive(filename.c_str(), "w+", &config);
        if (!archive) {
            state.SkipWithError("Failed to create archive");
            continue;
        }

        state.ResumeTiming();

        // Allocate files sequentially
        for (size_t i = 0; i < file_count; i++) {
            std::string name = "file_" + std::to_string(i);
            compio_file* file = compio_open_file(name.c_str(), archive);
            if (file) {
                std::vector<uint8_t> data(file_size, 'A');
                compio_write(data.data(), data.size(), file);
                compio_close_file(file);
            }
        }

        state.PauseTiming();
        compio_close_archive(archive);
        remove(filename.c_str());
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * file_count);
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * file_count * file_size));
}

// =============================================================================
// BENCHMARK 2: Archive Size After Fragmentation with Varied Sizes
// Creates files of different sizes, deletes selectively, then allocates varied sizes
// Uses random data to prevent compression from hiding differences
// Expected: Best Fit should have lowest overhead (reuses small gaps better)
//           Worst Fit should have higher overhead (may create more unusable small gaps)
// =============================================================================
static void BM_ArchiveSizeAfterFragmentation(benchmark::State& state) {
    const int strategy = static_cast<int>(state.range(0));

    std::string filename = "bench_space_" + std::to_string(strategy) + ".tmp";
    std::mt19937 gen(12345); // Fixed seed for reproducibility

    size_t final_archive_size = 0;
    size_t baseline_archive_size = 0;

    for ([[maybe_unused]] auto _ : state) {
        // First, create baseline with no fragmentation
        remove(filename.c_str());

        compio_config config = {};
        compio_build_default_config(&config);
        config.allocation_strategy = static_cast<compio_allocation_strategy>(strategy);
        compio_build_dummy_compressor(&config.compressor); // Disable compression to see real allocation differences

        compio_archive* archive = compio_open_archive(filename.c_str(), "w+", &config);
        if (archive) {
            // Create 80 files with varied sizes (using random data)
            for (size_t i = 0; i < 80; i++) {
                std::string name = "baseline_" + std::to_string(i);
                compio_file* file = compio_open_file(name.c_str(), archive);
                if (file) {
                    size_t size = 512 + (i % 5) * 256; // Sizes: 512, 768, 1024, 1280, 1536
                    std::vector<uint8_t> data(size);
                    fill_random_data(data, gen);
                    compio_write(data.data(), data.size(), file);
                    compio_close_file(file);
                }
            }
            compio_close_archive(archive);
            baseline_archive_size = get_file_size(filename.c_str());
        }

        // Now create fragmented archive
        remove(filename.c_str());
        gen.seed(12345); // Reset for same data

        archive = compio_open_archive(filename.c_str(), "w+", &config);
        if (!archive) {
            state.SkipWithError("Failed to create archive");
            continue;
        }

        // Phase 1: Create initial files with varied sizes
        for (size_t i = 0; i < 100; i++) {
            std::string name = "file_" + std::to_string(i);
            compio_file* file = compio_open_file(name.c_str(), archive);
            if (file) {
                size_t size = 512 + (i % 5) * 256; // Varied sizes
                std::vector<uint8_t> data(size);
                fill_random_data(data, gen);
                compio_write(data.data(), data.size(), file);
                compio_close_file(file);
            }
        }

        // Phase 2: Delete files to create gaps of different sizes
        // Delete pattern: every 3rd file, creating gaps of varied sizes
        for (size_t i = 0; i < 100; i += 3) {
            std::string name = "file_" + std::to_string(i);
            compio_remove_file(archive, name.c_str());
        }

        // Phase 3: Allocate new files with different sizes
        // This tests how well each strategy reuses the freed space
        for (size_t i = 0; i < 30; i++) {
            std::string name = "new_" + std::to_string(i);
            compio_file* file = compio_open_file(name.c_str(), archive);
            if (file) {
                size_t size = 600 + (i % 4) * 200; // Different sizes: 600, 800, 1000, 1200
                std::vector<uint8_t> data(size);
                fill_random_data(data, gen);
                compio_write(data.data(), data.size(), file);
                compio_close_file(file);
            }
        }

        compio_close_archive(archive);
        final_archive_size = get_file_size(filename.c_str());
        remove(filename.c_str());
    }

    // Report metrics
    state.counters["ArchiveSize_KB"] = static_cast<double>(final_archive_size) / 1024.0;
    state.counters["BaselineSize_KB"] = static_cast<double>(baseline_archive_size) / 1024.0;
    if (baseline_archive_size > 0) {
        double overhead_pct = 100.0 * (static_cast<double>(final_archive_size) -
                                       static_cast<double>(baseline_archive_size)) /
                                      static_cast<double>(baseline_archive_size);
        state.counters["Overhead%"] = overhead_pct;
    }
}

// =============================================================================
// BENCHMARK 3: Large Allocation After Fragmentation
// Tests how well strategies handle large allocations in fragmented space
// Expected: Worst Fit should perform best (preserves large blocks)
// =============================================================================
static void BM_LargeAllocAfterFragmentation(benchmark::State& state) {
    const int strategy = static_cast<int>(state.range(0));

    std::string filename = "bench_large_" + std::to_string(strategy) + ".tmp";

    size_t successful_large_allocs = 0;

    for ([[maybe_unused]] auto _ : state) {
        remove(filename.c_str());

        compio_config config = {};
        compio_build_default_config(&config);
        config.allocation_strategy = static_cast<compio_allocation_strategy>(strategy);

        compio_archive* archive = compio_open_archive(filename.c_str(), "w+", &config);
        if (!archive) {
            state.SkipWithError("Failed to create archive");
            continue;
        }

        // Phase 1: Create 100 small files (512 bytes)
        for (size_t i = 0; i < 100; i++) {
            std::string name = "small_" + std::to_string(i);
            compio_file* file = compio_open_file(name.c_str(), archive);
            if (file) {
                std::vector<uint8_t> data(512, 'A');
                compio_write(data.data(), data.size(), file);
                compio_close_file(file);
            }
        }

        // Phase 2: Delete every third file to create gaps
        for (size_t i = 0; i < 100; i += 3) {
            std::string name = "small_" + std::to_string(i);
            compio_remove_file(archive, name.c_str());
        }

        // Phase 3: Try to allocate large files (4096 bytes each)
        successful_large_allocs = 0;
        for (size_t i = 0; i < 20; i++) {
            std::string name = "large_" + std::to_string(i);
            compio_file* file = compio_open_file(name.c_str(), archive);
            if (file) {
                std::vector<uint8_t> data(4096, 'B');
                if (compio_write(data.data(), data.size(), file) == data.size()) {
                    successful_large_allocs++;
                }
                compio_close_file(file);
            }
        }

        compio_close_archive(archive);
        remove(filename.c_str());
    }

    state.counters["SuccessfulLargeAllocs"] = static_cast<double>(successful_large_allocs);
    state.counters["SuccessRate%"] = 100.0 * static_cast<double>(successful_large_allocs) / 20.0;
}

// =============================================================================
// BENCHMARK 4: Varying Size Allocations - Strategy Comparison
// Tests how strategies handle mixed allocation sizes
// Expected: Different strategies should show different characteristics
// =============================================================================
static void BM_MixedSizeAllocations(benchmark::State& state) {
    const int strategy = static_cast<int>(state.range(0));

    std::string filename = "bench_mixed_" + std::to_string(strategy) + ".tmp";

    std::vector<size_t> sizes = {256, 512, 1024, 2048, 4096};
    size_t total_files_created = 0;

    for ([[maybe_unused]] auto _ : state) {
        remove(filename.c_str());

        compio_config config = {};
        compio_build_default_config(&config);
        config.allocation_strategy = static_cast<compio_allocation_strategy>(strategy);

        compio_archive* archive = compio_open_archive(filename.c_str(), "w+", &config);
        if (!archive) {
            state.SkipWithError("Failed to create archive");
            continue;
        }

        total_files_created = 0;

        // Allocate files of varying sizes in a pattern
        for (size_t round = 0; round < 20; round++) {
            for (size_t size : sizes) {
                std::string name = "file_" + std::to_string(round) + "_" + std::to_string(size);
                compio_file* file = compio_open_file(name.c_str(), archive);
                if (file) {
                    std::vector<uint8_t> data(size, 'X');
                    if (compio_write(data.data(), data.size(), file) == data.size()) {
                        total_files_created++;
                    }
                    compio_close_file(file);
                }
            }
        }

        compio_close_archive(archive);
        remove(filename.c_str());
    }

    state.counters["FilesCreated"] = static_cast<double>(total_files_created);
}

// =============================================================================
// Register Benchmarks
// =============================================================================

BENCHMARK(BM_SimpleAllocationSpeed)
    ->Args({COMPIO_ALLOC_FIRST_FIT})
    ->Args({COMPIO_ALLOC_BEST_FIT})
    ->Args({COMPIO_ALLOC_WORST_FIT})
    ->Args({COMPIO_ALLOC_NEXT_FIT})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_ArchiveSizeAfterFragmentation)
    ->Args({COMPIO_ALLOC_FIRST_FIT})
    ->Args({COMPIO_ALLOC_BEST_FIT})
    ->Args({COMPIO_ALLOC_WORST_FIT})
    ->Args({COMPIO_ALLOC_NEXT_FIT})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_LargeAllocAfterFragmentation)
    ->Args({COMPIO_ALLOC_FIRST_FIT})
    ->Args({COMPIO_ALLOC_BEST_FIT})
    ->Args({COMPIO_ALLOC_WORST_FIT})
    ->Args({COMPIO_ALLOC_NEXT_FIT})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_MixedSizeAllocations)
    ->Args({COMPIO_ALLOC_FIRST_FIT})
    ->Args({COMPIO_ALLOC_BEST_FIT})
    ->Args({COMPIO_ALLOC_WORST_FIT})
    ->Args({COMPIO_ALLOC_NEXT_FIT})
    ->Unit(benchmark::kMillisecond);
