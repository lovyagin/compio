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
#include <optional>

#include "compio/allocator.hpp"
#include "compio/file.hpp"

#include "third_party/lrucache.hpp"

namespace compio {

using shared_node = smart_infile_object<index_node>;

/**
 * @brief Struct for reading nodes from memory with caching
 *
 */
struct node_reader {
public:
    node_reader(FILE *file, int tree_degree, int max_size);
    shared_node read_node(uint64_t addr);
    shared_node create_node(uint64_t addr);
    void remove_node(const shared_node &node);
    void clear_cache();

private:
    int tree_degree;
    FILE *file;

    cache::lru_cache<uint64_t, shared_node> cache;
};

/**
 * @brief B-Tree, that stores addresses of compressed blocks in
 * archive file, with access by their start position. B-Tree nodes
 * are stored in archive file.
 *
 */
struct btree {
public:
    /**
     * @brief Read B-Tree from archive file.
     * If archive->index_root == 0, create empty tree
     *
     * @param archive
     */
    btree(compio_archive *archive);

    /**
     * @brief Insert element into B-Tree
     *
     * @param key
     * @param value
     */
    void insert(const tree_key &key, const tree_val &value);

    /**
     * @brief Remove element by key from B-Tree
     *
     * @param key
     */
    void remove(const tree_key &key);

    /**
     * @brief Get list of blocks addresses in ascending by key order,
     * that intersect [key_min, key_max] interval.
     *
     * Example:
     *  blocks in tree: 0, 16, 32, 48, 64
     *  get_range(18, 36) -> {16, 32}
     *
     * @param key_min
     * @param key_max
     * @return std::vector<uint64_t>
     */
    void get_range(const tree_key &key_min, const tree_key &key_max,
                   std::vector<std::pair<tree_key, tree_val>> &result);

    /**
     * @brief Update element
     *
     * @param key
     * @param new_value
     * @return true on success
     */
    bool update(const tree_key &key, const tree_val &new_value);

    // private:
    uint64_t degree;
    compio_archive *archive;

    /**
     * @brief Search for node, that contains key
     *
     * @param key
     * @return uint64_t
     */
    std::optional<tree_val> get(const tree_key &key);

    /**
     * @brief Add value to all keys in the specified range
     * 
     * Adds a value to all keys within [lower_bound, upper_bound]
     * 
     * @param value Value to add to keys
     * @param lower_bound Lower bound of range (inclusive)
     * @param upper_bound Upper bound of range (inclusive)
     */
    void add_to_range(int64_t value, const tree_key &lower_bound, const tree_key &upper_bound);

    void insert_nonfull(shared_node &node, const tree_key &key, const tree_val &value);
    void split_child(shared_node &parent, shared_node &child, int index);
    void merge_children(shared_node &parent, int idx);
    void borrow_from_prev(shared_node &parent, int idx);
    void borrow_from_next(shared_node &parent, int idx);
    std::pair<tree_key, tree_val> find_max_in_node(const shared_node &node);
    std::pair<tree_key, tree_val> find_min_in_node(const shared_node &node);
    void remove_node(shared_node &node, const tree_key &key);
    void get_range_in_node(shared_node &node, const tree_key &key_min,
                           const tree_key &key_max,
                           std::vector<std::pair<tree_key, tree_val>> &result);
    bool update_in_node(shared_node &node, const tree_key &key, const tree_val &new_value);
    void add_to_range_in_node(shared_node &node, int64_t value, const tree_key &lower_bound, const tree_key &upper_bound);

    uint64_t allocate_node() const;
    void free_node(const shared_node &node);
    shared_node create_node();
    shared_node read_node(uint64_t addr);
    shared_node read_child(shared_node &node, uint64_t idx);
    shared_node read_root();

    // for debug
    void print_btree();

    node_reader reader;
};

} // namespace compio

#endif // BTREE_H
