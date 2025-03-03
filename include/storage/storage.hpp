#ifndef STORAGE_H
#define STORAGE_H
#include "storage_block.hpp"

class Storage {
public:
    Storage() = default;

private:
    StorageBlock* block = nullptr;
};
#endif //STORAGE_H
