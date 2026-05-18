/*
 * Original source code: https://github.com/lamerman/cpp-lru-cache.git
 * License: BSD-3-Clause (see below)
 *
 * Modifications:
 * + Added methods: remove, clear, is_full, pop_back, pop, add_to_range
 * + Replaced unordered_map with map
 * + Added hit probability measurement
 *
 * Copyright (c) 2014, lamerman
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of lamerman nor the names of its
 * contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _LRUCACHE_HPP_INCLUDED_
#define _LRUCACHE_HPP_INCLUDED_

#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace cache {

template <typename key_t, typename value_t, typename comparator = std::less<key_t>>
class lru_cache {
public:
    typedef typename std::pair<key_t, value_t> key_value_pair_t;
    typedef typename std::list<key_value_pair_t>::iterator list_iterator_t;

    lru_cache(size_t max_size) : _max_size(max_size), _hit_count(0), _total_count(0) {}

    void put(const key_t &key, const value_t &value) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _cache_items_map.find(key);
        if (it != _cache_items_map.end()) {
            _cache_items_list.splice(_cache_items_list.begin(), _cache_items_list, it->second);
            it->second->second = value;
            return;
        }
        _cache_items_list.push_front(key_value_pair_t(key, value));
        _cache_items_map[key] = _cache_items_list.begin();

        if (_cache_items_map.size() > _max_size) {
            auto last = _cache_items_list.end();
            last--;
            _cache_items_map.erase(last->first);
            _cache_items_list.pop_back();
        }
    }

    std::optional<value_t> get(const key_t &key) {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_total_count;
        auto it = _cache_items_map.find(key);
        if (it == _cache_items_map.end()) {
            return std::nullopt;
        } else {
            ++_hit_count;
            _cache_items_list.splice(_cache_items_list.begin(), _cache_items_list, it->second);
            return it->second->second;
        }
    }

    bool exists(const key_t &key) const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _cache_items_map.find(key) != _cache_items_map.end();
    }

    void remove(const key_t &key) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _cache_items_map.find(key);
        if (it == _cache_items_map.end()) {
            throw std::range_error("There is no such key in cache");
        } else {
            _cache_items_list.erase(it->second);
            _cache_items_map.erase(it);
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(_mutex);
        _cache_items_map.clear();
        _cache_items_list.clear();
    }

    bool is_full() { 
        std::lock_guard<std::mutex> lock(_mutex);
        return _cache_items_map.size() >= _max_size; 
    }

    std::vector<value_t> extract_all() {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<value_t> result;
        result.reserve(_cache_items_list.size());
        for (const auto& item : _cache_items_list) {
            result.push_back(item.second);
        }
        _cache_items_map.clear();
        _cache_items_list.clear();
        return result;
    }

    value_t pop_back() {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_cache_items_map.size() == 0) {
            throw std::range_error("Trying to pop back from empty cache");
        }
        auto last = _cache_items_list.end();
        last--;
        value_t result = last->second;
        _cache_items_map.erase(last->first);
        _cache_items_list.pop_back();
        return result;
    }

    value_t pop(const key_t &key) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _cache_items_map.find(key);
        if (it == _cache_items_map.end()) {
            throw std::range_error("There is no such key in cache");
        } else {
            value_t result = (*it->second).second;
            _cache_items_list.erase(it->second);
            _cache_items_map.erase(it);
            return result;
        }
    }

    size_t size() const { 
        std::lock_guard<std::mutex> lock(_mutex);
        return _cache_items_map.size(); 
    }

    template <typename Func>
    void for_each_in_range(const key_t& key_min, const key_t& key_max, Func&& func) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it_start = _cache_items_map.lower_bound(key_min);
        auto it_end = _cache_items_map.upper_bound(key_max);

        if (it_start == it_end) {
            return;
        }

        std::vector<std::pair<key_t, list_iterator_t>> to_update;

        for (auto it = it_start; it != it_end; ++it) {
            to_update.push_back(*it);
        }

        _cache_items_map.erase(it_start, it_end);

        for (auto &[key, list_it] : to_update) {
            func(key, list_it->second);
            list_it->first = key;
            _cache_items_map[key] = list_it;
        }
    }

    template <typename addition_t>
    void add_to_range(addition_t addition, const key_t &key_min, const key_t &key_max) {
        for_each_in_range(key_min, key_max, [addition](key_t& key, value_t&) {
            key = key + addition;
        });
    }

    double get_hit_probability() const {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_total_count == 0) return 0.0;
        return static_cast<double>(_hit_count) / _total_count;
    }

    std::list<key_value_pair_t> _cache_items_list;
    std::map<key_t, list_iterator_t, comparator> _cache_items_map;
    size_t _max_size;
    size_t _hit_count;
    size_t _total_count;
    mutable std::mutex _mutex;
};

template <typename key_t, typename value_t, typename hasher = std::hash<key_t>, typename key_equal = std::equal_to<key_t>>
class lru_cache_unordered {
    template <typename K, typename V, typename H, typename E>
    friend class sharded_lru_cache;
public:
    typedef typename std::pair<key_t, value_t> key_value_pair_t;
    typedef typename std::list<key_value_pair_t>::iterator list_iterator_t;

    lru_cache_unordered(size_t max_size) : _max_size(max_size), _hit_count(0), _total_count(0) {}

    void put(const key_t &key, const value_t &value) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _cache_items_map.find(key);
        if (it != _cache_items_map.end()) {
            _cache_items_list.splice(_cache_items_list.begin(), _cache_items_list, it->second);
            it->second->second = value;
            return;
        }
        _cache_items_list.push_front(key_value_pair_t(key, value));
        _cache_items_map[key] = _cache_items_list.begin();

        if (_cache_items_map.size() > _max_size) {
            auto last = _cache_items_list.end();
            last--;
            _cache_items_map.erase(last->first);
            _cache_items_list.pop_back();
        }
    }

    std::optional<value_t> get(const key_t &key) {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_total_count;
        auto it = _cache_items_map.find(key);
        if (it == _cache_items_map.end()) {
            return std::nullopt;
        } else {
            ++_hit_count;
            _cache_items_list.splice(_cache_items_list.begin(), _cache_items_list, it->second);
            return it->second->second;
        }
    }

    bool exists(const key_t &key) const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _cache_items_map.find(key) != _cache_items_map.end();
    }

    void remove(const key_t &key) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _cache_items_map.find(key);
        if (it == _cache_items_map.end()) {
            throw std::range_error("There is no such key in cache");
        } else {
            _cache_items_list.erase(it->second);
            _cache_items_map.erase(it);
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(_mutex);
        _cache_items_map.clear();
        _cache_items_list.clear();
    }

    bool is_full() { 
        std::lock_guard<std::mutex> lock(_mutex);
        return _cache_items_map.size() >= _max_size; 
    }

    std::vector<value_t> extract_all() {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<value_t> result;
        result.reserve(_cache_items_list.size());
        for (const auto& item : _cache_items_list) {
            result.push_back(item.second);
        }
        _cache_items_map.clear();
        _cache_items_list.clear();
        return result;
    }

    value_t pop_back() {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_cache_items_map.size() == 0) {
            throw std::range_error("Trying to pop back from empty cache");
        }
        auto last = _cache_items_list.end();
        last--;
        value_t result = last->second;
        _cache_items_map.erase(last->first);
        _cache_items_list.pop_back();
        return result;
    }

    value_t pop(const key_t &key) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _cache_items_map.find(key);
        if (it == _cache_items_map.end()) {
            throw std::range_error("There is no such key in cache");
        } else {
            value_t result = (*it->second).second;
            _cache_items_list.erase(it->second);
            _cache_items_map.erase(it);
            return result;
        }
    }

    size_t size() const { 
        std::lock_guard<std::mutex> lock(_mutex);
        return _cache_items_map.size(); 
    }

    double get_hit_probability() const {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_total_count == 0) {
            return 0.0;
        }
        return static_cast<double>(_hit_count) / _total_count;
    }

    std::list<key_value_pair_t> _cache_items_list;
    std::unordered_map<key_t, list_iterator_t, hasher, key_equal> _cache_items_map;
    size_t _max_size;
    size_t _hit_count;
    size_t _total_count;
    mutable std::mutex _mutex;
};

template <typename key_t, typename value_t, typename hasher = std::hash<key_t>, typename key_equal = std::equal_to<key_t>>
class sharded_lru_cache {
public:
    sharded_lru_cache(size_t max_size, size_t num_shards = 16)
        : _max_size(max_size), _num_shards(num_shards) {
        if (_num_shards == 0) _num_shards = 1;
        // For small caches, use a single shard to preserve associativity and avoid
        // thrashing due to collisions in small buckets (effectively direct-mapped).
        if (_max_size < _num_shards * 4) {
            _num_shards = 1;
        }
        
        size_t per_shard = _num_shards > 0 ? _max_size / _num_shards : 0;
        size_t remainder = _num_shards > 0 ? _max_size % _num_shards : 0;

        _shards.reserve(_num_shards);
        for (size_t i = 0; i < _num_shards; ++i) {
            size_t shard_size = per_shard + (i < remainder ? 1 : 0);
            _shards.emplace_back(new lru_cache_unordered<key_t, value_t, hasher, key_equal>(shard_size));
        }
    }

    void put(const key_t &key, const value_t &value) {
        get_shard(key)->put(key, value);
    }

    std::optional<value_t> get(const key_t &key) {
        return get_shard(key)->get(key);
    }

    bool exists(const key_t &key) const {
        return get_shard(key)->exists(key);
    }

    void remove(const key_t &key) {
        get_shard(key)->remove(key);
    }

    void clear() {
        for (auto &shard : _shards) {
            shard->clear();
        }
    }

    double get_hit_probability() const {
        size_t total_hits = 0;
        size_t total_count = 0;
        for (const auto &shard : _shards) {
            // Need to lock shard mutex to read stats safely, but lru_cache_unordered doesn't expose mutex.
            // Since we made it friend, we can access _mutex.
            std::lock_guard<std::mutex> lock(shard->_mutex);
            total_hits += shard->_hit_count;
            total_count += shard->_total_count;
        }
        if (total_count == 0) return 0.0;
        return static_cast<double>(total_hits) / total_count;
    }

private:
    lru_cache_unordered<key_t, value_t, hasher, key_equal>* get_shard(const key_t &key) const {
        return _shards[_hasher(key) % _num_shards].get();
    }

    size_t _max_size;
    size_t _num_shards;
    hasher _hasher;
    std::vector<std::unique_ptr<lru_cache_unordered<key_t, value_t, hasher, key_equal>>> _shards;
};

} // namespace cache

#endif /* _LRUCACHE_HPP_INCLUDED_ */
