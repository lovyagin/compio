#ifndef BENCHMARK_UTIL_HPP_
#define BENCHMARK_UTIL_HPP_

#include <filesystem>
#include <random>
#include <string>
#include <sys/stat.h>

inline unsigned long get_file_size(const char* fn) {
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

#endif // BENCHMARK_UTIL_HPP_