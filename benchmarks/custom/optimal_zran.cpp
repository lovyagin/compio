#include <benchmark_util.hpp>
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
    return "/dev/shm/zran_bench_" + std::to_string(d(rng)) + ".gz";
}

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
        std::cerr << "Usage: " << argv[0] << " <flush_interval> [file_path]\n";
        return 1;
    }

    std::size_t flush_interval = std::stoull(argv[1]);

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
            write_gzip_with_flush(file_path, sample_data, FILE_SIZE, flush_interval);
        }
    } else {
        file_path = make_temp_path();
        write_gzip_with_flush(file_path, sample_data, FILE_SIZE, flush_interval);
    }

    // Build the zran index
    FILE *f = fopen(file_path.c_str(), "rb");
    if (!f)
        throw std::runtime_error("fopen for index build failed");
    struct deflate_index *index = nullptr;
    int access_points = deflate_index_build(f, flush_interval, &index);
    fclose(f);
    if (access_points <= 0 || !index)
        throw std::runtime_error("deflate_index_build failed");

    std::size_t index_size = static_cast<std::size_t>(access_points) * (1 << 15);
    unsigned long file_size_stored = get_file_size(file_path.c_str()) + index_size;

    UsageStrategy strategy(0, sample_data, sample_data_size, FILE_SIZE, GAMMA_SHAPE, GAMMA_SCALE,
                           REGION_SIZE, N_SWITCH);
    std::unique_ptr<unsigned char[]> buffer(new unsigned char[FILE_SIZE]);

    double tp_sum = 0, tp_sum_sq = 0;
    int iterations = 0;
    Timer total_timer;

    while (iterations < MIN_ITERATIONS ||
           (total_timer.elapsed_seconds() < MAX_SECONDS && iterations < MAX_ITERATIONS)) {
        std::string fn = get_temporary_filename() + ".gz";
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
        remove(fn.c_str());
        if (failed)
            break;

        double elapsed = iter_timer.elapsed_seconds();
        double tp = static_cast<double>(iter_bytes) / elapsed;
        tp_sum += tp;
        tp_sum_sq += tp * tp;
        ++iterations;
    }

    deflate_index_free(index);

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
