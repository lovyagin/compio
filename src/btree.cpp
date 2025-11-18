#include "compio/btree.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <optional>

#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"
#include "compio/debug_print.hpp"

using namespace compio;

#define RO(x) readonly(x, index_node)

node_reader::node_reader(FILE *file, uint64_t tree_degree, uint64_t max_size)
    : tree_degree(tree_degree),
      file(file),
      cache(max_size) {}

shared_node node_reader::read_node(uint64_t addr) {
    if (!cache.exists(addr)) {
        auto result = shared_node(file, addr, new index_node(tree_degree));
        result.read();     // read from file (because constructor with obj& does not read)
        result.unmodify(); // constructor with obj& sets modified=true
        cache.put(addr, result);
        return result;
    } else {
        return cache.get(addr);
    }
}

shared_node node_reader::create_node(uint64_t addr) {
    auto result = shared_node(file, addr, new index_node(tree_degree));
    cache.put(addr, result);
    return result;
}

void node_reader::remove_node(const shared_node &node) {
    if (!cache.exists(node.addr())) {
        WARNING_PRINT("warning: trying to remove non-existing index node\n");
        return;
    }
    cache.remove(node.addr());
}

void node_reader::clear_cache() { cache.clear(); }

btree::btree(uint64_t degree, bool is_readonly, smart_infile_object<header> archive_header,
             block_allocator *allocator, FILE *file, uint64_t cache_size)
    : degree(degree),
      is_readonly(is_readonly),
      archive_header(archive_header),
      allocator(allocator),
      reader(file, degree, cache_size) {
    if (readonly(archive_header, header)->index_root != 0)
        return;

    shared_node root = create_node();
    archive_header->index_root = root.addr();
}

void btree::insert_nonfull(shared_node &node, const tree_key &key, const tree_val &value) {
    std::size_t idx = std::lower_bound(RO(node)->keys.begin(), RO(node)->keys.end(), key) -
                      RO(node)->keys.begin();
    if (RO(node)->is_leaf) {
        if (RO(node)->keys[idx] == key) {
            WARNING_PRINT("warning: trying to insert already existing key\n");
            return;
        }
        node->keys.insert(node->keys.begin() + idx, key);
        node->values.insert(node->values.begin() + idx, value);
        node->num_keys++;
        node->validate();
    } else {
        auto child = read_child(node, idx);
        if (RO(child)->num_keys == (2 * degree - 1)) {
            split_child(node, child, idx);
            if (key > node->keys[idx]) {
                idx++;
                child = read_child(node, idx);
            }
        }
        insert_nonfull(child, key, value);
    }
}

void btree::insert(const tree_key &key, const tree_val &value) {
    DEBUG_PRINT("[BTREE]: insert(key={%lu,%lu},value={%lu,%lu})\n", key.hash, key.pos, value.addr,
                value.size);
    auto root = read_root();
    if (RO(root)->num_keys == (2 * degree - 1)) {
        auto new_root = create_node();
        new_root->is_leaf = false;
        new_root->children.push_back(root.addr());
        new_root->key_additions.push_back(0);

        split_child(new_root, root, 0);
        insert_nonfull(new_root, key, value);
        new_root->validate();
        archive_header->index_root = new_root.addr();
    } else {
        insert_nonfull(root, key, value);
    }
}

void btree::_remove_in_node(shared_node &node, uint64_t idx) {
    if (RO(node)->is_leaf) {
        node->keys.erase(node->keys.begin() + idx);
        node->values.erase(node->values.begin() + idx);
        node->num_keys--;
        node->validate();
    } else {
        auto child = read_child(node, idx);
        auto successor = read_child(node, idx + 1);
        if (RO(child)->num_keys >= degree) {
            const auto [p_key, p_val] = find_max_in_node(child);
            node->keys[idx] = p_key;
            node->values[idx] = p_val;
            _remove(child, p_key);
        } else if (RO(successor)->num_keys >= degree) {
            const auto [s_key, s_val] = find_min_in_node(successor);
            node->keys[idx] = s_key;
            node->values[idx] = s_val;
            _remove(successor, s_key);
        } else {
            const tree_key key = RO(node)->keys[idx];
            merge_children(node, idx);
            _remove(child, key);
        }
    }
}

void btree::_remove(shared_node &node, const tree_key &key) {
    std::size_t idx = std::lower_bound(RO(node)->keys.begin(), RO(node)->keys.end(), key) -
                      RO(node)->keys.begin();

    if (idx < RO(node)->num_keys && key == RO(node)->keys[idx]) {
        _remove_in_node(node, idx);
    } else {
        if (RO(node)->is_leaf) {
            WARNING_PRINT("warning: called btree::remove() with non-existent key\n");
            return;
        }

        auto child = read_child(node, idx);
        if (RO(child)->num_keys < degree) {
            child = populate_child(node, idx);
            if (!child.ptr()) {
                return;
            }
        }

        _remove(child, key);
    }
}

void btree::remove(const tree_key &key) {
    DEBUG_PRINT("[BTREE]: remove(key={%lu,%lu})\n", key.hash, key.pos);
    auto root = read_root();
    _remove(root, key);
    if (root->num_keys == 0 && !root->is_leaf) {
        archive_header->index_root = root->children[0];
        free_node(root);
    }
}

void btree::_get_range(shared_node &node, const tree_key &key_min, const tree_key &key_max,
                       std::vector<std::pair<tree_key, tree_val>> &result) {
    if (RO(node)->num_keys == 0)
        return;

    tree_key start{0, 0};
    tree_key end = RO(node)->keys[0];

    for (std::size_t i = 0; i <= RO(node)->num_keys; ++i) {
        if (!RO(node)->is_leaf && (key_min < end) && (key_max > start)) {
            auto child = read_child(node, i);
            _get_range(child, key_min, key_max, result);
        }

        if (i < RO(node)->num_keys) {
            start = RO(node)->keys[i];
            end.pos = start.pos + RO(node)->values[i].size;
            end.hash = start.hash;

            if ((key_min < end) && (key_max > start)) {
                result.emplace_back(start, RO(node)->values[i]);
            }
            start = end;
            end = (i < RO(node)->num_keys - 1) ? RO(node)->keys[i + 1]
                                               : tree_key{UINT64_MAX, UINT64_MAX};
        }
    }
}

std::vector<std::pair<tree_key, tree_val>> btree::get_range(const tree_key &key_min,
                                                            const tree_key &key_max) {
    if (key_max <= key_min) {
        WARNING_PRINT("warning: btree::get_range received invalid range bounds (key_min={%lu,%lu} "
                      ">= {%lu,%lu}=key_max)\n",
                      key_min.hash, key_min.pos, key_max.hash, key_max.pos);
        return {};
    }
    std::vector<std::pair<tree_key, tree_val>> result;
    auto root = read_root();
    _get_range(root, key_min, key_max, result);
    return result;
}

bool btree::_update(shared_node &node, const tree_key &key, const tree_val &new_value) {
    for (std::size_t i = 0; i < RO(node)->num_keys; ++i) {
        auto current_key = RO(node)->keys[i];
        if (current_key >= key) {
            if (current_key == key) {
                node->values[i] = new_value;
                return true;
            }
            if (!RO(node)->is_leaf) {
                auto child = read_child(node, i);
                return _update(child, key, new_value);
            } else {
                return false;
            }
        } else if (key < current_key + RO(node)->values[i].size) {
            return false;
        }
    }
    if (!RO(node)->is_leaf) {
        auto child = read_child(node, RO(node)->num_keys);
        return _update(child, key, new_value);
    }
    return false;
}

void btree::update(const tree_key &key, const tree_val &new_value) {
    DEBUG_PRINT("[BTREE]: update(key={%lu,%lu},new_value={%lu,%lu})\n", key.hash, key.pos,
                new_value.addr, new_value.size);
    auto root = read_root();
    if (!_update(root, key, new_value)) {
        WARNING_PRINT("warning: trying to update non-existing key\n");
    }
}

std::optional<tree_val> btree::get(const tree_key &key) {
    auto current = read_root();

    while (true) {
        std::size_t idx =
            std::lower_bound(RO(current)->keys.begin(), RO(current)->keys.end(), key) -
            RO(current)->keys.begin();

        if (idx < RO(current)->num_keys && RO(current)->keys[idx] == key) {
            return RO(current)->values[idx];
        } else if (!RO(current)->is_leaf) {
            current = read_child(current, idx);
        } else {
            return std::nullopt;
        }
    }
}

void btree::_add_to_range(shared_node &node, int64_t addition, const tree_key &key_min,
                          const tree_key &key_max) {
    if (RO(node)->num_keys == 0)
        return;

    std::size_t idx = std::lower_bound(RO(node)->keys.begin(), RO(node)->keys.end(), key_min) -
                      RO(node)->keys.begin();

    if (!RO(node)->is_leaf) {
        // this child is in range
        auto child = read_child(node, idx);
        _add_to_range(child, addition, key_min, key_max);
    }

    // iterate through keys, that are in range
    while (idx < RO(node)->num_keys && RO(node)->keys[idx] <= key_max) {
        node->keys[idx] = node->keys[idx] + addition;
        if (!RO(node)->is_leaf) {
            // if next key is in range
            if (idx + 1 < RO(node)->num_keys && RO(node)->keys[idx + 1] <= key_max) {
                // then child #idx+1 is also in range
                node->key_additions[idx + 1] += addition;
            } else {
                // otherwise, child #idx+1 is partially in range
                auto child = read_child(node, idx + 1);
                _add_to_range(child, addition, key_min, key_max);
                break;
            }
        }
        ++idx;
    }

    RO(node)->validate();
}

void btree::add_to_range(int64_t addition, const tree_key &key_min, const tree_key &key_max) {
    if (key_min > key_max) {
        WARNING_PRINT("warning: passed invalid range into btree::add_pos_to_keys_in_range "
                      "(key_min={%lu,%lu} > {%lu,%lu}=key_max)\n",
                      key_min.hash, key_min.pos, key_max.hash, key_max.pos);
        return;
    }

    auto root = read_root();
    _add_to_range(root, addition, key_min, key_max);
}

void btree::_print(shared_node node, uint64_t depth) {
    for (std::size_t i = 0; i <= node->num_keys; ++i) {
        if (!node->is_leaf) {
            _print(read_child(node, i), depth + 1);
        }

        if (i < node->num_keys) {
            auto key = node->keys[i];
            auto val = node->values[i];
            UNUSED(key);
            UNUSED(val);
            DEBUG_PRINT("%s{%lu, %lu} -> {%lu, %lu}\n", std::string(depth * 2, ' ').c_str(),
                        key.hash, key.pos, val.addr, val.size);
        }
    }
}

void btree::print() { _print(read_root(), 0); }

void btree::clear_cache() { reader.clear_cache(); }

void btree::split_child(shared_node &parent, shared_node &child, const uint64_t idx) {
    auto new_node = create_node();

    new_node->is_leaf = child->is_leaf;
    new_node->num_keys = degree - 1;
    new_node->keys.insert(new_node->keys.end(), child->keys.begin() + degree, child->keys.end());
    new_node->values.insert(new_node->values.end(), child->values.begin() + degree,
                            child->values.end());

    if (!child->is_leaf) {
        new_node->children.resize(degree);
        new_node->key_additions.resize(degree);
        std::copy(child->children.begin() + degree, child->children.end(),
                  new_node->children.begin());
        std::copy(child->key_additions.begin() + degree, child->key_additions.end(),
                  new_node->key_additions.begin());
    }
    new_node->validate();

    tree_key middle_key = child->keys[degree - 1];
    tree_val middle_value = child->values[degree - 1];
    child->num_keys = degree - 1;
    child->keys.resize(degree - 1);
    child->values.resize(degree - 1);
    child->children.resize(degree);
    child->key_additions.resize(degree);
    child->validate();

    parent->children.insert(parent->children.begin() + idx + 1, new_node.addr());
    parent->key_additions.insert(parent->key_additions.begin() + idx + 1, 0);
    parent->keys.insert(parent->keys.begin() + idx, middle_key);
    parent->values.insert(parent->values.begin() + idx, middle_value);
    parent->num_keys++;
    parent->validate();
}

void btree::merge_children(shared_node &parent, const uint64_t idx) {
    auto child = read_child(parent, idx);
    auto sibling = read_child(parent, idx + 1);

    child->num_keys += sibling->num_keys + 1;
    child->keys.push_back(parent->keys[idx]);
    child->values.push_back(parent->values[idx]);
    child->keys.insert(child->keys.end(), sibling->keys.begin(), sibling->keys.end());
    child->values.insert(child->values.end(), sibling->values.begin(), sibling->values.end());
    if (!child->is_leaf) {
        child->children.insert(child->children.end(), sibling->children.begin(),
                               sibling->children.end());
        child->key_additions.insert(child->key_additions.end(), sibling->key_additions.begin(),
                                    sibling->key_additions.end());
    }
    child->validate();

    parent->keys.erase(parent->keys.begin() + idx);
    parent->values.erase(parent->values.begin() + idx);
    parent->children.erase(parent->children.begin() + idx + 1);
    parent->key_additions.erase(parent->key_additions.begin() + idx + 1);
    parent->num_keys--;
    parent->validate();

    free_node(sibling);
}

void btree::borrow_from_prev(shared_node &parent, const uint64_t idx) {
    auto child = read_child(parent, idx);
    auto sibling = read_child(parent, idx - 1);
    assert(child->is_leaf == sibling->is_leaf);

    child->keys.insert(child->keys.begin(), parent->keys[idx - 1]);
    child->values.insert(child->values.begin(), parent->values[idx - 1]);
    if (!child->is_leaf) {
        child->children.insert(child->children.begin(), sibling->children.back());
        child->key_additions.insert(child->key_additions.begin(), sibling->key_additions.back());
    }
    child->num_keys++;
    child->validate();

    parent->keys[idx - 1] = sibling->keys.back();
    parent->values[idx - 1] = sibling->values.back();
    parent->validate();

    sibling->keys.pop_back();
    sibling->values.pop_back();
    if (!sibling->is_leaf) {
        sibling->children.pop_back();
        sibling->key_additions.pop_back();
    }
    sibling->num_keys--;
    sibling->validate();
}

void btree::borrow_from_next(shared_node &parent, const uint64_t idx) {
    auto child = read_child(parent, idx);
    auto sibling = read_child(parent, idx + 1);
    assert(child->is_leaf == sibling->is_leaf);

    child->keys.push_back(parent->keys[idx]);
    child->values.push_back(parent->values[idx]);
    if (!child->is_leaf) {
        child->children.push_back(sibling->children[0]);
        child->key_additions.push_back(sibling->key_additions[0]);
    }
    child->num_keys++;
    child->validate();

    parent->keys[idx] = sibling->keys[0];
    parent->values[idx] = sibling->values[0];
    parent->validate();

    sibling->keys.erase(sibling->keys.begin());
    sibling->values.erase(sibling->values.begin());
    if (!sibling->is_leaf) {
        sibling->children.erase(sibling->children.begin());
        sibling->key_additions.erase(sibling->key_additions.begin());
    }
    sibling->num_keys--;
    sibling->validate();
}

shared_node btree::populate_child(shared_node &node, uint64_t idx) {
    if (RO(node)->num_keys == 0) {
        WARNING_PRINT("warning: called btree::populate_child on node with one child\n");
        return {};
    }

    if (idx > 0) {
        auto predecessor = read_child(node, idx - 1);
        if (RO(predecessor)->num_keys >= degree) {
            borrow_from_prev(node, idx);
            return read_child(node, idx);
        }
    }

    if (idx < RO(node)->num_keys) {
        auto successor = read_child(node, idx + 1);
        if (RO(successor)->num_keys >= degree) {
            borrow_from_next(node, idx);
            return read_child(node, idx);
        }
    }

    if (idx < RO(node)->num_keys) {
        merge_children(node, idx);
        return read_child(node, idx);
    } else {
        merge_children(node, idx - 1);
        return read_child(node, idx - 1);
    }
}

std::pair<tree_key, tree_val> btree::find_max_in_node(shared_node node) {
    while (!RO(node)->is_leaf) {
        node = read_child(node, RO(node)->num_keys);
    }
    return {RO(node)->keys.back(), RO(node)->values.back()};
}

std::pair<tree_key, tree_val> btree::find_min_in_node(shared_node node) {
    while (!RO(node)->is_leaf) {
        node = read_child(node, 0);
    }
    return {RO(node)->keys[0], RO(node)->values[0]};
}

uint64_t btree::allocate_node() { return allocator->allocate(INDEX_NODE_SIZE(degree)); }

void btree::free_node(const shared_node &node) {
    reader.remove_node(node);
    allocator->deallocate(node.addr(), INDEX_NODE_SIZE(degree));
}

shared_node btree::create_node() { return reader.create_node(allocate_node()); }

shared_node btree::read_node(uint64_t addr) {
    auto node = reader.read_node(addr);
    RO(node)->validate();
    return node;
}

shared_node btree::read_child(shared_node &node, uint64_t idx) {
    assert(RO(node)->is_leaf == false);
    assert(idx <= RO(node)->num_keys);
    auto child = read_node(RO(node)->children[idx]);
    const int64_t addition = RO(node)->key_additions[idx];
    if (addition != 0) {
        if (RO(child)->num_keys > 0) {
            for (auto &key : child->keys) {
                key = key + addition;
            }
        }
        if (!RO(child)->is_leaf) {
            for (auto &key_addition : child->key_additions) {
                key_addition += addition;
            }
        }
        node->key_additions[idx] = 0;
    }
    if (is_readonly) {
        // key_additions will be applied, but not written to file
        node.unmodify();
        child.unmodify();
    }
    return child;
}

shared_node btree::read_root() { return read_node(readonly(archive_header, header)->index_root); }
