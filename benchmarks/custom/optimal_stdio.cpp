#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>

#include "benchmark_util.hpp"
#include "optimal_benchmark_constants.hpp"

static void create_template(const std::string &path, const char *sample_data) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        throw std::runtime_error("fopen for template creation failed");
    if (fwrite(sample_data, 1, FILE_SIZE, f) != FILE_SIZE) {
        fclose(f);
        throw std::runtime_error("fwrite for template creation failed");
    }
    fclose(f);
}

int main(int argc, char *argv[]) {
    if (argc < 1) {
        std::cerr << "Usage: " << argv[0] << "\n";
        return 1;
    }

    std::minstd_rand rng(std::random_device{}());

    double tp_sum = 0, tp_sum_sq = 0;
    double file_size_sum = 0, file_size_sum_sq = 0;
    int iterations = 0;
    Timer total_timer;

    while (iterations < MIN_ITERATIONS ||
           (total_timer.elapsed_seconds() < MAX_SECONDS && iterations < MAX_ITERATIONS)) {
        auto [sample_data, file_offset] = load_random_sample_data(rng);

        std::string file_path = get_temporary_filename();
        create_template(file_path, sample_data.data());

        unsigned long file_size_stored = get_file_size(file_path.c_str());

        FILE *file = fopen(file_path.c_str(), IS_WRITE ? "r+b" : "rb");
        if (!file) {
            std::cerr << "fopen failed\n";
            remove(file_path.c_str());
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
            if (fseek(file, op.pos, SEEK_SET) != 0) {
                failed = true;
                break;
            }
            std::size_t bytes;
            if (IS_WRITE)
                bytes = fwrite(op.data, 1, op.size, file);
            else
                bytes = fread(buffer.get(), 1, op.size, file);

            if (bytes != op.size) {
                failed = true;
                break;
            }
            iter_bytes += op.size;
        }

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

    std::cout << "mean_throughput,stddev_throughput,mean_file_size,stddev_file_size,n_iterations\n";
    std::cout << mean << "," << stddev << "," << fs_mean << "," << fs_stddev << "," << iterations
              << "\n";

    return 0;
}
