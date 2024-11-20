#include "btree.h"
#include <stdbool.h>
#include "stdlib.h"
#include "string.h"



BTree* btree_create(size_t degree) {
    BTree* tree = malloc(sizeof(BTree));
    if (!tree) return NULL;

    tree->degree = degree;
    tree->root = malloc(sizeof(BTreeNode));
    if (!tree->root) {
        free(tree);
        return NULL;
    }

    tree->root->keys = malloc((2 * degree - 1) * sizeof(size_t));
    tree->root->values = malloc((2 * degree - 1) * sizeof(void*));
    tree->root->children = malloc((2 * degree) * sizeof(BTreeNode*));
    tree->root->num_keys = 0;
    tree->root->is_leaf = true;

    return tree;
}



void* btree_search(BTreeNode* node, size_t key) {
    while (node != NULL) {
        size_t i = 0;

        // Find the first key greater than or equal to the search key
        while (i < node->num_keys && key > node->keys[i]) {
            i++;
        }

        // If the key is found, return the associated value
        if (i < node->num_keys && key == node->keys[i]) {
            return node->values[i];
        }

        // If the node is a leaf, the key is not present
        if (node->is_leaf) {
            return NULL;
        }

        // Move to the next child
        node = node->children[i];
    }

    return NULL;  // Key not found
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

        if (node->children[i]->num_keys == 2 * degree - 1) {
            btree_split_child(node, i, node->children[i], degree);

            if (key > node->keys[i]) {
                i++;
            }
        }

        btree_insert_non_full(node->children[i], key, value, degree);
    }
}



void btree_insert(BTree* tree, size_t key, void* value) {
    BTreeNode* root = tree->root;

    if (root->num_keys == 2 * tree->degree - 1) {
        BTreeNode* new_root = malloc(sizeof(BTreeNode));
        new_root->keys = malloc((2 * tree->degree - 1) * sizeof(size_t));
        new_root->values = malloc((2 * tree->degree - 1) * sizeof(void*));
        new_root->children = malloc((2 * tree->degree) * sizeof(BTreeNode*));
        new_root->is_leaf = false;
        new_root->num_keys = 0;
        new_root->children[0] = root;

        btree_split_child(new_root, 0, root, tree->degree);

        tree->root = new_root;
        btree_insert_non_full(new_root, key, value, tree->degree);
    } else {
        btree_insert_non_full(root, key, value, tree->degree);
    }
}


void btree_split_child(BTreeNode* parent, size_t index, BTreeNode* child, size_t degree) {
    BTreeNode* new_child = malloc(sizeof(BTreeNode));
    new_child->is_leaf = child->is_leaf;
    new_child->num_keys = degree - 1;

    new_child->keys = malloc((2 * degree - 1) * sizeof(size_t));
    memcpy(new_child->keys, &child->keys[degree], (degree - 1) * sizeof(size_t));

    new_child->values = malloc((2 * degree - 1) * sizeof(void*));
    memcpy(new_child->values, &child->values[degree], (degree - 1) * sizeof(void*));

    if (!child->is_leaf) {
        new_child->children = malloc((2 * degree) * sizeof(BTreeNode*));
        memcpy(new_child->children, &child->children[degree], degree * sizeof(BTreeNode*));
    }

    child->num_keys = degree - 1;

    for (size_t i = parent->num_keys; i > index; i--) {
        parent->children[i + 1] = parent->children[i];
        parent->keys[i] = parent->keys[i - 1];
    }

    parent->children[index + 1] = new_child;
    parent->keys[index] = child->keys[degree - 1];
    parent->num_keys++;
}



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



void btree_free(BTree* tree) {
    if (!tree) return;

    btree_free_node(tree->root);
    free(tree);
}