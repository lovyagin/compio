#include <benchmark/benchmark.h>
#include <cstdio>
#include <string>
#include <vector>
#include <random>
#include "compio.h"

static void BM_FileLookup(benchmark::State& state) {
    const int NUM_FILES = state.range(0);
    std::string filename = "lookup_bench_" + std::to_string(NUM_FILES) + ".compio";
    
    compio_config config;
    compio_build_default_config(&config);
    config.max_files = NUM_FILES + 10;
    
    compio_archive* ar = compio_open_archive(filename.c_str(), "w", &config);
    if (!ar) {
        state.SkipWithError("Failed to create archive");
        return;
    }
    
    // Pre-populate files
    std::vector<std::string> names;
    names.reserve(NUM_FILES);
    bool setup_failed = false;
    for (int i = 0; i < NUM_FILES; ++i) {
        std::string name = "file_" + std::to_string(i);
        compio_file* f = compio_open_file(name.c_str(), ar);
        if (!f) {
            setup_failed = true;
            break;
        }
        compio_close_file(f);
        names.push_back(name);
    }
    compio_close_archive(ar);
    
    if (setup_failed) {
        remove(filename.c_str());
        remove((filename + ".wal").c_str());
        state.SkipWithError("Failed to populate archive");
        return;
    }
    
    // Open for reading (this triggers index build)
    ar = compio_open_archive(filename.c_str(), "r", &config);
    if (!ar) {
        remove(filename.c_str());
        remove((filename + ".wal").c_str());
        state.SkipWithError("Failed to open archive for reading");
        return;
    }
    
    // Benchmark loop: random lookups
    std::mt19937 rng(1337);
    std::uniform_int_distribution<int> dist(0, NUM_FILES - 1);
    
    for (auto _ : state) {
        int idx = dist(rng);
        const std::string& name = names[idx];
        compio_file* f = compio_open_file(name.c_str(), ar);
        if (f) compio_close_file(f);
        else state.SkipWithError("Failed to find existing file");
    }
    
    state.SetItemsProcessed(state.iterations());
    
    compio_close_archive(ar);
    remove(filename.c_str());
    remove((filename + ".wal").c_str());
}

// Test with 1k, 10k, 100k, 1M files (if possible)
BENCHMARK(BM_FileLookup)->Range(1000, 100000); // 100k takes ~200ms setup, manageable
