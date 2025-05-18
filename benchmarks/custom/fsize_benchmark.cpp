#include "compio.h"
#include "compio_file.hpp"

#include "sample_data.hpp"

#include <iostream>
#include <fstream>
#include <random>
#include <string>

std::size_t get_fsize(FILE* file) {
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

void save_csv(const std::vector<std::vector<std::size_t>>& columns, const std::vector<std::string>& column_names, const char* fn) {
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

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: ./fsize_benchmark <out_file>\n";
        return -1;
    }

    const std::size_t block_size = 4096;
    const std::size_t n_blocks = 1 << 14;

    std::minstd_rand0 rng(0);
    std::uniform_int_distribution<std::size_t> d(0, sizeof(html_data) - block_size);

    char fn[L_tmpnam];
    tmpnam(fn);

    std::vector<std::vector<std::size_t>> columns(2, std::vector<std::size_t>());
    for (auto& column : columns) {
        column.reserve(n_blocks);
    }

    {
        compio_config config;
        compio_build_default_config(&config);
        config.cache_size = 0;
        config.block_cache_size = 0;

        compio_archive* archive = compio_open_archive(fn, "w+", &config);
        compio_file* file = compio_open_file("A", archive);

        for (std::size_t i = 0; i < n_blocks; ++i) {
            auto bytes_written = compio_write(html_data + d(rng), block_size, file);
            if (bytes_written != block_size) {
                throw std::runtime_error("compio_write returned " + std::to_string(bytes_written) + " != " + std::to_string(block_size));
            }
            
            columns[0].push_back(get_fsize(archive->file));
        }
        
        compio_close_file(file);
        compio_close_archive(archive);
    }

    {
        FILE* file = fopen(fn, "w+");

        for (std::size_t i = 0; i < n_blocks; ++i) {
            auto bytes_written = fwrite(html_data + d(rng), 1, block_size, file);
            if (bytes_written != block_size) {
                throw std::runtime_error("fwrite returned " + std::to_string(bytes_written) + " != " + std::to_string(block_size));
            }
                
            columns[1].push_back(get_fsize(file));
        }

        fclose(file);
    }

    remove(fn);

    save_csv(columns, {"compio", "stdio"}, argv[1]);
    
    return 0;
}