#include "compio.h"
#include "sample_data.hpp"

#include <random>

#include <benchmark/benchmark.h>

static void BM_stdio_RandomWrite(benchmark::State& state) {
    const size_t n_blocks = state.range(0);
    const size_t block_size = state.range(1);
    const size_t file_size = state.range(2);

    char fn[L_tmpnam];
    tmpnam(fn);

    std::minstd_rand0 rng(0);
    std::uniform_int_distribution<std::size_t> d1(0, sizeof(html_data) - block_size);
    std::uniform_int_distribution<std::size_t> d2(0, file_size - block_size);

    while (state.KeepRunning()) {
        FILE* file = fopen(fn, "w+");
        if (!file) {
            state.SkipWithError("fopen failed");
            break;
        }

        for (std::size_t i = 0; i < n_blocks; ++i) {
            if (fseek(file, d2(rng), SEEK_SET) != 0) {
                fclose(file);
                state.SkipWithError("fseek failed");
                break;
            }

            auto bytes = fwrite(html_data + d1(rng), 1, block_size, file);
            if (bytes != block_size) {
                fclose(file);
                state.SkipWithError(std::string("fwrite returned ") + std::to_string(bytes) +
                                    std::string(" != ") + std::to_string(block_size));
                break;
            }
        }

        fclose(file);
    }

    state.SetBytesProcessed(state.iterations() * n_blocks * block_size);
    state.SetItemsProcessed(state.iterations() * n_blocks);

    remove(fn);
}

static void BM_compio_RandomWrite(benchmark::State& state) {
    const size_t n_blocks = state.range(0);
    const size_t block_size = state.range(1);
    const size_t file_size = state.range(2);

    char fn[L_tmpnam];
    tmpnam(fn);

    std::minstd_rand0 rng(0);
    std::uniform_int_distribution<std::size_t> d1(0, sizeof(html_data) - block_size);
    std::uniform_int_distribution<std::size_t> d2(0, file_size - block_size);

    while (state.KeepRunning()) {
        compio_config config;
        compio_build_default_config(&config);

        compio_archive* archive = compio_open_archive(fn, "w+", &config);
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
            if (compio_seek(file, d2(rng), COMP_SEEK_SET) != 0) {
                compio_close_file(file);
                compio_close_archive(archive);
                state.SkipWithError("compio_seek failed");
                break;
            }

            auto bytes = compio_write(html_data + d1(rng), block_size, file);
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
    state.SetItemsProcessed(state.iterations() * n_blocks);

    remove(fn);
}

const std::vector<std::vector<int64_t>> params_grid = {
    {128, 1024},
    {256, 512, 1024},
    {16384, 32768},
};

BENCHMARK(BM_stdio_RandomWrite)
    ->ArgsProduct(params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK(BM_compio_RandomWrite)
    ->ArgsProduct(params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();