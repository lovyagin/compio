#include "compio.h"
#include "sample_data.hpp"
#include "util.hpp"

#include <random>

#include <benchmark/benchmark.h>

static void BM_stdio_ConsecutiveRead(benchmark::State& state) {
    const size_t n_blocks = state.range(0);
    const size_t block_size = state.range(1);

    char fn[L_tmpnam];
    tmpnam(fn);

    std::minstd_rand0 rng(0);
    std::uniform_int_distribution<std::size_t> d(0, sizeof(html_data) - block_size);

    std::vector<char> buffer(block_size);

    {
        // prepare file
        FILE* file = fopen(fn, "w+");
        if (!file) {
            state.SkipWithError("fopen failed");
            return;
        }

        for (std::size_t i = 0; i < n_blocks; ++i) {
            auto bytes = fwrite(html_data + d(rng), 1, block_size, file);
            if (bytes != block_size) {
                fclose(file);
                state.SkipWithError(std::string("fwrite returned ") + std::to_string(bytes) +
                                    std::string(" != ") + std::to_string(block_size));
                return;
            }
        }

        fclose(file);
    }

    state.counters["file_size"] = get_file_size(fn);

    for (auto _ : state) {
        FILE* file = fopen(fn, "r");
        if (!file) {
            state.SkipWithError("fopen failed");
            break;
        }

        bool failed = false;
        for (std::size_t i = 0; i < n_blocks; ++i) {
            auto bytes = fread(buffer.data(), 1, block_size, file);
            if (bytes != block_size) {
                fclose(file);
                state.SkipWithError(std::string("fread returned ") + std::to_string(bytes) +
                                    std::string(" != ") + std::to_string(block_size));
                failed = true;
                break;
            }
        }

        if (failed) {
            break;
        }

        fclose(file);
    }

    state.SetBytesProcessed(state.iterations() * n_blocks * block_size);

    remove(fn);
}

static void BM_compio_ConsecutiveRead(benchmark::State& state) {
    const size_t n_blocks = state.range(0);
    const size_t block_size = state.range(1);

    char fn[L_tmpnam];
    tmpnam(fn);

    std::minstd_rand0 rng(0);
    std::uniform_int_distribution<std::size_t> d(0, sizeof(html_data) - block_size);

    std::vector<char> buffer(block_size);

    {
        // prepare file
        compio_config config;
        compio_build_default_config(&config);

        compio_archive* archive = compio_open_archive(fn, "w+", &config);
        if (!archive) {
            state.SkipWithError("compio_open_archive failed");
            return;
        }

        compio_file* file = compio_open_file("A", archive);
        if (!file) {
            compio_close_archive(archive);
            state.SkipWithError("compio_open_file failed");
            return;
        }

        for (std::size_t i = 0; i < n_blocks; ++i) {
            auto bytes = compio_write(html_data + d(rng), block_size, file);
            if (bytes != block_size) {
                compio_close_file(file);
                compio_close_archive(archive);
                state.SkipWithError(std::string("compio_write returned ") + std::to_string(bytes) +
                                    std::string(" != ") + std::to_string(block_size));
                return;
            }
        }

        compio_close_file(file);
        compio_close_archive(archive);
    }

    state.counters["file_size"] = get_file_size(fn);

    for (auto _ : state) {
        compio_config config;
        compio_build_default_config(&config);

        compio_archive* archive = compio_open_archive(fn, "r", &config);
        if (!archive) {
            state.SkipWithError("compio_open_archive failed");
            break;
        }

        compio_file* file = compio_open_file("A", archive);
        if (!file) {
            compio_close_archive(archive);
            state.SkipWithError("compio_open_file failed");
            break;
        }

        bool failed = false;
        for (std::size_t i = 0; i < n_blocks; ++i) {
            auto bytes = compio_read(buffer.data(), block_size, file);
            if (bytes != block_size) {
                compio_close_file(file);
                compio_close_archive(archive);
                state.SkipWithError(std::string("compio_write returned ") + std::to_string(bytes) +
                                    std::string(" != ") + std::to_string(block_size));
                failed = true;
                break;
            }
        }

        if (failed) {
            break;
        }

        compio_close_file(file);
        compio_close_archive(archive);
    }

    state.SetBytesProcessed(state.iterations() * n_blocks * block_size);

    remove(fn);
}

const std::vector<std::vector<int64_t>> params_grid = {{128, 1024}, {256, 512, 1024}};

BENCHMARK(BM_stdio_ConsecutiveRead)
    ->ArgsProduct(params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK(BM_compio_ConsecutiveRead)
    ->ArgsProduct(params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();