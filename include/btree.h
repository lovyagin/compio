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
    void** values;                /**< Array of associated values (e.g., pointers to data blocks) */
    struct BTreeNode** children;  /**< Array of pointers to child nodes */
    size_t num_keys;              /**< Number of keys in the node */
    bool is_leaf;                 /**< Indicates if the node is a leaf */
} BTreeNode;



/**
 * @struct BTree
 * @brief Represents the B-Tree structure.
 */
typedef struct {
    BTreeNode* root;              /**< Pointer to the root node of the tree */
    size_t degree;                /**< Minimum degree (t) of the tree */
} BTree;



/**
 * @brief Creates a new B-Tree with the specified degree.
 *
 * @param degree Minimum degree (t) of the B-Tree.
 * @return Pointer to the created B-Tree, or NULL if memory allocation fails.
 */
BTree* btree_create(size_t degree);



/**
 * @brief Searches for a key in the B-Tree.
 *
 * @param node Pointer to the node where the search starts.
 * @param key The key to search for.
 * @return Pointer to the associated value if found, or NULL otherwise.
 */
void* btree_search(BTreeNode* node, size_t key);



/**
 * @brief Inserts a key and value into the B-Tree.
 *
 * @param tree Pointer to the B-Tree.
 * @param key The key to insert.
 * @param value The value associated with the key.
 */
void btree_insert(BTree* tree, size_t key, void* value);



/**
 * @brief Frees the memory associated with the B-Tree.
 *
 * @param tree Pointer to the B-Tree to be freed.
 */
void btree_free(BTree* tree);



/**
 * @brief Splits a child node in the B-Tree.
 *
 * @param parent Pointer to the parent node.
 * @param index Index of the child to be split.
 * @param child Pointer to the child node to be split.
 * @param degree Minimum degree (t) of the tree.
 */
void btree_split_child(BTreeNode* parent, size_t index, BTreeNode* child, size_t degree);



/**
 * @brief Frees the memory associated with a single node and its children.
 *
 * @param node Pointer to the node to be freed.
 */
void btree_free_node(BTreeNode* node);



/**
 * @brief Frees the memory associated with the B-Tree.
 *
 * @param tree Pointer to the B-Tree to be freed.
 */
void btree_free(BTree* tree);



/**
 * @brief Removes a key-value pair from the B-Tree.
 *
 * @param tree Pointer to the B-Tree.
 * @param key The key to remove.
 * @return 0 if the key was successfully removed, -1 if the key was not found.
 */
int btree_remove(BTree* tree, size_t key);

#endif /* BTREE_H */
