#ifndef BTREE_H
#define BTREE_H

#include <stddef.h>
#include <stdbool.h>

/**
 * @struct BTreeNode
 * @brief Represents a node in the B-Tree.
 */
typedef struct BTreeNode {
    size_t* keys;                 /**< Array of keys in the node */
    void** values;                /**< Array of values associated with keys */
    struct BTreeNode** children;  /**< Array of pointers to child nodes */
    size_t num_keys;              /**< Number of keys in the node */
    bool is_leaf;                 /**< Indicates if the node is a leaf node */
} BTreeNode;

/**
 * @struct BTree
 * @brief Represents the B-Tree structure.
 */
typedef struct BTree {
    BTreeNode* root;              /**< Pointer to the root node of the tree */
    size_t degree;                /**< Minimum degree of the B-Tree */
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
 * @brief Creates a new B-Tree node.
 *
 * @param degree Minimum degree (t) of the B-Tree.
 * @param is_leaf Indicates if the node is a leaf.
 * @return Pointer to the created B-Tree node, or NULL if memory allocation fails.
 */
BTreeNode* btree_create_node(size_t degree, bool is_leaf);

/**
 * @brief Frees the memory associated with a B-Tree node.
 *
 * @param node Pointer to the node to be freed.
 */
void btree_free_node(BTreeNode* node);

/**
 * @brief Inserts a key and value into the B-Tree.
 *
 * @param tree Pointer to the B-Tree.
 * @param key The key to insert.
 * @param value The value associated with the key.
 */
void btree_insert(BTree* tree, size_t key, void* value);

/**
 * @brief Inserts a key into a non-full node.
 *
 * @param node Pointer to the node where the key should be inserted.
 * @param key The key to insert.
 * @param value The value associated with the key.
 * @param degree Minimum degree (t) of the B-Tree.
 */
void btree_insert_non_full(BTreeNode* node, size_t key, void* value, size_t degree);

/**
 * @brief Splits a full child node.
 *
 * @param parent Pointer to the parent node.
 * @param index Index of the child to be split.
 * @param child Pointer to the child node to be split.
 * @param degree Minimum degree (t) of the B-Tree.
 */
void btree_split_child(BTreeNode* parent, size_t index, BTreeNode* child, size_t degree);

/**
 * @brief Deletes a key from the B-Tree.
 *
 * @param tree Pointer to the B-Tree.
 * @param key The key to delete.
 */
void btree_delete(BTree* tree, size_t key);

/**
 * @brief Searches for a key in the B-Tree.
 *
 * @param tree Pointer to the B-Tree.
 * @param key The key to search for.
 * @return Pointer to the B-Tree node containing the key, or NULL if not found.
 */
BTreeNode* btree_search(BTree* tree, size_t key);

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

#endif // BTREE_H
