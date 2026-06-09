#ifndef _SHARDED_LRU_2Q_CACHE_HPP_INCLUDED_
#define _SHARDED_LRU_2Q_CACHE_HPP_INCLUDED_

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "lru_2q_cache.hpp"

namespace cache {

// Concurrency-friendly wrapper over lru_2q_cache: keys are partitioned across
// several independent shards, each with its own mutex. Point operations
// (get/put/exists/remove) touch only one shard, so reads of different keys
// proceed in parallel instead of serializing on a single cache lock. Range and
// whole-cache operations fan out across all shards (they are off the hot path).
//
// shard_selector maps a key to a size_t; the shard is selector(key) % nshards.
// Choose a selector that spreads the workload (e.g. file identity) so that
// independent streams land on distinct shards.
template <typename key_t, typename value_t, typename comparator = std::less<key_t>,
          typename shard_selector = std::hash<key_t>>
class sharded_lru_2q_cache {
    using shard_t = lru_2q_cache<key_t, value_t, comparator>;

    static constexpr size_t kMaxShards = 64;

    static size_t pick_shards(size_t max_size) {
        if (max_size == 0) return 1; // single no-retention shard
        return std::min<size_t>(kMaxShards, max_size);
    }

public:
    explicit sharded_lru_2q_cache(size_t max_size, shard_selector selector = shard_selector())
        : _nshards(pick_shards(max_size)), _selector(selector) {
        const size_t per_shard = (max_size == 0) ? 0 : std::max<size_t>(1, max_size / _nshards);
        _shards.reserve(_nshards);
        for (size_t i = 0; i < _nshards; ++i)
            _shards.push_back(std::make_unique<shard_t>(per_shard));
    }

    void put(const key_t &key, const value_t &value) { shard(key).put(key, value); }
    std::optional<value_t> get(const key_t &key) { return shard(key).get(key); }
    bool exists(const key_t &key) const { return shard(key).exists(key); }
    void remove(const key_t &key) { shard(key).remove(key); }

    void clear() {
        for (auto &s : _shards) s->clear();
    }

    std::vector<value_t> extract_all() {
        std::vector<value_t> result;
        for (auto &s : _shards) {
            auto part = s->extract_all();
            result.insert(result.end(), std::make_move_iterator(part.begin()),
                          std::make_move_iterator(part.end()));
        }
        return result;
    }

    size_t size() const {
        size_t n = 0;
        for (auto &s : _shards) n += s->size();
        return n;
    }

    double get_hit_probability() const {
        size_t hits = 0, accesses = 0;
        for (auto &s : _shards) {
            hits += s->hit_count();
            accesses += s->access_count();
        }
        if (accesses == 0) return 0.0;
        return static_cast<double>(hits) / accesses;
    }

    // A re-keying functor (add_to_range/shift) only changes the position within
    // a file, never the shard-selecting component, so entries stay in their
    // shard. Fanning out over every shard is therefore always correct.
    template <typename Func>
    void for_each_in_range(const key_t &key_min, const key_t &key_max, Func &&func) {
        for (auto &s : _shards) s->for_each_in_range(key_min, key_max, func);
    }

    template <typename addition_t>
    void add_to_range(addition_t addition, const key_t &key_min, const key_t &key_max) {
        for (auto &s : _shards) s->add_to_range(addition, key_min, key_max);
    }

#ifndef NDEBUG
    std::vector<key_t> debug_keys() const {
        std::vector<key_t> keys;
        for (auto &s : _shards) {
            auto part = s->debug_keys();
            keys.insert(keys.end(), part.begin(), part.end());
        }
        return keys;
    }
#endif

private:
    shard_t &shard(const key_t &key) { return *_shards[_selector(key) % _nshards]; }
    const shard_t &shard(const key_t &key) const { return *_shards[_selector(key) % _nshards]; }

    size_t _nshards;
    shard_selector _selector;
    std::vector<std::unique_ptr<shard_t>> _shards;
};

} // namespace cache

#endif /* _SHARDED_LRU_2Q_CACHE_HPP_INCLUDED_ */
