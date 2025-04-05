#include <errno.h>
#include <string.h>
#include <stdint.h>

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

void compio_build_dummy_compressor(compio_compressor* result) {
    result->compress = dummy_compress;
    result->decompress = dummy_decompress;
}
