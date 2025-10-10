#include <fstream>
#include <iostream>
#include <random>
#include <string>

#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"
#include "compio.h"

#include "sample_data.hpp"

std::size_t get_fsize(FILE *file) {
    auto cursor = ftell(file);
    auto ret = fseek(file, 0, SEEK_END);
    if (ret != 0) {
        throw std::runtime_error("fseek returned " + std::to_string(ret));
    }
    auto fsize = ftell(file);
    ret = fseek(file, cursor, SEEK_SET);
    if (ret != 0) {
        throw std::runtime_error("fseek returned " + std::to_string(ret));
    }
    return fsize;
}

void save_csv(const std::vector<std::vector<std::size_t>> &columns,
              const std::vector<std::string> &column_names, const char *fn) {
    std::ofstream ofs(fn);

    if (!ofs.is_open()) {
        throw std::runtime_error("failed to open file for writing");
    }

    for (std::size_t i = 0; i < column_names.size(); ++i) {
        ofs << column_names[i];
        if (i < column_names.size() - 1) {
            ofs << ",";
        }
    }
    ofs << "\n";

    for (size_t i = 0; i < columns[0].size(); ++i) {
        for (size_t j = 0; j < columns.size(); ++j) {
            ofs << columns[j][i];

            if (j < columns.size() - 1) {
                ofs << ",";
            }
        }
        ofs << "\n";
    }

    ofs.close();
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: ./fsize_benchmark <sample_file> <file_size> <n_blocks> <out_file>\n";
        return -1;
    }

    const char *sample_file = argv[1];
    std::size_t file_size = std::atol(argv[2]);
    std::size_t n_blocks = std::atol(argv[3]);
    const char *out_file = argv[4];

    compio_config config;
    compio_build_default_config(&config);

    std::ifstream ifs(sample_file);
    ifs.seekg(0, std::ios_base::end);
    std::size_t sample_size = ifs.tellg();

    if (sample_size < config.block_size * 4) {
        throw std::runtime_error("sample file is too small");
    }

    std::vector<char> sample_data(sample_size);
    ifs.seekg(0);
    ifs.read(sample_data.data(), sample_size);
    ifs.close();

    std::minstd_rand0 rng(0);
    std::uniform_int_distribution<std::size_t> d_start_prep(0, sample_size - config.block_size);
    std::uniform_int_distribution<std::size_t> d_size(config.block_size / 4, config.block_size * 4);

    char fn[L_tmpnam];
    tmpnam(fn);

    std::vector<std::vector<std::size_t>> columns(1, std::vector<std::size_t>());
    columns[0].reserve(n_blocks);

    {
        config.cache_size__nodes = 0;
        config.cache_size__blocks = 0;

        compio_archive *archive = compio_open_archive(fn, "w+", &config);
        compio_file *file = compio_open_file("A", archive);

        for (std::size_t i = 0; i < file_size; i += config.block_size) {
            compio_write(sample_data.data() + d_start_prep(rng), config.block_size, file);
        }

        columns[0].push_back(get_fsize(archive->file));

        for (std::size_t i = 0; i < n_blocks; ++i) {
            std::size_t size = d_size(rng);
            std::uniform_int_distribution<std::size_t> d_start(0, sample_size - size);
            std::size_t start = d_start(rng);
            compio_seek(file, d_start(rng), COMP_SEEK_SET);

            auto bytes_written = compio_write(sample_data.data() + start, size, file);
            if (bytes_written != size) {
                throw std::runtime_error("compio_write returned " + std::to_string(bytes_written) +
                                         " != " + std::to_string(size));
            }

            columns[0].push_back(get_fsize(archive->file));
        }

        compio_close_file(file);
        compio_close_archive(archive);
    }

    remove(fn);

    save_csv(columns, {"file size"}, out_file);

    return 0;
}