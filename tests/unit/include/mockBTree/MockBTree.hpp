#pragma once

#include <vector>
#include <unordered_map>
#include "include/btree.hpp"

class MockBTree {
public:
    using Hash = uint64_t;
    using Key = compio::tree_key;
    using Val = compio::tree_val;

    typedef struct {
		Key key;
        Val val;
    } KeyValuePair;

    std::vector<KeyValuePair> nodes;

    void get_range(const compio::tree_key& key_min, const compio::tree_key& key_max,
                   std::vector<std::pair<compio::tree_key, compio::tree_val>>& result) {
        for (const auto& node : nodes) {
            if (node.key.hash == key_min.hash && !(mp[node.key.hash].end < key_min.pos ||  mp[node.key.hash].start > key_max.pos)) {
                result.push_back({node.key, node.val});
            }
        }
    }

    bool update(const compio::tree_key& key, const compio::tree_val& new_value) {
        for (auto& node : nodes) {
            if (node.key.hash == key.hash && node.key.pos == key.pos) {
                node.val = new_value;
                return true;
            }
        }
        return false;
    }

	typedef struct {
		uint64_t start;
        uint64_t end;
	} Segment;

    void insert_segment(Hash hash, Segment segment) {
        mp[hash] = segment;
    }
private:
    std::unordered_map<Hash, Segment> mp;
};
