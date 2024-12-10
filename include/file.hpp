/**
 * @file file.hpp
 * @brief Archive file navigation
 *
 */

#ifndef COMPIO_FILE_H_
#define COMPIO_FILE_H_

#include <cstdint>
#include <cstdio>

#include "compio.h"

namespace compio {

/**
 * @brief File header of fixed size
 *
 */
struct header {
    int32_t magic_number;              /**< Constant bytes, file signature */
    uint64_t index_root;               /**< Address of B-Tree root in file */
    uint64_t n_files;                  /**< Current number of files in archive */
    char fnames[COMPIO_MAX_FILES];     /**< Filenames */
    uint64_t fsizes[COMPIO_MAX_FILES]; /**< File sizes */

    /**
     * @brief Construct default header
     *
     */
    header();

    /**
     * @brief Read header from file
     *
     * @param file opened file
     * @return std::shared_ptr<header*>
     */
    header(FILE* file);

    /**
     * @brief Write header to file
     *
     * @param file opened file
     */
    void write(FILE* file);
};

/**
 * @brief B-Tree (index) node
 *
 */
struct index_node {
    uint8_t is_leaf;    /**< Is this node a leaf */
    uint32_t n_keys;    /**< Number of used keys in node */
    uint64_t* keys;     /**< Array of keys on memory heap */
    uint64_t* blocks;   /**< Array of blocks addressed on memory heap */
    uint64_t* children; /**< Array of children addresses on memory heap */

    int tree_order; /**< B-Tree order (not saved in file) */

    /**
     * @brief Construct empty index node
     *
     */
    index_node(int tree_order);

    /**
     * @brief Read B-Tree node from file
     *
     * @param file opened file
     * @param addr address to read from
     * @param tree_order order of b-tree
     * @return std::shared_ptr<index_node*>
     */
    index_node(FILE* file, uint64_t addr, int tree_order);
    ~index_node();

    /**
     * @brief Write index node to file
     *
     * @param file opened file
     * @param addr address to write to
     */
    void write(FILE* file, uint64_t addr);
};

/**
 * @brief Size of index node metadata (without arrays)
 */
#define INDEX_NODE_METASIZE offsetof(index_node, keys)

/**
 * @brief Block of (usually compressed) data
 *
 */
struct storage_block {
    uint8_t is_compressed;  /**< Is this block compressed */
    uint64_t size;          /**< Size of data array */
    uint64_t original_size; /**< Original size (size of uncompressed data) */
    uint64_t index_key;     /**< Index key of this block */
    uint8_t* data;          /**< Data block on memory heap */

    /**
     * @brief Construct empty storage block
     *
     */
    storage_block();

    /**
     * @brief Read storage block from file
     *
     * @param file opened file
     * @param addr address to read from
     * @return std::shared_ptr<storage_block*>
     */
    storage_block(FILE* file, uint64_t addr);
    ~storage_block();

    /**
     * @brief Write storage block to file
     *
     * @param file opened file
     * @param addr address to write to
     */
    void write(FILE* file, uint64_t addr);
};

/**
 * @brief Size of storage block metadata (without data)
 */
#define STORAGE_BLOCK_METASIZE offsetof(storage_block, data)

} // namespace compio

#endif // COMPIO_FILE_H_