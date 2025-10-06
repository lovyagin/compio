#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include <lz4.h>
#include <zstd.h>
#include <brotli/encode.h>
#include <brotli/decode.h>

#include "compio.h"

int dummy_compress(void* dst, uint64_t* dst_size, const void* src, uint64_t src_size) {
    if (*dst_size < src_size) {
        errno = ENOBUFS;
        return -1;
    }
    memcpy(dst, src, src_size);
    *dst_size = src_size;
    return 0;
}

int dummy_decompress(void* dst, uint64_t* dst_size, const void* src, uint64_t src_size) {
    return dummy_compress(dst, dst_size, src, src_size);
}

uint64_t dummy_get_bufsize(uint64_t src_size) {
    return src_size;
}

void compio_build_dummy_compressor(compio_compressor* result) {
    result->compress = dummy_compress;
    result->decompress = dummy_decompress;
    result->get_bufsize = dummy_get_bufsize;
}

int zlib_compress(void* dst, uint64_t* dst_size, const void* src, uint64_t src_size) {
    uLongf compressed_size = (uLongf)*dst_size;
    
    int ret = compress2((Bytef*)dst, &compressed_size, 
                        (const Bytef*)src, (uLong)src_size, 
                        Z_DEFAULT_COMPRESSION);
    
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

int zlib_decompress(void* dst, uint64_t* dst_size, const void* src, uint64_t src_size) {
    uLongf decompressed_size = (uLongf)*dst_size;
    
    int ret = uncompress((Bytef*)dst, &decompressed_size, 
                        (const Bytef*)src, (uLong)src_size);
    
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

uint64_t zlib_get_bufsize(uint64_t src_size) {
    /*
    source: https://refspecs.linuxbase.org/LSB_3.0.0/LSB-Core-generic/LSB-Core-generic/zlib-compress2-1.html#:~:text=(sourceLen%20%EF%BF%BD%201.001)%20%2B%2012
    */
    return src_size + src_size / 1000 + 12;
}

void compio_build_zlib_compressor(compio_compressor* result) {
    result->compress = zlib_compress;
    result->decompress = zlib_decompress;
    result->get_bufsize = zlib_get_bufsize;
}

// LZ4 compressor implementation
int lz4_compress(void* dst, uint64_t* dst_size, const void* src, uint64_t src_size) {
    int compressed_size = LZ4_compress_default(
        (const char*)src,
        (char*)dst,
        (int)src_size,
        (int)*dst_size
    );

    if (compressed_size <= 0) {
        errno = ENOBUFS;
        return -1;
    }

    *dst_size = compressed_size;
    return 0;
}

int lz4_decompress(void* dst, uint64_t* dst_size, const void* src, uint64_t src_size) {
    int decompressed_size = LZ4_decompress_safe(
        (const char*)src,
        (char*)dst,
        (int)src_size,
        (int)*dst_size
    );

    if (decompressed_size < 0) {
        errno = EIO;
        return -1;
    }

    *dst_size = decompressed_size;
    return 0;
}

uint64_t lz4_get_bufsize(uint64_t src_size) {
    return LZ4_compressBound((int)src_size);
}

void compio_build_lz4_compressor(compio_compressor* result) {
    result->compress = lz4_compress;
    result->decompress = lz4_decompress;
    result->get_bufsize = lz4_get_bufsize;
}

// Zstandard compressor implementation
int zstd_compress(void* dst, uint64_t* dst_size, const void* src, uint64_t src_size) {
    size_t compressed_size = ZSTD_compress(
        dst,
        *dst_size,
        src,
        src_size,
        ZSTD_CLEVEL_DEFAULT
    );

    if (ZSTD_isError(compressed_size)) {
        errno = ENOBUFS;
        return -1;
    }

    *dst_size = compressed_size;
    return 0;
}

int zstd_decompress(void* dst, uint64_t* dst_size, const void* src, uint64_t src_size) {
    size_t decompressed_size = ZSTD_decompress(dst, *dst_size, src, src_size);

    if (ZSTD_isError(decompressed_size)) {
        errno = EIO;
        return -1;
    }

    *dst_size = decompressed_size;
    return 0;
}

uint64_t zstd_get_bufsize(uint64_t src_size) {
    return ZSTD_compressBound(src_size);
}

void compio_build_zstd_compressor(compio_compressor* result) {
    result->compress = zstd_compress;
    result->decompress = zstd_decompress;
    result->get_bufsize = zstd_get_bufsize;
}

// Brotli compressor implementation
int brotli_compress(void* dst, uint64_t* dst_size, const void* src, uint64_t src_size) {
    size_t encoded_size = *dst_size;

    int result = BrotliEncoderCompress(
        BROTLI_DEFAULT_QUALITY,
        BROTLI_DEFAULT_WINDOW,
        BROTLI_DEFAULT_MODE,
        src_size,
        (const uint8_t*)src,
        &encoded_size,
        (uint8_t*)dst
    );

    if (!result) {
        errno = ENOBUFS;
        return -1;
    }

    *dst_size = encoded_size;
    return 0;
}

int brotli_decompress(void* dst, uint64_t* dst_size, const void* src, uint64_t src_size) {
    size_t decoded_size = *dst_size;

    BrotliDecoderResult result = BrotliDecoderDecompress(
        src_size,
        (const uint8_t*)src,
        &decoded_size,
        (uint8_t*)dst
    );

    if (result != BROTLI_DECODER_RESULT_SUCCESS) {
        errno = EIO;
        return -1;
    }

    *dst_size = decoded_size;
    return 0;
}

uint64_t brotli_get_bufsize(uint64_t src_size) {
    return BrotliEncoderMaxCompressedSize(src_size);
}

void compio_build_brotli_compressor(compio_compressor* result) {
    result->compress = brotli_compress;
    result->decompress = brotli_decompress;
    result->get_bufsize = brotli_get_bufsize;
}
