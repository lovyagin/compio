#pragma once

#include <vector>
#include <unordered_map>
#include "include/btree.hpp"

class MockBTree {
public:
    std::vector<std::pair<compio::tree_key, compio::tree_val>> nodes;

    void get_range(const compio::tree_key& key_min, const compio::tree_key& key_max,
                   std::vector<std::pair<compio::tree_key, compio::tree_val>>& result) {
        for (const auto& node : nodes) {
            if (node.first.hash == key_min.hash && !(mp[node.first.hash].end < key_min.pos ||  mp[node.first.hash].start > key_max.pos)) {
                result.push_back(node);
            }
        }
    }

    bool update(const compio::tree_key& key, const compio::tree_val& new_value) {
        for (auto& node : nodes) {
            if (node.first.hash == key.hash && node.first.pos == key.pos) {
                node.second = new_value;
                return true;
            }
        }
        return false;
    }

    using Hash = uint64_t;

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
