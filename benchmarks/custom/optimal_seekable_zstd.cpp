#include <benchmark_util.hpp>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "zstd_seekable.h"

static constexpr std::size_t N_OPERATIONS = 1 << 13;
static constexpr std::size_t FILE_SIZE = 1 << 25;
static constexpr std::size_t N_SWITCH = 1;
static constexpr double GAMMA_SHAPE = 2.0;
static constexpr double GAMMA_SCALE = 1 << 12;
static constexpr std::size_t REGION_SIZE = 1 << 17;

static constexpr int MIN_ITERATIONS = 3;
static constexpr double MAX_SECONDS = 15.0;
static constexpr int MAX_ITERATIONS = 1000;

static std::string make_temp_path() {
    static std::minstd_rand rng(std::random_device{}());
    static std::uniform_int_distribution<int> d(10000, 99999);
    return "/dev/shm/seekable_zstd_bench_" + std::to_string(d(rng)) + ".seek.zst";
}

static void write_seekable_zstd(const std::string &path, const char *data, std::size_t data_size,
                                unsigned max_frame_size) {
    FILE *out = fopen(path.c_str(), "wb");
    if (!out)
        throw std::runtime_error("fopen failed");
    ZSTD_seekable_CStream *stream = ZSTD_seekable_createCStream();
    if (!stream) {
        fclose(out);
        throw std::runtime_error("ZSTD_seekable_createCStream failed");
    }
    size_t ret = ZSTD_seekable_initCStream(stream, 1, 1, max_frame_size);
    if (ZSTD_isError(ret)) {
        ZSTD_seekable_freeCStream(stream);
        fclose(out);
        throw std::runtime_error("ZSTD_seekable_initCStream failed");
    }
    std::vector<char> outbuf(ZSTD_CStreamOutSize());
    const std::size_t CHUNK = 1 << 15;
    std::size_t pos = 0;
    while (pos < data_size) {
        std::size_t chunk = std::min(CHUNK, data_size - pos);
        ZSTD_inBuffer in{data + pos, chunk, 0};
        while (in.pos < in.size) {
            ZSTD_outBuffer outb{outbuf.data(), outbuf.size(), 0};
            ret = ZSTD_seekable_compressStream(stream, &outb, &in);
            if (ZSTD_isError(ret)) {
                ZSTD_seekable_freeCStream(stream);
                fclose(out);
                throw std::runtime_error("compressStream failed");
            }
            fwrite(outbuf.data(), 1, outb.pos, out);
        }
        pos += chunk;
    }
    do {
        ZSTD_outBuffer outb{outbuf.data(), outbuf.size(), 0};
        ret = ZSTD_seekable_endStream(stream, &outb);
        if (ZSTD_isError(ret)) {
            ZSTD_seekable_freeCStream(stream);
            fclose(out);
            throw std::runtime_error("endStream failed");
        }
        fwrite(outbuf.data(), 1, outb.pos, out);
    } while (ret != 0);
    ZSTD_seekable_freeCStream(stream);
    fclose(out);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <max_frame_size> [file_path]\n";
        return 1;
    }

    unsigned max_frame_size = std::stoul(argv[1]);

    auto [sample_data, sample_data_size] = load_webster_data();
    if (!sample_data || sample_data_size < FILE_SIZE) {
        std::cerr << "Invalid sample data\n";
        return 1;
    }

    bool keep_file = false;
    std::string file_path;
    if (argc >= 3) {
        file_path = argv[2];
        keep_file = true;
        struct stat st;
        if (stat(file_path.c_str(), &st) != 0) {
            write_seekable_zstd(file_path, sample_data, FILE_SIZE, max_frame_size);
        }
    } else {
        file_path = make_temp_path();
        write_seekable_zstd(file_path, sample_data, FILE_SIZE, max_frame_size);
    }

    unsigned long file_size_stored = get_file_size(file_path.c_str());

    UsageStrategy strategy(0, sample_data, sample_data_size, FILE_SIZE, GAMMA_SHAPE, GAMMA_SCALE,
                           REGION_SIZE, N_SWITCH);
    std::unique_ptr<char[]> buffer(new char[FILE_SIZE]);

    double tp_sum = 0, tp_sum_sq = 0;
    int iterations = 0;
    Timer total_timer;

    while (iterations < MIN_ITERATIONS ||
           (total_timer.elapsed_seconds() < MAX_SECONDS && iterations < MAX_ITERATIONS)) {
        std::string fn = get_temporary_filename() + ".seek.zst";
        if (!copy_file(file_path, fn)) {
            std::cerr << "copy_file failed\n";
            break;
        }

        Timer iter_timer;
        FILE *file = fopen(fn.c_str(), "rb");
        if (!file) {
            std::cerr << "fopen failed\n";
            break;
        }
        ZSTD_seekable *seek = ZSTD_seekable_create();
        if (!seek || ZSTD_isError(ZSTD_seekable_initFile(seek, file))) {
            if (seek)
                ZSTD_seekable_free(seek);
            fclose(file);
            std::cerr << "ZSTD_seekable init failed\n";
            break;
        }

        bool failed = false;
        std::size_t iter_bytes = 0;
        for (std::size_t i = 0; i < N_OPERATIONS; ++i) {
            auto op = strategy.get_op();
            size_t n = ZSTD_seekable_decompress(seek, buffer.get(), op.size, op.pos);
            if (ZSTD_isError(n) || n != op.size) {
                failed = true;
                break;
            }
            iter_bytes += op.size;
        }

        ZSTD_seekable_free(seek);
        fclose(file);
        remove(fn.c_str());
        if (failed)
            break;

        double elapsed = iter_timer.elapsed_seconds();
        double tp = static_cast<double>(iter_bytes) / elapsed;
        tp_sum += tp;
        tp_sum_sq += tp * tp;
        ++iterations;
    }

    if (iterations == 0)
        return 1;

    double mean = tp_sum / iterations;
    double variance = (tp_sum_sq / iterations) - (mean * mean);
    double stddev = std::sqrt(variance > 0 ? variance : 0);

    std::cout << mean << "," << stddev << ","
              << static_cast<double>(file_size_stored) << "\n";

    if (!keep_file) {
        remove(file_path.c_str());
    }

    return 0;
}
