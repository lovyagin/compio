#include "compio/btree.hpp"

#include <algorithm>
#include <limits>
#include <cassert>
#include <optional>

#include "compio/allocator.hpp"
#include "compio/compio_file.hpp"
#include "compio/debug_print.hpp"

using namespace compio;

#define RO(x) readonly(x, index_node)

node_reader::node_reader(FILE *file, int tree_degree, int max_size)
    : tree_degree(tree_degree),
      file(file),
      cache(max_size) {}

shared_node node_reader::read_node(uint64_t addr) {
    if (!cache.exists(addr)) {
        // cache miss

        // TODO: new smart_infile_object constructor for this type of case (infile_object without
        // default constructor)
        auto result = shared_node(file, addr, new index_node(tree_degree));
        result.read();     // read from file (because constructor with obj& does not read)
        result.unmodify(); // constructor with obj& sets modified=true

        cache.put(addr, result);

        return result;
    } else {
        // cache hit
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

uint64_t btree::allocate_node() const {
    return archive->allocator->allocate(INDEX_NODE_SIZE(degree));
}

void btree::free_node(const shared_node &node) {
    reader.remove_node(node);
    archive->allocator->deallocate(node.addr(), INDEX_NODE_SIZE(degree));
}

shared_node btree::read_node(uint64_t addr) {
    auto node = reader.read_node(addr);
    RO(node)->validate();
    return node;
}

shared_node btree::read_child(shared_node &node, uint64_t idx) {
    assert(RO(node)->is_leaf == false);
    RO(node)->validate();
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
    if (archive->is_readonly()) {
        // key_additions will be applied, but not written to file
        node.unmodify();
        child.unmodify();
    }
    return child;
}

shared_node btree::create_node() { return reader.create_node(allocate_node()); }

shared_node btree::read_root() { return read_node(readonly(archive->header, header)->index_root); }

btree::btree(compio_archive *archive_)
    : degree(archive_->config->b_tree_degree),
      archive(archive_),
      reader(archive_->file, degree, archive_->config->cache_size__nodes) {
    if (readonly(archive_->header, header)->index_root != 0)
        return;

    shared_node root = create_node();
    archive_->header->index_root = root.addr();
}

void btree::split_child(shared_node &parent, shared_node &child, const int index) {
    parent->validate();
    auto new_node = create_node();

    new_node->is_leaf = child->is_leaf;
    new_node->num_keys = degree - 1;
    new_node->keys.insert(new_node->keys.end(), child->keys.begin() + degree, child->keys.end());
    new_node->values.insert(new_node->values.end(), child->values.begin() + degree, child->values.end());
    // for (uint64_t j = 0; j < degree - 1; j++) {
    //     new_node->keys[j] = child->keys[j + degree];
    //     new_node->values[j] = child->values[j + degree];
    // }
    if (!child->is_leaf) {
        new_node->children.resize(degree);
        new_node->key_additions.resize(degree);
        std::copy(child->children.begin() + degree, child->children.end(), new_node->children.begin());
        std::copy(child->key_additions.begin() + degree, child->key_additions.end(), new_node->key_additions.begin());
        // for (uint64_t j = 0; j < degree; j++) {
        //     new_node->children[j] = child->children[j + degree];
        // }
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

    // for (long j = parent->num_keys; j > index; j--) {
    //     parent->children[j + 1] = parent->children[j];
    // }
    // parent->children[index + 1] = new_node.addr();
    // for (long j = parent->num_keys; j > index; j--) {
    //     parent->keys[j] = parent->keys[j - 1];
    //     parent->values[j] = parent->values[j - 1];
    // }
    // parent->keys[index] = child->keys[degree - 1];
    // parent->values[index] = child->values[degree - 1];
    parent->children.insert(parent->children.begin() + index + 1, new_node.addr());
    parent->key_additions.insert(parent->key_additions.begin() + index + 1, 0);
    parent->keys.insert(parent->keys.begin() + index, middle_key);
    parent->values.insert(parent->values.begin() + index, middle_value);
    parent->num_keys++;
    parent->validate();
}

void btree::merge_children(shared_node &parent, const int idx) {
    parent->validate();
    auto child = read_child(parent, idx);
    auto sibling = read_child(parent, idx + 1);

    // child->keys[degree - 1] = parent->keys[idx];
    // for (size_t i = 0; i < sibling->num_keys; ++i) {
    //     child->keys[i + degree] = sibling->keys[i];
    //     child->values[i + degree] = sibling->values[i];
    // }
    child->num_keys += sibling->num_keys + 1;
    child->keys.push_back(parent->keys[idx]);
    child->values.push_back(parent->values[idx]);
    child->keys.insert(child->keys.end(), sibling->keys.begin(), sibling->keys.end());
    child->values.insert(child->values.end(), sibling->values.begin(), sibling->values.end());
    if (!child->is_leaf) {
        // for (size_t i = 0; i <= sibling->num_keys; ++i) {
        //     child->children[i + degree] = sibling->children[i];
        // }
        child->children.insert(child->children.end(), sibling->children.begin(), sibling->children.end());
        child->key_additions.insert(child->key_additions.end(), sibling->key_additions.begin(), sibling->key_additions.end());
    }
    child->validate();

    // for (size_t i = idx + 1; i < parent->num_keys; ++i) {
    //     parent->keys[i - 1] = parent->keys[i];
    //     parent->values[i - 1] = parent->values[i];
    //     parent->children[i] = parent->children[i + 1];
    // }
    parent->keys.erase(parent->keys.begin() + idx);
    parent->values.erase(parent->values.begin() + idx);
    parent->children.erase(parent->children.begin() + idx + 1);
    parent->key_additions.erase(parent->key_additions.begin() + idx + 1);
    parent->num_keys--;
    parent->validate();

    free_node(sibling);
}

void btree::insert_nonfull(shared_node &node, const tree_key &key, const tree_val &value) {
    RO(node)->validate();
    size_t i = RO(node)->num_keys;
    if (RO(node)->is_leaf) {
        node->keys.resize(node->keys.size() + 1);
        node->values.resize(node->values.size() + 1);
        while (i > 0 && key < node->keys[i - 1]) {
            node->keys[i] = node->keys[i - 1];
            node->values[i] = node->values[i - 1];
            i--;
        }
        node->keys[i] = key;
        node->values[i] = value;
        node->num_keys++;
        node->validate();
    } else {
        while (i > 0 && key < RO(node)->keys[i - 1]) {
            i--;
        }
        auto child = read_child(node, i);
        if (RO(child)->num_keys == (2 * degree - 1)) {
            split_child(node, child, i);
            if (key > node->keys[i]) {
                i++;
                child = read_child(node, i);
            }
        }
        insert_nonfull(child, key, value);
        RO(node)->validate();
    }
}

void btree::borrow_from_prev(shared_node &parent, const int idx) {
    parent->validate();
    auto child = read_child(parent, idx);
    auto sibling = read_child(parent, idx - 1);
    assert(child->is_leaf == sibling->is_leaf);
    
    // for (size_t i = child->num_keys; i > 0; --i) {
    //     child->keys[i] = child->keys[i - 1];
    //     child->values[i] = child->values[i - 1];
    // }
    child->keys.insert(child->keys.begin(), parent->keys[idx - 1]);
    child->values.insert(child->values.begin(), parent->values[idx - 1]);
    if (!child->is_leaf) {
        // for (size_t i = child->num_keys + 1; i > 0; --i) {
        //     child->children[i] = child->children[i - 1];
        // }
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
    // child->keys[0] = parent->keys[idx - 1];
    // if (!child->is_leaf) {
    //     child->children[0] = sibling->children[sibling->num_keys];
    // }
    // parent->keys[idx - 1] = sibling->keys[sibling->num_keys - 1];
    // child->values[0] = sibling->values[sibling->num_keys - 1];
}

void btree::borrow_from_next(shared_node &parent, const int idx) {
    parent->validate();
    auto child = read_child(parent, idx);
    auto sibling = read_child(parent, idx + 1);
    assert(child->is_leaf == sibling->is_leaf);

    // child->keys[child->num_keys] = parent->keys[idx];
    // child->values[child->num_keys] = parent->values[idx];
    child->keys.push_back(parent->keys[idx]);
    child->values.push_back(parent->values[idx]);
    if (!child->is_leaf) {
        // child->children[child->num_keys + 1] = sibling->children[0];
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
    // for (size_t i = 1; i < sibling->num_keys; ++i) {
    //     sibling->keys[i - 1] = sibling->keys[i];
    //     sibling->values[i - 1] = sibling->values[i];
    // }
    // if (!sibling->is_leaf) {
    //     for (size_t i = 1; i <= sibling->num_keys; ++i) {
    //         sibling->children[i - 1] = sibling->children[i];
    //     }
    // }

}

std::pair<tree_key, tree_val> btree::find_max_in_node(const shared_node &node) {
    RO(node)->validate();
    auto current = node;
    while (!RO(current)->is_leaf) {
        current = read_child(current, RO(current)->num_keys);
    }
    return {RO(current)->keys.back(), RO(current)->values.back()};
}

std::pair<tree_key, tree_val> btree::find_min_in_node(const shared_node &node) {
    RO(node)->validate();
    auto current = node;
    while (!RO(current)->is_leaf) {
        current = read_child(current, 0);
    }
    return {RO(current)->keys[0], RO(current)->values[0]};
}

void btree::insert(const tree_key &key, const tree_val &value) {
    auto root = read_root();
    if (RO(root)->num_keys == (2 * degree - 1)) {
        auto new_root = create_node();
        new_root->is_leaf = false;
        new_root->children.resize(1);
        new_root->key_additions.resize(1);
        new_root->children[0] = root.addr();
        new_root->key_additions[0] = 0;

        split_child(new_root, root, 0);
        insert_nonfull(new_root, key, value);
        new_root->validate();
        archive->header->index_root = new_root.addr();
    } else {
        insert_nonfull(root, key, value);
    }
}

void btree::remove_node(shared_node &node, const tree_key &key) {
    RO(node)->validate();
    // TODO: use upper_bound
    size_t idx = 0;
    while (idx < RO(node)->num_keys && key > RO(node)->keys[idx]) {
        idx++;
    }

    if (idx < RO(node)->num_keys && key == RO(node)->keys[idx]) {
        if (RO(node)->is_leaf) {
            // for (size_t i = idx + 1; i < RO(node)->num_keys; i++) {
            //     node->keys[i - 1] = RO(node)->keys[i];
            //     node->values[i - 1] = RO(node)->values[i];
            // }
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
                remove_node(child, p_key);
            } else if (RO(successor)->num_keys >= degree) {
                const auto [s_key, s_val] = find_min_in_node(successor);
                node->keys[idx] = s_key;
                node->values[idx] = s_val;
                remove_node(successor, s_key);
            } else {
                merge_children(node, idx);
                remove_node(child, key);
            }
        }
    } else {
        if (RO(node)->is_leaf) {
            WARNING_PRINT("warning: called btree::remove() with non-existent key\n");
            return;
        }
        auto child = read_child(node, idx);
        if (RO(child)->num_keys < degree) {
            // Check if we can borrow from predecessor (only if idx > 0)
            if (idx != 0) {
                const auto predecessor = read_child(node, idx - 1);
                if (RO(predecessor)->num_keys >= degree) {
                    borrow_from_prev(node, idx);
                    child = read_child(node, idx);
                    remove_node(child, key);
                    return;
                }
            }

            // Check if we can borrow from successor (only if idx < num_keys)
            if (idx != RO(node)->num_keys) {
                const auto successor = read_child(node, idx + 1);
                if (RO(successor)->num_keys >= degree) {
                    borrow_from_next(node, idx);
                    child = read_child(node, idx);
                    remove_node(child, key);
                    return;
                }
            }

            // Need to merge
            if (idx != RO(node)->num_keys) {
                merge_children(node, idx);
            } else if (idx > 0) {
                // Only merge with previous child if idx > 0
                merge_children(node, idx - 1);
            } else {
                // idx == 0 and idx == num_keys means this is the only child
                // This shouldn't happen in a valid B-tree, but handle gracefully
                WARNING_PRINT("warning: trying to remove key from empty node in b-tree::remove_node\n");
                return;
            }
        }

        // After potential merge, recalculate child position
        if (idx > RO(node)->num_keys) {
            idx = RO(node)->num_keys;
        }

        child = read_child(node, idx);
        remove_node(child, key);
    }
}

void btree::remove(const tree_key &key) {
    auto root = read_root();
    remove_node(root, key);
    if (RO(root)->num_keys == 0) {
        if (!RO(root)->is_leaf) {
            archive->header->index_root = RO(root)->children[0];
            free_node(root);
        }
    }
}

void btree::get_range(const tree_key &key_min, const tree_key &key_max,
                      std::vector<std::pair<tree_key, tree_val>> &result) {
    if (key_max <= key_min)
        return;
    auto root = read_root();
    get_range_in_node(root, key_min, key_max, result);
}

void btree::get_range_in_node(shared_node &node, const tree_key &key_min,
                              const tree_key &key_max,
                              std::vector<std::pair<tree_key, tree_val>> &result) {
    const int num_keys = RO(node)->num_keys;
    if (num_keys == 0)
        return;
    const bool is_leaf = RO(node)->is_leaf;

    tree_key start{0, 0};
    tree_key end = RO(node)->keys[0];

    for (int i = 0; i <= num_keys; ++i) {
        if (!is_leaf && (key_min < end) && (key_max > start)) {
            auto child = read_child(node, i);
            get_range_in_node(child, key_min, key_max, result);
        }

        if (i < num_keys) {
            start = RO(node)->keys[i];
            end.pos = start.pos + RO(node)->values[i].size;
            end.hash = start.hash;

            if ((key_min < end) && (key_max > start)) {
                result.emplace_back(start, RO(node)->values[i]);
            }
            start = end;
            end = (i < num_keys - 1) ? RO(node)->keys[i + 1] : tree_key{UINT64_MAX, UINT64_MAX};
        }
    }
}

void btree::add_to_range_in_node(shared_node &node, int64_t value, const tree_key &lower_bound,
                                 const tree_key &upper_bound) {
    RO(node)->validate();
    const int num_keys = RO(node)->num_keys;
    if (num_keys == 0)
        return;
    const bool is_leaf = RO(node)->is_leaf;

    int idx = 0;

    // skip keys that are before lower_bound
    while (idx < num_keys && RO(node)->keys[idx] < lower_bound) {
        ++idx;
    }

    if (!is_leaf) {
        // this child is in range
        auto child = read_child(node, idx);
        add_to_range_in_node(child, value, lower_bound, upper_bound);
    }

    // iterate through keys, that are in range
    while (idx < num_keys && RO(node)->keys[idx] <= upper_bound) {
        node->keys[idx] = node->keys[idx] + value;
        if (!is_leaf) {
            // if next key is in range
            if (idx + 1 < num_keys && RO(node)->keys[idx + 1] <= upper_bound) {
                // then child #idx+1 is also in range
                node->key_additions[idx + 1] += value;
            } else {
                // otherwise, child #idx+1 is partially in range
                auto child = read_child(node, idx + 1);
                add_to_range_in_node(child, value, lower_bound, upper_bound);
                break;
            }
        }
        ++idx;
    }

    RO(node)->validate();
}

void btree::add_to_range(int64_t value, const tree_key &lower_bound, const tree_key &upper_bound) {
    if (lower_bound > upper_bound) {
        WARNING_PRINT("warning: passed invalid range into btree::add_to_range "
                      "(lower_bound={%lu,%lu} > {%lu,%lu}=upper_bound)\n",
                      lower_bound.hash, lower_bound.pos, upper_bound.hash, upper_bound.pos);
        return;
    }

    auto root = read_root();
    add_to_range_in_node(root, value, lower_bound, upper_bound);
}

bool btree::update(const tree_key &key, const tree_val &new_value) {
    auto root = read_root();
    return update_in_node(root, key, new_value);
}

std::optional<tree_val> btree::get(const tree_key &key) {
    // TODO: rewrite this function accurately (one keys traverse)
    auto current = read_root();
    
    while (true) {
        RO(current)->validate();
        
        // If we found the key in current node, return its address
        for (uint32_t i = 0; i < RO(current)->num_keys; ++i) {
            if (RO(current)->keys[i] == key) {
                return RO(current)->values[i];
            }
        }
        
        // If this is a leaf node and we haven't found the key, return 0
        if (RO(current)->is_leaf) {
            return std::nullopt;
        }
        
        // Find the appropriate child to search
        uint32_t child_index = 0;
        while (child_index < RO(current)->num_keys && key > RO(current)->keys[child_index]) {
            child_index++;
        }
        
        // Read the child node and continue search
        current = read_child(current, child_index);
    }
}

bool btree::update_in_node(shared_node &node, const tree_key &key, const tree_val &new_value) {
    const auto num_keys = RO(node)->num_keys;
    const auto is_leaf = RO(node)->is_leaf;
    for (uint64_t i = 0; i < num_keys; ++i) {
        auto current_key = RO(node)->keys[i];
        if (current_key >= key) {
            if (current_key == key) {
                node->values[i] = new_value;
                return true;
            }
            if (!is_leaf) {
                auto child = read_child(node, i);
                return update_in_node(child, key, new_value);
            } else {
                return false;
            }
        } else if (key < current_key + RO(node)->values[i].size) {
            return false;
        }
    }
    if (!is_leaf) {
        auto child = read_child(node, num_keys);
        return update_in_node(child, key, new_value);
    }
    return false;
}

static void print_btree_(btree *tree, shared_node node, int depth = 0) {
    for (std::size_t i = 0; i <= node->num_keys; ++i) {
        if (!node->is_leaf) {
            print_btree_(tree, tree->read_child(node, i), depth + 1);
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

void btree::print_btree() { print_btree_(this, read_root()); }