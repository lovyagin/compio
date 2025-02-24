#ifndef ZLIB_ADAPTER_H
#define ZLIB_ADAPTER_H

#include <cstring>
#include <zlib.h>
#include <cstdlib>
#include <iostream>

#include "ICompressionAdapter.hpp"


class ZLibAdapter : public ICompressionAlgoAdapter {
public:
    ZLibAdapter(std::unique_ptr<compio::btree> btreeP) : _btreeP(std::move(btreeP)), _mockBtreeP(nullptr) {}

    void change_data(uint64_t hash, uint64_t startPos, uint64_t size, void* data) override;
    void setMockBtree(std::unique_ptr<MockBTree> btreeP); // TODO: CREATE INTERFACE FOR TREE
private:
    std::unique_ptr<compio::btree> _btreeP;
    std::unique_ptr<MockBTree> _mockBtreeP; // TODO: CREATE INTERFACE FOR TREE
};

#endif //ZLIB_ADAPTER_H
