#include "btree.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Helper function declarations
BTreeNode* btree_search_node(BTreeNode* node, size_t key);
void btree_delete_node(BTreeNode* node, size_t key, size_t degree);

BTree* btree_create(size_t degree) {
    BTree* tree = (BTree*)malloc(sizeof(BTree));
    if (tree == NULL) {
        fprintf(stderr, "Error: Could not allocate memory for B-Tree.\n");
        return NULL;
    }
    tree->degree = degree;
    tree->root = btree_create_node(degree, true);
    if (tree->root == NULL) {
        free(tree);
        return NULL;
    }
    return tree;
}

void btree_free(BTree* tree) {
    if (tree == NULL) return;
    btree_free_node(tree->root);
    free(tree);
}

BTreeNode* btree_create_node(size_t degree, bool is_leaf) {
    BTreeNode* node = (BTreeNode*)malloc(sizeof(BTreeNode));
    if (node == NULL) {
        fprintf(stderr, "Error: Could not allocate memory for B-Tree node.\n");
        return NULL;
    }
    node->keys = (size_t*)malloc((2 * degree - 1) * sizeof(size_t));
    node->values = (void**)malloc((2 * degree - 1) * sizeof(void*));
    node->children = (BTreeNode**)malloc((2 * degree) * sizeof(BTreeNode*));
    node->num_keys = 0;
    node->is_leaf = is_leaf;
    return node;
}

void btree_free_node(BTreeNode* node) {
    if (node == NULL) return;
    if (!node->is_leaf) {
        for (size_t i = 0; i <= node->num_keys; i++) {
            btree_free_node(node->children[i]);
        }
    }
    free(node->keys);
    free(node->values);
    free(node->children);
    free(node);
}

void btree_insert(BTree* tree, size_t key, void* value) {
    if (tree == NULL) return;
    BTreeNode* root = tree->root;
    if (root->num_keys == (2 * tree->degree - 1)) {
        BTreeNode* new_root = btree_create_node(tree->degree, false);
        if (new_root == NULL) return;
        new_root->children[0] = root;
        btree_split_child(new_root, 0, root, tree->degree);
        tree->root = new_root;
        btree_insert_non_full(new_root, key, value, tree->degree);
    } else {
        btree_insert_non_full(root, key, value, tree->degree);
    }
}

void btree_insert_non_full(BTreeNode* node, size_t key, void* value, size_t degree) {
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
        if (node->children[i]->num_keys == (2 * degree - 1)) {
            btree_split_child(node, i, node->children[i], degree);
            if (key > node->keys[i]) {
                i++;
            }
        }
        btree_insert_non_full(node->children[i], key, value, degree);
    }
}

void btree_split_child(BTreeNode* parent, size_t index, BTreeNode* child, size_t degree) {
    BTreeNode* new_node = btree_create_node(degree, child->is_leaf);
    if (new_node == NULL) return;
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
    parent->children[index + 1] = new_node;
    for (size_t j = parent->num_keys; j > index; j--) {
        parent->keys[j] = parent->keys[j - 1];
        parent->values[j] = parent->values[j - 1];
    }
    parent->keys[index] = child->keys[degree - 1];
    parent->values[index] = child->values[degree - 1];
    parent->num_keys++;
}

BTreeNode* btree_search(BTree* tree, size_t key) {
    if (tree == NULL) return NULL;
    return btree_search_node(tree->root, key);
}

BTreeNode* btree_search_node(BTreeNode* node, size_t key) {
    size_t i = 0;
    while (i < node->num_keys && key > node->keys[i]) {
        i++;
    }
    if (i < node->num_keys && key == node->keys[i]) {
        return node;
    }
    if (node->is_leaf) {
        return NULL;
    }
    return btree_search_node(node->children[i], key);
}

void btree_delete(BTree* tree, size_t key) {
    if (tree == NULL || tree->root == NULL) return;
    btree_delete_node(tree->root, key, tree->degree);
    if (tree->root->num_keys == 0) {
        BTreeNode* old_root = tree->root;
        if (!tree->root->is_leaf) {
            tree->root = tree->root->children[0];
        } else {
            tree->root = NULL;
        }
        btree_free_node(old_root);
    }
}

void btree_delete_node(BTreeNode* node, size_t key, size_t degree) {
    size_t idx = 0;
    while (idx < node->num_keys && key > node->keys[idx]) {
        idx++;
    }

    if (idx < node->num_keys && key == node->keys[idx]) {
        if (node->is_leaf) {
            for (size_t i = idx + 1; i < node->num_keys; i++) {
                node->keys[i - 1] = node->keys[i];
                node->values[i - 1] = node->values[i];
            }
            node->num_keys--;
        } else {
            if (node->children[idx]->num_keys >= degree) {
                size_t predecessor_key = btree_find_max_in_node(node->children[idx]);
                node->keys[idx] = predecessor_key;
                btree_delete_node(node->children[idx], predecessor_key, degree);
            } else if (node->children[idx + 1]->num_keys >= degree) {
                size_t successor_key = btree_find_min_in_node(node->children[idx + 1]);
                node->keys[idx] = successor_key;
                btree_delete_node(node->children[idx + 1], successor_key, degree);
            } else {
                btree_merge_children(node, idx, degree);
                btree_delete_node(node->children[idx], key, degree);
            }
        }
    } else {
        if (node->is_leaf) {
            printf("Key %zu not found in leaf node.\n", key);
            return;
        }
        bool flag = (idx == node->num_keys);
        if (node->children[idx]->num_keys < degree) {
            if (idx != 0 && node->children[idx - 1]->num_keys >= degree) {
                btree_borrow_from_prev(node, idx);
            } else if (idx != node->num_keys && node->children[idx + 1]->num_keys >= degree) {
                btree_borrow_from_next(node, idx);
            } else {
                if (idx != node->num_keys) {
                    btree_merge_children(node, idx, degree);
                } else {
                    btree_merge_children(node, idx - 1, degree);
                }
            }
        }

        if (flag && idx > node->num_keys) {
            btree_delete_node(node->children[idx - 1], key, degree);
        } else {
            btree_delete_node(node->children[idx], key, degree);
        }
    }
}

void btree_merge_children(BTreeNode* parent, size_t idx, size_t degree) {
    BTreeNode* child = parent->children[idx];
    BTreeNode* sibling = parent->children[idx + 1];
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
    free(sibling->keys);
    free(sibling->values);
    free(sibling->children);
    free(sibling);
}

void btree_borrow_from_prev(BTreeNode* parent, size_t idx) {
    BTreeNode* child = parent->children[idx];
    BTreeNode* sibling = parent->children[idx - 1];

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

void btree_borrow_from_next(BTreeNode* parent, size_t idx) {
    BTreeNode* child = parent->children[idx];
    BTreeNode* sibling = parent->children[idx + 1];

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

size_t btree_find_max_in_node(BTreeNode* node) {
    BTreeNode* current = node;
    while (!current->is_leaf) {
        current = current->children[current->num_keys];
    }
    return current->keys[current->num_keys - 1];
}

size_t btree_find_min_in_node(BTreeNode* node) {
    BTreeNode* current = node;
    while (!current->is_leaf) {
        current = current->children[0];
    }
    return current->keys[0];
}
