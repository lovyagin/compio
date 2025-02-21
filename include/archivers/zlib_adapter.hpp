#ifndef ZLIB_ADAPTER_H
#define ZLIB_ADAPTER_H

#include "ICompressionAdapter.hpp"

class ZLibAdapter : public ICompressionAlgoAdapter {
public:
    ZLibAdapter() = default;
};

#endif //ZLIB_ADAPTER_H
