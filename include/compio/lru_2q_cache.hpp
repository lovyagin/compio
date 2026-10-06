#ifndef _LRU_2Q_CACHE_HPP_INCLUDED_
#define _LRU_2Q_CACHE_HPP_INCLUDED_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace cache {

// 2Q replacement (Johnson and Shasha, VLDB 1994), full version.
//
// A new entry enters a FIFO queue (A1in). An entry that leaves that queue is
// dropped, but its key is remembered in a ghost queue (A1out). Only an entry
// that comes back while its key is still remembered goes to the main LRU queue
// (Am): it has been referenced again after a pause, which is what tells a
// reused entry from one that is touched a few times in a row and never again.
// Hits inside A1in therefore do not promote.
//
// A1in is meant to take a quarter of the cache and the ghost queue remembers
// half as many keys as there are entries. Nothing is evicted while there is
// room, so a cache that is not full keeps everything.
template <typename key_t, typename value_t, typename comparator = std::less<key_t>>
class lru_2q_cache {
public:
    lru_2q_cache(size_t max_size) : _max_size(max_size), _hit_count(0), _total_count(0) {}

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

        auto ghost = _ghost_map.find(key);
        if (ghost != _ghost_map.end()) {
            _ghost_queue.erase(ghost->second);
            _ghost_map.erase(ghost);
            _main_queue.push_front(key);
            _items_map[key] = {value, true, _main_queue.begin()};
        } else {
            _in_queue.push_back(key);
            _items_map[key] = {value, false, --_in_queue.end()};
        }

        while (_items_map.size() > _max_size) {
            evict_one_locked();
        }
        _approx_size.store(_items_map.size(), std::memory_order_relaxed);
    }

    // Drop one entry by the 2Q rule. Returns false if the cache is empty. The
    // entry is destroyed under the cache lock, so a lookup for it waits until
    // its destructor has run.
    bool evict_one() {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_items_map.empty()) {
            return false;
        }
        evict_one_locked();
        _approx_size.store(_items_map.size(), std::memory_order_relaxed);
        return true;
    }

    // Size as of the last change, readable without taking the cache lock.
    size_t approx_size() const { return _approx_size.load(std::memory_order_relaxed); }

    std::optional<value_t> get(const key_t &key) {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_total_count;
        auto it = _items_map.find(key);
        if (it == _items_map.end()) {
            return std::nullopt;
        }

        ++_hit_count;

        if (it->second.in_main_queue) {
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
        _approx_size.store(_items_map.size(), std::memory_order_relaxed);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(_mutex);
        _items_map.clear();
        _in_queue.clear();
        _main_queue.clear();
        _ghost_map.clear();
        _ghost_queue.clear();
        _approx_size.store(0, std::memory_order_relaxed);
    }

    // Values in the order extract_all() returns them; the cache keeps them.
    std::vector<value_t> values() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return values_locked();
    }

    std::vector<value_t> extract_all() {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<value_t> result = values_locked();
        _items_map.clear();
        _in_queue.clear();
        _main_queue.clear();
        _ghost_map.clear();
        _ghost_queue.clear();
        _approx_size.store(0, std::memory_order_relaxed);
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

        // The keys in the range are about to mean something else, so what is
        // remembered about them no longer applies.
        for (auto ghost = _ghost_map.lower_bound(key_min); ghost != _ghost_map.end() && !_ghost_map.key_comp()(key_max, ghost->first);) {
            _ghost_queue.erase(ghost->second);
            ghost = _ghost_map.erase(ghost);
        }

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

    // First the entries seen once (oldest first), then the main queue (most
    // recently used first).
    std::vector<value_t> values_locked() const {
        std::vector<value_t> result;
        result.reserve(_items_map.size());
        for (const auto &key : _in_queue) {
            result.push_back(_items_map.find(key)->second.value);
        }
        for (const auto &key : _main_queue) {
            result.push_back(_items_map.find(key)->second.value);
        }
        return result;
    }

    // The cache may be one shard of a shared budget, so its own share is not
    // known in advance: the quarter is taken of what it holds at the moment.
    void evict_one_locked() {
        const bool from_in_queue = _main_queue.empty() || _in_queue.size() * 4 > _items_map.size();
        if (from_in_queue) {
            auto key = _in_queue.front();
            _in_queue.pop_front();
            _items_map.erase(key);
            remember(key);
        } else {
            auto key = _main_queue.back();
            _main_queue.pop_back();
            _items_map.erase(key);
        }
    }

    void remember(const key_t &key) {
        const size_t limit = std::min(_max_size / 2, std::max<size_t>(_items_map.size() / 2, kMinGhostKeys));
        if (limit == 0) {
            return;
        }
        _ghost_queue.push_back(key);
        _ghost_map[key] = --_ghost_queue.end();
        while (_ghost_queue.size() > limit) {
            _ghost_map.erase(_ghost_queue.front());
            _ghost_queue.pop_front();
        }
    }

    static constexpr size_t kMinGhostKeys = 16;

    std::map<key_t, ItemInfo, comparator> _items_map;
    std::list<key_t> _in_queue;    // A1in: FIFO, values held
    std::list<key_t> _main_queue;  // Am: LRU, values held
    std::list<key_t> _ghost_queue; // A1out: FIFO, keys only
    std::map<key_t, typename std::list<key_t>::iterator, comparator> _ghost_map;

    size_t _max_size;
    size_t _hit_count;
    size_t _total_count;
    std::atomic<size_t> _approx_size{0};
    mutable std::mutex _mutex;
};

} // namespace cache

#endif /* _LRU_2Q_CACHE_HPP_INCLUDED_ */
