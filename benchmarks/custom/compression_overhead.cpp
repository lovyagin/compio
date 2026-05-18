#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "compio/btree.hpp"
#include "compio/compio_file.hpp"
#include "compio.h"

static std::string unique_path() {
    return "/dev/shm/compio_" + std::to_string(getpid()) + "_" + std::to_string(time(nullptr)) +
           ".tmp";
}

uint64_t compress_whole(const compio_compressor *comp, const void *src, uint64_t src_size) {
    uint64_t buf_size = comp->get_bufsize(comp, src_size);
    std::vector<char> dst(buf_size);
    uint64_t dst_size = buf_size;
    if (comp->compress(comp, dst.data(), &dst_size, src, src_size) != 0)
        return 0;
    return dst_size;
}

uint64_t compress_blocks(const compio_compressor *comp, const void *data, uint64_t data_size,
                         uint64_t block_size) {
    uint64_t total = 0;
    const char *ptr = static_cast<const char *>(data);
    for (uint64_t offset = 0; offset < data_size; offset += block_size) {
        uint64_t chunk = std::min(block_size, data_size - offset);
        uint64_t buf_size = comp->get_bufsize(comp, chunk);
        std::vector<char> dst(buf_size);
        uint64_t dst_size = buf_size;
        if (comp->compress(comp, dst.data(), &dst_size, ptr + offset, chunk) != 0)
            return 0;
        total += dst_size;
    }
    return total;
}

uint64_t create_archive_size(const void *data, uint64_t data_size, uint64_t block_size,
                             const compio_compressor *comp) {
    compio_config cfg;
    compio_build_default_config(&cfg);
    cfg.compressor = *comp;
    cfg.block_size = block_size / 4;
    cfg.block_size__minimum = block_size / 4 / 4;
    if (cfg.block_size__minimum < 1)
        cfg.block_size__minimum = 1;
    cfg.block_size__maximum = block_size;

    std::string archive_path = unique_path();
    compio_archive *arch = compio_open_archive(archive_path.c_str(), "w", &cfg);
    if (!arch)
        return 0;
    compio_file *file = compio_open_file("data", arch);
    if (!file) {
        compio_close_archive(arch);
        return 0;
    }
    if (compio_write(data, data_size, file) != data_size) {
        compio_close_file(file);
        compio_close_archive(arch);
        return 0;
    }
    compio_close_file(file);
    compio_close_archive(arch);

    struct stat st;
    if (stat(archive_path.c_str(), &st) != 0)
        return 0;
    uint64_t size = st.st_size;
    std::remove(archive_path.c_str());
    return size;
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0] << " <block_size> <input_file> <compressor> <level>\n";
        return 1;
    }
    uint64_t block_size = std::stoull(argv[1]);
    const char *input_path = argv[2];
    std::string comp_str = argv[3];
    int level = std::stoi(argv[4]);

    std::ifstream input(input_path, std::ios::binary | std::ios::ate);
    if (!input) {
        std::perror("open input file");
        return 1;
    }
    std::streamsize size = input.tellg();
    input.seekg(0, std::ios::beg);
    std::vector<char> data(size);
    if (!input.read(data.data(), size)) {
        std::perror("read input file");
        return 1;
    }
    input.close();

    compio_compressor comp;
    if (comp_str == "dummy")
        compio_build_dummy_compressor(&comp);
    else if (comp_str == "zlib")
        compio_build_zlib_compressor_with_level(&comp, level);
    else if (comp_str == "lz4")
        compio_build_lz4_compressor_with_level(&comp, level);
    else if (comp_str == "zstd")
        compio_build_zstd_compressor_with_level(&comp, level);
    else if (comp_str == "brotli")
        compio_build_brotli_compressor_with_level(&comp, level);
    else {
        std::cerr << "Unknown compressor: " << comp_str << "\n";
        return 1;
    }

    uint64_t whole = compress_whole(&comp, data.data(), data.size());
    if (whole == 0)
        return 1;
    uint64_t blocked = compress_blocks(&comp, data.data(), data.size(), block_size);
    if (blocked == 0)
        return 1;
    uint64_t archive = create_archive_size(data.data(), data.size(), block_size, &comp);
    if (archive == 0)
        return 1;

    std::cout << whole << " " << blocked << " " << archive << "\n";
    return 0;
}