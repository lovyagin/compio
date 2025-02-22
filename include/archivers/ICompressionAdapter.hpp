#ifndef ICOMPRESSION_ADAPTER_H
#define ICOMPRESSION_ADAPTER_H

#include <cstdint>

#include "file.hpp"
#include "btree.hpp"

class ICompressionAlgoAdapter {
public:
    ICompressionAlgoAdapter() = default;
    virtual ~ICompressionAlgoAdapter() = default;

    // using data_to_change = compio::tree_key;
    virtual void change_data(uint64_t hash, uint64_t startPos, uint64_t size, void* data) = 0;
private:
    std::unique_ptr<compio::btree> _btreeP;
};

#endif //ICOMPRESSION_ADAPTER_H
