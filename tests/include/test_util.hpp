#ifndef TEST_UTIL_HPP_
#define TEST_UTIL_HPP_

#include <chrono>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>

inline void generate_tmp_fn(char *fn, int max_size) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path();

    // generate random filename
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const auto r = rng();

    fs::path p = dir / ("compio_test_" + std::to_string(now) + "_" + std::to_string(r));
    std::string result = p.string();

    // copy to fn
    if (result.size() > max_size) {
        throw std::runtime_error("failed to generate temporary filename");
    }

    strcpy(fn, result.c_str());

    // create file (open and close)
    FILE *file = fopen(fn, "w+");
    if (file == nullptr) {
        throw std::runtime_error("failed to create temporary file at " + result);
    }

    fclose(file);
}

#endif // TEST_UTIL_HPP_