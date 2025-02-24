#ifndef ZLIB_ADAPTER_H
#define ZLIB_ADAPTER_H

#include <cstring>
#include <zlib.h>
#include <cstdlib>
#include <iostream>

#include "ICompressionAdapter.hpp"


class ZLibAdapter : public ICompressionAlgoAdapter {
public:
    explicit ZLibAdapter(std::unique_ptr<compio::btree> btreeP) : _btreeP(std::move(btreeP)), _mockBtreeP(nullptr) {}
    explicit ZLibAdapter(std::unique_ptr<MockBTree> mockBtreeP) : _btreeP(nullptr), _mockBtreeP(std::move(mockBtreeP)) {} // TODO: CREATE INTERFACE FOR TREE

    void change_data(uint64_t hash, uint64_t startPos, uint64_t size, void* data) override;
private:
    std::unique_ptr<compio::btree> _btreeP;
    std::unique_ptr<MockBTree> _mockBtreeP; // TODO: CREATE INTERFACE FOR TREE
};

#endif //ZLIB_ADAPTER_H
