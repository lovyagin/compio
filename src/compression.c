//
// Реализация абстракции для выбора алгоритмов сжатия
//

#include <string.h>
#include <errno.h>

#include "compression.h"


int dummy_compress(void* dst, size_t* dst_size, const void* src, size_t src_size) {
    if (*dst_size < src_size) {
        errno = ENOBUFS;
        return -1;
    }
    memcpy(dst, src, src_size);
    *dst_size = src_size;
    return 0;
}


int dummy_decompress(void* dst, size_t* dst_size, const void* src, size_t src_size) {
    // same as compress: just copy contents
    return dummy_compress(dst, dst_size, src, src_size);
}


void build_dummy_compressor(compio_compressor* result) {
    result->compress = dummy_compress;
    result->decompress = dummy_decompress;
}
