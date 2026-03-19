#include "compio/detail/flat_map.hpp"

namespace compio {
namespace detail {

const char* flat_map::deleted_ptr() {
    return reinterpret_cast<const char*>(1);
}

void flat_map::reserve(size_t n) {
    if (n == 0) return;
    size_t needed = static_cast<size_t>(n / 0.75) + 1;
    size_t cap = 16;
    while (cap < needed) cap *= 2;
    rehash(cap);
}

void flat_map::clear() {
    if (capacity_ > 0) {
        std::fill(table_.begin(), table_.end(), Entry{});
    }
    size_ = 0;
    deleted_ = 0;
}

uint32_t* flat_map::find(std::string_view key) {
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

const uint32_t* flat_map::find(std::string_view key) const {
    return const_cast<flat_map*>(this)->find(key);
}

void flat_map::insert_or_assign(std::string_view key, uint32_t value) {
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

void flat_map::emplace(std::string_view key, uint32_t value) {
    insert_or_assign(key, value);
}

bool flat_map::erase(std::string_view key) {
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

void flat_map::rehash(size_t new_cap) {
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

void flat_map::insert_internal(const Entry& entry) {
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

} // namespace detail
} // namespace compio