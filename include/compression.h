//
// Абстракция для выбора алгоритмов сжатия
//

#ifndef COMPIO_COMPRESSION_H
#define COMPIO_COMPRESSION_H

#include <stdio.h>


/**
 * @brief Compressor interface
 * 
 * @param compress Compress src_size of bytes from src buffer into dst buffer. 
 * On success, return 0 and write real size of compressed data into dst_size. 
 * If dst buffer is to small, return non-zero code and set errno = ENOBUFS.
 * 
 * @param decompress Decompress src_size of bytes, that was previously compressed 
 * with the same compressor, from src buffer into dst buffer. 
 * On success, return 0 and write real size of decompressed data into dst_size. 
 * If dst buffer is to small, return non-zero code and set errno = ENOBUFS.
 */
typedef struct compio_compressor {
    int (*compress)(void* dst, size_t* dst_size, const void* src, size_t src_size);
    int (*decompress)(void* dst, size_t* dst_size, const void* src, size_t src_size);
} compio_compressor;


/**
 * @brief Builder for dummy compressor, that does nothing
 * 
 * @param result Pointer to resulting compressor 
 */
void build_dummy_compressor(compio_compressor* result);


#endif //COMPIO_COMPRESSION_H
