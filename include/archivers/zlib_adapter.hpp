#ifndef ZLIB_ADAPTER_H
#define ZLIB_ADAPTER_H

#include "ICompressionAdapter.hpp"

class ZLibAdapter : public ICompressionAlgoAdapter {
public:
    ZLibAdapter() = default;

    void change_data(data_to_change& d, uint64_t size, void* data) override {}
};

#endif //ZLIB_ADAPTER_H
