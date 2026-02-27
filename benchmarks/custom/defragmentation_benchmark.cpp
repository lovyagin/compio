/**
 * @file defragmentation_benchmark.cpp
 * @brief Comprehensive defragmentation benchmarks
 *
 * Measures defragmentation performance across multiple dimensions:
 * 1. Speed: time to defragment archives of varying sizes
 * 2. Space reclamation: bytes recovered by compaction
 * 3. Data integrity: verify all data survives defragmentation
 * 4. Scalability: how defrag time grows with archive size
 * 5. Fragmentation metric: accuracy of calculate_fragmentation()
 * 6. Repeated cycles: defrag stability over multiple create/delete/defrag cycles
 *
 * Output: human-readable table + CSV file for analysis.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "compio.h"
#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"

// ============================================================================
// Utilities
// ============================================================================

static size_t file_size_on_disk(const char* path) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<size_t>(sz);
}

static void fill_pattern(std::vector<uint8_t>& buf, uint8_t seed) {
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = static_cast<uint8_t>((seed + i) & 0xFF);
}

struct Timer {
    using clock = std::chrono::high_resolution_clock;
    clock::time_point t0;
    Timer() : t0(clock::now()) {}
    double elapsed_ms() const {
        return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    }
};

// ============================================================================
// Scenario configuration
// ============================================================================

struct Scenario {
    std::string name;
    int num_files;            // total files to create
    size_t file_size;         // bytes per file (pre-compression)
    size_t block_size;        // archive block size
    double delete_ratio;      // fraction of files to delete (0.0–1.0)
    bool use_compression;     // use zlib vs dummy compressor
    compio_allocation_strategy strategy;
};

// ============================================================================
// Single benchmark result
// ============================================================================

struct Result {
    std::string scenario;
    int num_files;
    size_t file_size;
    size_t block_size;
    double delete_ratio;
    std::string strategy_name;
    bool compressed;

    // pre-defrag state
    uint8_t frag_metric_before;    // calculate_fragmentation() [0..100]
    size_t disk_size_before;       // on-disk file size before defrag

    // defrag timing
    double defrag_time_ms;

    // post-defrag state
    uint8_t frag_metric_after;
    size_t disk_size_after;

    // derived
    size_t space_reclaimed;        // bytes freed
    double space_reclaimed_pct;    // percentage
    bool data_intact;              // all files readable & correct after defrag
    int files_surviving;           // files still present after deletions
};

// ============================================================================
// Run a single scenario
// ============================================================================

static const char* strategy_str(compio_allocation_strategy s) {
    switch (s) {
        case COMPIO_ALLOC_FIRST_FIT: return "first_fit";
        case COMPIO_ALLOC_BEST_FIT:  return "best_fit";
        case COMPIO_ALLOC_WORST_FIT: return "worst_fit";
        case COMPIO_ALLOC_NEXT_FIT:  return "next_fit";
        default: return "unknown";
    }
}

static Result run_scenario(const Scenario& sc) {
    Result r{};
    r.scenario      = sc.name;
    r.num_files     = sc.num_files;
    r.file_size     = sc.file_size;
    r.block_size    = sc.block_size;
    r.delete_ratio  = sc.delete_ratio;
    r.strategy_name = strategy_str(sc.strategy);
    r.compressed    = sc.use_compression;

    std::string archive_path = "bench_defrag_" + sc.name + ".tmp";
    std::remove(archive_path.c_str());

    // --- configure archive ---
    compio_config cfg{};
    compio_build_default_config(&cfg);
    cfg.block_size           = static_cast<int>(sc.block_size);
    cfg.block_size__minimum  = 0;
    cfg.block_size__maximum  = cfg.block_size * 4;
    cfg.allocation_strategy  = sc.strategy;
    cfg.fragmentation_threshold = 1; // very low so maintenance won't auto-defrag

    if (sc.use_compression)
        compio_build_zlib_compressor(&cfg.compressor);
    else
        compio_build_dummy_compressor(&cfg.compressor);

    compio_archive* ar = compio_open_archive(archive_path.c_str(), "w+", &cfg);
    if (!ar) {
        std::cerr << "[ERROR] Cannot open archive for scenario: " << sc.name << "\n";
        r.data_intact = false;
        return r;
    }

    std::mt19937 rng(42);

    // --- Phase 1: create files with deterministic content ---
    // Content for file i = pattern seeded with i, so we can verify later
    for (int i = 0; i < sc.num_files; ++i) {
        std::string fname = "f_" + std::to_string(i);
        compio_file* f = compio_open_file(fname.c_str(), ar);
        if (!f) continue;

        std::vector<uint8_t> data(sc.file_size);
        fill_pattern(data, static_cast<uint8_t>(i & 0xFF));
        compio_write(data.data(), data.size(), f);
        compio_close_file(f);
    }
    compio_flush(ar);

    // --- Phase 2: delete files to create fragmentation ---
    int num_to_delete = static_cast<int>(sc.num_files * sc.delete_ratio);
    // Delete every other file for maximum fragmentation (interleaved gaps)
    std::vector<int> deleted;
    int step = (num_to_delete > 0) ? std::max(1, sc.num_files / num_to_delete) : sc.num_files + 1;
    for (int i = 0; i < sc.num_files && static_cast<int>(deleted.size()) < num_to_delete; i += step) {
        std::string fname = "f_" + std::to_string(i);
        compio_remove_file(ar, fname.c_str());
        deleted.push_back(i);
    }
    compio_flush(ar);

    r.files_surviving = sc.num_files - static_cast<int>(deleted.size());
    r.frag_metric_before = ar->allocator->get_fragmentation();
    r.disk_size_before   = file_size_on_disk(archive_path.c_str());

    // --- Phase 3: defragment and measure time ---
    {
        Timer t;
        int rc = compio_defragment(ar);
        r.defrag_time_ms = t.elapsed_ms();
        if (rc != 0) {
            std::cerr << "[WARN] compio_defragment returned " << rc << " for " << sc.name << "\n";
        }
    }

    compio_flush(ar);
    r.frag_metric_after = ar->allocator->get_fragmentation();
    r.disk_size_after   = file_size_on_disk(archive_path.c_str());
    r.space_reclaimed   = (r.disk_size_before > r.disk_size_after)
                            ? (r.disk_size_before - r.disk_size_after) : 0;
    r.space_reclaimed_pct = (r.disk_size_before > 0)
                            ? 100.0 * r.space_reclaimed / r.disk_size_before : 0.0;

    // --- Phase 4: verify data integrity ---
    r.data_intact = true;
    std::set<int> deleted_set(deleted.begin(), deleted.end());
    for (int i = 0; i < sc.num_files; ++i) {
        if (deleted_set.count(i)) continue;
        std::string fname = "f_" + std::to_string(i);
        compio_file* f = compio_open_file(fname.c_str(), ar);
        if (!f) {
            std::cerr << "[INTEGRITY] Cannot open " << fname << " after defrag in " << sc.name << "\n";
            r.data_intact = false;
            continue;
        }
        size_t sz = compio_get_size(f);
        if (sz != sc.file_size) {
            std::cerr << "[INTEGRITY] Size mismatch for " << fname << ": expected "
                      << sc.file_size << ", got " << sz << "\n";
            r.data_intact = false;
            compio_close_file(f);
            continue;
        }
        std::vector<uint8_t> buf(sz);
        compio_read(buf.data(), sz, f);
        compio_close_file(f);

        std::vector<uint8_t> expected(sc.file_size);
        fill_pattern(expected, static_cast<uint8_t>(i & 0xFF));
        if (buf != expected) {
            std::cerr << "[INTEGRITY] Data mismatch for " << fname << " in " << sc.name << "\n";
            r.data_intact = false;
        }
    }

    compio_close_archive(ar);
    std::remove(archive_path.c_str());
    return r;
}

// ============================================================================
// Cycle benchmark: repeated create/delete/defrag
// ============================================================================

struct CycleResult {
    int cycle;
    double defrag_time_ms;
    size_t disk_size_before;
    size_t disk_size_after;
    uint8_t frag_before;
    uint8_t frag_after;
    bool data_intact;
};

static std::vector<CycleResult> run_cycle_benchmark(int num_cycles, int files_per_cycle,
                                                     size_t file_size, size_t block_size) {
    std::vector<CycleResult> results;
    std::string path = "bench_defrag_cycles.tmp";
    std::remove(path.c_str());

    compio_config cfg{};
    compio_build_default_config(&cfg);
    cfg.block_size           = static_cast<int>(block_size);
    cfg.block_size__minimum  = 0;
    cfg.block_size__maximum  = cfg.block_size * 4;
    cfg.allocation_strategy  = COMPIO_ALLOC_FIRST_FIT;
    cfg.fragmentation_threshold = 1;
    compio_build_dummy_compressor(&cfg.compressor);

    compio_archive* ar = compio_open_archive(path.c_str(), "w+", &cfg);
    if (!ar) return results;

    int global_id = 0;
    std::map<int, std::vector<uint8_t>> live_files; // id -> expected data

    std::mt19937 rng(123);

    for (int cycle = 0; cycle < num_cycles; ++cycle) {
        // Create new files
        for (int i = 0; i < files_per_cycle; ++i) {
            int id = global_id++;
            std::string fname = "cf_" + std::to_string(id);
            compio_file* f = compio_open_file(fname.c_str(), ar);
            if (!f) continue;
            std::vector<uint8_t> data(file_size);
            fill_pattern(data, static_cast<uint8_t>(id & 0xFF));
            compio_write(data.data(), data.size(), f);
            compio_close_file(f);
            live_files[id] = std::move(data);
        }

        // Delete ~50% of all live files
        std::vector<int> live_ids;
        for (auto& [id, _] : live_files) live_ids.push_back(id);
        std::shuffle(live_ids.begin(), live_ids.end(), rng);
        int to_delete = static_cast<int>(live_ids.size()) / 2;
        for (int i = 0; i < to_delete; ++i) {
            compio_remove_file(ar, ("cf_" + std::to_string(live_ids[i])).c_str());
            live_files.erase(live_ids[i]);
        }
        compio_flush(ar);

        CycleResult cr{};
        cr.cycle = cycle;
        cr.frag_before      = ar->allocator->get_fragmentation();
        cr.disk_size_before  = file_size_on_disk(path.c_str());

        {
            Timer t;
            compio_defragment(ar);
            cr.defrag_time_ms = t.elapsed_ms();
        }
        compio_flush(ar);

        cr.frag_after       = ar->allocator->get_fragmentation();
        cr.disk_size_after  = file_size_on_disk(path.c_str());

        // Verify all live files
        cr.data_intact = true;
        for (auto& [id, expected] : live_files) {
            compio_file* f = compio_open_file(("cf_" + std::to_string(id)).c_str(), ar);
            if (!f) { cr.data_intact = false; continue; }
            size_t sz = compio_get_size(f);
            std::vector<uint8_t> buf(sz);
            compio_read(buf.data(), sz, f);
            compio_close_file(f);
            if (buf != expected) cr.data_intact = false;
        }

        results.push_back(cr);
    }

    compio_close_archive(ar);
    std::remove(path.c_str());
    return results;
}

// ============================================================================
// Reporting
// ============================================================================

static void print_header() {
    std::cout << "\n"
              << std::left
              << std::setw(28) << "Scenario"
              << std::setw(8)  << "Files"
              << std::setw(10) << "FileSize"
              << std::setw(8)  << "BlkSz"
              << std::setw(7)  << "Del%"
              << std::setw(11) << "Strategy"
              << std::setw(6)  << "Compr"
              << std::setw(10) << "Frag%Pre"
              << std::setw(10) << "Frag%Post"
              << std::setw(12) << "DiskPre(KB)"
              << std::setw(12) << "DiskPost(KB)"
              << std::setw(12) << "Reclaim(KB)"
              << std::setw(10) << "Reclaim%"
              << std::setw(12) << "Time(ms)"
              << std::setw(8)  << "Intact"
              << "\n"
              << std::string(164, '-') << "\n";
}

static void print_result(const Result& r) {
    std::cout << std::left
              << std::setw(28) << r.scenario
              << std::setw(8)  << r.num_files
              << std::setw(10) << r.file_size
              << std::setw(8)  << r.block_size
              << std::setw(7)  << static_cast<int>(r.delete_ratio * 100)
              << std::setw(11) << r.strategy_name
              << std::setw(6)  << (r.compressed ? "yes" : "no")
              << std::setw(10) << static_cast<int>(r.frag_metric_before)
              << std::setw(10) << static_cast<int>(r.frag_metric_after)
              << std::setw(12) << std::fixed << std::setprecision(1) << (r.disk_size_before / 1024.0)
              << std::setw(12) << std::fixed << std::setprecision(1) << (r.disk_size_after / 1024.0)
              << std::setw(12) << std::fixed << std::setprecision(1) << (r.space_reclaimed / 1024.0)
              << std::setw(10) << std::fixed << std::setprecision(1) << r.space_reclaimed_pct
              << std::setw(12) << std::fixed << std::setprecision(2) << r.defrag_time_ms
              << std::setw(8)  << (r.data_intact ? "OK" : "FAIL")
              << "\n";
}

static void write_csv(const std::vector<Result>& results, const std::string& path) {
    std::ofstream csv(path);
    csv << "scenario,num_files,file_size,block_size,delete_ratio,strategy,compressed,"
        << "frag_before,frag_after,disk_before_kb,disk_after_kb,reclaimed_kb,reclaimed_pct,"
        << "defrag_time_ms,data_intact,files_surviving\n";
    for (auto& r : results) {
        csv << r.scenario << ","
            << r.num_files << ","
            << r.file_size << ","
            << r.block_size << ","
            << r.delete_ratio << ","
            << r.strategy_name << ","
            << (r.compressed ? 1 : 0) << ","
            << static_cast<int>(r.frag_metric_before) << ","
            << static_cast<int>(r.frag_metric_after) << ","
            << std::fixed << std::setprecision(1) << (r.disk_size_before / 1024.0) << ","
            << (r.disk_size_after / 1024.0) << ","
            << (r.space_reclaimed / 1024.0) << ","
            << std::setprecision(1) << r.space_reclaimed_pct << ","
            << std::setprecision(2) << r.defrag_time_ms << ","
            << (r.data_intact ? 1 : 0) << ","
            << r.files_surviving << "\n";
    }
    std::cout << "\nCSV saved to: " << path << "\n";
}

// ============================================================================
// Scenario definitions
// ============================================================================

static std::vector<Scenario> build_scenarios() {
    std::vector<Scenario> scenarios;

    // Note: COMPIO_MAX_FILES = 64, so we stay within that limit

    // --- Scalability: vary file count ---
    for (int n : {10, 30, 50, 60}) {
        scenarios.push_back({
            "scale_" + std::to_string(n) + "files",
            n, 2048, 4096, 0.5, false, COMPIO_ALLOC_FIRST_FIT
        });
    }

    // --- Delete ratio impact ---
    for (double dr : {0.1, 0.3, 0.5, 0.7, 0.9}) {
        int pct = static_cast<int>(dr * 100);
        scenarios.push_back({
            "delratio_" + std::to_string(pct) + "pct",
            60, 2048, 4096, dr, false, COMPIO_ALLOC_FIRST_FIT
        });
    }

    // --- Block size impact ---
    for (size_t bs : {1024, 2048, 4096, 8192}) {
        scenarios.push_back({
            "blksz_" + std::to_string(bs),
            60, 2048, bs, 0.5, false, COMPIO_ALLOC_FIRST_FIT
        });
    }

    // --- File size impact ---
    for (size_t fs : {512, 2048, 8192, 32768}) {
        scenarios.push_back({
            "filesz_" + std::to_string(fs),
            40, fs, 4096, 0.5, false, COMPIO_ALLOC_FIRST_FIT
        });
    }

    // --- Strategy comparison ---
    for (auto strat : {COMPIO_ALLOC_FIRST_FIT, COMPIO_ALLOC_BEST_FIT,
                        COMPIO_ALLOC_WORST_FIT, COMPIO_ALLOC_NEXT_FIT}) {
        scenarios.push_back({
            std::string("strat_") + strategy_str(strat),
            60, 2048, 4096, 0.5, false, strat
        });
    }

    // --- Compression impact ---
    scenarios.push_back({"compr_off", 60, 4096, 4096, 0.5, false, COMPIO_ALLOC_FIRST_FIT});
    scenarios.push_back({"compr_on",  60, 4096, 4096, 0.5, true,  COMPIO_ALLOC_FIRST_FIT});

    // --- Stress: max files ---
    scenarios.push_back({"stress_max64", 64, 4096, 4096, 0.5, false, COMPIO_ALLOC_FIRST_FIT});

    // --- Edge: delete nearly everything ---
    scenarios.push_back({"edge_del95", 60, 2048, 4096, 0.95, false, COMPIO_ALLOC_FIRST_FIT});

    // --- Edge: no deletions (no fragmentation) ---
    scenarios.push_back({"edge_del0", 60, 2048, 4096, 0.0, false, COMPIO_ALLOC_FIRST_FIT});

    // --- Large files (stress I/O throughput) ---
    scenarios.push_back({"large_files", 30, 65536, 4096, 0.5, false, COMPIO_ALLOC_FIRST_FIT});

    return scenarios;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== Compio Defragmentation Benchmark ===\n";

    auto scenarios = build_scenarios();
    std::vector<Result> results;
    results.reserve(scenarios.size());

    // --- Part 1: single-pass scenarios ---
    std::cout << "\n--- Part 1: Single-pass defragmentation scenarios ---\n";
    print_header();

    int total = static_cast<int>(scenarios.size());
    int idx = 0;
    for (auto& sc : scenarios) {
        ++idx;
        std::cerr << "\r  Running " << idx << "/" << total << ": " << sc.name << "...          ";
        Result r = run_scenario(sc);
        results.push_back(r);
        print_result(r);
    }
    std::cerr << "\r  Done.                                          \n";

    // --- Part 2: cycle benchmark ---
    std::cout << "\n--- Part 2: Repeated create/delete/defrag cycles ---\n";
    std::cout << "  (10 cycles, 20 files/cycle, 2KB files, 4KB blocks)\n\n";
    std::cout << std::left
              << std::setw(8)  << "Cycle"
              << std::setw(10) << "Frag%Pre"
              << std::setw(10) << "Frag%Post"
              << std::setw(12) << "DiskPre(KB)"
              << std::setw(12) << "DiskPost(KB)"
              << std::setw(12) << "Time(ms)"
              << std::setw(8)  << "Intact"
              << "\n"
              << std::string(72, '-') << "\n";

    auto cycles = run_cycle_benchmark(10, 20, 2048, 4096);
    for (auto& c : cycles) {
        std::cout << std::left
                  << std::setw(8)  << c.cycle
                  << std::setw(10) << static_cast<int>(c.frag_before)
                  << std::setw(10) << static_cast<int>(c.frag_after)
                  << std::setw(12) << std::fixed << std::setprecision(1) << (c.disk_size_before / 1024.0)
                  << std::setw(12) << (c.disk_size_after / 1024.0)
                  << std::setw(12) << std::setprecision(2) << c.defrag_time_ms
                  << std::setw(8)  << (c.data_intact ? "OK" : "FAIL")
                  << "\n";
    }

    // --- Summary ---
    int integrity_ok = 0;
    for (auto& r : results) if (r.data_intact) ++integrity_ok;
    bool all_cycles_ok = std::all_of(cycles.begin(), cycles.end(),
                                      [](const CycleResult& c) { return c.data_intact; });

    std::cout << "\n=== Summary ===\n"
              << "  Scenarios:  " << results.size() << "\n"
              << "  Integrity:  " << integrity_ok << "/" << results.size()
              << (integrity_ok == static_cast<int>(results.size()) ? " (ALL OK)" : " (FAILURES!)") << "\n"
              << "  Cycles:     " << cycles.size() << " — "
              << (all_cycles_ok ? "ALL OK" : "FAILURES!") << "\n";

    // --- Write CSV ---
    write_csv(results, "defrag_benchmark_results.csv");

    return (integrity_ok == static_cast<int>(results.size()) && all_cycles_ok) ? 0 : 1;
}
