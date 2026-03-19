#include "benchmark_util.hpp"

#include "compio.h"

#include "sample_data.hpp"

#ifdef COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER
#include "compio/infile_object.hpp"
#endif

#include <benchmark/benchmark.h>
#include <random>

extern compio_config config;

static void BM_stdio_ConsecutiveWrite(benchmark::State &state) {
    const size_t n_blocks = state.range(0);
    const size_t block_size = state.range(1);

    std::string fn = get_temporary_filename();

    std::minstd_rand0 rng(0);
    std::uniform_int_distribution<std::size_t> d(0, sizeof(html_data) - block_size);

    for (auto _ : state) {
        FILE *file = fopen(fn.c_str(), "w+");
        if (!file) {
            state.SkipWithError("fopen failed");
            break;
        }

        for (std::size_t i = 0; i < n_blocks; ++i) {
            auto bytes = fwrite(html_data + d(rng), 1, block_size, file);
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
    state.counters["file_size"] = benchmark::Counter(
        get_file_size(fn.c_str()), benchmark::Counter::kDefaults, benchmark::Counter::kIs1024);

#ifdef COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER
    state.counters["read_bytes_per_op"] =
        benchmark::Counter(0, benchmark::Counter::kDefaults, benchmark::Counter::kIs1024);
    state.counters["written_bytes_per_op"] =
        benchmark::Counter(block_size, benchmark::Counter::kDefaults, benchmark::Counter::kIs1024);
#endif

    remove(fn.c_str());
    remove((fn + ".wal").c_str());
}

static void BM_compio_ConsecutiveWrite(benchmark::State &state) {
    const size_t n_blocks = state.range(0);
    const size_t block_size = state.range(1);

    std::string fn = get_temporary_filename();

    std::minstd_rand0 rng(0);
    std::uniform_int_distribution<std::size_t> d(0, sizeof(html_data) - block_size);

#ifdef COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER
    state.counters["read_bytes_per_op"] =
        benchmark::Counter(0, benchmark::Counter::kAvgIterations, benchmark::Counter::kIs1024);
    state.counters["written_bytes_per_op"] =
        benchmark::Counter(0, benchmark::Counter::kAvgIterations, benchmark::Counter::kIs1024);
#endif

    for (auto _ : state) {
        compio_archive *archive = compio_open_archive(fn.c_str(), "w+", &config);
        if (!archive) {
            state.SkipWithError("compio_open_archive failed");
            break;
        }

        compio_file *file = compio_open_file("A", archive);
        if (!file) {
            compio_close_archive(archive);
            state.SkipWithError("compio_open_file failed");
            break;
        }

#ifdef COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER
        int n_read_bytes_start = get_n_read_bytes();
        int n_written_bytes_start = get_n_written_bytes();
#endif

        for (std::size_t i = 0; i < n_blocks; ++i) {
            auto bytes = compio_write(html_data + d(rng), block_size, file);
            if (bytes != block_size) {
                compio_close_file(file);
                compio_close_archive(archive);
                state.SkipWithError(std::string("compio_write returned ") + std::to_string(bytes) +
                                    std::string(" != ") + std::to_string(block_size));
                break;
            }
        }

        compio_flush(archive);

#ifdef COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER
        state.counters["read_bytes_per_op"] +=
            static_cast<double>(get_n_read_bytes() - n_read_bytes_start) / n_blocks;
        state.counters["written_bytes_per_op"] +=
            static_cast<double>(get_n_written_bytes() - n_written_bytes_start) / n_blocks;
#endif

        compio_close_file(file);
        compio_close_archive(archive);
    }

    state.SetBytesProcessed(state.iterations() * n_blocks * block_size);
    state.counters["file_size"] = benchmark::Counter(
        get_file_size(fn.c_str()), benchmark::Counter::kDefaults, benchmark::Counter::kIs1024);

    remove(fn.c_str());
    remove((fn + ".wal").c_str());
}

const std::vector<std::vector<int64_t>> params_grid = {
    {1 << 13},
    {1 << 10, 1 << 12},
};

BENCHMARK(BM_stdio_ConsecutiveWrite)
    ->ArgsProduct(params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK(BM_compio_ConsecutiveWrite)
    ->ArgsProduct(params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();