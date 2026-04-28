#include <brotli/decode.h>
#include <brotli/encode.h>
#include <errno.h>
#include <lz4.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>
#include <zstd.h>

#include "compio.h"

int dummy_compress(const struct compio_compressor*, void *dst, uint64_t *dst_size, const void *src, uint64_t src_size) {
    if (*dst_size < src_size) {
        errno = ENOBUFS;
        return -1;
    }
    memcpy(dst, src, src_size);
    *dst_size = src_size;
    return 0;
}

int dummy_decompress(const struct compio_compressor* comp, void *dst, uint64_t *dst_size, const void *src, uint64_t src_size) {
    return dummy_compress(comp, dst, dst_size, src, src_size);
}

uint64_t dummy_get_bufsize(const struct compio_compressor*, uint64_t src_size) { return src_size; }

void compio_build_dummy_compressor(compio_compressor *result) {
    result->compress = dummy_compress;
    result->decompress = dummy_decompress;
    result->get_bufsize = dummy_get_bufsize;
    result->compression_type = COMPIO_COMPRESS_DUMMY;
}

int zlib_compress(const struct compio_compressor* comp, void *dst, uint64_t *dst_size, const void *src, uint64_t src_size) {
    uLongf compressed_size = (uLongf)*dst_size;

    int ret = compress2((Bytef *)dst, &compressed_size, (const Bytef *)src, (uLong)src_size,
                        comp->level);

    if (ret == Z_OK) {
        *dst_size = compressed_size;
        return 0;
    } else if (ret == Z_BUF_ERROR) {
        errno = ENOBUFS;
        return -1;
    } else {
        errno = EIO;
        return -1;
    }
}

int zlib_decompress(const struct compio_compressor*, void *dst, uint64_t *dst_size, const void *src, uint64_t src_size) {
    uLongf decompressed_size = (uLongf)*dst_size;

    int ret = uncompress((Bytef *)dst, &decompressed_size, (const Bytef *)src, (uLong)src_size);

    if (ret == Z_OK) {
        *dst_size = decompressed_size;
        return 0;
    } else if (ret == Z_BUF_ERROR) {
        errno = ENOBUFS;
        return -1;
    } else {
        errno = EIO;
        return -1;
    }
}

uint64_t zlib_get_bufsize(const struct compio_compressor*, uint64_t src_size) {
    /*
    source:
    https://refspecs.linuxbase.org/LSB_3.0.0/LSB-Core-generic/LSB-Core-generic/zlib-compress2-1.html#:~:text=(sourceLen%20%EF%BF%BD%201.001)%20%2B%2012
    */
    return src_size + src_size / 1000 + 12;
}

void compio_build_zlib_compressor_with_level(compio_compressor *result, int level) {
    result->compress = zlib_compress;
    result->decompress = zlib_decompress;
    result->get_bufsize = zlib_get_bufsize;
    result->compression_type = COMPIO_COMPRESS_ZLIB;
    result->level = level;
}

void compio_build_zlib_compressor(compio_compressor *result) {
    compio_build_zlib_compressor_with_level(result, Z_DEFAULT_COMPRESSION);
} 

/**
 * @brief Compress data using LZ4 algorithm
 *
 * @param dst Destination buffer for compressed data
 * @param dst_size Pointer to destination buffer size (input/output)
 * @param src Source data buffer
 * @param src_size Source data size in bytes
 * @return 0 on success, -1 on error (sets errno to ENOBUFS if buffer too small, EINVAL if size too
 * large)
 */
int lz4_compress(const struct compio_compressor* comp, void *dst, uint64_t *dst_size, const void *src, uint64_t src_size) {
    if (src_size > INT_MAX || *dst_size > INT_MAX) {
        errno = EINVAL;
        return -1;
    }

    int compressed_size = 
        LZ4_compress_fast((const char *)src, (char *)dst, (int)src_size, (int)*dst_size, comp->level);

    if (compressed_size <= 0) {
        errno = ENOBUFS;
        return -1;
    }

    *dst_size = compressed_size;
    return 0;
}

/**
 * @brief Decompress LZ4 compressed data
 *
 * @param dst Destination buffer for decompressed data
 * @param dst_size Pointer to destination buffer size (input/output)
 * @param src Compressed source data buffer
 * @param src_size Compressed data size in bytes
 * @return 0 on success, -1 on error (sets errno to EIO on decompression failure, EINVAL if size too
 * large)
 */
int lz4_decompress(const struct compio_compressor*, void *dst, uint64_t *dst_size, const void *src, uint64_t src_size) {
    if (src_size > INT_MAX || *dst_size > INT_MAX) {
        errno = EINVAL;
        return -1;
    }

    int decompressed_size =
        LZ4_decompress_safe((const char *)src, (char *)dst, (int)src_size, (int)*dst_size);

    if (decompressed_size < 0) {
        errno = EIO;
        return -1;
    }

    *dst_size = decompressed_size;
    return 0;
}

/**
 * @brief Get maximum buffer size needed for LZ4 compression
 *
 * @param src_size Size of data to be compressed
 * @return Maximum possible size of compressed data, or 0 if size too large
 */
uint64_t lz4_get_bufsize(const struct compio_compressor*, uint64_t src_size) {
    if (src_size > INT_MAX) {
        return 0;
    }
    return LZ4_compressBound((int)src_size);
}

void compio_build_lz4_compressor_with_level(compio_compressor *result, int level) {
    result->compress = lz4_compress;
    result->decompress = lz4_decompress;
    result->get_bufsize = lz4_get_bufsize;
    result->compression_type = COMPIO_COMPRESS_LZ4;
    result->level = level;
}

void compio_build_lz4_compressor(compio_compressor *result) {
    compio_build_lz4_compressor_with_level(result, 1);
}

/**
 * @brief Compress data using Zstandard algorithm
 *
 * @param dst Destination buffer for compressed data
 * @param dst_size Pointer to destination buffer size (input/output)
 * @param src Source data buffer
 * @param src_size Source data size in bytes
 * @return 0 on success, -1 on error (sets errno to ENOBUFS if buffer too small)
 */
int zstd_compress(const struct compio_compressor* comp, void *dst, uint64_t *dst_size, const void *src, uint64_t src_size) {
    size_t compressed_size = ZSTD_compress(dst, *dst_size, src, src_size, comp->level);

    if (ZSTD_isError(compressed_size)) {
        errno = ENOBUFS;
        return -1;
    }

    *dst_size = compressed_size;
    return 0;
}

/**
 * @brief Decompress Zstandard compressed data
 *
 * @param dst Destination buffer for decompressed data
 * @param dst_size Pointer to destination buffer size (input/output)
 * @param src Compressed source data buffer
 * @param src_size Compressed data size in bytes
 * @return 0 on success, -1 on error (sets errno to EIO on decompression failure)
 */
int zstd_decompress(const struct compio_compressor*, void *dst, uint64_t *dst_size, const void *src, uint64_t src_size) {
    size_t decompressed_size = ZSTD_decompress(dst, *dst_size, src, src_size);

    if (ZSTD_isError(decompressed_size)) {
        errno = EIO;
        return -1;
    }

    *dst_size = decompressed_size;
    return 0;
}

/**
 * @brief Get maximum buffer size needed for Zstandard compression
 *
 * @param src_size Size of data to be compressed
 * @return Maximum possible size of compressed data
 */
uint64_t zstd_get_bufsize(const struct compio_compressor*, uint64_t src_size) { return ZSTD_compressBound(src_size); }

void compio_build_zstd_compressor_with_level(compio_compressor *result, int level) {
    result->compress = zstd_compress;
    result->decompress = zstd_decompress;
    result->get_bufsize = zstd_get_bufsize;
    result->compression_type = COMPIO_COMPRESS_ZSTD;
    result->level = level;
}

void compio_build_zstd_compressor(compio_compressor *result) {
    compio_build_zstd_compressor_with_level(result, 5);
}

/**
 * @brief Compress data using Brotli algorithm
 *
 * @param dst Destination buffer for compressed data
 * @param dst_size Pointer to destination buffer size (input/output)
 * @param src Source data buffer
 * @param src_size Source data size in bytes
 * @return 0 on success, -1 on error (sets errno to ENOBUFS if buffer too small)
 */
int brotli_compress(const struct compio_compressor* comp, void *dst, uint64_t *dst_size, const void *src, uint64_t src_size) {
    size_t encoded_size = *dst_size;

    int result =
        BrotliEncoderCompress(comp->level, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
                              src_size, (const uint8_t *)src, &encoded_size, (uint8_t *)dst);

    if (!result) {
        errno = ENOBUFS;
        return -1;
    }

    *dst_size = encoded_size;
    return 0;
}

/**
 * @brief Decompress Brotli compressed data
 *
 * @param dst Destination buffer for decompressed data
 * @param dst_size Pointer to destination buffer size (input/output)
 * @param src Compressed source data buffer
 * @param src_size Compressed data size in bytes
 * @return 0 on success, -1 on error (sets errno to EIO on decompression failure)
 */
int brotli_decompress(const struct compio_compressor*, void *dst, uint64_t *dst_size, const void *src, uint64_t src_size) {
    size_t decoded_size = *dst_size;

    BrotliDecoderResult result =
        BrotliDecoderDecompress(src_size, (const uint8_t *)src, &decoded_size, (uint8_t *)dst);

    if (result != BROTLI_DECODER_RESULT_SUCCESS) {
        errno = EIO;
        return -1;
    }

    *dst_size = decoded_size;
    return 0;
}

/**
 * @brief Get maximum buffer size needed for Brotli compression
 *
 * @param src_size Size of data to be compressed
 * @return Maximum possible size of compressed data
 */
uint64_t brotli_get_bufsize(const struct compio_compressor*, uint64_t src_size) { return BrotliEncoderMaxCompressedSize(src_size); }

void compio_build_brotli_compressor_with_level(compio_compressor *result, int level) {
    result->compress = brotli_compress;
    result->decompress = brotli_decompress;
    result->get_bufsize = brotli_get_bufsize;
    result->compression_type = COMPIO_COMPRESS_BROTLI;
    result->level = level;
}

void compio_build_brotli_compressor(compio_compressor *result) {
    compio_build_brotli_compressor_with_level(result, 5);
}

/**
 * @brief Build compressor based on compression type
 *
 * @param result Pointer to compressor structure to initialize
 * @param type Compression type to use
 */
void compio_build_compressor_by_type(compio_compressor *result, compio_compression_type type) {
    switch (type) {
    case COMPIO_COMPRESS_DUMMY:
        compio_build_dummy_compressor(result);
        break;
    case COMPIO_COMPRESS_ZLIB:
        compio_build_zlib_compressor(result);
        break;
    case COMPIO_COMPRESS_LZ4:
        compio_build_lz4_compressor(result);
        break;
    case COMPIO_COMPRESS_ZSTD:
        compio_build_zstd_compressor(result);
        break;
    case COMPIO_COMPRESS_BROTLI:
        compio_build_brotli_compressor(result);
        break;
    case COMPIO_COMPRESS_CUSTOM:
        // For custom compressor, we cannot auto-initialize
        // User must provide their own implementation
        compio_build_dummy_compressor(result);
        result->compression_type = COMPIO_COMPRESS_CUSTOM;
        break;
    default:
        // Fallback to dummy
        compio_build_dummy_compressor(result);
        break;
    }
}
