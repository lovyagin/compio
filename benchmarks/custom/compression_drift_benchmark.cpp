/**
 * @file compression_drift_benchmark.cpp
 * @brief Benchmark demonstrating fragmentation via compression ratio drift
 *
 * This benchmark implements the original fragmentation strategy:
 * 1. Fill archive with highly compressible data (small compressed blocks)
 * 2. Gradually rewrite files with less compressible data (larger blocks)
 * 3. Observe fragmentation as larger blocks cannot fit in old gaps
 *
 * This creates realistic fragmentation without artificial file deletion.
 *
 * METRICS EXPLANATION:
 * - CompRatio: Fraction of random bytes (0.0 = all zeros/compressible, 1.0 = all random)
 * - Size(KB): Physical archive file size on disk
 * - Growth(KB): How much the archive grew since previous round
 * - FreeRgns: Number of separate free memory regions (holes) in the archive
 * - Frag%: Fragmentation level (0-100%), based on:
 *     - Number of free regions (more regions = more fragmented)
 *     - Size dispersion of free blocks (varied sizes = fragmented)
 *     - Gaps between free blocks
 * - Overhd%: Percentage of wasted space (free bytes / total size)
 * - Write(ms): Average time to write one file
 *
 * TEST DIFFERENCES:
 * Test 1: BEST_FIT, 100 files x 16KB, 10 rounds, compression drift 0.05->0.95
 * Test 2: FIRST_FIT (compare allocation strategy)
 * Test 3: BEST_FIT, 50 files x 32KB, 20 rounds, MORE AGGRESSIVE drift 0.02->0.98
 */

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>

#include "compio.h"
#include "compio/compio_file.hpp"
#include "compio/allocator.hpp"

// ----------------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------------

struct DriftParams {
    size_t block_size;
    compio_allocation_strategy strategy;

    int num_files;
    size_t file_size;  // Uncompressed size

    int rewrite_rounds;  // Number of times to rewrite with decreasing compressibility

    // Compression ratio progression (from highly compressible to incompressible)
    double initial_compress_ratio;  // e.g., 0.05 (highly compressible, lots of zeros)
    double final_compress_ratio;    // e.g., 0.95 (incompressible, random data)
};

// ----------------------------------------------------------------------------
// Data Generation
// ----------------------------------------------------------------------------

// Generate data with specific compression ratio
// ratio close to 0.0 = highly compressible (lots of zeros)
// ratio close to 1.0 = incompressible (random data)
void generate_data_with_ratio(std::vector<uint8_t>& data, double compress_ratio, std::mt19937& gen) {
    std::uniform_real_distribution<double> prob(0.0, 1.0);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    for (auto& byte : data) {
        if (prob(gen) < compress_ratio) {
            // Random byte (incompressible)
            byte = static_cast<uint8_t>(byte_dist(gen));
        } else {
            // Zero (highly compressible)
            byte = 0;
        }
    }
}

// ----------------------------------------------------------------------------
// Metrics Collection
// ----------------------------------------------------------------------------

struct Metrics {
    size_t physical_size;
    double avg_write_time_ms;

    // Fragmentation metrics
    size_t num_free_regions;
    size_t total_free_bytes;
    size_t largest_free_region;
    uint8_t fragmentation_percent;
    double overhead_percent;  // (total_size - used_size) / total_size * 100
};

Metrics collect_metrics(const std::string& archive_path, compio_archive* archive,
                        double write_time_ms, int num_files) {
    Metrics m = {};

    compio_flush(archive);

    // Get physical file size
    FILE* fp = fopen(archive_path.c_str(), "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        m.physical_size = ftell(fp);
        fclose(fp);
    }

    m.avg_write_time_ms = write_time_ms / num_files;

    // Get fragmentation statistics from allocator
    if (archive && archive->allocator) {
        auto stats = archive->allocator->get_fragmentation_stats();
        m.num_free_regions = stats.num_free_regions;
        m.total_free_bytes = stats.total_free_bytes;
        m.largest_free_region = stats.largest_free_region;
        m.fragmentation_percent = stats.fragmentation_percent;

        // Calculate overhead: wasted space percentage
        if (m.physical_size > 0) {
            m.overhead_percent = 100.0 * m.total_free_bytes / m.physical_size;
        }
    }

    return m;
}

// ----------------------------------------------------------------------------
// Compression Drift Test
// ----------------------------------------------------------------------------

void run_compression_drift_test(const DriftParams& params) {
    std::mt19937 gen(42);

    std::string filename = "/tmp/compression_drift_test.compio";
    remove(filename.c_str());

    compio_config config = {};
    compio_build_default_config(&config);
    config.block_size = static_cast<int>(params.block_size);
    config.allocation_strategy = params.strategy;
    compio_build_zlib_compressor(&config.compressor);

    compio_archive* archive = compio_open_archive(filename.c_str(), "w+", &config);
    if (!archive) {
        std::cerr << "Failed to create archive\n";
        return;
    }

    std::cout << "\n=== Compression Drift Fragmentation Test ===\n";
    std::cout << "Block Size: " << params.block_size << " bytes\n";
    std::cout << "Strategy: " << params.strategy << "\n";
    std::cout << "Files: " << params.num_files << " x " << params.file_size << " bytes\n";
    std::cout << "Compression drift: " << params.initial_compress_ratio
              << " -> " << params.final_compress_ratio << "\n\n";

    // ------------------------------------------------------------------------
    // PHASE 1: Initial population with highly compressible data
    // ------------------------------------------------------------------------

    std::cout << "Phase 1: Creating " << params.num_files << " files with highly compressible data...\n";

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < params.num_files; i++) {
        std::string fname = "file_" + std::to_string(i);
        std::vector<uint8_t> data(params.file_size);
        generate_data_with_ratio(data, params.initial_compress_ratio, gen);

        compio_file* file = compio_open_file(fname.c_str(), archive);
        if (file) {
            compio_write(data.data(), data.size(), file);
            compio_close_file(file);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double write_time = std::chrono::duration<double, std::milli>(end - start).count();

    Metrics initial = collect_metrics(filename, archive, write_time, params.num_files);

    std::cout << "  Physical size: " << initial.physical_size / 1024 << " KB\n";
    std::cout << "  Free space: " << initial.total_free_bytes / 1024 << " KB ("
              << std::fixed << std::setprecision(1) << initial.overhead_percent << "% overhead)\n";
    std::cout << "  Free regions: " << initial.num_free_regions << "\n";
    std::cout << "  Fragmentation: " << static_cast<int>(initial.fragmentation_percent) << "%\n";
    std::cout << "  Avg write time: " << std::setprecision(3)
              << initial.avg_write_time_ms << " ms/file\n";

    // ------------------------------------------------------------------------
    // PHASE 2: Gradual rewrite with decreasing compressibility
    // ------------------------------------------------------------------------

    std::cout << "\nPhase 2: Rewriting files with gradually decreasing compressibility...\n\n";
    std::cout << std::setw(6) << "Round"
              << std::setw(10) << "CompRatio"
              << std::setw(12) << "Size(KB)"
              << std::setw(11) << "Growth(KB)"
              << std::setw(10) << "FreeRgns"
              << std::setw(9) << "Frag%"
              << std::setw(11) << "Overhd%"
              << std::setw(12) << "Write(ms)"
              << "\n";
    std::cout << std::string(81, '-') << "\n";

    // Print initial state
    std::cout << std::setw(6) << 0
              << std::setw(10) << std::fixed << std::setprecision(2) << params.initial_compress_ratio
              << std::setw(12) << initial.physical_size / 1024
              << std::setw(11) << "-"
              << std::setw(10) << initial.num_free_regions
              << std::setw(9) << static_cast<int>(initial.fragmentation_percent)
              << std::setw(11) << std::setprecision(1) << initial.overhead_percent
              << std::setw(12) << std::setprecision(2) << initial.avg_write_time_ms
              << "\n";

    size_t prev_size = initial.physical_size;

    for (int round = 1; round <= params.rewrite_rounds; round++) {
        // Calculate compression ratio for this round (linear interpolation)
        double progress = static_cast<double>(round) / params.rewrite_rounds;
        double compress_ratio = params.initial_compress_ratio +
                                progress * (params.final_compress_ratio - params.initial_compress_ratio);

        start = std::chrono::high_resolution_clock::now();

        // Rewrite all files with new compression ratio
        for (int i = 0; i < params.num_files; i++) {
            std::string fname = "file_" + std::to_string(i);
            std::vector<uint8_t> data(params.file_size);
            generate_data_with_ratio(data, compress_ratio, gen);

            compio_file* file = compio_open_file(fname.c_str(), archive);
            if (file) {
                compio_write(data.data(), data.size(), file);
                compio_close_file(file);
            }
        }

        end = std::chrono::high_resolution_clock::now();
        write_time = std::chrono::duration<double, std::milli>(end - start).count();

        Metrics m = collect_metrics(filename, archive, write_time, params.num_files);

        long long growth = static_cast<long long>(m.physical_size) - static_cast<long long>(prev_size);

        std::cout << std::setw(6) << round
                  << std::setw(10) << std::fixed << std::setprecision(2) << compress_ratio
                  << std::setw(12) << m.physical_size / 1024
                  << std::setw(11) << (growth > 0 ? "+" : "") << growth / 1024
                  << std::setw(10) << m.num_free_regions
                  << std::setw(9) << static_cast<int>(m.fragmentation_percent)
                  << std::setw(11) << std::setprecision(1) << m.overhead_percent
                  << std::setw(12) << std::setprecision(2) << m.avg_write_time_ms
                  << "\n";

        prev_size = m.physical_size;
    }

    compio_close_archive(archive);
    remove(filename.c_str());

    // Print summary
    std::cout << "\n--- Summary ---\n";
    std::cout << "Archive grew from " << initial.physical_size / 1024 << " KB to "
              << prev_size / 1024 << " KB ("
              << std::fixed << std::setprecision(1)
              << (100.0 * prev_size / initial.physical_size - 100.0) << "% growth)\n";
    std::cout << "This demonstrates fragmentation: larger compressed blocks cannot reuse\n";
    std::cout << "space from smaller blocks, forcing allocator to append to file end.\n";
    std::cout << "\n";
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------

int main() {
    // Test 1: BEST_FIT with 4KB blocks
    DriftParams params1;
    params1.block_size = 4096;
    params1.strategy = COMPIO_ALLOC_BEST_FIT;
    params1.num_files = 100;
    params1.file_size = 16384;  // 16KB uncompressed
    params1.rewrite_rounds = 10;
    params1.initial_compress_ratio = 0.05;  // 95% zeros (highly compressible)
    params1.final_compress_ratio = 0.95;     // 95% random (incompressible)

    run_compression_drift_test(params1);

    // Test 2: FIRST_FIT comparison
    DriftParams params2 = params1;
    params2.strategy = COMPIO_ALLOC_FIRST_FIT;
    run_compression_drift_test(params2);

    // Test 3: More aggressive drift with larger files
    DriftParams params3;
    params3.block_size = 4096;
    params3.strategy = COMPIO_ALLOC_BEST_FIT;
    params3.num_files = 50;
    params3.file_size = 32768;  // 32KB uncompressed
    params3.rewrite_rounds = 20;
    params3.initial_compress_ratio = 0.02;  // 98% zeros
    params3.final_compress_ratio = 0.98;     // 98% random

    run_compression_drift_test(params3);

    return 0;
}
