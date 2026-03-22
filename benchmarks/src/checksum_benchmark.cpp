#include <benchmark/benchmark.h>
#include <vector>
#include <random>
#include "compio/utils.hpp"

// Generate random data for benchmarking
static std::vector<uint8_t> generate_data(size_t size) {
    std::vector<uint8_t> data(size);
    std::mt19937 rng(42); // Fixed seed for reproducibility
    std::uniform_int_distribution<unsigned int> dist(0, 255);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>(dist(rng));
    }
    return data;
}

static void BM_FNV1a_32(benchmark::State& state) {
    size_t size = state.range(0);
    auto data = generate_data(size);
    
    for (auto _ : state) {
        benchmark::DoNotOptimize(compio::fnv1a_32(data.data(), size));
    }
    state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(size));
}

static void BM_CRC32C(benchmark::State& state) {
    size_t size = state.range(0);
    auto data = generate_data(size);
    
    for (auto _ : state) {
        benchmark::DoNotOptimize(compio::crc32c(data.data(), size));
    }
    state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(size));
}

// Register benchmarks with various sizes
// 64B, 1KB, 4KB (page size), 64KB (default block size), 1MB
BENCHMARK(BM_FNV1a_32)->RangeMultiplier(4)->Range(64, 1024 * 1024);
BENCHMARK(BM_CRC32C)->RangeMultiplier(4)->Range(64, 1024 * 1024);
