#include <benchmark/benchmark.h>
#include <cstdio>
#include <string>
#include <vector>

#include "compio.h"
#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"

#ifndef COMPIO_SUCCESS
#define COMPIO_SUCCESS 0
#endif

static void fill_pattern(std::vector<uint8_t>& buf, uint8_t seed) {
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = static_cast<uint8_t>((seed + i) & 0xFF);
}

static void create_fragmented_archive(const std::string& path, int num_files,
                                       size_t file_size, size_t block_size,
                                       double delete_ratio, compio_allocation_strategy strat) {
    std::remove(path.c_str());
    compio_config cfg{};
    compio_build_default_config(&cfg);
    cfg.block_size = static_cast<int>(block_size);
    cfg.block_size__minimum = 0;
    cfg.block_size__maximum = cfg.block_size * 4;
    cfg.allocation_strategy = strat;
    cfg.fragmentation_threshold = 1;
    compio_build_dummy_compressor(&cfg.compressor);

    compio_archive* ar = compio_open_archive(path.c_str(), "w+", &cfg);
    if (!ar) return;

    for (int i = 0; i < num_files; ++i) {
        std::string fname = "f_" + std::to_string(i);
        compio_file* f = compio_open_file(fname.c_str(), ar);
        if (!f) continue;
        std::vector<uint8_t> data(file_size);
        fill_pattern(data, static_cast<uint8_t>(i & 0xFF));
        compio_write(data.data(), data.size(), f);
        compio_close_file(f);
    }

    int to_delete = static_cast<int>(num_files * delete_ratio);
    int step = (to_delete > 0) ? std::max(1, num_files / to_delete) : num_files + 1;
    for (int i = 0; i < num_files && to_delete > 0; i += step, --to_delete) {
        compio_remove_file(ar, ("f_" + std::to_string(i)).c_str());
    }

    compio_close_archive(ar);
}

/// Measures only the defragmentation call itself on a pre-fragmented archive.
static void BM_DefragmentationTime(benchmark::State& state) {
    const int num_files      = static_cast<int>(state.range(0));
    const size_t file_size   = static_cast<size_t>(state.range(1));
    const double delete_pct  = state.range(2) / 100.0;

    std::string path = "gbench_defrag_" + std::to_string(num_files) + ".tmp";

    for ([[maybe_unused]] auto _ : state) {
        state.PauseTiming();
        create_fragmented_archive(path, num_files, file_size, 4096, delete_pct, COMPIO_ALLOC_FIRST_FIT);

        compio_config cfg{};
        compio_build_default_config(&cfg);
        cfg.block_size = 4096;
        cfg.block_size__minimum = 0;
        cfg.block_size__maximum = cfg.block_size * 4;
        cfg.fragmentation_threshold = 1;
        compio_build_dummy_compressor(&cfg.compressor);

        compio_archive* ar = compio_open_archive(path.c_str(), "r+", &cfg);
        state.ResumeTiming();

        if (ar) {
            compio_defragment(ar);
            compio_close_archive(ar);
        }
    }

    std::remove(path.c_str());

    state.SetLabel(std::to_string(num_files) + " files, " +
                   std::to_string(static_cast<int>(delete_pct * 100)) + "% deleted");
}

/// Measures fragmentation metric calculation overhead.
static void BM_FragmentationMetric(benchmark::State& state) {
    const int num_files = static_cast<int>(state.range(0));
    std::string path = "gbench_metric_" + std::to_string(num_files) + ".tmp";

    create_fragmented_archive(path, num_files, 2048, 4096, 0.5, COMPIO_ALLOC_FIRST_FIT);

    compio_config cfg{};
    compio_build_default_config(&cfg);
    cfg.block_size = 4096;
    cfg.block_size__minimum = 0;
    cfg.block_size__maximum = cfg.block_size * 4;
    cfg.fragmentation_threshold = 1;
    compio_build_dummy_compressor(&cfg.compressor);

    compio_archive* ar = compio_open_archive(path.c_str(), "r+", &cfg);
    if (!ar) {
        state.SkipWithError("Cannot open archive");
        return;
    }

    for ([[maybe_unused]] auto _ : state) {
        benchmark::DoNotOptimize(ar->allocator->get_fragmentation());
    }

    compio_close_archive(ar);
    std::remove(path.c_str());
}

// --- Registration ---

BENCHMARK(BM_DefragmentationTime)
    ->Args({20,  2048, 50})
    ->Args({40,  2048, 50})
    ->Args({60,  2048, 50})
    ->Args({64,  2048, 50})
    ->Args({50,  2048, 30})
    ->Args({50,  2048, 70})
    ->Args({50,  2048, 90})
    ->Args({50,  8192, 50})
    ->Args({50, 32768, 50})
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3)
    ->Name("DefragTime");

BENCHMARK(BM_FragmentationMetric)
    ->Arg(20)
    ->Arg(40)
    ->Arg(60)
    ->Arg(64)
    ->Unit(benchmark::kNanosecond)
    ->Name("FragMetric");
