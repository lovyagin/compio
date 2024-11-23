#include "btree.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Helper function declarations
void btree_insert_non_full(BTreeNode* node, size_t key, void* value, size_t degree);
void btree_split_child(BTreeNode* parent, size_t index, BTreeNode* child, size_t degree);
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
        i++;
        if (node->children[i - 1]->num_keys == (2 * degree - 1)) {
            btree_split_child(node, i - 1, node->children[i - 1], degree);
            if (key > node->keys[i - 1]) {
                i++;
            }
        }
        btree_insert_non_full(node->children[i - 1], key, value, degree);
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
    // Implementation of deletion logic with handling of merging and redistribution
    // Placeholder for actual logic, which is complex and involves multiple cases
    printf("Deletion of key %zu is not fully implemented yet.\n", key);
}

size_t btree_find_min(BTree* tree) {
    if (tree == NULL || tree->root == NULL) return 0;
    BTreeNode* current = tree->root;
    while (!current->is_leaf) {
        current = current->children[0];
    }
    return current->keys[0];
}

size_t btree_find_max(BTree* tree) {
    if (tree == NULL || tree->root == NULL) return 0;
    BTreeNode* current = tree->root;
    while (!current->is_leaf) {
        current = current->children[current->num_keys];
    }
    return current->keys[current->num_keys - 1];
}
