// Minimal example: open a healthy archive and extract every file into a
// directory, streaming in fixed-size chunks. For the full-featured CLI (prefix
// mode, traversal guards, help) see util/unpack.cpp.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>

#include "compio.h"
#include "compio/compio_file.hpp"

namespace fs = std::filesystem;

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <archive_path> <output_dir>\n", argv[0]);
        return 1;
    }

    fs::create_directories(argv[2]);

    compio_config config;
    compio_build_default_config(&config);

    compio_archive *archive = compio_open_archive(argv[1], "r", &config);
    if (!archive) {
        fprintf(stderr, "failed to open archive '%s'\n", argv[1]);
        return 1;
    }

    const auto *ftable = &archive->header->ftable;
    constexpr size_t buffer_size = 64 * 1024;
    auto buffer = std::make_unique<uint8_t[]>(buffer_size);

    for (size_t i = 0; i < ftable->n_files; ++i) {
        const auto &f = ftable->files[i];
        compio_file *file = compio_open_file(f.name, archive);
        if (!file) {
            fprintf(stderr, "skip '%s': cannot open\n", f.name);
            continue;
        }

        fs::path out = fs::path(argv[2]) / f.name;
        FILE *out_file = fopen(out.string().c_str(), "wb");
        if (out_file) {
            uint64_t remaining = f.size;
            while (remaining > 0) {
                size_t n = std::min<uint64_t>(buffer_size, remaining);
                if (compio_read(buffer.get(), n, file) != n) break;
                fwrite(buffer.get(), 1, n, out_file);
                remaining -= n;
            }
            fclose(out_file);
            printf("extracted %s (%llu bytes)\n", f.name,
                   static_cast<unsigned long long>(f.size));
        }
        compio_close_file(file);
    }

    compio_close_archive(archive);
    return 0;
}
