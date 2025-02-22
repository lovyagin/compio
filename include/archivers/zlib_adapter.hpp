#ifndef ZLIB_ADAPTER_H
#define ZLIB_ADAPTER_H

#include <cstring>
#include <zlib.h>
#include <cstdlib>
#include <iostream>

#include "ICompressionAdapter.hpp"


class ZLibAdapter : public ICompressionAlgoAdapter {
public:
<<<<<<< HEAD
    explicit ZLibAdapter(std::shared_ptr<compio::IBTree> IbtreeP) : _IbtreeP(std::move(IbtreeP)) {} // TODO: CREATE INTERFACE FOR TREE

<<<<<<< HEAD
    void change_data(uint64_t hash, uint64_t startPos, uint64_t size, void* data) override;
private:
    std::shared_ptr<compio::IBTree> _IbtreeP; // TODO: CREATE INTERFACE FOR TREE
    void change_one_block_data(std::pair<compio::tree_key, compio::tree_val>& node, uint64_t startPos, uint64_t size, void* data) override;
=======
    virtual void change_data(uint64_t hash, uint64_t startPos, uint64_t size, void* data) override;
private:
    std::unique_ptr<compio::btree> _btreeP;
>>>>>>> 9950425 (dummy data change method)
<<<<<<< HEAD
=======
    ZLibAdapter() = default;

<<<<<<< HEAD
    void change_data(data_to_change& d, uint64_t size, void* data) override {}
>>>>>>> 7f69cff (rebased from develop, solve conflicts)
=======
    virtual void change_data(uint64_t hash, uint64_t startPos, uint64_t size, void* data) override;
private:
    std::unique_ptr<compio::btree> _btreeP;
>>>>>>> 46d7429 (dummy data change method)
=======
>>>>>>> b3d8f8a (dummy data change method)
};

#endif //ZLIB_ADAPTER_H
