#include <algorithm>
#include <benchmark/benchmark.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

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

extern compio_config config;

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

static bool copy_file(const std::string &src, const std::string &dst) {
    FILE *in = fopen(src.c_str(), "rb");
    if (!in)
        return false;
    FILE *out = fopen(dst.c_str(), "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return false;
        }
    }
    fclose(in);
    fclose(out);
    return true;
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

    if (file_size > sample_data_size) {
        state.SkipWithError("file_size exceeds sample data size");
        return;
    }

    std::string template_path = get_temporary_filename();
    FILE *tmpl = fopen(template_path.c_str(), "wb");
    if (!tmpl) {
        state.SkipWithError("failed to create template file");
        return;
    }
    if (fwrite(sample_data, 1, file_size, tmpl) != file_size) {
        fclose(tmpl);
        state.SkipWithError("failed to write template");
        return;
    }
    fclose(tmpl);

    UsageStrategy strategy(0, sample_data, sample_data_size, file_size, gamma_shape, gamma_scale,
                           region_size, n_switch);
    std::unique_ptr<char[]> buffer(new char[file_size]);
    std::size_t total_bytes_processed = 0;

    for (auto _ : state) {
        std::string fn;
        if (is_write) {
            state.PauseTiming();
            fn = get_temporary_filename();
            if (!copy_file(template_path, fn)) {
                state.SkipWithError("copy_file failed");
                break;
            }
            state.ResumeTiming();
        } else {
            fn = template_path;
        }

        FILE *file = fopen(fn.c_str(), is_write ? "r+b" : "rb");
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
                failed = true;
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
                state.SkipWithError(std::string(is_write ? "fwrite" : "fread") +
                                    " returned wrong bytes");
                failed = true;
                break;
            }

            total_bytes_processed += op.size;
        }

        fclose(file);

        if (failed)
            break;

        if (is_write) {
            state.PauseTiming();
            remove(fn.c_str());
            state.ResumeTiming();
        }
    }

    state.SetBytesProcessed(total_bytes_processed);
    state.counters["file_size"] = get_file_size(template_path.c_str());
    if (is_write)
        remove(template_path.c_str());
    else
        remove(template_path.c_str());
}

// Cache for compio template files in /dev/shm, keyed by (file_size, block_size).
// The template is a compio archive containing file_size bytes of sample data.
struct CachedTemplate {
    std::string path;
    std::string wal_path;
};

static std::map<std::pair<std::size_t, int>, CachedTemplate>& get_template_cache() {
    static std::map<std::pair<std::size_t, int>, CachedTemplate> cache;
    return cache;
}

static CachedTemplate get_cached_template(std::size_t file_size, int block_size,
                                           const char *sample_data, std::size_t sample_data_size) {
    auto &cache = get_template_cache();
    auto key = std::make_pair(file_size, block_size);
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }

    // Create template in /dev/shm
    std::string tmpl_path = std::string("/dev/shm/compio_tmpl_") + std::to_string(file_size) +
                            "_" + std::to_string(block_size);
    std::string tmpl_wal_path = tmpl_path + ".wal";

    // Remove stale files if they exist (e.g. from a previous crashed run)
    remove(tmpl_path.c_str());
    remove(tmpl_wal_path.c_str());

    config.block_size = block_size / 4;
    config.block_size__minimum = block_size / 4 / 4;
    config.block_size__maximum = block_size * 4 / 4;
    config.cache_size__blocks = 1;
    compio_archive *archive = compio_open_archive(tmpl_path.c_str(), "w+", &config);
    if (!archive) {
        throw std::runtime_error("get_cached_template: compio_open_archive failed");
    }
    compio_file *file = compio_open_file("A", archive);
    if (!file) {
        compio_close_archive(archive);
        throw std::runtime_error("get_cached_template: compio_open_file failed");
    }
    if (compio_write(sample_data, file_size, file) != file_size) {
        compio_close_file(file);
        compio_close_archive(archive);
        throw std::runtime_error("get_cached_template: compio_write failed");
    }
    if (compio_tell(file) != file_size) {
        compio_close_file(file);
        compio_close_archive(archive);
        throw std::runtime_error("get_cached_template: wrong file_size");
    }
    compio_close_file(file);
    compio_close_archive(archive);

    CachedTemplate ct{tmpl_path, tmpl_wal_path};
    cache[key] = ct;
    return ct;
}

void cleanup_cached_templates() {
    auto &cache = get_template_cache();
    for (auto &[key, ct] : cache) {
        remove(ct.path.c_str());
        remove(ct.wal_path.c_str());
    }
    cache.clear();
}

static void BM_compio_OptimalUsage(benchmark::State &state) {
    const bool is_write = state.range(0);
    const std::size_t n_operations = state.range(1);
    const std::size_t file_size = state.range(2);
    const std::size_t n_switch = state.range(3);
    const double gamma_shape = state.range(4);
    const double gamma_scale = state.range(5);
    const double region_size = state.range(6);
    const bool disable_cache = state.range(7);
    const int block_size = state.range(8);

    auto [sample_data, sample_data_size] = load_webster_data();
    if (!sample_data) {
        state.SkipWithError("failed to load sample data file (webster)");
        return;
    }

    if (file_size > sample_data_size) {
        state.SkipWithError("file_size exceeds sample data size");
        return;
    }

    CachedTemplate tmpl;
    try {
        tmpl = get_cached_template(file_size, block_size, sample_data, sample_data_size);
    } catch (const std::exception &e) {
        state.SkipWithError(e.what());
        return;
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
        state.PauseTiming();
        std::string fn = get_temporary_filename();
        std::string wal_fn = fn + ".wal";
        if (!copy_file(tmpl.path, fn) || !copy_file(tmpl.wal_path, wal_fn)) {
            state.SkipWithError("copy_file failed");
            break;
        }
        state.ResumeTiming();

        compio_archive *archive = compio_open_archive(fn.c_str(), is_write ? "r+" : "r", &config);
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
                failed = true;
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
                state.SkipWithError(std::string(is_write ? "compio_write" : "compio_read") +
                                    " returned wrong bytes");
                failed = true;
                break;
            }

            total_bytes_processed += op.size;

            if (disable_cache) {
                compio_flush(archive);
            }
        }

        if (failed)
            break;

        total_node_cache_hit_probability += archive->index->get_cache_hit_probability();
        total_block_cache_hit_probability += archive->block_reader->get_cache_hit_probability();

        compio_close_file(file);
        compio_close_archive(archive);

        state.PauseTiming();
        remove(fn.c_str());
        remove(wal_fn.c_str());
        state.ResumeTiming();
    }

    state.SetBytesProcessed(total_bytes_processed);
    state.counters["file_size"] = get_file_size(tmpl.path.c_str());
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
}

// --- Helper: write a gzip file with periodic Z_FULL_FLUSH for zran indexing ---
static void write_gzip_with_flush(const std::string& path, const char* data, std::size_t data_size,
                                  std::size_t flush_interval) {
    gzFile gz = gzopen(path.c_str(), "wb1");
    if (!gz) throw std::runtime_error("gzopen failed");
    std::size_t pos = 0, flushed = 0;
    while (pos < data_size) {
        std::size_t chunk = std::min<std::size_t>(1 << 15, data_size - pos);
        if (gzwrite(gz, data + pos, chunk) != static_cast<int>(chunk)) {
            gzclose(gz);
            throw std::runtime_error("gzwrite failed");
        }
        pos += chunk;
        flushed += chunk;
        if (flushed >= flush_interval) {
            if (gzflush(gz, Z_FULL_FLUSH) != Z_OK) {
                gzclose(gz);
                throw std::runtime_error("gzflush failed");
            }
            flushed = 0;
        }
    }
    if (gzclose(gz) != Z_OK) throw std::runtime_error("gzclose failed");
}

// --- Helper: write a seekable zstd file ---
static void write_seekable_zstd(const std::string& path, const char* data, std::size_t data_size,
                                unsigned max_frame_size) {
    FILE* out = fopen(path.c_str(), "wb");
    if (!out) throw std::runtime_error("fopen failed");
    ZSTD_seekable_CStream* stream = ZSTD_seekable_createCStream();
    if (!stream) { fclose(out); throw std::runtime_error("ZSTD_seekable_createCStream failed"); }
    size_t ret = ZSTD_seekable_initCStream(stream, 1, 1, max_frame_size);
    if (ZSTD_isError(ret)) { ZSTD_seekable_freeCStream(stream); fclose(out);
        throw std::runtime_error("ZSTD_seekable_initCStream failed"); }
    std::vector<char> outbuf(ZSTD_CStreamOutSize());
    std::size_t pos = 0;
    const std::size_t CHUNK = 1 << 15;
    while (pos < data_size) {
        std::size_t chunk = std::min(CHUNK, data_size - pos);
        ZSTD_inBuffer in{data + pos, chunk, 0};
        while (in.pos < in.size) {
            ZSTD_outBuffer outb{outbuf.data(), outbuf.size(), 0};
            ret = ZSTD_seekable_compressStream(stream, &outb, &in);
            if (ZSTD_isError(ret)) { ZSTD_seekable_freeCStream(stream); fclose(out);
                throw std::runtime_error("compressStream failed"); }
            fwrite(outbuf.data(), 1, outb.pos, out);
        }
        pos += chunk;
    }
    do {
        ZSTD_outBuffer outb{outbuf.data(), outbuf.size(), 0};
        ret = ZSTD_seekable_endStream(stream, &outb);
        if (ZSTD_isError(ret)) { ZSTD_seekable_freeCStream(stream); fclose(out);
            throw std::runtime_error("endStream failed"); }
        fwrite(outbuf.data(), 1, outb.pos, out);
    } while (ret != 0);
    ZSTD_seekable_freeCStream(stream);
    fclose(out);
}

// --- Cache for zran template files ---
struct ZranCachedTemplate {
    std::string gz_path;
    struct deflate_index* index;
    std::size_t index_size;
};

static std::map<std::pair<std::size_t, std::size_t>, ZranCachedTemplate>& get_zran_template_cache() {
    static std::map<std::pair<std::size_t, std::size_t>, ZranCachedTemplate> cache;
    return cache;
}

static ZranCachedTemplate get_cached_zran_template(std::size_t file_size, std::size_t flush_interval,
                                                    const char* sample_data, std::size_t sample_data_size) {
    auto& cache = get_zran_template_cache();
    auto key = std::make_pair(file_size, flush_interval);
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }

    std::string gz_path = std::string("/dev/shm/zran_tmpl_") + std::to_string(file_size) +
                          "_" + std::to_string(flush_interval) + ".gz";
    remove(gz_path.c_str());

    write_gzip_with_flush(gz_path, sample_data, file_size, flush_interval);

    FILE* f = fopen(gz_path.c_str(), "rb");
    if (!f) throw std::runtime_error("fopen for zran index build failed");
    struct deflate_index* index = nullptr;
    int access_points = deflate_index_build(f, flush_interval, &index);
    fclose(f);
    if (access_points <= 0 || !index)
        throw std::runtime_error("deflate_index_build failed");

    ZranCachedTemplate ct{gz_path, index, static_cast<std::size_t>(access_points) * (1 << 15)};
    cache[key] = ct;
    return ct;
}

void cleanup_zran_templates() {
    auto& cache = get_zran_template_cache();
    for (auto& [key, ct] : cache) {
        remove(ct.gz_path.c_str());
        deflate_index_free(ct.index);
    }
    cache.clear();
}

// --- Cache for seekable zstd template files ---
struct SeekableZstdCachedTemplate {
    std::string path;
};

static std::map<std::pair<std::size_t, unsigned>, SeekableZstdCachedTemplate>& get_seekable_zstd_template_cache() {
    static std::map<std::pair<std::size_t, unsigned>, SeekableZstdCachedTemplate> cache;
    return cache;
}

static SeekableZstdCachedTemplate get_cached_seekable_zstd_template(std::size_t file_size, unsigned max_frame_size,
                                                                     const char* sample_data, std::size_t sample_data_size) {
    auto& cache = get_seekable_zstd_template_cache();
    auto key = std::make_pair(file_size, max_frame_size);
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }

    std::string path = std::string("/dev/shm/seekable_zstd_tmpl_") + std::to_string(file_size) +
                       "_" + std::to_string(max_frame_size) + ".seek.zst";
    remove(path.c_str());

    write_seekable_zstd(path, sample_data, file_size, max_frame_size);

    SeekableZstdCachedTemplate ct{path};
    cache[key] = ct;
    return ct;
}

void cleanup_seekable_zstd_templates() {
    auto& cache = get_seekable_zstd_template_cache();
    for (auto& [key, ct] : cache) {
        remove(ct.path.c_str());
    }
    cache.clear();
}

// --- BM_zran_OptimalUsage: read-only benchmark using zran (gzip + deflate_index) ---
static void BM_zran_OptimalUsage(benchmark::State& state) {
    const std::size_t n_operations = state.range(0);
    const std::size_t file_size = state.range(1);
    const std::size_t n_switch = state.range(2);
    const double gamma_shape = state.range(3);
    const double gamma_scale = state.range(4);
    const double region_size = state.range(5);
    const std::size_t flush_interval = state.range(6);

    auto [sample_data, sample_data_size] = load_webster_data();
    if (!sample_data) {
        state.SkipWithError("failed to load sample data file (webster)");
        return;
    }

    if (file_size > sample_data_size) {
        state.SkipWithError("file_size exceeds sample data size");
        return;
    }

    ZranCachedTemplate tmpl;
    try {
        tmpl = get_cached_zran_template(file_size, flush_interval, sample_data, sample_data_size);
    } catch (const std::exception& e) {
        state.SkipWithError(e.what());
        return;
    }

    UsageStrategy strategy(0, sample_data, sample_data_size, file_size, gamma_shape, gamma_scale,
                           region_size, n_switch);
    std::unique_ptr<unsigned char[]> buffer(new unsigned char[file_size]);
    std::size_t total_bytes_processed = 0;

    for (auto _ : state) {
        state.PauseTiming();
        std::string fn = get_temporary_filename() + ".gz";
        if (!copy_file(tmpl.gz_path, fn)) {
            state.SkipWithError("copy_file failed");
            break;
        }
        state.ResumeTiming();

        FILE* file = fopen(fn.c_str(), "rb");
        if (!file) {
            state.SkipWithError("fopen failed");
            break;
        }

        bool failed = false;
        for (std::size_t i = 0; i < n_operations; ++i) {
            const auto op = strategy.get_op();
            ptrdiff_t n = deflate_index_extract(file, tmpl.index, op.pos, buffer.get(), op.size);
            if (n != static_cast<ptrdiff_t>(op.size)) {
                fclose(file);
                state.SkipWithError("deflate_index_extract returned wrong bytes");
                failed = true;
                break;
            }
            total_bytes_processed += op.size;
        }

        fclose(file);

        if (failed) break;

        state.PauseTiming();
        remove(fn.c_str());
        state.ResumeTiming();
    }

    state.SetBytesProcessed(total_bytes_processed);
    state.counters["file_size"] = get_file_size(tmpl.gz_path.c_str()) + tmpl.index_size;
}

// --- BM_seekable_zstd_OptimalUsage: read-only benchmark using seekable zstd ---
static void BM_seekable_zstd_OptimalUsage(benchmark::State& state) {
    const std::size_t n_operations = state.range(0);
    const std::size_t file_size = state.range(1);
    const std::size_t n_switch = state.range(2);
    const double gamma_shape = state.range(3);
    const double gamma_scale = state.range(4);
    const double region_size = state.range(5);
    const unsigned max_frame_size = state.range(6);

    auto [sample_data, sample_data_size] = load_webster_data();
    if (!sample_data) {
        state.SkipWithError("failed to load sample data file (webster)");
        return;
    }

    if (file_size > sample_data_size) {
        state.SkipWithError("file_size exceeds sample data size");
        return;
    }

    SeekableZstdCachedTemplate tmpl;
    try {
        tmpl = get_cached_seekable_zstd_template(file_size, max_frame_size, sample_data, sample_data_size);
    } catch (const std::exception& e) {
        state.SkipWithError(e.what());
        return;
    }

    UsageStrategy strategy(0, sample_data, sample_data_size, file_size, gamma_shape, gamma_scale,
                           region_size, n_switch);
    std::unique_ptr<char[]> buffer(new char[file_size]);
    std::size_t total_bytes_processed = 0;

    for (auto _ : state) {
        state.PauseTiming();
        std::string fn = get_temporary_filename() + ".seek.zst";
        if (!copy_file(tmpl.path, fn)) {
            state.SkipWithError("copy_file failed");
            break;
        }
        state.ResumeTiming();

        FILE* file = fopen(fn.c_str(), "rb");
        if (!file) {
            state.SkipWithError("fopen failed");
            break;
        }

        ZSTD_seekable* seek = ZSTD_seekable_create();
        if (!seek) {
            fclose(file);
            state.SkipWithError("ZSTD_seekable_create failed");
            break;
        }

        if (ZSTD_isError(ZSTD_seekable_initFile(seek, file))) {
            ZSTD_seekable_free(seek);
            fclose(file);
            state.SkipWithError("ZSTD_seekable_initFile failed");
            break;
        }

        bool failed = false;
        for (std::size_t i = 0; i < n_operations; ++i) {
            const auto op = strategy.get_op();
            size_t n = ZSTD_seekable_decompress(seek, buffer.get(), op.size, op.pos);
            if (ZSTD_isError(n) || n != op.size) {
                ZSTD_seekable_free(seek);
                fclose(file);
                state.SkipWithError("ZSTD_seekable_decompress failed");
                failed = true;
                break;
            }
            total_bytes_processed += op.size;
        }

        ZSTD_seekable_free(seek);
        fclose(file);

        if (failed) break;

        state.PauseTiming();
        remove(fn.c_str());
        state.ResumeTiming();
    }

    state.SetBytesProcessed(total_bytes_processed);
    state.counters["file_size"] = get_file_size(tmpl.path.c_str());
}

const std::vector<std::vector<int64_t>> params_grid = {
    {false, true},
    {1 << 13},
    {1 << 25},
    {1},
    {2},
    {1 << 12},
    {1 << 17},
    {false}, // {false, true},
    {1 << 8, 1 << 9, 1 << 10, 1 << 11, 1 << 12, 1 << 13, 1 << 14, 1 << 15, 1 << 16, 1 << 17, 1 << 18, 1 << 19, 1 << 20, 1 << 21},
};

BENCHMARK(BM_stdio_OptimalUsage)
    ->ArgsProduct(params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK(BM_compio_OptimalUsage)
    ->ArgsProduct(params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

// zran benchmark: read-only, so no is_write param. Uses flush_interval as the last param.
// We use the same operation/switch/region params as the others, but only read (no write).
const std::vector<std::vector<int64_t>> zran_params_grid = {
    {1 << 13},                          // n_operations
    {1 << 25},                          // file_size
    {1}, // n_switch
    {2},                                // gamma_shape
    {1 << 12},                          // gamma_scale
    {1 << 17},                          // region_size
    {1 << 15, 1 << 16, 1 << 17, 1 << 18, 1 << 19, 1 << 20, 1 << 21}, // flush_interval
};

BENCHMARK(BM_zran_OptimalUsage)
    ->ArgsProduct(zran_params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

// seekable zstd benchmark: read-only, no is_write param. Uses max_frame_size as the last param.
const std::vector<std::vector<int64_t>> seekable_zstd_params_grid = {
    {1 << 13},                          // n_operations
    {1 << 25},                          // file_size
    {1}, // n_switch
    {2},                                // gamma_shape
    {1 << 12},                          // gamma_scale
    {1 << 17},                          // region_size
    {1 << 9, 1 << 10, 1 << 11, 1 << 12, 1 << 13, 1 << 14, 1 << 15, 1 << 16, 1 << 17, 1 << 18, 1 << 19, 1 << 20, 1 << 21}, // max_frame_size
};

BENCHMARK(BM_seekable_zstd_OptimalUsage)
    ->ArgsProduct(seekable_zstd_params_grid)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
