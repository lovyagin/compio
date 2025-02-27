#ifndef STORAGE_BLOCK_HPP
#define STORAGE_BLOCK_HPP

#include <stdint.h>
#include <zconf.h>

class StorageBlock {
public:
    StorageBlock(uint64_t compressedSize, void* compressedData = nullptr)
            : compressedDataSize(compressedSize),
              compressedData(static_cast<Bytef*>(compressedData)) {};

    Bytef *compressedData;
    uint64_t compressedDataSize;
};

#endif //STORAGE_BLOCK_HPP
