#ifndef ICOMPRESSION_ADAPTER_H
#define ICOMPRESSION_ADAPTER_H

#include <cstdint>

#include "file.hpp"
<<<<<<< HEAD
#include "BTree/btree.hpp"
#include "tests/unit/include/mockBTree/MockBTree.hpp"
=======
#include "btree.hpp"
>>>>>>> 9950425 (dummy data change method)

class ICompressionAlgoAdapter {
public:
    ICompressionAlgoAdapter() = default;
    virtual ~ICompressionAlgoAdapter() = default;

<<<<<<< HEAD
    virtual void change_data(uint64_t hash, uint64_t startPos,
                             uint64_t size, void* data) = 0;
private:
    std::unique_ptr<compio::btree> _btreeP;
    virtual void change_one_block_data(std::pair<compio::tree_key,
                                       compio::tree_val>& node,
                                       uint64_t startPos,
                                       uint64_t size,
                                       void* data) = 0;
=======
    // using data_to_change = compio::tree_key;
    virtual void change_data(uint64_t hash, uint64_t startPos, uint64_t size, void* data) = 0;
private:
    std::unique_ptr<compio::btree> _btreeP;
>>>>>>> 9950425 (dummy data change method)
};

#endif //ICOMPRESSION_ADAPTER_H
