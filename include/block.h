#ifndef BLOCK_H
#define BLOCK_H

#include <stddef.h>
#include <stdbool.h>
#include "btree.h"

/**
 * @struct compio_block
 * @brief Represents a block of data in the storage system.
 */
typedef struct compio_block {
    size_t size;                   /**< Size of the block in bytes */
    bool is_compressed;            /**< Indicates if the block is compressed */
    char* compression_type;        /**< Type of compression used, if any */
    void* data;                    /**< Pointer to the block's data */
} compio_block;

/**
 * @struct compio_block_container
 * @brief Represents a container for managing multiple blocks.
 */
typedef struct compio_block_container {
    compio_block** blocks;         /**< Array of pointers to blocks */
    size_t total_size;             /**< Total capacity of the container */
    size_t block_count;            /**< Current number of blocks in the container */
    BTree* index;                  /**< B-Tree index for managing blocks */
} compio_block_container;

/**
 * @brief Creates a new block with the specified size.
 *
 * @param size Size of the block in bytes.
 * @param is_compressed Whether the block is compressed.
 * @param compression_type Type of compression used (can be NULL if not compressed).
 * @return Pointer to the created block, or NULL if memory allocation fails.
 */
compio_block* compio_create_block(size_t size, bool is_compressed, const char* compression_type);

/**
 * @brief Frees the memory associated with a block.
 *
 * @param block Pointer to the block to be freed.
 */
void compio_free_block(compio_block* block);

/**
 * @brief Creates a container for managing multiple blocks.
 *
 * @param total_size Total capacity of the container.
 * @return Pointer to the created block container, or NULL if memory allocation fails.
 */
compio_block_container* compio_create_block_container(size_t total_size);

/**
 * @brief Frees the memory associated with a block container.
 *
 * @param container Pointer to the block container to be freed.
 */
void compio_free_block_container(compio_block_container* container);

/**
 * @brief Adds a block to the container.
 *
 * @param container Pointer to the block container.
 * @param block Pointer to the block to be added.
 * @return 0 if the block is successfully added, or -1 on failure.
 */
int compio_add_block(compio_block_container* container, compio_block* block);

/**
 * @brief Removes a block from the container by its index.
 *
 * @param container Pointer to the block container.
 * @param block_index Index of the block to be removed.
 * @return 0 if the block is successfully removed, or -1 on failure.
 */
int compio_remove_block(compio_block_container* container, size_t block_index);

/**
 * @brief Finds a block in the container by its position.
 *
 * @param container Pointer to the block container.
 * @param position Position in the file to find the corresponding block.
 * @return Pointer to the block if found, or NULL if not found.
 */
compio_block* compio_find_block(compio_block_container* container, size_t position);

/**
 * @brief Updates the index of the block container.
 *
 * @param container Pointer to the block container.
 */
void compio_update_index(compio_block_container* container);

#endif // BLOCK_H
