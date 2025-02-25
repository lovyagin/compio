#ifndef ICOMPRESSION_ADAPTER_H
#define ICOMPRESSION_ADAPTER_H

#include <cstdint>

#include "file.hpp"
#include "BTree/btree.hpp"
#include "tests/unit/include/mockBTree/MockBTree.hpp"

class ICompressionAlgoAdapter {
public:
    ICompressionAlgoAdapter() = default;
    virtual ~ICompressionAlgoAdapter() = default;

    virtual void change_data(uint64_t hash, uint64_t startPos, uint64_t size, void* data) = 0;
private:
    std::unique_ptr<compio::btree> _btreeP;
    virtual void change_one_block_data(uint64_t hash, uint64_t startPos, uint64_t size, void* data) = 0;
};

#endif //ICOMPRESSION_ADAPTER_H
