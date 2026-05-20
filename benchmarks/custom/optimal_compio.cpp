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

    std::size_t sample_file_size = get_sample_file_size();
    std::size_t file_offset = 0;

    std::unique_ptr<char[]> buffer(new char[FILE_SIZE]);
    std::cout << "file_offset,throughput,file_size,block_cache_hit\n";

    for (outer_loop.reset(); !outer_loop.done(); ++outer_loop.count) {
        auto sample_data = load_sample_data_by_offset(file_offset);
        file_offset = (file_offset + FILE_SIZE) % (sample_file_size - FILE_SIZE);

        UsageStrategy strategy(0, sample_data.data(), sample_data.size(), FILE_SIZE, GAMMA_SHAPE,
                               GAMMA_SCALE, REGION_SIZE, N_SWITCH);

        std::string file_path = get_temporary_filename();
        std::string wal_path = file_path + ".wal";
        create_archive(file_path, config, sample_data.data());
        unsigned long file_size_stored = get_file_size(file_path.c_str());

        for (inner_loop.reset(); !inner_loop.done(); ++inner_loop.count) {
            std::string work_path = file_path;
            std::string work_wal_path = wal_path;
            if constexpr (IS_WRITE) {
                work_path = file_path + ".copy";
                work_wal_path = work_path + ".wal";
                copy_file(file_path.c_str(), work_path.c_str());
                copy_file(wal_path.c_str(), work_wal_path.c_str());
            }

            compio_archive *archive =
                compio_open_archive(work_path.c_str(), IS_WRITE ? "r+" : "r", &config);
            if (!archive) {
                std::cerr << "compio_open_archive failed\n";
                if constexpr (IS_WRITE) {
                    remove(work_path.c_str());
                    remove(work_wal_path.c_str());
                }
                break;
            }
            compio_file *file = compio_open_file("A", archive);
            if (!file) {
                compio_close_archive(archive);
                std::cerr << "compio_open_file failed\n";
                if constexpr (IS_WRITE) {
                    remove(work_path.c_str());
                    remove(work_wal_path.c_str());
                }
                break;
            }

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

            double block_cache_hit = 0.0;
            if (!failed) {
                block_cache_hit = archive->block_reader->get_cache_hit_probability();
            }

            compio_close_file(file);
            compio_close_archive(archive);

            if constexpr (IS_WRITE) {
                remove(work_path.c_str());
                remove(work_wal_path.c_str());
            }

            if (failed)
                break;

            double elapsed = iter_timer.elapsed_seconds();
            double tp = static_cast<double>(iter_bytes) / elapsed;

            std::cout << file_offset << "," << tp << "," << file_size_stored << ","
                      << block_cache_hit << "\n";
        }

        remove(file_path.c_str());
        remove(wal_path.c_str());
    }

    return 0;
}
