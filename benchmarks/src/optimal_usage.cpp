#include <algorithm>
#include <benchmark/benchmark.h>
#include <random>

#include "compio/compio_file.hpp"

#include "benchmark_util.hpp"

#include "compio.h"

#include "sample_data.hpp"

struct User {
    struct Operation {
        std::size_t pos;
        std::size_t size;
        const char *data;
    };

    User(int seed, const char *sample_data, std::size_t sample_data_size, std::size_t file_size,
         double stddev, double switch_p)
        : rng(seed),
          file_size(file_size),
          stddev(stddev),
          sample_data(sample_data),
          sample_data_size(sample_data_size),
          mean_d(0, file_size - 1),
          pos_d(0., stddev),
          switch_d(switch_p),
          current_mean(mean_d(rng)) {}

    Operation get_op() {
        if (switch_d(rng)) {
            current_mean = mean_d(rng);
        }
        const auto [left, right] = get_range();
        std::size_t size = std::min(right - left, sample_data_size);
        data_pos_d = std::uniform_int_distribution<std::size_t>(0, sample_data_size - size);
        return Operation{left, size, sample_data + data_pos_d(rng)};
    }

private:
    std::size_t get_pos() {
        double pos = -1;
        while (pos < 0 || pos > file_size - 1) {
            pos = current_mean + pos_d(rng);
        }
        return std::round(pos);
    }

    std::pair<std::size_t, std::size_t> get_range() {
        std::size_t a = get_pos(), b = get_pos();
        if (a > b) {
            return {b, a};
        } else {
            return {a, b};
        }
    }

    std::minstd_rand rng;
    std::size_t file_size;
    std::size_t stddev;
    const char *sample_data;
    std::size_t sample_data_size;

    std::uniform_int_distribution<std::size_t> mean_d;
    std::uniform_int_distribution<std::size_t> data_pos_d;
    std::normal_distribution<double> pos_d;
    std::bernoulli_distribution switch_d;

    double current_mean;
};

extern compio_config config;

static void BM_stdio_OptimalUsage(benchmark::State &state) {
    const bool is_write = state.range(0);
    const std::size_t n_operations = state.range(1);
    const std::size_t file_size = state.range(2);
    const double stddev = state.range(3);
    const double switch_p = state.range(4) / 100.;

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

    User user(0, html_data, sizeof(html_data), file_size, stddev, switch_p);
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
            const auto op = user.get_op();
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
    state.counters["file_size"] = benchmark::Counter(
        get_file_size(fn.c_str()), benchmark::Counter::kDefaults, benchmark::Counter::kIs1024);
    remove(fn.c_str());
}

static void BM_compio_OptimalUsage(benchmark::State &state) {
    const bool is_write = state.range(0);
    const std::size_t n_operations = state.range(1);
    const std::size_t file_size = state.range(2);
    const double stddev = state.range(3);
    const double switch_p = state.range(4) / 100.;

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

    User user(0, html_data, sizeof(html_data), file_size, stddev, switch_p);
    std::unique_ptr<char> buffer(new char[file_size]);
    std::size_t total_bytes_processed = 0;
    double total_node_cache_hit_probability = 0.;
    double total_block_cache_hit_probability = 0.;

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
            const auto op = user.get_op();
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
    state.counters["file_size"] = benchmark::Counter(
        get_file_size(fn.c_str()), benchmark::Counter::kDefaults, benchmark::Counter::kIs1024);
    state.counters["node_cache_hit"] = total_node_cache_hit_probability / state.iterations();
    state.counters["block_cache_hit"] = total_block_cache_hit_probability / state.iterations();
    remove(fn.c_str());
}

const std::vector<std::vector<int64_t>> params_grid = {
    {false, true}, {1 << 10}, {1 << 20}, {1 << 13}, {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100},
};

BENCHMARK(BM_stdio_OptimalUsage)
    ->ArgsProduct(params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK(BM_compio_OptimalUsage)
    ->ArgsProduct(params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
