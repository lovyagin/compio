#include <benchmark_util.hpp>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>

#include "compio/compio_file.hpp"
#include "compio/storage_block_reader.hpp"
#include "compio.h"

static constexpr std::size_t N_OPERATIONS = 1 << 13;
static constexpr std::size_t FILE_SIZE = 1 << 25;
static constexpr std::size_t N_SWITCH = 1;
static constexpr double GAMMA_SHAPE = 2.0;
static constexpr double GAMMA_SCALE = 1 << 12;
static constexpr std::size_t REGION_SIZE = 1 << 17;
static constexpr bool IS_WRITE = false;
static constexpr bool DISABLE_CACHE = false;

static constexpr int MIN_ITERATIONS = 3;
static constexpr double MAX_SECONDS = 15.0;
static constexpr int MAX_ITERATIONS = 1000;

static std::string make_temp_path() {
    static std::minstd_rand rng(std::random_device{}());
    static std::uniform_int_distribution<int> d(10000, 99999);
    return "/dev/shm/compio_bench_" + std::to_string(d(rng));
}

static void create_archive(const std::string &path, const compio_config &config,
                           const char *sample_data, std::size_t sample_data_size) {
    std::string wal_path = path + ".wal";
    remove(path.c_str());
    remove(wal_path.c_str());

    if (sample_data_size < FILE_SIZE)
        throw std::runtime_error("sample data is too small");

    compio_archive *archive = compio_open_archive(path.c_str(), "w+", &config);
    if (!archive)
        throw std::runtime_error("compio_open_archive failed");
    compio_file *file = compio_open_file("A", archive);
    if (!file) {
        compio_close_archive(archive);
        throw std::runtime_error("compio_open_file failed");
    }
    if (compio_write(sample_data, FILE_SIZE, file) != FILE_SIZE) {
        compio_close_file(file);
        compio_close_archive(archive);
        throw std::runtime_error("compio_write failed");
    }
    compio_close_file(file);
    compio_close_archive(archive);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <compressor> <level> <block_size> [file_path]\n";
        return 1;
    }

    std::string compressor = argv[1];
    int level = std::atoi(argv[2]);
    int block_size = std::atoi(argv[3]);

    auto [sample_data, sample_data_size] = load_webster_data();
    if (!sample_data || sample_data_size < FILE_SIZE) {
        std::cerr << "Invalid sample data\n";
        return 1;
    }

    compio_config config;
    compio_build_default_config(&config);
    if (compressor == "zlib")
        compio_build_zlib_compressor(&config.compressor);
    else if (compressor == "lz4")
        compio_build_lz4_compressor(&config.compressor);
    else if (compressor == "zstd")
        compio_build_zstd_compressor(&config.compressor);
    else if (compressor == "brotli")
        compio_build_brotli_compressor(&config.compressor);
    else if (compressor == "dummy")
        compio_build_dummy_compressor(&config.compressor);
    else
        throw std::runtime_error("Unknown compressor: " + compressor);
    config.compressor.level = level;
    config.block_size = block_size / 4;
    config.block_size__minimum = block_size / 4 / 4;
    config.block_size__maximum = block_size * 4 / 4;
    config.cache_size__blocks = 1;
    config.wal_sync_mode = COMPIO_WAL_SYNC_NORMAL;

    bool keep_file = false;
    std::string file_path;
    if (argc >= 5) {
        file_path = argv[4];
        keep_file = true;
        struct stat st;
        if (stat(file_path.c_str(), &st) != 0) {
            create_archive(file_path, config, sample_data, sample_data_size);
        }
    } else {
        file_path = make_temp_path();
        create_archive(file_path, config, sample_data, sample_data_size);
    }

    unsigned long file_size_stored = get_file_size(file_path.c_str());

    UsageStrategy strategy(0, sample_data, sample_data_size, FILE_SIZE, GAMMA_SHAPE, GAMMA_SCALE,
                           REGION_SIZE, N_SWITCH);
    std::unique_ptr<char[]> buffer(new char[FILE_SIZE]);

    double tp_sum = 0, tp_sum_sq = 0;
    double block_cache_hit_sum = 0;
    int iterations = 0;
    Timer total_timer;

    while (iterations < MIN_ITERATIONS ||
           (total_timer.elapsed_seconds() < MAX_SECONDS && iterations < MAX_ITERATIONS)) {
        std::string fn = get_temporary_filename();
        std::string wal_fn = fn + ".wal";
        if (!copy_file(file_path, fn) || !copy_file(file_path + ".wal", wal_fn)) {
            std::cerr << "copy_file failed\n";
            break;
        }

        Timer iter_timer;
        compio_archive *archive = compio_open_archive(fn.c_str(), IS_WRITE ? "r+" : "r", &config);
        if (!archive) {
            std::cerr << "compio_open_archive failed\n";
            break;
        }
        compio_file *file = compio_open_file("A", archive);
        if (!file) {
            compio_close_archive(archive);
            std::cerr << "compio_open_file failed\n";
            break;
        }

        bool failed = false;
        std::size_t iter_bytes = 0;
        for (std::size_t i = 0; i < N_OPERATIONS; ++i) {
            auto op = strategy.get_op();
            if (compio_seek(file, op.pos, COMPIO_SEEK_SET) != 0) {
                failed = true;
                break;
            }
            std::size_t bytes = 0;
            if (IS_WRITE)
                bytes = compio_write(op.data, op.size, file);
            else
                bytes = compio_read(buffer.get(), op.size, file);

            if (bytes != op.size) {
                failed = true;
                break;
            }
            iter_bytes += op.size;

            if (DISABLE_CACHE)
                compio_flush(archive);
        }

        if (!failed) {
            block_cache_hit_sum += archive->block_reader->get_cache_hit_probability();
        }

        compio_close_file(file);
        compio_close_archive(archive);
        remove(fn.c_str());
        remove(wal_fn.c_str());

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
    double avg_block_hit = block_cache_hit_sum / iterations;

    std::cout << mean << "," << stddev << "," << avg_block_hit << ","
              << static_cast<double>(file_size_stored) << "\n";

    if (!keep_file) {
        remove(file_path.c_str());
        remove((file_path + ".wal").c_str());
    }

    return 0;
}
