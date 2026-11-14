#ifndef TEST_UTIL_HPP_
#define TEST_UTIL_HPP_

#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <vector>

// Removes every generated archive and its journal when the test process exits, so
// tests do not have to know that the library creates "<path>.wal" next to the archive.
class tmp_file_registry {
    std::mutex mutex_;
    std::vector<std::string> paths_;

public:
    static tmp_file_registry &instance() {
        static tmp_file_registry registry;
        return registry;
    }

    void add(const std::string &path) {
        std::lock_guard<std::mutex> lock(mutex_);
        paths_.push_back(path);
    }

    ~tmp_file_registry() {
        for (const auto &path : paths_) {
            std::remove(path.c_str());
            std::remove((path + ".wal").c_str());
        }
    }
};

inline void generate_tmp_fn(char *fn, std::size_t max_size) {
    namespace fs = std::filesystem;
    fs::path dir;
    try {
        dir = fs::temp_directory_path();
    } catch(const fs::filesystem_error& e) {
        dir = fs::current_path();
    }

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
    tmp_file_registry::instance().add(result);
}

#endif // TEST_UTIL_HPP_