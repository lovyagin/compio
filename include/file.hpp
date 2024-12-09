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

#include "compio.h"

namespace compio {

/**
 * @brief File header of fixed size
 *
 */
struct header {
	int32_t magic_number;	 /**< Constant bytes, file signature */
	uint64_t original_fsize; /**< Original (uncompressed) file size */
	uint64_t index_root;	 /**< Address of B-Tree root in file */

    header();
};

/**
 * @brief B-Tree (index) node
 *
 */
struct index_node {
	uint8_t is_leaf;	/**< Is this node a leaf */
	uint32_t n_keys;	/**< Number of used keys in node */
	uint64_t* keys;		/**< Array of keys on memory heap */
	uint64_t* blocks;	/**< Array of blocks addressed on memory heap */
	uint64_t* children; /**< Array of children addresses on memory heap */

	~index_node();
};

/**
 * @brief Block of (usually compressed) data
 *
 */
struct storage_block {
	uint8_t is_compressed;	/**< Is this block compressed */
	uint64_t size;			/**< Size of data array */
	uint64_t original_size; /**< Original size (size of uncompressed data) */
	uint64_t index_key;		/**< Index key of this block */
	uint8_t* data;			/**< Data block on memory heap */

	~storage_block();
};

/**
 * @brief Read header from file
 *
 * @param file opened file
 * @return std::shared_ptr<header*>
 */
std::shared_ptr<header> read_header(FILE* file);

/**
 * @brief Read B-Tree node from file
 *
 * @param file opened file
 * @param addr address to read from
 * @return std::shared_ptr<index_node*>
 */
std::shared_ptr<index_node> read_index_node(FILE* file, uint64_t addr);

/**
 * @brief Read storage block from file
 *
 * @param file opened file
 * @param addr address to read from
 * @return std::shared_ptr<storage_block*>
 */
std::shared_ptr<storage_block> read_storage_block(FILE* file, uint64_t addr);

} // namespace compio

#endif // COMPIO_FILE_H_