#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>

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