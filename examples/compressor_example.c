/**
 * Example demonstrating different compression algorithms
 */

#include "compio.h"
#include <stdio.h>
#include <string.h>

int main() {
    const char* test_data = "Hello, World! This is a test of compression algorithms.";
    size_t data_size = strlen(test_data) + 1;

    printf("Original data: %s\n", test_data);
    printf("Original size: %zu bytes\n\n", data_size);

    // Test different compressors
    compio_compressor compressors[5];
    const char* names[] = {"Dummy", "ZLIB", "LZ4", "Zstandard", "Brotli"};

    compio_build_dummy_compressor(&compressors[0]);
    compio_build_zlib_compressor(&compressors[1]);
    compio_build_lz4_compressor(&compressors[2]);
    compio_build_zstd_compressor(&compressors[3]);
    compio_build_brotli_compressor(&compressors[4]);

    for (int i = 0; i < 5; i++) {
        uint64_t buf_size = compressors[i].get_bufsize(data_size);
        char compressed[buf_size];
        uint64_t compressed_size = buf_size;

        if (compressors[i].compress(compressed, &compressed_size, test_data, data_size) == 0) {
            printf("%s compressor:\n", names[i]);
            printf("  Compressed size: %lu bytes\n", compressed_size);
            printf("  Compression ratio: %.2f%%\n", (1.0 - (double)compressed_size / data_size) * 100);

            // Test decompression
            char decompressed[data_size];
            uint64_t decompressed_size = data_size;

            if (compressors[i].decompress(decompressed, &decompressed_size, compressed, compressed_size) == 0) {
                if (strcmp(test_data, decompressed) == 0) {
                    printf("  Decompression: OK\n");
                } else {
                    printf("  Decompression: FAILED (data mismatch)\n");
                }
            } else {
                printf("  Decompression: FAILED\n");
            }
        } else {
            printf("%s compressor: FAILED\n", names[i]);
        }
        printf("\n");
    }

    return 0;
}

