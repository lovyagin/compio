/**
 * @file file.hpp
 * @brief Archive file navigation
 *
 */

#ifndef COMPIO_FILE_H_
#define COMPIO_FILE_H_

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "compio/infile_object.hpp"
#include "compio/tree_types.hpp"
#include "compio.h"

namespace compio {

/**
 * @brief Files table for archive header
 *
 */
struct files_table {
    uint64_t n_files;
    struct file {
        char name[COMPIO_FNAME_MAX_SIZE];
        uint64_t size;
    };
    file files[COMPIO_MAX_FILES];

    files_table();

    const file *find(const char *name) const;
    file *find(const char *name);
    file *add(const char *name);
    int remove(const char *name);
};

/**
 * @brief File header of fixed size
 *
 */
struct header : public infile_object {
    int32_t magic_number; /**< Constant bytes, file signature */
    uint64_t index_root;  /**< Address of B-Tree root in file */
    uint64_t file_size;
    files_table ftable; /**< Files table */
    uint64_t allocator_state_offset;
    uint64_t allocator_state_size;
    uint32_t compression_type; /**< Type of compression algorithm used */

    /**
     * @brief Construct default header
     *
     */
    header();

    void read_from(FILE *file, uint64_t addr) override;
    void write_to(FILE *file, uint64_t addr) const override;
};

/**
 * @brief Key for B-Tree index entries
 *
 * Uniquely identifies a data block by combining filename hash
 * and position in the uncompressed file.
 */
// struct tree_key {
//     uint64_t hash; /**< 64-bit hash of the internal filename */
//     uint64_t pos;  /**< Starting byte offset in uncompressed file */
// };

/**
 * @brief Type for value in btree
 *
 */
// typedef struct {
//     uint64_t addr; /**< Address of storage_block in archive file */
//     uint64_t size; /**< Original size of uncompressed block */
// } tree_val;

/**
 * @brief B-Tree (index) node
 *
 */
struct index_node : public infile_object {
    uint8_t is_leaf;                /**< Is this node a leaf */
    uint32_t num_keys;              /**< Number of used keys in node */
    std::vector<tree_key> keys;     /**< Blocks start positions in uncompressed file */
    std::vector<tree_val> values;   /**< Storage blocks addresses in archive file */
    std::vector<uint64_t> children; /**< Children addresses in archive file */

    int tree_degree; /**< B-Tree degree (not saved in file) */

    /**
     * @brief Construct empty index node
     *
     * @param tree_degree degree of b-tree
     */
    index_node(int tree_degree);

    void read_from(FILE *file, uint64_t addr) override;
    void write_to(FILE *file, uint64_t addr) const override;
};

/**
 * @brief Size of index node metadata (without arrays)
 */
#define INDEX_NODE_METASIZE (sizeof(index_node::is_leaf) + sizeof(index_node::num_keys))
/**
 * @brief Whole size of index node
 */
#define INDEX_NODE_SIZE(degree)                                                                    \
    (INDEX_NODE_METASIZE + sizeof(tree_key) * (2 * degree - 1) +                                   \
     sizeof(tree_val) * (2 * degree - 1) + sizeof(uint64_t) * (2 * degree))

/**
 * @brief Block of (usually compressed) data
 *
 */
struct storage_block : public infile_object {
    uint8_t is_compressed;           /**< Is this block compressed */
    uint64_t size;                   /**< Size of data array */
    uint64_t original_size;          /**< Original size (size of uncompressed data) */
    tree_key index_key;              /**< Index key of this block */
    std::unique_ptr<uint8_t[]> data; /**< Data block */

    storage_block();

    storage_block(std::unique_ptr<uint8_t[]> &&data, uint64_t size);

    /**
     * @brief Construct storage block with data of size
     *
     * @param size size of data
     */
    storage_block(uint64_t size);

    void read_from(FILE *file, uint64_t addr) override;
    void write_to(FILE *file, uint64_t addr) const override;
};

/**
 * @brief Size of storage block metadata (without data)
 */
#define STORAGE_BLOCK_METASIZE                                                                     \
    (sizeof(storage_block::is_compressed) + sizeof(storage_block::size) +                          \
     sizeof(storage_block::original_size) + sizeof(storage_block::index_key))

} // namespace compio

#endif // COMPIO_FILE_H_