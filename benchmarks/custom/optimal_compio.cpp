#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>

#include "compio/compio_file.hpp"
#include "compio/storage_block_reader.hpp"

#include "benchmark_util.hpp"

#include "compio.h"

#include "optimal_benchmark_constants.hpp"

static void create_archive(const std::string &path, const compio_config &config,
                           const char *sample_data) {
    std::string wal_path = path + ".wal";
    remove(path.c_str());
    remove(wal_path.c_str());

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
        std::cerr << "Usage: " << argv[0] << " <compressor> <level> <block_size>\n";
        return 1;
    }

    std::string compressor = argv[1];
    int level = std::atoi(argv[2]);
    int block_size = std::atoi(argv[3]);

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
    if (DISABLE_CACHE)
        config.cache_size__blocks = 1;
    config.wal_sync_mode = COMPIO_WAL_SYNC_NORMAL;

    std::minstd_rand rng(std::random_device{}());

    double tp_sum = 0, tp_sum_sq = 0;
    double block_cache_hit_sum = 0;
    double file_size_sum = 0, file_size_sum_sq = 0;
    int iterations = 0;
    Timer total_timer;

    while (iterations < MIN_ITERATIONS ||
           (total_timer.elapsed_seconds() < MAX_SECONDS && iterations < MAX_ITERATIONS)) {
        auto [sample_data, file_offset] = load_random_sample_data(rng);

        std::string file_path = get_temporary_filename();
        std::string wal_path = file_path + ".wal";
        create_archive(file_path, config, sample_data.data());

        unsigned long file_size_stored = get_file_size(file_path.c_str());

        compio_archive *archive =
            compio_open_archive(file_path.c_str(), IS_WRITE ? "r+" : "r", &config);
        if (!archive) {
            std::cerr << "compio_open_archive failed\n";
            remove(file_path.c_str());
            remove(wal_path.c_str());
            break;
        }
        compio_file *file = compio_open_file("A", archive);
        if (!file) {
            compio_close_archive(archive);
            remove(file_path.c_str());
            remove(wal_path.c_str());
            std::cerr << "compio_open_file failed\n";
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
        }

        if (!failed) {
            block_cache_hit_sum += archive->block_reader->get_cache_hit_probability();
        }

        compio_close_file(file);
        compio_close_archive(archive);
        remove(file_path.c_str());
        remove(wal_path.c_str());

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
    double avg_block_hit = block_cache_hit_sum / iterations;
    double fs_mean = file_size_sum / iterations;
    double fs_variance = (file_size_sum_sq / iterations) - (fs_mean * fs_mean);
    double fs_stddev = std::sqrt(fs_variance > 0 ? fs_variance : 0);

    std::cout << mean << "," << stddev << "," << avg_block_hit << "," << fs_mean << "," << fs_stddev
              << "\n";

    return 0;
}
