#include <cstdio>
#include <vector>
#include <string>
#include <chrono>
#include <iostream>
#include <filesystem>
#include "compio.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    const int NUM_FILES = 1000;
    const int NUM_READS = 10000;
    const int DATA_SIZE = 64;
    const std::string filename = "io_overhead_bench.compio";

    if (fs::exists(filename)) fs::remove(filename);
    if (fs::exists(filename + ".wal")) fs::remove(filename + ".wal");

    compio_config config;
    compio_build_default_config(&config);
    config.wal_sync_mode = COMPIO_WAL_SYNC_NORMAL; 

    compio_archive* archive = compio_open_archive(filename.c_str(), "w+", &config);
    if (!archive) {
        std::cerr << "Failed to open archive" << std::endl;
        return 1;
    }

    std::vector<uint8_t> data(DATA_SIZE, 'A');
    std::vector<std::string> filenames;

    auto start_write = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_FILES; ++i) {
        std::string name = "f" + std::to_string(i);
        filenames.push_back(name);
        compio_file* f = compio_open_file(name.c_str(), archive);
        if (f) {
            compio_write(data.data(), DATA_SIZE, f);
            compio_close_file(f);
        }
    }
    auto end_write = std::chrono::high_resolution_clock::now();
    
    compio_flush(archive);

    std::vector<uint8_t> read_buf(DATA_SIZE);
    auto start_read = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_READS; ++i) {
        // Read files sequentially to hit cache or potentially disk if cache is small (but 1000 files fit in memory easily)
        const std::string& name = filenames[i % NUM_FILES];
        compio_file* f = compio_open_file(name.c_str(), archive);
        if (f) {
            compio_read(read_buf.data(), DATA_SIZE, f);
            compio_close_file(f);
        }
    }
    auto end_read = std::chrono::high_resolution_clock::now();

    compio_close_archive(archive);

    double write_duration = std::chrono::duration<double>(end_write - start_write).count();
    double read_duration = std::chrono::duration<double>(end_read - start_read).count();

    std::cout << "Write " << NUM_FILES << " files (" << DATA_SIZE << " B): " << write_duration << " s (" 
              << NUM_FILES / write_duration << " ops/s)" << std::endl;
    std::cout << "Read " << NUM_READS << " files (" << DATA_SIZE << " B): " << read_duration << " s (" 
              << NUM_READS / read_duration << " ops/s)" << std::endl;

    fs::remove(filename);
    fs::remove(filename + ".wal");
    return 0;
}
