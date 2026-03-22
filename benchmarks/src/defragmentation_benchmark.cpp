#include "benchmark_util.hpp"

#include "compio.h"
#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"

#include <benchmark/benchmark.h>
#include <chrono>
#include <random>
#include <string>
#include <vector>

extern compio_config config;

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

static void fill_pattern(std::vector<uint8_t>& buf, uint8_t seed) {
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = static_cast<uint8_t>((seed + i) & 0xFF);
}

using hrclock = std::chrono::high_resolution_clock;

/// Creates a fragmented archive and flushes it so disk size is accurate.
/// Returns open archive handle. Caller must close and remove.
static compio_archive* create_fragmented(const std::string& path, const compio_config& cfg,
                                          int num_files, size_t file_size, double del_ratio) {
    compio_archive* ar = compio_open_archive(path.c_str(), "w+", &cfg);
    if (!ar) return nullptr;

    for (int i = 0; i < num_files; ++i) {
        compio_file* f = compio_open_file(("f_" + std::to_string(i)).c_str(), ar);
        if (!f) continue;
        std::vector<uint8_t> data(file_size);
        fill_pattern(data, static_cast<uint8_t>(i & 0xFF));
        compio_write(data.data(), data.size(), f);
        compio_close_file(f);
    }

    int to_delete = static_cast<int>(num_files * del_ratio);
    int step = (to_delete > 0) ? std::max(1, num_files / to_delete) : num_files + 1;
    for (int i = 0, d = 0; i < num_files && d < to_delete; i += step, ++d)
        compio_remove_file(ar, ("f_" + std::to_string(i)).c_str());

    compio_flush(ar);
    return ar;
}

// ----------------------------------------------------------------------------
// BM_DefragThroughput
//
// Measures defragmentation throughput. Setup (archive creation) is included
// in the wall-clock time, but the actual defrag-only time is reported via
// the "defrag_ms" counter using manual timing.
//
// bytes_per_second is based on defrag-only time, not total iteration time.
//
// Args: [num_files, file_size_bytes, delete_percent]
// ----------------------------------------------------------------------------

static void BM_DefragThroughput(benchmark::State& state) {
    const int num_files     = static_cast<int>(state.range(0));
    const size_t file_size  = static_cast<size_t>(state.range(1));
    const double del_pct    = state.range(2) / 100.0;
    const int64_t data_bytes = static_cast<int64_t>(num_files * (1.0 - del_pct) * file_size);

    compio_config cfg = config;
    cfg.fragmentation_threshold = 100;
    compio_build_dummy_compressor(&cfg.compressor);

    double total_defrag_ns = 0;
    int64_t iterations = 0;

    for (auto _ : state) {
        std::string path = get_temporary_filename();
        compio_archive* ar = create_fragmented(path, cfg, num_files, file_size, del_pct);
        if (!ar) { state.SkipWithError("Cannot create archive"); break; }

        size_t disk_before = get_file_size(path.c_str());
        // Disk overhead = how much bigger the file is than pure user data
        size_t useful_bytes = static_cast<size_t>(num_files * (1.0 - del_pct) * file_size);
        double overhead_pct = disk_before > 0
            ? 100.0 * (1.0 - static_cast<double>(useful_bytes) / disk_before) : 0;

        auto t0 = hrclock::now();
        compio_defragment(ar);
        auto t1 = hrclock::now();
        total_defrag_ns += std::chrono::duration<double, std::nano>(t1 - t0).count();

        size_t disk_after = get_file_size(path.c_str());
        double overhead_after = disk_after > 0
            ? 100.0 * (1.0 - static_cast<double>(useful_bytes) / disk_after) : 0;

        state.counters["overhead%_before"] = overhead_pct;
        state.counters["overhead%_after"]  = overhead_after;
        state.counters["reclaimed"] = benchmark::Counter(
            static_cast<double>(disk_before > disk_after ? disk_before - disk_after : 0),
            benchmark::Counter::kDefaults, benchmark::Counter::kIs1024);

        compio_close_archive(ar);
        remove(path.c_str());
        remove((path + ".wal").c_str());
        ++iterations;
    }

    double defrag_sec = total_defrag_ns / 1e9;
    state.counters["defrag_ms"] = (total_defrag_ns / 1e6) / iterations;
    state.counters["defrag_throughput"] = benchmark::Counter(
        static_cast<double>(iterations * data_bytes) / defrag_sec,
        benchmark::Counter::kDefaults, benchmark::Counter::kIs1024);
    state.SetBytesProcessed(iterations * data_bytes);
}

// ----------------------------------------------------------------------------
// BM_DefragSpaceRecovery
//
// Measures space reclamation. Archive is flushed before disk_before
// measurement to ensure allocator state is on disk.
//
// Args: [num_files, delete_percent]
// ----------------------------------------------------------------------------

static void BM_DefragSpaceRecovery(benchmark::State& state) {
    const int num_files = static_cast<int>(state.range(0));
    const double del_pct = state.range(1) / 100.0;
    const size_t file_size = 2048;

    compio_config cfg = config;
    cfg.fragmentation_threshold = 100;
    compio_build_dummy_compressor(&cfg.compressor);

    for (auto _ : state) {
        std::string path = get_temporary_filename();
        compio_archive* ar = create_fragmented(path, cfg, num_files, file_size, del_pct);
        if (!ar) { state.SkipWithError("Cannot create archive"); break; }

        size_t disk_before = get_file_size(path.c_str());

        compio_defragment(ar);
        compio_flush(ar);

        size_t disk_after = get_file_size(path.c_str());
        size_t reclaimed = disk_before > disk_after ? disk_before - disk_after : 0;

        state.counters["disk_before"] = benchmark::Counter(
            static_cast<double>(disk_before), benchmark::Counter::kDefaults, benchmark::Counter::kIs1024);
        state.counters["disk_after"] = benchmark::Counter(
            static_cast<double>(disk_after), benchmark::Counter::kDefaults, benchmark::Counter::kIs1024);
        state.counters["recovery%"] = disk_before > 0
            ? 100.0 * static_cast<double>(reclaimed) / static_cast<double>(disk_before) : 0.0;

        compio_close_archive(ar);
        remove(path.c_str());
        remove((path + ".wal").c_str());
    }

    state.SetBytesProcessed(
        state.iterations() * static_cast<int64_t>(num_files * (1.0 - del_pct) * file_size));
}

// ----------------------------------------------------------------------------
// BM_FragmentationMetric
//
// Microbenchmark for get_fragmentation() overhead.
// Archive created once outside loop; only the metric call is timed.
//
// Args: [num_files]
// ----------------------------------------------------------------------------

static void BM_FragmentationMetric(benchmark::State& state) {
    const int num_files = static_cast<int>(state.range(0));

    compio_config cfg = config;
    cfg.fragmentation_threshold = 100;
    compio_build_dummy_compressor(&cfg.compressor);

    std::string path = get_temporary_filename();
    compio_archive* ar = create_fragmented(path, cfg, num_files, 2048, 0.5);
    if (!ar) { state.SkipWithError("Cannot create archive"); return; }

    for (auto _ : state) {
        benchmark::DoNotOptimize(ar->allocator->get_fragmentation());
    }

    state.counters["fragmentation"] = ar->allocator->get_fragmentation();
    compio_close_archive(ar);
    remove(path.c_str());
    remove((path + ".wal").c_str());
}

// ----------------------------------------------------------------------------
// BM_DefragCycle
//
// Repeated create/delete/defrag cycles on a single archive.
// Measures total lifecycle time; reports defrag-only time via counter.
//
// Args: [files_per_cycle, num_cycles]
// ----------------------------------------------------------------------------

static void BM_DefragCycle(benchmark::State& state) {
    const int files_per_cycle = static_cast<int>(state.range(0));
    const int num_cycles      = static_cast<int>(state.range(1));
    const size_t file_size = 2048;

    compio_config cfg = config;
    cfg.fragmentation_threshold = 100;
    compio_build_dummy_compressor(&cfg.compressor);

    std::mt19937 rng(42);
    double total_defrag_ns = 0;
    int64_t total_data = 0;

    for (auto _ : state) {
        std::string path = get_temporary_filename();
        compio_archive* ar = compio_open_archive(path.c_str(), "w+", &cfg);
        if (!ar) { state.SkipWithError("Cannot open archive"); break; }

        int global_id = 0;
        std::vector<int> live_ids;

        for (int c = 0; c < num_cycles; ++c) {
            int to_create = std::min(files_per_cycle,
                                      COMPIO_MAX_FILES - static_cast<int>(live_ids.size()));
            for (int i = 0; i < to_create; ++i) {
                int id = global_id++;
                compio_file* f = compio_open_file(("f_" + std::to_string(id)).c_str(), ar);
                if (!f) continue;
                std::vector<uint8_t> data(file_size);
                fill_pattern(data, static_cast<uint8_t>(id & 0xFF));
                compio_write(data.data(), data.size(), f);
                compio_close_file(f);
                live_ids.push_back(id);
            }

            std::shuffle(live_ids.begin(), live_ids.end(), rng);
            int to_delete = static_cast<int>(live_ids.size()) / 2;
            for (int i = 0; i < to_delete; ++i) {
                compio_remove_file(ar, ("f_" + std::to_string(live_ids.back())).c_str());
                live_ids.pop_back();
            }

            auto t0 = hrclock::now();
            compio_defragment(ar);
            total_defrag_ns += std::chrono::duration<double, std::nano>(hrclock::now() - t0).count();
        }

        total_data = static_cast<int64_t>(live_ids.size()) * static_cast<int64_t>(file_size);

        compio_flush(ar);
        state.counters["live_files"] = static_cast<double>(live_ids.size());
        state.counters["disk_size"] = benchmark::Counter(
            static_cast<double>(get_file_size(path.c_str())),
            benchmark::Counter::kDefaults, benchmark::Counter::kIs1024);
        state.counters["defrag_ms_total"] = total_defrag_ns / 1e6;

        compio_close_archive(ar);
        remove(path.c_str());
        remove((path + ".wal").c_str());
    }

    state.SetBytesProcessed(state.iterations() * total_data);
}

// ----------------------------------------------------------------------------
// Registration
// ----------------------------------------------------------------------------

BENCHMARK(BM_DefragThroughput)
    ->ArgsProduct({
        {10, 30, 60},         // num_files (within COMPIO_MAX_FILES=64)
        {1024, 4096, 16384},  // file_size
        {50},                 // delete_percent
    })
    ->Unit(benchmark::kMillisecond)
    ->MinTime(0.5)
    ->MinWarmUpTime(0.1)
    ->UseRealTime();

BENCHMARK(BM_DefragSpaceRecovery)
    ->ArgsProduct({
        {30, 60},              // num_files
        {10, 30, 50, 70, 90}, // delete_percent
    })
    ->Unit(benchmark::kMillisecond)
    ->MinTime(0.5)
    ->MinWarmUpTime(0.1)
    ->UseRealTime();

BENCHMARK(BM_FragmentationMetric)
    ->Arg(20)
    ->Arg(60)
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BM_DefragCycle)
    ->ArgsProduct({
        {10, 20},  // files_per_cycle
        {5, 10},   // num_cycles
    })
    ->Unit(benchmark::kMillisecond)
    ->MinTime(0.5)
    ->MinWarmUpTime(0.1)
    ->UseRealTime();
