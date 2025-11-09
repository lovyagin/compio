#include <benchmark/benchmark.h>
#include <vector>
#include <string>
#include <random>
#include <cstdio>

#include "compio.h"
#include "compio/compio_file.hpp"

#ifndef COMPIO_SUCCESS
#define COMPIO_SUCCESS 0
#endif

/// Generates random incompressible data for testing.
static void fill_random_data(std::vector<uint8_t>& data, std::mt19937& gen) {
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    for (auto& byte : data) {
        byte = dist(gen);
    }
}

/// Returns the size of a file in bytes.
static size_t get_file_size(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fclose(f);
    return size;
}

/**
 * Benchmark: Fragmentation Overhead
 *
 * Measures space waste due to fragmentation with different block sizes.
 * Parameters: strategy, block_size, num_gaps, alloc_size
 *
 * Expected results: Best-fit minimizes wasted space, Worst-fit maximizes it.
 */
static void BM_FragmentationOverhead(benchmark::State& state) {
    const int strategy = static_cast<int>(state.range(0));
    const size_t block_size = state.range(1);      // Archive block size (1KB, 4KB, 8KB)
    const size_t num_gaps = state.range(2);        // Number of gaps to create (10, 20, 50)
    const size_t alloc_size = state.range(3);      // Size of files to allocate (500, 1000, 2000 bytes)

    std::string filename = "bench_frag_" + std::to_string(strategy) + ".tmp";
    std::mt19937 gen(12345);

    size_t total_wasted_space = 0;
    size_t total_allocated_files = 0;

    for ([[maybe_unused]] auto _ : state) {
        remove(filename.c_str());

        compio_config config = {};
        compio_build_default_config(&config);
        config.block_size = block_size;
        config.allocation_strategy = static_cast<compio_allocation_strategy>(strategy);
        compio_build_dummy_compressor(&config.compressor);

        compio_archive* archive = compio_open_archive(filename.c_str(), "w+", &config);
        if (!archive) {
            state.SkipWithError("Failed to create archive");
            continue;
        }

        // Create small (1 block) and large (3 blocks) files that will be deleted to form gaps
        for (size_t i = 0; i < num_gaps; i++) {
            // Create small file for 1-block gap
            std::string del1 = "del_s_" + std::to_string(i);
            compio_file* file = compio_open_file(del1.c_str(), archive);
            if (file) {
                std::vector<uint8_t> data(block_size - 100);
                fill_random_data(data, gen);
                compio_write(data.data(), data.size(), file);
                compio_close_file(file);
            }

            // Create separator file to prevent gap merging
            std::string sep = "sep_" + std::to_string(i);
            file = compio_open_file(sep.c_str(), archive);
            if (file) {
                std::vector<uint8_t> data(block_size, 'S');
                compio_write(data.data(), data.size(), file);
                compio_close_file(file);
            }

            // Create large file for 3-block gap
            std::string del2 = "del_l_" + std::to_string(i);
            file = compio_open_file(del2.c_str(), archive);
            if (file) {
                std::vector<uint8_t> data(block_size * 3 - 100);
                fill_random_data(data, gen);
                compio_write(data.data(), data.size(), file);
                compio_close_file(file);
            }

            // Create another separator file
            std::string sep2 = "sep2_" + std::to_string(i);
            file = compio_open_file(sep2.c_str(), archive);
            if (file) {
                std::vector<uint8_t> data(block_size, 'S');
                compio_write(data.data(), data.size(), file);
                compio_close_file(file);
            }
        }

        size_t size_before_delete = get_file_size(filename.c_str());

        // Delete files to create fragmented gaps
        for (size_t i = 0; i < num_gaps; i++) {
            compio_remove_file(archive, ("del_s_" + std::to_string(i)).c_str());
            compio_remove_file(archive, ("del_l_" + std::to_string(i)).c_str());
        }

        // Allocate new files of specified size into the fragmented space
        total_allocated_files = num_gaps * 2;
        for (size_t i = 0; i < total_allocated_files; i++) {
            std::string name = "new_" + std::to_string(i);
            compio_file* file = compio_open_file(name.c_str(), archive);
            if (file) {
                std::vector<uint8_t> data(alloc_size);
                fill_random_data(data, gen);
                compio_write(data.data(), data.size(), file);
                compio_close_file(file);
            }
        }

        size_t size_after_alloc = get_file_size(filename.c_str());
        total_wasted_space = size_after_alloc - size_before_delete;

        compio_close_archive(archive);
        remove(filename.c_str());
    }

    state.counters["WastedSpace_KB"] = static_cast<double>(total_wasted_space) / 1024.0;
    state.counters["WastedPerFile_bytes"] = static_cast<double>(total_wasted_space) / total_allocated_files;
    state.counters["NumGaps"] = static_cast<double>(num_gaps);
    state.counters["AllocSize_bytes"] = static_cast<double>(alloc_size);
}

/**
 * Benchmark: Space Reuse Efficiency
 *
 * Measures how efficiently different allocation strategies reuse freed space.
 * Parameters: strategy, block_size, num_files, delete_percentage
 *
 * Expected results: Best-fit should demonstrate the most efficient space reuse.
 */
static void BM_SpaceReuseEfficiency(benchmark::State& state) {
    const int strategy = static_cast<int>(state.range(0));
    const size_t block_size = state.range(1);
    const size_t num_files = state.range(2);
    const size_t delete_pct = state.range(3);  // Percentage of files to delete (30, 50, 70)

    std::string filename = "bench_reuse_" + std::to_string(strategy) + ".tmp";
    std::mt19937 gen(999);

    size_t space_before = 0;
    size_t space_after = 0;
    size_t num_deleted = 0;

    for ([[maybe_unused]] auto _ : state) {
        remove(filename.c_str());

        compio_config config = {};
        compio_build_default_config(&config);
        config.block_size = block_size;
        config.allocation_strategy = static_cast<compio_allocation_strategy>(strategy);
        compio_build_dummy_compressor(&config.compressor);

        compio_archive* archive = compio_open_archive(filename.c_str(), "w+", &config);
        if (!archive) {
            state.SkipWithError("Failed to create archive");
            continue;
        }

        // Phase 1: Create initial files with varied sizes
        for (size_t i = 0; i < num_files; i++) {
            std::string name = "initial_" + std::to_string(i);
            compio_file* file = compio_open_file(name.c_str(), archive);
            if (file) {
                size_t size = block_size / 2 + (i % 10) * (block_size / 10);
                std::vector<uint8_t> data(size);
                fill_random_data(data, gen);
                compio_write(data.data(), data.size(), file);
                compio_close_file(file);
            }
        }

        compio_flush(archive);
        space_before = get_file_size(filename.c_str());

        // Phase 2: Delete specified percentage of files
        num_deleted = (num_files * delete_pct) / 100;
        for (size_t i = 0; i < num_files; i += 100 / delete_pct) {
            std::string name = "initial_" + std::to_string(i);
            compio_remove_file(archive, name.c_str());
        }

        // Phase 3: Reallocate same number of files
        for (size_t i = 0; i < num_deleted; i++) {
            std::string name = "reuse_" + std::to_string(i);
            compio_file* file = compio_open_file(name.c_str(), archive);
            if (file) {
                size_t size = block_size / 2 + (i % 10) * (block_size / 10);
                std::vector<uint8_t> data(size);
                fill_random_data(data, gen);
                compio_write(data.data(), data.size(), file);
                compio_close_file(file);
            }
        }

        compio_flush(archive);
        space_after = get_file_size(filename.c_str());

        compio_close_archive(archive);
        remove(filename.c_str());
    }

    double space_growth_pct = 100.0 * (static_cast<double>(space_after - space_before)) / space_before;
    state.counters["SpaceGrowth%"] = space_growth_pct;
    state.counters["SpaceBefore_KB"] = static_cast<double>(space_before) / 1024.0;
    state.counters["SpaceAfter_KB"] = static_cast<double>(space_after) / 1024.0;
    state.counters["FilesDeleted"] = static_cast<double>(num_deleted);
}

/**
 * Benchmark Registration: Fragmentation Overhead
 *
 * Tests various configurations of block sizes, gap counts, and allocation sizes
 * to measure fragmentation impact across different allocation strategies.
 */
BENCHMARK(BM_FragmentationOverhead)
    // Small block size (1KB), few gaps, small allocations
    ->Args({COMPIO_ALLOC_FIRST_FIT, 1024, 10, 500})
    ->Args({COMPIO_ALLOC_BEST_FIT, 1024, 10, 500})
    ->Args({COMPIO_ALLOC_WORST_FIT, 1024, 10, 500})
    ->Args({COMPIO_ALLOC_NEXT_FIT, 1024, 10, 500})
    // Medium block size (4KB), medium gaps, medium allocations
    ->Args({COMPIO_ALLOC_FIRST_FIT, 4096, 20, 1000})
    ->Args({COMPIO_ALLOC_BEST_FIT, 4096, 20, 1000})
    ->Args({COMPIO_ALLOC_WORST_FIT, 4096, 20, 1000})
    ->Args({COMPIO_ALLOC_NEXT_FIT, 4096, 20, 1000})
    // Large block size (8KB), many gaps, large allocations
    ->Args({COMPIO_ALLOC_FIRST_FIT, 8192, 30, 2000})
    ->Args({COMPIO_ALLOC_BEST_FIT, 8192, 30, 2000})
    ->Args({COMPIO_ALLOC_WORST_FIT, 8192, 30, 2000})
    ->Args({COMPIO_ALLOC_NEXT_FIT, 8192, 30, 2000})
    ->Unit(benchmark::kMillisecond)
    ->Name("FragmentationOverhead");

/**
 * Benchmark Registration: Space Reuse Efficiency
 *
 * Tests various configurations of block sizes, file counts, and deletion percentages
 * to measure how efficiently each allocation strategy reuses freed space.
 */
BENCHMARK(BM_SpaceReuseEfficiency)
    // Small blocks, few files, 30% deletion
    ->Args({COMPIO_ALLOC_FIRST_FIT, 1024, 30, 30})
    ->Args({COMPIO_ALLOC_BEST_FIT, 1024, 30, 30})
    ->Args({COMPIO_ALLOC_WORST_FIT, 1024, 30, 30})
    ->Args({COMPIO_ALLOC_NEXT_FIT, 1024, 30, 30})
    // Medium blocks, medium files, 50% deletion
    ->Args({COMPIO_ALLOC_FIRST_FIT, 4096, 50, 50})
    ->Args({COMPIO_ALLOC_BEST_FIT, 4096, 50, 50})
    ->Args({COMPIO_ALLOC_WORST_FIT, 4096, 50, 50})
    ->Args({COMPIO_ALLOC_NEXT_FIT, 4096, 50, 50})
    // Large blocks, many files, 70% deletion (stress test)
    ->Args({COMPIO_ALLOC_FIRST_FIT, 8192, 60, 70})
    ->Args({COMPIO_ALLOC_BEST_FIT, 8192, 60, 70})
    ->Args({COMPIO_ALLOC_WORST_FIT, 8192, 60, 70})
    ->Args({COMPIO_ALLOC_NEXT_FIT, 8192, 60, 70})
    ->Unit(benchmark::kMillisecond)
    ->Name("SpaceReuse");
