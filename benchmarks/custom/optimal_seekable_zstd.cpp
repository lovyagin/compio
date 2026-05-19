#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "benchmark_util.hpp"
#include "optimal_benchmark_constants.hpp"
#include "zstd_seekable.h"

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
        std::cerr << "Usage: " << argv[0] << " <max_frame_size>\n";
        return 1;
    }

    unsigned max_frame_size = std::stoul(argv[1]);

    std::minstd_rand rng(std::random_device{}());

    double tp_sum = 0, tp_sum_sq = 0;
    double file_size_sum = 0, file_size_sum_sq = 0;
    int iterations = 0;
    Timer total_timer;

    while (iterations < MIN_ITERATIONS ||
           (total_timer.elapsed_seconds() < MAX_SECONDS && iterations < MAX_ITERATIONS)) {
        auto [sample_data, file_offset] = load_random_sample_data(rng);

        std::string file_path = get_temporary_filename() + ".seek.zst";
        write_seekable_zstd(file_path, sample_data.data(), FILE_SIZE, max_frame_size);

        unsigned long file_size_stored = get_file_size(file_path.c_str());

        FILE *file = fopen(file_path.c_str(), "rb");
        if (!file) {
            std::cerr << "fopen failed\n";
            remove(file_path.c_str());
            break;
        }
        ZSTD_seekable *seek = ZSTD_seekable_create();
        if (!seek || ZSTD_isError(ZSTD_seekable_initFile(seek, file))) {
            if (seek)
                ZSTD_seekable_free(seek);
            fclose(file);
            remove(file_path.c_str());
            std::cerr << "ZSTD_seekable init failed\n";
            break;
        }

        UsageStrategy strategy(0, sample_data.data(), sample_data.size(), FILE_SIZE, GAMMA_SHAPE,
                               GAMMA_SCALE, REGION_SIZE, N_SWITCH);
        std::unique_ptr<char[]> buffer(new char[FILE_SIZE]);

        Timer iter_timer;
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
        remove(file_path.c_str());
        if (failed)
            break;

        double elapsed = iter_timer.elapsed_seconds();
        double tp = static_cast<double>(iter_bytes) / elapsed;
        tp_sum += tp;
        tp_sum_sq += tp * tp;
        file_size_sum += static_cast<double>(file_size_stored);
        file_size_sum_sq +=
            static_cast<double>(file_size_stored) * static_cast<double>(file_size_stored);
        ++iterations;
    }

    if (iterations == 0)
        return 1;

    double mean = tp_sum / iterations;
    double variance = (tp_sum_sq / iterations) - (mean * mean);
    double stddev = std::sqrt(variance > 0 ? variance : 0);
    double fs_mean = file_size_sum / iterations;
    double fs_variance = (file_size_sum_sq / iterations) - (fs_mean * fs_mean);
    double fs_stddev = std::sqrt(fs_variance > 0 ? fs_variance : 0);

    std::cout << "mean_throughput,stddev_throughput,mean_file_size,stddev_file_size,n_iterations,"
                 "max_frame_size\n";
    std::cout << mean << "," << stddev << "," << fs_mean << "," << fs_stddev << "," << iterations
              << "," << max_frame_size << "\n";

    return 0;
}
