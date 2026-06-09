#ifndef _LRU_2Q_CACHE_HPP_INCLUDED_
#define _LRU_2Q_CACHE_HPP_INCLUDED_

#include <cstddef>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace cache {

template <typename key_t, typename value_t, typename comparator = std::less<key_t>>
class lru_2q_cache {
public:
    lru_2q_cache(size_t max_size)
        : _max_size(max_size),
          _in_max_size(max_size / 4),
          _main_max_size(max_size - _in_max_size),
          _hit_count(0),
          _total_count(0) {}

    void put(const key_t &key, const value_t &value) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _items_map.find(key);
        if (it != _items_map.end()) {
            // Key already exists: update value
            it->second.value = value;
            if (it->second.in_main_queue) {
                // Move to front of main_queue
                _main_queue.splice(_main_queue.begin(), _main_queue, it->second.queue_iter);
            }
            // If in in_queue, leave it in place (FIFO order preserved)
            return;
        }

        // New entry: add to in_queue
        _in_queue.push_back(key);
        _items_map[key] = {value, false, --_in_queue.end()};

        // Evict from in_queue if over capacity
        if (_in_queue.size() > _in_max_size) {
            auto oldest_key = _in_queue.front();
            _in_queue.pop_front();
            _items_map.erase(oldest_key);
        }

        // Evict from main_queue if total over capacity
        if (_items_map.size() > _max_size) {
            auto oldest_key = _main_queue.back();
            _main_queue.pop_back();
            _items_map.erase(oldest_key);
        }
    }

    std::optional<value_t> get(const key_t &key) {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_total_count;
        auto it = _items_map.find(key);
        if (it == _items_map.end()) {
            return std::nullopt;
        }

        ++_hit_count;

        if (!it->second.in_main_queue) {
            // Promote from in_queue to main_queue
            _in_queue.erase(it->second.queue_iter);
            _main_queue.push_front(key);
            it->second.queue_iter = _main_queue.begin();
            it->second.in_main_queue = true;
        } else {
            // Already in main_queue: move to front (LRU)
            _main_queue.splice(_main_queue.begin(), _main_queue, it->second.queue_iter);
        }

        return it->second.value;
    }

    bool exists(const key_t &key) const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _items_map.find(key) != _items_map.end();
    }

    void remove(const key_t &key) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _items_map.find(key);
        if (it == _items_map.end()) {
            throw std::range_error("There is no such key in cache");
        }
        if (it->second.in_main_queue) {
            _main_queue.erase(it->second.queue_iter);
        } else {
            _in_queue.erase(it->second.queue_iter);
        }
        _items_map.erase(it);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(_mutex);
        _items_map.clear();
        _in_queue.clear();
        _main_queue.clear();
    }

    std::vector<value_t> extract_all() {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<value_t> result;
        result.reserve(_items_map.size());
        // Iterate in insertion order: first in_queue (FIFO, oldest first),
        // then main_queue (most recently promoted first).
        for (const auto &key : _in_queue) {
            auto it = _items_map.find(key);
            if (it != _items_map.end()) {
                result.push_back(it->second.value);
            }
        }
        for (const auto &key : _main_queue) {
            auto it = _items_map.find(key);
            if (it != _items_map.end()) {
                result.push_back(it->second.value);
            }
        }
        _items_map.clear();
        _in_queue.clear();
        _main_queue.clear();
        return result;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _items_map.size();
    }

    double get_hit_probability() const {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_total_count == 0) return 0.0;
        return static_cast<double>(_hit_count) / _total_count;
    }

    // Raw counters (used to aggregate hit-rate across sharded caches).
    size_t hit_count() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _hit_count;
    }
    size_t access_count() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _total_count;
    }

    template <typename Func>
    void for_each_in_range(const key_t& key_min, const key_t& key_max, Func&& func) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it_start = _items_map.lower_bound(key_min);
        auto it_end = _items_map.upper_bound(key_max);

        if (it_start == it_end) {
            return;
        }

        // Copy entries into a temporary vector
        std::vector<std::pair<key_t, ItemInfo>> to_update;
        for (auto it = it_start; it != it_end; ++it) {
            to_update.push_back(*it);
        }

        // Erase the range from the map
        _items_map.erase(it_start, it_end);

        // Process each entry
        for (auto &[key, info] : to_update) {
            // Call the functor: it receives the new key (by reference) and the value
            func(key, info.value);
            // Update the list node's key
            *info.queue_iter = key;
            // Insert into map with the new key
            _items_map[key] = info;
        }
    }

    template <typename addition_t>
    void add_to_range(addition_t addition, const key_t &key_min, const key_t &key_max) {
        for_each_in_range(key_min, key_max, [addition](key_t& key, value_t&) {
            key = key + addition;
        });
    }

#ifndef NDEBUG
    /// Debug helper: return a copy of all keys currently in the cache (for debug printing).
    std::vector<key_t> debug_keys() const {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<key_t> keys;
        keys.reserve(_items_map.size());
        for (const auto &key : _in_queue) {
            keys.push_back(key);
        }
        for (const auto &key : _main_queue) {
            keys.push_back(key);
        }
        return keys;
    }
#endif

private:
    struct ItemInfo {
        value_t value;
        bool in_main_queue;
        typename std::list<key_t>::iterator queue_iter;
    };

    std::map<key_t, ItemInfo, comparator> _items_map;
    std::list<key_t> _in_queue;   // FIFO queue
    std::list<key_t> _main_queue; // LRU queue

    size_t _max_size;
    size_t _in_max_size;
    size_t _main_max_size;
    size_t _hit_count;
    size_t _total_count;
    mutable std::mutex _mutex;
};

} // namespace cache

#endif /* _LRU_2Q_CACHE_HPP_INCLUDED_ */
