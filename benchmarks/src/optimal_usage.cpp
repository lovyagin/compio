#include <algorithm>
#include <benchmark/benchmark.h>
#include <random>

#include "compio/compio_file.hpp"
#include "compio/storage_block_reader.hpp"

#ifdef COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER
#include "compio/infile_object.hpp"
#endif

#include "benchmark_util.hpp"

#include "compio.h"

#include "sample_data.hpp"

struct UsageStrategy {
    struct Operation {
        std::size_t pos;
        std::size_t size;
        const char *data;
    };

    UsageStrategy(int seed, const char *sample_data, std::size_t sample_data_size,
                  std::size_t file_size, double gamma_shape, double gamma_scale,
                  std::size_t region_size, std::size_t n_switch)
        : rng(seed),
          file_size(file_size),
          sample_data(sample_data),
          sample_data_size(sample_data_size),
          gamma_shape(gamma_shape),
          gamma_scale(gamma_scale),
          region_size(std::min(region_size, file_size)),
          region_start_dist(0, file_size - region_size),
          n_ops_until_switch(n_switch),
          n_switch(n_switch) {
        region_start = region_start_dist(rng);
    }

    Operation get_op() {
        if (--n_ops_until_switch == 0) {
            region_start = region_start_dist(rng);
            n_ops_until_switch = n_switch;
        }

        double gamma_sample = gamma_dist(rng);
        std::size_t max_possible = std::min(region_size, sample_data_size);
        std::size_t size =
            static_cast<std::size_t>(std::min(gamma_sample, static_cast<double>(max_possible)));
        if (size == 0)
            size = 1;

        std::uniform_int_distribution<std::size_t> offset_dist(0, region_size - size);
        std::size_t offset = offset_dist(rng);

        std::uniform_int_distribution<std::size_t> data_pos_dist(0, sample_data_size - size);

        return Operation{region_start + offset, size, sample_data + data_pos_dist(rng)};
    }

private:
    std::minstd_rand rng;
    std::size_t file_size;
    const char *sample_data;
    std::size_t sample_data_size;

    double gamma_shape;
    double gamma_scale;
    std::gamma_distribution<double> gamma_dist{gamma_shape, gamma_scale};

    std::size_t region_size;
    std::uniform_int_distribution<std::size_t> region_start_dist;
    std::size_t region_start;

    std::size_t n_ops_until_switch;
    std::size_t n_switch;
};

extern compio_config config;

static void BM_stdio_OptimalUsage(benchmark::State &state) {
    const bool is_write = state.range(0);
    const std::size_t n_operations = state.range(1);
    const std::size_t file_size = state.range(2);
    const std::size_t n_switch = state.range(3);
    const double gamma_shape = state.range(4);
    const double gamma_scale = state.range(5);
    const double region_size = state.range(6);

    std::string fn = get_temporary_filename();

    if (!is_write) {
        // prepare file
        FILE *file = fopen(fn.c_str(), "w+");
        if (!file) {
            state.SkipWithError("fopen failed");
            return;
        }

        const std::size_t block_size = 4096;
        std::minstd_rand rng(0);
        std::uniform_int_distribution<std::size_t> d1(0, sizeof(html_data) - block_size);

        for (std::size_t i = 0; i < file_size; i += block_size) {
            auto bytes_to_write = std::min(block_size, file_size - i);
            auto bytes = fwrite(html_data + d1(rng), 1, bytes_to_write, file);
            if (bytes != bytes_to_write) {
                fclose(file);
                state.SkipWithError(std::string("fwrite returned ") + std::to_string(bytes) +
                                    std::string(" != ") + std::to_string(bytes_to_write));
                return;
            }
        }

        auto actual_file_size = ftell(file);
        if (static_cast<std::size_t>(actual_file_size) != file_size) {
            state.SkipWithError("wrong file_size: " + std::to_string(actual_file_size) +
                                " != " + std::to_string(file_size));
        }

        fclose(file);
    }

    UsageStrategy strategy(0, html_data, sizeof(html_data), file_size, gamma_shape, gamma_scale,
                           region_size, n_switch);
    std::unique_ptr<char> buffer(new char[file_size]);
    std::size_t total_bytes_processed = 0;

    for (auto _ : state) {
        FILE *file = fopen(fn.c_str(), is_write ? "w" : "r");
        if (!file) {
            state.SkipWithError("fopen failed");
            break;
        }

        bool failed = false;
        for (std::size_t i = 0; i < n_operations; ++i) {
            const auto op = strategy.get_op();
            if (fseek(file, op.pos, SEEK_SET) != 0) {
                fclose(file);
                state.SkipWithError("fseek failed");
                break;
            }

            std::size_t bytes = 0;
            if (is_write) {
                bytes = fwrite(op.data, 1, op.size, file);
            } else {
                bytes = fread(buffer.get(), 1, op.size, file);
            }

            if (bytes != op.size) {
                fclose(file);
                state.SkipWithError(std::string(is_write ? "fwrite returned " : "fread returned ") +
                                    std::to_string(bytes) + std::string(" != ") +
                                    std::to_string(op.size));
                failed = true;
                break;
            }

            total_bytes_processed += op.size;
        }

        if (failed) {
            break;
        }

        fclose(file);
    }

    state.SetBytesProcessed(total_bytes_processed);
    state.counters["file_size"] = get_file_size(fn.c_str());
    state.counters[is_write ? "n_bytes_written" : "n_bytes_read"] =
        static_cast<double>(total_bytes_processed) / state.iterations();
    remove(fn.c_str());
}

static void BM_compio_OptimalUsage(benchmark::State &state) {
    const bool is_write = state.range(0);
    const std::size_t n_operations = state.range(1);
    const std::size_t file_size = state.range(2);
    const std::size_t n_switch = state.range(3);
    const double gamma_shape = state.range(4);
    const double gamma_scale = state.range(5);
    const double region_size = state.range(6);

    std::string fn = get_temporary_filename();

    if (!is_write) {
        // prepare file
        compio_archive *archive = compio_open_archive(fn.c_str(), "w+", &config);
        if (!archive) {
            state.SkipWithError("compio_open_archive failed");
            return;
        }

        compio_file *file = compio_open_file("A", archive);
        if (!file) {
            compio_close_archive(archive);
            state.SkipWithError("compio_open_file failed");
            return;
        }

        const std::size_t block_size = 4096;
        std::minstd_rand rng(0);
        std::uniform_int_distribution<std::size_t> d1(0, sizeof(html_data) - block_size);

        for (std::size_t i = 0; i < file_size / block_size; ++i) {
            auto bytes_to_write = std::min(block_size, file_size - i * block_size);
            auto bytes = compio_write(html_data + d1(rng), bytes_to_write, file);
            if (bytes != bytes_to_write) {
                compio_close_file(file);
                compio_close_archive(archive);
                state.SkipWithError("compio_write returned " + std::to_string(bytes) +
                                    " != " + std::to_string(bytes_to_write));
                return;
            }
        }

        auto actual_file_size = compio_tell(file);
        if (actual_file_size != file_size) {
            state.SkipWithError("wrong file_size: " + std::to_string(actual_file_size) +
                                " != " + std::to_string(file_size));
        }

        compio_close_file(file);
        compio_close_archive(archive);
    }

    UsageStrategy strategy(0, html_data, sizeof(html_data), file_size, gamma_shape, gamma_scale,
                           region_size, n_switch);
    std::unique_ptr<char> buffer(new char[file_size]);
    std::size_t total_bytes_processed = 0;
    double total_node_cache_hit_probability = 0.;
    double total_block_cache_hit_probability = 0.;

#ifdef COMPIO_BENCHMARK_COMPRESSION_BYTES
    long long n_bytes_compressed = compio::bm_n_compressed_bytes;
    long long n_bytes_decompressed = compio::bm_n_decompressed_bytes;
#endif

#ifdef COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER
    int n_bytes_read = get_n_read_bytes();
    int n_bytes_written = get_n_written_bytes();
#endif

    for (auto _ : state) {
        compio_archive *archive = compio_open_archive(fn.c_str(), is_write ? "w" : "r", &config);
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

        bool failed = false;
        for (std::size_t i = 0; i < n_operations; ++i) {
            const auto op = strategy.get_op();
            if (compio_seek(file, op.pos, COMPIO_SEEK_SET) != 0) {
                compio_close_file(file);
                compio_close_archive(archive);
                state.SkipWithError("compio_seek failed");
                break;
            }

            std::size_t bytes = 0;
            if (is_write) {
                bytes = compio_write(op.data, op.size, file);
            } else {
                bytes = compio_read(buffer.get(), op.size, file);
            }

            if (bytes != op.size) {
                compio_close_file(file);
                compio_close_archive(archive);
                state.SkipWithError(
                    std::string(is_write ? "compio_write returned " : "compio_read returned ") +
                    std::to_string(bytes) + std::string(" != ") + std::to_string(op.size));
                failed = true;
                break;
            }

            total_bytes_processed += op.size;
        }

        if (failed) {
            break;
        }

        total_node_cache_hit_probability += archive->index->get_cache_hit_probability();
        total_block_cache_hit_probability += archive->block_reader->get_cache_hit_probability();

        compio_close_file(file);
        compio_close_archive(archive);
    }

    state.SetBytesProcessed(total_bytes_processed);
    state.counters["file_size"] = get_file_size(fn.c_str());
    state.counters["node_cache_hit"] = total_node_cache_hit_probability / state.iterations();
    state.counters["block_cache_hit"] = total_block_cache_hit_probability / state.iterations();

#ifdef COMPIO_BENCHMARK_COMPRESSION_BYTES
    state.counters["n_bytes_compressed"] =
        static_cast<double>(compio::bm_n_compressed_bytes - n_bytes_compressed) /
        state.iterations();
    state.counters["n_bytes_decompressed"] =
        static_cast<double>(compio::bm_n_decompressed_bytes - n_bytes_decompressed) /
        state.iterations();
#endif

#ifdef COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER
    state.counters["n_bytes_written"] =
        static_cast<double>(get_n_written_bytes() - n_bytes_written) / state.iterations();
    state.counters["n_bytes_read"] =
        static_cast<double>(get_n_read_bytes() - n_bytes_read) / state.iterations();
#endif

    remove(fn.c_str());
}

const std::vector<std::vector<int64_t>> params_grid = {
    {false, true}, {1 << 11}, {1 << 20}, {1, 2, 4, 8, 16, 32, 64, 128, 256, 512},
    {2},           {2048},    {1 << 13},
};

BENCHMARK(BM_stdio_OptimalUsage)
    ->ArgsProduct(params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK(BM_compio_OptimalUsage)
    ->ArgsProduct(params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
