#ifndef ZLIB_ADAPTER_H
#define ZLIB_ADAPTER_H

#include <cstring>
#include <zlib.h>
#include <cstdlib>
#include <iostream>

#include "ICompressionAdapter.hpp"


class ZLibAdapter : public ICompressionAlgoAdapter {
public:
    explicit ZLibAdapter(std::shared_ptr<compio::IBTree> IbtreeP) : _IbtreeP(std::move(IbtreeP)) {} // TODO: CREATE INTERFACE FOR TREE

    void change_data(uint64_t hash, uint64_t startPos, uint64_t size, void* data) override;
private:
    // std::unique_ptr<compio::btree> _btreeP;
    std::shared_ptr<compio::IBTree> _IbtreeP; // TODO: CREATE INTERFACE FOR TREE
};

#endif //ZLIB_ADAPTER_H
