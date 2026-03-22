//
// Пример простой программы с использованием библиотеки
//

#include <stdio.h>
#include <inttypes.h>

#include "compio.h"

int main() {
    // --- Configuration

    compio_config config;
    compio_build_default_config(&config);

    // Set compressor (default: zlib)
    // compio_build_zlib_compressor(&config.compressor);

    // Set B-Tree degree parameter (default: 16)
    // config.b_tree_degree = 32;

    // Set block size (default: 4096)
    // config.block_size = 1024;

    // Set B-Tree nodes cache size (default: 128)
    // config.cache_size__nodes = 64
    // , or disable it
    // config.cache_size__nodes = 0

    // Set block cache size (default: 16)
    // config.cache_size__blocks = 128;
    // , or disable it
    // config.cache_size__blocks = 0;

    // Set allocation strategy (default: FIRST_FIT)
    // config.allocation_strategy = COMPIO_ALLOC_BEST_FIT

    // Set allocator fragmentation trhreshold in percentage (default: 30)
    // config.fragmentation_threshold = 20;

    // --- Opening archive and file

    const char *fp = "archive.compio";
    compio_archive *archive = compio_open_archive(fp, "w+", &config);
    if (archive == NULL) {
        fprintf(stderr, "failed to open archive \"%s\"\n", fp);
        return -1;
    }

    const char *name = "DATA";
    compio_file *file = compio_open_file(name, archive);

    if (archive == NULL) {
        fprintf(stderr, "failed to open file with name \"%s\"\n", name);
        compio_close_archive(archive);
        return -2;
    }

    // --- Writing and reading

    const char buffer[] = "Hello, World!";
    uint64_t bytes_written = compio_write(buffer, sizeof(buffer), file);
    if (bytes_written != sizeof(buffer)) {
        fprintf(stderr, "compio_write returned %" PRIu64 " != %zu\n", bytes_written, sizeof(buffer));
        compio_close_file(file);
        compio_close_archive(archive);
        return -3;
    }

    int ret = compio_seek(file, 0, COMPIO_SEEK_SET);
    if (ret != 0) {
        fprintf(stderr, "compio_seek returned %d\n", ret);
        compio_close_file(file);
        compio_close_archive(archive);
        return -4;
    }

    char out_buffer[sizeof(buffer)];
    uint64_t bytes_read = compio_read(out_buffer, sizeof(buffer), file);
    if (bytes_read != sizeof(buffer)) {
        fprintf(stderr, "compio_read returned %" PRIu64 " != %zu\n", bytes_read, sizeof(buffer));
        compio_close_file(file);
        compio_close_archive(archive);
        return -5;
    }

    uint64_t fsize = compio_tell(file);

    if (fsize != sizeof(buffer)) {
        fprintf(stderr, "file size is not equal to number of written bytes (%" PRIu64 " != %zu)\n", fsize,
                sizeof(buffer));
        compio_close_file(file);
        compio_close_archive(archive);
        return -6;
    }

    for (size_t i = 0; i < sizeof(buffer); ++i) {
        if (buffer[i] != out_buffer[i]) {
            fprintf(stderr, "out data != in data (position: %zu, %d != %d)\n", i, out_buffer[i],
                    buffer[i]);
            compio_close_file(file);
            compio_close_archive(archive);
            return -7;
        }
    }

    // --- Closing

    compio_close_file(file);
    compio_close_archive(archive);

    printf("successfully passed!\n");

    return 0;
}