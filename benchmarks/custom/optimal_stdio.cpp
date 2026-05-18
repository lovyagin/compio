#include <benchmark_util.hpp>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>

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
    return "/dev/shm/stdio_bench_" + std::to_string(d(rng));
}

static void create_template(const std::string &path, const char *sample_data,
                            std::size_t sample_data_size) {
    if (sample_data_size < FILE_SIZE)
        throw std::runtime_error("sample data is too small");

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
        std::cerr << "Usage: " << argv[0] << " [file_path]\n";
        return 1;
    }

    auto [sample_data, sample_data_size] = load_webster_data();
    if (!sample_data || sample_data_size < FILE_SIZE) {
        std::cerr << "Invalid sample data\n";
        return 1;
    }

    bool keep_file = false;
    std::string file_path;
    if (argc >= 2) {
        file_path = argv[1];
        keep_file = true;
        struct stat st;
        if (stat(file_path.c_str(), &st) != 0) {
            create_template(file_path, sample_data, sample_data_size);
        }
    } else {
        file_path = make_temp_path();
        create_template(file_path, sample_data, sample_data_size);
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
        std::string fn;
        fn = get_temporary_filename();
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
            if (fseek(file, op.pos, SEEK_SET) != 0) {
                failed = true;
                break;
            }
            std::size_t bytes = fread(buffer.get(), 1, op.size, file);

            if (bytes != op.size) {
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
