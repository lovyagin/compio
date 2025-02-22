#ifndef ICOMPRESSION_ADAPTER_H
#define ICOMPRESSION_ADAPTER_H

#include <cstdint>

#include "../file.hpp"

class ICompressionAlgoAdapter {
public:
    ICompressionAlgoAdapter() = default;
    virtual ~ICompressionAlgoAdapter() = default;

    using data_to_change = compio::tree_key;
    virtual void change_data(data_to_change& d, uint64_t size, void* data) = 0;
};

#endif //ICOMPRESSION_ADAPTER_H
