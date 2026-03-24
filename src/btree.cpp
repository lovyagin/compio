#define __STDC_FORMAT_MACROS
#include "compio/btree.hpp"

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <limits>
#include <optional>

#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"
#include "compio/debug_print.hpp"

using namespace compio;

#define RO(x) readonly(x, index_node)

node_reader::node_reader(FILE *file, uint64_t tree_degree, uint64_t max_size, std::mutex *io_mutex, compio::WalManager *wal)
    : tree_degree(tree_degree),
      file(file),
      io_mutex(io_mutex),
      wal(wal),
      cache(max_size) {}

shared_node node_reader::read_node(uint64_t addr) {
    auto node = cache.get(addr);
    if (!node.has_value()) {
        auto result = shared_node(file, addr, new index_node(tree_degree), io_mutex, true, wal);
        if (!result.read()) {
            WARNING_PRINT("error: failed to read index node at addr %" PRIu64 "\n", addr);
            // Return an empty/null shared_node to indicate failure.
            // smart_infile_object default constructor creates a null state.
            return shared_node();
        }
        result.unmodify(); // constructor with obj& sets modified=true
        cache.put(addr, result);
        return result;
    } else {
        return node.value();
    }
}

shared_node node_reader::create_node(uint64_t addr) {
    auto result = shared_node(file, addr, new index_node(tree_degree), io_mutex, wal);
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

double node_reader::get_cache_hit_probability() const { return cache.get_hit_probability(); }

btree::btree(uint64_t degree, bool is_readonly, smart_infile_object<header> archive_header,
             block_allocator *allocator, FILE *file, uint64_t cache_size, std::mutex *io_mutex, compio::WalManager *wal)
    : degree(degree),
      is_readonly(is_readonly),
      archive_header(archive_header),
      allocator(allocator),
      reader(file, degree, cache_size, io_mutex, wal) {
    if (readonly(archive_header, header)->index_root != 0)
        return;

    shared_node root = create_node();
    archive_header->index_root = root.addr();
}

void btree::insert_nonfull(shared_node &node, const tree_key &key, const tree_val &value) {
    std::size_t idx = std::lower_bound(RO(node)->keys.begin(), RO(node)->keys.end(), key) -
                      RO(node)->keys.begin();
    if (RO(node)->is_leaf) {
        if (idx < RO(node)->num_keys && RO(node)->keys[idx] == key) {
            WARNING_PRINT("warning: trying to insert already existing key\n");
            return;
        }
        node->keys.insert(node->keys.begin() + idx, key);
        node->values.insert(node->values.begin() + idx, value);
        node->num_keys++;
        node->validate();
    } else {
        auto child = read_child(node, idx);
        if (!child) {
            WARNING_PRINT("error: failed to read child node during insert\n");
            return;
        }
        if (RO(child)->num_keys == (2 * degree - 1)) {
            split_child(node, child, idx);
            if (key > node->keys[idx]) {
                idx++;
                child = read_child(node, idx);
                if (!child) {
                    WARNING_PRINT("error: failed to read child node after split during insert\n");
                    return;
                }
            }
        }
        insert_nonfull(child, key, value);
    }
}

void btree::insert(const tree_key &key, const tree_val &value) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    DEBUG_PRINT("[BTREE]: insert(key={...,%" PRIu64 "},value={%" PRIu64 ",%" PRIu64 "})\n", key.pos, value.addr,
                value.size);
    auto root = read_root();
    if (!root) {
        WARNING_PRINT("error: failed to read root node for insert\n");
        return;
    }
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
            auto max_res = find_max_in_node(child);
            if (!max_res) {
                WARNING_PRINT("error: failed to find max in node during remove (corruption)\n");
                return;
            }
            const auto [p_key, p_val] = *max_res;
            node->keys[idx] = p_key;
            node->values[idx] = p_val;
            _remove(child, p_key);
        } else if (RO(successor)->num_keys >= degree) {
            auto min_res = find_min_in_node(successor);
            if (!min_res) {
                WARNING_PRINT("error: failed to find min in node during remove (corruption)\n");
                return;
            }
            const auto [s_key, s_val] = *min_res;
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
    std::unique_lock<std::shared_mutex> lock(mutex);
    DEBUG_PRINT("[BTREE]: remove(key={...,%" PRIu64 "})\n", key.pos);
    auto root = read_root();
    _remove(root, key);
    if (root->num_keys == 0 && !root->is_leaf) {
        archive_header->index_root = root->children[0];
        free_node(root);
    }
}

bool btree::_get_range(shared_node &node, const tree_key &key_min, const tree_key &key_max,
                       std::vector<std::pair<tree_key, tree_val>> &result) {
    if (RO(node)->num_keys == 0)
        return true;

    tree_key start{0, 0};
    tree_key end = RO(node)->keys[0];

    for (std::size_t i = 0; i <= RO(node)->num_keys; ++i) {
        if (!RO(node)->is_leaf && (key_min < end) && (key_max > start)) {
            auto child = read_child(node, i);
            if (!child) return false;
            if (!_get_range(child, key_min, key_max, result)) return false;
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
    return true;
}

std::optional<std::vector<std::pair<tree_key, tree_val>>> btree::get_range(const tree_key &key_min,
                                                            const tree_key &key_max) {
    std::shared_lock<std::shared_mutex> lock(mutex);
    return _get_range_impl(key_min, key_max);
}

std::optional<std::vector<std::pair<tree_key, tree_val>>> btree::_get_range_impl(const tree_key &key_min,
                                                                                 const tree_key &key_max) {
    if (key_max <= key_min) {
        WARNING_PRINT("warning: btree::get_range received invalid range bounds (key_min={%" PRIu64 ",%" PRIu64 "} "
                      ">= {%" PRIu64 ",%" PRIu64 "}=key_max)\n",
                      key_min.hash, key_min.pos, key_max.hash, key_max.pos);
        return std::vector<std::pair<tree_key, tree_val>>{};
    }
    std::vector<std::pair<tree_key, tree_val>> result;
    auto root = read_root();
    if (!root) return std::nullopt;
    
    if (!_get_range(root, key_min, key_max, result)) return std::nullopt;

    DEBUG_PRINT("[BTREE]: get_range(key_min={...,%" PRIu64 "},key_max={...,%" PRIu64 "}) ->\n", key_min.pos,
                key_max.pos);
    for (const auto &[key, val] : result) {
        (void)key; (void)val;
        DEBUG_PRINT("\t{...,%" PRIu64 "} -> {%" PRIu64 ",%" PRIu64 "}\n", key.pos, val.addr, val.size);
    }
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
                if (!child) return false;
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
        if (!child) return false;
        return _update(child, key, new_value);
    }
    return false;
}

void btree::update(const tree_key &key, const tree_val &new_value) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    _update_impl(key, new_value);
}

void btree::_update_impl(const tree_key &key, const tree_val &new_value) {
    DEBUG_PRINT("[BTREE]: update(key={...,%" PRIu64 "},new_value={%" PRIu64 ",%" PRIu64 "})\n", key.pos, new_value.addr,
                new_value.size);
    auto root = read_root();
    if (!root) {
        WARNING_PRINT("error: failed to read root node for update\n");
        return;
    }
    if (!_update(root, key, new_value)) {
        fprintf(stderr, "ERROR: trying to update non-existing key {%" PRIu64 ",%" PRIu64 "}\n", key.hash, key.pos);
        WARNING_PRINT("warning: trying to update non-existing key {%" PRIu64 ",%" PRIu64 "}\n", key.hash, key.pos);
    } else {
        // fprintf(stderr, "DEBUG: Updated key {%" PRIu64 ",%" PRIu64 "}\n", key.hash, key.pos);
    }
}

std::optional<tree_val> btree::get(const tree_key &key) {
    std::shared_lock<std::shared_mutex> lock(mutex);
    auto current = read_root();
    if (!current) return std::nullopt;

    while (true) {
        std::size_t idx =
            std::lower_bound(RO(current)->keys.begin(), RO(current)->keys.end(), key) -
            RO(current)->keys.begin();

        if (idx < RO(current)->num_keys && RO(current)->keys[idx] == key) {
            const auto val = RO(current)->values[idx];
            // DEBUG_PRINT("[BTREE]: get(key={...,%" PRIu64 "}) -> {%" PRIu64 ",%" PRIu64 "}\n", key.pos, val.addr, val.size);
            return val;
        } else if (!RO(current)->is_leaf) {
            current = read_child(current, idx);
            if (!current) return std::nullopt;
        } else {
            // DEBUG_PRINT("[BTREE]: get(key={...,%" PRIu64 "}) -> nullopt\n", key.pos);
            return std::nullopt;
        }
    }
}

std::optional<std::pair<tree_key, tree_val>> btree::get_block(const tree_key &key) {
    // get_range acquires shared_lock internally.
    auto range_opt = get_range(key, key + 1);
    if (!range_opt) return std::nullopt;
    const auto& range = *range_opt;

    assert(key.pos < UINT64_MAX);
    assert(range.size() < 2);
    if (!range.empty()) {
        return range[0];
    } else {
        return std::nullopt;
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
        if (child) _add_to_range(child, addition, key_min, key_max);
    }

    // iterate through keys, that are in range
    while (idx < RO(node)->num_keys && RO(node)->keys[idx] <= key_max) {
        node->keys[idx] += addition;
        if (!RO(node)->is_leaf) {
            // if next key is in range
            if (idx + 1 < RO(node)->num_keys && RO(node)->keys[idx + 1] <= key_max) {
                // then child #idx+1 is also in range
                node->key_additions[idx + 1] += addition;
            } else {
                // otherwise, child #idx+1 is partially in range
                auto child = read_child(node, idx + 1);
                if (child) _add_to_range(child, addition, key_min, key_max);
                break;
            }
        }
        ++idx;
    }

    RO(node)->validate();
}

void btree::add_to_range(int64_t addition, const tree_key &key_min, const tree_key &key_max) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    DEBUG_PRINT("[BTREE]: add_to_range(addition=%" PRId64 ",key_min={...,%" PRIu64 "},key_max={...,%" PRIu64 "})\n",
                addition, key_min.pos, key_max.pos);
    if (key_min > key_max) {
        WARNING_PRINT("warning: passed invalid range into btree::add_pos_to_keys_in_range "
                      "(key_min={...,%" PRIu64 "} > {...,%" PRIu64 "}=key_max)\n",
                      key_min.pos, key_max.pos);
        return;
    }

    auto root = read_root();
    if (root) _add_to_range(root, addition, key_min, key_max);
}

void btree::_print(shared_node node, uint64_t depth) {
    if (!node) return;
    for (std::size_t i = 0; i <= node->num_keys; ++i) {
        if (!node->is_leaf) {
            _print(read_child(node, i), depth + 1);
        }

        if (i < node->num_keys) {
            auto key = node->keys[i];
            auto val = node->values[i];
            UNUSED(key);
            UNUSED(val);
            DEBUG_PRINT("%s{%" PRIu64 ", %" PRIu64 "} -> {%" PRIu64 ", %" PRIu64 "}\n", std::string(depth * 2, ' ').c_str(),
                        key.hash, key.pos, val.addr, val.size);
        }
    }
}

void btree::print() {
    std::shared_lock<std::shared_mutex> lock(mutex);
    _print(read_root(), 0);
}

void btree::clear_cache() {
    std::unique_lock<std::shared_mutex> lock(mutex);
    _clear_cache();
}

void btree::_clear_cache() {
    reader.clear_cache();
}

double btree::get_cache_hit_probability() const {
    std::shared_lock<std::shared_mutex> lock(mutex);
    return reader.get_cache_hit_probability();
}

std::vector<uint64_t> btree::collect_node_addresses(uint64_t &node_size) {
    std::shared_lock<std::shared_mutex> lock(mutex);
    return _collect_node_addresses(node_size);
}

std::vector<uint64_t> btree::_collect_node_addresses(uint64_t &node_size) {
    node_size = INDEX_NODE_SIZE(degree);
    std::vector<uint64_t> addrs;

    uint64_t root_addr = readonly(archive_header, header)->index_root;
    if (root_addr == 0) return addrs;

    // BFS traversal of all nodes.
    std::vector<uint64_t> queue;
    queue.push_back(root_addr);

    while (!queue.empty()) {
        uint64_t addr = queue.back();
        queue.pop_back();
        addrs.push_back(addr);

        auto node = reader.read_node(addr);
        if (!RO(node)->is_leaf) {
            for (uint32_t i = 0; i <= RO(node)->num_keys; i++) {
                uint64_t child_addr = RO(node)->children[i];
                if (child_addr != 0) {
                    queue.push_back(child_addr);
                }
            }
        }
    }

    std::sort(addrs.begin(), addrs.end());
    return addrs;
}

void btree::split_child(shared_node &parent, shared_node &child, const uint64_t idx) {
    DEBUG_PRINT("[BTREE]: split_child(parent.addr=%" PRIu64 ", child.addr=%" PRIu64 ", idx=%zu)\n", parent.addr(), child.addr(), idx);
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
        if (predecessor && RO(predecessor)->num_keys >= degree) {
            borrow_from_prev(node, idx);
            return read_child(node, idx);
        }
    }

    if (idx < RO(node)->num_keys) {
        auto successor = read_child(node, idx + 1);
        if (successor && RO(successor)->num_keys >= degree) {
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

std::optional<std::pair<tree_key, tree_val>> btree::find_max_in_node(shared_node node) {
    if (!node) return std::nullopt;
    while (!RO(node)->is_leaf) {
        auto next = read_child(node, RO(node)->num_keys);
        if (!next) return std::nullopt;
        node = next;
    }
    return std::pair{RO(node)->keys.back(), RO(node)->values.back()};
}

std::optional<std::pair<tree_key, tree_val>> btree::find_min_in_node(shared_node node) {
    if (!node) return std::nullopt;
    while (!RO(node)->is_leaf) {
        auto next = read_child(node, 0);
        if (!next) return std::nullopt;
        node = next;
    }
    return std::pair{RO(node)->keys[0], RO(node)->values[0]};
}

uint64_t btree::allocate_node() { return allocator->allocate(INDEX_NODE_SIZE(degree)); }

void btree::free_node(const shared_node &node) {
    reader.remove_node(node);
    allocator->deallocate(node.addr(), INDEX_NODE_SIZE(degree));
}

shared_node btree::create_node() { return reader.create_node(allocate_node()); }

shared_node btree::read_node(uint64_t addr) {
    DEBUG_PRINT("[BTREE][read_node]: addr=%" PRIu64 "\n", addr);
    auto node = reader.read_node(addr);
    if (!node) {
        return node;
    }
    RO(node)->validate();
    return node;
}

shared_node btree::read_child(shared_node &node, uint64_t idx) {
    assert(RO(node)->is_leaf == false);
    assert(idx <= RO(node)->num_keys);
    auto child = read_node(RO(node)->children[idx]);
    if (!child) {
        return child;
    }
    const int64_t addition = RO(node)->key_additions[idx];
    if (addition != 0) {
        if (RO(child)->num_keys > 0) {
            for (auto &key : child->keys) {
                key += addition;
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

shared_node btree::find_node(const tree_key &key) {
    std::shared_lock<std::shared_mutex> lock(mutex);
    auto node = read_root();
    while (!RO(node)->is_leaf) {
        auto it = std::lower_bound(RO(node)->keys.begin(), RO(node)->keys.end(), key);
        if (it != RO(node)->keys.end() && *it == key) {
             return node; // Key found in internal node!
        }
        
        // key < *it. So child index is dist(begin, it).
        size_t i = std::distance(RO(node)->keys.begin(), it);
        node = read_child(node, i);
    }
    return node;
}
