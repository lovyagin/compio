#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include "compio.h"

static std::string unique_path() {
    return "/dev/shm/compio_" + std::to_string(getpid()) + "_" + std::to_string(time(nullptr)) + ".tmp";
}

int main(int argc, char* argv[]) {
    if (argc != 6) {
        std::cerr << "Usage: " << argv[0] << " <block_size> <compressor> <level> <sample_file> <N_bytes>\n";
        std::cerr << "  compressor: dummy, zlib, lz4, zstd, brotli\n";
        std::cerr << "  level: compression level (ignored for dummy, algorithm-specific)\n";
        return 1;
    }

    int block_size = std::stoi(argv[1]);
    std::string comp_str = argv[2];
    int level = std::stoi(argv[3]);
    std::string sample_path = argv[4];
    size_t N = std::stoull(argv[5]);

    // Map compression string to type and prepare compressor
    compio_compression_type comp_type;
    if (comp_str == "dummy")      comp_type = COMPIO_COMPRESS_DUMMY;
    else if (comp_str == "zlib")  comp_type = COMPIO_COMPRESS_ZLIB;
    else if (comp_str == "lz4")   comp_type = COMPIO_COMPRESS_LZ4;
    else if (comp_str == "zstd")  comp_type = COMPIO_COMPRESS_ZSTD;
    else if (comp_str == "brotli") comp_type = COMPIO_COMPRESS_BROTLI;
    else {
        std::cerr << "Unknown compression: " << comp_str << "\n";
        return 1;
    }

    // Read exactly N bytes from sample file
    std::ifstream sample(sample_path, std::ios::binary);
    if (!sample) {
        std::perror("open sample file");
        return 1;
    }
    std::vector<char> buffer(N);
    sample.read(buffer.data(), N);
    if (sample.gcount() != static_cast<std::streamsize>(N)) {
        std::cerr << "Read only " << sample.gcount() << " bytes, expected " << N << "\n";
        return 1;
    }
    sample.close();

    // Set up configuration
    compio_config cfg;
    compio_build_default_config(&cfg);

    // Build compressor with given level
    switch (comp_type) {
        case COMPIO_COMPRESS_DUMMY:
            compio_build_dummy_compressor(&cfg.compressor);
            break;
        case COMPIO_COMPRESS_ZLIB:
            compio_build_zlib_compressor_with_level(&cfg.compressor, level);
            break;
        case COMPIO_COMPRESS_LZ4:
            compio_build_lz4_compressor_with_level(&cfg.compressor, level);
            break;
        case COMPIO_COMPRESS_ZSTD:
            compio_build_zstd_compressor_with_level(&cfg.compressor, level);
            break;
        case COMPIO_COMPRESS_BROTLI:
            compio_build_brotli_compressor_with_level(&cfg.compressor, level);
            break;
        default:
            // Should not happen
            std::cerr << "Unsupported compression type\n";
            return 1;
    }

    cfg.block_size = block_size;
    cfg.block_size__minimum = block_size / 4;
    if (cfg.block_size__minimum < 1) cfg.block_size__minimum = 1;
    cfg.block_size__maximum = block_size * 4;

    // Create archive
    std::string archive_path = unique_path();
    compio_archive* arch = compio_open_archive(archive_path.c_str(), "w", &cfg);
    if (!arch) {
        std::perror("compio_open_archive");
        return 1;
    }

    // Open a file inside the archive
    compio_file* file = compio_open_file("data", arch);
    if (!file) {
        std::perror("compio_open_file");
        compio_close_archive(arch);
        return 1;
    }

    // Write the whole buffer
    uint64_t written = compio_write(buffer.data(), N, file);
    if (written != N) {
        std::cerr << "compio_write wrote " << written << " bytes, expected " << N << "\n";
        compio_close_file(file);
        compio_close_archive(arch);
        return 1;
    }

    // Clean up
    compio_close_file(file);
    compio_close_archive(arch);

    // Get archive file size
    struct stat st;
    if (stat(archive_path.c_str(), &st) != 0) {
        std::perror("stat");
        return 1;
    }

    // Output only the size
    std::cout << st.st_size << "\n";

    // Delete the archive file
    std::remove(archive_path.c_str());

    return 0;
}