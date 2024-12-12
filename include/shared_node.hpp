#ifndef SHARED_NODE_HEADER_
#define SHARED_NODE_HEADER_

#include "file.hpp"

namespace compio {

#define RO(x) (const_cast<const shared_node&>(x))

class shared_node {
public:
    shared_node(FILE* file, uint64_t addr, index_node* obj)
        : storage(new _storage(file, addr, obj)) {}

    shared_node(FILE* file, uint64_t addr, int degree)
        : storage(new _storage(file, addr, new index_node(file, addr, degree))) {}

    shared_node(const shared_node& other) { 
        storage = other.storage; 
        ++storage->ref_count;
    }

    shared_node& operator=(shared_node other) {
        std::swap(storage, other.storage);
        ++storage->ref_count;
        return *this;
    }

    ~shared_node() {
        --storage->ref_count;
        if (storage->ref_count == 0)
            delete storage;
    }

    uint64_t addr() const { return storage->addr; }

    index_node* ptr() const {
        storage->modified = true;
        return storage->data;
    }

    const index_node* operator->() const { return storage->data; }

    index_node* operator->() {
        storage->modified = true;
        return storage->data;
    }

    const index_node& operator*() const { return *storage->data; }

    index_node& operator*() {
        storage->modified = true;
        return *storage->data;
    }

    void remove() { storage->removed = true; }

    void modify() { storage->modified = true; }

private:
    struct _storage {
        int ref_count;
        bool modified;
        bool removed;
        index_node* data;
        FILE* file;
        uint64_t addr;

        _storage(FILE* file, uint64_t addr, index_node* obj)
            : file(file),
              addr(addr),
              ref_count(1),
              modified(false),
              removed(false),
              data(obj) {}

        ~_storage() {
            if (modified && !removed)
                data->write(file, addr);
            delete data;
        }
    } * storage;
};

}

#endif // SHARED_NODE_HEADER_