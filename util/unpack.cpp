#include "compio.h"

#include "compio_file.hpp"

#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 3) {
        throw std::runtime_error("usage: ./unpack <file> <output_prefix>");
    }

    const std::string out_prefix = argv[2];

    compio_config config;
    compio_build_default_config(&config);

    compio_archive* archive = compio_open_archive(argv[1], "r", &config);
    if (!archive) {
        throw std::runtime_error("failed to open archive");
    }

    const auto ftable = &archive->header->ftable;
    if (ftable->n_files == 0) {
        throw std::runtime_error("archive has no files");
    }

    for (std::size_t i = 0; i < ftable->n_files; ++i) {
        const auto& f = ftable->files[i];
        printf("reading file %s\n", f.name);

        compio_file* file = compio_open_file(f.name, archive);
        if (!file) {
            throw std::runtime_error("failed to open file " + std::string(f.name));
        }

        printf("size = %d\n", f.size);

        auto buffer = std::make_unique<uint8_t[]>(f.size);
        std::size_t read_bytes = compio_read(buffer.get(), f.size, file);
        if (read_bytes != f.size) {
            throw std::runtime_error("failed to read file " + std::string(f.name));
        }

        std::string out_fp = out_prefix + f.name;
        FILE* out_file = fopen(out_fp.c_str(), "w+");

        std::size_t written_bytes = fwrite(buffer.get(), 1, f.size, out_file);
        if (written_bytes != f.size) {
            throw std::runtime_error("failed to write data to file " + out_fp);
        }

        fclose(out_file);

        compio_close_file(file);
    }

    compio_close_archive(archive);

    return 0;
}