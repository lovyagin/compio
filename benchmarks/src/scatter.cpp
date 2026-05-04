#include <algorithm>
#include <benchmark/benchmark.h>
#include <cstdio>
#include <exception>
#include <random>
#include <sys/stat.h>
#include <vector>

#include "benchmark_util.hpp"

#include "compio.h"

#include "sample_data.hpp"

constexpr size_t UNCOMPRESSED_SIZE = 1 << 18;
constexpr size_t READ_SIZE = 4096;
constexpr size_t OPS_PER_BENCH = 1000;

static constexpr size_t HTML_DATA_SIZE = sizeof(html_data);

extern compio_config config;

static void BM_compio_ScatterWrite(benchmark::State &state) {
    const size_t block_size = state.range(0);
    config.block_size = block_size;
    config.block_size__minimum = block_size / 4;
    config.block_size__maximum = block_size * 4;

    std::vector<char> payload(UNCOMPRESSED_SIZE);
    for (size_t i = 0; i < UNCOMPRESSED_SIZE; i += HTML_DATA_SIZE) {
        size_t copy = std::min(HTML_DATA_SIZE, UNCOMPRESSED_SIZE - i);
        std::copy_n(html_data, copy, payload.data() + i);
    }

    std::vector<char> write_buffer(READ_SIZE);
    for (size_t i = 0; i < READ_SIZE; ++i) {
        write_buffer[i] = html_data[i % HTML_DATA_SIZE];
    }

    std::vector<std::pair<size_t, size_t>> read_ops;
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> pos_dist(0, UNCOMPRESSED_SIZE - READ_SIZE);
    for (size_t i = 0; i < OPS_PER_BENCH; ++i) {
        read_ops.emplace_back(pos_dist(rng), READ_SIZE);
    }

    std::string fn = get_temporary_filename();

    {
        compio_archive *arch = compio_open_archive(fn.c_str(), "w+", &config);
        if (!arch)
            throw std::runtime_error("compio_open_archive failed");
        compio_file *f = compio_open_file("A", arch);
        if (!f) {
            compio_close_archive(arch);
            throw std::runtime_error("compio_open_file failed");
        }
        size_t written = compio_write(payload.data(), payload.size(), f);
        if (written != payload.size()) {
            compio_close_file(f);
            compio_close_archive(arch);
            throw std::runtime_error("compio_write failed");
        }
        compio_close_file(f);
        compio_close_archive(arch);
    }

    size_t total_bytes = 0;
    for (auto _ : state) {
        compio_archive *arch = compio_open_archive(fn.c_str(), "r+", &config); // works with 'a'
        if (!arch) {
            state.SkipWithError("open archive failed");
            break;
        }
        compio_file *f = compio_open_file("A", arch);
        if (!f) {
            compio_close_archive(arch);
            state.SkipWithError("open file failed");
            break;
        }

        for (auto [pos, len] : read_ops) {
            if (compio_seek(f, pos, COMPIO_SEEK_SET) != 0) {
                state.SkipWithError("seek failed");
                break;
            }
            size_t n = compio_write(write_buffer.data(), len, f);
            if (n != len) {
                state.SkipWithError("write failed");
                break;
            }
            total_bytes += len;
            compio_flush(arch);
        }

        compio_close_file(f);
        compio_close_archive(arch);
    }

    state.SetBytesProcessed(total_bytes);
    state.counters["file_size"] = get_file_size(fn.c_str());
    remove(fn.c_str());
    remove((fn + ".wal").c_str());
}

BENCHMARK(BM_compio_ScatterWrite)->Arg(1 << 8)->Unit(benchmark::kMillisecond)->UseRealTime();