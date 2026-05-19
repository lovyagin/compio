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

    std::minstd_rand rng;
    int iterations = 0;
    Timer total_timer;

    std::cout << "seed,throughput_bytes_per_sec,file_size\n";

    while (iterations < MIN_ITERATIONS ||
           (total_timer.elapsed_seconds() < MAX_SECONDS && iterations < MAX_ITERATIONS)) {
        rng.seed(static_cast<std::minstd_rand::result_type>(iterations));
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

        UsageStrategy strategy(iterations, sample_data.data(), sample_data.size(), FILE_SIZE,
                               GAMMA_SHAPE, GAMMA_SCALE, REGION_SIZE, N_SWITCH);
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

        std::cout << iterations << "," << tp << "," << file_size_stored << "\n";
        ++iterations;
    }

    if (iterations == 0)
        return 1;

    std::cout << "# n_iterations=" << iterations << "\n";

    return 0;
}
