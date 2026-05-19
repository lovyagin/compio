#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include "zran.h"
}

#include "benchmark_util.hpp"
#include "optimal_benchmark_constants.hpp"

static void write_gzip_with_flush(const std::string &path, const char *data, std::size_t data_size,
                                  std::size_t flush_interval) {
    gzFile gz = gzopen(path.c_str(), "wb1");
    if (!gz)
        throw std::runtime_error("gzopen failed");
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
    if (gzclose(gz) != Z_OK)
        throw std::runtime_error("gzclose failed");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <flush_interval>\n";
        return 1;
    }

    std::size_t flush_interval = std::stoull(argv[1]);

    std::minstd_rand rng(std::random_device{}());

    double tp_sum = 0, tp_sum_sq = 0;
    double file_size_sum = 0, file_size_sum_sq = 0;
    int iterations = 0;
    Timer total_timer;

    while (iterations < MIN_ITERATIONS ||
           (total_timer.elapsed_seconds() < MAX_SECONDS && iterations < MAX_ITERATIONS)) {
        auto [sample_data, file_offset] = load_random_sample_data(rng);

        std::string file_path = get_temporary_filename() + ".gz";
        write_gzip_with_flush(file_path, sample_data.data(), FILE_SIZE, flush_interval);

        FILE *f = fopen(file_path.c_str(), "rb");
        if (!f) {
            remove(file_path.c_str());
            throw std::runtime_error("fopen for index build failed");
        }
        struct deflate_index *index = nullptr;
        int access_points = deflate_index_build(f, flush_interval, &index);
        fclose(f);
        if (access_points <= 0 || !index) {
            remove(file_path.c_str());
            throw std::runtime_error("deflate_index_build failed");
        }

        std::size_t index_size = static_cast<std::size_t>(access_points) * (1 << 15);
        unsigned long file_size_stored = get_file_size(file_path.c_str()) + index_size;

        FILE *file = fopen(file_path.c_str(), "rb");
        if (!file) {
            std::cerr << "fopen failed\n";
            deflate_index_free(index);
            remove(file_path.c_str());
            break;
        }

        UsageStrategy strategy(0, sample_data.data(), sample_data.size(), FILE_SIZE, GAMMA_SHAPE,
                               GAMMA_SCALE, REGION_SIZE, N_SWITCH);
        std::unique_ptr<unsigned char[]> buffer(new unsigned char[FILE_SIZE]);

        Timer iter_timer;
        bool failed = false;
        std::size_t iter_bytes = 0;
        for (std::size_t i = 0; i < N_OPERATIONS; ++i) {
            auto op = strategy.get_op();
            ptrdiff_t n = deflate_index_extract(file, index, op.pos, buffer.get(), op.size);
            if (n != static_cast<ptrdiff_t>(op.size)) {
                failed = true;
                break;
            }
            iter_bytes += op.size;
        }

        fclose(file);
        deflate_index_free(index);
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

    std::cout << mean << "," << stddev << "," << fs_mean << "," << fs_stddev << "\n";

    return 0;
}
