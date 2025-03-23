/**
 * @file allocator.hpp
 * @brief Memory allocation management for compressed blocks storage
 */

#ifndef COMPIO_ALLOCATOR_HPP
#define COMPIO_ALLOCATOR_HPP

#include <cstdint>
#include "compio.h"

namespace compio {

/**
 * @brief Structure representing a free block in storage
 */
struct free_block {
    uint64_t offset;     /**< Block start offset in file */
    uint64_t size;       /**< Block size in bytes */
    free_block* next;    /**< Pointer to next free block */
    free_block* prev;    /**< Pointer to previous free block */
};

/**
 * @brief Free blocks management strategies
 */
enum class allocation_strategy {
    FIRST_FIT,          /**< Allocate first suitable block */
    BEST_FIT,           /**< Allocate smallest suitable block */
    WORST_FIT,          /**< Allocate largest suitable block */
    NEXT_FIT            /**< Continue search from last allocation */
};

/**
 * @brief Free blocks table manager
 */
class free_blocks_manager {
public:
    /**
     * @brief Initialize manager with storage parameters
     * @param file_size Pointer to total file size reference
     */
    explicit free_blocks_manager(uint64_t* file_size);

    /**
     * @brief Add new free block to the storage
     * @param offset Block start offset
     * @param size Block size
     */
    void add_free_block(uint64_t offset, uint64_t size);

    /**
     * @brief Find and allocate suitable block
     * @param size Required block size
     * @param strategy Allocation strategy to use
     * @return Offset of allocated block or UINT64_MAX if not found
     */
    uint64_t allocate_block(uint64_t size, allocation_strategy strategy);

    /**
     * @brief Merge adjacent free blocks
     */
    void defragment();

    /**
     * @brief Calculate current fragmentation level
     * @return Fragmentation percentage (0-100)
     */
    uint8_t calculate_fragmentation() const;

    /**
     * @brief Get pointer to file size reference
     * @return Raw pointer to managed file size
     */
    uint64_t* get_file_size_ptr() const { return file_size_; }

private:
    free_block* head_;           /**< Head of free blocks list */
    free_block* tail_;           /**< Tail of free blocks list */
    free_block* last_alloc_;     /**< Last allocation position for NEXT_FIT */
    uint64_t total_free_;        /**< Total free space in bytes */
    uint64_t* file_size_;        /**< Reference to total file size */

    void merge_with_neighbors(free_block* block);
    free_block* find_first_fit(uint64_t size) const;
    free_block* find_best_fit(uint64_t size) const;
    free_block* find_worst_fit(uint64_t size) const;
    free_block* find_next_fit(uint64_t size) const;
};

/**
 * @brief Interface for block allocation operations
 */
class block_allocator {
public:
    /**
     * @brief Initialize allocator with archive configuration
     * @param archive Pointer to opened archive
     */
    explicit block_allocator(compio_archive* archive);

    /**
     * @brief Allocate storage block
     * @param size Required block size
     * @return Offset of allocated block
     */
    uint64_t allocate(uint64_t size);

    /**
     * @brief Release storage block
     * @param offset Block start offset
     * @param size Block size
     */
    void deallocate(uint64_t offset, uint64_t size);

    /**
     * @brief Perform maintenance operations if needed
     */
    void maintenance();

private:
    compio_archive* archive_;            /**< Associated archive */
    free_blocks_manager blocks_manager_; /**< Free blocks manager */
    uint8_t last_fragmentation_;         /**< Last measured fragmentation */

    [[nodiscard]] bool needs_defragmentation() const;
    void perform_defragmentation();
};

} // namespace compio

#ifdef __cplusplus
extern "C" {
#endif

    typedef void* compio_allocator_handle;

    compio_allocator_handle compio_create_allocator(compio_archive* archive);
    void compio_destroy_allocator(compio_allocator_handle handle);

#ifdef __cplusplus
}
#endif

#endif // COMPIO_ALLOCATOR_HPP