#ifndef COMPIO_BLOCK_H
#define COMPIO_BLOCK_H

#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>

/**
 * @struct compio_block
 * @brief Represents a data block with metadata and raw data.
 *
 * Describes a block of data, including its offset, size, compression state,
 * and a pointer to uncompressed data.
 */
typedef struct {
    size_t offset;           /**< Offset of the block in the file */
    size_t size;             /**< Size of the block */
    bool is_compressed;      /**< Flag indicating if the block is compressed */
    char* compression_type;  /**< Compression type (NULL if uncompressed) */
    void* data;              /**< Pointer to the block's uncompressed data */
} compio_block;

/**
 * @struct compio_block_container
 * @brief Container for managing multiple blocks and their index.
 *
 * Holds an array of blocks and provides interfaces for manipulating them.
 * Includes an index structure for fast block lookup (e.g., a B-tree).
 */
typedef struct {
    size_t total_size;       /**< Total size of the file */
    size_t block_count;      /**< Number of blocks */
    compio_block** blocks;   /**< Array of pointers to blocks */
    void* index;             /**< Index structure for fast block lookup (e.g., B-tree) */
} compio_block_container;

/**
 * @struct compio_file_state
 * @brief Tracks the current file position within a block container.
 *
 * Manages the current position and access to blocks within a file.
 */
typedef struct {
    compio_block_container* container; /**< Pointer to the block container */
    size_t position;                   /**< Current position in the file */
    size_t current_block;              /**< Index of the current block */
    size_t block_offset;               /**< Offset within the current block */
} compio_file_state;

/* Block management functions */

/**
 * @brief Creates a new data block.
 *
 * @param size The size of the block.
 * @param is_compressed Indicates if the block is compressed.
 * @param compression_type The type of compression (NULL if uncompressed).
 * @return A pointer to the created block, or NULL on failure.
 *
 * @note The `data` field stores uncompressed data.
 */
compio_block* compio_create_block(size_t size, bool is_compressed, const char* compression_type);

/**
 * @brief Frees memory associated with a data block.
 *
 * @param block A pointer to the block to free.
 */
void compio_free_block(compio_block* block);



/* Block container management functions */

/**
 * @brief Creates a container for managing blocks.
 *
 * @param total_size The total size of the file.
 * @return A pointer to the created container, or NULL on failure.
 */
compio_block_container* compio_create_block_container(size_t total_size);

/**
 * @brief Adds a block to the container.
 *
 * @param container A pointer to the container.
 * @param block A pointer to the block to add.
 * @return 0 on success, -1 on failure.
 */
int compio_add_block(compio_block_container* container, compio_block* block);

/**
 * @brief Removes a block from the container.
 *
 * @param container A pointer to the container.
 * @param block_index The index of the block to remove.
 * @return 0 on success, -1 on failure.
 */
int compio_remove_block(compio_block_container* container, size_t block_index);

/**
 * @brief Finds a block by position in the file.
 *
 * @param container A pointer to the container.
 * @param position The position in the file.
 * @param block_offset Pointer to store the offset within the found block.
 * @return A pointer to the found block, or NULL if not found.
 *
 * @note Uses a B-tree or another index structure for fast lookup.
 */
compio_block* compio_find_block(compio_block_container* container, size_t position, size_t* block_offset);

/**
 * @brief Updates the container's index structure.
 *
 * @param container A pointer to the container.
 * @return 0 on success, -1 on failure.
 *
 * @note This function must be called after adding or removing blocks.
 */
int compio_update_index(compio_block_container* container);

/**
 * @brief Frees all resources associated with a block container.
 *
 * This function deallocates the memory used by the block container, including
 * the blocks array, index structure (if used), and individual blocks.
 *
 * @param container A pointer to the block container to be freed.
 *                  If NULL, the function does nothing.
 */
void compio_free_block_container(compio_block_container* container);

/* File state management functions */

/**
 * @brief Opens a file state for managing block access.
 *
 * @param container A pointer to the block container.
 * @return A pointer to the file state, or NULL on failure.
 */
compio_file_state* compio_open_block_file_state(compio_block_container* container);

/**
 * @brief Sets the current position in the file.
 *
 * @param state A pointer to the file state.
 * @param position The new position in the file.
 * @return 0 on success, -1 on failure.
 */
int compio_set_file_position(compio_file_state* state, size_t position);

/**
 * @brief Frees all resources associated with a file state.
 *
 * This function deallocates the memory used by the file state, including
 * detaching it from the associated block container. It does not free
 * the container itself, which must be managed separately.
 *
 * @param state A pointer to the file state to be freed.
 *              If NULL, the function does nothing.
 */
void compio_free_file_state(compio_file_state* state);

#endif /* COMPIO_BLOCK_H */
