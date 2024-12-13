#include "btree.hpp"
#include "utils.hpp"

#include <algorithm>
#include <limits>

using namespace compio;

uint64_t btree::allocate_node() { return allocate_block(archive, INDEX_NODE_SIZE(degree)); }

void btree::free_node(shared_node node) {
    node.remove();
    free_block(archive, node.addr(), INDEX_NODE_SIZE(degree));
}

shared_node btree::read_node(uint64_t addr) { return shared_node(archive->file, addr, degree); }

shared_node btree::create_node() {
    return shared_node(archive->file, allocate_node(), new index_node(degree));
}

shared_node btree::read_root() { return read_node(archive->header->index_root); }

btree::btree(compio_archive* archive) : archive(archive), degree(archive->config->b_tree_degree) {
    if (archive->header->index_root != 0)
        return;

    shared_node root = create_node();
    root.modify();
    archive->header->index_root = root.addr();
    flush_header(archive);
}

void btree::split_child(shared_node parent, shared_node child, int index) {
    auto new_node = create_node();

    new_node->is_leaf = child->is_leaf;
    new_node->num_keys = degree - 1;
    for (size_t j = 0; j < degree - 1; j++) {
        new_node->keys[j] = child->keys[j + degree];
        new_node->values[j] = child->values[j + degree];
    }
    if (!child->is_leaf) {
        for (size_t j = 0; j < degree; j++) {
            new_node->children[j] = child->children[j + degree];
        }
    }
    child->num_keys = degree - 1;
    for (size_t j = parent->num_keys; j > index; j--) {
        parent->children[j + 1] = parent->children[j];
    }
    parent->children[index + 1] = new_node.addr();
    for (size_t j = parent->num_keys; j > index; j--) {
        parent->keys[j] = parent->keys[j - 1];
        parent->values[j] = parent->values[j - 1];
    }
    parent->keys[index] = child->keys[degree - 1];
    parent->values[index] = child->values[degree - 1];
    parent->num_keys++;
}

void btree::merge_children(shared_node parent, int idx) {
    auto child = read_node(parent->children[idx]);
    auto sibling = read_node(parent->children[idx + 1]);

    child->keys[degree - 1] = parent->keys[idx];
    for (size_t i = 0; i < sibling->num_keys; ++i) {
        child->keys[i + degree] = sibling->keys[i];
        child->values[i + degree] = sibling->values[i];
    }
    if (!child->is_leaf) {
        for (size_t i = 0; i <= sibling->num_keys; ++i) {
            child->children[i + degree] = sibling->children[i];
        }
    }
    for (size_t i = idx + 1; i < parent->num_keys; ++i) {
        parent->keys[i - 1] = parent->keys[i];
        parent->values[i - 1] = parent->values[i];
        parent->children[i] = parent->children[i + 1];
    }
    child->num_keys += sibling->num_keys + 1;
    parent->num_keys--;

    free_node(sibling);
}

void btree::insert_nonfull(shared_node node, tree_key key, tree_val value) {
    size_t i = node->num_keys;
    if (node->is_leaf) {
        while (i > 0 && key < node->keys[i - 1]) {
            node->keys[i] = node->keys[i - 1];
            node->values[i] = node->values[i - 1];
            i--;
        }
        node->keys[i] = key;
        node->values[i] = value;
        node->num_keys++;
    } else {
        while (i > 0 && key < node->keys[i - 1]) {
            i--;
        }
        auto child = read_node(node->children[i]);
        if (RO(child)->num_keys == (2 * degree - 1)) {
            split_child(node, child, i);
            if (key > node->keys[i]) {
                i++;
            }
        }
        insert_nonfull(child, key, value);
    }
}

void btree::borrow_from_prev(shared_node parent, int idx) {
    auto child = read_node(parent->children[idx]);
    auto sibling = read_node(parent->children[idx - 1]);

    for (size_t i = child->num_keys; i > 0; --i) {
        child->keys[i] = child->keys[i - 1];
        child->values[i] = child->values[i - 1];
    }
    if (!child->is_leaf) {
        for (size_t i = child->num_keys + 1; i > 0; --i) {
            child->children[i] = child->children[i - 1];
        }
    }
    child->keys[0] = parent->keys[idx - 1];
    if (!child->is_leaf) {
        child->children[0] = sibling->children[sibling->num_keys];
    }
    parent->keys[idx - 1] = sibling->keys[sibling->num_keys - 1];
    child->values[0] = sibling->values[sibling->num_keys - 1];
    child->num_keys++;
    sibling->num_keys--;
}

void btree::borrow_from_next(shared_node parent, int idx) {
    auto child = read_node(parent->children[idx]);
    auto sibling = read_node(parent->children[idx + 1]);

    child->keys[child->num_keys] = parent->keys[idx];
    child->values[child->num_keys] = parent->values[idx];
    if (!child->is_leaf) {
        child->children[child->num_keys + 1] = sibling->children[0];
    }

    parent->keys[idx] = sibling->keys[0];
    parent->values[idx] = sibling->values[0];

    for (size_t i = 1; i < sibling->num_keys; ++i) {
        sibling->keys[i - 1] = sibling->keys[i];
        sibling->values[i - 1] = sibling->values[i];
    }
    if (!sibling->is_leaf) {
        for (size_t i = 1; i <= sibling->num_keys; ++i) {
            sibling->children[i - 1] = sibling->children[i];
        }
    }

    child->num_keys++;
    sibling->num_keys--;
}

tree_key btree::find_max_in_node(shared_node node) {
    auto current = node;
    while (!RO(current)->is_leaf) {
        current = read_node(RO(current)->children[RO(current)->num_keys]);
    }
    return RO(current)->keys[current->num_keys - 1];
}

tree_key btree::find_min_in_node(shared_node node) {
    auto current = node;
    while (!RO(current)->is_leaf) {
        current = read_node(RO(current)->children[0]);
    }
    return RO(current)->keys[0];
}

void btree::insert(tree_key key, tree_val value) {
    auto root = read_root();
    if (RO(root)->num_keys == (2 * degree - 1)) {
        auto new_root = create_node();
        new_root->is_leaf = false;
        new_root->children[0] = root.addr();

        split_child(new_root, root, 0);
        insert_nonfull(new_root, key, value);
        archive->header->index_root = new_root.addr();
        flush_header(archive);
    } else {
        insert_nonfull(root, key, value);
    }
}

void btree::remove_node(shared_node node, tree_key key) {
    size_t idx = 0;
    while (idx < RO(node)->num_keys && key > RO(node)->keys[idx]) {
        idx++;
    }

    if (idx < RO(node)->num_keys && key == RO(node)->keys[idx]) {
        if (RO(node)->is_leaf) {
            for (size_t i = idx + 1; i < RO(node)->num_keys; i++) {
                node->keys[i - 1] = RO(node)->keys[i];
                node->values[i - 1] = RO(node)->values[i];
            }
            node->num_keys--;
        } else {
            auto child = read_node(RO(node)->children[idx]);
            auto successor = read_node(RO(node)->children[idx + 1]);
            if (child->num_keys >= degree) {
                size_t predecessor_key = find_max_in_node(child);
                node->keys[idx] = predecessor_key;
                remove_node(child, predecessor_key);
            } else if (RO(successor)->num_keys >= degree) {
                size_t successor_key = find_min_in_node(successor);
                node->keys[idx] = successor_key;
                remove_node(successor, successor_key);
            } else {
                merge_children(node, idx);
                remove_node(child, key);
            }
        }
    } else {
        if (RO(node)->is_leaf)
            return;
        bool flag = (idx == RO(node)->num_keys);
        auto child = read_node(RO(node)->children[idx]);
        if (RO(child)->num_keys < degree) {
            auto predecessor = read_node(RO(node)->children[idx - 1]);
            auto successor = read_node(RO(node)->children[idx + 1]);
            if (idx != 0 && RO(predecessor)->num_keys >= degree) {
                borrow_from_prev(node, idx);
            } else if (idx != RO(node)->num_keys && RO(successor)->num_keys >= degree) {
                borrow_from_next(node, idx);
            } else {
                if (idx != RO(node)->num_keys) {
                    merge_children(node, idx);
                } else {
                    merge_children(node, idx - 1);
                }
            }
        }

        child = read_node(RO(node)->children[idx]);
        auto predecessor = read_node(RO(node)->children[idx - 1]);
        if (flag && idx > RO(node)->num_keys) {
            remove_node(predecessor, key);
        } else {
            remove_node(child, key);
        }
    }
}

void btree::remove(tree_key key) {
    auto root = read_root();
    remove_node(root, key);
    if (RO(root)->num_keys == 0) {
        if (!RO(root)->is_leaf) {
            archive->header->index_root = RO(root)->children[0];
            flush_header(archive);
            free_node(root);
        }
    }
}

void btree::get_range(tree_key key_min, tree_key key_max, std::vector<uint64_t>& result) {
    if (key_max <= key_min)
        return;
    get_range_in_node(read_root(), key_min, key_max, result);
}

void btree::get_range_in_node(shared_node node, tree_key key_min, tree_key key_max,
                              std::vector<tree_key>& result) {
    int num_keys = RO(node)->num_keys;
    if (num_keys == 0)
        return;
    bool is_leaf = RO(node)->is_leaf;

    auto start = std::numeric_limits<tree_key>::min();
    auto end = RO(node)->keys[0];
    for (int i = 0; i <= num_keys; ++i) {
        if (!is_leaf) {
            if (key_min <= end && key_max > start) {
                auto child = read_node(RO(node)->children[i]);
                get_range_in_node(child, key_min, key_max, result);
            }
        }
        if (i != num_keys) {
            start = RO(node)->keys[i];
            end = start + RO(node)->values[i].size;
            if (key_min <= end && key_max > start)
                result.push_back(start);
            start = end;
            end = (i < num_keys - 1) ? RO(node)->keys[i + 1] : std::numeric_limits<tree_key>::max();
        }
    }
}

bool btree::update(tree_key key, tree_val new_value) {
    return update_in_node(read_root(), key, new_value);
}

bool btree::update_in_node(shared_node node, tree_key key, tree_val new_value) {
    auto num_keys = RO(node)->num_keys;
    auto is_leaf = RO(node)->is_leaf;
    for (int i = 0; i < num_keys; ++i) {
        auto current_key = RO(node)->keys[i];
        if (current_key >= key) {
            if (current_key == key) {
                node->values[i] = new_value;
                return true;
            }
            if (!is_leaf)
                return update_in_node(read_node(RO(node)->children[i]), key, new_value);
        }
    }
    if (!is_leaf)
        return update_in_node(read_node(RO(node)->children[num_keys]), key, new_value);
    return false;
}