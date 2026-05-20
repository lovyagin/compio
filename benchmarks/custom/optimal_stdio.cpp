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
    std::unique_ptr<char[]> buffer(new char[FILE_SIZE]);
    std::cout << "data_seed,usage_seed,throughput_bytes_per_sec,file_size\n";

    for (outer_loop.reset(); !outer_loop.done(); ++outer_loop.count) {
        rng.seed(outer_loop.count + 1);
        auto [sample_data, file_offset] = load_random_sample_data(rng);

        UsageStrategy strategy(0, sample_data.data(), sample_data.size(), FILE_SIZE, GAMMA_SHAPE,
                               GAMMA_SCALE, REGION_SIZE, N_SWITCH);

        std::string file_path = get_temporary_filename();
        create_template(file_path, sample_data.data());
        unsigned long file_size_stored = get_file_size(file_path.c_str());

        for (inner_loop.reset(); !inner_loop.done(); ++inner_loop.count) {
            strategy.seed(inner_loop.count + 1);

            std::string work_path = file_path;
            if constexpr (IS_WRITE) {
                work_path = file_path + ".copy";
                copy_file(file_path.c_str(), work_path.c_str());
            }

            FILE *file = fopen(work_path.c_str(), IS_WRITE ? "r+b" : "rb");
            if (!file) {
                std::cerr << "fopen failed\n";
                if constexpr (IS_WRITE) {
                    remove(work_path.c_str());
                }
                break;
            }

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

            if constexpr (IS_WRITE) {
                remove(work_path.c_str());
            }

            if (failed)
                break;

            double elapsed = iter_timer.elapsed_seconds();
            double tp = static_cast<double>(iter_bytes) / elapsed;

            std::cout << outer_loop.count + 1 << "," << inner_loop.count + 1 << "," << tp << ","
                      << file_size_stored << "\n";
        }

        remove(file_path.c_str());
    }

    return 0;
}
