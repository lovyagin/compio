#include "compio.h"
#include "sample_data.hpp"
#include "compio_file.hpp"
#include "btree.hpp"

#include <cstdlib>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>


#include <benchmark/benchmark.h>

static void BM_ConsecutiveBlockedWrite(benchmark::State& state) {
    const size_t n_blocks = state.range(0);
    const size_t block_size = state.range(1);

    char fn[L_tmpnam];
    tmpnam(fn);

    std::minstd_rand0 rng;
    std::uniform_int_distribution<std::size_t> d(0, sizeof(html_data) - block_size);

    while (state.KeepRunning()) {
        compio_config config;
        compio_build_default_config(&config);

        compio_archive* archive = compio_open_archive(fn, "w+", &config);
        compio_file* file = compio_open_file("A", archive);

        for (std::size_t i = 0; i < n_blocks; ++i) {
            compio_write(html_data + d(rng), block_size, file);
        }

        compio_close_file(file);
        compio_close_archive(archive);
    }

    state.SetBytesProcessed(state.iterations() * n_blocks * block_size);
    state.SetItemsProcessed(state.iterations() * n_blocks);

    remove(fn);
}

BENCHMARK(BM_ConsecutiveBlockedWrite)
    ->Args({4096, 512})
    ->Args({4096, 1024})
    ->Args({4096, 4096})
    ->Args({65536, 1024})
    ->Args({65536, 4096})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

static void BM_RandomBlockedWrite(benchmark::State& state) {
    const size_t n_blocks = state.range(0);
    const size_t block_size = state.range(1);
    const size_t file_size = state.range(2);

    char fn[L_tmpnam];
    tmpnam(fn);

    std::minstd_rand0 rng;
    std::uniform_int_distribution<std::size_t> d1(0, sizeof(html_data) - block_size);
    std::uniform_int_distribution<std::size_t> d2(0, file_size - block_size);

    while (state.KeepRunning()) {
        compio_config config;
        compio_build_default_config(&config);

        compio_archive* archive = compio_open_archive(fn, "w+", &config);
        compio_file* file = compio_open_file("A", archive);

        for (std::size_t i = 0; i < n_blocks; ++i) {
            compio_seek(file, d2(rng), COMP_SEEK_SET);
            compio_write(html_data + d1(rng), block_size, file);
        }

        compio_close_file(file);
        compio_close_archive(archive);
    }

    state.SetBytesProcessed(state.iterations() * n_blocks * block_size);
    state.SetItemsProcessed(state.iterations() * n_blocks);

    remove(fn);
}

BENCHMARK(BM_RandomBlockedWrite)
    ->Args({4096, 512, 4096 * 16})
    ->Args({4096, 1024, 4096 * 16})
    ->Args({4096, 4096, 4096 * 16})
    ->Args({65536, 1024, 65536 * 16})
    ->Args({65536, 4096, 65536 * 16})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
