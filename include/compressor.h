#ifndef COMPRESSOR_H
#define COMPRESSOR_H

<<<<<<< HEAD
#ifdef __cplusplus
extern "C" {
#endif


=======
>>>>>>> 294f7c6 (rebased from develop, solve conflicts)
#include <errno.h>
#include <string.h>
#include <stdint.h>

/**
 * @brief Compressor interface
 */
typedef struct compio_compressor {
    /**
     * @brief Compress src_size of bytes from src buffer into dst buffer.
     * On success, return 0 and write real size of compressed data into
     * dst_size. If dst buffer is to small, return non-zero code and set errno =
     * ENOBUFS.
     */
    int (*compress)(void* dst, uint64_t* dst_size, const void* src, uint64_t src_size);

    /**
     * @brief Decompress src_size of bytes, that was previously
     * compressed with the same compressor, from src buffer into dst buffer. On
     * success, return 0 and write real size of decompressed data into dst_size.
     * If dst buffer is to small, return non-zero code and set errno = ENOBUFS.
     */
    int (*decompress)(void* dst, uint64_t* dst_size, const void* src, uint64_t src_size);
} compio_compressor;

int dummy_compress(void* dst, size_t* dst_size, const void* src, size_t src_size);

int dummy_decompress(void* dst, size_t* dst_size, const void* src, size_t src_size);

/**
 * @brief Test compressor, keeps data exactly the same
 *
 * @param result
 */
void compio_build_dummy_compressor(compio_compressor* result);

#ifdef __cplusplus
}
#endif

#endif //COMPRESSOR_H
