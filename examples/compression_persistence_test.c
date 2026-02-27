/**
 * Test demonstrating compression type persistence across archive open/close cycles
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "compio.h"

static void get_temp_path(char *buf, size_t buf_size, const char *filename) {
#ifdef _WIN32
    char tmp_dir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp_dir);
    snprintf(buf, buf_size, "%s%s", tmp_dir, filename);
#else
    snprintf(buf, buf_size, "/tmp/%s", filename);
#endif
}

int main() {
    char archive_path[512];
    get_temp_path(archive_path, sizeof(archive_path), "test_compression_persistence.cmp");
    const char *test_data = "This is test data to verify compression persistence!";
    size_t data_size = strlen(test_data) + 1;

    printf("=== Compression Type Persistence Test ===\n\n");

    // Test different compressors
    const char *compressor_names[] = {"LZ4", "Zstandard", "Brotli"};
    void (*build_funcs[])(compio_compressor *) = {
        compio_build_lz4_compressor, compio_build_zstd_compressor, compio_build_brotli_compressor};

    for (int c = 0; c < 3; c++) {
        printf("Testing %s compressor:\n", compressor_names[c]);

        // Create archive with specific compressor
        compio_config config;
        compio_build_default_config(&config);
        build_funcs[c](&config.compressor);

        compio_archive *archive = compio_open_archive(archive_path, "w+", &config);
        if (!archive) {
            printf("  ERROR: Failed to create archive\n");
            continue;
        }

        compio_file *file = compio_open_file("test.txt", archive);
        if (!file) {
            printf("  ERROR: Failed to create file\n");
            compio_close_archive(archive);
            continue;
        }

        compio_write(test_data, data_size, file);
        compio_close_file(file);
        compio_close_archive(archive);
        printf("  ✓ Created archive with %s compression\n", compressor_names[c]);

        // Get compression type from archive and configure matching compressor
        compio_compression_type stored_type;
        if (compio_get_compression_type(archive_path, &stored_type) != 0) {
            printf("  ERROR: Failed to get compression type from archive\n");
            continue;
        }

        compio_config reopen_config;
        compio_build_default_config(&reopen_config);
        compio_build_compressor_by_type(&reopen_config.compressor, stored_type);

        archive = compio_open_archive(archive_path, "r+", &reopen_config);
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
