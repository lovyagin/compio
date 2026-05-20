#ifndef OPTIMAL_BENCHMARK_CONSTANTS_HPP_
#define OPTIMAL_BENCHMARK_CONSTANTS_HPP_

#include <chrono>
#include <cstddef>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

static constexpr std::size_t N_OPERATIONS = 1 << 13;
static constexpr std::size_t FILE_SIZE = 1 << 25;
static constexpr std::size_t N_SWITCH = 1;
static constexpr double GAMMA_SHAPE = 2.0;
static constexpr double GAMMA_SCALE = 1 << 12;
static constexpr std::size_t REGION_SIZE = 1 << 17;

static constexpr bool IS_WRITE = false;
static constexpr bool DISABLE_CACHE = true;

static constexpr const char *SAMPLE_FILE = BENCHMARK_DATA_DIR "/enwik9";

struct BenchmarkLoop {
    int min_iterations;
    int max_iterations;
    double max_seconds;
    int count = 0;
    using clock = std::chrono::high_resolution_clock;
    clock::time_point start = clock::now();

    BenchmarkLoop(int min_it, int max_it, double max_sec)
        : min_iterations(min_it),
          max_iterations(max_it),
          max_seconds(max_sec) {}

    bool done() const {
        if (count < min_iterations)
            return false;
        if (count >= max_iterations)
            return true;
        double elapsed = std::chrono::duration<double>(clock::now() - start).count();
        return elapsed >= max_seconds;
    }

    void reset() {
        count = 0;
        start = clock::now();
    }
};

BenchmarkLoop outer_loop(20, 20, 0);
BenchmarkLoop inner_loop(15, 15, 0);

inline std::size_t get_sample_file_size() {
    std::ifstream sf(SAMPLE_FILE, std::ios::binary | std::ios::ate);
    if (!sf.is_open())
        throw std::runtime_error("could not open sample file");
    std::streamsize sample_file_size = sf.tellg();
    sf.close();

    return sample_file_size;
}

inline std::vector<char> load_sample_data_by_offset(std::size_t offset) {
    if (offset > get_sample_file_size() - FILE_SIZE)
        throw std::runtime_error("offset is too big");

    std::ifstream file(SAMPLE_FILE, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::vector<char> data(FILE_SIZE);
    if (!file.read(data.data(), FILE_SIZE))
        throw std::runtime_error("failed to read sample data");
    return data;
}

inline std::pair<std::vector<char>, std::size_t> load_random_sample_data(std::minstd_rand &rng) {
    std::size_t max_offset = get_sample_file_size() - FILE_SIZE;
    std::uniform_int_distribution<std::size_t> offset_dist(0, max_offset);
    std::size_t offset = offset_dist(rng);

    std::ifstream file(SAMPLE_FILE, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::vector<char> data(FILE_SIZE);
    if (!file.read(data.data(), FILE_SIZE))
        throw std::runtime_error("failed to read sample data");
    return {std::move(data), offset};
}

#endif // OPTIMAL_BENCHMARK_CONSTANTS
