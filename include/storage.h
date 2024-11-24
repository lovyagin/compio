#include <stdint.h>
#include <stdio.h>
#include "block.h"
#include <stdbool.h>

//
// Управление хранилищем, внешней фрагментацией
//

#ifndef COMPIO_STORAGE_H
#define COMPIO_STORAGE_H

#endif //COMPIO_STORAGE_H

enum CompressionType {
    NONE,
    GZIP,
    LZ4,
};

typedef struct {
    bool is_compressed;
    int8_t compression_level;
    int32_t original_size;
    int32_t compressed_size;
    CompressionType type;
} CompressionInfo;

typedef struct {
    char *filename;
    int32_t magic_num;
} MetaData;

typedef struct {
    MetaData *md;
    CompressionInfo *compression_info;
    compio_block *blocks;
} CFile;

