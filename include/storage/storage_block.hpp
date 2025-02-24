#ifndef STORAGE_BLOCK_HPP
#define STORAGE_BLOCK_HPP

#include <stdint.h>
#include <zconf.h>

class StorageBlock {
public:
    StorageBlock(const void* compressedData = nullptr, uint64_t compressedSize)
            : compressedData(static_cast<const Bytef*>(compressedData)),
              compressedDataSize(compressedSize) {};

    const Bytef *compressedData;
    uint64_t compressedDataSize;
};

#endif //STORAGE_BLOCK_HPP
