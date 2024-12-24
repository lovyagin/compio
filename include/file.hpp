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

namespace compio {

/**
 * @brief Files table for archive header
 *
 */
struct files_table {
    uint64_t n_files; /**< Current number of files in archive */

    struct file {
        char name[COMPIO_FNAME_MAX_SIZE];
        uint64_t size;
    };

    file files[COMPIO_MAX_FILES];

    files_table();                /**< Construct empty files table */
    file* find(const char* name); /**< Find file by name, nullptr if doesn't exist */
    file* add(const char* name);  /**< Add file, nullptr if table is full */
    int remove(const char* name); /**< Remove file */

    /**
     * @brief Swap bytes of all integers in struct
     * 
     * @param from_valid True, when struct is valid before this function, 
     * and we want to swap bytes for writing to file
     */
    void _swap_endianness(bool from_valid);
};

/**
 * @brief File header of fixed size
 *
 */
struct header {
    int32_t magic_number; /**< Constant bytes, file signature */
    uint64_t index_root;  /**< Address of B-Tree root in file */
    uint64_t file_size;
    files_table ftable; /**< Files table */

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
    header(FILE* file, bool swap_endianness);

    /**
     * @brief Write header to file
     *
     * @param file opened file
     */
    void write(FILE* file, bool swap_endianness);

private:
    /**
     * @brief Swap bytes of all integers in struct
     * 
     * @param from_valid True, when struct is valid before this function, 
     * and we want to swap bytes for writing to file
     */
    void _swap_endianness(bool from_valid);
};

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
 * @brief B-Tree (index) node
 *
 */
struct index_node {
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

    /**
     * @brief Read index node from file
     *
     * @param file opened file
     * @param addr address to read from
     * @param tree_degree degree of b-tree
     */
    index_node(FILE* file, uint64_t addr, bool swap_endianness, int tree_degree);

    /**
     * @brief Write index node to file
     *
     * @param file opened file
     * @param addr address to write to
     */
    void write(FILE* file, uint64_t addr, bool swap_endianness);

private:
    /**
     * @brief Swap bytes of all integers in struct
     * 
     * @param from_valid True, when struct is valid before this function, 
     * and we want to swap bytes for writing to file
     */
    void _swap_endianness(bool from_valid);
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
struct storage_block {
    uint8_t is_compressed;     /**< Is this block compressed */
    uint64_t size;             /**< Size of data array */
    uint64_t original_size;    /**< Original size (size of uncompressed data) */
    tree_key index_key;        /**< Index key of this block */
    std::vector<uint8_t> data; /**< Data block */

    /**
     * @brief Construct storage block with data of size
     *
     * @param size size of data
     */
    storage_block(uint64_t size);

    /**
     * @brief Read storage block from file
     *
     * @param file opened file
     * @param addr address to read from
     * @return std::shared_ptr<storage_block*>
     */
    storage_block(FILE* file, uint64_t addr, bool swap_endianness);

    /**
     * @brief Write storage block to file
     *
     * @param file opened file
     * @param addr address to write to
     */
    void write(FILE* file, uint64_t addr, bool swap_endianness);

private:
    /**
     * @brief Swap bytes of all integers in struct
     * 
     * @param from_valid True, when struct is valid before this function, 
     * and we want to swap bytes for writing to file
     */
    void _swap_endianness(bool from_valid);
};

/**
 * @brief Size of storage block metadata (without data)
 */
#define STORAGE_BLOCK_METASIZE offsetof(storage_block, data)

} // namespace compio

#endif // COMPIO_FILE_H_