#ifndef BENCHMARK_UTIL_HPP_
#define BENCHMARK_UTIL_HPP_

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "compio.h"

inline unsigned long get_file_size(const char *fn) {
    struct stat st;
    if (stat(fn, &st) != 0) {
        return 0;
    }
    return st.st_size;
}

inline std::string get_temporary_filename() {
    static std::minstd_rand rng;
    static std::uniform_int_distribution<int> d(10000, 99999);

    static const std::string prefix = "compio_tmpfile_";

    std::string result;
    do {
        result = prefix + std::to_string(d(rng));
    } while (std::filesystem::exists(result));

    return result;
}

using benchmark_context = std::vector<std::pair<std::string, std::string>>;

benchmark_context build_config_from_file(std::string fn, compio_config *config);

std::pair<const char *, std::size_t> load_webster_data();
std::pair<const char *, std::size_t> load_custom_sample_data(const char *file_path,
                                                             std::size_t offset, std::size_t size);

struct Timer {
    using clock = std::chrono::high_resolution_clock;
    clock::time_point start;
    Timer() : start(clock::now()) {}
    double elapsed_seconds() const {
        return std::chrono::duration<double>(clock::now() - start).count();
    }
};

struct UsageStrategy {
    struct Operation {
        std::size_t pos;
        std::size_t size;
        const char *data;
    };

    UsageStrategy(int seed, const char *sample_data, std::size_t sample_data_size,
                  std::size_t file_size, double gamma_shape, double gamma_scale,
                  std::size_t region_size, std::size_t n_switch);
    Operation get_op();

private:
    std::minstd_rand rng;
    const char *sample_data;
    std::size_t sample_data_size;
    double gamma_shape, gamma_scale;
    std::gamma_distribution<double> gamma_dist;
    std::size_t region_size;
    std::uniform_int_distribution<std::size_t> region_start_dist;
    std::size_t region_start;
    std::size_t n_ops_until_switch, n_switch;
};

bool copy_file(const std::string &src, const std::string &dst);

#endif // BENCHMARK_UTIL_HPP_
