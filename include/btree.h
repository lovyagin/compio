/**
 * @internal
 * @file btree.h
 * @brief Internal header for B-Tree implementation.
 *
 * This file contains structures and functions for the B-Tree data structure,
 * which is used for indexing and efficient data retrieval in the compression library.
 */

#ifndef BTREE_H
#define BTREE_H

#include <stddef.h>
#include <stdbool.h>

/**
 * @internal
 * @struct BTreeNode
 * @brief Represents a node in the B-Tree.
 */
typedef struct BTreeNode {
    size_t* keys;
    void** values;
    struct BTreeNode** children;
    size_t num_keys;
    bool is_leaf;
} BTreeNode;

/**
 * @internal
 * @struct BTree
 * @brief Represents the B-Tree structure.
 */
typedef struct BTree {
    BTreeNode* root;
    size_t degree;
} BTree;

/**
 * @brief Creates a new B-Tree with the specified degree.
 *
 * @param degree Minimum degree (t) of the B-Tree.
 * @return Pointer to the created B-Tree, or NULL if memory allocation fails.
 */
BTree* btree_create(size_t degree);

/**
 * @brief Frees the memory associated with the B-Tree.
 *
 * @param tree Pointer to the B-Tree to be freed.
 */
void btree_free(BTree* tree);

/**
 * @brief Inserts a key and value into the B-Tree.
 *
 * @param tree Pointer to the B-Tree.
 * @param key The key to insert.
 * @param value The value associated with the key.
 */
void btree_insert(BTree* tree, size_t key, void* value);

/**
 * @brief Searches for a key in the B-Tree.
 *
 * @param tree Pointer to the B-Tree.
 * @param key The key to search for.
 * @return Pointer to the B-Tree node containing the key, or NULL if not found.
 */
BTreeNode* btree_search(BTree* tree, size_t key);

/**
 * @brief Deletes a key from the B-Tree.
 *
 * @param tree Pointer to the B-Tree.
 * @param key The key to delete.
 */
void btree_delete(BTree* tree, size_t key);

/**
 * @brief Searches for the minimum key in the B-Tree.
 *
 * @param tree Pointer to the B-Tree.
 * @return The minimum key in the B-Tree, or 0 if the tree is empty.
 */
size_t btree_find_min(BTree* tree);

/**
 * @brief Searches for the maximum key in the B-Tree.
 *
 * @param tree Pointer to the B-Tree.
 * @return The maximum key in the B-Tree, or 0 if the tree is empty.
 */
size_t btree_find_max(BTree* tree);

/**
 * @brief Updates the value associated with a given key in the B-Tree.
 *
 * @param tree Pointer to the B-Tree.
 * @param key The key to update.
 * @param new_value The new value to associate with the key.
 * @return 0 if the update is successful, or -1 if the key is not found.
 */
int btree_update(BTree* tree, size_t key, void* new_value);

/**
 * @internal
 * @brief Creates a new B-Tree node.
 *
 * @param degree Minimum degree (t) of the B-Tree.
 * @param is_leaf Indicates if the node is a leaf.
 * @return Pointer to the created B-Tree node, or NULL if memory allocation fails.
 */
BTreeNode* btree_create_node(size_t degree, bool is_leaf);

/**
 * @internal
 * @brief Frees the memory associated with a B-Tree node.
 *
 * @param node Pointer to the node to be freed.
 */
void btree_free_node(BTreeNode* node);

/**
 * @internal
 * @brief Inserts a key into a non-full node.
 *
 * @param node Pointer to the node where the key should be inserted.
 * @param key The key to insert.
 * @param value The value associated with the key.
 * @param degree Minimum degree (t) of the B-Tree.
 */
void btree_insert_non_full(BTreeNode* node, size_t key, void* value, size_t degree);

/**
 * @internal
 * @brief Splits a full child node.
 *
 * @param parent Pointer to the parent node.
 * @param index Index of the child to be split.
 * @param child Pointer to the child node to be split.
 * @param degree Minimum degree (t) of the B-Tree.
 */
void btree_split_child(BTreeNode* parent, size_t index, BTreeNode* child, size_t degree);

/**
 * @internal
 * @brief Merges two children of a B-Tree node.
 *
 * @param parent Pointer to the parent node.
 * @param idx Index of the child to be merged with its sibling.
 * @param degree Minimum degree (t) of the B-Tree.
 */
void btree_merge_children(BTreeNode* parent, size_t idx, size_t degree);

/**
 * @internal
 * @brief Borrows a key from the previous sibling of a child node.
 *
 * @param parent Pointer to the parent node.
 * @param idx Index of the child node to borrow a key for.
 */
void btree_borrow_from_prev(BTreeNode* parent, size_t idx);

/**
 * @internal
 * @brief Borrows a key from the next sibling of a child node.
 *
 * @param parent Pointer to the parent node.
 * @param idx Index of the child node to borrow a key for.
 */
void btree_borrow_from_next(BTreeNode* parent, size_t idx);

/**
 * @internal
 * @brief Finds the maximum key in a given B-Tree node.
 *
 * @param node Pointer to the B-Tree node.
 * @return The maximum key in the node.
 */
size_t btree_find_max_in_node(BTreeNode* node);

/**
 * @internal
 * @brief Finds the minimum key in a given B-Tree node.
 *
 * @param node Pointer to the B-Tree node.
 * @return The minimum key in the node.
 */
size_t btree_find_min_in_node(BTreeNode* node);

#endif // BTREE_H
