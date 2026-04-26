#include <benchmark/benchmark.h>
#include <random>
#include <vector>
#include <cstdio>
#include <memory>
#include <algorithm>
#include <exception>
#include <cstring>
#include <sys/stat.h>

#include "benchmark_util.hpp"

#include "compio.h"
#include <zlib.h>
#include "zstd_seekable.h"
extern "C" {
#include "zran.h"
}

constexpr size_t UNCOMPRESSED_SIZE = 33'554'432;
constexpr size_t READ_SIZE = 4096;
constexpr size_t OPS_PER_BENCH = 1000;

static std::vector<char> sample_data;
static size_t sample_data_size = 0;

extern std::pair<const char*, size_t> load_webster_data();

extern compio_config config;

static std::vector<char> build_payload() {
    if (sample_data.empty()) {
        auto [data, sz] = load_webster_data();
        sample_data.assign(data, data + sz);
        sample_data_size = sz;
    }
    std::vector<char> payload(UNCOMPRESSED_SIZE);
    for (size_t i = 0; i < UNCOMPRESSED_SIZE; i += sample_data_size) {
        size_t copy = std::min(sample_data_size, UNCOMPRESSED_SIZE - i);
        std::copy_n(sample_data.data(), copy, payload.data() + i);
    }
    return payload;
}

static void write_gzip_with_flush(const std::string& path, const std::vector<char>& payload,
                                  size_t flush_interval) {
    gzFile gz = gzopen(path.c_str(), "wb");
    if (!gz) throw std::runtime_error("gzopen failed");
    size_t pos = 0, flushed = 0;
    while (pos < payload.size()) {
        size_t chunk = std::min<size_t>(1 << 15, payload.size() - pos);
        if (gzwrite(gz, payload.data() + pos, chunk) != (int)chunk) {
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

static void write_seekable_zstd(const std::string& path, const std::vector<char>& payload,
                                unsigned max_frame_size) {
    FILE* out = fopen(path.c_str(), "wb");
    if (!out) throw std::runtime_error("fopen failed");
    ZSTD_seekable_CStream* stream = ZSTD_seekable_createCStream();
    if (!stream) { fclose(out); throw std::runtime_error("ZSTD_seekable_createCStream failed"); }
    size_t ret = ZSTD_seekable_initCStream(stream, 5, 1, max_frame_size);
    if (ZSTD_isError(ret)) { ZSTD_seekable_freeCStream(stream); fclose(out);
        throw std::runtime_error("ZSTD_seekable_initCStream failed"); }
    std::vector<char> outbuf(ZSTD_CStreamOutSize());
    size_t pos = 0;
    const size_t CHUNK = 1 << 15;
    while (pos < payload.size()) {
        size_t chunk = std::min(CHUNK, payload.size() - pos);
        ZSTD_inBuffer in{payload.data() + pos, chunk, 0};
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

static std::vector<std::pair<size_t, size_t>> generate_random_ops() {
    std::vector<std::pair<size_t, size_t>> ops;
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> pos_dist(0, UNCOMPRESSED_SIZE - READ_SIZE);
    for (size_t i = 0; i < OPS_PER_BENCH; ++i)
        ops.emplace_back(pos_dist(rng), READ_SIZE);
    return ops;
}
static const std::vector<std::pair<size_t, size_t>> kReadOps = generate_random_ops();

static void BM_compio_Scatter(benchmark::State& state) {
    const size_t block_size = state.range(0);
    config.block_size = block_size;
    config.block_size__minimum = block_size / 4;
    config.block_size__maximum = block_size * 4;

    static std::vector<char> payload = build_payload();
    std::string fn = get_temporary_filename();

    compio_archive* arch = compio_open_archive(fn.c_str(), "w+", &config);
    if (!arch) throw std::runtime_error("compio_open_archive failed");
    compio_file* f = compio_open_file("A", arch);
    if (!f) { compio_close_archive(arch); throw std::runtime_error("compio_open_file failed"); }
    size_t written = compio_write(payload.data(), payload.size(), f);
    if (written != payload.size()) { compio_close_file(f); compio_close_archive(arch);
        throw std::runtime_error("compio_write failed"); }
    compio_close_file(f);
    compio_close_archive(arch);

    size_t total_bytes = 0;
    std::vector<char> buffer(READ_SIZE);
    for (auto _ : state) {
        compio_archive* arch = compio_open_archive(fn.c_str(), "r", &config);
        if (!arch) { state.SkipWithError("open archive failed"); break; }
        compio_file* f = compio_open_file("A", arch);
        if (!f) { compio_close_archive(arch); state.SkipWithError("open file failed"); break; }

        for (auto [pos, len] : kReadOps) {
            if (compio_seek(f, pos, COMPIO_SEEK_SET) != 0) {
                state.SkipWithError("seek failed"); break;
            }
            size_t n = compio_read(buffer.data(), len, f);
            if (n != len) {
                state.SkipWithError("read failed"); break;
            }
            total_bytes += len;
            compio_flush(arch);
        }
        compio_close_file(f);
        compio_close_archive(arch);
    }
    state.SetBytesProcessed(total_bytes);
    state.counters["file_size"] = get_file_size(fn.c_str());
    remove(fn.c_str()); remove((fn + ".wal").c_str());
}

static void BM_zran_Scatter(benchmark::State& state) {
    const size_t flush_interval = state.range(0);
    std::vector<char> payload = build_payload();
    std::string gz_path;
    struct deflate_index* zran_index = nullptr;
    size_t index_size = 0;
    size_t total_file_size = 0;

    if (zran_index) deflate_index_free(zran_index);
    gz_path = get_temporary_filename() + ".gz";
    write_gzip_with_flush(gz_path, payload, flush_interval);
    FILE* f = fopen(gz_path.c_str(), "rb");
    if (!f) throw std::runtime_error("fopen for index build failed");
    int access_points = deflate_index_build(f, flush_interval, &zran_index);
    fclose(f);
    if (access_points <= 0 || !zran_index)
        throw std::runtime_error("deflate_index_build failed");
    index_size = access_points * (1 << 15);
    total_file_size = get_file_size(gz_path.c_str()) + index_size;

    size_t total_bytes = 0;
    std::vector<unsigned char> scratch(READ_SIZE);
    for (auto _ : state) {
        FILE* f = fopen(gz_path.c_str(), "rb");
        if (!f) { state.SkipWithError("fopen failed"); break; }
        for (auto [pos, len] : kReadOps) {
            ptrdiff_t n = deflate_index_extract(f, zran_index, pos, scratch.data(), len);
            if (n != (ptrdiff_t)len) {
                fclose(f); state.SkipWithError("extract failed"); break;
            }
            total_bytes += len;
        }
        fclose(f);
    }
    state.SetBytesProcessed(total_bytes);
    state.counters["file_size"] = total_file_size;
    remove(gz_path.c_str());
}

static void BM_seekable_zstd_Scatter(benchmark::State& state) {
    const size_t max_frame_size = state.range(0);
    std::vector<char> payload = build_payload();
    std::string zst_path;
    size_t total_file_size = 0;

    zst_path = get_temporary_filename() + ".seek.zst";
    write_seekable_zstd(zst_path, payload, max_frame_size);
    total_file_size = get_file_size(zst_path.c_str());

    size_t total_bytes = 0;
    std::vector<char> buffer(READ_SIZE);
    for (auto _ : state) {
        FILE* f = fopen(zst_path.c_str(), "rb");
        if (!f) { state.SkipWithError("fopen failed"); break; }
        ZSTD_seekable* seek = ZSTD_seekable_create();
        if (!seek) { fclose(f); state.SkipWithError("seekable create failed"); break; }
        if (ZSTD_isError(ZSTD_seekable_initFile(seek, f))) {
            ZSTD_seekable_free(seek); fclose(f); state.SkipWithError("initFile failed"); break;
        }
        for (auto [pos, len] : kReadOps) {
            size_t n = ZSTD_seekable_decompress(seek, buffer.data(), len, pos);
            if (ZSTD_isError(n) || n != len) {
                ZSTD_seekable_free(seek); fclose(f); state.SkipWithError("decompress failed"); break;
            }
            total_bytes += len;
        }
        ZSTD_seekable_free(seek);
        fclose(f);
    }
    state.SetBytesProcessed(total_bytes);
    state.counters["file_size"] = total_file_size;
    remove(zst_path.c_str());
}

BENCHMARK(BM_compio_Scatter)
    ->ArgsProduct({{1 << 8, 1 << 9, 1 << 10, 1 << 11, 1 << 12, 1 << 13, 1 << 14, 1 << 15, 1 << 16, 1 << 17, 1 << 18}})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK(BM_zran_Scatter)
    ->ArgsProduct({{1 << 15, 1 << 16, 1 << 17, 1 << 18, 1 << 19, 1 << 20, 1 << 21}})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK(BM_seekable_zstd_Scatter)
    ->ArgsProduct({{1 << 9, 1 << 10, 1 << 11, 1 << 12, 1 << 13, 1 << 14, 1 << 15, 1 << 16, 1 << 17, 1 << 18, 1 << 19, 1 << 20, 1 << 21}})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
