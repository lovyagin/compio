/**
 * @file file.hpp
 * @brief Archive file navigation
 *
 */

#ifndef COMPIO_FILE_H_
#define COMPIO_FILE_H_

#include <cstdint>
#include <cstdio>
#include <vector>

#include "compio.h"
#include "infile_object.hpp"

namespace compio {

/**
 * @brief Type for key in btree
 *
 */
typedef struct {
    uint64_t hash; /**< last 64 bits of hashed internal file name */
    uint64_t pos;  /**< Position of block start in uncompressed file */
} tree_key;

/**
 * @brief Type for value in btree
 *
 */
typedef struct {
    uint64_t addr; /**< Address of storage_block in archive file */
    uint64_t size; /**< Original size of uncompressed block */
} tree_val;

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

    const file* find(const char* name) const;
    file* find(const char* name);
    file* add(const char* name);
    int remove(const char* name);
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

    /**
     * @brief Construct default header
     *
     */
    header();

    void read_from(FILE* file, uint64_t addr) override;
    void write_to(FILE* file, uint64_t addr) const override;
};

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

    void read_from(FILE* file, uint64_t addr) override;
    void write_to(FILE* file, uint64_t addr) const override;
};

/**
 * @brief Size of index node metadata (without arrays)
 */
#define INDEX_NODE_METASIZE offsetof(index_node, keys)
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
    uint8_t is_compressed;     /**< Is this block compressed */
    uint64_t size;             /**< Size of data array */
    uint64_t original_size;    /**< Original size (size of uncompressed data) */
    tree_key index_key;        /**< Index key of this block */
    std::vector<uint8_t> data; /**< Data block */

    storage_block(std::vector<uint8_t>&& data);

    /**
     * @brief Construct storage block with data of size
     *
     * @param size size of data
     */
    storage_block(uint64_t size);
    
    void read_from(FILE* file, uint64_t addr) override;
    void write_to(FILE* file, uint64_t addr) const override;
};

/**
 * @brief Size of storage block metadata (without data)
 */
#define STORAGE_BLOCK_METASIZE offsetof(storage_block, data)

} // namespace compio

#endif // COMPIO_FILE_H_