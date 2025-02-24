#pragma once

#include <algorithm>
#include <vector>
#include <unordered_map>
#include "BTree/btree.hpp"
#include "BTree/IBTree.hpp"

class MockBTree : public compio::IBTree {
public:
    using Hash = uint64_t;
    using Key = compio::tree_key;
    using Val = compio::tree_val;

    typedef struct {
		Key key;
        Val val;
    } KeyValuePair;

    std::vector<KeyValuePair> nodes;

    void insert(const compio::tree_key key, const compio::tree_val value) override {
        nodes.push_back({key, value});
    }

    void remove(const compio::tree_key key) override {
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                    [&key](const KeyValuePair& kv) { return kv.key.hash == key.hash && kv.key.pos == key.pos; }),
                    nodes.end());
    }

    void get_range(const compio::tree_key key_min, const compio::tree_key key_max,
                   std::vector<std::pair<compio::tree_key, compio::tree_val>>& result) override {
        for (const auto& [key, val] : nodes) {
            if (key.hash == key_min.hash && !(mp[key.hash].end < key_min.pos ||  mp[key.hash].start > key_max.pos)) {
                result.emplace_back(key, val);
            }
        }
    }

    bool update(const compio::tree_key key, const compio::tree_val new_value) override {
        for (auto& [k, v] : nodes) {
            if (k.hash == key.hash && k.pos == key.pos) {
                v = new_value;
                return true;
            }
        }
        return false;
    }

	typedef struct {
		uint64_t start;
        uint64_t end;
	} Segment;

    void insert_segment(const Hash hash, const Segment segment) {
        mp[hash] = segment;
    }
private:
    std::unordered_map<Hash, Segment> mp;
};
