/**
 * @file allocator.hpp
 * @brief Memory allocation management for compressed blocks storage
 */

#ifndef COMPIO_ALLOCATOR_HPP
#define COMPIO_ALLOCATOR_HPP

#include <cstdint>
#include <vector>
#include "../compio.h"

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
     * @brief Print current free blocks list for debugging
     */
    void print_list() const;

    /**
     * @brief Get cached fragmentation level
     * @return Cached fragmentation percentage (0-100)
     */
    uint8_t get_cached_fragmentation() const;

    /**
     * @brief Calculate current fragmentation level
     * @return Fragmentation percentage (0-100)
     */
    uint8_t calculate_fragmentation() const;

    /**
     * @brief Update the cached fragmentation value
     */
    void update_fragmentation();

    /**
     * @brief Set a custom cached fragmentation value
     * @param value New fragmentation value to set
     */
    void set_cached_fragmentation(uint8_t value);

    /**
     * @brief Check if a region is already marked as free
     * @param offset Start offset of the region
     * @param size Size of the region
     * @return True if region is already in the free list
     */
    bool is_region_free(uint64_t offset, uint64_t size) const;

    /**
     * @brief Get pointer to file size reference
     * @return Raw pointer to managed file size
     */
    uint64_t* get_file_size_ptr() const { return file_size_; }

    /**
     * @brief Serialize free blocks list to a buffer
     * @param buffer Output buffer to store serialized data
     * @return Size of serialized data in bytes
     */
    uint32_t serialize(std::vector<uint8_t>& buffer);

    /**
     * @brief Deserialize free blocks list from a buffer
     * @param buffer Buffer containing serialized data
     * @param size Size of serialized data in bytes
     * @return True if deserialization succeeded
     */
    bool deserialize(const uint8_t* buffer, uint32_t size);

    /**
     * @brief Save free blocks table to archive file
     * @param archive Pointer to the archive
     * @return True if save succeeded
     */
    bool save_to_file(compio_archive* archive);

    /**
     * @brief Load free blocks table from archive file
     * @param archive Pointer to the archive
     * @return True if load succeeded
     */
    bool load_from_file(compio_archive* archive);

private:
    free_block* head_;           /**< Head of free blocks list */
    free_block* tail_;           /**< Tail of free blocks list */
    free_block* last_alloc_;     /**< Last allocation position for NEXT_FIT */
    uint64_t total_free_;        /**< Total free space in bytes */
    uint64_t* file_size_;        /**< Reference to total file size */
    uint8_t cached_fragmentation_; /**< Cached fragmentation level */
    mutable bool recently_defragmented_ = false; /**< Flag for recent defragmentation */

    /**
     * @brief Find the first suitable block for allocation
     * @param size Required block size
     * @return Pointer to the first suitable block or nullptr if not found
     */
    free_block* find_first_fit(uint64_t size) const;

    /**
     * @brief Find the smallest suitable block for allocation
     * @param size Required block size
     * @return Pointer to the best-fit block or nullptr if not found
     */
    free_block* find_best_fit(uint64_t size) const;

    /**
     * @brief Find the largest suitable block for allocation
     * @param size Required block size
     * @return Pointer to the worst-fit block or nullptr if not found
     */
    free_block* find_worst_fit(uint64_t size) const;

    /**
     * @brief Find the next suitable block for allocation
     * @param size Required block size
     * @return Pointer to the next-fit block or nullptr if not found
     */
    free_block* find_next_fit(uint64_t size) const;
};

/**
 * @brief Interface for block allocation operations
 */
class block_allocator {
public:
    /**
     * @brief Get current fragmentation level
     * @return Fragmentation percentage (0-100)
     */
    [[nodiscard]] uint8_t get_fragmentation() const;

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

    friend compio_archive* ::compio_open_archive(const char*, const char*, const compio_config*);
    friend int ::compio_close_archive(compio_archive*);

private:
    compio_archive* archive_;            /**< Associated archive */
    free_blocks_manager blocks_manager_; /**< Free blocks manager */
    uint8_t last_fragmentation_;         /**< Last measured fragmentation */

    /**
     * @brief Check if defragmentation is needed
     * @return True if fragmentation exceeds the threshold
     */
    [[nodiscard]] bool needs_defragmentation() const;

    /**
     * @brief Perform defragmentation of the storage
     */
    void perform_defragmentation();
};

} // namespace compio

#ifdef __cplusplus
extern "C" {
#endif

    typedef void* compio_allocator_handle;

    /**
     * @brief Create a new block allocator
     * @param archive Pointer to the archive
     * @return Handle to the created allocator
     */
    compio_allocator_handle compio_create_allocator(compio_archive* archive);

    /**
     * @brief Destroy an existing block allocator
     * @param handle Handle to the allocator
     */
    void compio_destroy_allocator(compio_allocator_handle handle);

#ifdef __cplusplus
}
#endif

#endif // COMPIO_ALLOCATOR_HPP