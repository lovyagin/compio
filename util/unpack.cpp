#include <stdexcept>
#include <algorithm>
#include <memory>
#include <cstdio>
#include <string>
#include <filesystem>

#include "compio/compio_file.hpp"
#include "compio.h"

namespace fs = std::filesystem;

int main(int argc, char **argv) {
    if (argc != 3) {
        throw std::runtime_error("usage: ./compio_unpack <file> <output_prefix>");
    }

    const std::string out_prefix = argv[2];

    // Ensure parent directory exists if prefix contains a path
    fs::path prefix_path(out_prefix);
    if (prefix_path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(prefix_path.parent_path(), ec);
        if (ec) {
             throw std::runtime_error("failed to create directory " + prefix_path.parent_path().string());
        }
    }

    compio_config config;
    compio_build_default_config(&config);

    compio_archive *archive = compio_open_archive(argv[1], "r", &config);
    if (!archive) {
        throw std::runtime_error("failed to open archive");
    }

    const auto ftable = &archive->header->ftable;
    if (ftable->n_files == 0) {
        throw std::runtime_error("archive has no files");
    }

    for (std::size_t i = 0; i < ftable->n_files; ++i) {
        const auto &f = ftable->files[i];
        printf("reading file %s\n", f.name);

        compio_file *file = compio_open_file(f.name, archive);
        if (!file) {
            throw std::runtime_error("failed to open file " + std::string(f.name));
        }

        printf("size = %lu\n", f.size);

        std::string out_fp = out_prefix + f.name;
        FILE *out_file = fopen(out_fp.c_str(), "wb");
        if (!out_file) {
            compio_close_file(file);
            throw std::runtime_error("failed to open output file " + out_fp);
        }

        // Use 1MB buffer to avoid huge allocations
        constexpr size_t buffer_size = 1024 * 1024;
        auto buffer = std::make_unique<uint8_t[]>(buffer_size);
        
        uint64_t remaining = f.size;
        while (remaining > 0) {
            size_t to_read = std::min(static_cast<uint64_t>(buffer_size), remaining);
            size_t read_bytes = compio_read(buffer.get(), to_read, file);
            
            if (read_bytes != to_read) {
                fclose(out_file);
                compio_close_file(file);
                throw std::runtime_error("failed to read file " + std::string(f.name));
            }
            
            if (fwrite(buffer.get(), 1, read_bytes, out_file) != read_bytes) {
                fclose(out_file);
                compio_close_file(file);
                throw std::runtime_error("failed to write data to file " + out_fp);
            }
            remaining -= read_bytes;
        }

        fclose(out_file);

        compio_close_file(file);
    }

    compio_close_archive(archive);

    return 0;
}