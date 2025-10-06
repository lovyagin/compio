/**
 * Test demonstrating compression type persistence across archive open/close cycles
 */

#include "compio.h"
#include <stdio.h>
#include <string.h>

int main() {
    const char* archive_path = "/tmp/test_compression_persistence.cmp";
    const char* test_data = "This is test data to verify compression persistence!";
    size_t data_size = strlen(test_data) + 1;

    printf("=== Compression Type Persistence Test ===\n\n");

    // Test different compressors
    const char* compressor_names[] = {"LZ4", "Zstandard", "Brotli"};
    void (*build_funcs[])(compio_compressor*) = {
        compio_build_lz4_compressor,
        compio_build_zstd_compressor,
        compio_build_brotli_compressor
    };

    for (int c = 0; c < 3; c++) {
        printf("Testing %s compressor:\n", compressor_names[c]);

        // Create archive with specific compressor
        compio_config config;
        compio_build_default_config(&config);
        build_funcs[c](&config.compressor);

        compio_archive* archive = compio_open_archive(archive_path, "w+", &config);
        if (!archive) {
            printf("  ERROR: Failed to create archive\n");
            continue;
        }

        compio_file* file = compio_open_file("test.txt", archive);
        if (!file) {
            printf("  ERROR: Failed to create file\n");
            compio_close_archive(archive);
            continue;
        }

        compio_write(test_data, data_size, file);
        compio_close_file(file);
        compio_close_archive(archive);
        printf("  ✓ Created archive with %s compression\n", compressor_names[c]);

        // Reopen archive with default config (should auto-detect compressor)
        compio_config default_config;
        compio_build_default_config(&default_config);

        archive = compio_open_archive(archive_path, "r+", &default_config);
        if (!archive) {
            printf("  ERROR: Failed to reopen archive\n");
            continue;
        }

        file = compio_open_file("test.txt", archive);
        if (!file) {
            printf("  ERROR: Failed to open file\n");
            compio_close_archive(archive);
            continue;
        }

        char read_buffer[256] = {0};
        size_t read_bytes = compio_read(read_buffer, data_size, file);

        if (read_bytes == data_size && strcmp(test_data, read_buffer) == 0) {
            printf("  ✓ Successfully read data after reopening\n");
            printf("  ✓ Compression type was correctly persisted!\n");
        } else {
            printf("  ERROR: Data mismatch after reopening\n");
        }

        compio_close_file(file);
        compio_close_archive(archive);
        printf("\n");
    }

    printf("=== Test Complete ===\n");
    return 0;
}

