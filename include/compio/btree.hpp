/**
 * @internal
 * @file btree.hpp
 * @brief Internal header for B-Tree implementation.
 *
 * This file contains structures and functions for the B-Tree data structure,
 * which is used for indexing and efficient data retrieval in the compression library.
 */

#ifndef BTREE_H
#define BTREE_H

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "compio/allocator.hpp"
#include "compio/file.hpp"

#include "third_party/lrucache.hpp"

namespace compio {

class WalManager;

using shared_node = smart_infile_object<index_node>;

/**
 * @brief Struct for reading nodes from memory with caching
 *
 * Provides cached access to B-Tree nodes stored in file, using LRU cache
 * to improve performance by avoiding repeated file I/O operations.
 */
struct node_reader {
    /**
     * @brief Construct a new node reader object
     *
     * @param file Pointer to the file handle for reading/writing nodes
     * @param tree_degree The degree of the B-Tree (branching factor)
     * @param max_size Maximum number of nodes to cache
     * @param io_mutex Mutex for I/O operations
     * @param wal WAL manager
     */
    node_reader(FILE *file, uint64_t tree_degree, uint64_t max_size, std::mutex *io_mutex, compio::WalManager *wal = nullptr);

    /**
     * @brief Read a node from the specified address
     *
     * Retrieves a node from the given file address, using cache if available.
     * If not cached, reads from file and adds to cache.
     *
     * @param addr File address of the node to read
     * @return shared_node Smart pointer to the read node
     */
    shared_node read_node(uint64_t addr);

    /**
     * @brief Create a new node at the specified address
     *
     * Creates a new empty node at the given file address and adds it to cache.
     *
     * @param addr File address where the node should be created
     * @return shared_node Smart pointer to the created node
     */
    shared_node create_node(uint64_t addr);

    /**
     * @brief Remove a node from the cache
     *
     * Removes the specified node from the LRU cache. The node remains
     * valid until all smart_infile_object references are destroyed.
     *
     * @param node Smart pointer to the node to remove from cache
     */
    void remove_node(const shared_node &node);

    /**
     * @brief Clear all nodes from the cache
     *
     * Empties the LRU cache, removing all cached nodes.
     */
    void clear_cache();

    /**
     * @brief Get cache hit probability
     * 
     * @return double Cache hit probability
     */
    double get_cache_hit_probability() const;

private:
    /** @brief The degree of the B-Tree (branching factor) */
    uint64_t tree_degree;
    /** @brief File handle for reading/writing nodes */
    FILE *file;
    std::mutex *io_mutex;
    compio::WalManager *wal;
    /** @brief LRU cache for storing frequently accessed nodes */
    cache::lru_cache<uint64_t, shared_node> cache;
};

/**
 * @brief B-Tree, that stores addresses of compressed blocks in
 * archive file, with access by their start position. B-Tree nodes
 * are stored in archive file.
 *
 * Provides efficient key-value storage and retrieval for compressed block
 * addresses within archive files. Supports standard B-Tree operations including
 * insertion, deletion, range queries, and key updates.
 */
struct btree {
    /**
     * @brief Construct a new btree object
     *
     * Initializes a B-Tree. Creates root node if archive is new (archive_header->index_root == 0).
     *
     *
     * @param degree The B-Tree degree (branching factor)
     * @param is_readonly Is compio_archive opened with readonly (should we propagate key_additions
     * and save them to file or not)
     * @param archive_header Smart pointer to header to set and get root address
     * @param allocator Pointer to allocator to allocate memory for index_nodes
     * @param file File handle for reading/writing B-Tree nodes
     * @param cache_size Maximum number of nodes to cache for performance optimization
     * @param io_mutex Mutex for I/O operations
     * @param wal WAL manager
     */
    btree(uint64_t degree, bool is_readonly, smart_infile_object<header> archive_header,
          block_allocator *allocator, FILE *file, uint64_t cache_size, std::mutex *io_mutex, compio::WalManager *wal = nullptr);

    /**
     * @brief Insert a key-value pair into the B-Tree
     *
     * Inserts the specified key and value into the B-Tree. If the key
     * already exists, prints a warning and does not overwrite.
     *
     * @param key The key to insert
     * @param value The value associated with the key
     */
    void insert(const tree_key &key, const tree_val &value);

    /**
     * @brief Remove a key-value pair from the B-Tree
     *
     * Removes the specified key and its associated value from the B-Tree.
     * If the key doesn't exist, prints a warning.
     *
     * @param key The key to remove
     */
    void remove(const tree_key &key);

    /**
     * @brief Get all key-value pairs, whose blocks intersect specified range
     *
     * Retrieves all key-value pairs where block [key, key + val.size) intersect range [key_min,
     * key_max)
     *
     * @param key_min Minimum key (inclusive)
     * @param key_max Maximum key (exclusive)
     * @return std::optional<std::vector<std::pair<tree_key, tree_val>>> result Vector with resulting key-value
     * pairs, or nullopt on read failure
     */
    std::optional<std::vector<std::pair<tree_key, tree_val>>> get_range(const tree_key &key_min,
                                                         const tree_key &key_max);

    /**
     * @brief Update the value associated with an existing key
     *
     * Updates the value for the specified key. If the key doesn't exist,
     * prints a warning and no update is performed.
     *
     * @param key The key whose value should be updated
     * @param new_value The new value to associate with the key
     */
    void update(const tree_key &key, const tree_val &new_value);

    /**
     * @brief Get the value associated with a specific key
     *
     * Retrieves the value for the specified key. If the key doesn't exist,
     * returns std::nullopt.
     *
     * @param key The key to look up
     * @return std::optional<tree_val> The associated value, or nullopt if not found
     */
    std::optional<tree_val> get(const tree_key &key);

    /**
     * @brief Get a key-value pair, whose block contains specified key
     *
     * Finds key-value pair, such that block [key, key + size) contains specified key
     *
     * @param key The key to look up
     * @return std::optional<std::pair<tree_key, tree_val>> key-value pair, or nullopt if not found
     */
    std::optional<std::pair<tree_key, tree_val>> get_block(const tree_key &key);

    /**
     * @brief Add a value to .pos field of all keys within the specified range
     *
     * Adds the specified addition value to .pos field of all keys that fall within
     * the range [key_min, key_max].
     *
     * @param addition The value to add to each key in range
     * @param key_min Minimum key (inclusive)
     * @param key_max Maximum key (inclusive)
     */
    void add_to_range(int64_t addition, const tree_key &key_min, const tree_key &key_max);

    /**
     * @brief Print the B-Tree structure for debugging
     *
     * Outputs the entire B-Tree structure to debug output, showing
     * the hierarchical relationship between nodes and their key-value pairs.
     */
    void print();

    /**
     * @brief Clear the node cache
     *
     * Clears all cached nodes, forcing subsequent operations to read
     * from file. Useful for memory management or consistency operations.
     */
    void clear_cache();

    /**
     * @brief Collect file addresses of all B-tree nodes
     *
     * Traverses the tree and returns the file address of every node.
     * Used by defragmentation to avoid overwriting B-tree data when
     * compacting storage blocks.
     *
     * @param node_size Output: the fixed byte size of each node
     * @return std::vector<uint64_t> Sorted vector of node addresses
     */
    std::vector<uint64_t> collect_node_addresses(uint64_t &node_size);

    /**
     * @brief Get a lock object for the B-Tree mutex
     * 
     * @return std::unique_lock<std::mutex> Lock object
     */
    std::unique_lock<std::mutex> get_lock() const { return std::unique_lock<std::mutex>(mutex); }

    /**
     * @brief Get cache hit probability
     * 
     * @return double Cache hit probability
     */
    double get_cache_hit_probability() const;

private:
    mutable std::mutex mutex;
    /** @brief The degree of the B-Tree (branching factor) */
    uint64_t degree;
    /** @brief Is compio_archive opened with readonly (should we propagate key_additions and save
     * them to file or not) */
    bool is_readonly;
    /** @brief Smart pointer to header to set and get root address */
    smart_infile_object<header> archive_header;
    /** @brief Pointer to allocator to allocate memory for index_nodes */
    block_allocator *allocator;
    /** @brief Node reader for cached file access */
    node_reader reader;

    /**
     * @brief Insert into a node that is guaranteed not to be full
     *
     * Recursive helper for inserting into a non-full node. Handles
     * both leaf and internal node cases.
     *
     * @param node The node to insert into (must not be full)
     * @param key The key to insert
     * @param value The value to insert
     */
    void insert_nonfull(shared_node &node, const tree_key &key, const tree_val &value);

    /**
     * @brief Split a full child node
     *
     * Splits a full child node into two nodes, moving the middle key
     * up to the parent. Used during insertion when nodes become full.
     *
     * @param parent The parent node containing the child to split
     * @param child The full child node to split
     * @param index The index of the child in the parent's children array
     */
    void split_child(shared_node &parent, shared_node &child, uint64_t idx);

    /**
     * @brief Merge two sibling child nodes
     *
     * Merges two adjacent child nodes along with the separating key
     * from the parent. Used during deletion when nodes become too small.
     *
     * @param parent The parent node containing the children to merge
     * @param idx The index of the first child to merge (merges idx and idx+1)
     */
    void merge_children(shared_node &parent, uint64_t idx);

    /**
     * @brief Borrow a key from the previous sibling
     *
     * Borrows a key-value pair from the left sibling to maintain
     * B-Tree properties during deletion operations.
     *
     * @param parent The parent node containing the children
     * @param idx The index of the child that needs to borrow
     */
    void borrow_from_prev(shared_node &parent, uint64_t idx);

    /**
     * @brief Borrow a key from the next sibling
     *
     * Borrows a key-value pair from the right sibling to maintain
     * B-Tree properties during deletion operations.
     *
     * @param parent The parent node containing the children
     * @param idx The index of the child that needs to borrow
     */
    void borrow_from_next(shared_node &parent, uint64_t idx);

    /**
     * @brief Populate child with keys either by borrowing or merging
     *
     * Tries to populate child by borrowing from predecessor, then from successor, otherwise merges
     * with one of them
     *
     * @param node The parent node containing child
     * @param idx Index of child to populate
     * @return shared_node Smart pointer to populated child, or nullptr if it's the only child
     */
    shared_node populate_child(shared_node &node, uint64_t idx);

    /**
     * @brief Find the maximum key-value pair in a subtree
     *
     * Traverses to the rightmost leaf to find the maximum key
     * and its associated value in the given subtree.
     *
     * @param node The root of the subtree to search
     * @return std::optional<std::pair<tree_key, tree_val>> The maximum key-value pair
     */
    std::optional<std::pair<tree_key, tree_val>> find_max_in_node(shared_node node);

    /**
     * @brief Find the minimum key-value pair in a subtree
     *
     * Traverses to the leftmost leaf to find the minimum key
     * and its associated value in the given subtree.
     *
     * @param node The root of the subtree to search
     * @return std::optional<std::pair<tree_key, tree_val>> The minimum key-value pair
     */
    std::optional<std::pair<tree_key, tree_val>> find_min_in_node(shared_node node);

    /**
     * @brief Helper for removing a key, that exists in current node
     *
     * Removes key from current node if it's leaf, otherwise takes neighbouring new_key from one of
     * two children, and recursively removes new_key from that child
     *
     * @param node The current node being processed
     * @param idx Index of key to remove
     */
    void _remove_in_node(shared_node &node, uint64_t idx);

    /**
     * @brief Recursive helper for removing a key
     *
     * Recursively removes the specified key from the B-Tree,
     * handling all necessary rebalancing operations.
     *
     * @param node The current node being processed
     * @param key The key to remove
     */
    void _remove(shared_node &node, const tree_key &key);

    /**
     * @brief Recursive helper for range queries
     *
     * Recursively collects all key-value pairs within the specified range
     * from the subtree rooted at the given node.
     *
     * @param node The current node being processed
     * @param key_min Minimum key (inclusive)
     * @param key_max Maximum key (exclusive)
     * @param result Vector to store the resulting key-value pairs
     * @return true on success, false on read failure
     */
    bool _get_range(shared_node &node, const tree_key &key_min, const tree_key &key_max,
                    std::vector<std::pair<tree_key, tree_val>> &result);

    /**
     * @brief Recursive helper for updating a key-value pair
     *
     * Recursively searches for and updates the value associated with
     * the specified key in the subtree rooted at the given node.
     *
     * @param node The current node being processed
     * @param key The key whose value should be updated
     * @param new_value The new value to associate with the key
     * @return true if the key was found and updated, false otherwise
     */
    bool _update(shared_node &node, const tree_key &key, const tree_val &new_value);

    /**
     * @brief Recursive helper for adding values to keys in range
     *
     * Recursively adds the specified value to all keys within the range
     * in the subtree rooted at the given node.
     *
     * @param node The current node being processed
     * @param value The value to add to each key in range
     * @param key_min Minimum key (inclusive)
     * @param key_max Maximum key (inclusive)
     */
    void _add_to_range(shared_node &node, int64_t value, const tree_key &key_min,
                       const tree_key &key_max);

    /**
     * @brief Recursive helper for printing the B-Tree
     *
     * Recursively prints the subtree rooted at the given node with
     * proper indentation to show the hierarchical structure.
     *
     * @param node The current node to print
     * @param depth The current depth in the tree (for indentation)
     */
    void _print(shared_node node, uint64_t depth);

    /**
     * @brief Allocate space for a new node in the archive
     *
     * Allocates the required space for a B-Tree node in the archive's
     * allocator and returns the file address.
     *
     * @return uint64_t The file address where the node should be stored
     */
    uint64_t allocate_node();

    /**
     * @brief Free the space occupied by a node
     *
     * Deallocates the space used by the node and removes it from cache.
     *
     * @param node Smart pointer to the node to free
     */
    void free_node(const shared_node &node);

    /**
     * @brief Create a new node with allocated space
     *
     * Allocates space for a new node and creates it at that address.
     *
     * @return shared_node Smart pointer to the created node
     */
    shared_node create_node();

    /**
     * @brief Read a node from the specified address
     *
     * Reads a node from the given file address using the node_reader.
     *
     * @param addr File address of the node to read
     * @return shared_node Smart pointer to the read node
     */
    shared_node read_node(uint64_t addr);

    /**
     * @brief Read a child node by index
     *
     * Reads the child node at the specified index from the parent,
     * applying any pending key additions.
     *
     * @param node The parent node
     * @param idx The index of the child to read
     * @return shared_node Smart pointer to the child node
     */
    shared_node read_child(shared_node &node, uint64_t idx);

    /**
     * @brief Read the root node of the B-Tree
     *
     * Reads and returns the root node from the archive header.
     *
     * @return shared_node Smart pointer to the root node
     */
    shared_node read_root();
};

} // namespace compio

#endif // BTREE_H
