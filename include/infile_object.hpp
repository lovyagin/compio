#ifndef INFILE_OBJECT_HPP
#define INFILE_OBJECT_HPP

#include <cstdint>
#include <type_traits>

#define readonly(x, t) (const_cast<const smart_infile_object<t>&>(x))

uint64_t lendian_fwrite(const void* ptr, uint64_t size, uint64_t nmemb, FILE* stream);

uint64_t lendian_fread(void* ptr, uint64_t size, uint64_t nmemb, FILE* stream);

class infile_object {
public:
    virtual void read_from(FILE* file, uint64_t addr) = 0;
    virtual void write_to(FILE* file, uint64_t addr) const = 0;
    virtual ~infile_object() = default;
};

template <typename T> class smart_infile_object {
    static_assert(std::is_base_of<infile_object, T>::value, "T must be derived from infile_object");

private:
    struct storage {
        int ref_count;
        bool modified;
        bool removed;
        T* data;
        FILE* file;
        uint64_t addr;

        storage(FILE* file, uint64_t addr, T* data)
            : file(file),
              addr(addr),
              ref_count(1),
              modified(true),
              removed(false),
              data(data) {}

        storage(FILE* file, uint64_t addr) : storage(file, addr, new T()) {
            modified = false;
            data->read_from(file, addr);
        }

        ~storage() {
            if (modified && !removed) {
                data->write_to(file, addr);
            }
            delete data;
        }
    };

    storage* S;

public:
    smart_infile_object() : S(nullptr) {}

    smart_infile_object(FILE* file, uint64_t addr, T* data) : S(new storage(file, addr, data)) {}

    smart_infile_object(FILE* file, uint64_t addr) : S(new storage(file, addr)) {}

    smart_infile_object(const smart_infile_object& other) {
        S = other.S;
        ++S->ref_count;
    }

    smart_infile_object& operator=(smart_infile_object other) {
        std::swap(S, other.S);
        ++S->ref_count;
        return *this;
    }

    ~smart_infile_object() {
        --S->ref_count;
        if (S->ref_count == 0)
            delete S;
    }

    uint64_t addr() const { return S->addr; }

    T* ptr() const {
        S->modified = true;
        return S->data;
    }

    const T* operator->() const { return S->data; }

    T* operator->() {
        S->modified = true;
        return S->data;
    }

    const T& operator*() const { return *S->data; }

    T& operator*() {
        S->modified = true;
        return *S->data;
    }

    void remove() { S->removed = true; }

    void modify() { S->modified = true; }

    // for developing purposes
    void unmodify() const { S->modified = false; }
};

#endif // INFILE_OBJECT_HPP