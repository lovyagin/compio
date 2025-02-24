#ifndef STORAGE_BLOCK_HPP
#define STORAGE_BLOCK_HPP

#include <stdint.h>

class StorageBlock {
public:
    StorageBlock(void* compressedData = nullptr) : compressedData(compressedData) {};

    void* compressedData;
    uint64_t compressedDataSize;
};

#endif //STORAGE_BLOCK_HPP
