/**
 * @file allocator.hpp
 * @brief Memory allocation management for compressed blocks storage
 *
 * This file implements a sophisticated block allocation system for managing
 * storage space in compressed archives. It includes free block management,
 * multiple allocation strategies, and automatic defragmentation support.
 */

#ifndef COMPIO_ALLOCATOR_HPP
#define COMPIO_ALLOCATOR_HPP

#include <cstdint>
#include <map>
#include <vector>

#include "compio.h"

namespace compio {

/**
 * @brief Structure representing a free block in storage
 */
struct free_block {
    uint64_t offset;  /**< Block start offset in file */
    uint64_t size;    /**< Block size in bytes */
    free_block *next; /**< Pointer to next free block */
    free_block *prev; /**< Pointer to previous free block */
};

/**
 * @brief Free blocks management strategies
 *
 * Different strategies for finding suitable free blocks during allocation:
 * - FIRST_FIT: Fast, finds first block that fits (good for speed)
 * - BEST_FIT: Finds smallest block that fits (minimizes wasted space)
 * - WORST_FIT: Finds largest block that fits (leaves larger fragments)
 * - NEXT_FIT: Like FIRST_FIT but continues from last allocation (spreads allocations)
 */
enum class allocation_strategy {
    FIRST_FIT, /**< Allocate first suitable block found */
    BEST_FIT,  /**< Allocate smallest suitable block to minimize waste */
    WORST_FIT, /**< Allocate largest suitable block to avoid small fragments */
    NEXT_FIT   /**< Continue search from last allocation point */
};

/**
 * @brief Free blocks table manager
 *
 * Manages a linked list of free blocks in the archive storage.
 * Supports multiple allocation strategies (first-fit, best-fit, worst-fit, next-fit)
 * and provides automatic block merging to reduce fragmentation.
 *
 * The manager maintains:
 * - A doubly-linked list of free blocks sorted by offset
 * - A size-based index for efficient best-fit and worst-fit searches
 * - Fragmentation tracking and caching
 * - Serialization/deserialization for persistent storage
 */
class free_blocks_manager {
public:
    /**
     * @brief Initialize manager with storage parameters
     * @param file_size Pointer to total file size reference
     */
    explicit free_blocks_manager(const uint64_t *file_size);

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
    const uint64_t *get_file_size_ptr() const { return file_size_; }

    /**
     * @brief Serialize manager state to buffer
     * @param buffer Vector to store serialized data
     * @return Size of serialized data
     */
    uint32_t serialize(std::vector<uint8_t> &buffer);

    /**
     * @brief Deserialize manager state from buffer
     * @param buffer Source buffer with serialized data
     * @param size Buffer size
     * @return True if deserialization successful
     */
    bool deserialize(const uint8_t *buffer, uint32_t size);

    /**
     * @brief Save manager state to archive file
     * @param archive Target archive
     * @return True if save successful
     */
    bool save_to_file(compio_archive *archive);

    /**
     * @brief Load manager state from archive file
     * @param archive Source archive
     * @return True if load successful
     */
    bool load_from_file(compio_archive *archive);

private:
    /**
     * @brief Remove block from the list and deallocate memory
     * @param block Block to remove
     */
    void remove_block(free_block *block);
    /**
     * @brief Custom index for fast block size lookups
     */
    struct size_index {
        std::multimap<uint64_t, free_block *> blocks_by_size;

        void insert(free_block *block) { blocks_by_size.insert({block->size, block}); }

        void remove(free_block *block) {
            auto range = blocks_by_size.equal_range(block->size);
            for (auto it = range.first; it != range.second; ++it) {
                if (it->second == block) {
                    blocks_by_size.erase(it);
                    break;
                }
            }
        }

        void clear() { blocks_by_size.clear(); }

        free_block *find_best_fit(uint64_t size) const {
            auto it = blocks_by_size.lower_bound(size);
            return it != blocks_by_size.end() ? it->second : nullptr;
        }

        free_block *find_worst_fit(uint64_t size) const {
            auto it = blocks_by_size.lower_bound(size);
            if (it == blocks_by_size.end())
                return nullptr;

            // Worst-fit: find the LARGEST block among those >= size
            // Start from lower_bound and iterate to find the maximum
            auto last = blocks_by_size.end();
            --last;

            // Verify that the last block is actually >= size
            if (last->first >= size) {
                return last->second;
            }

            // If the largest block is too small, return nullptr
            return nullptr;
        }
    };

    size_index size_idx_;

    free_block *head_;                           /**< Head of free blocks list */
    free_block *tail_;                           /**< Tail of free blocks list */
    free_block *last_alloc_;                     /**< Last allocation position for NEXT_FIT */
    uint64_t total_free_;                        /**< Total free space in bytes */
    const uint64_t *file_size_;                  /**< Reference to total file size */
    uint8_t cached_fragmentation_;               /**< Cached fragmentation level */
    mutable bool recently_defragmented_ = false; /**< Flag for recent defragmentation */

    /**
     * @brief Find the first suitable block for allocation
     * @param size Required block size
     * @return Pointer to the first suitable block or nullptr if not found
     */
    free_block *find_first_fit(uint64_t size) const;

    /**
     * @brief Find the smallest suitable block for allocation
     * @param size Required block size
     * @return Pointer to the best-fit block or nullptr if not found
     */
    free_block *find_best_fit(uint64_t size) const;

    /**
     * @brief Find the largest suitable block for allocation
     * @param size Required block size
     * @return Pointer to the worst-fit block or nullptr if not found
     */
    free_block *find_worst_fit(uint64_t size) const;

    /**
     * @brief Find the next suitable block for allocation
     * @param size Required block size
     * @return Pointer to the next-fit block or nullptr if not found
     */
    free_block *find_next_fit(uint64_t size) const;

    /**
     * @brief Find blocks that can be merged with the given region
     * @param offset Start offset of the region
     * @param size Size of the region
     * @param prev Output parameter for previous mergeable block
     * @param next Output parameter for next mergeable block
     */
    void find_mergeable_blocks(uint64_t offset, uint64_t size, free_block *&prev,
                               free_block *&next) const;

    /**
     * @brief Insert new block in the ordered list
     * @param new_block Block to insert
     */
    void insert_ordered_block(free_block *new_block);

    /**
     * @brief Merge three blocks (prev + new + next)
     * @param prev Previous block
     * @param offset New block offset
     * @param size New block size
     * @param next Next block
     */
    void merge_blocks(free_block *prev, uint64_t offset, uint64_t size, free_block *next);
};

/**
 * @brief High-level interface for block allocation operations
 *
 * Provides a simplified interface for allocating and deallocating storage blocks
 * in compressed archives. Automatically handles:
 * - Block allocation using the configured strategy
 * - Block deallocation and free space tracking
 * - Automatic defragmentation when fragmentation exceeds threshold
 * - State persistence (save/load allocator state)
 *
 * This is the main interface used by the archive system for managing storage.
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
    explicit block_allocator(compio_archive *archive);

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

    /**
     * @brief Save allocator state to archive
     * @param archive Target archive
     * @return True if save was successful
     */
    bool save_state(compio_archive *archive) { return blocks_manager_.save_to_file(archive); }

    /**
     * @brief Load allocator state from archive
     * @param archive Source archive
     * @return True if load was successful
     */
    bool load_state(compio_archive *archive) { return blocks_manager_.load_from_file(archive); }

private:
    compio_archive *archive_;            /**< Associated archive */
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

typedef void *compio_allocator_handle;

/**
 * @brief Create a new block allocator
 * @param archive Pointer to the archive
 * @return Handle to the created allocator
 */
compio_allocator_handle compio_create_allocator(compio_archive *archive);

/**
 * @brief Destroy an existing block allocator
 * @param handle Handle to the allocator
 */
void compio_destroy_allocator(compio_allocator_handle handle);

#ifdef __cplusplus
}
#endif

#endif // COMPIO_ALLOCATOR_HPP
