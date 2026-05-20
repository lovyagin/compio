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

    std::minstd_rand rng;
    std::unique_ptr<unsigned char[]> buffer(new unsigned char[FILE_SIZE]);
    std::cout << "file_offset,throughput,file_size\n";

    for (outer_loop.reset(); !outer_loop.done(); ++outer_loop.count) {
        auto [sample_data, file_offset] = load_random_sample_data(rng);

        UsageStrategy strategy(0, sample_data.data(), sample_data.size(), FILE_SIZE, GAMMA_SHAPE,
                               GAMMA_SCALE, REGION_SIZE, N_SWITCH);

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
            return 1;
        }

        for (inner_loop.reset(); !inner_loop.done(); ++inner_loop.count) {
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

            if (failed)
                break;

            double elapsed = iter_timer.elapsed_seconds();
            double tp = static_cast<double>(iter_bytes) / elapsed;

            std::cout << file_offset << "," << tp << "," << file_size_stored << "\n";
        }

        fclose(file);
        deflate_index_free(index);
        remove(file_path.c_str());
    }

    return 0;
}
