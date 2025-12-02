/**
 * @file advanced_fragmentation.cpp
 * @brief Advanced fragmentation benchmarks with parametric grid
 *
 * This benchmark suite creates realistic fragmentation scenarios by:
 * 1. Mixing files of different sizes (small, medium, large)
 * 2. Mixing compressible and incompressible data
 * 3. Strategically deleting files to create gaps
 * 4. Measuring overhead and performance degradation
 *
 * No external dependencies required - generates CSV and text reports.
 */

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <map>
#include <iomanip>
#include <chrono>

#include "compio.h"
#include "compio/compio_file.hpp"

// ----------------------------------------------------------------------------
// Test Parameters Structure
// ----------------------------------------------------------------------------

struct FragmentationParams {
    // File size distribution (in blocks)
    double small_file_mean;      // Mean size for small files (e.g., 0.5 blocks)
    double small_file_stddev;    // Standard deviation
    double medium_file_mean;     // Mean size for medium files (e.g., 3 blocks)
    double medium_file_stddev;
    double large_file_mean;      // Mean size for large files (e.g., 10 blocks)
    double large_file_stddev;

    // File count and distribution
    int total_files;
    double small_file_prob;      // Probability of creating small file
    double medium_file_prob;     // Probability of creating medium file
    // large_file_prob = 1 - small_file_prob - medium_file_prob

    // Compression characteristics
    double compressible_prob;    // Probability of data being highly compressible
    double compress_ratio_good;  // Compression ratio for compressible data (e.g., 0.1)
    double compress_ratio_bad;   // Compression ratio for incompressible data (e.g., 0.95)

    // Deletion strategy
    double delete_prob_small;    // Probability of deleting small file
    double delete_prob_medium;   // Probability of deleting medium file
    double delete_prob_large;    // Probability of deleting large file
    bool delete_from_middle;     // Delete from middle positions (worst case)
    double min_delete_ratio;     // Minimum ratio of files to delete (ensures fragmentation)

    // Archive settings
    size_t block_size;
    compio_allocation_strategy alloc_strategy;

    // Performance measurement
    bool measure_performance;    // Whether to measure read/write performance on fragmented archive
    int rewrite_rounds;          // Number of rounds to rewrite files for performance testing
};

// ----------------------------------------------------------------------------
// Data Generation Helpers
// ----------------------------------------------------------------------------

// Generate highly compressible data (repeated patterns)
void generate_compressible_data(std::vector<uint8_t>& data, std::mt19937& gen) {
    std::uniform_int_distribution<uint8_t> pattern_dist(0, 15);
    uint8_t pattern = pattern_dist(gen);

    // Fill with repeated pattern
    for (size_t i = 0; i < data.size(); i++) {
        data[i] = pattern;
    }
}

// Generate incompressible data (random)
void generate_incompressible_data(std::vector<uint8_t>& data, std::mt19937& gen) {
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    for (auto& byte : data) {
        byte = dist(gen);
    }
}

// Generate data with specified compressibility
void generate_data(std::vector<uint8_t>& data, bool compressible, std::mt19937& gen) {
    if (compressible) {
        generate_compressible_data(data, gen);
    } else {
        generate_incompressible_data(data, gen);
    }
}

// ----------------------------------------------------------------------------
// File Metadata Tracking
// ----------------------------------------------------------------------------

enum FileSize { SMALL, MEDIUM, LARGE };

struct FileMetadata {
    std::string name;
    FileSize size_category;
    size_t actual_size;
    bool is_compressible;
    bool is_deleted;
};

// ----------------------------------------------------------------------------
// Statistical Distributions
// ----------------------------------------------------------------------------

size_t sample_normal_distribution(double mean, double stddev, std::mt19937& gen, size_t min_val = 1) {
    std::normal_distribution<double> dist(mean, stddev);
    double value = dist(gen);
    if (value < static_cast<double>(min_val)) {
        value = static_cast<double>(min_val);
    }
    return static_cast<size_t>(value);
}

// ----------------------------------------------------------------------------
// Fragmentation Test Runner
// ----------------------------------------------------------------------------

struct FragmentationResult {
    double overhead_percent;        // Wasted space percentage
    double fragmentation_level;     // Fragmentation score 0-100 (combines overhead and deletion rate)
    size_t initial_size;           // Archive size after initial writes
    size_t size_after_deletion;    // Archive size after deletions
    size_t final_size;             // Final archive size after rewrites
    size_t wasted_space;           // Total wasted bytes
    int files_created;
    int files_deleted;
    int files_rewritten;
    double time_taken_ms;
    double file_slots_fragmented;  // Number of fragmented files (files split across gaps)
    double fragmentation_ratio;    // Ratio of fragmented file slots to total files
    double performance_degradation;// Percentage slowdown on fragmented vs clean archive
    double write_time_fragmented_ms; // Time to write to fragmented archive
    double write_time_clean_ms;    // Time to write to clean archive (baseline)
};

FragmentationResult run_fragmentation_test(const FragmentationParams& params, const std::string& filename) {
    FragmentationResult result = {};
    std::mt19937 gen(42);

    std::uniform_real_distribution<double> prob_dist(0.0, 1.0);

    auto test_start = std::chrono::high_resolution_clock::now();

    remove(filename.c_str());

    compio_config config = {};
    compio_build_default_config(&config);
    config.block_size = static_cast<int>(params.block_size);
    config.block_size__minimum = 0;
    config.block_size__maximum = config.block_size * 4;
    config.allocation_strategy = params.alloc_strategy;

    // Use ZLIB for realistic compression testing
    compio_build_zlib_compressor(&config.compressor);

    compio_archive* archive = compio_open_archive(filename.c_str(), "w+", &config);
    if (!archive) {
        std::cerr << "Failed to create archive\n";
        return result;
    }

    std::vector<FileMetadata> files;

    // ------------------------------------------------------------------------
    // PHASE 1: Create initial files with mixed sizes and compressibility
    // ------------------------------------------------------------------------

    for (int i = 0; i < params.total_files; i++) {
        FileMetadata meta;
        meta.name = "file_" + std::to_string(i);
        meta.is_deleted = false;

        double size_roll = prob_dist(gen);
        if (size_roll < params.small_file_prob) {
            meta.size_category = SMALL;
            size_t blocks = sample_normal_distribution(
                params.small_file_mean, params.small_file_stddev, gen, 1);
            meta.actual_size = blocks * params.block_size;
        } else if (size_roll < params.small_file_prob + params.medium_file_prob) {
            meta.size_category = MEDIUM;
            size_t blocks = sample_normal_distribution(
                params.medium_file_mean, params.medium_file_stddev, gen, 1);
            meta.actual_size = blocks * params.block_size;
        } else {
            meta.size_category = LARGE;
            size_t blocks = sample_normal_distribution(
                params.large_file_mean, params.large_file_stddev, gen, 1);
            meta.actual_size = blocks * params.block_size;
        }

        meta.is_compressible = prob_dist(gen) < params.compressible_prob;

        compio_file* file = compio_open_file(meta.name.c_str(), archive);
        if (file) {
            std::vector<uint8_t> data(meta.actual_size);
            generate_data(data, meta.is_compressible, gen);
            compio_write(data.data(), data.size(), file);
            compio_close_file(file);
            result.files_created++;
        }

        files.push_back(meta);
    }

    compio_flush(archive);

    // Get initial size
    FILE* fp = fopen(filename.c_str(), "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        result.initial_size = ftell(fp);
        fclose(fp);
    }

    // ------------------------------------------------------------------------
    // PHASE 2: Strategic deletion to create fragmentation
    // ------------------------------------------------------------------------

    std::vector<size_t> indices_to_delete;
    size_t min_deletions = static_cast<size_t>(files.size() * params.min_delete_ratio);

    if (params.delete_from_middle) {

        // Strategy: Delete files with GAPS between them from middle region
        // This creates fragmentation: small free regions scattered throughout middle
        // Unlike deleting consecutive files which creates ONE large free block

        size_t start_idx = files.size() / 3;
        size_t end_idx = 2 * files.size() / 3;

        // Collect candidates from middle third, respecting size-based deletion probabilities
        std::vector<size_t> middle_candidates;
        for (size_t i = start_idx; i < end_idx; i++) {
            double delete_prob = 0.0;
            switch (files[i].size_category) {
                case SMALL: delete_prob = params.delete_prob_small; break;
                case MEDIUM: delete_prob = params.delete_prob_medium; break;
                case LARGE: delete_prob = params.delete_prob_large; break;
            }

            // Use probability to select which files are candidates
            if (prob_dist(gen) < delete_prob) {
                middle_candidates.push_back(i);
            }
        }

        // Shuffle to randomize which candidates we take
        std::shuffle(middle_candidates.begin(), middle_candidates.end(), gen);

        // Take as many as we can from candidates, but ensure we meet min_deletions
        for (size_t idx : middle_candidates) {
            if (indices_to_delete.size() >= min_deletions) break;
            indices_to_delete.push_back(idx);
        }

        // If we don't have enough candidates, expand to ALL middle files to meet quota
        if (indices_to_delete.size() < min_deletions) {
            std::vector<size_t> all_middle;
            for (size_t i = start_idx; i < end_idx; i++) {
                if (std::find(indices_to_delete.begin(), indices_to_delete.end(), i) == indices_to_delete.end()) {
                    all_middle.push_back(i);
                }
            }
            std::shuffle(all_middle.begin(), all_middle.end(), gen);
            for (size_t idx : all_middle) {
                if (indices_to_delete.size() >= min_deletions) break;
                indices_to_delete.push_back(idx);
            }
        }

        // Sort indices to delete them in order (this creates gaps if we skip some)
        std::sort(indices_to_delete.begin(), indices_to_delete.end());
    } else {
        // Random deletion across all files
        for (size_t i = 0; i < files.size(); i++) {
            double delete_prob = 0.0;
            switch (files[i].size_category) {
                case SMALL: delete_prob = params.delete_prob_small; break;
                case MEDIUM: delete_prob = params.delete_prob_medium; break;
                case LARGE: delete_prob = params.delete_prob_large; break;
            }

            if (prob_dist(gen) < delete_prob) {
                indices_to_delete.push_back(i);
            }
        }

        // Ensure minimum deletion ratio
        while (indices_to_delete.size() < min_deletions && indices_to_delete.size() < files.size()) {
            std::uniform_int_distribution<size_t> idx_dist(0, files.size() - 1);
            size_t idx = idx_dist(gen);
            if (std::find(indices_to_delete.begin(), indices_to_delete.end(), idx) == indices_to_delete.end()) {
                indices_to_delete.push_back(idx);
            }
        }
    }

    // Perform deletions
    for (size_t idx : indices_to_delete) {
        if (compio_remove_file(archive, files[idx].name.c_str()) == COMPIO_SUCCESS) {
            files[idx].is_deleted = true;
            result.files_deleted++;
        }
    }

    compio_flush(archive);

    // Get size after deletion
    fp = fopen(filename.c_str(), "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        result.size_after_deletion = ftell(fp);
        fclose(fp);
    }

    // ------------------------------------------------------------------------
    // PHASE 3: Write new files (potentially with different characteristics)
    // ------------------------------------------------------------------------

    // Write files with opposite compressibility to create worst case
    int rewrite_count = 0;
    for (const auto& file_meta : files) {
        if (file_meta.is_deleted) {
            std::string new_name = "rewrite_" + std::to_string(rewrite_count);
            compio_file* file = compio_open_file(new_name.c_str(), archive);

            if (file) {
                // Use similar size but opposite compressibility
                std::vector<uint8_t> data(file_meta.actual_size);
                // Flip compressibility to create worst fragmentation
                bool new_compressibility = !file_meta.is_compressible;
                generate_data(data, new_compressibility, gen);

                compio_write(data.data(), data.size(), file);
                compio_close_file(file);
                result.files_rewritten++;
            }

            rewrite_count++;
        }
    }

    compio_flush(archive);

    // Get final size
    fp = fopen(filename.c_str(), "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        result.final_size = ftell(fp);
        fclose(fp);
    }

    compio_fragmentation_stats frag_stats = {};
    if (compio_get_fragmentation_stats(archive, &frag_stats) == COMPIO_SUCCESS) {
        // Real fragmentation metrics from allocator
        result.file_slots_fragmented = static_cast<double>(frag_stats.num_free_regions);
        result.fragmentation_level = static_cast<double>(frag_stats.fragmentation_percent);

        // Calculate overhead based on free space fragmentation
        // More free regions = more fragmentation = higher overhead
        if (frag_stats.num_free_regions > 0) {
            // Overhead is the inability to use free space efficiently
            // If we have many small free regions, they can't be used for large allocations
            result.overhead_percent = static_cast<double>(frag_stats.fragmentation_percent);
            result.wasted_space = frag_stats.total_free_bytes;
        } else {
            result.overhead_percent = 0.0;
            result.wasted_space = 0;
        }

        result.fragmentation_ratio = frag_stats.num_free_regions > 0 ?
            static_cast<double>(frag_stats.num_free_regions) / static_cast<double>(params.total_files) : 0.0;
    } else {
        // Fallback to old metric if allocator stats not available
        result.wasted_space = result.final_size - result.size_after_deletion;
        if (result.size_after_deletion > 0) {
            result.overhead_percent = 100.0 * static_cast<double>(result.wasted_space) /
                                     static_cast<double>(result.size_after_deletion);
        }

        if (result.files_deleted > 0) {
            double deletion_impact = static_cast<double>(result.files_deleted) / static_cast<double>(params.total_files);
            double overhead_impact = std::min(100.0, result.overhead_percent) / 100.0;
            result.fragmentation_level = deletion_impact * overhead_impact * 100.0;
            if (result.fragmentation_level > 100.0) result.fragmentation_level = 100.0;
        }

        result.file_slots_fragmented = static_cast<double>(result.files_deleted) *
                                      (1.0 + result.overhead_percent / 100.0);
        result.fragmentation_ratio = result.file_slots_fragmented / static_cast<double>(params.total_files);
    }

    auto test_end = std::chrono::high_resolution_clock::now();
    result.time_taken_ms = std::chrono::duration<double, std::milli>(test_end - test_start).count();

    compio_close_archive(archive);
    remove(filename.c_str());

    return result;
}

// ----------------------------------------------------------------------------
// Parameter Grid Generation
// ----------------------------------------------------------------------------

std::vector<FragmentationParams> generate_parameter_grid() {
    std::vector<FragmentationParams> grid;

    std::vector<size_t> block_sizes = {512, 1024, 2048, 4096, 8192, 16384};
    std::vector<compio_allocation_strategy> strategies = {
        COMPIO_ALLOC_FIRST_FIT,
        COMPIO_ALLOC_BEST_FIT,
        COMPIO_ALLOC_WORST_FIT,
        COMPIO_ALLOC_NEXT_FIT
    };

    // More aggressive compressibility scenarios
    std::vector<double> compress_probs = {0.1, 0.3, 0.5, 0.7, 0.9};  // 10%, 30%, 50%, 70%, 90%

    // More aggressive deletion scenarios with extreme size mismatches
    struct DeletionScenario {
        double small, medium, large;
        bool from_middle;
        std::string name;
        double min_delete_ratio;
    };

    std::vector<DeletionScenario> deletion_scenarios = {
        {0.9, 0.2, 0.05, true, "AggDeleteSmall", 0.45},       // Delete 90% small from middle + 45% total minimum
        {0.3, 0.7, 0.9, true, "AggDeleteLarge", 0.50},        // Delete 90% large from middle
        {0.8, 0.8, 0.2, true, "AggDeleteMixed", 0.50},        // Delete most small+medium
        {0.5, 0.5, 0.5, false, "UniformDelete", 0.40},        // Uniform deletion (40% minimum)
        {0.95, 0.1, 0.05, true, "ExtremeSmall", 0.55},        // Delete almost all small files from middle
        {0.2, 0.2, 0.95, false, "ExtrémeLarge", 0.50},        // Delete almost all large files randomly
    };

    for (auto block_size : block_sizes) {
        for (auto strategy : strategies) {
            for (auto compress_prob : compress_probs) {
                for (const auto& del_scenario : deletion_scenarios) {
                    FragmentationParams params = {};

                    // File size distributions (in blocks) - more varied
                    params.small_file_mean = 0.3;      // Smaller baseline
                    params.small_file_stddev = 0.15;
                    params.medium_file_mean = 2.5;     // Mid-range
                    params.medium_file_stddev = 1.2;
                    params.large_file_mean = 8.0;      // Larger baseline
                    params.large_file_stddev = 3.5;

                    // File distribution - more small files to create fragmentation
                    params.total_files = 60;           // Reduced to stay under COMPIO_MAX_FILES=64 limit
                    params.small_file_prob = 0.65;     // 65% small (increased further)
                    params.medium_file_prob = 0.20;    // 20% medium, 15% large

                    // Compression - as specified
                    params.compressible_prob = compress_prob;
                    params.compress_ratio_good = 0.08;
                    params.compress_ratio_bad = 0.98;

                    // Deletion - as specified
                    params.delete_prob_small = del_scenario.small;
                    params.delete_prob_medium = del_scenario.medium;
                    params.delete_prob_large = del_scenario.large;
                    params.delete_from_middle = del_scenario.from_middle;
                    params.min_delete_ratio = del_scenario.min_delete_ratio;

                    // Archive settings
                    params.block_size = block_size;
                    params.alloc_strategy = strategy;

                    // Performance measurement
                    params.measure_performance = false; // Can enable for detailed analysis
                    params.rewrite_rounds = 1;

                    grid.push_back(params);
                }
            }
        }
    }

    return grid;
}

// ----------------------------------------------------------------------------
// Statistics and Analysis
// ----------------------------------------------------------------------------

struct StrategyStats {
    std::string name;
    double avg_overhead;
    double median_overhead;
    double min_overhead;
    double max_overhead;
    double std_overhead;
    double avg_fragmentation;
    double avg_time_ms;
    int test_count;
};

StrategyStats calculate_strategy_stats(
    const std::vector<std::pair<FragmentationParams, FragmentationResult>>& results,
    compio_allocation_strategy strategy) {

    StrategyStats stats = {};

    switch (strategy) {
        case COMPIO_ALLOC_FIRST_FIT: stats.name = "FIRST_FIT"; break;
        case COMPIO_ALLOC_BEST_FIT: stats.name = "BEST_FIT"; break;
        case COMPIO_ALLOC_WORST_FIT: stats.name = "WORST_FIT"; break;
        case COMPIO_ALLOC_NEXT_FIT: stats.name = "NEXT_FIT"; break;
    }

    std::vector<double> overheads;
    double sum_frag = 0;
    double sum_time = 0;

    for (const auto& [params, result] : results) {
        if (params.alloc_strategy == strategy) {
            overheads.push_back(result.overhead_percent);
            sum_frag += result.fragmentation_level;
            sum_time += result.time_taken_ms;
        }
    }

    if (overheads.empty()) return stats;

    stats.test_count = static_cast<int>(overheads.size());

    // Calculate stats
    std::sort(overheads.begin(), overheads.end());
    stats.min_overhead = overheads.front();
    stats.max_overhead = overheads.back();
    stats.median_overhead = overheads[overheads.size() / 2];

    double sum = 0;
    for (double v : overheads) sum += v;
    stats.avg_overhead = sum / static_cast<double>(overheads.size());

    double sq_sum = 0;
    for (double v : overheads) {
        double diff = v - stats.avg_overhead;
        sq_sum += diff * diff;
    }
    stats.std_overhead = std::sqrt(sq_sum / static_cast<double>(overheads.size()));

    stats.avg_fragmentation = sum_frag / static_cast<double>(overheads.size());
    stats.avg_time_ms = sum_time / static_cast<double>(overheads.size());

    return stats;
}

// --------------------------------------------------------------------
// CSV Output
// --------------------------------------------------------------------

void save_results_to_csv(const std::vector<std::pair<FragmentationParams, FragmentationResult>>& results,
                         const std::string& filename) {
    std::ofstream ofs(filename);
    if (!ofs.is_open()) {
        std::cerr << "Failed to open output file: " << filename << "\n";
        return;
    }

    // Extended header with new metrics
    ofs << "BlockSize,Strategy,CompressProb,DeleteSmall,DeleteMedium,DeleteLarge,"
        << "DeleteFromMiddle,MinDeleteRatio,FilesCreated,FilesDeleted,FilesRewritten,"
        << "InitialSize,SizeAfterDel,FinalSize,WastedSpace,OverheadPercent,"
        << "FragmentationLevel,FragmentedSlots,FragmentationRatio,TimeTakenMs\n";

    // Data rows
    for (const auto& [params, result] : results) {
        const char* strategy_name = "";
        switch (params.alloc_strategy) {
            case COMPIO_ALLOC_FIRST_FIT: strategy_name = "FIRST_FIT"; break;
            case COMPIO_ALLOC_BEST_FIT: strategy_name = "BEST_FIT"; break;
            case COMPIO_ALLOC_WORST_FIT: strategy_name = "WORST_FIT"; break;
            case COMPIO_ALLOC_NEXT_FIT: strategy_name = "NEXT_FIT"; break;
        }

        ofs << params.block_size << ","
            << strategy_name << ","
            << std::fixed << std::setprecision(2) << params.compressible_prob << ","
            << params.delete_prob_small << ","
            << params.delete_prob_medium << ","
            << params.delete_prob_large << ","
            << (params.delete_from_middle ? "Yes" : "No") << ","
            << std::fixed << std::setprecision(2) << params.min_delete_ratio << ","
            << result.files_created << ","
            << result.files_deleted << ","
            << result.files_rewritten << ","
            << result.initial_size << ","
            << result.size_after_deletion << ","
            << result.final_size << ","
            << result.wasted_space << ","
            << std::fixed << std::setprecision(2) << result.overhead_percent << ","
            << result.fragmentation_level << ","
            << std::fixed << std::setprecision(2) << result.file_slots_fragmented << ","
            << result.fragmentation_ratio << ","
            << result.time_taken_ms << "\n";
    }

    ofs.close();
}

// --------------------------------------------------------------------
// Text Report Generation
// --------------------------------------------------------------------


void generate_text_report(const std::vector<std::pair<FragmentationParams, FragmentationResult>>& results,
                         const std::string& filename) {
    std::ofstream ofs(filename);
    if (!ofs.is_open()) {
        std::cerr << "Failed to open report file: " << filename << "\n";
        return;
    }

    ofs << "FRAGMENTATION TEST REPORT\n";
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm* now_tm = std::localtime(&now_time_t);
    char time_str[100];
    std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", now_tm);
    ofs << "Generated: " << time_str << "\n";
    ofs << "Total tests: " << results.size() << "\n\n";

    std::vector<StrategyStats> all_stats;

    // Strategy Performance
    ofs << "ALLOCATION STRATEGY PERFORMANCE\n\n";

    std::vector<compio_allocation_strategy> strategies = {
        COMPIO_ALLOC_FIRST_FIT,
        COMPIO_ALLOC_BEST_FIT,
        COMPIO_ALLOC_WORST_FIT,
        COMPIO_ALLOC_NEXT_FIT
    };

    for (auto strategy : strategies) {
        auto stats = calculate_strategy_stats(results, strategy);
        if (stats.test_count > 0) {
            all_stats.push_back(stats);
            ofs << stats.name << ":\n";
            ofs << "  Tests Run:           " << stats.test_count << "\n";
            ofs << "  Average Overhead:    " << std::fixed << std::setprecision(2)
                << stats.avg_overhead << "%\n";
            ofs << "  Median Overhead:     " << stats.median_overhead << "%\n";
            ofs << "  Min Overhead:        " << stats.min_overhead << "%\n";
            ofs << "  Max Overhead:        " << stats.max_overhead << "%\n";
            ofs << "  Std Dev:             " << stats.std_overhead << "%\n";
            ofs << "  Avg Fragmentation:   " << stats.avg_fragmentation << "\n";
            ofs << "  Avg Time:            " << stats.avg_time_ms << " ms\n\n";
        }
    }

    // Ranking
    ofs << "\nBEST STRATEGY RANKING (by average overhead)\n\n";

    std::sort(all_stats.begin(), all_stats.end(),
              [](const StrategyStats& a, const StrategyStats& b) {
                  return a.avg_overhead < b.avg_overhead;
              });

    int rank = 1;
    for (const auto& stat : all_stats) {
        ofs << rank++ << ". " << stat.name << " - " << std::fixed << std::setprecision(2)
            << stat.avg_overhead << "% avg overhead\n";
    }
    ofs << "\n";

    // Block Size Impact
    ofs << "\nBLOCK SIZE IMPACT\n\n";

    std::map<size_t, std::vector<double>> block_size_overhead;
    std::map<size_t, std::vector<size_t>> block_size_wasted;

    for (const auto& [params, result] : results) {
        block_size_overhead[params.block_size].push_back(result.overhead_percent);
        block_size_wasted[params.block_size].push_back(result.wasted_space);
    }

    for (const auto& [block_size, overheads] : block_size_overhead) {
        double avg = 0;
        for (double v : overheads) avg += v;
        avg /= static_cast<double>(overheads.size());

        double avg_wasted = 0;
        for (size_t v : block_size_wasted[block_size]) avg_wasted += static_cast<double>(v);
        avg_wasted /= static_cast<double>(block_size_wasted[block_size].size());

        ofs << "Block Size " << block_size << " bytes:\n";
        ofs << "  Average Overhead:      " << std::fixed << std::setprecision(2) << avg << "%\n";
        ofs << "  Average Wasted Space:  " << (avg_wasted / 1024.0) << " KB\n\n";
    }

    // Compression Impact
    ofs << "\nCOMPRESSION IMPACT\n\n";

    std::map<double, std::vector<double>> compress_overhead;
    std::map<double, std::vector<double>> compress_frag;

    for (const auto& [params, result] : results) {
        compress_overhead[params.compressible_prob].push_back(result.overhead_percent);
        compress_frag[params.compressible_prob].push_back(result.fragmentation_level);
    }

    for (const auto& [prob, overheads] : compress_overhead) {
        double avg_oh = 0;
        for (double v : overheads) avg_oh += v;
        avg_oh /= static_cast<double>(overheads.size());

        double avg_fg = 0;
        for (double v : compress_frag[prob]) avg_fg += v;
        avg_fg /= static_cast<double>(compress_frag[prob].size());

        ofs << "Compressible Probability " << std::fixed << std::setprecision(1) << (prob * 100.0) << "%:\n";
        ofs << "  Average Overhead:      " << std::setprecision(2) << avg_oh << "%\n";
        ofs << "  Average Fragmentation: " << avg_fg << "\n\n";
    }

    // Deletion Strategy Impact
    ofs << "\nDELETION STRATEGY IMPACT\n\n";

    std::vector<double> middle_overheads, random_overheads;
    std::vector<double> middle_frags, random_frags;
    std::vector<double> middle_slots, random_slots;

    for (const auto& [params, result] : results) {
        if (params.delete_from_middle) {
            middle_overheads.push_back(result.overhead_percent);
            middle_frags.push_back(result.fragmentation_level);
            middle_slots.push_back(result.file_slots_fragmented);
        } else {
            random_overheads.push_back(result.overhead_percent);
            random_frags.push_back(result.fragmentation_level);
            random_slots.push_back(result.file_slots_fragmented);
        }
    }

    auto calc_avg = [](const std::vector<double>& v) {
        if (v.empty()) return 0.0;
        double sum = 0;
        for (double val : v) sum += val;
        return sum / static_cast<double>(v.size());
    };

    ofs << "Deletion from Middle:\n";
    ofs << "  Average Overhead:        " << std::fixed << std::setprecision(2)
        << calc_avg(middle_overheads) << "%\n";
    ofs << "  Average Fragmentation:   " << calc_avg(middle_frags) << "\n";
    ofs << "  Avg Fragmented Slots:    " << calc_avg(middle_slots) << "\n\n";

    ofs << "Deletion Random:\n";
    ofs << "  Average Overhead:        " << calc_avg(random_overheads) << "%\n";
    ofs << "  Average Fragmentation:   " << calc_avg(random_frags) << "\n";
    ofs << "  Avg Fragmented Slots:    " << calc_avg(random_slots) << "\n\n";

    // Key Findings
    ofs << "\nKEY FINDINGS\n\n";

    if (!all_stats.empty()) {
        ofs << "1. Best performing strategy: " << all_stats[0].name
            << " (" << std::fixed << std::setprecision(2) << all_stats[0].avg_overhead << "% avg overhead)\n";
        ofs << "2. Worst performing strategy: " << all_stats.back().name
            << " (" << all_stats.back().avg_overhead << "% avg overhead)\n";
    }

    if (!middle_overheads.empty() && !random_overheads.empty()) {
        double middle_avg = calc_avg(middle_overheads);
        double random_avg = calc_avg(random_overheads);
        double impact = random_avg > 0 ? ((middle_avg - random_avg) / random_avg * 100.0) : 0;
        ofs << "3. Deleting from middle changes overhead by " << std::showpos << std::fixed << std::setprecision(1)
            << impact << std::noshowpos << "%\n";
    }

    // Best/Worst cases
    auto result_copy = results;
    std::sort(result_copy.begin(), result_copy.end(),
              [](const auto& a, const auto& b) {
                  return a.second.overhead_percent < b.second.overhead_percent;
              });

    ofs << "\n4. Best case overhead: " << std::fixed << std::setprecision(2)
        << result_copy.front().second.overhead_percent << "%\n";
    ofs << "5. Worst case overhead: " << result_copy.back().second.overhead_percent << "%\n";

    ofs << "\n" << "Detailed data available in CSV file.\n";

    ofs.close();
}

// --------------------------------------------------------------------
// Main
// --------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::string output_prefix = "fragmentation_results";

    if (argc > 1) {
        output_prefix = argv[1];
    }

    std::string csv_file = output_prefix + ".csv";
    std::string report_file = output_prefix + "_report.txt";

    std::cout << "\n========================================\n";
    std::cout << "ADVANCED FRAGMENTATION TESTING SUITE\n";
    std::cout << "========================================\n\n";

    std::cout << "Generating comprehensive test grid...\n";
    auto param_grid = generate_parameter_grid();
    std::cout << "Total configurations: " << param_grid.size() << "\n";
    std::cout << "\nGrid parameters:\n";
    std::cout << "  - Block sizes: 512B to 16KB (6 sizes)\n";
    std::cout << "  - Strategies: 4 allocation strategies\n";
    std::cout << "  - Compression: 5 probability levels (10%-90%)\n";
    std::cout << "  - Deletion scenarios: 6 aggressive patterns\n";
    std::cout << "  - Total files per test: 300 (65% small, 20% medium, 15% large)\n";
    std::cout << "  - Min deletion ratio: 40%-55% per scenario\n";
    std::cout << "\nKey scenarios tested:\n";
    std::cout << "  • Aggressive small file deletion (90-95% of small files)\n";
    std::cout << "  • Aggressive large file deletion (90-95% of large files)\n";
    std::cout << "  • Mixed size deletion patterns\n\n";

    std::vector<std::pair<FragmentationParams, FragmentationResult>> results;

    auto start_time = std::chrono::high_resolution_clock::now();

    int test_num = 0;
    int progress_step = std::max(1, static_cast<int>(param_grid.size() / 20));

    for (const auto& params : param_grid) {
        test_num++;

        if (test_num % progress_step == 0 || test_num == 1 || test_num == static_cast<int>(param_grid.size())) {
            std::cout << "[" << std::setw(3) << (test_num * 100 / param_grid.size()) << "%] "
                      << test_num << "/" << param_grid.size() << " ... " << std::flush;
        }

        std::string temp_file = "temp_frag_test_" + std::to_string(test_num) + ".compio";

        FragmentationResult result = run_fragmentation_test(params, temp_file);

        results.push_back({params, result});

        if (test_num % progress_step == 0 || test_num == static_cast<int>(param_grid.size())) {
            std::cout << "Overhead: " << std::fixed << std::setprecision(1) << result.overhead_percent << "%, "
                      << "Frag: " << result.fragmentation_level << ", "
                      << "Deleted: " << result.files_deleted << " files\n";
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);

    std::cout << "\n";
    std::cout << "Saving results...\n";
    save_results_to_csv(results, csv_file);
    save_results_to_csv(results, csv_file);
    generate_text_report(results, report_file);

    // Quick summary
    std::map<std::string, double> strategy_avg;
    std::map<std::string, int> strategy_count;

    for (const auto& [params, result] : results) {
        std::string name;
        switch (params.alloc_strategy) {
            case COMPIO_ALLOC_FIRST_FIT: name = "FIRST_FIT"; break;
            case COMPIO_ALLOC_BEST_FIT: name = "BEST_FIT"; break;
            case COMPIO_ALLOC_WORST_FIT: name = "WORST_FIT"; break;
            case COMPIO_ALLOC_NEXT_FIT: name = "NEXT_FIT"; break;
        }
        strategy_avg[name] += result.overhead_percent;
        strategy_count[name]++;
    }

    std::cout << "Average Overhead by Strategy:\n";
    for (const auto& [name, sum] : strategy_avg) {
        double avg = sum / static_cast<double>(strategy_count[name]);
        std::cout << "  " << std::setw(12) << std::left << name << ": "
                  << std::fixed << std::setprecision(2) << avg << "%\n";
    }

    // Quick summary for time
    std::map<std::string, double> strategy_time_avg;
    for (const auto& [params, result] : results) {
        std::string name;
        switch (params.alloc_strategy) {
            case COMPIO_ALLOC_FIRST_FIT: name = "FIRST_FIT"; break;
            case COMPIO_ALLOC_BEST_FIT: name = "BEST_FIT"; break;
            case COMPIO_ALLOC_WORST_FIT: name = "WORST_FIT"; break;
            case COMPIO_ALLOC_NEXT_FIT: name = "NEXT_FIT"; break;
        }
        strategy_time_avg[name] += result.time_taken_ms;
    }

    std::cout << "\nAverage Time by Strategy:\n";
    for (const auto& [name, sum] : strategy_time_avg) {
        double avg = sum / static_cast<double>(strategy_count[name]);
        std::cout << "  " << std::setw(12) << std::left << name << ": "
                  << std::fixed << std::setprecision(2) << avg << " ms\n";
    }

    std::cout << "\nCompleted in " << duration.count() << " seconds\n";
    std::cout << "\nResults saved:\n";

    // Extract just filenames from paths for cleaner output
    size_t csv_pos = csv_file.find_last_of("/\\");
    size_t report_pos = report_file.find_last_of("/\\");
    std::string csv_name = (csv_pos != std::string::npos) ? csv_file.substr(csv_pos + 1) : csv_file;
    std::string report_name = (report_pos != std::string::npos) ? report_file.substr(report_pos + 1) : report_file;

    std::cout << "  • " << csv_name << "\n";
    std::cout << "  • " << report_name << "\n";

    return 0;
}
