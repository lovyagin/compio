//
// Абстракция для выбора алгоритмов сжатия
//

#ifndef COMPIO_COMPRESSION_H
#define COMPIO_COMPRESSION_H

#include <stdio.h>


/**
 * @brief Compressor interface
 * 
 * @param compress Compress data
 * @param decompress Deompress data
 */
typedef struct compio_compressor {
    int (*compress)(void* dst, const void* src, size_t size);
    int (*decompress)(void* dst, const void* src, size_t size);
} compio_compressor;


/**
 * @brief Dummy compressor, that does nothing
 * 
 * @param result Pointer to resulting compressor 
 */
void build_dummy_compressor(compio_compressor* result);


#endif //COMPIO_COMPRESSION_H
