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
    static const char* deleted_ptr();

    struct Entry {
        std::string_view key;
        uint32_t value;
        uint64_t hash;
        
        bool is_empty() const { return key.data() == nullptr; }
        bool is_deleted() const { return key.data() == deleted_ptr(); }
        bool is_occupied() const { return !is_empty() && !is_deleted(); }
    };
    
    flat_map() : size_(0), deleted_(0), capacity_(0) {}
    
    /// @brief Pre-allocate slots. Power of 2 required.
    void reserve(size_t n);
    
    /// @brief Clear the map.
    void clear();
    
    /// @brief Find value by key. Returns pointer to value or nullptr.
    uint32_t* find(std::string_view key);
    
    const uint32_t* find(std::string_view key) const;
    
    /// @brief Insert or assign value to key.
    void insert_or_assign(std::string_view key, uint32_t value);
    
    /// @brief Emplace helper (behaves like insert_or_assign for unique keys)
    void emplace(std::string_view key, uint32_t value);
    
    /// @brief Erase key.
    bool erase(std::string_view key);

    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }

private:
    void rehash(size_t new_cap);
    
    void insert_internal(const Entry& entry);

    std::vector<Entry> table_;
    size_t size_;
    size_t deleted_;
    size_t capacity_;
};

} // namespace detail
} // namespace compio

#endif // COMPIO_DETAIL_FLAT_MAP_HPP_
