#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "btree.h"

// Create a new B-Tree Node
BTreeNode* btree_create_node(size_t degree, bool is_leaf) {
    BTreeNode* node = (BTreeNode*)malloc(sizeof(BTreeNode));
    if (!node) {
        fprintf(stderr, "Error: Memory allocation failed for new node.\n");
        return NULL;
    }

    node->keys = (size_t*)malloc((2 * degree - 1) * sizeof(size_t));
    node->values = (void**)malloc((2 * degree - 1) * sizeof(void*));
    node->children = (BTreeNode**)malloc((2 * degree) * sizeof(BTreeNode*));
    node->num_keys = 0;
    node->is_leaf = is_leaf;

    if (!node->keys || !node->values || (!is_leaf && !node->children)) {
        fprintf(stderr, "Error: Memory allocation failed for node components.\n");
        free(node->keys);
        free(node->values);
        free(node->children);
        free(node);
        return NULL;
    }

    return node;
}

// Create a new B-Tree
BTree* btree_create(size_t degree) {
    BTree* tree = (BTree*)malloc(sizeof(BTree));
    if (!tree) {
        fprintf(stderr, "Error: Memory allocation failed for B-tree.\n");
        return NULL;
    }

    tree->degree = degree;
    tree->root = btree_create_node(degree, true);
    if (!tree->root) {
        free(tree);
        return NULL;
    }

    return tree;
}

// Insert a key into the B-Tree
void btree_insert(BTree* tree, size_t key, void* value) {
    BTreeNode* root = tree->root;

    // If the root is full, create a new node and increase tree height
    if (root->num_keys == 2 * tree->degree - 1) {
        BTreeNode* new_root = btree_create_node(tree->degree, false);
        if (!new_root) {
            return;
        }

        new_root->children[0] = root;
        btree_split_child(new_root, 0, root, tree->degree);
        tree->root = new_root;
    }

    btree_insert_non_full(tree->root, key, value, tree->degree);
}

// Insert a key into a non-full node
void btree_insert_non_full(BTreeNode* node, size_t key, void* value, size_t degree) {
    size_t i = node->num_keys;

    if (node->is_leaf) {
        // Insert into a leaf node
        while (i > 0 && key < node->keys[i - 1]) {
            node->keys[i] = node->keys[i - 1];
            node->values[i] = node->values[i - 1];
            i--;
        }

        node->keys[i] = key;
        node->values[i] = value;
        node->num_keys++;
        printf("Inserted key %zu in leaf node.\n", key);
    } else {
        // Find the child to insert into
        while (i > 0 && key < node->keys[i - 1]) {
            i--;
        }

        if (node->children[i]->num_keys == 2 * degree - 1) {
            btree_split_child(node, i, node->children[i], degree);
            if (key > node->keys[i]) {
                i++;
            }
        }

        btree_insert_non_full(node->children[i], key, value, degree);
    }
}

// Split a full child node
void btree_split_child(BTreeNode* parent, size_t index, BTreeNode* child, size_t degree) {
    BTreeNode* new_child = btree_create_node(degree, child->is_leaf);
    if (!new_child) {
        return;
    }

    new_child->num_keys = degree - 1;

    // Copy keys and values from the child to the new node
    for (size_t j = 0; j < degree - 1; j++) {
        new_child->keys[j] = child->keys[j + degree];
        new_child->values[j] = child->values[j + degree];
    }

    // Copy the children from the child to the new node (if not a leaf)
    if (!child->is_leaf) {
        for (size_t j = 0; j < degree; j++) {
            new_child->children[j] = child->children[j + degree];
        }
    }

    child->num_keys = degree - 1;

    // Shift children and keys in the parent node to make space for the new child
    for (size_t j = parent->num_keys; j > index; j--) {
        parent->children[j + 1] = parent->children[j];
        parent->keys[j] = parent->keys[j - 1];
    }

    parent->children[index + 1] = new_child;
    parent->keys[index] = child->keys[degree - 1];
    parent->num_keys++;

    printf("Split child node at index %zu. Promoted key: %zu.\n", index, child->keys[degree - 1]);
}

// Free a B-Tree node
void btree_free_node(BTreeNode* node) {
    if (!node) return;

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

// Free a B-Tree
void btree_free(BTree* tree) {
    if (!tree) return;

    btree_free_node(tree->root);
    free(tree);
}
