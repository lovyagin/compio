// Write throughput against the number of compression threads, for each built-in
// compressor. Output is CSV: compressor,threads,wal,mib,seconds,mib_per_s.
//
// Usage: parallel_compression [mib] [path]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "compio.h"

namespace {

// Text-like data: compressible, but not to nothing.
std::vector<uint8_t> make_data(size_t size) {
    static const char *words[] = {"archive ", "block ", "index ", "node ",   "write ",  "read ",
                                  "cache ",   "file ",  "tree ",  "commit ", "header ", "space "};
    std::vector<uint8_t> data;
    data.reserve(size);
    uint32_t x = 12345;
    while (data.size() < size) {
        x = x * 1103515245u + 12345u;
        const char *w = words[(x >> 16) % 12];
        for (const char *p = w; *p && data.size() < size; ++p) data.push_back(static_cast<uint8_t>(*p));
        if ((x >> 8) % 7 == 0 && data.size() < size) data.push_back(static_cast<uint8_t>('0' + (x >> 20) % 10));
    }
    return data;
}

double write_once(const char *path, compio_compression_type type, int threads, bool wal,
                  const std::vector<uint8_t> &data, size_t chunk) {
    remove(path);
    remove((std::string(path) + ".wal").c_str());
    compio_config config;
    compio_build_default_config(&config);
    compio_build_compressor_by_type(&config.compressor, type);
    config.compression_threads = threads;
    config.enable_wal = wal;

    const auto start = std::chrono::steady_clock::now();
    compio_archive *archive = compio_open_archive(path, "w+", &config);
    if (!archive) {
        perror("compio_open_archive");
        exit(1);
    }
    compio_file *file = compio_open_file("data", archive);
    for (size_t pos = 0; pos < data.size(); pos += chunk) {
        const size_t n = std::min(chunk, data.size() - pos);
        if (compio_write(data.data() + pos, n, file) != n) {
            perror("compio_write");
            exit(1);
        }
    }
    compio_close_file(file);
    compio_close_archive(archive);
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(end - start).count();
}

} // namespace

int main(int argc, char **argv) {
    const size_t mib = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 256;
    const char *path = argc > 2 ? argv[2] : "parallel_compression.compio";
    const auto data = make_data(mib << 20);
    const unsigned cores = std::thread::hardware_concurrency();

    std::vector<int> thread_counts = {1, 2, 4};
    if (cores > 4) thread_counts.push_back(static_cast<int>(cores));

    const struct {
        compio_compression_type type;
        const char *name;
    } compressors[] = {{COMPIO_COMPRESS_LZ4, "lz4"},
                       {COMPIO_COMPRESS_ZSTD, "zstd"},
                       {COMPIO_COMPRESS_ZLIB, "zlib"},
                       {COMPIO_COMPRESS_BROTLI, "brotli"}};

    printf("compressor,threads,wal,mib,seconds,mib_per_s\n");
    for (const auto &c : compressors) {
        for (const bool wal : {true, false}) {
            for (const int threads : thread_counts) {
                double best = 1e30;
                for (int run = 0; run < 3; ++run) {
                    best = std::min(best, write_once(path, c.type, threads, wal, data, 1 << 20));
                }
                printf("%s,%d,%d,%zu,%.3f,%.1f\n", c.name, threads, wal ? 1 : 0, mib, best,
                       static_cast<double>(mib) / best);
                fflush(stdout);
            }
        }
    }
    remove(path);
    remove((std::string(path) + ".wal").c_str());
    return 0;
}
