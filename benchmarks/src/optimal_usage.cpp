#include <algorithm>
#include <benchmark/benchmark.h>
#include <random>
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <cstdio>

#include "compio/compio_file.hpp"
#include "compio/storage_block_reader.hpp"

#ifdef COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER
#include "compio/infile_object.hpp"
#endif

#include "benchmark_util.hpp"

#include "compio.h"

#include <zlib.h>
#include "zstd_seekable.h"
extern "C" {
#include "zran.h"
}

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

static void write_gzip_file_with_flush(const std::string& path, const std::vector<char>& payload,
                                       std::size_t flush_interval) {
    gzFile gz = gzopen(path.c_str(), "wb");
    if (!gz) {
        throw std::runtime_error("gzopen failed for " + path);
    }

    std::size_t pos = 0;
    constexpr std::size_t kChunk = 1u << 15;
    std::size_t bytes_since_flush = 0;
    while (pos < payload.size()) {
        const std::size_t chunk = std::min(kChunk, payload.size() - pos);
        int written = gzwrite(gz, payload.data() + pos, static_cast<unsigned>(chunk));
        if (written != static_cast<int>(chunk)) {
            int err_no = Z_OK;
            const char* err = gzerror(gz, &err_no);
            gzclose(gz);
            throw std::runtime_error(std::string("gzwrite failed: ") + (err ? err : "unknown"));
        }
        pos += chunk;
        bytes_since_flush += chunk;

        if (bytes_since_flush >= flush_interval) {
            if (gzflush(gz, Z_FULL_FLUSH) != Z_OK) {
                int err_no = Z_OK;
                const char* err = gzerror(gz, &err_no);
                gzclose(gz);
                throw std::runtime_error(std::string("gzflush failed: ") + (err ? err : "unknown"));
            }
            bytes_since_flush = 0;
        }
    }

    if (gzclose(gz) != Z_OK) {
        throw std::runtime_error("gzclose failed");
    }
}

static void write_seekable_zstd_file(const std::string& path, const std::vector<char>& payload,
                                     unsigned max_frame_size) {
    FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) {
        throw std::runtime_error("fopen failed for " + path);
    }

    ZSTD_seekable_CStream* stream = ZSTD_seekable_createCStream();
    if (!stream) {
        std::fclose(out);
        throw std::runtime_error("ZSTD_seekable_createCStream failed");
    }

    size_t ret = ZSTD_seekable_initCStream(stream, 5, 1, max_frame_size);
    if (ZSTD_isError(ret)) {
        ZSTD_seekable_freeCStream(stream);
        std::fclose(out);
        throw std::runtime_error(std::string("ZSTD_seekable_initCStream failed: ") +
                                 ZSTD_getErrorName(ret));
    }

    std::vector<char> out_buffer(ZSTD_CStreamOutSize());
    std::size_t pos = 0;
    constexpr std::size_t kChunk = 1u << 15;
    while (pos < payload.size()) {
        const std::size_t chunk = std::min(kChunk, payload.size() - pos);
        ZSTD_inBuffer input{payload.data() + pos, chunk, 0};

        while (input.pos < input.size) {
            ZSTD_outBuffer output{out_buffer.data(), out_buffer.size(), 0};
            ret = ZSTD_seekable_compressStream(stream, &output, &input);
            if (ZSTD_isError(ret)) {
                ZSTD_seekable_freeCStream(stream);
                std::fclose(out);
                throw std::runtime_error(std::string("ZSTD_seekable_compressStream failed: ") +
                                         ZSTD_getErrorName(ret));
            }
            if (std::fwrite(out_buffer.data(), 1, output.pos, out) != output.pos) {
                ZSTD_seekable_freeCStream(stream);
                std::fclose(out);
                throw std::runtime_error("fwrite failed");
            }
        }
        pos += chunk;
    }

    do {
        ZSTD_outBuffer output{out_buffer.data(), out_buffer.size(), 0};
        ret = ZSTD_seekable_endStream(stream, &output);
        if (ZSTD_isError(ret)) {
            ZSTD_seekable_freeCStream(stream);
            std::fclose(out);
            throw std::runtime_error(std::string("ZSTD_seekable_endStream failed: ") +
                                     ZSTD_getErrorName(ret));
        }
        if (std::fwrite(out_buffer.data(), 1, output.pos, out) != output.pos) {
            ZSTD_seekable_freeCStream(stream);
            std::fclose(out);
            throw std::runtime_error("fwrite failed while finalizing");
        }
    } while (ret != 0);

    ZSTD_seekable_freeCStream(stream);
    std::fclose(out);
}

static std::vector<char> build_payload_from_sample(const char* sample_data,
                                                   std::size_t sample_data_size,
                                                   std::size_t file_size) {
    std::vector<char> payload(file_size);

    for (std::size_t i = 0; i < file_size; i += sample_data_size) {
        const std::size_t bytes_to_copy = std::min(sample_data_size, file_size - i);
        std::copy_n(sample_data, bytes_to_copy, payload.data() + i);
    }
    return payload;
}

static void BM_stdio_OptimalUsage(benchmark::State &state) {
    const bool is_write = state.range(0);
    const std::size_t n_operations = state.range(1);
    const std::size_t file_size = state.range(2);
    const std::size_t n_switch = state.range(3);
    const double gamma_shape = state.range(4);
    const double gamma_scale = state.range(5);
    const double region_size = state.range(6);

    auto [sample_data, sample_data_size] = load_webster_data();
    if (!sample_data) {
        state.SkipWithError("failed to load sample data file (webster)");
        return;
    }

    std::string fn = get_temporary_filename();

    if (!is_write) {
        // prepare file
        FILE *file = fopen(fn.c_str(), "w+");
        if (!file) {
            state.SkipWithError("fopen failed");
            return;
        }

        const std::size_t block_size = sample_data_size;
        std::minstd_rand rng(0);
        std::uniform_int_distribution<std::size_t> d1(0, sample_data_size - block_size);

        for (std::size_t i = 0; i < file_size; i += block_size) {
            auto bytes_to_write = std::min(block_size, file_size - i);
            auto bytes = fwrite(sample_data + d1(rng), 1, bytes_to_write, file);
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

    UsageStrategy strategy(0, sample_data, sample_data_size, file_size, gamma_shape, gamma_scale,
                           region_size, n_switch);
    std::unique_ptr<char[]> buffer(new char[file_size]);
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
    remove(fn.c_str()); remove((fn + ".wal").c_str());
}

static void BM_compio_OptimalUsage(benchmark::State &state) {
    const bool is_write = state.range(0);
    const std::size_t n_operations = state.range(1);
    const std::size_t file_size = state.range(2);
    const std::size_t n_switch = state.range(3);
    const double gamma_shape = state.range(4);
    const double gamma_scale = state.range(5);
    const double region_size = state.range(6);

    auto [sample_data, sample_data_size] = load_webster_data();
    if (!sample_data) {
        state.SkipWithError("failed to load sample data file (webster)");
        return;
    }

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

        const std::size_t block_size = sample_data_size;
        std::minstd_rand rng(0);
        std::uniform_int_distribution<std::size_t> d1(0, sample_data_size - block_size);

        for (std::size_t i = 0; i < file_size; i += block_size) {
            auto bytes_to_write = std::min(block_size, file_size - i);
            auto bytes = compio_write(sample_data + d1(rng), bytes_to_write, file);
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
                                " != " + std::to_string(file_size) + "; " + std::to_string(block_size));
        }

        compio_close_file(file);
        compio_close_archive(archive);
    }

    UsageStrategy strategy(0, sample_data, sample_data_size, file_size, gamma_shape, gamma_scale,
                           region_size, n_switch);
    std::unique_ptr<char[]> buffer(new char[file_size]);
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

    remove(fn.c_str()); remove((fn + ".wal").c_str());
}

// ------------------------------------------------------------
// New benchmarks: zran (random access gzip) and seekable zstd
// Both are read-only and use the same access pattern.
// ------------------------------------------------------------

static void BM_zran_OptimalUsage(benchmark::State &state) {
    const std::size_t n_operations = state.range(0);
    const std::size_t file_size = state.range(1);
    const std::size_t n_switch = state.range(2);
    const double gamma_shape = state.range(3);
    const double gamma_scale = state.range(4);
    const std::size_t region_size = state.range(5);
    const std::size_t block_size = state.range(6);

    auto [sample_data, sample_data_size] = load_webster_data();
    if (!sample_data) {
        state.SkipWithError("failed to load sample data file (webster)");
        return;
    }

    // Build the raw payload from the sample data (same as compio's prep)
    std::vector<char> payload = build_payload_from_sample(sample_data, sample_data_size, file_size);

    // Create a gzip file with flush intervals
    std::string gzip_path = get_temporary_filename() + ".gz";
    write_gzip_file_with_flush(gzip_path, payload, block_size);

    // Build zran index (once)
    FILE* index_file = std::fopen(gzip_path.c_str(), "rb");
    if (!index_file) {
        state.SkipWithError("fopen failed for zran index building");
        return;
    }
    struct deflate_index* zran_index = nullptr;
    int access_points = deflate_index_build(index_file, static_cast<off_t>(block_size), &zran_index);
    std::fclose(index_file);
    if (access_points < 1 || !zran_index) {
        state.SkipWithError("deflate_index_build failed");
        remove(gzip_path.c_str());
        return;
    }

    const std::size_t bytes_per_index_entry = 1 << 15;
    std::size_t index_storage_size = static_cast<std::size_t>(access_points) * bytes_per_index_entry;

    // Scratch buffer for reads (max size = region_size)
    std::vector<char> scratch(region_size);
    std::size_t total_bytes_processed = 0;

    for (auto _ : state) {
        FILE* file = std::fopen(gzip_path.c_str(), "rb");
        if (!file) {
            state.SkipWithError("fopen failed for zran read");
            break;
        }

        // Create a generator for this iteration (starting from same seed each time)
        UsageStrategy strategy(0, sample_data, sample_data_size, file_size, gamma_shape, gamma_scale,
                               region_size, n_switch);

        bool failed = false;
        for (std::size_t i = 0; i < n_operations; ++i) {
            const auto op = strategy.get_op();
            ptrdiff_t read = deflate_index_extract(file, zran_index, static_cast<off_t>(op.pos),
                                                   reinterpret_cast<unsigned char*>(scratch.data()),
                                                   op.size);
            if (read < 0 || static_cast<std::size_t>(read) != op.size) {
                std::fclose(file);
                state.SkipWithError("deflate_index_extract returned unexpected bytes");
                failed = true;
                break;
            }
            total_bytes_processed += op.size;
        }

        if (failed) break;
        std::fclose(file);
    }

    state.SetBytesProcessed(total_bytes_processed);
    state.counters["file_size"] = get_file_size(gzip_path.c_str()) + index_storage_size;
    state.counters["index_size"] = index_storage_size;

    deflate_index_free(zran_index);
    remove(gzip_path.c_str());
}

static void BM_seekable_zstd_OptimalUsage(benchmark::State &state) {
    const std::size_t n_operations = state.range(0);
    const std::size_t file_size = state.range(1);
    const std::size_t n_switch = state.range(2);
    const double gamma_shape = state.range(3);
    const double gamma_scale = state.range(4);
    const std::size_t region_size = state.range(5);
    const std::size_t max_frame_size = state.range(6);

    auto [sample_data, sample_data_size] = load_webster_data();
    if (!sample_data) {
        state.SkipWithError("failed to load sample data file (webster)");
        return;
    }

    // Build the raw payload from the sample data
    std::vector<char> payload = build_payload_from_sample(sample_data, sample_data_size, file_size);

    std::string zstd_path = get_temporary_filename() + ".seek.zst";
    write_seekable_zstd_file(zstd_path, payload, max_frame_size);

    // Scratch buffer for reads
    std::vector<char> scratch(region_size);
    std::size_t total_bytes_processed = 0;

    for (auto _ : state) {
        FILE* file = std::fopen(zstd_path.c_str(), "rb");
        if (!file) {
            state.SkipWithError("fopen failed for seekable zstd");
            break;
        }

        ZSTD_seekable* seekable = ZSTD_seekable_create();
        if (!seekable) {
            std::fclose(file);
            state.SkipWithError("ZSTD_seekable_create failed");
            break;
        }
        size_t init_res = ZSTD_seekable_initFile(seekable, file);
        if (ZSTD_isError(init_res)) {
            ZSTD_seekable_free(seekable);
            std::fclose(file);
            state.SkipWithError(std::string("ZSTD_seekable_initFile failed: ") +
                                ZSTD_getErrorName(init_res));
            break;
        }

        UsageStrategy strategy(0, sample_data, sample_data_size, file_size, gamma_shape, gamma_scale,
                               region_size, n_switch);
        bool failed = false;
        for (std::size_t i = 0; i < n_operations; ++i) {
            const auto op = strategy.get_op();
            size_t read = ZSTD_seekable_decompress(seekable, scratch.data(), op.size,
                                                   static_cast<unsigned long long>(op.pos));
            if (ZSTD_isError(read) || read != op.size) {
                ZSTD_seekable_free(seekable);
                std::fclose(file);
                state.SkipWithError(std::string("ZSTD_seekable_decompress failed: ") +
                                    (ZSTD_isError(read) ? ZSTD_getErrorName(read) : "size mismatch"));
                failed = true;
                break;
            }
            total_bytes_processed += op.size;
        }

        if (failed) break;
        ZSTD_seekable_free(seekable);
        std::fclose(file);
    }

    state.SetBytesProcessed(total_bytes_processed);
    state.counters["file_size"] = get_file_size(zstd_path.c_str());

    remove(zstd_path.c_str());
}

// Parameter grids
const std::vector<std::vector<int64_t>> params_grid = {
    {false, true}, // is_write
    {1 << 11},     // n_operations
    {1 << 25},     // file_size
    {1, 2, 4, 8, 16, 32, 64, 128, 256, 512}, // n_switch
    {2},           // gamma_shape
    {1 << 12},     // gamma_scale
    {1 << 17},     // region_size
};

// Parameter grid for read-only benchmarks (no is_write flag)
const std::vector<std::vector<int64_t>> read_params_grid = {
    {1 << 11},     // n_operations
    {1 << 25},     // file_size
    {1, 2, 4, 8, 16, 32, 64, 128, 256, 512}, // n_switch
    {2},           // gamma_shape
    {1 << 12},     // gamma_scale
    {1 << 17},     // region_size
};

const std::vector<std::vector<int64_t>> read_params_grid_alt = {
    {1 << 11},     // n_operations
    {1 << 25},     // file_size
    {32},          // n_switch
    {2},           // gamma_shape
    {1 << 12},     // gamma_scale
    {1 << 17},     // region_size
    {1 << 14, 1 << 15, 1 << 16, 1 << 17, 1 << 18, 1 << 19, 1 << 20, 1 << 21, 1 << 22, 1 << 23},     // frame/block size
};

BENCHMARK(BM_stdio_OptimalUsage)
    ->ArgsProduct(params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK(BM_compio_OptimalUsage)
    ->ArgsProduct(params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK(BM_zran_OptimalUsage)
    ->ArgsProduct(read_params_grid_alt)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK(BM_seekable_zstd_OptimalUsage)
    ->ArgsProduct(read_params_grid_alt)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();