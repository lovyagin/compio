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

#include <cstdbool>
#include <cstddef>
#include <memory>

#include "allocator.hpp"
#include "file.hpp"
#include "shared_node.hpp"

namespace compio {

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
    btree(compio_archive* archive);

    /**
     * @brief Insert element into B-Tree
     *
     * @param key
     * @param value
     */
    void insert(key_t key, value_t value);

    /**
     * @brief Remove element by key from B-Tree
     *
     * @param key
     */
    void remove(key_t key);

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
     * @return const std::vector<uint64_t>&
     */
    const std::vector<uint64_t>& get_range(key_t key_min, key_t key_max);

    /**
     * @brief Update element
     *
     * @param key
     * @param new_value
     */
    void update(key_t key, value_t new_value);

// private:
    int degree;
    compio_archive* archive;

    /**
     * @brief Search for node, that contains key
     *
     * @param key
     * @return uint64_t
     */
    uint64_t search_node(key_t key);

    void insert_nonfull(shared_node node, key_t key, value_t value);
    void split_child(shared_node parent, shared_node child, int index);
    void merge_children(shared_node parent, int idx);
    void borrow_from_prev(shared_node parent, int idx);
    void borrow_from_next(shared_node parent, int idx);
    key_t find_max_in_node(shared_node node);
    key_t find_min_in_node(shared_node node);
    void remove_node(shared_node node, key_t key);

    uint64_t allocate_node();
    void free_node(shared_node node);
    shared_node create_node();
    shared_node read_node(uint64_t addr);
    shared_node read_root();
};

} // namespace compio

#endif // BTREE_H
