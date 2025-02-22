#ifndef ZLIB_ADAPTER_H
#define ZLIB_ADAPTER_H

#include <cstring>

#include "ICompressionAdapter.hpp"

class ZLibAdapter : public ICompressionAlgoAdapter {
public:
    ZLibAdapter() = default;

    virtual void change_data(uint64_t hash, uint64_t startPos, uint64_t size, void* data) override;
private:
    std::unique_ptr<compio::btree> _btreeP;
};

#endif //ZLIB_ADAPTER_H
