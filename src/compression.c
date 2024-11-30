//
// Реализация абстракции для выбора алгоритмов сжатия
//

#include <string.h>

#include "compression.h"


int dummy_compress(void* dst, const void* src, size_t size) {
    memcpy(dst, src, size);
}


int dummy_decompress(void* dst, const void* src, size_t size) {
    memcpy(dst, src, size);
}


void build_dummy_compressor(compio_compressor* result) {
    result->compress = dummy_compress;
    result->decompress = dummy_decompress;
}
