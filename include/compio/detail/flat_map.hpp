#ifndef COMPIO_DETAIL_FLAT_MAP_HPP_
#define COMPIO_DETAIL_FLAT_MAP_HPP_

#include <vector>
#include <string_view>
#include <cstdint>
#include <functional>
#include <cstring>
#include <cassert>
#include <algorithm>

namespace compio {
namespace detail {

/**
 * @brief Simple flat map using Linear Probing with Tombstones.
 * Optimized for string_view keys and uint32_t values.
 * Stores full 64-bit hash to speed up lookups and resizing.
 * Max load factor ~0.75 (including tombstones).
 */
class flat_map {
public:
    static const char* deleted_ptr() {
        return reinterpret_cast<const char*>(1);
    }

    struct Entry {
        std::string_view key;
        uint32_t value;
        uint64_t hash;
        
        bool is_empty() const { return key.data() == nullptr; }
        bool is_deleted() const { return key.data() == deleted_ptr(); }
        bool is_occupied() const { return !is_empty() && !is_deleted(); }
    };
    
    flat_map() : size_(0), deleted_(0), capacity_(0) {}
    
    // Pre-allocate slots. Power of 2 required.
    void reserve(size_t n) {
        if (n == 0) return;
        size_t needed = static_cast<size_t>(n / 0.75) + 1;
        size_t cap = 16;
        while (cap < needed) cap *= 2;
        rehash(cap);
    }
    
    void clear() {
        if (capacity_ > 0) {
            std::fill(table_.begin(), table_.end(), Entry{});
        }
        size_ = 0;
        deleted_ = 0;
    }
    
    // Find value by key. Returns pointer to value or nullptr.
    uint32_t* find(std::string_view key) {
        if (capacity_ == 0) return nullptr;
        
        uint64_t h = std::hash<std::string_view>{}(key);
        size_t idx = h & (capacity_ - 1);
        
        for (size_t i = 0; i < capacity_; ++i) {
            Entry& e = table_[idx];
            
            if (e.is_empty()) return nullptr;
            
            if (e.is_occupied() && e.hash == h && e.key == key) {
                return &e.value;
            }
            
            idx = (idx + 1) & (capacity_ - 1);
        }
        return nullptr;
    }
    
    const uint32_t* find(std::string_view key) const {
        return const_cast<flat_map*>(this)->find(key);
    }
    
    // Insert or assign
    void insert_or_assign(std::string_view key, uint32_t value) {
        // Rehash if total load (size + deleted) exceeds 75%
        if (capacity_ == 0 || (size_ + deleted_ + 1) > capacity_ * 0.75) {
            rehash(capacity_ == 0 ? 16 : capacity_ * 2);
        }
        
        uint64_t h = std::hash<std::string_view>{}(key);
        size_t idx = h & (capacity_ - 1);
        size_t first_deleted = SIZE_MAX;
        
        for (size_t i = 0; i < capacity_; ++i) {
            Entry& e = table_[idx];
            
            if (e.is_empty()) {
                // Found empty slot. Use it or the first deleted slot found earlier.
                if (first_deleted != SIZE_MAX) {
                    idx = first_deleted;
                    deleted_--; // Converting deleted to occupied
                }
                table_[idx] = {key, value, h};
                size_++;
                return;
            }
            
            if (e.is_deleted()) {
                if (first_deleted == SIZE_MAX) first_deleted = idx;
            } else if (e.hash == h && e.key == key) {
                e.value = value;
                return; // Update existing
            }
            
            idx = (idx + 1) & (capacity_ - 1);
        }
    }
    
    // Emplace helper (behaves like insert_or_assign for unique keys)
    void emplace(std::string_view key, uint32_t value) {
        insert_or_assign(key, value);
    }
    
    // Erase key
    bool erase(std::string_view key) {
        if (capacity_ == 0) return false;
        
        uint64_t h = std::hash<std::string_view>{}(key);
        size_t idx = h & (capacity_ - 1);
        
        for (size_t i = 0; i < capacity_; ++i) {
            Entry& e = table_[idx];
            
            if (e.is_empty()) return false;
            
            if (e.is_occupied() && e.hash == h && e.key == key) {
                // Mark as deleted
                e.key = std::string_view(deleted_ptr(), 0); 
                size_--;
                deleted_++;
                return true;
            }
            
            idx = (idx + 1) & (capacity_ - 1);
        }
        return false;
    }

    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }

private:
    void rehash(size_t new_cap) {
        std::vector<Entry> old_table = std::move(table_);
        table_.resize(new_cap); // zero-initialized
        capacity_ = new_cap;
        size_ = 0;
        deleted_ = 0;
        
        for (const auto& e : old_table) {
            if (e.is_occupied()) {
                insert_internal(e);
            }
        }
    }
    
    void insert_internal(const Entry& entry) {
        size_t idx = entry.hash & (capacity_ - 1);
        while (true) {
            if (table_[idx].is_empty()) {
                table_[idx] = entry;
                size_++;
                return;
            }
            idx = (idx + 1) & (capacity_ - 1);
        }
    }

    std::vector<Entry> table_;
    size_t size_;
    size_t deleted_;
    size_t capacity_;
};

} // namespace detail
} // namespace compio

#endif // COMPIO_DETAIL_FLAT_MAP_HPP_
