/**
 * Block Size Comparison Benchmark
 * 
 * Tests different block sizes to find optimal defaults for:
 * 1. Sequential I/O performance
 * 2. Random I/O performance  
 * 3. Fragmentation overhead
 * 4. Space efficiency (compression ratio)
 */

#include <benchmark/benchmark.h>
#include "compio.h"
#include <vector>
#include <random>
#include <cstring>

static void BM_BlockSizeImpact(benchmark::State& state) {
    const int block_size = state.range(0);
    const int num_blocks = state.range(1);
    const bool sequential = state.range(2);
    
    std::string filename = "/tmp/block_test_" + std::to_string(block_size) + ".tmp";
    std::vector<unsigned char> data(block_size);
    
    // Fill with pseudo-random but compressible data
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = (i % 256) ^ ((i / 256) % 128);
    }
    
    for (auto _ : state) {
        state.PauseTiming();
        
        // Create archive with specific block size
        compio_config config = compio_build_default_config();
        config.block_size = block_size;
        config.block_size__minimum = block_size / 2;
        config.block_size__maximum = block_size * 2;
        config.wal_sync_mode = COMPIO_WAL_NORMAL;
        
        compio_archive* archive = compio_create_archive(filename.c_str(), &config);
        if (!archive) {
            state.SkipWithError("Failed to create compio archive");
            state.ResumeTiming();
            continue;
        }
        compio_file* file = compio_open_file(archive, "testfile", "wb");
        if (!file) {
            compio_close_archive(archive);
            state.SkipWithError("Failed to open compio file");
            state.ResumeTiming();
            continue;
        }
        
        state.ResumeTiming();
        
        if (sequential) {
            // Sequential write
            for (int i = 0; i < num_blocks; ++i) {
                compio_write(data.data(), 1, data.size(), file);
            }
        } else {
            // Random write pattern
            std::mt19937 rng(42);
            std::uniform_int_distribution<int> dist(0, num_blocks - 1);
            
            for (int i = 0; i < num_blocks; ++i) {
                int block_idx = dist(rng);
                compio_seek(file, block_idx * block_size, COMPIO_SEEK_SET);
                compio_write(data.data(), 1, data.size(), file);
            }
        }
        
        state.PauseTiming();
        
        uint64_t compressed_size = compio_ftell(file);
        uint64_t uncompressed_size = block_size * num_blocks;
        
        compio_close_file(file);
        compio_close_archive(archive);
        remove(filename.c_str());
        remove((filename + ".wal").c_str());
        
        state.counters["BlockSize_KB"] = block_size / 1024.0;
        state.counters["UncompressedSize_MB"] = uncompressed_size / (1024.0 * 1024.0);
        state.counters["CompressedSize_MB"] = compressed_size / (1024.0 * 1024.0);
        state.counters["CompressionRatio"] = (double)uncompressed_size / compressed_size;
        state.counters["SpaceOverhead_%"] = 
            100.0 * (compressed_size - uncompressed_size * 0.3) / uncompressed_size;
        
        state.ResumeTiming();
    }
    
    state.SetBytesProcessed(state.iterations() * block_size * num_blocks);
}

// Sequential write with different block sizes
BENCHMARK(BM_BlockSizeImpact)
    ->Name("Sequential/4KB")
    ->Args({4096, 1024, 1})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_BlockSizeImpact)
    ->Name("Sequential/16KB")
    ->Args({16384, 256, 1})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_BlockSizeImpact)
    ->Name("Sequential/64KB")
    ->Args({65536, 64, 1})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_BlockSizeImpact)
    ->Name("Sequential/256KB")
    ->Args({262144, 16, 1})
    ->Unit(benchmark::kMillisecond);

// Random write with different block sizes  
BENCHMARK(BM_BlockSizeImpact)
    ->Name("Random/4KB")
    ->Args({4096, 1024, 0})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_BlockSizeImpact)
    ->Name("Random/16KB")
    ->Args({16384, 256, 0})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_BlockSizeImpact)
    ->Name("Random/64KB")
    ->Args({65536, 64, 0})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_BlockSizeImpact)
    ->Name("Random/256KB")
    ->Args({262144, 16, 0})
    ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
