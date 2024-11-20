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



/* Additional functions for btree_remove */
static void btree_merge_children(BTreeNode* parent, size_t index, size_t degree);
static void btree_shift_left(BTreeNode* parent, size_t index);
static void btree_shift_right(BTreeNode* parent, size_t index);
static int btree_remove_from_node(BTreeNode* node, size_t key, size_t degree);

int btree_remove(BTree* tree, size_t key) {
    if (!tree || !tree->root) return -1;

    BTreeNode* root = tree->root;

    // Remove the key from the root
    int result = btree_remove_from_node(root, key, tree->degree);

    // Root is empty and is not a leaf: promote the first child as the new root
    if (root->num_keys == 0 && !root->is_leaf) {
        tree->root = root->children[0];
        free(root);
    } else if (root->num_keys == 0) {
        // Root is empty and is a leaf: the tree becomes empty
        free(root);
        tree->root = NULL;
    }

    return result;
}

static int btree_remove_from_node(BTreeNode* node, size_t key, size_t degree) {
    size_t idx = 0;

    // Find the key index
    while (idx < node->num_keys && key > node->keys[idx]) {
        idx++;
    }

    if (idx < node->num_keys && key == node->keys[idx]) {
        // Case 1: Key is found in the node
        if (node->is_leaf) {
            // Simple case: remove from a leaf
            for (size_t i = idx; i < node->num_keys - 1; i++) {
                node->keys[i] = node->keys[i + 1];
                node->values[i] = node->values[i + 1];
            }
            node->num_keys--;
            return 0;
        } else {
            // Complex case: remove from an internal node
            if (node->children[idx]->num_keys >= degree) {
                // 1a. Replace with predecessor
                BTreeNode* pred = node->children[idx];
                while (!pred->is_leaf) {
                    pred = pred->children[pred->num_keys];
                }
                size_t pred_key = pred->keys[pred->num_keys - 1];
                void* pred_value = pred->values[pred->num_keys - 1];

                node->keys[idx] = pred_key;
                node->values[idx] = pred_value;

                return btree_remove_from_node(node->children[idx], pred_key, degree);
            } else if (node->children[idx + 1]->num_keys >= degree) {
                // 1b. Replace with successor
                BTreeNode* succ = node->children[idx + 1];
                while (!succ->is_leaf) {
                    succ = succ->children[0];
                }
                size_t succ_key = succ->keys[0];
                void* succ_value = succ->values[0];

                node->keys[idx] = succ_key;
                node->values[idx] = succ_value;

                return btree_remove_from_node(node->children[idx + 1], succ_key, degree);
            } else {
                // 1c. Merge two children
                btree_merge_children(node, idx, degree);
                return btree_remove_from_node(node->children[idx], key, degree);
            }
        }
    }

    // Case 2: Key is not in this node
    if (node->is_leaf) return -1; // Key not found

    // Ensure the child has enough keys before descending
    if (node->children[idx]->num_keys < degree) {
        if (idx > 0 && node->children[idx - 1]->num_keys >= degree) {
            btree_shift_right(node, idx); // Borrow a key from the left sibling
        } else if (idx < node->num_keys && node->children[idx + 1]->num_keys >= degree) {
            btree_shift_left(node, idx); // Borrow a key from the right sibling
        } else {
            // Merge the child with a sibling
            if (idx < node->num_keys) {
                btree_merge_children(node, idx, degree);
            } else {
                btree_merge_children(node, idx - 1, degree);
            }
        }
    }

    return btree_remove_from_node(node->children[idx], key, degree);
}

static void btree_merge_children(BTreeNode* parent, size_t index, size_t degree) {
    BTreeNode* left = parent->children[index];
    BTreeNode* right = parent->children[index + 1];

    // Move the key from the parent into the left node
    left->keys[left->num_keys] = parent->keys[index];
    left->values[left->num_keys] = parent->values[index];
    left->num_keys++;

    // Move all keys and children from the right node into the left node
    for (size_t i = 0; i < right->num_keys; i++) {
        left->keys[left->num_keys + i] = right->keys[i];
        left->values[left->num_keys + i] = right->values[i];
    }
    if (!right->is_leaf) {
        for (size_t i = 0; i <= right->num_keys; i++) {
            left->children[left->num_keys + i] = right->children[i];
        }
    }
    left->num_keys += right->num_keys;

    // Remove the right node reference from the parent
    for (size_t i = index; i < parent->num_keys - 1; i++) {
        parent->keys[i] = parent->keys[i + 1];
        parent->values[i] = parent->values[i + 1];
        parent->children[i + 1] = parent->children[i + 2];
    }
    parent->num_keys--;

    btree_free_node(right);
}

static void btree_shift_left(BTreeNode* parent, size_t index) {
    BTreeNode* child = parent->children[index];
    BTreeNode* sibling = parent->children[index + 1];

    child->keys[child->num_keys] = parent->keys[index];
    child->values[child->num_keys] = parent->values[index];
    child->num_keys++;

    parent->keys[index] = sibling->keys[0];
    parent->values[index] = sibling->values[0];

    for (size_t i = 0; i < sibling->num_keys - 1; i++) {
        sibling->keys[i] = sibling->keys[i + 1];
        sibling->values[i] = sibling->values[i + 1];
    }

    if (!sibling->is_leaf) {
        for (size_t i = 0; i < sibling->num_keys; i++) {
            sibling->children[i] = sibling->children[i + 1];
        }
    }
    sibling->num_keys--;
}

static void btree_shift_right(BTreeNode* parent, size_t index) {
    BTreeNode* child = parent->children[index];
    BTreeNode* sibling = parent->children[index - 1];

    for (size_t i = child->num_keys; i > 0; i--) {
        child->keys[i] = child->keys[i - 1];
        child->values[i] = child->values[i - 1];
    }

    child->keys[0] = parent->keys[index - 1];
    child->values[0] = parent->values[index - 1];
    if (!child->is_leaf) {
        for (size_t i = child->num_keys + 1; i > 0; i--) {
            child->children[i] = child->children[i - 1];
        }
        child->children[0] = sibling->children[sibling->num_keys];
    }
    child->num_keys++;

    parent->keys[index - 1] = sibling->keys[sibling->num_keys - 1];
    parent->values[index - 1] = sibling->values[sibling->num_keys - 1];
    sibling->num_keys--;
}
